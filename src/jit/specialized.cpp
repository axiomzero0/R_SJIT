// rjit/jit/specialized.cpp
#include "rjit/jit/specialized.hpp"
#include "rjit/jit/baseline.hpp"
#include "rjit/feedback/feedback.hpp"

namespace rjit {

SpecializedJIT::SpecializedJIT(Context& ctx) : ctx_(ctx) {}

JitCode* SpecializedJIT::compile(BytecodeFunction* fn) {
    // For the initial version: just call the baseline JIT. The
    // type-specialized compilation passes will be added in a
    // later iteration.
    BaselineJIT base(ctx_);
    return base.compile(fn);
}

}  // namespace rjit
