// rjit/core/context.hpp - Execution context: thread-local VM state
//
// The Context holds everything a thread needs to execute R code:
//   * the current frame (function being executed)
//   * the global / base environments
//   * the GC instance (if per-thread) or shared GC pointer
//   * the symbol table (interned identifiers)
//   * the JIT tier manager
//   * OSR / deopt state
//
// All VM runtime functions take a Context& rather than using a global
// variable. This makes the VM re-entrant and lets multiple threads
// execute R code in parallel (important for parLapply and friends).

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "rjit/core/value.hpp"
#include "rjit/core/environment.hpp"

namespace rjit {

class GC;
class Symbol;
class TierManager;
class Interpreter;
class FeedbackEngine;
class DeoptContext;

// Symbol interning. R's symbol table is global; we replicate that.
class SymbolTable {
public:
    SymbolTable();

    // Intern a string. Returns the symbol id (stable for the lifetime
    // of the process).
    uint32_t intern(std::string_view name);

    // Reverse lookup. Returns empty string_view if not found.
    std::string_view name_of(uint32_t id) const;

    // Total symbols interned.
    size_t size() const noexcept { return names_.size(); }

private:
    std::unordered_map<std::string, uint32_t> map_;
    std::vector<std::string>                  names_;
};

class Context {
public:
    Context();
    ~Context();

    // Non-copyable, non-movable
    Context(Context const&) = delete;
    Context& operator=(Context const&) = delete;

    // Accessors
    GC&               gc()            noexcept { return *gc_; }
    SymbolTable&      symbols()       noexcept { return *symbols_; }
    Environment*      global_env()    noexcept { return global_env_; }
    Environment*      base_env()      noexcept { return base_env_; }
    Interpreter&      interpreter()   noexcept { return *interp_; }
    FeedbackEngine&   feedback()      noexcept { return *feedback_; }
    TierManager&      tiers()         noexcept { return *tiers_; }
    DeoptContext&     deopt()         noexcept { return *deopt_; }

    // Intern a symbol id from a name
    uint32_t intern_symbol(std::string_view name) { return symbols_->intern(name); }
    std::string_view symbol_name(uint32_t id) { return symbols_->name_of(id); }

    // Push/pop a root (for GC).
    void push_root(Value* v) { gc_->push_root(v); }
    void pop_root(Value* v) { gc_->pop_root(v); }

    // The current "depth" of execution, used to decide when to tier up.
    // Incremented on function call, decremented on return.
    uint32_t call_depth() const noexcept { return call_depth_; }
    void     inc_call_depth() noexcept { ++call_depth_; }
    void     dec_call_depth() noexcept { --call_depth_; }

    // Raise an R error (longjmp-style). The currently-executing frame
    // is unwound; control returns to the nearest R tryCatch.
    [[noreturn]] void raise_error(std::string msg);

    // Most recent error message (for tests).
    std::string const& last_error() const noexcept { return last_error_; }
    void set_last_error(std::string msg) { last_error_ = std::move(msg); }

private:
    std::unique_ptr<GC>             gc_;
    std::unique_ptr<SymbolTable>    symbols_;
    Environment*                    global_env_;
    Environment*                    base_env_;
    std::unique_ptr<Interpreter>    interp_;
    std::unique_ptr<FeedbackEngine> feedback_;
    std::unique_ptr<TierManager>    tiers_;
    std::unique_ptr<DeoptContext>   deopt_;
    uint32_t                        call_depth_ = 0;
    std::string                     last_error_;
};

// Per-thread context accessor.
Context& current_context();
void set_current_context(Context* ctx);

}  // namespace rjit
