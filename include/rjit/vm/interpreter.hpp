// rjit/vm/interpreter.hpp - Register-based superinstruction interpreter (T0)
//
// The interpreter is the foundation of the entire system. It must be
// fast enough that we don't need to JIT-compile every function,
// while still recording feedback that lets the JIT produce good
// code.
//
// Design:
//   * Threaded dispatch: each opcode handler ends by tail-calling
//     the next handler (via a function pointer table).
//   * Register-pinned VM state: on x86-64, we keep the current
//     Frame*, the current Environment*, and the GC pointer in
//     callee-saved registers. (For portability we don't actually
//     pin them in C++; instead we use local variables that the
//     compiler tends to keep in registers.)
//   * Per-instruction feedback recording is gated by a counter
//     (one decrement per instruction), so feedback is sampled
//     rather than recorded every time.
//   * OSR points are explicit at LOOP_HEADER instructions.

#pragma once
#include <cstdint>
#include "rjit/core/context.hpp"
#include "rjit/core/value.hpp"
#include "rjit/core/environment.hpp"
#include "rjit/bytecode/module.hpp"
#include "rjit/vm/frame.hpp"
#include "rjit/vm/inline_cache.hpp"
#include "rjit/feedback/feedback.hpp"

namespace rjit {

class Value;
class Closure;
class Builtin;
class Ast;

class Interpreter {
public:
    explicit Interpreter(Context& ctx);

    // Evaluate an AST in the given environment. Used for top-level
    // execution and for forcing promises.
    Value eval(Ast const& ast, Environment* env);

    // Execute a BytecodeFunction in the given environment. The
    // arguments are already bound in `env` (as parameter slots).
    // Returns the return value.
    Value execute(BytecodeFunction* fn, Environment* env);

    // Execute a BytecodeFunction with arguments pre-bound into
    // registers 0..nparams-1. Used by closure calls.
    Value execute_with_args(BytecodeFunction* fn, Environment* env, Value* args, uint32_t nargs);

    // Execute a function call: bind arguments, execute, return value.
    Value call_function(Value callee, Value* args, uint32_t nargs, Environment* caller_env);

    // Get the IC table (for the JIT to read)
    InlineCacheTable& ic_table() noexcept { return ic_table_; }

    // Frame pool (for OSR / deopt to construct frames)
    FramePool& frame_pool() noexcept { return frame_pool_; }

    Context& context() noexcept { return ctx_; }

    ~Interpreter() {
        frame_pool_.reset();
    }

private:
    Context&            ctx_;
    InlineCacheTable    ic_table_;
    FramePool           frame_pool_;

    // Pre-allocated frame stack — raw array, no vector overhead.
    // CALL just increments frame_depth_ and writes to the next slot.
    // RETURN decrements frame_depth_. No malloc, no capacity checks,
    // no pointer chasing. Sized for deep recursion (4096 levels).
    static constexpr size_t kFrameStackCapacity = 4096;
    std::unique_ptr<Frame[]> frame_stack_;
    size_t frame_depth_ = 0;

    // Pre-allocated register arena — a single large array of Values.
    // CALL bumps reg_sp_ by callee_nregs; RETURN decrements. No malloc,
    // no freelist, no bucket lookup. Just pointer arithmetic.
    // 256K Values = 4MB, enough for 4096 frames * 64 regs each.
    static constexpr size_t kRegArenaSize = 262144;
    std::unique_ptr<Value[]> reg_arena_;
    size_t reg_sp_ = 0;

    // Dispatch loop. Returns the value left in r0.
    Value dispatch_loop(Frame& frame);

    // Slow paths
    Value slow_call(Value callee, Value* args, uint32_t nargs, Environment* caller_env);
    Value slow_lookup_var(Environment* env, uint32_t sym_id);
    void  slow_define_var(Environment* env, uint32_t sym_id, Value v);
    void  slow_set_super(Environment* env, uint32_t sym_id, Value v);

    // Helper: arithmetic on two Values (returns the result).
    Value add_values(Value a, Value b);
    Value sub_values(Value a, Value b);
    Value mul_values(Value a, Value b);
    Value div_values(Value a, Value b);
    Value lt_values(Value a, Value b);
    Value le_values(Value a, Value b);
    Value gt_values(Value a, Value b);
    Value ge_values(Value a, Value b);
    Value eq_values(Value a, Value b);
    Value ne_values(Value a, Value b);

    // Helper: index a vector/list
    Value index_value(Value base, Value idx);
    Value index2_value(Value base, Value idx);
    Value dollar_value(Value base, uint32_t sym_id);

    // Helper: convert a Value to a logical scalar (for if conditions)
    int32_t as_logical_scalar(Value v);

    // Stack of frames for backtrace.
    std::vector<Frame*> call_stack_;
};

}  // namespace rjit
