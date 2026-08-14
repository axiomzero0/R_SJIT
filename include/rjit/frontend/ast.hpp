// rjit/frontend/ast.hpp - Abstract Syntax Tree
//
// The AST mirrors R's source structure closely. We don't do any
// desugaring in the AST itself — that happens during lowering to
// bytecode. This keeps the AST faithful to the source for tools that
// want source-to-source transformation (e.g., `substitute()`).

#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <variant>
#include "rjit/core/value.hpp"

namespace rjit {

enum class AstKind : uint8_t {
    Nil,
    Logical,        // TRUE / FALSE / NA
    Integer,        // 42L
    Real,           // 3.14
    String,         // "foo"
    Symbol,         // foo
    Call,           // f(a, b)
    Function,       // function(args) body
    BinOp,          // a + b
    UnaryOp,        // -a
    Assign,         // x <- value
    SuperAssign,    // x <<- value
    If,
    For,
    While,
    Repeat,
    Break,
    Next,
    Return,
    Block,          // { ... }
    Paren,          // (expr)
    Index,          // x[i]
    Index2,         // x[[i]]
    Dollar,         // x$name
    Slot,           // x@name
    NamedArg,       // name = value (inside a call)
    Param,          // function parameter declaration
    MissingArg,     // an empty argument in a call: f(,)
};

class Ast {
public:
    AstKind   kind;
    uint32_t  line   = 0;
    uint32_t  column = 0;

    Ast(AstKind k) : kind(k) {}
    virtual ~Ast() = default;

    // Pretty-printing (defined in ast_print.cpp)
    virtual void print(int indent = 0) const = 0;
};

using AstPtr = std::unique_ptr<Ast>;

// ----- Leaf nodes -----
class NilAst : public Ast {
public:
    NilAst() : Ast(AstKind::Nil) {}
    void print(int i=0) const override;
};

class LogicalAst : public Ast {
public:
    int32_t value;
    explicit LogicalAst(int32_t v) : Ast(AstKind::Logical), value(v) {}
    void print(int i=0) const override;
};

class IntegerAst : public Ast {
public:
    int32_t value;
    explicit IntegerAst(int32_t v) : Ast(AstKind::Integer), value(v) {}
    void print(int i=0) const override;
};

class RealAst : public Ast {
public:
    double value;
    explicit RealAst(double v) : Ast(AstKind::Real), value(v) {}
    void print(int i=0) const override;
};

class StringAst : public Ast {
public:
    std::string value;
    explicit StringAst(std::string v) : Ast(AstKind::String), value(std::move(v)) {}
    void print(int i=0) const override;
};

class SymbolAst : public Ast {
public:
    std::string name;
    explicit SymbolAst(std::string n) : Ast(AstKind::Symbol), name(std::move(n)) {}
    void print(int i=0) const override;
};

class MissingArgAst : public Ast {
public:
    MissingArgAst() : Ast(AstKind::MissingArg) {}
    void print(int i=0) const override;
};

// ----- Internal nodes -----
class BinOpAst : public Ast {
public:
    std::string op;
    AstPtr lhs;
    AstPtr rhs;
    BinOpAst(std::string o, AstPtr l, AstPtr r)
        : Ast(AstKind::BinOp), op(std::move(o)), lhs(std::move(l)), rhs(std::move(r)) {}
    void print(int i=0) const override;
};

class UnaryOpAst : public Ast {
public:
    std::string op;
    AstPtr operand;
    UnaryOpAst(std::string o, AstPtr x)
        : Ast(AstKind::UnaryOp), op(std::move(o)), operand(std::move(x)) {}
    void print(int i=0) const override;
};

class AssignAst : public Ast {
public:
    AstPtr target;
    AstPtr value;
    bool   super = false;  // <<- vs <-
    AssignAst(AstPtr t, AstPtr v, bool s)
        : Ast(AstKind::Assign), target(std::move(t)), value(std::move(v)), super(s) {}
    void print(int i=0) const override;
};

class CallAst : public Ast {
public:
    AstPtr callee;
    std::vector<AstPtr> args;
    CallAst(AstPtr c, std::vector<AstPtr> a)
        : Ast(AstKind::Call), callee(std::move(c)), args(std::move(a)) {}
    void print(int i=0) const override;
};

class NamedArgAst : public Ast {
public:
    std::string name;
    AstPtr value;
    NamedArgAst(std::string n, AstPtr v)
        : Ast(AstKind::NamedArg), name(std::move(n)), value(std::move(v)) {}
    void print(int i=0) const override;
};

class FunctionAst : public Ast {
public:
    struct Param {
        std::string name;
        AstPtr      default_value;  // may be nullptr
    };
    std::vector<Param> params;
    AstPtr body;
    FunctionAst(std::vector<Param> p, AstPtr b)
        : Ast(AstKind::Function), params(std::move(p)), body(std::move(b)) {}
    void print(int i=0) const override;
};

class IfAst : public Ast {
public:
    AstPtr cond;
    AstPtr then_branch;
    AstPtr else_branch;  // may be nullptr
    IfAst(AstPtr c, AstPtr t, AstPtr e)
        : Ast(AstKind::If), cond(std::move(c)), then_branch(std::move(t)), else_branch(std::move(e)) {}
    void print(int i=0) const override;
};

class ForAst : public Ast {
public:
    std::string var;
    AstPtr      seq;
    AstPtr      body;
    ForAst(std::string v, AstPtr s, AstPtr b)
        : Ast(AstKind::For), var(std::move(v)), seq(std::move(s)), body(std::move(b)) {}
    void print(int i=0) const override;
};

class WhileAst : public Ast {
public:
    AstPtr cond;
    AstPtr body;
    WhileAst(AstPtr c, AstPtr b)
        : Ast(AstKind::While), cond(std::move(c)), body(std::move(b)) {}
    void print(int i=0) const override;
};

class RepeatAst : public Ast {
public:
    AstPtr body;
    explicit RepeatAst(AstPtr b) : Ast(AstKind::Repeat), body(std::move(b)) {}
    void print(int i=0) const override;
};

class BreakAst : public Ast {
public:
    BreakAst() : Ast(AstKind::Break) {}
    void print(int i=0) const override;
};

class NextAst : public Ast {
public:
    NextAst() : Ast(AstKind::Next) {}
    void print(int i=0) const override;
};

class ReturnAst : public Ast {
public:
    AstPtr value;  // may be nullptr (returns NULL)
    explicit ReturnAst(AstPtr v) : Ast(AstKind::Return), value(std::move(v)) {}
    void print(int i=0) const override;
};

class BlockAst : public Ast {
public:
    std::vector<AstPtr> stmts;
    explicit BlockAst(std::vector<AstPtr> s) : Ast(AstKind::Block), stmts(std::move(s)) {}
    void print(int i=0) const override;
};

class ParenAst : public Ast {
public:
    AstPtr expr;
    explicit ParenAst(AstPtr e) : Ast(AstKind::Paren), expr(std::move(e)) {}
    void print(int i=0) const override;
};

class IndexAst : public Ast {
public:
    AstPtr base;
    std::vector<AstPtr> args;  // x[i, j]
    bool   double_bracket = false;
    IndexAst(AstPtr b, std::vector<AstPtr> a, bool db)
        : Ast(AstKind::Index), base(std::move(b)), args(std::move(a)), double_bracket(db) {}
    void print(int i=0) const override;
};

class DollarAst : public Ast {
public:
    AstPtr       base;
    std::string  name;
    DollarAst(AstPtr b, std::string n)
        : Ast(AstKind::Dollar), base(std::move(b)), name(std::move(n)) {}
    void print(int i=0) const override;
};

class SlotAst : public Ast {
public:
    AstPtr       base;
    std::string  name;
    SlotAst(AstPtr b, std::string n)
        : Ast(AstKind::Slot), base(std::move(b)), name(std::move(n)) {}
    void print(int i=0) const override;
};

}  // namespace rjit
