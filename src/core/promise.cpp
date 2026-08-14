// rjit/core/promise.cpp
#include "rjit/core/promise.hpp"
#include "rjit/core/environment.hpp"
#include "rjit/core/context.hpp"
#include "rjit/vm/interpreter.hpp"

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
    // expr_ is a Value wrapping a Call/Symbol AST node. For now, we
    // can't easily re-evaluate an AST from a Value (we'd need to
    // reconstruct the Ast from a Call heap object). For the initial
    // implementation we just return nil — proper promise forcing
    // requires storing a BytecodeFunction in the Promise instead.
    Value result = Value::nil();
    (void)env_;
    value_ = result;
    state_ = PromiseState::kForced;
    return result;
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
