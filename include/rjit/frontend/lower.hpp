// rjit/frontend/lower.hpp - Lower AST to bytecode
#pragma once
#include "rjit/frontend/ast.hpp"
#include "rjit/bytecode/module.hpp"
#include "rjit/core/context.hpp"

namespace rjit {

class Lowerer {
public:
    explicit Lowerer(Context& ctx);

    // Lower an entire program (a sequence of statements) into a
    // top-level BytecodeFunction. The returned function has
    // nparams=0 and is executed in the global environment.
    BytecodeFunction* lower_program(Ast const& root);

    // Lower a function definition into a BytecodeFunction.
    BytecodeFunction* lower_function(FunctionAst const& fn, std::string name);

private:
    Context& ctx_;
    BytecodeBuilder b_;
    uint32_t nparams_ = 0;

    // Map from parameter name to slot register index (for LOAD_LOCAL).
    std::unordered_map<std::string, uint32_t> param_slots_;

    // Map from local variable name to register index. Used to keep
    // LOAD_VAR / STORE_VAR on a fast path inside a function — we
    // pre-allocate a register for every variable we see.
    std::unordered_map<std::string, uint32_t> local_slots_;

    // Loop context (for break/next)
    struct LoopCtx {
        uint32_t break_target;
        uint32_t next_target;
    };
    std::vector<LoopCtx> loops_;

    uint32_t intern_symbol(std::string_view name) { return ctx_.intern_symbol(name); }

    // Returns the destination register that holds the result.
    uint32_t lower_expr(Ast const& node, uint32_t dst = UINT32_MAX);
    void     lower_stmt(Ast const& node);
    uint32_t lower_binop(BinOpAst const& node, uint32_t dst);
    uint32_t lower_unaryop(UnaryOpAst const& node, uint32_t dst);
    uint32_t lower_assign(AssignAst const& node, uint32_t dst, bool discard);
    uint32_t lower_call(CallAst const& node, uint32_t dst);
    uint32_t lower_if(IfAst const& node, uint32_t dst);
    uint32_t lower_for(ForAst const& node, uint32_t dst);
    uint32_t lower_while(WhileAst const& node, uint32_t dst);
    uint32_t lower_repeat(RepeatAst const& node, uint32_t dst);
    uint32_t lower_block(BlockAst const& node, uint32_t dst);
    uint32_t lower_index(IndexAst const& node, uint32_t dst);
    uint32_t lower_dollar(DollarAst const& node, uint32_t dst);
    uint32_t lower_function(FunctionAst const& node, uint32_t dst);

    // Allocate-or-fetch a register for a local variable name.
    uint32_t slot_for(std::string const& name);
};

}  // namespace rjit
