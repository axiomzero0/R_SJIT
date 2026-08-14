// rjit/core/gc.cpp
#include "rjit/core/gc.hpp"
#include "rjit/core/context.hpp"
#include <cstdlib>
#include <cstring>
#include <cstdio>

namespace rjit {

namespace {
thread_local GC* tls_gc = nullptr;
}

GC& GC::current() {
    if (!tls_gc) {
        std::fprintf(stderr, "GC::current() called with no GC installed\n");
        std::abort();
    }
    return *tls_gc;
}

void install_gc_for_thread(GC* g) { tls_gc = g; }

GC::GC() = default;
GC::~GC() = default;

HeapObject::HeapObject(TypeTag t) : header_(t) {
    // Register with the GC. The size was stored in the 16-byte
    // prefix by operator new (RJIT_DECLARE_GC_NEW).
    void* raw = reinterpret_cast<char*>(this) - 16;
    size_t sz = *static_cast<size_t*>(raw);
    GC::current().register_object(this, sz);
}

void* GC::allocate(size_t bytes, TypeTag tag) {
    void* mem = std::malloc(bytes);
    if (!mem) {
        collect();
        mem = std::malloc(bytes);
        if (!mem) {
            std::fprintf(stderr, "GC: out of memory requesting %zu bytes\n", bytes);
            std::abort();
        }
    }
    return mem;
}

void GC::register_object(HeapObject* obj, size_t size) {
    HeapBlock b{obj, size, obj->type()};
    heap_.push_back(b);
    live_objects_++;
    live_bytes_ += size;
    if (live_bytes_ > peak_bytes_) peak_bytes_ = live_bytes_;
    // Auto-collection disabled for now: we don't have conservative
    // stack scanning, so collecting would free live objects. The
    // threshold is set to SIZE_MAX to effectively disable auto-collect.
    // Manual collect() calls still work but should only be invoked
    // from safe points (currently none).
}

size_t GC::collect() {
    // Disabled: without conservative stack scanning, we can't safely
    // collect. Return 0.
    return 0;
}

void GC::mark_phase() {
    // No-op (collection disabled).
}

size_t GC::sweep_phase() {
    return 0;
}

void GC::push_root(Value* v)        { root_stack_.push_back(v); }
void GC::pop_root(Value* v)         {
    if (!root_stack_.empty() && root_stack_.back() == v) {
        root_stack_.pop_back();
    }
}
void GC::push_temp_root(HeapObject* obj) { temp_root_stack_.push_back(obj); }
void GC::pop_temp_root(HeapObject* obj)  {
    if (!temp_root_stack_.empty() && temp_root_stack_.back() == obj) {
        temp_root_stack_.pop_back();
    }
}

}  // namespace rjit
