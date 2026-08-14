// rjit/core/vector.hpp - R vector representation
//
// Vectors are the workhorse of R. Almost every value is, or behaves like,
// a vector. Our representation is designed to:
//
//   * minimize header overhead per vector (single fixed header);
//   * allow the optimizer to reason about element type, length, NA
//     presence, contiguity, and ALTREP-style deferred representation;
//   * support copy-on-write semantics via an explicit NAMED/REFCNT
//     counter, mirroring GNU R's `NAMED` mechanism but with a single
//     bit (0/1+) rather than a saturating counter (modern research
//     shows the saturating counter is unnecessary in practice).
//
// We do NOT use std::vector<...> internally. Reasons:
//   1. We need to control the allocator (GC-aware).
//   2. We need to keep the data buffer *adjacent* to the header for
//      cache locality on short vectors (<=8 elements). For longer
//      vectors the data is allocated separately.
//   3. std::vector's resize semantics can trigger copies that violate
//      R's copy-on-write invariants.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include "rjit/core/value.hpp"
#include "rjit/core/gc.hpp"

namespace rjit {

enum class VectorType : uint8_t {
    kLogical = 0,
    kInteger = 1,
    kReal    = 2,
    kString  = 3,
    kComplex = 4,
    kRaw     = 5,
    kList    = 6,   // heterogeneous
};

const char* vector_type_name(VectorType vt) noexcept;
size_t element_size(VectorType vt) noexcept;

// ALTREP-style deferred representation. The default is kConcrete (data
// is in the buffer). Other variants allow us to represent things like
// `1:1e9` without materializing a billion elements.
enum class VectorRepr : uint8_t {
    kConcrete   = 0,  // data buffer is authoritative
    kRange      = 1,  // 1:n or n:1, not yet materialized
    kRepeat     = 2,  // rep(x, n) with concrete x
    kCoercion   = 3,  // type-coerced view of another vector
};

// Flags that the optimizer can probe. All flags are conservative: if a
// flag is set, the property definitely holds. If it is unset, the
// property may or may not hold (must be checked at runtime or
// re-derived by analysis).
struct VectorFlags {
    uint8_t bits;
    // bit 0: no NA in any element
    // bit 1: all elements finite (no Inf/-Inf/NaN)
    // bit 2: no attributes attached
    // bit 3: no class attribute
    // bit 4: no dim/dimnames attribute
    // bit 5: no names attribute
    // bit 6: data is 16-byte aligned
    // bit 7: data is contiguous (no stride)
    static constexpr uint8_t kNoNA          = 1u << 0;
    static constexpr uint8_t kAllFinite     = 1u << 1;
    static constexpr uint8_t kNoAttributes  = 1u << 2;
    static constexpr uint8_t kNoClass       = 1u << 3;
    static constexpr uint8_t kNoDim         = 1u << 4;
    static constexpr uint8_t kNoNames       = 1u << 5;
    static constexpr uint8_t kAligned       = 1u << 6;
    static constexpr uint8_t kContiguous    = 1u << 7;

    constexpr VectorFlags() : bits(kNoNA | kAllFinite | kNoAttributes | kNoClass | kNoDim | kNoNames | kAligned | kContiguous) {}
    bool has(uint8_t f) const noexcept { return (bits & f) != 0; }
    void set(uint8_t f) noexcept { bits |= f; }
    void clear(uint8_t f) noexcept { bits &= ~f; }
};

class Vector : public HeapObject {
public:
    RJIT_DECLARE_GC_NEW(Vector)

    Vector(VectorType vt, size_t length);
    ~Vector();

    VectorType vtype()  const noexcept { return vtype_; }
    TypeTag    type()   const noexcept { return TypeTag::kVector; }
    size_t     length() const noexcept { return length_; }
    VectorRepr repr()   const noexcept { return repr_; }
    VectorFlags flags() const noexcept { return flags_; }
    void set_flags(VectorFlags f) noexcept { flags_ = f; }

    // Element access (concrete vectors only). These are NOT bounds-checked
    // in release builds; the interpreter/JIT must check length first.
    int32_t  integer_at(size_t i) const noexcept;
    double   real_at(size_t i)    const noexcept;
    int32_t  logical_at(size_t i) const noexcept;
    uint32_t string_at(size_t i)  const noexcept;
    Value    list_at(size_t i)    const noexcept;

    void set_integer(size_t i, int32_t v) noexcept;
    void set_real(size_t i, double v) noexcept;
    void set_logical(size_t i, int32_t v) noexcept;
    void set_string(size_t i, uint32_t v) noexcept;
    void set_list(size_t i, Value v) noexcept;

    // Raw data pointer (for JIT to use in vectorized loops)
    void*       data()       noexcept { return data_; }
    void const* data() const noexcept { return data_; }

    // Coercion (lazy where possible). Returns a new vector (or self if
    // no coercion is needed). The caller owns the new reference.
    Vector* coerce_to(VectorType target) const;

    // Concatenation (`c(...)`). Returns a new vector.
    static Vector* concat(Vector* const* parts, size_t n_parts);

    // Constructs a range vector without materializing: `1:n`.
    static Vector* range(int64_t start, int64_t end);

    // Materializes a deferred representation into a concrete buffer.
    void materialize();

    void trace(Visitor& v) const override;

private:
    VectorType  vtype_;
    VectorRepr  repr_;
    VectorFlags flags_;
    size_t      length_;
    void*       data_;          // owned
    // For deferred representations:
    int64_t     range_start_ = 0;
    int64_t     range_step_  = 1;
    Vector*     altrep_dep_  = nullptr;  // for kCoercion or kRepeat

    void allocate_data();
    void free_data();
};

// --- Helper constructors ---

Value make_real_vector(double const* data, size_t n);
Value make_int_vector(int32_t const* data, size_t n);
Value make_logical_vector(int32_t const* data, size_t n);
Value make_string_vector(uint32_t const* data, size_t n);

// Length-1 vector wrapping a scalar. Used when an operation generalizes
// a scalar to a vector (rare; usually scalars stay scalar).
Value scalar_to_vector(Value scalar);

}  // namespace rjit
