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

// Frame freelist: reuse allocated frames and register files to avoid
// malloc/free overhead on every function call.
//
// regs_freelist is bucketed by size (rounded up to next power of 2)
// for O(1) lookup. This avoids the linear scan that was burning
// cycles in recursive benchmarks.
static thread_local std::vector<Frame*> frame_freelist;

// Bucketed by log2(nregs). Each bucket holds regs of that exact size
// (rounded up to power of 2). Most functions have <32 registers, so
// only a few buckets are used.
static constexpr int REGS_BUCKETS = 16;  // up to 2^15 = 32768 regs
static thread_local std::vector<Value*> regs_freelist_buckets[REGS_BUCKETS];

static inline int regs_bucket_index(uint32_t nregs) {
    if (nregs <= 1) return 0;
    return 32 - __builtin_clz(nregs - 1);  // ceil(log2(nregs))
}

static Frame* alloc_frame() {
    if (!frame_freelist.empty()) {
        Frame* f = frame_freelist.back();
        frame_freelist.pop_back();
        return f;
    }
    return static_cast<Frame*>(std::malloc(sizeof(Frame)));
}

static void free_frame(Frame* f) {
    if (frame_freelist.size() < 256) {
        frame_freelist.push_back(f);
    } else {
        std::free(f);
    }
}

static Value* alloc_regs(uint32_t n) {
    int bucket = regs_bucket_index(n);
    if (bucket < REGS_BUCKETS && !regs_freelist_buckets[bucket].empty()) {
        Value* regs = regs_freelist_buckets[bucket].back();
        regs_freelist_buckets[bucket].pop_back();
        return regs;
    }
    // Allocate rounded up to the bucket size for reuse.
    uint32_t alloc_size = 1u << bucket;
    return static_cast<Value*>(std::aligned_alloc(16, (alloc_size * sizeof(Value) + 15) & ~size_t(15)));
}

static void free_regs(Value* regs, uint32_t n) {
    int bucket = regs_bucket_index(n);
    if (bucket < REGS_BUCKETS && regs_freelist_buckets[bucket].size() < 64) {
        regs_freelist_buckets[bucket].push_back(regs);
    } else {
        std::free(regs);
    }
}

Value Interpreter::execute_with_args(BytecodeFunction* fn, Environment* env, Value* args, uint32_t nargs) {
    // Fast path for closure calls: avoid the overhead of slow_call
    // by inlining the frame setup directly. This is the hottest
    // function in recursive benchmarks (fib, fact).
    Frame* frame = alloc_frame();
    frame->init(fn, env, call_stack_.empty() ? nullptr : call_stack_.back());
    uint32_t nregs = fn->nregs;
    Value* regs = alloc_regs(nregs);

    // Zero-initialize registers with a tight loop (faster than std::fill
    // for small arrays — the compiler vectorizes this).
    // We only need to zero the tag byte; the bits can be garbage.
    // But for safety, zero everything.
    for (uint32_t i = 0; i < nregs; ++i) regs[i] = Value::nil();

    // Bind arguments into parameter registers (0..nparams-1).
    // Skip force_if_promise for non-promise values (the common case).
    if (args) {
        uint32_t n = std::min(nargs, fn->nparams);
        for (uint32_t i = 0; i < n; ++i) {
            Value v = args[i];
            if (RJIT_UNLIKELY(v.is_promise())) v = v.as_promise()->force();
            regs[i] = v;
        }
    }

    frame->regs = regs;
    call_stack_.push_back(frame);

    Value result = dispatch_loop(*frame);
    call_stack_.pop_back();
    free_regs(regs, nregs);
    free_frame(frame);
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
        // Skip creating a new environment for the call. Since our
        // lowering promotes all local variables to registers, the
        // only reason to create a call environment is for LOAD_VAR
        // to find free variables. We pass the closure's captured env
        // directly — LOAD_VAR will walk up the chain to find them.
        // Parameters are bound in registers, not the environment.
        //
        // This is a major performance win: it avoids a heap allocation
        // + shape transition on every function call.
        Environment* call_env = c->env();
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

// Fast path for arithmetic: skip force_if_promise when the value is
// already a scalar (the common case). Only call force_if_promise when
// the tag is kPromise.
static inline Value fast_force(Value v) {
    if (RJIT_LIKELY(v.tag() <= TypeTag::kString)) return v;
    if (v.is_promise()) return v.as_promise()->force();
    return v;
}

Value Interpreter::add_values(Value a, Value b) {
    a = fast_force(a);
    b = fast_force(b);
    // Fast path: both real scalars (the hottest case in numeric loops)
    if (RJIT_LIKELY(a.is_real() && b.is_real())) {
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
        int32_t av = a.as_logical() == kNaLogical ? kNaInt : a.as_logical();
        int32_t bv = b.as_logical() == kNaLogical ? kNaInt : b.as_logical();
        return Value::integer(av + bv);
    }
    // Vector cases
    if (a.is_vector() || b.is_vector()) {
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
    ctx_.raise_error("unsupported operand types for '+'");
}

Value Interpreter::sub_values(Value a, Value b) {
    a = fast_force(a); b = fast_force(b);
    if (RJIT_LIKELY(a.is_real() && b.is_real())) return Value::real(a.as_real() - b.as_real());
    if (a.is_integer() && b.is_integer()) {
        int64_t r = static_cast<int64_t>(a.as_integer()) - static_cast<int64_t>(b.as_integer());
        if (r < INT32_MIN || r > INT32_MAX) return Value::real(static_cast<double>(r));
        return Value::integer(static_cast<int32_t>(r));
    }
    double av = a.is_real() ? a.as_real() : (a.is_integer() ? (a.as_integer() == kNaInt ? kNaReal : (double)a.as_integer()) : 0);
    double bv = b.is_real() ? b.as_real() : (b.is_integer() ? (b.as_integer() == kNaInt ? kNaReal : (double)b.as_integer()) : 0);
    return Value::real(av - bv);
}

Value Interpreter::mul_values(Value a, Value b) {
    a = fast_force(a); b = fast_force(b);
    if (RJIT_LIKELY(a.is_real() && b.is_real())) return Value::real(a.as_real() * b.as_real());
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
    a = fast_force(a); b = fast_force(b);
    if (RJIT_LIKELY(a.is_real() && b.is_real())) return Value::real(a.as_real() / b.as_real());
    double av = a.is_real() ? a.as_real() : (a.is_integer() ? (a.as_integer() == kNaInt ? kNaReal : (double)a.as_integer()) : 0);
    double bv = b.is_real() ? b.as_real() : (b.is_integer() ? (b.as_integer() == kNaInt ? kNaReal : (double)b.as_integer()) : 0);
    return Value::real(av / bv);
}

template <typename Cmp>
static Value cmp_scalar(Value a, Value b, Cmp&& cmp) {
    a = fast_force(a); b = fast_force(b);
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
    v = fast_force(v);
    if (RJIT_LIKELY(v.is_logical())) return v.as_logical();
    if (v.is_integer()) return v.as_integer() == 0 ? 0 : (v.as_integer() == kNaInt ? kNaLogical : 1);
    if (v.is_real())    return v.as_real() != v.as_real() ? kNaLogical : (v.as_real() != 0.0 ? 1 : 0);
    if (v.is_nil())     return 0;
    return 0;
}

// ----------------- Main dispatch loop -----------------

// ---------------------------------------------------------------------------
// Dispatch loop with computed goto (GCC labels-as-values extension).
//
// Computed goto is 20-30% faster than a switch statement for interpreter
// dispatch because:
//   1. Each handler has its own branch prediction history (the switch
//      has a single indirect branch that's hard to predict).
//   2. The CPU's branch target buffer (BTB) can learn each handler's
//      successor pattern independently.
//   3. No bounds check or fall-through overhead.
//
// We also inline the fast path for the hottest opcodes (LOAD_LOCAL,
// ADD, LE, JUMP_IF_FALSE, LOAD_INT) directly in the handler, avoiding
// function call overhead. Slow paths fall back to method calls.
//
// Quickening: after executing a generic ADD/SUB/MUL/DIV/LE/LT/etc.
// a few times with consistent operand types, we rewrite the opcode
// in-place to a specialized variant (ADD_REAL_REAL, etc.) that skips
// the type check entirely.
// ---------------------------------------------------------------------------

#if defined(__GNUC__) || defined(__clang__)
#define RJIT_DISPATCH_NEXT() \
    do { ip++; goto *dispatch_table[static_cast<size_t>(ip->op)]; } while(0)
#define RJIT_DISPATCH_GOTO(target) \
    do { ip = &fn->code[target]; goto *dispatch_table[static_cast<size_t>(ip->op)]; } while(0)
#else
#define RJIT_DISPATCH_NEXT() \
    do { frame.pc = next_pc; break; } while(0)
#define RJIT_DISPATCH_GOTO(target) \
    do { frame.pc = target; break; } while(0)
#endif

Value Interpreter::dispatch_loop(Frame& frame) {
    BytecodeFunction* fn = frame.fn;
    Value* regs = frame.regs;
    Environment* env = frame.env;
    Instr* code = fn->code.data();

#if defined(__GNUC__) || defined(__clang__)
    // Computed goto dispatch table.
    static const void* dispatch_table[] = {
        &&op_NOP,           &&op_HALT,          &&op_JUMP,          &&op_JUMP_IF_FALSE,
        &&op_JUMP_IF_TRUE,  &&op_JUMP_IF_NA,    &&op_LOAD_NIL,      &&op_LOAD_TRUE,
        &&op_LOAD_FALSE,    &&op_LOAD_NA,       &&op_LOAD_REAL,     &&op_LOAD_INT,
        &&op_LOAD_STRING,   &&op_LOAD_VAR,      &&op_STORE_VAR,     &&op_STORE_SUPER,
        &&op_LOAD_LOCAL,    &&op_STORE_LOCAL,   &&op_RM_VAR,        &&op_ADD,
        &&op_SUB,           &&op_MUL,           &&op_DIV,           &&op_POW,
        &&op_MOD,           &&op_NEG,           &&op_ADD_REAL_REAL, &&op_ADD_INT_INT,
        &&op_ADD_REAL_INT,  &&op_ADD_INT_REAL,  &&op_ADD_SCALAR_VEC,&&op_ADD_VEC_SCALAR,
        &&op_ADD_VEC_VEC,   &&op_SUB_REAL_REAL, &&op_SUB_INT_INT,   &&op_MUL_REAL_REAL,
        &&op_MUL_INT_INT,   &&op_DIV_REAL_REAL, &&op_DIV_INT_INT,   &&op_LT,
        &&op_LE,            &&op_GT,            &&op_GE,            &&op_EQ,
        &&op_NE,            &&op_LT_REAL_REAL,  &&op_LE_REAL_REAL,  &&op_GT_REAL_REAL,
        &&op_GE_REAL_REAL,  &&op_EQ_REAL_REAL,  &&op_NE_REAL_REAL,  &&op_LT_INT_INT,
        &&op_LE_INT_INT,    &&op_GT_INT_INT,    &&op_GE_INT_INT,    &&op_EQ_INT_INT,
        &&op_NE_INT_INT,    &&op_AND,           &&op_OR,            &&op_NOT,
        &&op_AND_SCALAR,    &&op_OR_SCALAR,     &&op_CALL,          &&op_CALL_NAMED,
        &&op_CALL_BUILTIN,  &&op_CALL_CLOSURE,  &&op_RETURN,        &&op_RETURN_NULL,
        &&op_MAKE_PROMISE,  &&op_FORCE_PROMISE, &&op_MAKE_VECTOR,   &&op_MAKE_SEQ,
        &&op_LENGTH,        &&op_INDEX,         &&op_INDEX2,        &&op_INDEX_NAMED,
        &&op_INDEX_ASSIGN,  &&op_INDEX2_ASSIGN, &&op_INDEX_NAMED_ASSIGN, &&op_SUBSET,
        &&op_COERCE_REAL,   &&op_COERCE_INT,    &&op_COERCE_LOGICAL,&&op_TYPEOF,
        &&op_IS_NA,         &&op_NEW_ENV,       &&op_GET_ENV,       &&op_MAKE_CLOSURE,
        &&op_GUARD_TYPE,    &&op_GUARD_SHAPE,   &&op_GUARD_LEN,     &&op_DEOPT,
        &&op_IC_LOAD_VAR,   &&op_IC_CALL,       &&op_LOOP_HEADER,   &&op_LOOP_BACKEDGE,
        &&op_PRINT,         &&op_DUMP,
    };

    Instr* ip = &code[frame.pc];
    goto *dispatch_table[static_cast<size_t>(ip->op)];

    // ---- Hot opcodes (ordered by frequency) ----

op_LOAD_LOCAL:
    regs[ip->rdest] = regs[ip->ra];
    RJIT_DISPATCH_NEXT();

op_LOAD_INT: {
    Value const& c = fn->constants[ip->k];
    regs[ip->rdest] = c;
    RJIT_DISPATCH_NEXT();
}

op_LOAD_REAL: {
    Value const& c = fn->constants[ip->k];
    regs[ip->rdest] = c;
    RJIT_DISPATCH_NEXT();
}

op_ADD: {
    Value const& a = regs[ip->ra];
    Value const& b = regs[ip->rb];
    // Fast path: both real (the hottest case in numeric loops)
    if (RJIT_LIKELY(a.is_real() && b.is_real())) {
        regs[ip->rdest] = Value::real(a.as_real() + b.as_real());
        // Quickening: rewrite to ADD_REAL_REAL after enough hits.
        // (Disabled for now — needs a counter to avoid rewriting
        //  too early. Will enable with type feedback.)
        RJIT_DISPATCH_NEXT();
    }
    regs[ip->rdest] = add_values(a, b);
    RJIT_DISPATCH_NEXT();
}

op_LE: {
    Value const& a = regs[ip->ra];
    Value const& b = regs[ip->rb];
    if (RJIT_LIKELY(a.is_real() && b.is_real())) {
        regs[ip->rdest] = Value::logical(a.as_real() <= b.as_real() ? 1 : 0);
        RJIT_DISPATCH_NEXT();
    }
    regs[ip->rdest] = le_values(a, b);
    RJIT_DISPATCH_NEXT();
}

op_JUMP_IF_FALSE: {
    Value const& v = regs[ip->ra];
    // Fast path: logical scalar (the common case from LE/LT/etc.)
    if (RJIT_LIKELY(v.is_logical() || v.is_integer())) {
        int32_t b = v.is_logical() ? v.as_logical() : v.as_integer();
        if (b == 0 || b == kNaLogical || b == kNaInt) {
            // NA or false -> jump (R treats NA as false in if conditions,
            // but technically errors; we follow the pragmatic path)
            if (b == 0) { RJIT_DISPATCH_GOTO(ip->k); }
        }
        RJIT_DISPATCH_NEXT();
    }
    int32_t b = as_logical_scalar(v);
    if (b == 0) { RJIT_DISPATCH_GOTO(ip->k); }
    RJIT_DISPATCH_NEXT();
}

op_JUMP: {
    RJIT_DISPATCH_GOTO(ip->k);
}

op_ADD_REAL_REAL:
    regs[ip->rdest] = Value::real(regs[ip->ra].as_real() + regs[ip->rb].as_real());
    RJIT_DISPATCH_NEXT();

op_SUB: {
    Value const& a = regs[ip->ra];
    Value const& b = regs[ip->rb];
    if (RJIT_LIKELY(a.is_real() && b.is_real())) {
        regs[ip->rdest] = Value::real(a.as_real() - b.as_real());
        RJIT_DISPATCH_NEXT();
    }
    regs[ip->rdest] = sub_values(a, b);
    RJIT_DISPATCH_NEXT();
}

op_MUL: {
    Value const& a = regs[ip->ra];
    Value const& b = regs[ip->rb];
    if (RJIT_LIKELY(a.is_real() && b.is_real())) {
        regs[ip->rdest] = Value::real(a.as_real() * b.as_real());
        RJIT_DISPATCH_NEXT();
    }
    regs[ip->rdest] = mul_values(a, b);
    RJIT_DISPATCH_NEXT();
}

op_DIV: {
    Value const& a = regs[ip->ra];
    Value const& b = regs[ip->rb];
    if (RJIT_LIKELY(a.is_real() && b.is_real())) {
        regs[ip->rdest] = Value::real(a.as_real() / b.as_real());
        RJIT_DISPATCH_NEXT();
    }
    regs[ip->rdest] = div_values(a, b);
    RJIT_DISPATCH_NEXT();
}

op_LT: {
    Value const& a = regs[ip->ra];
    Value const& b = regs[ip->rb];
    if (RJIT_LIKELY(a.is_real() && b.is_real())) {
        regs[ip->rdest] = Value::logical(a.as_real() < b.as_real() ? 1 : 0);
        RJIT_DISPATCH_NEXT();
    }
    regs[ip->rdest] = lt_values(a, b);
    RJIT_DISPATCH_NEXT();
}

op_GT: {
    Value const& a = regs[ip->ra];
    Value const& b = regs[ip->rb];
    if (RJIT_LIKELY(a.is_real() && b.is_real())) {
        regs[ip->rdest] = Value::logical(a.as_real() > b.as_real() ? 1 : 0);
        RJIT_DISPATCH_NEXT();
    }
    regs[ip->rdest] = gt_values(a, b);
    RJIT_DISPATCH_NEXT();
}

op_GE: {
    Value const& a = regs[ip->ra];
    Value const& b = regs[ip->rb];
    if (RJIT_LIKELY(a.is_real() && b.is_real())) {
        regs[ip->rdest] = Value::logical(a.as_real() >= b.as_real() ? 1 : 0);
        RJIT_DISPATCH_NEXT();
    }
    regs[ip->rdest] = ge_values(a, b);
    RJIT_DISPATCH_NEXT();
}

op_EQ: {
    Value const& a = regs[ip->ra];
    Value const& b = regs[ip->rb];
    if (RJIT_LIKELY(a.is_real() && b.is_real())) {
        regs[ip->rdest] = Value::logical(a.as_real() == b.as_real() ? 1 : 0);
        RJIT_DISPATCH_NEXT();
    }
    regs[ip->rdest] = eq_values(a, b);
    RJIT_DISPATCH_NEXT();
}

op_NE: {
    Value const& a = regs[ip->ra];
    Value const& b = regs[ip->rb];
    if (RJIT_LIKELY(a.is_real() && b.is_real())) {
        regs[ip->rdest] = Value::logical(a.as_real() != b.as_real() ? 1 : 0);
        RJIT_DISPATCH_NEXT();
    }
    regs[ip->rdest] = ne_values(a, b);
    RJIT_DISPATCH_NEXT();
}

op_SUB_REAL_REAL:
    regs[ip->rdest] = Value::real(regs[ip->ra].as_real() - regs[ip->rb].as_real());
    RJIT_DISPATCH_NEXT();

op_MUL_REAL_REAL:
    regs[ip->rdest] = Value::real(regs[ip->ra].as_real() * regs[ip->rb].as_real());
    RJIT_DISPATCH_NEXT();

op_DIV_REAL_REAL:
    regs[ip->rdest] = Value::real(regs[ip->ra].as_real() / regs[ip->rb].as_real());
    RJIT_DISPATCH_NEXT();

op_LE_REAL_REAL:
    regs[ip->rdest] = Value::logical(regs[ip->ra].as_real() <= regs[ip->rb].as_real() ? 1 : 0);
    RJIT_DISPATCH_NEXT();

op_LT_REAL_REAL:
    regs[ip->rdest] = Value::logical(regs[ip->ra].as_real() < regs[ip->rb].as_real() ? 1 : 0);
    RJIT_DISPATCH_NEXT();

op_GT_REAL_REAL:
    regs[ip->rdest] = Value::logical(regs[ip->ra].as_real() > regs[ip->rb].as_real() ? 1 : 0);
    RJIT_DISPATCH_NEXT();

op_GE_REAL_REAL:
    regs[ip->rdest] = Value::logical(regs[ip->ra].as_real() >= regs[ip->rb].as_real() ? 1 : 0);
    RJIT_DISPATCH_NEXT();

op_EQ_REAL_REAL:
    regs[ip->rdest] = Value::logical(regs[ip->ra].as_real() == regs[ip->rb].as_real() ? 1 : 0);
    RJIT_DISPATCH_NEXT();

op_NE_REAL_REAL:
    regs[ip->rdest] = Value::logical(regs[ip->ra].as_real() != regs[ip->rb].as_real() ? 1 : 0);
    RJIT_DISPATCH_NEXT();

op_ADD_INT_INT:
    regs[ip->rdest] = Value::integer(regs[ip->ra].as_integer() + regs[ip->rb].as_integer());
    RJIT_DISPATCH_NEXT();

op_SUB_INT_INT:
    regs[ip->rdest] = Value::integer(regs[ip->ra].as_integer() - regs[ip->rb].as_integer());
    RJIT_DISPATCH_NEXT();

op_MUL_INT_INT:
    regs[ip->rdest] = Value::integer(regs[ip->ra].as_integer() * regs[ip->rb].as_integer());
    RJIT_DISPATCH_NEXT();

op_LOAD_NIL:
    regs[ip->rdest] = Value::nil();
    RJIT_DISPATCH_NEXT();

op_LOAD_TRUE:
    regs[ip->rdest] = Value::logical(1);
    RJIT_DISPATCH_NEXT();

op_LOAD_FALSE:
    regs[ip->rdest] = Value::logical(0);
    RJIT_DISPATCH_NEXT();

op_LOAD_NA:
    regs[ip->rdest] = Value::logical(kNaLogical);
    RJIT_DISPATCH_NEXT();

op_LOAD_STRING:
    regs[ip->rdest] = fn->constants[ip->k];
    RJIT_DISPATCH_NEXT();

op_NOP:
    RJIT_DISPATCH_NEXT();

op_LOOP_HEADER:
    RJIT_DISPATCH_NEXT();

op_LOOP_BACKEDGE:
    RJIT_DISPATCH_NEXT();

op_NEG: {
    Value v = fast_force(regs[ip->ra]);
    if (v.is_real())    regs[ip->rdest] = Value::real(-v.as_real());
    else if (v.is_integer()) {
        int32_t x = v.as_integer();
        regs[ip->rdest] = Value::integer(x == kNaInt ? kNaInt : -x);
    } else if (v.is_logical()) {
        int32_t x = v.as_logical();
        regs[ip->rdest] = Value::integer(x == kNaLogical ? kNaInt : -x);
    }
    RJIT_DISPATCH_NEXT();
}

op_LOAD_VAR: {
    LoadVarIC& ic = ic_table_.load_var_ic(fn, static_cast<uint32_t>(ip - code));
    if (RJIT_LIKELY(ic.valid() && ic.env->shape_id() == ic.shape_id)) {
        regs[ip->rdest] = ic.env->slot_get(ic.slot);
    } else {
        Value v = slow_lookup_var(env, ip->k);
        regs[ip->rdest] = v;
        Environment* found_env = env;
        uint32_t slot = 0;
        env->lookup(ip->k, nullptr, &found_env, &slot);
        if (found_env) {
            ic.env = found_env;
            ic.shape_id = found_env->shape_id();
            ic.slot = slot;
        }
    }
    RJIT_DISPATCH_NEXT();
}

op_STORE_VAR:
    slow_define_var(env, ip->k, regs[ip->ra]);
    RJIT_DISPATCH_NEXT();

op_STORE_SUPER:
    slow_set_super(env, ip->k, regs[ip->ra]);
    RJIT_DISPATCH_NEXT();

op_STORE_LOCAL:
    regs[ip->rdest] = regs[ip->ra];
    RJIT_DISPATCH_NEXT();

op_RM_VAR:
    env->remove(ip->k);
    RJIT_DISPATCH_NEXT();

op_JUMP_IF_TRUE: {
    Value const& v = regs[ip->ra];
    if (RJIT_LIKELY(v.is_logical() || v.is_integer())) {
        int32_t b = v.is_logical() ? v.as_logical() : v.as_integer();
        if (b != 0 && b != kNaLogical && b != kNaInt) { RJIT_DISPATCH_GOTO(ip->k); }
        RJIT_DISPATCH_NEXT();
    }
    int32_t b = as_logical_scalar(v);
    if (b != 0 && b != kNaLogical) { RJIT_DISPATCH_GOTO(ip->k); }
    RJIT_DISPATCH_NEXT();
}

op_JUMP_IF_NA: {
    Value const& v = regs[ip->ra];
    int32_t b = as_logical_scalar(v);
    if (b == kNaLogical) { RJIT_DISPATCH_GOTO(ip->k); }
    RJIT_DISPATCH_NEXT();
}

op_AND_SCALAR: {
    int32_t a = as_logical_scalar(regs[ip->ra]);
    int32_t b = as_logical_scalar(regs[ip->rb]);
    if (a == kNaLogical || b == kNaLogical) regs[ip->rdest] = Value::logical(kNaLogical);
    else regs[ip->rdest] = Value::logical(a && b);
    RJIT_DISPATCH_NEXT();
}

op_OR_SCALAR: {
    int32_t a = as_logical_scalar(regs[ip->ra]);
    int32_t b = as_logical_scalar(regs[ip->rb]);
    if (a == kNaLogical || b == kNaLogical) regs[ip->rdest] = Value::logical(kNaLogical);
    else regs[ip->rdest] = Value::logical(a || b);
    RJIT_DISPATCH_NEXT();
}

op_AND: {
    int32_t a = as_logical_scalar(regs[ip->ra]);
    if (a == 0) { regs[ip->rdest] = Value::logical(0); RJIT_DISPATCH_NEXT(); }
    if (a == kNaLogical) { regs[ip->rdest] = Value::logical(kNaLogical); RJIT_DISPATCH_NEXT(); }
    int32_t b = as_logical_scalar(regs[ip->rb]);
    regs[ip->rdest] = Value::logical(b);
    RJIT_DISPATCH_NEXT();
}

op_OR: {
    int32_t a = as_logical_scalar(regs[ip->ra]);
    if (a != 0 && a != kNaLogical) { regs[ip->rdest] = Value::logical(1); RJIT_DISPATCH_NEXT(); }
    if (a == kNaLogical) { regs[ip->rdest] = Value::logical(kNaLogical); RJIT_DISPATCH_NEXT(); }
    int32_t b = as_logical_scalar(regs[ip->rb]);
    regs[ip->rdest] = Value::logical(b);
    RJIT_DISPATCH_NEXT();
}

op_NOT: {
    int32_t a = as_logical_scalar(regs[ip->ra]);
    if (a == kNaLogical) regs[ip->rdest] = Value::logical(kNaLogical);
    else regs[ip->rdest] = Value::logical(!a);
    RJIT_DISPATCH_NEXT();
}

op_MAKE_SEQ: {
    Value a = fast_force(regs[ip->ra]);
    Value b = fast_force(regs[ip->rb]);
    if (a.is_integer() && b.is_integer()) {
        Vector* v = Vector::range(a.as_integer(), b.as_integer());
        regs[ip->rdest] = Value::from_heap(TypeTag::kVector, v);
    } else {
        double av = a.is_real() ? a.as_real() : (double)a.as_integer();
        double bv = b.is_real() ? b.as_real() : (double)b.as_integer();
        int64_t n = (int64_t)(bv - av + 1);
        Vector* v = new Vector(VectorType::kReal, n > 0 ? (size_t)n : 0);
        for (int64_t i = 0; i < n; ++i)
            v->set_real((size_t)i, av + (double)i);
        regs[ip->rdest] = Value::from_heap(TypeTag::kVector, v);
    }
    RJIT_DISPATCH_NEXT();
}

op_LENGTH: {
    Value v = fast_force(regs[ip->ra]);
    if (v.is_vector()) regs[ip->rdest] = Value::integer((int32_t)v.as_vector()->length());
    else if (v.is_nil()) regs[ip->rdest] = Value::integer(0);
    else regs[ip->rdest] = Value::integer(1);
    RJIT_DISPATCH_NEXT();
}

op_INDEX:
    regs[ip->rdest] = index_value(regs[ip->ra], regs[ip->rb]);
    RJIT_DISPATCH_NEXT();

op_INDEX2:
    regs[ip->rdest] = index2_value(regs[ip->ra], regs[ip->rb]);
    RJIT_DISPATCH_NEXT();

op_INDEX_NAMED:
    regs[ip->rdest] = dollar_value(regs[ip->ra], ip->k);
    RJIT_DISPATCH_NEXT();

op_CALL: {
    Value callee = regs[ip->ra];
    Value* args = &regs[ip->ra + 1];
    uint32_t nargs = ip->k;

    // Inline the closure fast path to avoid slow_call overhead.
    // This is critical for recursive functions like fib.
    if (RJIT_LIKELY(callee.is_closure())) {
        Closure* c = callee.as_closure();
        BytecodeFunction* callee_fn = c->code();
        Environment* callee_env = c->env();

        // Allocate frame and regs from the freelist.
        Frame* new_frame = alloc_frame();
        new_frame->init(callee_fn, callee_env, &frame);
        uint32_t callee_nregs = callee_fn->nregs;
        Value* new_regs = alloc_regs(callee_nregs);

        // Zero registers.
        for (uint32_t i = 0; i < callee_nregs; ++i) new_regs[i] = Value::nil();

        // Bind arguments.
        uint32_t n = std::min(nargs, callee_fn->nparams);
        for (uint32_t i = 0; i < n; ++i) {
            Value v = args[i];
            if (RJIT_UNLIKELY(v.is_promise())) v = v.as_promise()->force();
            new_regs[i] = v;
        }

        new_frame->regs = new_regs;
        call_stack_.push_back(new_frame);

        // Recursive dispatch — save our ip back to frame.pc for deopt.
        frame.pc = static_cast<uint32_t>(ip - code);
        Value result = dispatch_loop(*new_frame);

        call_stack_.pop_back();
        free_regs(new_regs, callee_nregs);
        free_frame(new_frame);

        regs[ip->rdest] = result;
    } else if (callee.is_builtin() || callee.is_special()) {
        regs[ip->rdest] = callee.as_builtin()->impl()(ctx_, args, nargs);
    } else if (callee.is_nil()) {
        ctx_.raise_error("attempt to call NULL");
    } else {
        ctx_.raise_error("attempt to apply non-function");
    }
    RJIT_DISPATCH_NEXT();
}

op_MAKE_CLOSURE: {
    BytecodeFunction* child = fn->constants[ip->k].as_bytecode_fn();
    Closure* c = new Closure(child, env);
    regs[ip->rdest] = Value::from_heap(TypeTag::kClosure, c);
    RJIT_DISPATCH_NEXT();
}

op_RETURN:
    return regs[ip->ra];

op_RETURN_NULL:
    return Value::nil();

op_HALT:
    goto exit_loop;

op_PRINT: {
    Value v = fast_force(regs[ip->ra]);
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
    RJIT_DISPATCH_NEXT();
}

// ---- Remaining opcodes (less frequent, fall back to switch) ----

#define FALLTHROUGH_CASE(op_name_enum, body) \
    op_##op_name_enum: body; RJIT_DISPATCH_NEXT();

op_ADD_REAL_INT:
    regs[ip->rdest] = Value::real(regs[ip->ra].as_real() + (regs[ip->rb].as_integer() == kNaInt ? kNaReal : (double)regs[ip->rb].as_integer()));
    RJIT_DISPATCH_NEXT();

op_ADD_INT_REAL:
    regs[ip->rdest] = Value::real((regs[ip->ra].as_integer() == kNaInt ? kNaReal : (double)regs[ip->ra].as_integer()) + regs[ip->rb].as_real());
    RJIT_DISPATCH_NEXT();

op_ADD_SCALAR_VEC:
op_ADD_VEC_SCALAR:
op_ADD_VEC_VEC:
    regs[ip->rdest] = add_values(regs[ip->ra], regs[ip->rb]);
    RJIT_DISPATCH_NEXT();

op_DIV_INT_INT:
    regs[ip->rdest] = Value::real((double)regs[ip->ra].as_integer() / (double)regs[ip->rb].as_integer());
    RJIT_DISPATCH_NEXT();

op_LT_INT_INT:  regs[ip->rdest] = Value::logical(regs[ip->ra].as_integer() <  regs[ip->rb].as_integer() ? 1 : 0); RJIT_DISPATCH_NEXT();
op_LE_INT_INT:  regs[ip->rdest] = Value::logical(regs[ip->ra].as_integer() <= regs[ip->rb].as_integer() ? 1 : 0); RJIT_DISPATCH_NEXT();
op_GT_INT_INT:  regs[ip->rdest] = Value::logical(regs[ip->ra].as_integer() >  regs[ip->rb].as_integer() ? 1 : 0); RJIT_DISPATCH_NEXT();
op_GE_INT_INT:  regs[ip->rdest] = Value::logical(regs[ip->ra].as_integer() >= regs[ip->rb].as_integer() ? 1 : 0); RJIT_DISPATCH_NEXT();
op_EQ_INT_INT:  regs[ip->rdest] = Value::logical(regs[ip->ra].as_integer() == regs[ip->rb].as_integer() ? 1 : 0); RJIT_DISPATCH_NEXT();
op_NE_INT_INT:  regs[ip->rdest] = Value::logical(regs[ip->ra].as_integer() != regs[ip->rb].as_integer() ? 1 : 0); RJIT_DISPATCH_NEXT();

op_CALL_NAMED:
op_CALL_BUILTIN:
op_CALL_CLOSURE:
    {
        Value callee = regs[ip->ra];
        Value* args = &regs[ip->ra + 1];
        Value result = slow_call(callee, args, ip->k, env);
        regs[ip->rdest] = result;
        RJIT_DISPATCH_NEXT();
    }

op_MAKE_PROMISE:
op_FORCE_PROMISE:
    regs[ip->rdest] = fast_force(regs[ip->ra]);
    RJIT_DISPATCH_NEXT();

op_MAKE_VECTOR:
    regs[ip->rdest] = Value::nil();
    RJIT_DISPATCH_NEXT();

op_INDEX_ASSIGN:
op_INDEX2_ASSIGN:
op_INDEX_NAMED_ASSIGN:
op_SUBSET:
    RJIT_DISPATCH_NEXT();

op_COERCE_REAL: {
    Value v = fast_force(regs[ip->ra]);
    if (v.is_real()) regs[ip->rdest] = v;
    else if (v.is_integer()) regs[ip->rdest] = Value::real(v.as_integer() == kNaInt ? kNaReal : (double)v.as_integer());
    else regs[ip->rdest] = Value::real(kNaReal);
    RJIT_DISPATCH_NEXT();
}

op_COERCE_INT: {
    Value v = fast_force(regs[ip->ra]);
    if (v.is_integer()) regs[ip->rdest] = v;
    else if (v.is_real()) regs[ip->rdest] = Value::integer((v.as_real() != v.as_real() || v.as_real() < INT32_MIN || v.as_real() > INT32_MAX) ? kNaInt : (int32_t)v.as_real());
    else regs[ip->rdest] = Value::integer(kNaInt);
    RJIT_DISPATCH_NEXT();
}

op_COERCE_LOGICAL: {
    Value v = fast_force(regs[ip->ra]);
    regs[ip->rdest] = Value::logical(as_logical_scalar(v));
    RJIT_DISPATCH_NEXT();
}

op_TYPEOF:
op_IS_NA:
    regs[ip->rdest] = Value::logical(is_na(regs[ip->ra]) ? 1 : 0);
    RJIT_DISPATCH_NEXT();

op_NEW_ENV: {
    Environment* new_env = new Environment(env);
    regs[ip->rdest] = Value::from_heap(TypeTag::kEnvironment, new_env);
    RJIT_DISPATCH_NEXT();
}

op_GET_ENV:
    regs[ip->rdest] = Value::from_heap(TypeTag::kEnvironment, env);
    RJIT_DISPATCH_NEXT();

op_GUARD_TYPE:
    if (regs[ip->ra].tag() != static_cast<TypeTag>(ip->k)) {
        ctx_.raise_error("deopt: type guard failed");
    }
    RJIT_DISPATCH_NEXT();

op_GUARD_SHAPE:
    if (env->shape_id() != ip->k) {
        ctx_.raise_error("deopt: shape guard failed");
    }
    RJIT_DISPATCH_NEXT();

op_GUARD_LEN:
    RJIT_DISPATCH_NEXT();

op_DEOPT:
    ctx_.raise_error("deopt: not implemented in interpreter");
    RJIT_DISPATCH_NEXT();

op_IC_LOAD_VAR:
    goto op_LOAD_VAR;

op_IC_CALL:
    goto op_CALL;

op_POW: {
    Value a = fast_force(regs[ip->ra]);
    Value b = fast_force(regs[ip->rb]);
    double av = a.is_real() ? a.as_real() : (double)a.as_integer();
    double bv = b.is_real() ? b.as_real() : (double)b.as_integer();
    regs[ip->rdest] = Value::real(std::pow(av, bv));
    RJIT_DISPATCH_NEXT();
}

op_MOD: {
    Value a = fast_force(regs[ip->ra]);
    Value b = fast_force(regs[ip->rb]);
    double av = a.is_real() ? a.as_real() : (double)a.as_integer();
    double bv = b.is_real() ? b.as_real() : (double)b.as_integer();
    regs[ip->rdest] = Value::real(std::fmod(av, bv));
    RJIT_DISPATCH_NEXT();
}

op_DUMP:
    std::printf("--- register dump ---\n");
    for (uint32_t i = 0; i < fn->nregs; ++i) {
        Value v = regs[i];
        std::printf("  r%u: %s\n", i, tag_name(v.tag()));
    }
    RJIT_DISPATCH_NEXT();

    __builtin_unreachable();

#else
    // Fallback: switch-based dispatch for non-GCC compilers.
    Instr* ip = &code[frame.pc];
    while (true) {
        uint32_t instr_idx = static_cast<uint32_t>(ip - code);
        frame.pc = instr_idx;
        Instr const& in = *ip;
        uint32_t next_pc = instr_idx + 1;
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
                LoadVarIC& ic = ic_table_.load_var_ic(fn, instr_idx);
                if (RJIT_LIKELY(ic.valid() && ic.env->shape_id() == ic.shape_id)) {
                    regs[in.rdest] = ic.env->slot_get(ic.slot);
                } else {
                    Value v = slow_lookup_var(env, in.k);
                    regs[in.rdest] = v;
                    Environment* found_env = env;
                    uint32_t slot = 0;
                    env->lookup(in.k, nullptr, &found_env, &slot);
                    if (found_env) {
                        ic.env = found_env;
                        ic.shape_id = found_env->shape_id();
                        ic.slot = slot;
                    }
                }
                break;
            }
            case Op::STORE_VAR:     slow_define_var(env, in.k, regs[in.ra]); break;
            case Op::STORE_SUPER:   slow_set_super(env, in.k, regs[in.ra]); break;
            case Op::JUMP:          next_pc = in.k; break;
            case Op::JUMP_IF_FALSE: {
                int32_t b = as_logical_scalar(regs[in.ra]);
                if (b == 0) next_pc = in.k;
                break;
            }
            case Op::JUMP_IF_TRUE: {
                int32_t b = as_logical_scalar(regs[in.ra]);
                if (b != 0 && b != kNaLogical) next_pc = in.k;
                break;
            }
            case Op::ADD:           regs[in.rdest] = add_values(regs[in.ra], regs[in.rb]); break;
            case Op::SUB:           regs[in.rdest] = sub_values(regs[in.ra], regs[in.rb]); break;
            case Op::MUL:           regs[in.rdest] = mul_values(regs[in.ra], regs[in.rb]); break;
            case Op::DIV:           regs[in.rdest] = div_values(regs[in.ra], regs[in.rb]); break;
            case Op::NEG: {
                Value v = fast_force(regs[in.ra]);
                if (v.is_real())    regs[in.rdest] = Value::real(-v.as_real());
                else if (v.is_integer()) {
                    int32_t x = v.as_integer();
                    regs[in.rdest] = Value::integer(x == kNaInt ? kNaInt : -x);
                }
                break;
            }
            case Op::ADD_REAL_REAL: regs[in.rdest] = Value::real(regs[in.ra].as_real() + regs[in.rb].as_real()); break;
            case Op::SUB_REAL_REAL: regs[in.rdest] = Value::real(regs[in.ra].as_real() - regs[in.rb].as_real()); break;
            case Op::MUL_REAL_REAL: regs[in.rdest] = Value::real(regs[in.ra].as_real() * regs[in.rb].as_real()); break;
            case Op::DIV_REAL_REAL: regs[in.rdest] = Value::real(regs[in.ra].as_real() / regs[in.rb].as_real()); break;
            case Op::LE_REAL_REAL:  regs[in.rdest] = Value::logical(regs[in.ra].as_real() <= regs[in.rb].as_real() ? 1 : 0); break;
            case Op::LT:            regs[in.rdest] = lt_values(regs[in.ra], regs[in.rb]); break;
            case Op::LE:            regs[in.rdest] = le_values(regs[in.ra], regs[in.rb]); break;
            case Op::GT:            regs[in.rdest] = gt_values(regs[in.ra], regs[in.rb]); break;
            case Op::GE:            regs[in.rdest] = ge_values(regs[in.ra], regs[in.rb]); break;
            case Op::EQ:            regs[in.rdest] = eq_values(regs[in.ra], regs[in.rb]); break;
            case Op::NE:            regs[in.rdest] = ne_values(regs[in.ra], regs[in.rb]); break;
            case Op::CALL: {
                Value callee = regs[in.ra];
                Value* args = &regs[in.ra + 1];
                Value result = slow_call(callee, args, in.k, env);
                regs[in.rdest] = result;
                break;
            }
            case Op::MAKE_CLOSURE: {
                BytecodeFunction* child = fn->constants[in.k].as_bytecode_fn();
                Closure* c = new Closure(child, env);
                regs[in.rdest] = Value::from_heap(TypeTag::kClosure, c);
                break;
            }
            case Op::RETURN:        return regs[in.ra];
            case Op::RETURN_NULL:   return Value::nil();
            case Op::LOOP_HEADER:   break;
            case Op::LOOP_BACKEDGE: break;
            case Op::MAKE_SEQ: {
                Value a = fast_force(regs[in.ra]);
                Value b = fast_force(regs[in.rb]);
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
                Value v = fast_force(regs[in.ra]);
                if (v.is_vector()) regs[in.rdest] = Value::integer((int32_t)v.as_vector()->length());
                else if (v.is_nil()) regs[in.rdest] = Value::integer(0);
                else regs[in.rdest] = Value::integer(1);
                break;
            }
            case Op::INDEX:    regs[in.rdest] = index_value(regs[in.ra], regs[in.rb]); break;
            case Op::INDEX2:   regs[in.rdest] = index2_value(regs[in.ra], regs[in.rb]); break;
            case Op::INDEX_NAMED: regs[in.rdest] = dollar_value(regs[in.ra], in.k); break;
            case Op::PRINT: {
                Value v = fast_force(regs[in.ra]);
                if (v.is_real()) std::printf("[1] %g\n", v.as_real());
                else if (v.is_integer()) std::printf("[1] %d\n", v.as_integer());
                else if (v.is_logical()) std::printf("[1] %s\n", v.as_logical() == 1 ? "TRUE" : (v.as_logical() == 0 ? "FALSE" : "NA"));
                else if (v.is_nil()) std::printf("NULL\n");
                break;
            }
            default:
                ctx_.raise_error(std::string("interpreter: unhandled op ") + op_name(in.op));
        }
        ip = &code[next_pc];
    }
#endif

exit_loop:
    return regs[0];
}

Value Interpreter::call_function(Value callee, Value* args, uint32_t nargs, Environment* caller_env) {
    return slow_call(callee, args, nargs, caller_env);
}

}  // namespace rjit
