// rjit/frontend/lexer.cpp
#include "rjit/frontend/lexer.hpp"
#include "rjit/core/error.hpp"
#include <cctype>
#include <cstring>
#include <cstdlib>

namespace rjit {

const char* tok_name(TokKind k) noexcept {
    switch (k) {
        case TokKind::Eof:        return "eof";
        case TokKind::Real:       return "real";
        case TokKind::Integer:    return "integer";
        case TokKind::Complex:    return "complex";
        case TokKind::String:     return "string";
        case TokKind::Ident:      return "ident";
        case TokKind::BacktickIdent: return "backtick_ident";
        case TokKind::LParen:     return "(";
        case TokKind::RParen:     return ")";
        case TokKind::LBrace:     return "{";
        case TokKind::RBrace:     return "}";
        case TokKind::LBracket:   return "[";
        case TokKind::RBracket:   return "]";
        case TokKind::DoubleLBracket: return "[[";
        case TokKind::DoubleRBracket: return "]]";
        case TokKind::Comma:      return ",";
        case TokKind::Semicolon:  return ";";
        case TokKind::Plus:       return "+";
        case TokKind::Minus:      return "-";
        case TokKind::Star:       return "*";
        case TokKind::Slash:      return "/";
        case TokKind::Caret:      return "^";
        case TokKind::_LT:        return "<";
        case TokKind::_GT:        return ">";
        case TokKind::LE:         return "<=";
        case TokKind::GE:         return ">=";
        case TokKind::EQ:         return "==";
        case TokKind::NE:         return "!=";
        case TokKind::And:        return "&&";
        case TokKind::Or:         return "||";
        case TokKind::Not:        return "!";
        case TokKind::AmdAmp:     return "&";
        case TokKind::PipePipe:   return "|";
        case TokKind::Assign:     return "<-";
        case TokKind::SuperAssign:return "<<-";
        case TokKind::RightAssign:return "->";
        case TokKind::RightSuperAssign: return "->>";
        case TokKind::Equal:      return "=";
        case TokKind::Colon:      return ":";
        case TokKind::Dollar:     return "$";
        case TokKind::At:         return "@";
        case TokKind::Question:   return "?";
        case TokKind::Tilde:      return "~";
        case TokKind::Kw_function:return "function";
        case TokKind::Kw_if:      return "if";
        case TokKind::Kw_else:    return "else";
        case TokKind::Kw_for:     return "for";
        case TokKind::Kw_while:   return "while";
        case TokKind::Kw_repeat:  return "repeat";
        case TokKind::Kw_break:   return "break";
        case TokKind::Kw_next:    return "next";
        case TokKind::Kw_return:  return "return";
        case TokKind::Kw_in:      return "in";
        case TokKind::Kw_NULL:    return "NULL";
        case TokKind::Kw_TRUE:    return "TRUE";
        case TokKind::Kw_FALSE:   return "FALSE";
        case TokKind::Kw_NA:      return "NA";
        case TokKind::Kw_NaInt:   return "NA_integer_";
        case TokKind::Kw_NaReal:  return "NA_real_";
        case TokKind::Kw_NaString:return "NA_character_";
        case TokKind::Kw_Inf:     return "Inf";
        case TokKind::Kw_NaN:     return "NaN";
        case TokKind::Kw_quote:   return "quote";
        case TokKind::Kw_missing: return "missing";
        case TokKind::Newline:    return "newline";
    }
    return "?";
}

Lexer::Lexer(std::string_view src) : src_(src) {}

char Lexer::peek(size_t ahead) const noexcept {
    size_t p = pos_ + ahead;
    return p < src_.size() ? src_[p] : '\0';
}

char Lexer::advance() noexcept {
    if (pos_ >= src_.size()) return '\0';
    char c = src_[pos_++];
    if (c == '\n') { ++line_; col_ = 1; }
    else ++col_;
    return c;
}

void Lexer::skip_whitespace_and_comments() noexcept {
    while (pos_ < src_.size()) {
        char c = src_[pos_];
        if (c == ' ' || c == '\t' || c == '\r') {
            advance();
        } else if (c == '#') {
            // Check for #! directive (test runner metadata)
            if (pos_ + 1 < src_.size() && src_[pos_ + 1] == '!') {
                // Check if it's #!expect or #!expect_error
                bool is_expect = (pos_ + 7 < src_.size() &&
                                  src_[pos_+2] == 'e' && src_[pos_+3] == 'x' &&
                                  src_[pos_+4] == 'p' && src_[pos_+5] == 'e' &&
                                  src_[pos_+6] == 'c' && src_[pos_+7] == 't');
                // Skip the #! line
                while (pos_ < src_.size() && src_[pos_] != '\n') advance();
                if (pos_ < src_.size()) advance(); // skip the newline

                if (is_expect) {
                    // Skip everything until #!end
                    while (pos_ < src_.size()) {
                        // Check for #!end
                        if (src_[pos_] == '#' && pos_ + 5 < src_.size() &&
                            src_[pos_+1] == '!' && src_[pos_+2] == 'e' &&
                            src_[pos_+3] == 'n' && src_[pos_+4] == 'd') {
                            while (pos_ < src_.size() && src_[pos_] != '\n') advance();
                            if (pos_ < src_.size()) advance();
                            break;
                        }
                        advance();
                    }
                }
            } else {
                // Regular comment: skip to end of line
                while (pos_ < src_.size() && src_[pos_] != '\n') advance();
            }
        } else {
            break;
        }
    }
}

bool Lexer::is_ident_start(char c) const noexcept {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '.';
}

bool Lexer::is_ident_cont(char c) const noexcept {
    return is_ident_start(c) || std::isdigit(static_cast<unsigned char>(c));
}

[[noreturn]] void Lexer::error(std::string msg) {
    throw RJitError("lex error at line " + std::to_string(line_) +
                    " col " + std::to_string(col_) + ": " + msg);
}

Token Lexer::lex_number() {
    Token t;
    t.line = line_; t.column = col_;
    size_t start = pos_;
    bool has_dot = false;
    bool has_exp = false;
    bool has_hex = false;

    if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
        has_hex = true;
        advance(); advance();
        while (pos_ < src_.size() && std::isxdigit(static_cast<unsigned char>(src_[pos_])))
            advance();
    } else {
        while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_])))
            advance();
        if (pos_ < src_.size() && src_[pos_] == '.') {
            has_dot = true; advance();
            while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_])))
                advance();
        }
        if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
            has_exp = true; advance();
            if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-')) advance();
            while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_])))
                advance();
        }
    }

    // Suffix L / i
    bool is_integer = false;
    bool is_complex = false;
    if (pos_ < src_.size() && (src_[pos_] == 'L' || src_[pos_] == 'l')) {
        is_integer = true; advance();
    } else if (pos_ < src_.size() && src_[pos_] == 'i') {
        is_complex = true; advance();
    }

    std::string text(src_.substr(start, pos_ - start));
    t.text = text;

    if (is_complex) {
        t.kind = TokKind::Complex;
        t.real_val = std::strtod(text.c_str(), nullptr);
    } else if (is_integer) {
        t.kind = TokKind::Integer;
        if (has_hex) t.int_val = static_cast<int64_t>(std::strtoll(text.c_str(), nullptr, 16));
        else t.int_val = static_cast<int64_t>(std::strtoll(text.c_str(), nullptr, 10));
    } else {
        t.kind = TokKind::Real;
        t.real_val = std::strtod(text.c_str(), nullptr);
    }
    return t;
}

Token Lexer::lex_string() {
    Token t;
    t.line = line_; t.column = col_;
    char quote = advance();
    std::string s;
    bool escaped = false;
    while (pos_ < src_.size()) {
        char c = src_[pos_];
        if (escaped) {
            switch (c) {
                case 'n': s.push_back('\n'); break;
                case 't': s.push_back('\t'); break;
                case 'r': s.push_back('\r'); break;
                case '\\': s.push_back('\\'); break;
                case '\'': s.push_back('\''); break;
                case '"': s.push_back('"'); break;
                case '0': s.push_back('\0'); break;
                case 'x': {
                    advance();
                    std::string hex;
                    hex.push_back(advance());
                    hex.push_back(advance());
                    s.push_back(static_cast<char>(std::strtol(hex.c_str(), nullptr, 16)));
                    continue;
                }
                default: s.push_back(c); break;
            }
            escaped = false;
            advance();
        } else if (c == '\\') {
            escaped = true;
            advance();
        } else if (c == quote) {
            advance();
            break;
        } else {
            s.push_back(c);
            advance();
        }
    }
    t.kind = TokKind::String;
    t.text = std::move(s);
    return t;
}

Token Lexer::lex_ident_or_keyword() {
    Token t;
    t.line = line_; t.column = col_;
    size_t start = pos_;
    while (pos_ < src_.size() && is_ident_cont(src_[pos_]))
        advance();
    std::string name(src_.substr(start, pos_ - start));
    t.text = name;

    if (name == "function")      t.kind = TokKind::Kw_function;
    else if (name == "if")       t.kind = TokKind::Kw_if;
    else if (name == "else")     t.kind = TokKind::Kw_else;
    else if (name == "for")      t.kind = TokKind::Kw_for;
    else if (name == "while")    t.kind = TokKind::Kw_while;
    else if (name == "repeat")   t.kind = TokKind::Kw_repeat;
    else if (name == "break")    t.kind = TokKind::Kw_break;
    else if (name == "next")     t.kind = TokKind::Kw_next;
    else if (name == "return")   t.kind = TokKind::Kw_return;
    else if (name == "in")       t.kind = TokKind::Kw_in;
    else if (name == "NULL")     t.kind = TokKind::Kw_NULL;
    else if (name == "TRUE")     t.kind = TokKind::Kw_TRUE;
    else if (name == "FALSE")    t.kind = TokKind::Kw_FALSE;
    else if (name == "NA")       t.kind = TokKind::Kw_NA;
    else if (name == "NA_integer_")  t.kind = TokKind::Kw_NaInt;
    else if (name == "NA_real_")     t.kind = TokKind::Kw_NaReal;
    else if (name == "NA_character_")t.kind = TokKind::Kw_NaString;
    else if (name == "Inf")      t.kind = TokKind::Kw_Inf;
    else if (name == "NaN")      t.kind = TokKind::Kw_NaN;
    else if (name == "quote")    t.kind = TokKind::Kw_quote;
    else if (name == "missing")  t.kind = TokKind::Kw_missing;
    else if (name == "T")        { t.kind = TokKind::Kw_TRUE; }
    else if (name == "F")        { t.kind = TokKind::Kw_FALSE; }
    else t.kind = TokKind::Ident;
    return t;
}

Token Lexer::lex_operator() {
    Token t;
    t.line = line_; t.column = col_;
    char c = advance();

    auto two = [&](char second, TokKind two_kind, TokKind one_kind) -> TokKind {
        if (peek() == second) { advance(); return two_kind; }
        return one_kind;
    };

    switch (c) {
        case '(': t.kind = TokKind::LParen; break;
        case ')': t.kind = TokKind::RParen; break;
        case '{': t.kind = TokKind::LBrace; break;
        case '}': t.kind = TokKind::RBrace; break;
        case '[':
            if (peek() == '[') { advance(); t.kind = TokKind::DoubleLBracket; }
            else t.kind = TokKind::LBracket;
            break;
        case ']':
            if (peek() == ']') { advance(); t.kind = TokKind::DoubleRBracket; }
            else t.kind = TokKind::RBracket;
            break;
        case ',': t.kind = TokKind::Comma; break;
        case ';': t.kind = TokKind::Semicolon; break;
        case '+': t.kind = TokKind::Plus; break;
        case '-':
            if (peek() == '>') {
                advance();
                if (peek() == '>') { advance(); t.kind = TokKind::RightSuperAssign; }
                else t.kind = TokKind::RightAssign;
            } else t.kind = TokKind::Minus;
            break;
        case '*': t.kind = TokKind::Star; break;
        case '/': t.kind = TokKind::Slash; break;
        case '^': t.kind = TokKind::Caret; break;
        case '<':
            if (peek() == '<') {
                advance();
                if (peek() == '-') { advance(); t.kind = TokKind::SuperAssign; }
                else error("expected '<-' or '<<-'");
            } else if (peek() == '=') { advance(); t.kind = TokKind::LE; }
            else if (peek() == '-') { advance(); t.kind = TokKind::Assign; }
            else t.kind = TokKind::_LT;
            break;
        case '>':
            if (peek() == '=') { advance(); t.kind = TokKind::GE; }
            else t.kind = TokKind::_GT;
            break;
        case '=':
            if (peek() == '=') { advance(); t.kind = TokKind::EQ; }
            else t.kind = TokKind::Equal;
            break;
        case '!':
            if (peek() == '=') { advance(); t.kind = TokKind::NE; }
            else t.kind = TokKind::Not;
            break;
        case '&':
            if (peek() == '&') { advance(); t.kind = TokKind::And; }
            else t.kind = TokKind::AmdAmp;
            break;
        case '|':
            if (peek() == '|') { advance(); t.kind = TokKind::Or; }
            else t.kind = TokKind::PipePipe;
            break;
        case ':': t.kind = TokKind::Colon; break;
        case '$': t.kind = TokKind::Dollar; break;
        case '@': t.kind = TokKind::At; break;
        case '?': t.kind = TokKind::Question; break;
        case '~': t.kind = TokKind::Tilde; break;
        default:
            error(std::string("unexpected character '") + c + "'");
    }
    return t;
}

Token Lexer::next_token() {
    skip_whitespace_and_comments();
    if (pos_ >= src_.size()) {
        Token t; t.kind = TokKind::Eof; t.line = line_; t.column = col_;
        return t;
    }
    char c = src_[pos_];
    if (c == '\n') {
        Token t; t.kind = TokKind::Newline; t.line = line_; t.column = col_;
        advance();
        return t;
    }
    if (std::isdigit(static_cast<unsigned char>(c)) ||
        (c == '.' && pos_+1 < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_+1]))))
        return lex_number();
    if (c == '"' || c == '\'') return lex_string();
    if (c == '`') {
        Token t; t.line = line_; t.column = col_;
        advance();
        size_t start = pos_;
        while (pos_ < src_.size() && src_[pos_] != '`') advance();
        std::string name(src_.substr(start, pos_ - start));
        if (pos_ < src_.size()) advance();
        t.kind = TokKind::BacktickIdent;
        t.text = std::move(name);
        return t;
    }
    if (is_ident_start(c)) return lex_ident_or_keyword();
    return lex_operator();
}

std::vector<Token> Lexer::lex_all() {
    std::vector<Token> out;
    while (true) {
        Token t = next_token();
        bool is_eof = (t.kind == TokKind::Eof);
        out.push_back(std::move(t));
        if (is_eof) break;
    }
    return out;
}

}  // namespace rjit
