// rjit/ir/types.hpp - Type lattice for the IR
//
// The IR uses a refinement of TypeTag that supports:
//   * scalar types (Real, Int, Logical, String)
//   * vector types with element type and length range
//   * shape types (environment shapes)
//   * Top (unknown) and Bottom (unreachable)
//
// Types are interned so that pointer equality means type equality.

#pragma once
#include <cstdint>
#include "rjit/core/value.hpp"

namespace rjit {

class Type {
public:
    enum class Kind : uint8_t {
        kTop,
        kBottom,
        kNil,
        kLogicalScalar,
        kIntegerScalar,
        kRealScalar,
        kStringScalar,
        kVector,         // element type + length range
        kEnvironment,    // + shape id (or 0 = any)
        kClosure,
        kPromise,
        kBuiltin,
    };

    Kind      kind;
    TypeTag   element_tag;   // for kVector
    uint32_t  shape_id;      // for kEnvironment
    uint32_t  min_length;
    uint32_t  max_length;

    Type() : kind(Kind::kTop), element_tag(TypeTag::kNil), shape_id(0), min_length(0), max_length(UINT32_MAX) {}

    static Type top() { Type t; return t; }
    static Type bottom() { Type t; t.kind = Kind::kBottom; return t; }
    static Type real_scalar() { Type t; t.kind = Kind::kRealScalar; return t; }
    static Type int_scalar() { Type t; t.kind = Kind::kIntegerScalar; return t; }
    static Type logical_scalar() { Type t; t.kind = Kind::kLogicalScalar; return t; }
    static Type string_scalar() { Type t; t.kind = Kind::kStringScalar; return t; }
    static Type nil() { Type t; t.kind = Kind::kNil; return t; }

    bool is_top() const { return kind == Kind::kTop; }
    bool is_bottom() const { return kind == Kind::kBottom; }

    // Type meet (intersection). Used in SSA phi nodes.
    Type meet(Type const& other) const;

    // Type join (union). Used for type widening.
    Type join(Type const& other) const;
};

}  // namespace rjit
