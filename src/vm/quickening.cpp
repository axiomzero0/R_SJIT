// rjit/vm/quickening.cpp
#include "rjit/vm/quickening.hpp"
#include "rjit/bytecode/module.hpp"
#include "rjit/feedback/feedback.hpp"

namespace rjit {

uint32_t quicken(BytecodeFunction& fn, FeedbackEngine const& fb) {
    uint32_t rewrites = 0;
    for (uint32_t i = 0; i < fn.code.size(); ++i) {
        if (quicken_instr(fn.code[i], fb, i)) ++rewrites;
    }
    return rewrites;
}

bool quicken_instr(Instr& in, FeedbackEngine const& fb, uint32_t instr_idx) {
    switch (in.op) {
        case Op::ADD: {
            // Inspect the type feedback for RA and RB at this instruction.
            // (We use the same slot for both operands, since the feedback
            // engine records the *result* type — for input types we'd
            // need separate slots. For this initial version we use the
            // result type as a proxy.)
            TypeFeedback const& tf = const_cast<FeedbackEngine&>(fb).type(nullptr, instr_idx);
            (void)tf;
            // Without per-operand feedback we can't safely specialize.
            return false;
        }
        default:
            return false;
    }
}

}  // namespace rjit
