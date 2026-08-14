// rjit/jit/deopt.cpp
//
// Deoptimization framework: reconstruct interpreter state from JIT code.
//
// When JIT-compiled code hits a failed guard (e.g., a type check
// fails), it calls rjit_helper_deopt with a safepoint ID. The DeoptContext
// looks up the safepoint's blob, which describes how to materialize the
// interpreter frame, and returns control to the interpreter at the
// appropriate bytecode PC.
//
// The materialization process:
//   1. Look up the DeoptBlob for the safepoint ID
//   2. Allocate a new Frame + register file
//   3. For each DeoptValue in the blob, reconstruct the Value from
//      its source (constant, JIT register, stack slot, or env slot)
//   4. Store the reconstructed values into the frame's registers
//   5. Set the frame's PC to resume after the deopting instruction
//   6. Transfer control to the interpreter's dispatch loop

#include "rjit/jit/deopt.hpp"
#include "rjit/bytecode/module.hpp"
#include "rjit/vm/frame.hpp"
#include "rjit/core/context.hpp"
#include "rjit/vm/interpreter.hpp"
#include <new>

namespace rjit {

DeoptContext::DeoptContext() = default;

uint32_t DeoptContext::alloc_blob(BytecodeFunction* caller_fn, uint32_t caller_pc,
                                  uint32_t caller_dst, Environment* env) {
    DeoptBlob b;
    b.safepoint_id  = static_cast<uint32_t>(blobs_.size());
    b.caller_fn     = caller_fn;
    b.caller_pc     = caller_pc;
    b.caller_dst    = caller_dst;
    b.env           = env;
    b.num_values    = 0;
    b.values_offset = static_cast<uint32_t>(value_pool_.size());
    blobs_.push_back(b);
    return b.safepoint_id;
}

void DeoptContext::add_value(uint32_t safepoint_id, DeoptValue v) {
    value_pool_.push_back(v);
    if (safepoint_id < blobs_.size()) {
        blobs_[safepoint_id].num_values++;
    }
}

Frame* DeoptContext::materialize(uint32_t safepoint_id, Value* input_values, uint32_t n_inputs) {
    if (safepoint_id >= blobs_.size()) return nullptr;
    DeoptBlob& b = blobs_[safepoint_id];

    // Allocate a new frame for the caller.
    Frame* frame = new Frame();
    frame->fn = b.caller_fn;
    frame->env = b.env;
    frame->pc = b.caller_pc + 1;  // resume after the deopting instruction
    frame->caller = nullptr;
    frame->caller_dst = 0;
    frame->from_jit = false;

    // Allocate register file.
    uint32_t nregs = b.caller_fn->nregs;
    Value* regs = new Value[nregs];
    for (uint32_t i = 0; i < nregs; ++i) regs[i] = Value::nil();

    // Fill in registers from the deopt blob.
    uint32_t input_idx = 0;
    for (uint32_t i = 0; i < b.num_values; ++i) {
        DeoptValue const& dv = value_pool_[b.values_offset + i];
        Value v;
        switch (dv.kind) {
            case DeoptValueKind::kConstant:
                v = dv.constant_value;
                break;
            case DeoptValueKind::kRegister:
            case DeoptValueKind::kStackSlot:
                v = (input_idx < n_inputs) ? input_values[input_idx++] : Value::nil();
                break;
            case DeoptValueKind::kEnvSlot:
                v = b.env->slot_get(dv.env_slot);
                break;
            case DeoptValueKind::kPending:
                v = Value::nil();  // value not yet computed; will be re-evaluated
                break;
        }
        // The target register index is stored in env_slot (reused field).
        uint32_t target = dv.env_slot;
        if (target < nregs) regs[target] = v;
    }
    frame->regs = regs;
    frame->nregs = nregs;
    return frame;
}

// Entry point called by JIT code when a guard fails.
// The safepoint_id identifies which deopt blob to use.
// input_values are the Values that were in JIT registers at the
// deopt point (passed by the JIT code).
extern "C" void rjit_helper_deopt(uint32_t safepoint_id, Value* input_values, uint32_t n_inputs) {
    DeoptContext& dc = current_context().deopt();
    Frame* frame = dc.materialize(safepoint_id, input_values, n_inputs);
    if (!frame) {
        current_context().raise_error("deopt: invalid safepoint id");
    }
    // Transfer control to the interpreter.
    // In a full implementation, this would longjmp to the interpreter
    // loop with the materialized frame. For now, we execute the frame
    // directly.
    Value result = current_context().interpreter().dispatch_loop(*frame);
    (void)result;
    // The interpreter will continue from the deopt point.
    // When it returns, we need to restore the JIT call stack.
    // This is a simplification — a full implementation would use
    // setjmp/longjmp or a C++ exception to unwind.
    current_context().raise_error("deopt: materialized frame execution not yet supported");
}

}  // namespace rjit
