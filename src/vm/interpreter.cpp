// rjit/vm/interpreter.cpp
#include "rjit/vm/interpreter.hpp"
#include "rjit/vm/quickening.hpp"
#include "rjit/core/error.hpp"
#include "rjit/core/closure.hpp"
#include "rjit/core/symbol.hpp"
#include "rjit/core/promise.hpp"
#include "rjit/core/vector.hpp"
#include "rjit/core/context.hpp"
#include "rjit/jit/tier_manager.hpp"
#include "rjit/jit/baseline.hpp"
#include "rjit/frontend/ast.hpp"
#include "rjit/frontend/lower.hpp"
#include "rjit/bytecode/opcodes.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace rjit {

// ----------------- Frame / FramePool -----------------

Instr const& Frame::instr() const {
    return fn->code[pc];
}

Value* FramePool::alloc_regs(uint32_t n) {
    regs_chunks_.emplace_back(n, Value::nil());
    return regs_chunks_.back().data();
}

Frame* FramePool::alloc_frame() {
    if (frames_used_ < frames_.size()) {
        return &frames_[frames_used_++];
    }
    frames_.emplace_back();
    return &frames_[frames_used_++];
}

// Note: std::deque guarantees that existing element pointers remain
// valid when new elements are appended. This is essential for deep
// recursion, where each frame's `caller` pointer must remain valid
// even as new frames are allocated.

// ----------------- Interpreter -----------------

Interpreter::Interpreter(Context& ctx) : ctx_(ctx) {}

Value Interpreter::eval(Ast const& ast, Environment* env) {
    // For promise forcing, we compile the AST on the fly and execute.
    Lowerer lower(ctx_);
    BytecodeFunction* fn = lower.lower_program(ast);
    return execute(fn, env);
}

Value Interpreter::execute(BytecodeFunction* fn, Environment* env) {
    return execute_with_args(fn, env, nullptr, 0);
}

Value Interpreter::execute_with_args(BytecodeFunction* fn, Environment* env, Value* args, uint32_t nargs) {
    Frame* frame = new Frame();
    frame->init(fn, env, call_stack_.empty() ? nullptr : call_stack_.back());
    Value* regs = new Value[fn->nregs];
    for (uint32_t i = 0; i < fn->nregs; ++i) regs[i] = Value::nil();
    if (args) {
        for (uint32_t i = 0; i < nargs && i < fn->nparams; ++i) {
            regs[i] = force_if_promise(args[i]);
        }
    }
    frame->regs = regs;
    call_stack_.push_back(frame);

    // Tier check: if the call count exceeds the baseline threshold,
    // try to compile with the baseline JIT.
    uint64_t count = ctx_.feedback().call_count(fn).fetch_add(1, std::memory_order_relaxed);
    if (count == FeedbackEngine::kBaselineThreshold &&
        ctx_.tiers().current_tier(fn) == Tier::kInterpreter) {
        BaselineJIT jit(ctx_);
        JitCode* code = jit.compile(fn);
        if (code) {
            ctx_.tiers().set_current_tier(fn, Tier::kBaseline);
            // Store the compiled code for future use.
            // (A full implementation would cache this in a code cache.)
        }
    }

    Value result = dispatch_loop(*frame);
    call_stack_.pop_back();
    delete[] regs;
    delete frame;
    return result;
}

// Slow path: look up a variable in the environment chain.
Value Interpreter::slow_lookup_var(Environment* env, uint32_t sym_id) {
    Value out;
    if (!env->lookup(sym_id, &out)) {
        ctx_.raise_error("object '" + std::string(ctx_.symbol_name(sym_id)) + "' not found");
    }
    return out;
}

void Interpreter::slow_define_var(Environment* env, uint32_t sym_id, Value v) {
    env->define(sym_id, v);
}

void Interpreter::slow_set_super(Environment* env, uint32_t sym_id, Value v) {
    if (!env->set_existing(sym_id, v)) {
        // Not found anywhere: define in global.
        get_global_env()->define(sym_id, v);
    }
}

// Slow path: function call (handles builtins and closures).
Value Interpreter::slow_call(Value callee, Value* args, uint32_t nargs, Environment* caller_env) {
    if (callee.is_closure()) {
        Closure* c = callee.as_closure();
        BytecodeFunction* fn = c->code();
        // Create a new environment for the call, chained to the
        // closure's captured environment.
        Environment* call_env = new Environment(c->env());
        // Bind parameters by name into the environment (so LOAD_VAR
        // works for variables not in registers).
        for (uint32_t i = 0; i < nargs && i < fn->nparams; ++i) {
            if (i < fn->param_names.size()) {
                uint32_t sym = ctx_.intern_symbol(fn->param_names[i]);
                call_env->define(sym, force_if_promise(args[i]));
            }
        }
        // Execute with args bound into registers 0..nparams-1.
        return execute_with_args(fn, call_env, args, nargs);
    }
    if (callee.is_builtin() || callee.is(TypeTag::kSpecial)) {
        Builtin* b = callee.as_builtin();
        return b->impl()(ctx_, args, nargs);
    }
    if (callee.is_nil()) {
        ctx_.raise_error("attempt to call NULL");
    }
    ctx_.raise_error("attempt to apply non-function");
}

// ----------------- Arithmetic -----------------

Value Interpreter::add_values(Value a, Value b) {
    a = force_if_promise(a);
    b = force_if_promise(b);
    // Fast path: both real scalars
    if (a.is_real() && b.is_real()) {
        return Value::real(a.as_real() + b.as_real());
    }
    if (a.is_integer() && b.is_integer()) {
        int64_t r = static_cast<int64_t>(a.as_integer()) + static_cast<int64_t>(b.as_integer());
        if (r < INT32_MIN || r > INT32_MAX) return Value::real(static_cast<double>(r));
        return Value::integer(static_cast<int32_t>(r));
    }
    if (a.is_real() && b.is_integer()) {
        int32_t bi = b.as_integer();
        return Value::real(a.as_real() + (bi == kNaInt ? kNaReal : static_cast<double>(bi)));
    }
    if (a.is_integer() && b.is_real()) {
        int32_t ai = a.as_integer();
        return Value::real((ai == kNaInt ? kNaReal : static_cast<double>(ai)) + b.as_real());
    }
    if (a.is_logical() && b.is_logical()) {
        // promote to integer
        int32_t av = a.as_logical() == kNaLogical ? kNaInt : a.as_logical();
        int32_t bv = b.as_logical() == kNaLogical ? kNaInt : b.as_logical();
        return Value::integer(av + bv);
    }
    // Vector cases
    if (a.is_vector() || b.is_vector()) {
        // For simplicity, coerce both to real vectors and add elementwise.
        // A production implementation would specialize per vector type
        // and use SIMD.
        Vector* av = a.is_vector() ? a.as_vector() : nullptr;
        Vector* bv = b.is_vector() ? b.as_vector() : nullptr;
        if (av && bv) {
            size_t n = std::max(av->length(), bv->length());
            Vector* out = new Vector(VectorType::kReal, n);
            for (size_t i = 0; i < n; ++i) {
                double av_i = (av->length() == 0) ? kNaReal :
                              (av->vtype() == VectorType::kReal ? av->real_at(i % av->length()) :
                              av->vtype() == VectorType::kInteger ? (av->integer_at(i % av->length()) == kNaInt ? kNaReal : (double)av->integer_at(i % av->length())) :
                              0.0);
                double bv_i = (bv->length() == 0) ? kNaReal :
                              (bv->vtype() == VectorType::kReal ? bv->real_at(i % bv->length()) :
                              bv->vtype() == VectorType::kInteger ? (bv->integer_at(i % bv->length()) == kNaInt ? kNaReal : (double)bv->integer_at(i % bv->length())) :
                              0.0);
                out->set_real(i, (av_i == kNaReal || bv_i == kNaReal) ? kNaReal : av_i + bv_i);
            }
            return Value::from_heap(TypeTag::kVector, out);
        }
        // scalar + vector
        Vector* v = av ? av : bv;
        double s = av ? (b.is_real() ? b.as_real() : (b.is_integer() ? (b.as_integer() == kNaInt ? kNaReal : (double)b.as_integer()) : 0))
                      : (a.is_real() ? a.as_real() : (a.is_integer() ? (a.as_integer() == kNaInt ? kNaReal : (double)a.as_integer()) : 0));
        Vector* out = new Vector(VectorType::kReal, v->length());
        for (size_t i = 0; i < v->length(); ++i) {
            double vi = (v->vtype() == VectorType::kReal ? v->real_at(i) :
                        v->vtype() == VectorType::kInteger ? (v->integer_at(i) == kNaInt ? kNaReal : (double)v->integer_at(i)) :
                        0.0);
            out->set_real(i, (s == kNaReal || vi == kNaReal) ? kNaReal : s + vi);
        }
        return Value::from_heap(TypeTag::kVector, out);
    }
    // Fallback: error
    ctx_.raise_error("unsupported operand types for '+'");
}

Value Interpreter::sub_values(Value a, Value b) {
    a = force_if_promise(a); b = force_if_promise(b);
    if (a.is_real() && b.is_real()) return Value::real(a.as_real() - b.as_real());
    if (a.is_integer() && b.is_integer()) {
        int64_t r = static_cast<int64_t>(a.as_integer()) - static_cast<int64_t>(b.as_integer());
        if (r < INT32_MIN || r > INT32_MAX) return Value::real(static_cast<double>(r));
        return Value::integer(static_cast<int32_t>(r));
    }
    // Fallback: coerce to real
    double av = a.is_real() ? a.as_real() : (a.is_integer() ? (a.as_integer() == kNaInt ? kNaReal : (double)a.as_integer()) : 0);
    double bv = b.is_real() ? b.as_real() : (b.is_integer() ? (b.as_integer() == kNaInt ? kNaReal : (double)b.as_integer()) : 0);
    return Value::real(av - bv);
}

Value Interpreter::mul_values(Value a, Value b) {
    a = force_if_promise(a); b = force_if_promise(b);
    if (a.is_real() && b.is_real()) return Value::real(a.as_real() * b.as_real());
    if (a.is_integer() && b.is_integer()) {
        int64_t r = static_cast<int64_t>(a.as_integer()) * static_cast<int64_t>(b.as_integer());
        if (r < INT32_MIN || r > INT32_MAX) return Value::real(static_cast<double>(r));
        return Value::integer(static_cast<int32_t>(r));
    }
    double av = a.is_real() ? a.as_real() : (a.is_integer() ? (a.as_integer() == kNaInt ? kNaReal : (double)a.as_integer()) : 0);
    double bv = b.is_real() ? b.as_real() : (b.is_integer() ? (b.as_integer() == kNaInt ? kNaReal : (double)b.as_integer()) : 0);
    return Value::real(av * bv);
}

Value Interpreter::div_values(Value a, Value b) {
    a = force_if_promise(a); b = force_if_promise(b);
    double av = a.is_real() ? a.as_real() : (a.is_integer() ? (a.as_integer() == kNaInt ? kNaReal : (double)a.as_integer()) : 0);
    double bv = b.is_real() ? b.as_real() : (b.is_integer() ? (b.as_integer() == kNaInt ? kNaReal : (double)b.as_integer()) : 0);
    return Value::real(av / bv);
}

template <typename Cmp>
static Value cmp_scalar(Value a, Value b, Cmp&& cmp) {
    a = force_if_promise(a); b = force_if_promise(b);
    // Promote integer/logical to real for comparison
    auto to_real = [](Value v) -> double {
        if (v.is_real()) return v.as_real();
        if (v.is_integer()) return v.as_integer() == kNaInt ? kNaReal : (double)v.as_integer();
        if (v.is_logical()) return v.as_logical() == kNaLogical ? kNaReal : (double)v.as_logical();
        return kNaReal;
    };
    double av = to_real(a);
    double bv = to_real(b);
    if (av == kNaReal || bv == kNaReal) return Value::logical(kNaLogical);
    return Value::logical(cmp(av, bv) ? 1 : 0);
}

Value Interpreter::lt_values(Value a, Value b) { return cmp_scalar(a, b, [](double x, double y){ return x < y; }); }
Value Interpreter::le_values(Value a, Value b) { return cmp_scalar(a, b, [](double x, double y){ return x <= y; }); }
Value Interpreter::gt_values(Value a, Value b) { return cmp_scalar(a, b, [](double x, double y){ return x > y; }); }
Value Interpreter::ge_values(Value a, Value b) { return cmp_scalar(a, b, [](double x, double y){ return x >= y; }); }
Value Interpreter::eq_values(Value a, Value b) { return cmp_scalar(a, b, [](double x, double y){ return x == y; }); }
Value Interpreter::ne_values(Value a, Value b) { return cmp_scalar(a, b, [](double x, double y){ return x != y; }); }

// ----------------- Indexing -----------------

Value Interpreter::index_value(Value base, Value idx) {
    base = force_if_promise(base); idx = force_if_promise(idx);
    if (!base.is_vector()) return Value::nil();
    Vector* v = base.as_vector();
    if (idx.is_integer() || idx.is_real() || idx.is_logical()) {
        int32_t i = idx.is_integer() ? idx.as_integer() :
                    idx.is_real() ? (int32_t)idx.as_real() :
                    idx.as_logical();
        if (i == kNaInt || i < 1 || i > (int32_t)v->length()) return Value::nil();
        switch (v->vtype()) {
            case VectorType::kReal:    return Value::real(v->real_at(i-1));
            case VectorType::kInteger: return Value::integer(v->integer_at(i-1));
            case VectorType::kLogical: return Value::logical(v->logical_at(i-1));
            case VectorType::kString:  return Value::string(v->string_at(i-1));
            case VectorType::kList:    return v->list_at(i-1);
            default: return Value::nil();
        }
    }
    return Value::nil();
}

Value Interpreter::index2_value(Value base, Value idx) {
    return index_value(base, idx);  // for now, identical to [[
}

Value Interpreter::dollar_value(Value base, uint32_t sym_id) {
    base = force_if_promise(base);
    // For now: if base is an environment, look up sym_id
    if (base.is_environment()) {
        Value out;
        if (base.as_environment()->lookup(sym_id, &out)) return out;
        return Value::nil();
    }
    // If base is a list, look up by name attribute (not yet supported)
    return Value::nil();
}

int32_t Interpreter::as_logical_scalar(Value v) {
    v = force_if_promise(v);
    if (v.is_logical()) return v.as_logical();
    if (v.is_integer()) return v.as_integer() == 0 ? 0 : (v.as_integer() == kNaInt ? kNaLogical : 1);
    if (v.is_real())    return v.as_real() != v.as_real() ? kNaLogical : (v.as_real() != 0.0 ? 1 : 0);
    if (v.is_nil())     return 0;  // NULL is treated as FALSE in conditions? Actually R errors. For simplicity, treat as FALSE.
    return 0;
}

// ----------------- Main dispatch loop -----------------

Value Interpreter::dispatch_loop(Frame& frame) {
    BytecodeFunction* fn = frame.fn;
    Value* regs = frame.regs;
    Environment* env = frame.env;

    while (true) {
        Instr const& in = fn->code[frame.pc];
        uint32_t next_pc = frame.pc + 1;
        switch (in.op) {
            case Op::NOP:           break;
            case Op::HALT:          goto exit_loop;
            case Op::LOAD_NIL:      regs[in.rdest] = Value::nil(); break;
            case Op::LOAD_TRUE:     regs[in.rdest] = Value::logical(1); break;
            case Op::LOAD_FALSE:    regs[in.rdest] = Value::logical(0); break;
            case Op::LOAD_NA:       regs[in.rdest] = Value::logical(kNaLogical); break;
            case Op::LOAD_REAL:     regs[in.rdest] = fn->constants[in.k]; break;
            case Op::LOAD_INT:      regs[in.rdest] = fn->constants[in.k]; break;
            case Op::LOAD_STRING:   regs[in.rdest] = fn->constants[in.k]; break;

            case Op::LOAD_LOCAL:    regs[in.rdest] = regs[in.ra]; break;

            case Op::LOAD_VAR: {
                // Inline cache fast path — keyed by (fn, pc) to avoid
                // collisions between functions sharing the same PC.
                LoadVarIC& ic = ic_table_.load_var_ic(fn, frame.pc);
                if (ic.valid() && ic.env->shape_id() == ic.shape_id) {
                    regs[in.rdest] = ic.env->slot_get(ic.slot);
                } else {
                    Value v = slow_lookup_var(env, in.k);
                    regs[in.rdest] = v;
                    // Update IC
                    Environment* found_env = env;
                    uint32_t slot = 0;
                    env->lookup(in.k, nullptr, &found_env, &slot);
                    if (found_env) {
                        ic.env = found_env;
                        ic.shape_id = found_env->shape_id();
                        ic.slot = slot;
                    }
                }
                ctx_.feedback().type(fn, frame.pc).record(regs[in.rdest].tag());
                break;
            }

            case Op::STORE_VAR:     slow_define_var(env, in.k, regs[in.ra]); break;
            case Op::STORE_SUPER:   slow_set_super(env, in.k, regs[in.ra]); break;

            case Op::JUMP:          next_pc = in.k; break;
            case Op::JUMP_IF_FALSE: {
                int32_t b = as_logical_scalar(regs[in.ra]);
                if (b == 0) next_pc = in.k;
                ctx_.feedback().branch(fn, frame.pc).record(b != 0);
                break;
            }
            case Op::JUMP_IF_TRUE: {
                int32_t b = as_logical_scalar(regs[in.ra]);
                if (b != 0) next_pc = in.k;
                ctx_.feedback().branch(fn, frame.pc).record(b != 0);
                break;
            }
            case Op::JUMP_IF_NA: {
                int32_t b = as_logical_scalar(regs[in.ra]);
                if (b == kNaLogical) next_pc = in.k;
                break;
            }

            case Op::ADD:           regs[in.rdest] = add_values(regs[in.ra], regs[in.rb]);
                                    ctx_.feedback().type(fn, frame.pc).record(regs[in.rdest].tag()); break;
            case Op::SUB:           regs[in.rdest] = sub_values(regs[in.ra], regs[in.rb]); break;
            case Op::MUL:           regs[in.rdest] = mul_values(regs[in.ra], regs[in.rb]); break;
            case Op::DIV:           regs[in.rdest] = div_values(regs[in.ra], regs[in.rb]); break;
            case Op::NEG: {
                Value v = force_if_promise(regs[in.ra]);
                if (v.is_real())    regs[in.rdest] = Value::real(-v.as_real());
                else if (v.is_integer()) {
                    int32_t x = v.as_integer();
                    regs[in.rdest] = Value::integer(x == kNaInt ? kNaInt : -x);
                } else if (v.is_logical()) {
                    int32_t x = v.as_logical();
                    regs[in.rdest] = Value::integer(x == kNaLogical ? kNaInt : -x);
                }
                break;
            }
            case Op::ADD_REAL_REAL: regs[in.rdest] = Value::real(regs[in.ra].as_real() + regs[in.rb].as_real()); break;
            case Op::ADD_INT_INT:   regs[in.rdest] = Value::integer(regs[in.ra].as_integer() + regs[in.rb].as_integer()); break;
            case Op::ADD_REAL_INT:  regs[in.rdest] = Value::real(regs[in.ra].as_real() + (regs[in.rb].as_integer() == kNaInt ? kNaReal : (double)regs[in.rb].as_integer())); break;
            case Op::ADD_INT_REAL:  regs[in.rdest] = Value::real((regs[in.ra].as_integer() == kNaInt ? kNaReal : (double)regs[in.ra].as_integer()) + regs[in.rb].as_real()); break;
            case Op::SUB_REAL_REAL: regs[in.rdest] = Value::real(regs[in.ra].as_real() - regs[in.rb].as_real()); break;
            case Op::SUB_INT_INT:   regs[in.rdest] = Value::integer(regs[in.ra].as_integer() - regs[in.rb].as_integer()); break;
            case Op::MUL_REAL_REAL: regs[in.rdest] = Value::real(regs[in.ra].as_real() * regs[in.rb].as_real()); break;
            case Op::MUL_INT_INT:   regs[in.rdest] = Value::integer(regs[in.ra].as_integer() * regs[in.rb].as_integer()); break;
            case Op::DIV_REAL_REAL: regs[in.rdest] = Value::real(regs[in.ra].as_real() / regs[in.rb].as_real()); break;
            case Op::DIV_INT_INT:   regs[in.rdest] = Value::real((double)regs[in.ra].as_integer() / (double)regs[in.rb].as_integer()); break;

            case Op::LT:            regs[in.rdest] = lt_values(regs[in.ra], regs[in.rb]); break;
            case Op::LE:            regs[in.rdest] = le_values(regs[in.ra], regs[in.rb]); break;
            case Op::GT:            regs[in.rdest] = gt_values(regs[in.ra], regs[in.rb]); break;
            case Op::GE:            regs[in.rdest] = ge_values(regs[in.ra], regs[in.rb]); break;
            case Op::EQ:            regs[in.rdest] = eq_values(regs[in.ra], regs[in.rb]); break;
            case Op::NE:            regs[in.rdest] = ne_values(regs[in.ra], regs[in.rb]); break;
            case Op::LT_REAL_REAL:  regs[in.rdest] = Value::logical(regs[in.ra].as_real() <  regs[in.rb].as_real() ? 1 : 0); break;
            case Op::LE_REAL_REAL:  regs[in.rdest] = Value::logical(regs[in.ra].as_real() <= regs[in.rb].as_real() ? 1 : 0); break;
            case Op::GT_REAL_REAL:  regs[in.rdest] = Value::logical(regs[in.ra].as_real() >  regs[in.rb].as_real() ? 1 : 0); break;
            case Op::GE_REAL_REAL:  regs[in.rdest] = Value::logical(regs[in.ra].as_real() >= regs[in.rb].as_real() ? 1 : 0); break;
            case Op::EQ_REAL_REAL:  regs[in.rdest] = Value::logical(regs[in.ra].as_real() == regs[in.rb].as_real() ? 1 : 0); break;
            case Op::NE_REAL_REAL:  regs[in.rdest] = Value::logical(regs[in.ra].as_real() != regs[in.rb].as_real() ? 1 : 0); break;

            case Op::AND_SCALAR: {
                int32_t a = as_logical_scalar(regs[in.ra]);
                int32_t b = as_logical_scalar(regs[in.rb]);
                if (a == kNaLogical || b == kNaLogical) regs[in.rdest] = Value::logical(kNaLogical);
                else regs[in.rdest] = Value::logical(a && b);
                break;
            }
            case Op::OR_SCALAR: {
                int32_t a = as_logical_scalar(regs[in.ra]);
                int32_t b = as_logical_scalar(regs[in.rb]);
                if (a == kNaLogical || b == kNaLogical) regs[in.rdest] = Value::logical(kNaLogical);
                else regs[in.rdest] = Value::logical(a || b);
                break;
            }
            case Op::AND: {
                // short-circuit &&
                int32_t a = as_logical_scalar(regs[in.ra]);
                if (a == 0) { regs[in.rdest] = Value::logical(0); break; }
                if (a == kNaLogical) { regs[in.rdest] = Value::logical(kNaLogical); break; }
                int32_t b = as_logical_scalar(regs[in.rb]);
                regs[in.rdest] = Value::logical(b);
                break;
            }
            case Op::OR: {
                int32_t a = as_logical_scalar(regs[in.ra]);
                if (a != 0 && a != kNaLogical) { regs[in.rdest] = Value::logical(1); break; }
                if (a == kNaLogical) { regs[in.rdest] = Value::logical(kNaLogical); break; }
                int32_t b = as_logical_scalar(regs[in.rb]);
                regs[in.rdest] = Value::logical(b);
                break;
            }
            case Op::NOT: {
                int32_t a = as_logical_scalar(regs[in.ra]);
                if (a == kNaLogical) regs[in.rdest] = Value::logical(kNaLogical);
                else regs[in.rdest] = Value::logical(!a);
                break;
            }

            case Op::MAKE_SEQ: {
                Value a = force_if_promise(regs[in.ra]);
                Value b = force_if_promise(regs[in.rb]);
                if (a.is_integer() && b.is_integer()) {
                    Vector* v = Vector::range(a.as_integer(), b.as_integer());
                    regs[in.rdest] = Value::from_heap(TypeTag::kVector, v);
                } else {
                    double av = a.is_real() ? a.as_real() : (double)a.as_integer();
                    double bv = b.is_real() ? b.as_real() : (double)b.as_integer();
                    int64_t n = (int64_t)(bv - av + 1);
                    Vector* v = new Vector(VectorType::kReal, n > 0 ? (size_t)n : 0);
                    for (int64_t i = 0; i < n; ++i)
                        v->set_real((size_t)i, av + (double)i);
                    regs[in.rdest] = Value::from_heap(TypeTag::kVector, v);
                }
                break;
            }

            case Op::LENGTH: {
                Value v = force_if_promise(regs[in.ra]);
                if (v.is_vector()) regs[in.rdest] = Value::integer((int32_t)v.as_vector()->length());
                else if (v.is_nil()) regs[in.rdest] = Value::integer(0);
                else regs[in.rdest] = Value::integer(1);
                break;
            }

            case Op::INDEX:    regs[in.rdest] = index_value(regs[in.ra], regs[in.rb]); break;
            case Op::INDEX2:   regs[in.rdest] = index2_value(regs[in.ra], regs[in.rb]); break;
            case Op::INDEX_NAMED: regs[in.rdest] = dollar_value(regs[in.ra], in.k); break;

            case Op::CALL: {
                Value callee = regs[in.ra];
                Value* args = &regs[in.ra + 1];
                Value result = slow_call(callee, args, in.k, env);
                regs[in.rdest] = result;
                if (callee.is_heap())
                    ctx_.feedback().call(fn, frame.pc).record(callee.as_heap());
                break;
            }

            case Op::MAKE_CLOSURE: {
                BytecodeFunction* child = fn->constants[in.k].as_bytecode_fn();
                Closure* c = new Closure(child, env);
                regs[in.rdest] = Value::from_heap(TypeTag::kClosure, c);
                break;
            }

            case Op::RETURN:
                return regs[in.ra];

            case Op::RETURN_NULL:
                return Value::nil();

            case Op::LOOP_HEADER: {
                // OSR check: if loop iteration count exceeds threshold,
                // trigger tier-up.
                auto& iter = ctx_.feedback().loop_iter(fn, frame.pc);
                uint64_t n = iter.fetch_add(1, std::memory_order_relaxed);
                if (n == FeedbackEngine::kOSRThreshold) {
                    // For now, just continue interpreting. The JIT
                    // tier manager will pick this up on the next
                    // call to the function.
                    // TODO: trigger OSR here.
                }
                break;
            }
            case Op::LOOP_BACKEDGE: break;

            case Op::PRINT: {
                Value v = force_if_promise(regs[in.ra]);
                if (v.is_real()) std::printf("[1] %g\n", v.as_real());
                else if (v.is_integer()) std::printf("[1] %d\n", v.as_integer());
                else if (v.is_logical()) std::printf("[1] %s\n", v.as_logical() == 1 ? "TRUE" : (v.as_logical() == 0 ? "FALSE" : "NA"));
                else if (v.is_nil()) std::printf("NULL\n");
                else if (v.is_vector()) {
                    Vector* vec = v.as_vector();
                    if (vec->vtype() == VectorType::kReal) {
                        for (size_t i = 0; i < vec->length(); ++i)
                            std::printf("[%zu] %g\n", i+1, vec->real_at(i));
                    } else if (vec->vtype() == VectorType::kInteger) {
                        for (size_t i = 0; i < vec->length(); ++i)
                            std::printf("[%zu] %d\n", i+1, vec->integer_at(i));
                    }
                }
                break;
            }

            case Op::DUMP:
                std::printf("--- register dump ---\n");
                for (uint32_t i = 0; i < fn->nregs; ++i) {
                    Value v = regs[i];
                    std::printf("  r%u: %s\n", i, tag_name(v.tag()));
                }
                break;

            case Op::GUARD_TYPE:
                if (regs[in.ra].tag() != static_cast<TypeTag>(in.k)) {
                    ctx_.raise_error("deopt: type guard failed");
                }
                break;

            case Op::DEOPT:
                ctx_.raise_error("deopt: not implemented in interpreter");
                break;

            default:
                ctx_.raise_error(std::string("interpreter: unhandled op ") + op_name(in.op));
        }
        frame.pc = next_pc;
    }
exit_loop:
    return regs[0];
}

Value Interpreter::call_function(Value callee, Value* args, uint32_t nargs, Environment* caller_env) {
    return slow_call(callee, args, nargs, caller_env);
}

}  // namespace rjit
