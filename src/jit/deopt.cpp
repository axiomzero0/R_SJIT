// rjit/jit/deopt.cpp
#include "rjit/jit/deopt.hpp"
#include "rjit/bytecode/module.hpp"
#include "rjit/vm/frame.hpp"
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

    // Allocate register file.
    Value* regs = new Value[b.caller_fn->nregs];
    for (uint32_t i = 0; i < b.caller_fn->nregs; ++i) regs[i] = Value::nil();

    // Fill in registers from the deopt blob.
    for (uint32_t i = 0; i < b.num_values; ++i) {
        DeoptValue const& dv = value_pool_[b.values_offset + i];
        Value v;
        switch (dv.kind) {
            case DeoptValueKind::kConstant:  v = dv.constant_value; break;
            case DeoptValueKind::kRegister:  v = (i < n_inputs) ? input_values[i] : Value::nil(); break;
            case DeoptValueKind::kStackSlot: v = (i < n_inputs) ? input_values[i] : Value::nil(); break;
            case DeoptValueKind::kEnvSlot:   v = b.env->slot_get(dv.env_slot); break;
            case DeoptValueKind::kPending:   v = Value::nil(); break;
        }
        // The "register" the value goes to is encoded in env_slot for
        // lack of a dedicated field. (Real impl: add a `target_reg`
        // field to DeoptValue.)
        uint32_t target = dv.env_slot;  // reuse for target register index
        if (target < b.caller_fn->nregs) regs[target] = v;
    }
    frame->regs = regs;
    return frame;
}

}  // namespace rjit
