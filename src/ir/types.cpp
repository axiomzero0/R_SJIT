// rjit/ir/types.cpp
#include "rjit/ir/types.hpp"

namespace rjit {

Type Type::meet(Type const& other) const {
    if (is_top())   return other;
    if (other.is_top()) return *this;
    if (is_bottom() || other.is_bottom()) return bottom();
    if (kind == other.kind) {
        if (kind == Kind::kEnvironment) {
            if (shape_id == other.shape_id) return *this;
            // Different shapes — meet to "any environment".
            Type t = *this; t.shape_id = 0; return t;
        }
        return *this;
    }
    // Different non-top kinds: meet to bottom (impossible).
    return bottom();
}

Type Type::join(Type const& other) const {
    if (is_bottom()) return other;
    if (other.is_bottom()) return *this;
    if (is_top() || other.is_top()) return top();
    if (kind == other.kind) return *this;
    // Different specific kinds: join to top (unknown).
    return top();
}

}  // namespace rjit
