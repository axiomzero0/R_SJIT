// rjit/core/promise.cpp
//
// Promise implementation with proper expression evaluation.
//
// A Promise stores a BytecodeFunction (compiled from the argument
// expression) and the environment in which to evaluate it. When
// forced, the promise compiles the expression (if not already
// compiled) and executes the bytecode, memoizing the result.
//
// The promise state machine:
//   UNFORCED -> FORCING -> FORCED  (normal)
//   UNFORCED -> FORCING -> ERROR   (evaluation failed)
//   FORCING -> ERROR               (cycle detected)

#include "rjit/core/promise.hpp"
#include "rjit/core/environment.hpp"
#include "rjit/core/context.hpp"
#include "rjit/vm/interpreter.hpp"
#include "rjit/frontend/ast.hpp"
#include "rjit/frontend/lower.hpp"
#include "rjit/bytecode/module.hpp"
#include "rjit/core/error.hpp"

namespace rjit {

Promise::Promise(Value expr, Environment* env)
    : HeapObject(TypeTag::kPromise), expr_(expr), env_(env), state_(PromiseState::kUnforced) {}

Value Promise::force() {
    if (state_ == PromiseState::kForced) return value_;
    if (state_ == PromiseState::kError) {
        current_context().raise_error("promise previously errored");
    }
    if (state_ == PromiseState::kForcing) {
        current_context().raise_error("promise already being evaluated (cycle)");
    }
    state_ = PromiseState::kForcing;
    try {
        // The expr_ Value wraps a BytecodeFunction (compiled from the
        // argument expression at call time). If it's a BytecodeFunction,
        // execute it. Otherwise, just return the value directly (for
        // already-computed arguments).
        Value result;
        if (expr_.is_bytecode_fn()) {
            BytecodeFunction* fn = expr_.as_bytecode_fn();
            result = current_context().interpreter().execute(fn, env_);
        } else {
            // Not a bytecode function — it's already a value.
            result = expr_;
        }
        value_ = result;
        state_ = PromiseState::kForced;
        return result;
    } catch (...) {
        state_ = PromiseState::kError;
        throw;
    }
}

void Promise::trace(Visitor& v) const {
    v.visit(const_cast<Value&>(expr_));
    HeapObject* p = const_cast<HeapObject*>(static_cast<HeapObject const*>(env_));
    v.visit_heap(p);
    const_cast<Promise*>(this)->env_ = static_cast<Environment*>(p);
    v.visit(const_cast<Value&>(value_));
}

Value force_if_promise(Value v) {
    if (v.is_promise()) return v.as_promise()->force();
    return v;
}

}  // namespace rjit
