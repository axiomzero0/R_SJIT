// rjit/jit/tier_manager.hpp - JIT tiering decisions
//
// The TierManager watches per-function call counts and loop iteration
// counts and decides when to compile a function at the next tier:
//
//   Tier 0: Interpreter (with quickening + ICs)
//   Tier 1: Baseline JIT (bytecode -> machine code, no optimization)
//   Tier 1.5: Type-specialized JIT (uses type feedback)
//   Tier 2: Sea-of-Nodes optimizing JIT
//   Tier 3: Tracing JIT
//   Tier 4: PGO-guided recompilation
//
// Tier-up thresholds are configurable. Tier-down (deopt) is handled
// by the DeoptContext, not by TierManager.

#pragma once
#include <cstdint>
#include <unordered_map>
#include "rjit/feedback/feedback.hpp"
#include "rjit/jit/deopt.hpp"

namespace rjit {

class BytecodeFunction;
class JitCode;

enum class Tier : uint8_t {
    kInterpreter = 0,
    kBaseline    = 1,
    kSpecialized = 2,
    kOptimizing  = 3,
    kTracing     = 4,
    kPGO         = 5,
};

class TierManager {
public:
    TierManager(FeedbackEngine& fb, DeoptContext& deopt);

    // Decide which tier a function should be at, based on feedback.
    Tier desired_tier(BytecodeFunction* fn) const;

    // Record the current tier of a function.
    void set_current_tier(BytecodeFunction* fn, Tier t);

    Tier current_tier(BytecodeFunction* fn) const;

    // Thresholds (overridable for testing)
    uint64_t baseline_threshold    = FeedbackEngine::kBaselineThreshold;
    uint64_t specialized_threshold = FeedbackEngine::kSpecializedThreshold;
    uint64_t optimizing_threshold  = FeedbackEngine::kOptimizingThreshold;
    uint64_t osr_threshold         = FeedbackEngine::kOSRThreshold;

    FeedbackEngine& feedback() noexcept { return fb_; }
    DeoptContext&   deopt()     noexcept { return deopt_; }

private:
    FeedbackEngine& fb_;
    DeoptContext&   deopt_;
    std::unordered_map<BytecodeFunction*, Tier> current_tier_;
};

}  // namespace rjit
