// rjit/jit/osr.cpp
//
// On-Stack-Replacement: transition from interpreter to JIT code
// in the middle of a function (at a loop header).
//
// When the interpreter hits a LOOP_HEADER and the loop iteration
// count exceeds the OSR threshold, OSR transfers control to JIT-
// compiled code starting at that loop header. This avoids waiting
// for the function to return before tiering up.
//
// Mechanism:
//   1. The interpreter hits LOOP_HEADER and checks the iteration count.
//   2. If count > OSR threshold, it calls OSR::osr_into().
//   3. osr_into() compiles the function (or fetches cached JitCode).
//   4. It creates a Frame from the current interpreter state.
//   5. It calls the JIT entry point, which resumes at the loop header.
//   6. When the JIT code returns, the result is propagated back to
//      the interpreter.

#include "rjit/jit/osr.hpp"
#include "rjit/jit/baseline.hpp"
#include "rjit/core/context.hpp"
#include "rjit/jit/tier_manager.hpp"

namespace rjit {

OSR::OSR(Context& ctx) : ctx_(ctx) {}

Value OSR::osr_into(Frame& frame, uint32_t loop_header_pc) {
    // Compile the function (or fetch cached JIT code).
    BytecodeFunction* fn = frame.fn;
    JitCode* jc = ctx_.tiers().jit_code(fn);
    if (!jc) {
        BaselineJIT base(ctx_);
        jc = base.compile(fn);
        if (!jc) return Value::nil();
        ctx_.tiers().set_jit_code(fn, jc);
        ctx_.tiers().set_current_tier(fn, Tier::kBaseline);
    }

    // Set the frame PC to the loop header so the JIT resumes there.
    frame.pc = loop_header_pc;

    // Call the JIT entry point. The JIT code will execute from the
    // loop header until the function returns.
    if (jc->entry) {
        return jc->entry(&frame);
    }
    return Value::nil();
}

}  // namespace rjit
