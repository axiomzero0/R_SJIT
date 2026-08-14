// rjit/frontend/lexer.hpp
#pragma once
#include <string>
#include <string_view>
#include <vector>
#include "rjit/frontend/token.hpp"

namespace rjit {

class Lexer {
public:
    explicit Lexer(std::string_view src);

    // Lex the entire source into a token stream. The final token will
    // have kind == Eof.
    std::vector<Token> lex_all();

    // Lex one token (for testing).
    Token next_token();

private:
    std::string_view src_;
    size_t           pos_   = 0;
    uint32_t         line_  = 1;
    uint32_t         col_   = 1;

    [[noreturn]] void error(std::string msg);
    char peek(size_t ahead = 0) const noexcept;
    char advance() noexcept;
    void skip_whitespace_and_comments() noexcept;
    Token lex_number();
    Token lex_string();
    Token lex_ident_or_keyword();
    Token lex_operator();

    bool is_ident_start(char c) const noexcept;
    bool is_ident_cont(char c) const noexcept;
};

}  // namespace rjit
