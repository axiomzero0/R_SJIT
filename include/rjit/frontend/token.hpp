// rjit/frontend/token.hpp - Token definitions
//
// Tokens carry enough source-location information for the parser to
// generate useful error messages and for the JIT to embed source
// ranges in IR nodes (which the deopt path uses to reconstruct R
// stack frames).

#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace rjit {

enum class TokKind : uint16_t {
    // End of input
    Eof,

    // Literals
    Real,        // 3.14, 1e10, 0x1p4
    Integer,     // 42L, 0x1L
    Complex,     // 3i
    String,      // "foo"
    Ident,       // foo, foo.bar
    BacktickIdent, // `foo bar`

    // Operators and punctuation
    LParen, RParen,
    LBrace, RBrace,
    LBracket, RBracket,
    DoubleLBracket, DoubleRBracket,  // [[ ]]
    Comma, Semicolon,
    Plus, Minus, Star, Slash, Caret,    // + - * / ^
   _LT, _GT, LE, GE, EQ, NE,            // < > <= >= == !=
    And, Or, Not,                        // && || !
    AmdAmp, PipePipe,                    // & |
    Assign,                      // <-
    SuperAssign,                 // <<-
    RightAssign,                 // ->
    RightSuperAssign,            // ->>
    Equal,                       // = (context-dependent)
    Colon,                       // :
    Dollar,                      // $
    At,                          // @
    Question,                    // ?
    Tilde,                       // ~

    // Keywords
    Kw_function, Kw_if, Kw_else, Kw_for, Kw_while, Kw_repeat,
    Kw_break, Kw_next, Kw_return, Kw_in, Kw_NULL, Kw_TRUE, Kw_FALSE,
    Kw_NA, Kw_NaInt, Kw_NaReal, Kw_NaString, Kw_Inf, Kw_NaN,
    Kw_quote, Kw_missing,

    // Special forms
    Newline,  // significant at top level for statement separation
};

const char* tok_name(TokKind k) noexcept;

struct Token {
    TokKind       kind;
    uint32_t      line;
    uint32_t      column;
    std::string   text;     // for idents, strings, numbers
    union {
        double   real_val;
        int64_t  int_val;
    };

    Token() : kind(TokKind::Eof), line(0), column(0), real_val(0) {}
};

}  // namespace rjit
