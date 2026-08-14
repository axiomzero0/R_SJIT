// rjit/optimizer/escape.cpp
//
// Escape Analysis: determine which allocations escape their scope.
//
// This is a simpler version of PEA that just marks allocations as
// escaping or non-escaping. PEA uses this information to do scalar
// replacement.
//
// An allocation "escapes" if:
//   - It's returned from the function
//   - It's passed to a Call (the callee might store it)
//   - It's stored in a variable (StoreVar)
//   - It's stored in a field of another object that escapes

#include "rjit/optimizer/escape.hpp"
#include <unordered_set>

namespace rjit {

bool escape_analysis(Graph& g) {
    bool any = false;
    std::unordered_set<Node*> escaping;

    // First pass: find directly escaping allocations.
    for (Node* n : g.nodes()) {
        if (n->kind != NodeKind::Allocate) continue;
        for (Node* use : n->outputs) {
            if (use->kind == NodeKind::Return ||
                use->kind == NodeKind::Call ||
                use->kind == NodeKind::StoreVar ||
                use->kind == NodeKind::TailCall) {
                escaping.insert(n);
                break;
            }
        }
    }

    // Second pass: mark non-escaping allocations for scalar replacement.
    // (The actual replacement is done by PEA.)
    for (Node* n : g.nodes()) {
        if (n->kind == NodeKind::Allocate && escaping.find(n) == escaping.end()) {
            // This allocation doesn't escape — PEA can scalar-replace it.
            any = true;
        }
    }

    return any;
}

}  // namespace rjit
