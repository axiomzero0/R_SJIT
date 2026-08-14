// rjit/jit/specialized.hpp - Tier 1.5: type-specialized JIT
//
// Takes the baseline JIT output and rewrites it using type feedback
// to emit specialized operations (ADD -> ADD_REAL_REAL, etc.) and
// inline cache fast paths.
#pragma once
#include "rjit/jit/baseline.hpp"
#include "rjit/jit/jit_code.hpp"

namespace rjit {

class Context;
class BytecodeFunction;

class SpecializedJIT {
public:
    explicit SpecializedJIT(Context& ctx);
    JitCode* compile(BytecodeFunction* fn);
private:
    Context& ctx_;
};

}  // namespace rjit
