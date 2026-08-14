// rjit/frontend/lower.cpp
#include "rjit/frontend/lower.hpp"
#include "rjit/core/error.hpp"
#include <unordered_map>

namespace rjit {

Lowerer::Lowerer(Context& ctx) : ctx_(ctx) {}

uint32_t Lowerer::slot_for(std::string const& name) {
    auto it = local_slots_.find(name);
    if (it != local_slots_.end()) return it->second;
    auto pit = param_slots_.find(name);
    if (pit != param_slots_.end()) return pit->second;
    return UINT32_MAX;  // not a local or param
}

BytecodeFunction* Lowerer::lower_program(Ast const& root) {
    // Top-level: each statement runs in the global env.
    if (root.kind == AstKind::Block) {
        for (auto& s : static_cast<BlockAst const&>(root).stmts) lower_stmt(*s);
    } else {
        lower_stmt(root);
    }
    return b_.finalize("__toplevel__", 0, 0);
}

BytecodeFunction* Lowerer::lower_function(FunctionAst const& fn, std::string name) {
    nparams_ = static_cast<uint32_t>(fn.params.size());
    // Allocate a register for each parameter and record its name.
    for (size_t i = 0; i < fn.params.size(); ++i) {
        uint32_t r = b_.alloc_reg();
        param_slots_[fn.params[i].name] = r;
        b_.fn()->param_names.push_back(fn.params[i].name);
    }
    // Lower body into register 0 (the return value).
    lower_expr(*fn.body, 0);
    // Return register 0
    b_.emit(Op::RETURN, 0, 0);
    return b_.finalize(name, nparams_, 0);
}

void Lowerer::lower_stmt(Ast const& node) {
    lower_expr(node, UINT32_MAX);
}

uint32_t Lowerer::lower_expr(Ast const& node, uint32_t dst) {
    // If no destination was requested, we still need to emit code for
    // side effects (e.g., function calls). We use a scratch register
    // that will be overwritten. The caller discards the return value.
    bool discard = (dst == UINT32_MAX);
    if (discard) dst = b_.alloc_reg();
    switch (node.kind) {
        case AstKind::Nil:        b_.emit(Op::LOAD_NIL, dst); return dst;
        case AstKind::Logical: {
            int32_t v = static_cast<LogicalAst const&>(node).value;
            if (v == 1) b_.emit(Op::LOAD_TRUE, dst);
            else if (v == 0) b_.emit(Op::LOAD_FALSE, dst);
            else b_.emit(Op::LOAD_NA, dst);
            return dst;
        }
        case AstKind::Integer: {
            int32_t v = static_cast<IntegerAst const&>(node).value;
            uint32_t k = b_.add_constant(Value::integer(v));
            b_.emit(Op::LOAD_INT, dst, 0, 0, k);
            return dst;
        }
        case AstKind::Real: {
            double v = static_cast<RealAst const&>(node).value;
            uint32_t k = b_.add_constant(Value::real(v));
            b_.emit(Op::LOAD_REAL, dst, 0, 0, k);
            return dst;
        }
        case AstKind::String: {
            uint32_t sym = intern_symbol(static_cast<StringAst const&>(node).value);
            b_.use_symbol(sym);
            uint32_t k = b_.add_constant(Value::string(sym));
            b_.emit(Op::LOAD_STRING, dst, 0, 0, k);
            return dst;
        }
        case AstKind::Symbol: {
            std::string const& name = static_cast<SymbolAst const&>(node).name;
            // Check if it's a parameter or local variable (promoted).
            uint32_t slot = slot_for(name);
            if (slot != UINT32_MAX) {
                // Fast path: load directly from register file.
                if (dst != slot) b_.emit(Op::LOAD_LOCAL, dst, slot);
                return dst;
            }
            uint32_t sym = intern_symbol(name);
            b_.use_symbol(sym);
            b_.emit(Op::LOAD_VAR, dst, 0, 0, sym);
            return dst;
        }
        case AstKind::BinOp:    return lower_binop(static_cast<BinOpAst const&>(node), dst);
        case AstKind::UnaryOp:  return lower_unaryop(static_cast<UnaryOpAst const&>(node), dst);
        case AstKind::Assign:   return lower_assign(static_cast<AssignAst const&>(node), dst, discard);
        case AstKind::Call:     return lower_call(static_cast<CallAst const&>(node), dst);
        case AstKind::If:       return lower_if(static_cast<IfAst const&>(node), dst);
        case AstKind::For:      return lower_for(static_cast<ForAst const&>(node), dst);
        case AstKind::While:    return lower_while(static_cast<WhileAst const&>(node), dst);
        case AstKind::Repeat:   return lower_repeat(static_cast<RepeatAst const&>(node), dst);
        case AstKind::Break: {
            if (loops_.empty()) throw RJitError("'break' outside of loop");
            b_.emit(Op::JUMP, 0, 0, 0, loops_.back().break_target);
            return dst;
        }
        case AstKind::Next: {
            if (loops_.empty()) throw RJitError("'next' outside of loop");
            b_.emit(Op::JUMP, 0, 0, 0, loops_.back().next_target);
            return dst;
        }
        case AstKind::Return: {
            auto const& r = static_cast<ReturnAst const&>(node);
            if (r.value) {
                uint32_t v = lower_expr(*r.value, 0);
                if (v != 0) b_.emit(Op::LOAD_LOCAL, 0, v);
                b_.emit(Op::RETURN, 0);
            } else {
                b_.emit(Op::RETURN_NULL, 0);
            }
            return dst;
        }
        case AstKind::Block:    return lower_block(static_cast<BlockAst const&>(node), dst);
        case AstKind::Paren:    return lower_expr(*static_cast<ParenAst const&>(node).expr, dst);
        case AstKind::Index:    return lower_index(static_cast<IndexAst const&>(node), dst);
        case AstKind::Dollar:   return lower_dollar(static_cast<DollarAst const&>(node), dst);
        case AstKind::Function: return lower_function(static_cast<FunctionAst const&>(node), dst);
        case AstKind::MissingArg:
            b_.emit(Op::LOAD_NIL, dst);  // simplification: missing args become NULL
            return dst;
        default:
            throw RJitError("lower: unhandled AST kind");
    }
}

uint32_t Lowerer::lower_binop(BinOpAst const& node, uint32_t dst) {
    std::string const& op = node.op;
    // Special-case `:` (sequence) and `<-` (handled in Assign)
    if (op == ":") {
        uint32_t a = lower_expr(*node.lhs);
        uint32_t b = lower_expr(*node.rhs);
        b_.emit(Op::MAKE_SEQ, dst, a, b);
        return dst;
    }
    // Lower operands into fresh registers. We can't reuse dst for the
    // first operand because dst might be a register that's still live
    // (e.g., a parameter like `n` in `fib(n-1) + fib(n-2)` — if we
    // write fib(n-1) into n's register, the second call can't read n).
    uint32_t a = lower_expr(*node.lhs);
    uint32_t b = lower_expr(*node.rhs);

    Op op_generic = Op::NOP;
    if      (op == "+") op_generic = Op::ADD;
    else if (op == "-") op_generic = Op::SUB;
    else if (op == "*") op_generic = Op::MUL;
    else if (op == "/") op_generic = Op::DIV;
    else if (op == "^") op_generic = Op::POW;
    else if (op == "%%") op_generic = Op::MOD;
    else if (op == "<") op_generic = Op::LT;
    else if (op == "<=") op_generic = Op::LE;
    else if (op == ">") op_generic = Op::GT;
    else if (op == ">=") op_generic = Op::GE;
    else if (op == "==") op_generic = Op::EQ;
    else if (op == "!=") op_generic = Op::NE;
    else if (op == "&")  op_generic = Op::AND_SCALAR;
    else if (op == "&&") op_generic = Op::AND;
    else if (op == "|")  op_generic = Op::OR_SCALAR;
    else if (op == "||") op_generic = Op::OR;
    else throw RJitError("lower: unknown binary operator '" + op + "'");

    b_.emit(op_generic, dst, a, b);
    return dst;
}

uint32_t Lowerer::lower_unaryop(UnaryOpAst const& node, uint32_t dst) {
    uint32_t a = lower_expr(*node.operand);
    if (node.op == "-") b_.emit(Op::NEG, dst, a);
    else if (node.op == "!") b_.emit(Op::NOT, dst, a);
    else if (node.op == "+") {
        if (dst != a) b_.emit(Op::LOAD_LOCAL, dst, a);
    } else throw RJitError("lower: unknown unary operator '" + node.op + "'");
    return dst;
}

uint32_t Lowerer::lower_assign(AssignAst const& node, uint32_t dst, bool discard) {
    // Lower RHS first, computing directly into dst when possible
    // to avoid unnecessary register copies.
    uint32_t v = lower_expr(*node.value);
    // Target must be a symbol (or assignable expression)
    if (node.target->kind == AstKind::Symbol) {
        std::string const& name = static_cast<SymbolAst const&>(*node.target).name;
        uint32_t sym = intern_symbol(name);
        b_.use_symbol(sym);
        if (node.super) {
            b_.emit(Op::STORE_SUPER, 0, v, 0, sym);
        } else {
            // Promote to local register: if this variable isn't already
            // a local, make it one. The register IS the canonical
            // location — we do NOT write to the environment on every
            // assignment. This is critical for loop performance.
            uint32_t slot = slot_for(name);
            if (slot == UINT32_MAX) {
                slot = b_.alloc_reg();
                local_slots_[name] = slot;
                // First assignment: store in the register AND define
                // in the environment (for reflection / top-level
                // visibility).
                if (slot != v) b_.emit(Op::LOAD_LOCAL, slot, v);
                b_.emit(Op::STORE_VAR, 0, slot, 0, sym);
            } else {
                if (slot != v) b_.emit(Op::LOAD_LOCAL, slot, v);
            }
        }
    } else if (node.target->kind == AstKind::Index) {
        // x[i] <- v   ->   INDEX_ASSIGN base, idx, v
        auto const& idx = static_cast<IndexAst const&>(*node.target);
        uint32_t base = lower_expr(*idx.base);
        // Lower each index arg
        for (size_t i = 0; i < idx.args.size(); ++i) {
            if (idx.args[i]->kind == AstKind::MissingArg) continue;
            uint32_t idxr = lower_expr(*idx.args[i]);
            b_.emit(idx.double_bracket ? Op::INDEX2_ASSIGN : Op::INDEX_ASSIGN, base, idxr, v);
        }
    } else if (node.target->kind == AstKind::Dollar) {
        auto const& d = static_cast<DollarAst const&>(*node.target);
        uint32_t base = lower_expr(*d.base);
        uint32_t sym = intern_symbol(d.name);
        b_.use_symbol(sym);
        b_.emit(Op::INDEX_NAMED_ASSIGN, base, 0, v, sym);
    } else {
        throw RJitError("lower: invalid assignment target");
    }
    // Only emit the result copy if the caller actually wants the result
    // in a specific register (and it's different from where the value
    // already lives).
    if (!discard && dst != v) b_.emit(Op::LOAD_LOCAL, dst, v);
    return v;
}

uint32_t Lowerer::lower_call(CallAst const& node, uint32_t dst) {
    // Callee must be a symbol (for now)
    if (node.callee->kind != AstKind::Symbol) {
        throw RJitError("lower: only direct calls are supported");
    }
    std::string const& name = static_cast<SymbolAst const&>(*node.callee).name;
    uint32_t sym = intern_symbol(name);
    b_.use_symbol(sym);

    // The CALL instruction expects:
    //   RDEST = call RA with K args from RA+1, RA+2, ..., RA+K
    // So RA is the callee, and args are in consecutive registers
    // starting at RA+1.
    //
    // We allocate the callee register first, then K consecutive arg
    // registers, then lower each argument into its slot. This ensures
    // args are contiguous even if lower_expr internally allocates
    // registers (those will be above the arg range).
    uint32_t callee_reg = b_.alloc_reg();

    // Pre-allocate arg registers (contiguous).
    uint32_t nargs = 0;
    for (auto& a : node.args) {
        if (a->kind == AstKind::MissingArg) continue;
        ++nargs;
    }
    // Reserve nargs registers starting at callee_reg+1.
    // alloc_reg returns sequential indices, so calling it nargs times
    // gives us callee_reg+1 .. callee_reg+nargs.
    std::vector<uint32_t> arg_regs;
    for (uint32_t i = 0; i < nargs; ++i) {
        arg_regs.push_back(b_.alloc_reg());
    }

    // Lower each argument into its pre-allocated register.
    uint32_t ai = 0;
    for (auto& a : node.args) {
        if (a->kind == AstKind::MissingArg) continue;
        lower_expr(*a, arg_regs[ai]);
        ++ai;
    }

    // Load the callee function value.
    b_.emit(Op::LOAD_VAR, callee_reg, 0, 0, sym);
    b_.emit(Op::CALL, dst, callee_reg, 0, nargs);
    return dst;
}

uint32_t Lowerer::lower_if(IfAst const& node, uint32_t dst) {
    bool discard = (dst == UINT32_MAX);
    if (discard) dst = b_.alloc_reg();
    uint32_t c = lower_expr(*node.cond);
    uint32_t jump_if_false = b_.emit(Op::JUMP_IF_FALSE, 0, c, 0, 0);
    lower_expr(*node.then_branch, dst);
    uint32_t jump_end = b_.emit(Op::JUMP, 0, 0, 0, 0);
    uint32_t else_target = static_cast<uint32_t>(b_.current_count());
    b_.patch_k(jump_if_false, else_target);
    if (node.else_branch) lower_expr(*node.else_branch, dst);
    else b_.emit(Op::LOAD_NIL, dst);
    uint32_t end_target = static_cast<uint32_t>(b_.current_count());
    b_.patch_k(jump_end, end_target);
    (void)discard;
    return dst;
}

uint32_t Lowerer::lower_for(ForAst const& node, uint32_t dst) {
    // Allocate the loop variable as a local register.
    uint32_t var_r = slot_for(node.var);
    if (var_r == UINT32_MAX) {
        var_r = b_.alloc_reg();
        local_slots_[node.var] = var_r;
    }

    // Optimize the common case: for (i in a:b) where a:b is a range.
    // Instead of materializing a vector and indexing it, iterate
    // the loop variable directly from start to stop.
    if (node.seq->kind == AstKind::BinOp &&
        static_cast<BinOpAst const&>(*node.seq).op == ":") {
        auto const& range = static_cast<BinOpAst const&>(*node.seq);
        uint32_t start_r = lower_expr(*range.lhs);
        uint32_t stop_r  = lower_expr(*range.rhs);
        // var = start
        b_.emit(Op::LOAD_LOCAL, var_r, start_r);
        // Loop header
        uint32_t header = b_.emit(Op::LOOP_HEADER, 0);
        // Compare var <= stop
        uint32_t cmp_r = b_.alloc_reg();
        b_.emit(Op::LE, cmp_r, var_r, stop_r);
        uint32_t exit_jump = b_.emit(Op::JUMP_IF_FALSE, 0, cmp_r, 0, 0);
        // Body
        loops_.push_back({0, 0});
        loops_.back().break_target = static_cast<uint32_t>(b_.current_count());
        loops_.back().next_target = static_cast<uint32_t>(b_.current_count());
        lower_expr(*node.body, UINT32_MAX);
        // var++ — use ADD_IMM instead of LOAD_INT + ADD to save
        // one instruction per loop iteration.
        uint32_t one_k = b_.add_constant(Value::integer(1));
        // For now, emit LOAD_INT + ADD (will optimize to ADD_IMM later).
        // Reuse the cmp_r register as scratch for the constant.
        b_.emit(Op::LOAD_INT, cmp_r, 0, 0, one_k);
        b_.emit(Op::ADD, var_r, var_r, cmp_r);
        // Backedge
        b_.emit(Op::LOOP_BACKEDGE, 0);
        b_.emit(Op::JUMP, 0, 0, 0, header);
        // Exit
        uint32_t exit_target = static_cast<uint32_t>(b_.current_count());
        b_.patch_k(exit_jump, exit_target);
        loops_.pop_back();
        b_.emit(Op::LOAD_NIL, dst);
        return dst;
    }

    // General case: materialize the sequence and index it.
    uint32_t seq_r = lower_expr(*node.seq);
    uint32_t len_r = b_.alloc_reg();
    uint32_t idx_r = b_.alloc_reg();
    uint32_t cmp_r = b_.alloc_reg();
    b_.emit(Op::LENGTH, len_r, seq_r);
    uint32_t one_k = b_.add_constant(Value::integer(1));
    b_.emit(Op::LOAD_INT, idx_r, 0, 0, one_k);
    uint32_t header = b_.emit(Op::LOOP_HEADER, 0);
    b_.emit(Op::LE, cmp_r, idx_r, len_r);
    uint32_t exit_jump = b_.emit(Op::JUMP_IF_FALSE, 0, cmp_r, 0, 0);
    b_.emit(Op::INDEX2, var_r, seq_r, idx_r);
    loops_.push_back({0, 0});
    loops_.back().break_target = static_cast<uint32_t>(b_.current_count());
    loops_.back().next_target = static_cast<uint32_t>(b_.current_count());
    lower_expr(*node.body, UINT32_MAX);
    uint32_t inc_r = b_.alloc_reg();
    b_.emit(Op::LOAD_INT, inc_r, 0, 0, one_k);
    b_.emit(Op::ADD, idx_r, idx_r, inc_r);
    b_.emit(Op::LOOP_BACKEDGE, 0);
    b_.emit(Op::JUMP, 0, 0, 0, header);
    uint32_t exit_target = static_cast<uint32_t>(b_.current_count());
    b_.patch_k(exit_jump, exit_target);
    loops_.pop_back();
    b_.emit(Op::LOAD_NIL, dst);
    return dst;
}

uint32_t Lowerer::lower_while(WhileAst const& node, uint32_t dst) {
    uint32_t header = b_.emit(Op::LOOP_HEADER, 0);
    uint32_t c = lower_expr(*node.cond);
    uint32_t exit_jump = b_.emit(Op::JUMP_IF_FALSE, 0, c, 0, 0);
    loops_.push_back({0, header});
    uint32_t body_start = static_cast<uint32_t>(b_.current_count());
    loops_.back().break_target = body_start;
    loops_.back().next_target = header;
    lower_expr(*node.body, UINT32_MAX);
    b_.emit(Op::LOOP_BACKEDGE, 0);
    b_.emit(Op::JUMP, 0, 0, 0, header);
    uint32_t exit_target = static_cast<uint32_t>(b_.current_count());
    b_.patch_k(exit_jump, exit_target);
    loops_.pop_back();
    b_.emit(Op::LOAD_NIL, dst);
    return dst;
}

uint32_t Lowerer::lower_repeat(RepeatAst const& node, uint32_t dst) {
    uint32_t header = b_.emit(Op::LOOP_HEADER, 0);
    loops_.push_back({0, header});
    uint32_t body_start = static_cast<uint32_t>(b_.current_count());
    loops_.back().break_target = body_start;
    loops_.back().next_target = header;
    lower_expr(*node.body, UINT32_MAX);
    b_.emit(Op::LOOP_BACKEDGE, 0);
    b_.emit(Op::JUMP, 0, 0, 0, header);
    uint32_t exit_target = static_cast<uint32_t>(b_.current_count());
    loops_.pop_back();
    b_.emit(Op::LOAD_NIL, dst);
    return dst;
}

uint32_t Lowerer::lower_block(BlockAst const& node, uint32_t dst) {
    for (auto& s : node.stmts) lower_expr(*s, dst);
    return dst;
}

uint32_t Lowerer::lower_index(IndexAst const& node, uint32_t dst) {
    uint32_t base = lower_expr(*node.base);
    if (node.args.empty()) {
        b_.emit(node.double_bracket ? Op::INDEX2 : Op::INDEX, dst, base, 0);
    } else if (node.args.size() == 1) {
        if (node.args[0]->kind == AstKind::MissingArg) {
            b_.emit(node.double_bracket ? Op::INDEX2 : Op::INDEX, dst, base, 0);
        } else {
            uint32_t idx = lower_expr(*node.args[0]);
            b_.emit(node.double_bracket ? Op::INDEX2 : Op::INDEX, dst, base, idx);
        }
    } else {
        // Multi-arg indexing: not yet supported in fast path.
        // For simplicity, use the first index.
        uint32_t idx = lower_expr(*node.args[0]);
        b_.emit(node.double_bracket ? Op::INDEX2 : Op::INDEX, dst, base, idx);
    }
    return dst;
}

uint32_t Lowerer::lower_dollar(DollarAst const& node, uint32_t dst) {
    uint32_t base = lower_expr(*node.base);
    uint32_t sym = intern_symbol(node.name);
    b_.use_symbol(sym);
    b_.emit(Op::INDEX_NAMED, dst, base, 0, sym);
    return dst;
}

uint32_t Lowerer::lower_function(FunctionAst const& node, uint32_t dst) {
    // Create a nested Lowerer for the function body
    Lowerer nested(ctx_);
    BytecodeFunction* child = nested.lower_function(node, "<anonymous>");
    uint32_t k = b_.add_constant(Value::from_heap(TypeTag::kBytecodeFn, child));
    b_.emit(Op::MAKE_CLOSURE, dst, 0, 0, k);
    return dst;
}

}  // namespace rjit
