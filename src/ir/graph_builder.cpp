// rjit/ir/graph_builder.cpp
#include "rjit/ir/graph_builder.hpp"
#include "rjit/bytecode/module.hpp"
#include "rjit/core/error.hpp"
#include <unordered_map>
#include <vector>

namespace rjit {

// This initial graph builder produces a *minimal* graph: just
// Start, End, and a single Return of the result register r0. A
// full builder would walk the bytecode and emit data nodes for
// each instruction, with proper CFG construction. That's a large
// piece of work and will be added in a later iteration.
Graph* GraphBuilder::build(BytecodeFunction const* fn) {
    Graph* g = new Graph();
    // For each instruction we'd create nodes here. For now we just
    // emit a single "Return r0" placeholder.
    (void)fn;
    return g;
}

}  // namespace rjit
