// rjit/codegen/regalloc.cpp
#include "rjit/codegen/regalloc.hpp"

namespace rjit {

RegAlloc::RegAlloc() = default;

void RegAlloc::run(Graph& g) {
    // Placeholder: real linear-scan allocator computes live ranges,
    // sorts them by start position, and assigns physical registers.
    (void)g;
}

}  // namespace rjit
