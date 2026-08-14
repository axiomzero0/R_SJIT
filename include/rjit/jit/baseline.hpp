// rjit/jit/baseline.hpp - Tier-1 baseline JIT
//
// The baseline JIT compiles each bytecode instruction into machine
// code *without optimization*. The goal is to remove the dispatch
// overhead of the interpreter while keeping compilation very fast
// (~hundreds of µs per function).
//
// Strategy:
//   * One-to-one mapping from bytecode instructions to machine code
//     snippets. Each snippet loads operands from the register file,
//     calls into a shared C++ runtime function that does the actual
//     operation, and stores the result back.
//   * The register file pointer is kept in a callee-saved register
//     (rbx on x86-64) for the duration of the function.
//   * The current Environment* is kept in r14.
//   * The GC pointer is kept in r15.
//   * Safepoints are emitted at every LOOP_HEADER and at every
//     potential deopt site.
//
// This produces correct but slow machine code. Tier 1.5 and Tier 2
// will improve on it.

#pragma once
#include "rjit/bytecode/module.hpp"
#include "rjit/jit/jit_code.hpp"
#include "rjit/core/context.hpp"

namespace rjit {

class Context;
class BytecodeFunction;

class BaselineJIT {
public:
    explicit BaselineJIT(Context& ctx);

    // Compile a function. Returns a JitCode object that can be
    // called via its `entry` function pointer.
    JitCode* compile(BytecodeFunction* fn);

private:
    Context& ctx_;
};

}  // namespace rjit
