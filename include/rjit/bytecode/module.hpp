// rjit/bytecode/module.hpp - Bytecode module: a compiled R function
//
// A BytecodeModule contains:
//   * the instruction array
//   * the constant pool (scalars + symbol ids + nested function bytecode)
//   * the function-level metadata (max registers, param count, debug info)
//
// One module corresponds to one function (closures may share the
// same module). The module is immutable after construction.

#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include "rjit/bytecode/opcodes.hpp"
#include "rjit/core/value.hpp"
#include "rjit/core/gc.hpp"

namespace rjit { class Environment; }

namespace rjit {

class Environment;
class Closure;

// Source location for an instruction (for error messages + deopt)
struct SourceLoc {
    uint32_t line   = 0;
    uint32_t column = 0;
};

// Debug info per instruction. Stored separately to keep Instr small.
struct DebugInfo {
    std::vector<SourceLoc>   instr_locs;     // per-instruction source location
    std::vector<std::string> register_names; // optional: name of each register (for pretty-printing)
};

class BytecodeFunction : public HeapObject {
public:
    RJIT_DECLARE_GC_NEW(BytecodeFunction)

    BytecodeFunction();

    // Instruction array
    std::vector<Instr> code;

    // Constant pool. Each entry is a Value (so we can store scalars,
    // strings, nested Closures, etc.).
    std::vector<Value> constants;

    // Symbols used by this function (so we can GC-trace them if needed)
    std::vector<uint32_t> symbols_used;

    // Metadata
    uint32_t nparams     = 0;
    uint32_t nregs       = 0;        // size of register file
    bool     is_variadic = false;    // supports ... (dots)
    uint32_t dots_pos    = UINT32_MAX;  // slot index of ..., if any

    // Parameter names (for binding arguments at call time).
    std::vector<std::string> param_names;

    // Inline cache storage: one entry per instruction index.
    // Allocated lazily on first LOAD_VAR/STORE_VAR.
    // This avoids unordered_map overhead in the hot path.
    // Each entry is: {env_pointer, shape_id, slot}
    // Using a flat array indexed by PC gives O(1) lookup with no hashing.
    mutable std::vector<Environment*> ic_envs;
    mutable std::vector<uint32_t>     ic_shapes;
    mutable std::vector<uint32_t>     ic_slots;
    // Bit i = 1 means instruction i has a valid IC entry.
    mutable std::vector<uint64_t>     ic_valid;  // bitmap

    // Source-level name (for backtraces). May be empty.
    std::string name;

    // Debug info (optional, may be empty in release)
    std::unique_ptr<DebugInfo> debug;

    void trace(Visitor& v) const override;
};

// Helper: build a BytecodeFunction from an AST.
class BytecodeBuilder {
public:
    BytecodeBuilder();

    // Allocate a new register, returns its index.
    uint32_t alloc_reg();

    // Emit an instruction (with default operands); returns the index.
    uint32_t emit(Op op, uint32_t rdest = 0, uint32_t ra = 0, uint32_t rb = 0, uint32_t k = 0, uint16_t flags = 0);

    // Patch an existing instruction's k field (for jump targets).
    void patch_k(uint32_t instr_idx, uint32_t new_k);

    // Current instruction count (used to compute jump targets).
    uint32_t current_count() const noexcept {
        return static_cast<uint32_t>(fn_->code.size());
    }

    // Returns the count *after* the next emitted instruction (i.e.,
    // the jump target for an instruction that will be emitted next).
    // Equivalent to current_count() at the time of the next emit().
    uint32_t finalize_placeholder_count() const noexcept {
        return current_count();
    }

    // Add a constant to the pool, returns its index.
    uint32_t add_constant(Value v);

    // Add a symbol id to the symbols_used list (idempotent).
    void use_symbol(uint32_t sym_id);

    // Finalize and return the function.
    BytecodeFunction* finalize(std::string name, uint32_t nparams, uint32_t nregs);

    // Access the underlying function (for the Lowerer to add param names).
    BytecodeFunction* fn() noexcept { return fn_; }

private:
    BytecodeFunction* fn_;
};

}  // namespace rjit
