// rjit/core/value.hpp - Tagged R value representation
//
// The Value type is the universal carrier of R data inside the VM. It uses a
// NaN-boxing-like scheme (but with a tag word rather than cramming everything
// into a 64-bit double) so that:
//
//   * scalars (real, int, logical) can be unboxed and re-boxed cheaply;
//   * heap objects (vectors, environments, promises, closures, ...) are
//     referenced through a tagged pointer with a discriminator;
//   * the tag is small enough to fit in a single byte, letting the JIT
//     emit a single `cmp` for type checks.
//
// Important: every Value is *implicitly* a length-1 R value. Longer
// sequences are always Vector objects on the heap. This matches GNU R's
// internal model (a scalar is just a length-1 vector) while still letting
// the optimizer unbox hot scalars into machine registers.
//
// No hardcoded limits: tag values are pulled from an enum, capacity fields
// use size_t, and all sizes flow through the allocator.

#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <atomic>
#include <type_traits>

namespace rjit {

// ---------------------------------------------------------------------------
// Type tags
// ---------------------------------------------------------------------------
//
// Tags are ordered so that numeric-like scalars are grouped together. This
// lets the JIT use a single range check (`tag - kReal <= kLogical - kReal`)
// for "any numeric scalar" guards.

enum class TypeTag : uint8_t {
    // Scalars (immediate payload in Value::bits_)
    kNil      = 0x00,  // R's NULL
    kLogical  = 0x01,  // length-1 logical (TRUE/FALSE/NA)
    kInteger  = 0x02,  // length-1 int32
    kReal     = 0x03,  // length-1 double
    kString   = 0x04,  // length-1 character (interned symbol index)

    // Heap objects (payload is a HeapObject*)
    kVector       = 0x10,  // length>=2 vector (any element type)
    kEnvironment  = 0x11,
    kPromise      = 0x12,
    kClosure      = 0x13,  // function + enclosing environment
    kBuiltin      = 0x14,  // C-implemented primitive
    kSpecial      = 0x15,  // C-implemented special-form (unevaluated args)
    kCall         = 0x16,  // unevaluated call AST node
    kSymbol       = 0x17,  // interned identifier
    kPairList     = 0x18,  // dotted pair list (used for argument lists)
    kBytecodeFn   = 0x19,  // compiled closure
    kJitCode      = 0x1A,  // already-compiled machine code
    kExternalPtr  = 0x1B,

    kMissing      = 0xFF,  // missing-argument sentinel
};

constexpr bool is_scalar_tag(TypeTag t) noexcept {
    return t >= TypeTag::kNil && t <= TypeTag::kString;
}

constexpr bool is_numeric_scalar_tag(TypeTag t) noexcept {
    return t == TypeTag::kReal || t == TypeTag::kInteger || t == TypeTag::kLogical;
}

constexpr bool is_heap_tag(TypeTag t) noexcept {
    return !is_scalar_tag(t) && t != TypeTag::kMissing;
}

const char* tag_name(TypeTag t) noexcept;

// Forward declarations of heap objects
class HeapObject;
class Vector;
class Environment;
class Promise;
class Closure;
class Builtin;
class Symbol;
class PairList;
class Call;
class BytecodeFunction;
class JitCode;

// ---------------------------------------------------------------------------
// Value: 16-byte tagged value
// ---------------------------------------------------------------------------
//
// Layout chosen so the JIT can:
//   * load tag with a single byte load;
//   * load scalar payload with a single 64-bit load (8-byte aligned);
//   * compare two values for tag equality with a 64-bit compare on the
//     first 8 bytes when the payload is identical.
//
// We deliberately *do not* use NaN-boxing. The reasons:
//   1. NaN-boxing forces every pointer to be shifted, adding instructions
//      to every heap access. R does a *lot* of heap access.
//   2. NaN-boxing makes 32-bit ints awkward (they must be sign-extended
//      out of the mantissa), which hurts integer arithmetic.
//   3. We pay 16 bytes per Value, but Values on the interpreter stack are
//      not a cache bottleneck in practice — the bottleneck is heap object
//      layout, which we optimize separately.

class alignas(16) Value {
public:
    constexpr Value() noexcept : tag_(TypeTag::kNil), pad_{0,0,0,0,0,0,0}, bits_(0) {}
    constexpr static Value nil() noexcept { return Value{}; }

    // --- scalar constructors ---
    static Value logical(int32_t v) noexcept {
        Value x; x.tag_ = TypeTag::kLogical; x.bits_ = static_cast<uint64_t>(static_cast<int64_t>(v));
        return x;
    }
    static Value integer(int32_t v) noexcept {
        Value x; x.tag_ = TypeTag::kInteger; x.bits_ = static_cast<uint64_t>(static_cast<int64_t>(v));
        return x;
    }
    static Value real(double v) noexcept {
        Value x; x.tag_ = TypeTag::kReal; x.bits_ = bit_cast<double, uint64_t>(v);
        return x;
    }
    static Value string(uint32_t sym_id) noexcept {
        Value x; x.tag_ = TypeTag::kString; x.bits_ = static_cast<uint64_t>(sym_id);
        return x;
    }
    static Value from_heap(TypeTag tag, HeapObject* obj) noexcept {
        Value x; x.tag_ = tag; x.bits_ = reinterpret_cast<uint64_t>(obj);
        return x;
    }
    static Value missing() noexcept {
        Value x; x.tag_ = TypeTag::kMissing; x.bits_ = 0;
        return x;
    }

    // --- accessors ---
    TypeTag tag() const noexcept { return tag_; }
    bool is(TypeTag t) const noexcept { return tag_ == t; }
    bool is_nil() const noexcept { return tag_ == TypeTag::kNil; }
    bool is_missing() const noexcept { return tag_ == TypeTag::kMissing; }
    bool is_scalar() const noexcept { return is_scalar_tag(tag_); }
    bool is_numeric_scalar() const noexcept { return is_numeric_scalar_tag(tag_); }
    bool is_heap() const noexcept { return is_heap_tag(tag_); }
    bool is_vector()       const noexcept { return tag_ == TypeTag::kVector; }
    bool is_environment()  const noexcept { return tag_ == TypeTag::kEnvironment; }
    bool is_promise()      const noexcept { return tag_ == TypeTag::kPromise; }
    bool is_closure()      const noexcept { return tag_ == TypeTag::kClosure; }
    bool is_builtin()      const noexcept { return tag_ == TypeTag::kBuiltin; }
    bool is_special()      const noexcept { return tag_ == TypeTag::kSpecial; }
    bool is_symbol()       const noexcept { return tag_ == TypeTag::kSymbol; }
    bool is_pairlist()     const noexcept { return tag_ == TypeTag::kPairList; }
    bool is_call()         const noexcept { return tag_ == TypeTag::kCall; }
    bool is_jit_code()     const noexcept { return tag_ == TypeTag::kJitCode; }
    bool is_bytecode_fn()  const noexcept { return tag_ == TypeTag::kBytecodeFn; }
    bool is_real()         const noexcept { return tag_ == TypeTag::kReal; }
    bool is_integer()      const noexcept { return tag_ == TypeTag::kInteger; }
    bool is_logical()      const noexcept { return tag_ == TypeTag::kLogical; }
    bool is_string()       const noexcept { return tag_ == TypeTag::kString; }

    int32_t  as_logical() const noexcept { return static_cast<int32_t>(static_cast<int64_t>(bits_)); }
    int32_t  as_integer() const noexcept { return static_cast<int32_t>(static_cast<int64_t>(bits_)); }
    double   as_real()    const noexcept { return bit_cast<uint64_t, double>(bits_); }
    uint32_t as_string()  const noexcept { return static_cast<uint32_t>(bits_); }

    HeapObject* as_heap() const noexcept {
        return reinterpret_cast<HeapObject*>(bits_);
    }

    template <typename T>
    T* as_heap() const noexcept {
        return reinterpret_cast<T*>(bits_);
    }

    // --- convenience downcasts ---
    Vector*      as_vector()      const noexcept { return as_heap<Vector>(); }
    Environment* as_environment() const noexcept { return as_heap<Environment>(); }
    Promise*     as_promise()     const noexcept { return as_heap<Promise>(); }
    Closure*     as_closure()     const noexcept { return as_heap<Closure>(); }
    Builtin*     as_builtin()     const noexcept { return as_heap<Builtin>(); }
    Symbol*      as_symbol()      const noexcept { return as_heap<Symbol>(); }
    PairList*    as_pairlist()    const noexcept { return as_heap<PairList>(); }
    Call*        as_call()        const noexcept { return as_heap<Call>(); }
    BytecodeFunction* as_bytecode_fn() const noexcept { return as_heap<BytecodeFunction>(); }
    JitCode*     as_jit_code()    const noexcept { return as_heap<JitCode>(); }

    bool operator==(Value const& other) const noexcept {
        return tag_ == other.tag_ && bits_ == other.bits_;
    }
    bool operator!=(Value const& other) const noexcept { return !(*this == other); }

private:
    TypeTag  tag_;
    uint8_t  pad_[7];   // keep tag and bits_ on the same cache line
    uint64_t bits_;     // scalar payload OR heap pointer

    template <typename From, typename To>
    static constexpr To bit_cast(From v) noexcept {
        static_assert(sizeof(From) == sizeof(To), "bit_cast size mismatch");
        To out;
        std::memcpy(&out, &v, sizeof(out));
        return out;
    }
};

static_assert(sizeof(Value) == 16, "Value must be exactly 16 bytes");
static_assert(alignof(Value) == 16, "Value must be 16-byte aligned");

// R's NA sentinels
constexpr int32_t  kNaInt    = -2147483648;  // INT_MIN
constexpr double   kNaReal   = __builtin_nan("0x7ff00000000007a2");
constexpr int32_t  kNaLogical = -2147483648;

bool is_na(Value const& v) noexcept;
Value from_scalar_double(double d) noexcept;
Value from_scalar_int(int32_t i) noexcept;
Value from_scalar_logical(int32_t l) noexcept;

}  // namespace rjit
