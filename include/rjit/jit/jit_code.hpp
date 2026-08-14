// rjit/jit/jit_code.hpp - Container for compiled machine code
//
// JitCode is a heap object wrapping an executable memory region
// produced by the JIT. It holds:
//   * the entry point pointer
//   * the size of the code
//   * metadata for deopt (safepoint offsets, etc.)
//   * a pointer to the BytecodeFunction it was compiled from (for
//     recompilation and deopt)

#pragma once
#include <cstdint>
#include <vector>
#include "rjit/core/value.hpp"
#include "rjit/core/gc.hpp"

namespace rjit {

class BytecodeFunction;

class JitCode : public HeapObject {
public:
    RJIT_DECLARE_GC_NEW(JitCode)

    JitCode();
    ~JitCode();

    // The entry point. Calling this with a Frame* returns the
    // function's return value.
    using EntryFn = Value (*)(void* frame);
    EntryFn entry = nullptr;

    // Owned executable memory.
    void*  code_mem   = nullptr;
    size_t code_size  = 0;

    // Bytecode this was compiled from.
    BytecodeFunction* source = nullptr;

    // Safepoint offsets (one per LOOP_HEADER / potential deopt site).
    // Maps bytecode PC -> machine code offset.
    std::vector<std::pair<uint32_t, uint32_t>> safepoints;

    void trace(Visitor& v) const override;
};

}  // namespace rjit
