// rjit/feedback/feedback.hpp - Runtime feedback engine
//
// The feedback engine records per-instruction and per-call-site
// metadata that the JIT uses to specialize code. Recording is
// selective (we don't record every execution) to keep overhead low.
//
// Feedback slots are organized by (function, instruction index).
// Each slot can hold a small amount of type, shape, call, branch,
// and allocation data. The JIT requests specific slots via
// `request_slot()`; unrequested slots are never recorded, which
// keeps the hot path fast.

#pragma once
#include <cstdint>
#include <atomic>
#include <vector>
#include <unordered_map>
#include "rjit/core/value.hpp"

namespace rjit {

class BytecodeFunction;
class Environment;

enum class FeedbackKind : uint8_t {
    kNone      = 0,
    kType      = 1,   // observed TypeTag of a value
    kShape     = 2,   // observed shape id of an environment
    kCall      = 3,   // observed callee
    kBranch    = 4,   // observed branch direction
    kLength    = 5,   // observed vector length
    kAlloc     = 6,   // observed allocation site
};

struct TypeFeedback {
    std::atomic<uint64_t> counts[16] = {};  // counts per TypeTag (low 4 bits)
    std::atomic<uint64_t> total       = 0;
    std::atomic<uint32_t> last_tag    = 0;

    void record(TypeTag t) noexcept {
        unsigned i = static_cast<unsigned>(t) & 0xF;
        counts[i].fetch_add(1, std::memory_order_relaxed);
        total.fetch_add(1, std::memory_order_relaxed);
        last_tag.store(static_cast<uint32_t>(t), std::memory_order_relaxed);
    }
    TypeTag dominant() const noexcept {
        unsigned best = 0; uint64_t best_count = 0;
        for (unsigned i = 0; i < 16; ++i) {
            uint64_t c = counts[i].load(std::memory_order_relaxed);
            if (c > best_count) { best_count = c; best = i; }
        }
        return static_cast<TypeTag>(best);
    }
};

struct ShapeFeedback {
    std::atomic<uint32_t> last_shape = 0;
    std::atomic<uint64_t> total      = 0;
    std::atomic<uint64_t> distinct   = 0;  // approximate count of distinct shapes seen

    void record(uint32_t shape_id) noexcept {
        if (last_shape.exchange(shape_id, std::memory_order_relaxed) != shape_id) {
            distinct.fetch_add(1, std::memory_order_relaxed);
        }
        total.fetch_add(1, std::memory_order_relaxed);
    }
};

struct CallFeedback {
    static constexpr uint32_t kMaxCallees = 4;
    struct Entry {
        std::atomic<void*> callee;
        std::atomic<uint64_t> count;
    };
    Entry entries[kMaxCallees];
    std::atomic<uint64_t> total = 0;
    std::atomic<bool>     megamorphic = false;

    void record(void* callee) noexcept {
        total.fetch_add(1, std::memory_order_relaxed);
        for (unsigned i = 0; i < kMaxCallees; ++i) {
            void* cur = entries[i].callee.load(std::memory_order_relaxed);
            if (cur == callee) {
                entries[i].count.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (cur == nullptr) {
                bool ok = entries[i].callee.compare_exchange_strong(
                    cur, callee, std::memory_order_relaxed);
                if (ok) {
                    entries[i].count.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                if (cur == callee) {
                    entries[i].count.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
            }
        }
        megamorphic.store(true, std::memory_order_relaxed);
    }
};

struct BranchFeedback {
    std::atomic<uint64_t> taken     = 0;
    std::atomic<uint64_t> not_taken = 0;

    void record(bool did_take) noexcept {
        if (did_take) taken.fetch_add(1, std::memory_order_relaxed);
        else          not_taken.fetch_add(1, std::memory_order_relaxed);
    }
};

struct LengthFeedback {
    std::atomic<uint32_t> last_length = 0;
    std::atomic<uint64_t> total        = 0;
    std::atomic<bool>     always_one   = true;
    std::atomic<bool>     always_small = true;  // <= 64

    void record(uint32_t len) noexcept {
        last_length.store(len, std::memory_order_relaxed);
        total.fetch_add(1, std::memory_order_relaxed);
        if (len != 1)        always_one.store(false, std::memory_order_relaxed);
        if (len > 64)        always_small.store(false, std::memory_order_relaxed);
    }
};

struct AllocFeedback {
    std::atomic<uint64_t> total       = 0;
    std::atomic<uint64_t> escapes     = 0;  // set if the object escapes its allocation site
    std::atomic<bool>     never_escapes = true;

    void record(bool did_escape) noexcept {
        total.fetch_add(1, std::memory_order_relaxed);
        if (did_escape) {
            escapes.fetch_add(1, std::memory_order_relaxed);
            never_escapes.store(false, std::memory_order_relaxed);
        }
    }
};

class FeedbackEngine {
public:
    FeedbackEngine();

    // Type feedback for a given (function, instruction) pair.
    TypeFeedback& type(BytecodeFunction* fn, uint32_t instr_idx);
    ShapeFeedback& shape(BytecodeFunction* fn, uint32_t instr_idx);
    CallFeedback& call(BytecodeFunction* fn, uint32_t instr_idx);
    BranchFeedback& branch(BytecodeFunction* fn, uint32_t instr_idx);
    LengthFeedback& length(BytecodeFunction* fn, uint32_t instr_idx);
    AllocFeedback& alloc(BytecodeFunction* fn, uint32_t instr_idx);

    // Per-function counters
    std::atomic<uint64_t>& call_count(BytecodeFunction* fn);
    std::atomic<uint64_t>& loop_iter(BytecodeFunction* fn, uint32_t loop_header_idx);

    // Tiering thresholds
    static constexpr uint64_t kBaselineThreshold    = 100;     // calls
    static constexpr uint64_t kSpecializedThreshold = 1000;    // calls
    static constexpr uint64_t kOptimizingThreshold  = 10000;   // calls
    static constexpr uint64_t kOSRThreshold         = 5000;    // loop iterations

private:
    using Key = std::pair<BytecodeFunction*, uint32_t>;
    struct KeyHash { size_t operator()(Key const& k) const noexcept {
        return reinterpret_cast<uintptr_t>(k.first) ^ (size_t{k.second} << 8);
    }};

    std::unordered_map<Key, TypeFeedback,   KeyHash> types_;
    std::unordered_map<Key, ShapeFeedback,  KeyHash> shapes_;
    std::unordered_map<Key, CallFeedback,   KeyHash> calls_;
    std::unordered_map<Key, BranchFeedback, KeyHash> branches_;
    std::unordered_map<Key, LengthFeedback, KeyHash> lengths_;
    std::unordered_map<Key, AllocFeedback,  KeyHash> allocs_;
    std::unordered_map<BytecodeFunction*, std::atomic<uint64_t>> call_counts_;
    std::unordered_map<Key, std::atomic<uint64_t>, KeyHash> loop_iters_;
};

}  // namespace rjit
