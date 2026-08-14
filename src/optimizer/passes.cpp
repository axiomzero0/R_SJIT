// rjit/optimizer/passes.cpp
#include "rjit/optimizer/passes.hpp"
#include <unordered_set>
#include <unordered_map>

namespace rjit {

// DCE: remove nodes with no uses and no side effects, iteratively.
// Side-effecting nodes: StoreVar, StoreField, StoreElement, Call,
// CallKnown, Return, Deopt, Safepoint.
static bool has_side_effects(Node const* n) {
    switch (n->kind) {
        case NodeKind::StoreVar:
        case NodeKind::StoreEnvSlot:
        case NodeKind::StoreField:
        case NodeKind::StoreElement:
        case NodeKind::Call:
        case NodeKind::CallKnown:
        case NodeKind::Return:
        case NodeKind::Deopt:
        case NodeKind::Safepoint:
        case NodeKind::TailCall:
        case NodeKind::End:
            return true;
        default:
            return false;
    }
}

bool dce(Graph& g) {
    bool changed = true;
    bool any = false;
    while (changed) {
        changed = false;
        for (size_t i = 0; i < g.nodes().size(); ) {
            Node* n = g.nodes()[i];
            if (n->kind == NodeKind::Start || n->kind == NodeKind::End) { ++i; continue; }
            if (has_side_effects(n)) { ++i; continue; }
            if (n->outputs.empty()) {
                g.remove_node(n);
                changed = true;
                any = true;
            } else {
                ++i;
            }
        }
    }
    return any;
}

bool gvn(Graph& g) {
    // Very simple GVN: hash each (kind, inputs, k) tuple and replace
    // duplicates. This doesn't handle control-flow-sensitive equivalence.
    bool any = false;
    std::unordered_map<uint64_t, Node*> seen;
    for (Node* n : g.nodes()) {
        if (has_side_effects(n)) continue;
        if (n->is_control()) continue;
        uint64_t h = (uint64_t)n->kind * 31 + (uint64_t)n->k;
        for (Node* in : n->inputs) {
            h = h * 131 + (in ? in->id : 0);
        }
        auto it = seen.find(h);
        if (it == seen.end()) {
            seen[h] = n;
        } else {
            g.replace_node(n, it->second);
            any = true;
        }
    }
    return any;
}

bool sccp(Graph& g) {
    // Placeholder: real SCCP is a worklist algorithm.
    return false;
}

bool licm(Graph& g) {
    // Placeholder.
    return false;
}

bool cse(Graph& g) { return gvn(g); }

bool constant_fold(Graph& g) {
    bool any = false;
    for (Node* n : g.nodes()) {
        if (n->kind == NodeKind::Add) {
            // Check if both inputs are constants.
            if (n->inputs.size() >= 3 &&
                n->inputs[1] && n->inputs[2] &&
                n->inputs[1]->kind == NodeKind::ConstReal &&
                n->inputs[2]->kind == NodeKind::ConstReal) {
                double a = n->inputs[1]->constant.as_real();
                double b = n->inputs[2]->constant.as_real();
                Node* folded = g.new_node(NodeKind::ConstReal, {n->inputs[0]});
                folded->constant = Value::real(a + b);
                g.replace_node(n, folded);
                any = true;
            }
        }
    }
    return any;
}

bool strength_reduce(Graph& g) {
    // Placeholder.
    return false;
}

uint32_t run_all_passes(Graph& g) {
    uint32_t iter = 0;
    while (true) {
        bool any = false;
        if (constant_fold(g)) any = true;
        if (gvn(g))           any = true;
        if (sccp(g))          any = true;
        if (licm(g))          any = true;
        if (dce(g))           any = true;
        if (!any) break;
        ++iter;
        if (iter > 100) break;  // safety
    }
    return iter;
}

}  // namespace rjit
