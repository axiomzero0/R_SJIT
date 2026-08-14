// rjit/core/symbol.hpp - Symbol objects (interned identifiers as heap objects)
//
// In R, symbols are first-class: they can be passed around, stored in
// environments, and reified by `quote()` / `substitute()`. We
// represent them as heap objects wrapping a symbol id from the
// Context's symbol table.

#pragma once
#include "rjit/core/value.hpp"
#include "rjit/core/gc.hpp"

namespace rjit {

class Symbol : public HeapObject {
public:
    RJIT_DECLARE_GC_NEW(Symbol)

    explicit Symbol(uint32_t id) : HeapObject(TypeTag::kSymbol), id_(id) {}

    uint32_t id() const noexcept { return id_; }

    void trace(Visitor& v) const override {}

private:
    uint32_t id_;
};

}  // namespace rjit
