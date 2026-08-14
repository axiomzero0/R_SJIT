// rjit/vm/quickening.hpp - Bytecode quickening (runtime opcode rewriting)
#pragma once
#include "rjit/bytecode/opcodes.hpp"
#include "rjit/core/value.hpp"

namespace rjit {

class BytecodeFunction;
class Instr;
class FeedbackEngine;

// Record an observation of operand types and quicken if appropriate.
// Called from the dispatch loop's generic ADD/SUB/etc. handlers.
// Returns the (possibly rewritten) opcode.
Op quicken_observe(Instr& in, TypeTag ta, TypeTag tb);

// Batch quickening: scan all instructions and rewrite based on flags.
uint32_t quicken(BytecodeFunction& fn, FeedbackEngine const& fb);

// Per-instruction rewrite (legacy, kept for ABI).
bool quicken_instr(Instr& in, FeedbackEngine const& fb, uint32_t instr_idx);

}  // namespace rjit
