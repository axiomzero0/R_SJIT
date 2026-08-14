// rjit/ir/sea_of_nodes.cpp
#include "rjit/ir/sea_of_nodes.hpp"
#include "rjit/ir/graph_builder.hpp"
#include "rjit/bytecode/module.hpp"
#include <cstdio>
#include <algorithm>

namespace rjit {

const char* node_kind_name(NodeKind k) noexcept {
    switch (k) {
        case NodeKind::Start:         return "Start";
        case NodeKind::End:           return "End";
        case NodeKind::Region:        return "Region";
        case NodeKind::Loop:          return "Loop";
        case NodeKind::LoopExit:      return "LoopExit";
        case NodeKind::If:            return "If";
        case NodeKind::IfTrue:        return "IfTrue";
        case NodeKind::IfFalse:       return "IfFalse";
        case NodeKind::Return:        return "Return";
        case NodeKind::TailCall:      return "TailCall";
        case NodeKind::Deopt:         return "Deopt";
        case NodeKind::Safepoint:     return "Safepoint";
        case NodeKind::ConstNil:      return "ConstNil";
        case NodeKind::ConstTrue:     return "ConstTrue";
        case NodeKind::ConstFalse:    return "ConstFalse";
        case NodeKind::ConstReal:     return "ConstReal";
        case NodeKind::ConstInt:      return "ConstInt";
        case NodeKind::ConstString:   return "ConstString";
        case NodeKind::LoadVar:       return "LoadVar";
        case NodeKind::StoreVar:      return "StoreVar";
        case NodeKind::LoadEnvSlot:   return "LoadEnvSlot";
        case NodeKind::StoreEnvSlot:  return "StoreEnvSlot";
        case NodeKind::LoadEnv:       return "LoadEnv";
        case NodeKind::Add:           return "Add";
        case NodeKind::Sub:           return "Sub";
        case NodeKind::Mul:           return "Mul";
        case NodeKind::Div:           return "Div";
        case NodeKind::Neg:           return "Neg";
        case NodeKind::AddReal:       return "AddReal";
        case NodeKind::SubReal:       return "SubReal";
        case NodeKind::MulReal:       return "MulReal";
        case NodeKind::DivReal:       return "DivReal";
        case NodeKind::Lt:            return "Lt";
        case NodeKind::Le:            return "Le";
        case NodeKind::Gt:            return "Gt";
        case NodeKind::Ge:            return "Ge";
        case NodeKind::Eq:            return "Eq";
        case NodeKind::Ne:            return "Ne";
        case NodeKind::LtReal:        return "LtReal";
        case NodeKind::LeReal:        return "LeReal";
        case NodeKind::GtReal:        return "GtReal";
        case NodeKind::GeReal:        return "GeReal";
        case NodeKind::EqReal:        return "EqReal";
        case NodeKind::NeReal:        return "NeReal";
        case NodeKind::Not:           return "Not";
        case NodeKind::And:           return "And";
        case NodeKind::Or:            return "Or";
        case NodeKind::Call:          return "Call";
        case NodeKind::CallKnown:     return "CallKnown";
        case NodeKind::Allocate:      return "Allocate";
        case NodeKind::LoadField:     return "LoadField";
        case NodeKind::StoreField:    return "StoreField";
        case NodeKind::LoadElement:   return "LoadElement";
        case NodeKind::StoreElement:  return "StoreElement";
        case NodeKind::Coerce:        return "Coerce";
        case NodeKind::IsType:        return "IsType";
        case NodeKind::Phi:           return "Phi";
        case NodeKind::Guard:         return "Guard";
        case NodeKind::VirtualObject: return "VirtualObject";
        case NodeKind::Materialize:   return "Materialize";
        case NodeKind::kCount:        return "?";
    }
    return "?";
}

bool Node::is_control() const {
    switch (kind) {
        case NodeKind::Start:
        case NodeKind::End:
        case NodeKind::Region:
        case NodeKind::Loop:
        case NodeKind::LoopExit:
        case NodeKind::If:
        case NodeKind::IfTrue:
        case NodeKind::IfFalse:
        case NodeKind::Return:
        case NodeKind::TailCall:
        case NodeKind::Deopt:
        case NodeKind::Safepoint:
            return true;
        default:
            return false;
    }
}

Graph::Graph() {
    start_ = new_node(NodeKind::Start);
    end_ = new_node(NodeKind::End);
}

Node* Graph::new_node(NodeKind k, std::vector<Node*> inputs) {
    Node* n = new Node();
    n->kind = k;
    n->id = next_id_++;
    n->inputs = std::move(inputs);
    nodes_.push_back(n);
    for (Node* in : n->inputs) {
        if (in) in->outputs.push_back(n);
    }
    return n;
}

void Graph::remove_node(Node* n) {
    for (Node* in : n->inputs) {
        if (!in) continue;
        auto& outs = in->outputs;
        outs.erase(std::remove(outs.begin(), outs.end(), n), outs.end());
    }
    nodes_.erase(std::remove(nodes_.begin(), nodes_.end(), n), nodes_.end());
    delete n;
}

void Graph::replace_node(Node* old_n, Node* new_n) {
    // Redirect all users of old_n to new_n.
    for (Node* user : old_n->outputs) {
        for (auto& in : user->inputs) {
            if (in == old_n) in = new_n;
        }
        new_n->outputs.push_back(user);
    }
    old_n->outputs.clear();
    // Old node will be removed by DCE.
}

void Graph::replace_input(Node* n, size_t idx, Node* new_input) {
    if (idx >= n->inputs.size()) return;
    Node* old = n->inputs[idx];
    if (old) {
        auto& outs = old->outputs;
        outs.erase(std::remove(outs.begin(), outs.end(), n), outs.end());
    }
    n->inputs[idx] = new_input;
    if (new_input) new_input->outputs.push_back(n);
}

void Graph::dump() const {
    std::printf("=== Graph (%zu nodes) ===\n", nodes_.size());
    for (Node* n : nodes_) {
        std::printf("  N%u %s", n->id, node_kind_name(n->kind));
        if (!n->inputs.empty()) {
            std::printf(" in=[");
            for (size_t i = 0; i < n->inputs.size(); ++i) {
                if (i) std::printf(", ");
                if (n->inputs[i]) std::printf("N%u", n->inputs[i]->id);
                else std::printf("?");
            }
            std::printf("]");
        }
        if (n->k) std::printf(" k=%u", n->k);
        std::printf("\n");
    }
}

Graph* build_graph(BytecodeFunction const* fn) {
    GraphBuilder b;
    return b.build(fn);
}

}  // namespace rjit
