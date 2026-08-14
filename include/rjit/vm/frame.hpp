// rjit/vm/frame.hpp - Call frame (activation record)
//
// A Frame holds:
//   * the BytecodeFunction being executed
//   * the Environment for this call (created from the closure's env)
//   * the register file (sized to fn->nregs)
//   * the program counter
//   * the previous frame (caller)
//
// We deliberately do NOT use a C++ call stack for R-level recursion
// (we use an explicit Frame* chain). This makes OSR and deopt much
// simpler — we can materialize a Frame from JIT-compiled code by
// just allocating one and filling it in.

#pragma once
#include <cstdint>
#include <vector>
#include <deque>
#include "rjit/core/value.hpp"
#include "rjit/core/environment.hpp"

namespace rjit {

class BytecodeFunction;
class Instr;

class Frame {
public:
    BytecodeFunction* fn           = nullptr;
    Environment*      env          = nullptr;
    Value*            regs         = nullptr;
    uint32_t          pc           = 0;  // saved PC when this frame is suspended
    Frame*            caller       = nullptr;
    uint32_t          caller_dst   = 0;  // register in caller that receives return value
    uint32_t          nregs        = 0;  // size of regs array (for freeing)
    bool              from_jit     = false;

    Frame() = default;

    void init(BytecodeFunction* f, Environment* e, Frame* caller) {
        fn = f;
        env = e;
        pc = 0;
        this->caller = caller;
        from_jit = false;
    }

    Instr const& instr() const;
};

// Stack-allocated register file pool. Allocates register files in
// large chunks to avoid per-call malloc.
//
// IMPORTANT: Frame objects are stored in a std::deque (not a vector)
// so that existing Frame* pointers remain valid when new frames are
// allocated. This is critical for deep recursion: each recursive
// call allocates a frame, and the caller's Frame* must remain valid.
class FramePool {
public:
    FramePool() = default;

    // Allocate a register file of `n` Values. The pointer is valid
    // until reset() is called.
    Value* alloc_regs(uint32_t n);

    // Allocate a Frame. The pointer is valid until reset() is called.
    Frame* alloc_frame();

    // Reset (called at safe points — not while frames are live).
    void reset() { regs_chunks_.clear(); frames_.clear(); frames_used_ = 0; }

private:
    std::deque<std::vector<Value>>   regs_chunks_;
    std::deque<Frame>                frames_;
    uint32_t                         frames_used_ = 0;
};

}  // namespace rjit
