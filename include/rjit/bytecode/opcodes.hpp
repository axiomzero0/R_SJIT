// rjit/bytecode/opcodes.hpp - Register-based bytecode opcodes
//
// Design:
//   * Bytecode is register-based: each instruction reads/writes up to
//     three registers (RDEST, RA, RB). Some instructions have an
//     immediate operand (K) for symbol ids, constant indices, jump
//     targets, or argument counts.
//   * The register file is per-frame; each function call gets a fresh
//     register file sized to the function's maximum register usage.
//   * Operands use uint32_t to allow large functions (no 256-reg
//     ceiling) but the JIT will prefer to use uint8_t operands
//     internally for the fast path.
//   * Each opcode has a `specialized` variant (e.g., ADD_REAL_REAL)
//     that the quickening pass emits after observing operand types.
//     The generic opcode (e.g., ADD) is the unspecialized form.
//
// The bytecode format is:
//   [opcode: 1 byte][flags: 1 byte][rdest: 4 bytes][ra: 4 bytes][rb: 4 bytes][k: 4 bytes]
// for a total of 18 bytes per instruction. We pad to 24 bytes for
// alignment. (Yes, this is large — but the trade-off is that the
// interpreter can dispatch with a single indexed load and the JIT
// can read all operands with a small number of constant offsets.)
//
// Actually for the in-memory representation we use a struct; for
// the on-disk format we'd compress, but in-memory we prioritize
// interpreter speed over density.

#pragma once
#include <cstdint>
#include <cstddef>
#include "rjit/core/value.hpp"

namespace rjit {

enum class Op : uint16_t {
    // --- control ---
    NOP,            // no-op
    HALT,           // stop the interpreter
    JUMP,           // unconditional jump to K (PC offset)
    JUMP_IF_FALSE,  // jump if RA is FALSE or NA
    JUMP_IF_TRUE,   // jump if RA is TRUE
    JUMP_IF_NA,     // jump if RA is NA

    // --- constants and literals ---
    LOAD_NIL,       // RDEST = NULL
    LOAD_TRUE,      // RDEST = TRUE
    LOAD_FALSE,     // RDEST = FALSE
    LOAD_NA,        // RDEST = NA
    LOAD_REAL,      // RDEST = constant[K] as real
    LOAD_INT,       // RDEST = constant[K] as int
    LOAD_STRING,    // RDEST = constant[K] as string

    // --- variables ---
    LOAD_VAR,       // RDEST = lookup(symbol[K])
    STORE_VAR,      // symbol[K] = RA
    STORE_SUPER,    // <<- : set existing up the chain
    LOAD_LOCAL,     // RDEST = local[K]  (frame-local binding, no lookup)
    STORE_LOCAL,    // local[K] = RA
    RM_VAR,         // remove(symbol[K])

    // --- arithmetic (generic + specialized) ---
    ADD,            // RDEST = RA + RB
    SUB,
    MUL,
    DIV,
    POW,
    MOD,
    NEG,            // RDEST = -RA

    // Specialized arithmetic (produced by quickening / type feedback)
    ADD_REAL_REAL,
    ADD_INT_INT,
    ADD_REAL_INT,
    ADD_INT_REAL,
    ADD_SCALAR_VEC,    // RA is scalar, RB is vector
    ADD_VEC_SCALAR,
    ADD_VEC_VEC,

    SUB_REAL_REAL,
    SUB_INT_INT,
    MUL_REAL_REAL,
    MUL_INT_INT,
    DIV_REAL_REAL,
    DIV_INT_INT,

    // --- comparisons ---
    LT, LE, GT, GE, EQ, NE,
    LT_REAL_REAL, LE_REAL_REAL, GT_REAL_REAL, GE_REAL_REAL, EQ_REAL_REAL, NE_REAL_REAL,
    LT_INT_INT,   LE_INT_INT,   GT_INT_INT,   GE_INT_INT,   EQ_INT_INT,   NE_INT_INT,

    // --- logical ---
    AND,            // && (short-circuit)
    OR,             // ||
    NOT,            // !
    AND_SCALAR,
    OR_SCALAR,

    // --- function call / return ---
    CALL,           // RDEST = call symbol/function RA with K args from RA+1..
    CALL_NAMED,     // CALL with named args (constant table holds arg names)
    CALL_BUILTIN,   // direct call to a known builtin
    CALL_CLOSURE,   // direct call to a known closure
    RETURN,         // return RA
    RETURN_NULL,    // return NULL

    // --- promises ---
    MAKE_PROMISE,   // RDEST = promise(expr=K, env=current)
    FORCE_PROMISE,  // RDEST = force(RA)

    // --- vectors ---
    MAKE_VECTOR,    // RDEST = vector from K consecutive registers
    MAKE_SEQ,       // RDEST = RA:RB (range vector)
    LENGTH,         // RDEST = length(RA)
    INDEX,          // RDEST = RA[RB]   (single index)
    INDEX2,         // RDEST = RA[[RB]] (single index, list-like)
    INDEX_NAMED,    // RDEST = RA$K
    INDEX_ASSIGN,   // RA[RB] = RC
    INDEX2_ASSIGN,
    INDEX_NAMED_ASSIGN,
    SUBSET,         // RDEST = RA[RB]   (vector subsetting)
    COERCE_REAL,    // RDEST = as.real(RA)
    COERCE_INT,     // RDEST = as.integer(RA)
    COERCE_LOGICAL, // RDEST = as.logical(RA)

    // --- typeof / class ---
    TYPEOF,         // RDEST = typeof(RA) as string
    IS_NA,          // RDEST = is.na(RA)

    // --- environment ---
    NEW_ENV,        // RDEST = new environment with parent=RA
    GET_ENV,        // RDEST = current environment

    // --- function literal ---
    MAKE_CLOSURE,   // RDEST = closure(code=bytecode[K], env=current)

    // --- branch on type (for deoptless guards) ---
    GUARD_TYPE,      // deopt if RA.tag != K
    GUARD_SHAPE,     // deopt if env.shape != K
    GUARD_LEN,       // deopt if length(RA) != K
    DEOPT,           // unconditional deopt (re-enter interpreter)

    // --- inline cache helpers ---
    IC_LOAD_VAR,     // RDEST = lookup via IC at slot K
    IC_CALL,         // call via IC at slot K

    // --- loop bookkeeping ---
    LOOP_HEADER,     // marks a loop header (for OSR + tier-up)
    LOOP_BACKEDGE,   // marks a backedge

    // --- debug ---
    PRINT,           // print RA
    DUMP,            // dump register state

    kCount
};

const char* op_name(Op op) noexcept;

// Instruction layout: 24 bytes for alignment.
struct alignas(8) Instr {
    Op       op;
    uint16_t flags;   // opcode-specific
    uint32_t rdest;
    uint32_t ra;
    uint32_t rb;
    uint32_t k;       // immediate (symbol id, constant idx, jump target, count, ...)

    Instr() : op(Op::NOP), flags(0), rdest(0), ra(0), rb(0), k(0) {}
};

static_assert(sizeof(Instr) == 24, "Instr must be 24 bytes");
static_assert(alignof(Instr) == 8, "Instr must be 8-byte aligned");

}  // namespace rjit
