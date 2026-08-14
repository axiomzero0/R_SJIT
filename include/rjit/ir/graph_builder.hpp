// rjit/ir/graph_builder.hpp
#pragma once
#include "rjit/ir/sea_of_nodes.hpp"
#include "rjit/bytecode/module.hpp"

namespace rjit {

// Builder converts a BytecodeFunction into a Sea-of-Nodes graph.
// It does basic-block discovery via jump targets, then walks the
// blocks in CFG order emitting data nodes.
class GraphBuilder {
public:
    Graph* build(BytecodeFunction const* fn);
};

}  // namespace rjit
