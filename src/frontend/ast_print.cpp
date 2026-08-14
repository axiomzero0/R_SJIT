// rjit/frontend/ast_print.cpp
#include "rjit/frontend/ast.hpp"
#include <cstdio>

namespace rjit {

static void indent(int n) {
    for (int i = 0; i < n; ++i) std::fputs("  ", stdout);
}

void NilAst::print(int i) const            { indent(i); std::puts("NULL"); }
void LogicalAst::print(int i) const        { indent(i); std::printf("Logical(%d)\n", value); }
void IntegerAst::print(int i) const        { indent(i); std::printf("Integer(%d)\n", value); }
void RealAst::print(int i) const           { indent(i); std::printf("Real(%g)\n", value); }
void StringAst::print(int i) const         { indent(i); std::printf("String(\"%s\")\n", value.c_str()); }
void SymbolAst::print(int i) const         { indent(i); std::printf("Symbol(%s)\n", name.c_str()); }
void MissingArgAst::print(int i) const     { indent(i); std::puts("MissingArg"); }
void BreakAst::print(int i) const          { indent(i); std::puts("break"); }
void NextAst::print(int i) const           { indent(i); std::puts("next"); }

void BinOpAst::print(int i) const {
    indent(i); std::printf("BinOp('%s')\n", op.c_str());
    lhs->print(i+1);
    rhs->print(i+1);
}

void UnaryOpAst::print(int i) const {
    indent(i); std::printf("UnaryOp('%s')\n", op.c_str());
    operand->print(i+1);
}

void AssignAst::print(int i) const {
    indent(i); std::printf("Assign(%s)\n", super ? "<<-" : "<-");
    target->print(i+1);
    value->print(i+1);
}

void CallAst::print(int i) const {
    indent(i); std::puts("Call:");
    callee->print(i+1);
    indent(i+1); std::printf("args (%zu):\n", args.size());
    for (auto& a : args) a->print(i+2);
}

void NamedArgAst::print(int i) const {
    indent(i); std::printf("NamedArg(%s):\n", name.c_str());
    value->print(i+1);
}

void FunctionAst::print(int i) const {
    indent(i); std::puts("Function:");
    indent(i+1); std::printf("params (%zu):\n", params.size());
    for (auto& p : params) {
        indent(i+2); std::printf("%s", p.name.c_str());
        if (p.default_value) {
            std::puts(" =");
            p.default_value->print(i+3);
        } else {
            std::puts("");
        }
    }
    indent(i+1); std::puts("body:");
    body->print(i+2);
}

void IfAst::print(int i) const {
    indent(i); std::puts("If:");
    indent(i+1); std::puts("cond:");
    cond->print(i+2);
    indent(i+1); std::puts("then:");
    then_branch->print(i+2);
    if (else_branch) {
        indent(i+1); std::puts("else:");
        else_branch->print(i+2);
    }
}

void ForAst::print(int i) const {
    indent(i); std::printf("For (%s in):\n", var.c_str());
    seq->print(i+1);
    indent(i+1); std::puts("body:");
    body->print(i+2);
}

void WhileAst::print(int i) const {
    indent(i); std::puts("While:");
    cond->print(i+1);
    body->print(i+1);
}

void RepeatAst::print(int i) const {
    indent(i); std::puts("Repeat:");
    body->print(i+1);
}

void ReturnAst::print(int i) const {
    indent(i); std::puts("Return:");
    if (value) value->print(i+1);
    else { indent(i+1); std::puts("(NULL)"); }
}

void BlockAst::print(int i) const {
    indent(i); std::printf("Block (%zu stmts):\n", stmts.size());
    for (auto& s : stmts) s->print(i+1);
}

void ParenAst::print(int i) const {
    indent(i); std::puts("Paren:");
    expr->print(i+1);
}

void IndexAst::print(int i) const {
    indent(i); std::printf("Index(%s):\n", double_bracket ? "[[" : "[");
    base->print(i+1);
    indent(i+1); std::printf("args (%zu):\n", args.size());
    for (auto& a : args) a->print(i+2);
}

void DollarAst::print(int i) const {
    indent(i); std::printf("Dollar($%s):\n", name.c_str());
    base->print(i+1);
}

void SlotAst::print(int i) const {
    indent(i); std::printf("Slot(@%s):\n", name.c_str());
    base->print(i+1);
}

}  // namespace rjit
