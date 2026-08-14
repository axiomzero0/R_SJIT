// rjit/codegen/x86_64.cpp
#include "rjit/codegen/x86_64.hpp"
#include "rjit/jit/baseline.hpp"
#include "rjit/core/context.hpp"

namespace rjit {

JitCode* codegen_x86_64(Graph& g) {
    // For now: return nullptr. A real implementation walks the graph,
    // does linear scan register allocation, and emits machine code
    // for each node.
    (void)g;
    return nullptr;
}

}  // namespace rjit
