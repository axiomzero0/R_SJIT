// rjit/vm/quickening.hpp - Bytecode quickening
//
// Quickening is the process of rewriting generic opcodes into
// specialized ones based on observed operand types. For example:
//
//   ADD  r1, r2, r3   --(observe r1=real, r2=real)-->   ADD_REAL_REAL r1, r2, r3
//
// The rewrite happens in-place: the opcode field is changed, but
// the operand fields stay the same. This means the interpreter
// dispatch loop sees the new opcode without any indirection.
//
// We use type feedback from the FeedbackEngine to drive quickening.
// A second pass (in the JIT) can use the same feedback to emit
// machine code directly.

#pragma once
#include "rjit/bytecode/module.hpp"
#include "rjit/feedback/feedback.hpp"

namespace rjit {

class BytecodeFunction;

// Run quickening on the given function, using type feedback to
// guide the rewrites. Returns the number of rewrites performed.
uint32_t quicken(BytecodeFunction& fn, FeedbackEngine const& fb);

// Per-instruction rewrite. Returns true if the instruction was
// rewritten.
bool quicken_instr(Instr& in, FeedbackEngine const& fb, uint32_t instr_idx);

}  // namespace rjit
