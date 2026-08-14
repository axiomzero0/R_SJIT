// rjit/vm/quickening.cpp
//
// Quickening: runtime opcode rewriting based on observed operand types.
//
// After executing a generic ADD/SUB/MUL/DIV/comparison a few times,
// we rewrite the opcode field in-place to a specialized variant
// (ADD_REAL_REAL, etc.). The specialized handler skips the type check
// branch entirely, falling straight to the arithmetic.
//
// This is the same technique CPython 3.12 uses (adaptive specialization),
// and it's the single biggest interpreter win after computed goto.
//
// We use a per-instruction counter (stored in the instruction's `flags`
// field) to avoid rewriting too early. After kQuickenThreshold hits
// with consistent types, we rewrite.

#include "rjit/vm/quickening.hpp"
#include "rjit/bytecode/module.hpp"
#include "rjit/feedback/feedback.hpp"

namespace rjit {

// How many times an instruction must execute with consistent types
// before we quicken it. Too low = premature specialization; too high
// = missed optimization window. 8 is a good default (matches CPython).
static constexpr uint16_t kQuickenThreshold = 8;

// Flags layout:
//   bits 0-3:   execution counter (0..15)
//   bits 4-7:   last-seen type tag of RA
//   bits 8-11:  last-seen type tag of RB
//   bit 12:     types are consistent so far
//   bit 13:     already quickened
static inline uint16_t get_counter(uint16_t flags) { return flags & 0xF; }
static inline uint8_t get_ra_type(uint16_t flags) { return (flags >> 4) & 0xF; }
static inline uint8_t get_rb_type(uint16_t flags) { return (flags >> 8) & 0xF; }
static inline bool is_consistent(uint16_t flags) { return (flags >> 12) & 1; }
static inline bool is_quickened(uint16_t flags) { return (flags >> 13) & 1; }

static inline uint16_t set_counter(uint16_t f, uint16_t c) { return (f & ~0xF) | (c & 0xF); }
static inline uint16_t set_ra_type(uint16_t f, uint8_t t) { return (f & ~0xF0) | ((t & 0xF) << 4); }
static inline uint16_t set_rb_type(uint16_t f, uint8_t t) { return (f & ~0xF00) | ((t & 0xF) << 8); }
static inline uint16_t set_consistent(uint16_t f, bool c) { return (f & ~0x1000) | (c ? 0x1000 : 0); }
static inline uint16_t set_quickened(uint16_t f) { return f | 0x2000; }

// Record an observation and quicken if appropriate.
// Called from the dispatch loop's generic ADD/SUB/etc. handlers.
// Returns the (possibly rewritten) opcode.
Op quicken_observe(Instr& in, TypeTag ta, TypeTag tb) {
    uint16_t flags = in.flags;
    if (is_quickened(flags)) return in.op;

    uint16_t counter = get_counter(flags);
    uint8_t prev_a = get_ra_type(flags);
    uint8_t prev_b = get_rb_type(flags);
    uint8_t cur_a = static_cast<uint8_t>(ta) & 0xF;
    uint8_t cur_b = static_cast<uint8_t>(tb) & 0xF;

    if (counter == 0) {
        // First observation.
        flags = set_ra_type(flags, cur_a);
        flags = set_rb_type(flags, cur_b);
        flags = set_consistent(flags, true);
    } else {
        // Check consistency.
        if (!is_consistent(flags) || prev_a != cur_a || prev_b != cur_b) {
            flags = set_consistent(flags, false);
        }
    }

    counter = std::min(static_cast<uint16_t>(counter + 1), static_cast<uint16_t>(15));
    flags = set_counter(flags, counter);

    // If we've seen consistent types enough times, rewrite.
    if (counter >= kQuickenThreshold && is_consistent(flags)) {
        TypeTag a_tag = static_cast<TypeTag>(get_ra_type(flags));
        TypeTag b_tag = static_cast<TypeTag>(get_rb_type(flags));
        Op new_op = in.op;
        switch (in.op) {
            case Op::ADD:
                if (a_tag == TypeTag::kReal && b_tag == TypeTag::kReal) new_op = Op::ADD_REAL_REAL;
                else if (a_tag == TypeTag::kInteger && b_tag == TypeTag::kInteger) new_op = Op::ADD_INT_INT;
                break;
            case Op::SUB:
                if (a_tag == TypeTag::kReal && b_tag == TypeTag::kReal) new_op = Op::SUB_REAL_REAL;
                else if (a_tag == TypeTag::kInteger && b_tag == TypeTag::kInteger) new_op = Op::SUB_INT_INT;
                break;
            case Op::MUL:
                if (a_tag == TypeTag::kReal && b_tag == TypeTag::kReal) new_op = Op::MUL_REAL_REAL;
                else if (a_tag == TypeTag::kInteger && b_tag == TypeTag::kInteger) new_op = Op::MUL_INT_INT;
                break;
            case Op::DIV:
                if (a_tag == TypeTag::kReal && b_tag == TypeTag::kReal) new_op = Op::DIV_REAL_REAL;
                break;
            case Op::LT:
                if (a_tag == TypeTag::kReal && b_tag == TypeTag::kReal) new_op = Op::LT_REAL_REAL;
                break;
            case Op::LE:
                if (a_tag == TypeTag::kReal && b_tag == TypeTag::kReal) new_op = Op::LE_REAL_REAL;
                break;
            case Op::GT:
                if (a_tag == TypeTag::kReal && b_tag == TypeTag::kReal) new_op = Op::GT_REAL_REAL;
                break;
            case Op::GE:
                if (a_tag == TypeTag::kReal && b_tag == TypeTag::kReal) new_op = Op::GE_REAL_REAL;
                break;
            case Op::EQ:
                if (a_tag == TypeTag::kReal && b_tag == TypeTag::kReal) new_op = Op::EQ_REAL_REAL;
                break;
            case Op::NE:
                if (a_tag == TypeTag::kReal && b_tag == TypeTag::kReal) new_op = Op::NE_REAL_REAL;
                break;
            default: break;
        }
        if (new_op != in.op) {
            in.op = new_op;
            flags = set_quickened(flags);
        }
    }

    in.flags = flags;
    return in.op;
}

// Batch quickening: scan all instructions and rewrite based on feedback.
// Called by the tier manager before promoting to T1.
uint32_t quicken(BytecodeFunction& fn, FeedbackEngine const& fb) {
    uint32_t rewrites = 0;
    for (uint32_t i = 0; i < fn.code.size(); ++i) {
        // For batch quickening, we check if the instruction's flags
        // indicate consistent types. If so, rewrite.
        Instr& in = fn.code[i];
        uint16_t flags = in.flags;
        if (is_quickened(flags) || !is_consistent(flags)) continue;
        if (get_counter(flags) < kQuickenThreshold) continue;

        TypeTag a_tag = static_cast<TypeTag>(get_ra_type(flags));
        TypeTag b_tag = static_cast<TypeTag>(get_rb_type(flags));
        Op new_op = in.op;
        switch (in.op) {
            case Op::ADD:
                if (a_tag == TypeTag::kReal && b_tag == TypeTag::kReal) new_op = Op::ADD_REAL_REAL;
                else if (a_tag == TypeTag::kInteger && b_tag == TypeTag::kInteger) new_op = Op::ADD_INT_INT;
                break;
            case Op::SUB:
                if (a_tag == TypeTag::kReal && b_tag == TypeTag::kReal) new_op = Op::SUB_REAL_REAL;
                else if (a_tag == TypeTag::kInteger && b_tag == TypeTag::kInteger) new_op = Op::SUB_INT_INT;
                break;
            case Op::MUL:
                if (a_tag == TypeTag::kReal && b_tag == TypeTag::kReal) new_op = Op::MUL_REAL_REAL;
                else if (a_tag == TypeTag::kInteger && b_tag == TypeTag::kInteger) new_op = Op::MUL_INT_INT;
                break;
            case Op::DIV:
                if (a_tag == TypeTag::kReal && b_tag == TypeTag::kReal) new_op = Op::DIV_REAL_REAL;
                break;
            case Op::LT:
                if (a_tag == TypeTag::kReal && b_tag == TypeTag::kReal) new_op = Op::LT_REAL_REAL;
                break;
            case Op::LE:
                if (a_tag == TypeTag::kReal && b_tag == TypeTag::kReal) new_op = Op::LE_REAL_REAL;
                break;
            case Op::GT:
                if (a_tag == TypeTag::kReal && b_tag == TypeTag::kReal) new_op = Op::GT_REAL_REAL;
                break;
            case Op::GE:
                if (a_tag == TypeTag::kReal && b_tag == TypeTag::kReal) new_op = Op::GE_REAL_REAL;
                break;
            case Op::EQ:
                if (a_tag == TypeTag::kReal && b_tag == TypeTag::kReal) new_op = Op::EQ_REAL_REAL;
                break;
            case Op::NE:
                if (a_tag == TypeTag::kReal && b_tag == TypeTag::kReal) new_op = Op::NE_REAL_REAL;
                break;
            default: break;
        }
        if (new_op != in.op) {
            in.op = new_op;
            in.flags = set_quickened(flags);
            ++rewrites;
        }
    }
    return rewrites;
}

bool quicken_instr(Instr& in, FeedbackEngine const& fb, uint32_t instr_idx) {
    (void)fb; (void)instr_idx;
    // This function is kept for ABI compatibility but the real
    // quickening happens in quicken_observe() (called from the
    // dispatch loop) and quicken() (called by the tier manager).
    return false;
}

}  // namespace rjit
