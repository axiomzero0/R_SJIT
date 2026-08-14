// rjit/ir/graph_builder.cpp
//
// Builds a Sea-of-Nodes graph from a BytecodeFunction.
//
// The builder walks the bytecode in CFG order, maintaining a mapping
// from bytecode register index to current SSA Node*. At control-flow
// merge points (loop headers, if-else joins), Phi nodes are inserted
// to merge values from different predecessors.
//
// The resulting graph has:
//   - A Start node (entry)
//   - An End node (exit)
//   - Control nodes (If, IfTrue, IfFalse, Region, Loop, Return)
//   - Data nodes (Const, LoadVar, StoreVar, Add, Sub, etc.)
//   - Phi nodes at merge points

#include "rjit/ir/graph_builder.hpp"
#include "rjit/ir/sea_of_nodes.hpp"
#include "rjit/bytecode/module.hpp"
#include "rjit/core/error.hpp"
#include <vector>
#include <unordered_map>

namespace rjit {

// A basic block in the CFG.
struct BasicBlock {
    uint32_t start_pc;
    uint32_t end_pc;
    std::vector<uint32_t> predecessors;
    std::vector<uint32_t> successors;
    bool is_loop_header = false;
};

// Build the CFG from bytecode by scanning for jump targets.
static std::vector<BasicBlock> build_cfg(BytecodeFunction const* fn) {
    std::vector<BasicBlock> blocks;

    // Find all jump targets (leaders).
    std::vector<bool> is_leader(fn->code.size(), false);
    is_leader[0] = true;
    for (size_t i = 0; i < fn->code.size(); ++i) {
        Instr const& in = fn->code[i];
        if (in.op == Op::JUMP || in.op == Op::JUMP_IF_FALSE ||
            in.op == Op::JUMP_IF_TRUE || in.op == Op::JUMP_IF_NA) {
            if (in.k < fn->code.size()) is_leader[in.k] = true;
            if (i + 1 < fn->code.size()) is_leader[i + 1] = true;
        }
        if (in.op == Op::LOOP_HEADER || in.op == Op::LOOP_BACKEDGE) {
            is_leader[i] = true;
        }
    }

    // Create basic blocks.
    for (size_t i = 0; i < fn->code.size(); ++i) {
        if (is_leader[i]) {
            BasicBlock bb;
            bb.start_pc = static_cast<uint32_t>(i);
            // Find end of block (next leader or jump/return).
            for (size_t j = i; j < fn->code.size(); ++j) {
                Instr const& in = fn->code[j];
                if (in.op == Op::JUMP || in.op == Op::JUMP_IF_FALSE ||
                    in.op == Op::JUMP_IF_TRUE || in.op == Op::RETURN ||
                    in.op == Op::RETURN_NULL || in.op == Op::HALT) {
                    bb.end_pc = static_cast<uint32_t>(j);
                    break;
                }
                if (j + 1 < fn->code.size() && is_leader[j + 1]) {
                    bb.end_pc = static_cast<uint32_t>(j);
                    break;
                }
            }
            if (bb.end_pc == 0) bb.end_pc = static_cast<uint32_t>(fn->code.size() - 1);
            if (fn->code[bb.start_pc].op == Op::LOOP_HEADER) bb.is_loop_header = true;
            blocks.push_back(bb);
        }
    }

    // Connect successors.
    for (size_t i = 0; i < blocks.size(); ++i) {
        BasicBlock& bb = blocks[i];
        Instr const& last = fn->code[bb.end_pc];
        if (last.op == Op::JUMP) {
            // Find the block starting at last.k
            for (size_t j = 0; j < blocks.size(); ++j) {
                if (blocks[j].start_pc == last.k) {
                    bb.successors.push_back(static_cast<uint32_t>(j));
                    blocks[j].predecessors.push_back(static_cast<uint32_t>(i));
                    break;
                }
            }
        } else if (last.op == Op::JUMP_IF_FALSE || last.op == Op::JUMP_IF_TRUE) {
            // Fall-through successor
            if (bb.end_pc + 1 < fn->code.size()) {
                for (size_t j = 0; j < blocks.size(); ++j) {
                    if (blocks[j].start_pc == bb.end_pc + 1) {
                        bb.successors.push_back(static_cast<uint32_t>(j));
                        blocks[j].predecessors.push_back(static_cast<uint32_t>(i));
                        break;
                    }
                }
            }
            // Jump target successor
            for (size_t j = 0; j < blocks.size(); ++j) {
                if (blocks[j].start_pc == last.k) {
                    bb.successors.push_back(static_cast<uint32_t>(j));
                    blocks[j].predecessors.push_back(static_cast<uint32_t>(i));
                    break;
                }
            }
        } else if (last.op == Op::RETURN || last.op == Op::RETURN_NULL || last.op == Op::HALT) {
            // No successors (terminal).
        } else {
            // Fall-through to next block.
            if (i + 1 < blocks.size()) {
                bb.successors.push_back(static_cast<uint32_t>(i + 1));
                blocks[i + 1].predecessors.push_back(static_cast<uint32_t>(i));
            }
        }
    }

    return blocks;
}

Graph* GraphBuilder::build(BytecodeFunction const* fn) {
    Graph* g = new Graph();

    // Build the CFG.
    std::vector<BasicBlock> blocks = build_cfg(fn);

    // Map from PC to current SSA value for each register.
    // reg_state[pc][reg_idx] = Node*
    // We use a simpler approach: maintain a per-block entry state.
    std::vector<std::vector<Node*>> entry_state(blocks.size(),
        std::vector<Node*>(fn->nregs, nullptr));

    // For the entry block, all registers start as "undefined" (null).
    // We'll create LoadVar nodes for parameters.

    // Map from PC to the control node at that PC.
    std::unordered_map<uint32_t, Node*> control_at;

    // Start node is the initial control.
    Node* current_control = g->start();
    control_at[0] = current_control;

    // Process blocks in order.
    for (size_t bi = 0; bi < blocks.size(); ++bi) {
        BasicBlock const& bb = blocks[bi];

        // If this block has multiple predecessors, create a Region/Phi.
        if (bb.predecessors.size() > 1) {
            std::vector<Node*> preds;
            for (uint32_t pred_idx : bb.predecessors) {
                preds.push_back(control_at[blocks[pred_idx].end_pc]);
            }
            Node* region = g->new_node(NodeKind::Region, preds);
            current_control = region;
            control_at[bb.start_pc] = region;

            // Create Phi nodes for each register.
            for (uint32_t reg = 0; reg < fn->nregs; ++reg) {
                std::vector<Node*> inputs = {region};
                for (uint32_t pred_idx : bb.predecessors) {
                    inputs.push_back(entry_state[pred_idx][reg]);
                }
                Node* phi = g->new_node(NodeKind::Phi, inputs);
                entry_state[bi][reg] = phi;
            }
        } else if (bb.predecessors.size() == 1) {
            // Single predecessor: inherit state.
            uint32_t pred_idx = bb.predecessors[0];
            entry_state[bi] = entry_state[pred_idx];
            current_control = control_at[blocks[pred_idx].end_pc];
            if (!current_control) current_control = g->start();
        }

        // Process instructions in this block.
        std::vector<Node*> state = entry_state[bi];
        for (uint32_t pc = bb.start_pc; pc <= bb.end_pc && pc < fn->code.size(); ++pc) {
            Instr const& in = fn->code[pc];
            switch (in.op) {
                case Op::NOP: break;
                case Op::HALT: {
                    Node* ret = g->new_node(NodeKind::Return, {current_control, state[0]});
                    (void)ret;
                    break;
                }
                case Op::LOAD_NIL:
                    state[in.rdest] = g->new_node(NodeKind::ConstNil, {current_control});
                    break;
                case Op::LOAD_TRUE:
                    state[in.rdest] = g->new_node(NodeKind::ConstTrue, {current_control});
                    break;
                case Op::LOAD_FALSE:
                    state[in.rdest] = g->new_node(NodeKind::ConstFalse, {current_control});
                    break;
                case Op::LOAD_REAL:
                case Op::LOAD_INT: {
                    Node* n = g->new_node(
                        in.op == Op::LOAD_REAL ? NodeKind::ConstReal : NodeKind::ConstInt,
                        {current_control});
                    n->constant = fn->constants[in.k];
                    state[in.rdest] = n;
                    break;
                }
                case Op::LOAD_LOCAL:
                    state[in.rdest] = state[in.ra];
                    break;
                case Op::LOAD_VAR: {
                    Node* env = g->new_node(NodeKind::LoadEnv, {current_control});
                    Node* n = g->new_node(NodeKind::LoadVar, {current_control, env});
                    n->k = in.k;
                    state[in.rdest] = n;
                    break;
                }
                case Op::STORE_VAR: {
                    Node* env = g->new_node(NodeKind::LoadEnv, {current_control});
                    Node* n = g->new_node(NodeKind::StoreVar, {current_control, env, state[in.ra]});
                    n->k = in.k;
                    current_control = n;
                    break;
                }
                case Op::JUMP: break;  // handled by CFG
                case Op::JUMP_IF_FALSE: {
                    Node* if_node = g->new_node(NodeKind::If, {current_control, state[in.ra]});
                    Node* if_false = g->new_node(NodeKind::IfFalse, {if_node});
                    Node* if_true = g->new_node(NodeKind::IfTrue, {if_node});
                    control_at[pc] = if_true;  // fall-through
                    // The jump target's control will be set when we process that block.
                    (void)if_false;
                    current_control = if_true;
                    break;
                }
                case Op::ADD: {
                    Node* n = g->new_node(NodeKind::Add, {current_control, state[in.ra], state[in.rb]});
                    state[in.rdest] = n;
                    break;
                }
                case Op::SUB: {
                    Node* n = g->new_node(NodeKind::Sub, {current_control, state[in.ra], state[in.rb]});
                    state[in.rdest] = n;
                    break;
                }
                case Op::MUL: {
                    Node* n = g->new_node(NodeKind::Mul, {current_control, state[in.ra], state[in.rb]});
                    state[in.rdest] = n;
                    break;
                }
                case Op::DIV: {
                    Node* n = g->new_node(NodeKind::Div, {current_control, state[in.ra], state[in.rb]});
                    state[in.rdest] = n;
                    break;
                }
                case Op::NEG: {
                    Node* n = g->new_node(NodeKind::Neg, {current_control, state[in.ra]});
                    state[in.rdest] = n;
                    break;
                }
                case Op::LT: {
                    Node* n = g->new_node(NodeKind::Lt, {current_control, state[in.ra], state[in.rb]});
                    state[in.rdest] = n;
                    break;
                }
                case Op::LE: {
                    Node* n = g->new_node(NodeKind::Le, {current_control, state[in.ra], state[in.rb]});
                    state[in.rdest] = n;
                    break;
                }
                case Op::GT: {
                    Node* n = g->new_node(NodeKind::Gt, {current_control, state[in.ra], state[in.rb]});
                    state[in.rdest] = n;
                    break;
                }
                case Op::GE: {
                    Node* n = g->new_node(NodeKind::Ge, {current_control, state[in.ra], state[in.rb]});
                    state[in.rdest] = n;
                    break;
                }
                case Op::EQ: {
                    Node* n = g->new_node(NodeKind::Eq, {current_control, state[in.ra], state[in.rb]});
                    state[in.rdest] = n;
                    break;
                }
                case Op::NE: {
                    Node* n = g->new_node(NodeKind::Ne, {current_control, state[in.ra], state[in.rb]});
                    state[in.rdest] = n;
                    break;
                }
                case Op::CALL: {
                    std::vector<Node*> inputs = {current_control, state[in.ra]};
                    for (uint32_t i = 0; i < in.k; ++i) {
                        inputs.push_back(state[in.ra + 1 + i]);
                    }
                    Node* n = g->new_node(NodeKind::Call, inputs);
                    n->k = in.k;
                    state[in.rdest] = n;
                    current_control = n;  // Call has side effects
                    break;
                }
                case Op::MAKE_CLOSURE: {
                    Node* n = g->new_node(NodeKind::LoadEnv, {current_control});
                    n->k = in.k;
                    state[in.rdest] = n;
                    break;
                }
                case Op::RETURN: {
                    Node* n = g->new_node(NodeKind::Return, {current_control, state[in.ra]});
                    current_control = n;
                    break;
                }
                case Op::RETURN_NULL: {
                    Node* nil = g->new_node(NodeKind::ConstNil, {current_control});
                    Node* n = g->new_node(NodeKind::Return, {current_control, nil});
                    current_control = n;
                    break;
                }
                case Op::MAKE_SEQ: {
                    Node* n = g->new_node(NodeKind::Call, {current_control, state[in.ra], state[in.rb]});
                    state[in.rdest] = n;
                    break;
                }
                case Op::LENGTH: {
                    Node* n = g->new_node(NodeKind::Call, {current_control, state[in.ra]});
                    state[in.rdest] = n;
                    break;
                }
                case Op::LOOP_HEADER: {
                    Node* loop = g->new_node(NodeKind::Loop, {current_control});
                    current_control = loop;
                    break;
                }
                case Op::LOOP_BACKEDGE: break;
                default: break;
            }
        }

        // Save the exit state for this block.
        control_at[bb.end_pc] = current_control;
        if (bi + 1 < blocks.size()) {
            entry_state[bi + 1] = state;
        }
    }

    return g;
}

}  // namespace rjit
