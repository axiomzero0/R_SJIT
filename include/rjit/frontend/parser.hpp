// rjit/frontend/parser.hpp
#pragma once
#include <vector>
#include "rjit/frontend/ast.hpp"
#include "rjit/frontend/token.hpp"

namespace rjit {

class Parser {
public:
    explicit Parser(std::vector<Token> toks);

    // Parse the whole input as a sequence of statements. The result
    // is a BlockAst containing the top-level statements.
    AstPtr parse_program();

private:
    std::vector<Token> toks_;
    size_t             pos_ = 0;

    Token const& peek(size_t ahead = 0) const noexcept;
    Token const& advance() noexcept;
    bool         at(TokKind k) const noexcept;
    bool         accept(TokKind k) noexcept;
    void         expect(TokKind k, const char* what);
    [[noreturn]] void error(std::string msg);

    // Skip newlines (used between statements)
    void skip_newlines() noexcept;

    // Precedence-climbing expression parser
    AstPtr parse_expr();
    AstPtr parse_assign();
    AstPtr parse_tilde();       // formula (~)
    AstPtr parse_or();          // ||  (also |)
    AstPtr parse_and();         // &&  (also &)
    AstPtr parse_not();         // !
    AstPtr parse_comparison();  // == != < > <= >=
    AstPtr parse_range();       // :
    AstPtr parse_additive();    // + -
    AstPtr parse_multiplicative(); // * /
    AstPtr parse_power();       // ^ (right-assoc)
    AstPtr parse_unary();       // - +
    AstPtr parse_postfix();     // $ @ [ [[  (
    AstPtr parse_primary();

    AstPtr parse_call_args(AstPtr callee);
    AstPtr parse_index_args(AstPtr base, bool dbl);
    AstPtr parse_function();
    AstPtr parse_if();
    AstPtr parse_for();
    AstPtr parse_while();
    AstPtr parse_repeat();
    AstPtr parse_block();

    int op_precedence(TokKind k) const noexcept;
};

}  // namespace rjit
