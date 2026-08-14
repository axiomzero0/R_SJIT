// rjit/jit/osr.hpp - On-Stack-Replacement
//
// OSR transitions execution from the interpreter (or a lower tier)
// into JIT-compiled code in the middle of a function. This is
// critical for hot loops: we don't want to wait for the function
// to return before tiering up.
//
// Mechanism:
//   1. The interpreter hits a LOOP_HEADER and notices the iteration
//      count has crossed the OSR threshold.
//   2. It calls OSR::osr_into(frame, loop_header_pc), passing the
//      current Frame.
//   3. The OSR code finds (or compiles) a JitCode for the function
//      starting at the loop header, then tail-calls its entry point
//      with a Frame constructed from the current state.
#pragma once
#include "rjit/vm/frame.hpp"
#include "rjit/jit/jit_code.hpp"

namespace rjit {

class Context;
class BytecodeFunction;
class Frame;

class OSR {
public:
    explicit OSR(Context& ctx);

    // Attempt OSR from the current frame at the given loop header.
    // Returns the value returned by the JIT code, or nullopt if
    // OSR was not possible.
    Value osr_into(Frame& frame, uint32_t loop_header_pc);

private:
    Context& ctx_;
};

}  // namespace rjit
