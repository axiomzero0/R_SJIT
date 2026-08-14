// rjit/core/gc.hpp - Garbage collector and HeapObject base class
//
// We use a non-moving mark-and-sweep collector with conservative stack
// scanning for the C++ frames, plus precise scanning of heap objects.
// Non-moving was chosen because R objects are routinely aliased by raw
// pointers inside compiled JIT code; moving GC would require read barriers
// in the JIT which is far more complexity than the time saved.
//
// HeapObject layout:
//   [ header: gc word + type + flags + aux ]
//   [ derived class body                                  ]
//
// Every heap object knows how to trace its own outgoing references
// via a virtual `trace(Visitor&)` method. This costs one vtable slot
// per object but makes the GC precise and simple.

#pragma once

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <vector>
#include <functional>
#include "rjit/core/value.hpp"

namespace rjit {

class HeapObject;
class Visitor;
class GC;

// GC metadata lives in a header word placed before each HeapObject. We
// mark "before" rather than embedding in the object so that the GC can
// scan headers without polluting the object's own cache line.
struct HeapHeader {
    std::atomic<uint32_t> gc_color;     // 0=white, 1=gray, 2=black (tri-color)
    TypeTag                type;
    uint8_t                flags;
    uint16_t               aux;          // type-specific (e.g., shape id)

    HeapHeader(TypeTag t) : gc_color(0), type(t), flags(0), aux(0) {}
};

class HeapObject {
public:
    HeapObject(TypeTag t);
    virtual ~HeapObject() = default;

    TypeTag type() const noexcept { return header_.type; }
    uint8_t flags() const noexcept { return header_.flags; }
    void set_flags(uint8_t f) noexcept { header_.flags = f; }
    uint16_t aux() const noexcept { return header_.aux; }
    void set_aux(uint16_t a) noexcept { header_.aux = a; }

    // GC mark bit access (tri-color marking)
    uint32_t gc_color() const noexcept { return header_.gc_color.load(std::memory_order_relaxed); }
    void set_gc_color(uint32_t c) noexcept { header_.gc_color.store(c, std::memory_order_relaxed); }

    // Every subclass overrides trace() to visit outgoing references.
    virtual void trace(Visitor& v) const = 0;

private:
    HeapHeader header_;
};

class Visitor {
public:
    virtual ~Visitor() = default;
    virtual void visit_value(Value& v) = 0;
    virtual void visit_heap(HeapObject*& p) = 0;

    void visit(Value& v) {
        if (v.is_heap()) {
            HeapObject* p = v.as_heap();
            visit_heap(p);
            // visit_heap may rewrite p (it doesn't for non-moving GC, but
            // we still re-store to be future-proof)
            v = Value::from_heap(v.tag(), p);
        }
    }
};

// GC: a non-moving mark-and-sweep collector.
class GC {
public:
    // Trigger a collection. Returns the number of bytes reclaimed.
    size_t collect();

    // Allocate `bytes` of memory for an object of `tag`. The constructor
    // of the derived type is responsible for calling this through `operator new`.
    void* allocate(size_t bytes, TypeTag tag);

    // Register an already-allocated object (called by operator new
    // after the constructor runs). `size` is the allocation size.
    void register_object(HeapObject* obj, size_t size);

    // Push/pop roots (used by the interpreter frame stack and by the JIT
    // for values stored in the JS-style "handle scope" of compiled code).
    void push_root(Value* v);
    void pop_root(Value* v);
    void push_temp_root(HeapObject* obj);
    void pop_temp_root(HeapObject* obj);

    // Stats
    size_t live_objects() const noexcept { return live_objects_; }
    size_t live_bytes()   const noexcept { return live_bytes_;   }
    size_t peak_bytes()   const noexcept { return peak_bytes_;   }

    static GC& current();

    // Constructor is public so std::make_unique works. (In a more
    // polished version we'd make this private and friend Context,
    // but that requires also friending std::unique_ptr<GC>::reset(),
    // which is messy.)
    GC();
    ~GC();

private:
    struct HeapBlock {
        HeapObject* obj;
        size_t      size;
        TypeTag     tag;
    };
    std::vector<HeapBlock> heap_;
    std::vector<Value*>       root_stack_;
    std::vector<HeapObject*>  temp_root_stack_;
    size_t live_objects_ = 0;
    size_t live_bytes_   = 0;
    size_t peak_bytes_   = 0;
    size_t threshold_    = 4 * 1024 * 1024;  // 4 MiB trigger

    void mark_phase();
    size_t sweep_phase();
};

// RAII guard for temporary roots.
class TempRoot {
public:
    explicit TempRoot(HeapObject* obj) : obj_(obj) {
        GC::current().push_temp_root(obj_);
    }
    ~TempRoot() { GC::current().pop_temp_root(obj_); }
    HeapObject* get() const noexcept { return obj_; }
    TempRoot(TempRoot const&) = delete;
    TempRoot& operator=(TempRoot const&) = delete;
private:
    HeapObject* obj_;
};

// HeapObjectTraits: maps a heap object type to its TypeTag. Specialized
// below for all known heap object types. The primary template is
// declared but not defined; instantiating it for an unknown type is
// a compile error.
template <typename T> struct HeapObjectTraits;

// Macro used by HeapObject subclasses to install GC-aware operator new.
// The allocation stores the byte size in a 16-byte prefix so that
// register_object (called by the HeapObject constructor) can record
// the size for the GC's accounting.
#define RJIT_DECLARE_GC_NEW(T)                                              \
    static void* operator new(size_t bytes) {                               \
        void* raw = ::rjit::GC::current().allocate(bytes + 16,             \
            ::rjit::TypeTag(::rjit::HeapObjectTraits<T>::tag_value));       \
        *static_cast<size_t*>(raw) = bytes;                                 \
        return static_cast<char*>(raw) + 16;                                \
    }                                                                        \
    static void operator delete(void* p) {                                  \
        if (!p) return;                                                      \
        void* raw = static_cast<char*>(p) - 16;                             \
        ::free(raw);                                                         \
    }

// Install GC for the calling thread (called by Context).
void install_gc_for_thread(GC* g);

}  // namespace rjit

// Pull in HeapObjectTraits specializations for known types. This is
// in a separate header so that the GC header itself doesn't need to
// know about every heap object type.
#include "rjit/core/heap_traits.hpp"

namespace rjit {

// (file intentionally left without a closing brace: this header is
// meant to be included from inside another namespace or at global
// scope. Closing the namespace here would break users that include
// us before their own `namespace rjit {`.)
}  // namespace rjit
