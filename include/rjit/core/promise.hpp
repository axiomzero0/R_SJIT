// rjit/core/promise.hpp - R promises (delayed arguments)
//
// R arguments are lazy: when a function is called, each argument is
// wrapped in a "promise" that captures its expression and the
// environment in which it should be evaluated. The promise is forced
// (evaluated) only when the callee actually reads its value, and the
// result is memoized so it's only evaluated once.
//
// For the JIT, promises are both:
//   * a huge optimization opportunity — many promises can be
//     eliminated entirely by inlining + escape analysis;
//   * a huge correctness hazard — promises can be reified by `substitute()`,
//     `quote()`, reflection, etc., so we must keep enough info to
//     reconstruct them on deopt.
//
// State machine:
//   UNFORCED -> FORCING -> FORCED  (and on FORCING cycle: ERROR)
//
// We also support "forced constant" promises where the optimizer has
// determined the value will never change and the expression can be
// discarded entirely.

#pragma once

#include <cstdint>
#include "rjit/core/value.hpp"
#include "rjit/core/gc.hpp"

namespace rjit {

class Environment;
class Call;
class Symbol;

enum class PromiseState : uint8_t {
    kUnforced = 0,
    kForcing  = 1,
    kForced   = 2,
    kError    = 3,
};

class Promise : public HeapObject {
public:
    RJIT_DECLARE_GC_NEW(Promise)

    // `expr` is the AST node (a Call or Symbol) captured at call time.
    // `env` is the calling environment.
    Promise(Value expr, Environment* env);

    Value const& expr() const noexcept { return expr_; }
    Environment* env() const noexcept { return env_; }
    PromiseState state() const noexcept { return state_; }
    Value const& value() const noexcept { return value_; }

    // Force the promise. May recursively force other promises. Returns
    // the value. If forcing cycles back to this promise (infinite
    // recursion), an R error is signaled.
    Value force();

    // Mark as forced with a precomputed value (used by JIT-compiled
    // code that has already evaluated the expression).
    void set_forced(Value v) noexcept {
        value_ = v;
        state_ = PromiseState::kForced;
    }

    void trace(Visitor& v) const override;

private:
    Value         expr_;
    Environment*  env_;
    Value         value_;
    PromiseState  state_;
};

// Helper: if `v` is a Promise, force it and replace with the value.
Value force_if_promise(Value v);

}  // namespace rjit
