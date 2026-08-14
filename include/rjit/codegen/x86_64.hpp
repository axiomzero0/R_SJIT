// rjit/codegen/x86_64.hpp - x86-64 machine code generation from Sea-of-Nodes
//
// This is the back-end of the optimizing JIT. It walks a Graph and
// emits machine code for x86-64. Currently a stub that calls the
// baseline JIT for code emission.

#pragma once
#include "rjit/ir/sea_of_nodes.hpp"
#include "rjit/jit/jit_code.hpp"

namespace rjit {

class Graph;

JitCode* codegen_x86_64(Graph& g);

}  // namespace rjit
