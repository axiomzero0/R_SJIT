// rjit/feedback/feedback.cpp
#include "rjit/feedback/feedback.hpp"
#include "rjit/bytecode/module.hpp"

namespace rjit {

FeedbackEngine::FeedbackEngine() = default;

TypeFeedback& FeedbackEngine::type(BytecodeFunction* fn, uint32_t i)   { return types_[{fn,i}]; }
ShapeFeedback& FeedbackEngine::shape(BytecodeFunction* fn, uint32_t i) { return shapes_[{fn,i}]; }
CallFeedback& FeedbackEngine::call(BytecodeFunction* fn, uint32_t i)   { return calls_[{fn,i}]; }
BranchFeedback& FeedbackEngine::branch(BytecodeFunction* fn, uint32_t i) { return branches_[{fn,i}]; }
LengthFeedback& FeedbackEngine::length(BytecodeFunction* fn, uint32_t i) { return lengths_[{fn,i}]; }
AllocFeedback& FeedbackEngine::alloc(BytecodeFunction* fn, uint32_t i) { return allocs_[{fn,i}]; }

std::atomic<uint64_t>& FeedbackEngine::call_count(BytecodeFunction* fn) {
    auto it = call_counts_.find(fn);
    if (it == call_counts_.end()) {
        auto [ins, _] = call_counts_.emplace(fn, 0);
        return ins->second;
    }
    return it->second;
}

std::atomic<uint64_t>& FeedbackEngine::loop_iter(BytecodeFunction* fn, uint32_t i) {
    auto it = loop_iters_.find({fn, i});
    if (it == loop_iters_.end()) {
        auto [ins, _] = loop_iters_.emplace(std::make_pair(fn, i), 0);
        return ins->second;
    }
    return it->second;
}

}  // namespace rjit
