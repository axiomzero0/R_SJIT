// rjit/jit/deopt.hpp - Deoptimization framework
//
// Deoptimization reconstructs interpreter state from JIT-compiled
// code when a speculative assumption fails. The mechanism:
//
//   1. JIT code emits a "safepoint" at every point where state
//      could be reconstructed. Each safepoint has a "deopt blob"
//      describing how to materialize the interpreter state.
//   2. On guard failure, JIT code jumps to a per-safepoint "deopt
//      handler" that reads the safepoint's blob, reconstructs the
//      Frame, and tail-calls the interpreter.
//
// The DeoptContext maintains a table of deopt blobs, indexed by
// safepoint id. The JIT requests a blob id at compile time; the
// deopt handler reads it at runtime.

#pragma once
#include <cstdint>
#include <vector>
#include "rjit/core/value.hpp"

namespace rjit {

class BytecodeFunction;
class Frame;
class Environment;

// What kind of value lives in a given JIT "register" at a safepoint?
enum class DeoptValueKind : uint8_t {
    kConstant,         // value is the literal `constant_value`
    kRegister,         // value is in physical register `phys_reg`
    kStackSlot,        // value is in stack slot `stack_offset`
    kEnvSlot,          // value is in environment slot `env_slot`
    kPending,          // value has not been computed yet (must be re-evaluated)
};

struct DeoptValue {
    DeoptValueKind kind;
    uint8_t        phys_reg;
    int32_t        stack_offset;
    uint32_t       env_slot;
    Value          constant_value;
};

struct DeoptBlob {
    uint32_t          safepoint_id;
    BytecodeFunction* caller_fn;
    uint32_t          caller_pc;          // PC at the deopt point
    uint32_t          caller_dst;         // result register in caller
    Environment*      env;
    uint32_t          num_values;
    uint32_t          values_offset;      // offset into DeoptContext's value pool
};

class DeoptContext {
public:
    DeoptContext();

    // Allocate a new deopt blob (called by the JIT at compile time).
    // Returns the safepoint id (a small integer used as a key).
    uint32_t alloc_blob(BytecodeFunction* caller_fn, uint32_t caller_pc,
                        uint32_t caller_dst, Environment* env);

    // Append a value to the most recent blob.
    void add_value(uint32_t safepoint_id, DeoptValue v);

    // Reconstruct an interpreter Frame from a deopt blob. Returns
    // the new frame (caller must install it as the current frame).
    // The `input_values` array provides the values for kRegister
    // and kStackSlot entries (the JIT code passes these in).
    Frame* materialize(uint32_t safepoint_id, Value* input_values, uint32_t n_inputs);

private:
    std::vector<DeoptBlob>  blobs_;
    std::vector<DeoptValue> value_pool_;
};

}  // namespace rjit
