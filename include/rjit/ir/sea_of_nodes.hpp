// rjit/ir/sea_of_nodes.hpp - Sea of Nodes IR
//
// The Sea of Nodes representation places *every* value-producing
// operation in a single graph, with control flow as a separate
// dimension. This makes many optimizations (GVN, constant folding,
// loop-invariant code motion) much simpler than in a basic-block IR.
//
// Each Node has:
//   * an opcode (Add, Sub, LoadVar, Call, Start, Region, If, ...)
//   * a list of input edges (data dependencies + control)
//   * a list of output edges (uses)
//   * a type (from the type lattice)
//   * an id (for stable iteration)
//
// Control nodes (Start, End, Region, If, IfTrue, IfFalse, Loop,
// LoopExit, Return) form a control subgraph. Data nodes refer to
// their control predecessor via inputs[0].

#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include "rjit/ir/types.hpp"
#include "rjit/core/value.hpp"

namespace rjit {

class Node;
class Graph;

enum class NodeKind : uint16_t {
    // Control
    Start,
    End,
    Region,         // merge point
    Loop,           // loop header
    LoopExit,
    If,
    IfTrue,
    IfFalse,
    Return,
    TailCall,
    Deopt,
    Safepoint,

    // Constants
    ConstNil,
    ConstTrue,
    ConstFalse,
    ConstReal,
    ConstInt,
    ConstString,

    // Variables
    LoadVar,        // inputs: [ctrl, env]
    StoreVar,       // inputs: [ctrl, env, value]
    LoadEnvSlot,    // inputs: [ctrl, env]; k = slot
    StoreEnvSlot,   // inputs: [ctrl, env, value]; k = slot
    LoadEnv,        // current environment

    // Arithmetic
    Add, Sub, Mul, Div, Neg,
    AddReal, SubReal, MulReal, DivReal,  // specialized
    Lt, Le, Gt, Ge, Eq, Ne,
    LtReal, LeReal, GtReal, GeReal, EqReal, NeReal,
    Not, And, Or,

    // Function call
    Call,           // inputs: [ctrl, callee, args...]
    CallKnown,      // k = function index

    // Memory
    Allocate,
    LoadField,      // k = field offset
    StoreField,
    LoadElement,    // inputs: [ctrl, vector, index]
    StoreElement,

    // Type operations
    Coerce,         // inputs: [ctrl, value]; k = target type
    IsType,         // inputs: [ctrl, value]; k = type to check

    // Phi (merge point)
    Phi,            // inputs: [region, v1, v2, ...]

    // Guard (deopt on failure)
    Guard,          // inputs: [ctrl, value]; k = expected type

    // PEA / Escape analysis
    VirtualObject,  // a not-yet-materialized object
    Materialize,    // force a virtual object into a real one

    kCount,
};

const char* node_kind_name(NodeKind k) noexcept;

class Node {
public:
    NodeKind        kind;
    uint32_t        id;
    Type            type;
    std::vector<Node*> inputs;
    std::vector<Node*> outputs;
    uint32_t        k = 0;       // immediate (slot, type, etc.)
    Value           constant;    // for Const* nodes
    bool            is_control() const;

    // Optimization flags
    bool            visited      = false;
    bool            in_worklist  = false;
};

class Graph {
public:
    Graph();

    Node* new_node(NodeKind k, std::vector<Node*> inputs = {});
    void  remove_node(Node* n);
    void  replace_node(Node* old_n, Node* new_n);
    void  replace_input(Node* n, size_t idx, Node* new_input);

    Node* start() const noexcept { return start_; }
    Node* end()   const noexcept { return end_; }

    std::vector<Node*> const& nodes() const noexcept { return nodes_; }

    // Iterate over all nodes (in arbitrary order). The graph owns
    // the nodes; they are freed when the graph is destroyed.
    void dump() const;

private:
    std::vector<Node*> nodes_;
    Node*              start_ = nullptr;
    Node*              end_   = nullptr;
    uint32_t           next_id_ = 0;
};

// Build a Sea-of-Nodes graph from a BytecodeFunction.
Graph* build_graph(BytecodeFunction const* fn);

}  // namespace rjit
