// rjit/jit/osr.cpp
#include "rjit/jit/osr.hpp"
#include "rjit/jit/baseline.hpp"
#include "rjit/core/context.hpp"

namespace rjit {

OSR::OSR(Context& ctx) : ctx_(ctx) {}

Value OSR::osr_into(Frame& frame, uint32_t loop_header_pc) {
    // Compile the function (or fetch cached JitCode)
    BaselineJIT base(ctx_);
    JitCode* jc = base.compile(frame.fn);
    if (!jc) return Value::nil();
    // Set the frame PC to the loop header so the JIT resumes there.
    frame.pc = loop_header_pc;
    // Call the JIT entry. Note: the JIT signature is Value(Frame*),
    // so we just pass the frame pointer.
    return jc->entry(&frame);
}

}  // namespace rjit
