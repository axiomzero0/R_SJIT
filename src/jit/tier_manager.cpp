// rjit/jit/tier_manager.cpp
#include "rjit/jit/tier_manager.hpp"
#include "rjit/bytecode/module.hpp"

namespace rjit {

TierManager::TierManager(FeedbackEngine& fb, DeoptContext& deopt)
    : fb_(fb), deopt_(deopt) {}

Tier TierManager::desired_tier(BytecodeFunction* fn) const {
    uint64_t n = fb_.call_count(fn).load(std::memory_order_relaxed);
    if (n < baseline_threshold)    return Tier::kInterpreter;
    if (n < specialized_threshold) return Tier::kBaseline;
    if (n < optimizing_threshold)  return Tier::kSpecialized;
    return Tier::kOptimizing;
}

void TierManager::set_current_tier(BytecodeFunction* fn, Tier t) {
    current_tier_[fn] = t;
}

Tier TierManager::current_tier(BytecodeFunction* fn) const {
    auto it = current_tier_.find(fn);
    return it != current_tier_.end() ? it->second : Tier::kInterpreter;
}

}  // namespace rjit
