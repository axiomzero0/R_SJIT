// rjit/core/attributes.hpp - R object attributes
//
// R allows arbitrary key->value attributes on any vector. The most
// common are: `names`, `dim`, `dimnames`, `class`, `tsp` (time series),
// `levels` (factors). Attributes are themselves stored as a pairlist
// or, more efficiently, as a (names_vector, values_list) pair.
//
// Most vectors never have attributes, so we represent attributes as an
// out-of-line optional object — a `nullptr` attributes pointer means
// "no attributes". This keeps the hot path fast.

#pragma once

#include <cstdint>
#include <string>
#include "rjit/core/value.hpp"
#include "rjit/core/gc.hpp"

namespace rjit {

class Vector;
class Symbol;
class PairList;

class Attributes : public HeapObject {
public:
    RJIT_DECLARE_GC_NEW(Attributes)

    Attributes();

    // Get the attribute value for a symbol, or nil if not present.
    Value get(uint32_t sym_id) const noexcept;

    // Set an attribute (replacing if present).
    void set(uint32_t sym_id, Value v);

    // Remove an attribute. Returns true if removed.
    bool remove(uint32_t sym_id);

    // Iterate. Returns false when done.
    bool next(size_t& cursor, uint32_t& sym_out, Value& val_out) const;

    size_t size() const noexcept { return count_; }

    void trace(Visitor& v) const override;

private:
    // Stored as parallel arrays for cache-friendly iteration.
    std::vector<uint32_t> names_;
    std::vector<Value>    values_;
    size_t                count_ = 0;
};

// Convenience: attach a single attribute to a value, returning a new
// value (since copy-on-write may apply).
Value with_attribute(Value v, uint32_t sym_id, Value attr);

// Get the class attribute as a list of symbol ids (R allows multiple
// inheritance). Returns empty vector if no class.
std::vector<uint32_t> get_class_chain(Value v);

// Get the `names` attribute as a vector of symbol ids.
Vector* get_names(Value v);

}  // namespace rjit
