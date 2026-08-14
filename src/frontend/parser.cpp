// rjit/frontend/parser.cpp
#include "rjit/frontend/parser.hpp"
#include "rjit/core/error.hpp"
#include <sstream>
#include <utility>

namespace rjit {

Parser::Parser(std::vector<Token> toks) : toks_(std::move(toks)) {}

Token const& Parser::peek(size_t ahead) const noexcept {
    static Token eof;
    if (pos_ + ahead < toks_.size()) return toks_[pos_ + ahead];
    return toks_.back();
}

Token const& Parser::advance() noexcept {
    if (pos_ < toks_.size()) return toks_[pos_++];
    return toks_.back();
}

bool Parser::at(TokKind k) const noexcept { return peek().kind == k; }
bool Parser::accept(TokKind k) noexcept {
    if (peek().kind == k) { advance(); return true; }
    return false;
}

void Parser::expect(TokKind k, const char* what) {
    if (!accept(k))
        error(std::string("expected ") + what + " but got '" + tok_name(peek().kind) + "'");
}

[[noreturn]] void Parser::error(std::string msg) {
    auto const& t = peek();
    throw RJitError("parse error at line " + std::to_string(t.line) +
                    " col " + std::to_string(t.column) + ": " + msg);
}

void Parser::skip_newlines() noexcept {
    while (peek().kind == TokKind::Newline || peek().kind == TokKind::Semicolon) advance();
}

// ----- Program -----

AstPtr Parser::parse_program() {
    std::vector<AstPtr> stmts;
    skip_newlines();
    while (peek().kind != TokKind::Eof) {
        stmts.push_back(parse_expr());
        // Statement separators: newline or semicolon. Both are optional
        // before '}' or EOF.
        if (peek().kind != TokKind::Eof && peek().kind != TokKind::RBrace) {
            if (peek().kind == TokKind::Newline || peek().kind == TokKind::Semicolon) {
                skip_newlines();
            } else {
                error("expected newline or ';' between statements, got '"
                      + std::string(tok_name(peek().kind)) + "'");
            }
        }
    }
    return std::make_unique<BlockAst>(std::move(stmts));
}

// ----- Expression precedence climbing -----

int Parser::op_precedence(TokKind k) const noexcept {
    switch (k) {
        case TokKind::Question:    return 1;   // ?help
        case TokKind::Assign:
        case TokKind::SuperAssign:
        case TokKind::RightAssign:
        case TokKind::RightSuperAssign:
        case TokKind::Equal:       return 2;   // assignment (right-assoc)
        case TokKind::Tilde:       return 3;   // formula
        case TokKind::Or:
        case TokKind::PipePipe:    return 4;
        case TokKind::And:
        case TokKind::AmdAmp:      return 5;
        case TokKind::Not:         return 6;   // unary !
        case TokKind::EQ:
        case TokKind::NE:
        case TokKind::_LT:
        case TokKind::_GT:
        case TokKind::LE:
        case TokKind::GE:          return 7;
        case TokKind::Colon:       return 8;
        case TokKind::Plus:
        case TokKind::Minus:       return 9;
        case TokKind::Star:
        case TokKind::Slash:       return 10;
        case TokKind::Caret:       return 11;  // right-assoc
        case TokKind::Dollar:
        case TokKind::At:          return 12;
        default:                   return -1;
    }
}

AstPtr Parser::parse_expr() { return parse_assign(); }

AstPtr Parser::parse_assign() {
    AstPtr lhs = parse_tilde();
    TokKind k = peek().kind;
    if (k == TokKind::Assign || k == TokKind::SuperAssign ||
        k == TokKind::Equal || k == TokKind::RightAssign || k == TokKind::RightSuperAssign) {
        bool super = (k == TokKind::SuperAssign || k == TokKind::RightSuperAssign);
        bool right = (k == TokKind::RightAssign || k == TokKind::RightSuperAssign);
        advance();
        AstPtr rhs = parse_assign();
        if (right) std::swap(lhs, rhs);
        return std::make_unique<AssignAst>(std::move(lhs), std::move(rhs), super);
    }
    return lhs;
}

AstPtr Parser::parse_tilde() {
    if (peek().kind == TokKind::Tilde) {
        advance();
        AstPtr operand = parse_tilde();
        return std::make_unique<UnaryOpAst>("~", std::move(operand));
    }
    return parse_or();
}

AstPtr Parser::parse_or() {
    AstPtr lhs = parse_and();
    while (peek().kind == TokKind::Or || peek().kind == TokKind::PipePipe) {
        bool dbl = (peek().kind == TokKind::Or);
        advance();
        AstPtr rhs = parse_and();
        lhs = std::make_unique<BinOpAst>(dbl ? "||" : "|", std::move(lhs), std::move(rhs));
    }
    return lhs;
}

AstPtr Parser::parse_and() {
    AstPtr lhs = parse_not();
    while (peek().kind == TokKind::And || peek().kind == TokKind::AmdAmp) {
        bool dbl = (peek().kind == TokKind::And);
        advance();
        AstPtr rhs = parse_not();
        lhs = std::make_unique<BinOpAst>(dbl ? "&&" : "&", std::move(lhs), std::move(rhs));
    }
    return lhs;
}

AstPtr Parser::parse_not() {
    if (peek().kind == TokKind::Not) {
        advance();
        AstPtr operand = parse_not();
        return std::make_unique<UnaryOpAst>("!", std::move(operand));
    }
    return parse_comparison();
}

AstPtr Parser::parse_comparison() {
    AstPtr lhs = parse_range();
    while (true) {
        TokKind k = peek().kind;
        std::string op;
        switch (k) {
            case TokKind::EQ: op = "=="; break;
            case TokKind::NE: op = "!="; break;
            case TokKind::_LT: op = "<"; break;
            case TokKind::_GT: op = ">"; break;
            case TokKind::LE: op = "<="; break;
            case TokKind::GE: op = ">="; break;
            default: return lhs;
        }
        advance();
        AstPtr rhs = parse_range();
        lhs = std::make_unique<BinOpAst>(op, std::move(lhs), std::move(rhs));
    }
}

AstPtr Parser::parse_range() {
    AstPtr lhs = parse_additive();
    while (peek().kind == TokKind::Colon) {
        advance();
        AstPtr rhs = parse_additive();
        lhs = std::make_unique<BinOpAst>(":", std::move(lhs), std::move(rhs));
    }
    return lhs;
}

AstPtr Parser::parse_additive() {
    AstPtr lhs = parse_multiplicative();
    while (peek().kind == TokKind::Plus || peek().kind == TokKind::Minus) {
        std::string op = (peek().kind == TokKind::Plus) ? "+" : "-";
        advance();
        AstPtr rhs = parse_multiplicative();
        lhs = std::make_unique<BinOpAst>(op, std::move(lhs), std::move(rhs));
    }
    return lhs;
}

AstPtr Parser::parse_multiplicative() {
    AstPtr lhs = parse_power();
    while (peek().kind == TokKind::Star || peek().kind == TokKind::Slash) {
        std::string op = (peek().kind == TokKind::Star) ? "*" : "/";
        advance();
        AstPtr rhs = parse_power();
        lhs = std::make_unique<BinOpAst>(op, std::move(lhs), std::move(rhs));
    }
    return lhs;
}

AstPtr Parser::parse_power() {
    AstPtr lhs = parse_unary();
    if (peek().kind == TokKind::Caret) {
        advance();
        AstPtr rhs = parse_power();  // right-assoc
        return std::make_unique<BinOpAst>("^", std::move(lhs), std::move(rhs));
    }
    return lhs;
}

AstPtr Parser::parse_unary() {
    if (peek().kind == TokKind::Minus) {
        advance();
        AstPtr operand = parse_unary();
        return std::make_unique<UnaryOpAst>("-", std::move(operand));
    }
    if (peek().kind == TokKind::Plus) {
        advance();
        return parse_unary();
    }
    return parse_postfix();
}

AstPtr Parser::parse_postfix() {
    AstPtr base = parse_primary();
    while (true) {
        TokKind k = peek().kind;
        if (k == TokKind::LParen) {
            base = parse_call_args(std::move(base));
        } else if (k == TokKind::LBracket) {
            base = parse_index_args(std::move(base), false);
        } else if (k == TokKind::DoubleLBracket) {
            base = parse_index_args(std::move(base), true);
        } else if (k == TokKind::Dollar) {
            advance();
            if (peek().kind == TokKind::Ident || peek().kind == TokKind::BacktickIdent ||
                peek().kind == TokKind::String) {
                std::string name = peek().text;
                advance();
                base = std::make_unique<DollarAst>(std::move(base), std::move(name));
            } else {
                error("expected identifier after '$'");
            }
        } else if (k == TokKind::At) {
            advance();
            if (peek().kind == TokKind::Ident || peek().kind == TokKind::BacktickIdent ||
                peek().kind == TokKind::String) {
                std::string name = peek().text;
                advance();
                base = std::make_unique<SlotAst>(std::move(base), std::move(name));
            } else {
                error("expected identifier after '@'");
            }
        } else {
            break;
        }
    }
    return base;
}

AstPtr Parser::parse_call_args(AstPtr callee) {
    expect(TokKind::LParen, "'('");
    std::vector<AstPtr> args;
    skip_newlines();
    if (peek().kind != TokKind::RParen) {
        while (true) {
            skip_newlines();
            // Named arg: IDENT '=' expr  (but '=' could also be assignment —
            // inside call args, '=' is treated as named-arg unless followed
            // by another '=').
            if (peek().kind == TokKind::Ident &&
                peek(1).kind == TokKind::Equal) {
                std::string name = peek().text;
                advance(); advance();
                skip_newlines();
                AstPtr v = parse_expr();
                args.push_back(std::make_unique<NamedArgAst>(std::move(name), std::move(v)));
            } else if (peek().kind == TokKind::BacktickIdent ||
                       (peek().kind == TokKind::String && peek(1).kind == TokKind::Equal)) {
                std::string name = peek().text;
                advance(); advance();
                skip_newlines();
                AstPtr v = parse_expr();
                args.push_back(std::make_unique<NamedArgAst>(std::move(name), std::move(v)));
            } else if (peek().kind == TokKind::Comma || peek().kind == TokKind::RParen) {
                // missing argument
                args.push_back(std::make_unique<MissingArgAst>());
            } else {
                args.push_back(parse_expr());
            }
            skip_newlines();
            if (accept(TokKind::Comma)) continue;
            break;
        }
    }
    expect(TokKind::RParen, "')'");
    return std::make_unique<CallAst>(std::move(callee), std::move(args));
}

AstPtr Parser::parse_index_args(AstPtr base, bool dbl) {
    advance();  // consume '[' or '[['
    std::vector<AstPtr> args;
    skip_newlines();
    if (peek().kind != (dbl ? TokKind::DoubleRBracket : TokKind::RBracket)) {
        while (true) {
            skip_newlines();
            if (peek().kind == TokKind::Comma ||
                peek().kind == (dbl ? TokKind::DoubleRBracket : TokKind::RBracket)) {
                args.push_back(std::make_unique<MissingArgAst>());
            } else {
                args.push_back(parse_expr());
            }
            skip_newlines();
            if (accept(TokKind::Comma)) continue;
            break;
        }
    }
    expect(dbl ? TokKind::DoubleRBracket : TokKind::RBracket,
           dbl ? "']]'" : "']'");
    return std::make_unique<IndexAst>(std::move(base), std::move(args), dbl);
}

AstPtr Parser::parse_function() {
    expect(TokKind::Kw_function, "'function'");
    expect(TokKind::LParen, "'('");
    std::vector<FunctionAst::Param> params;
    skip_newlines();
    if (peek().kind != TokKind::RParen) {
        while (true) {
            skip_newlines();
            if (peek().kind != TokKind::Ident && peek().kind != TokKind::BacktickIdent &&
                peek().kind != TokKind::String) {
                error("expected parameter name");
            }
            std::string name = peek().text;
            advance();
            AstPtr def;
            if (accept(TokKind::Equal)) {
                def = parse_expr();
            }
            params.push_back({std::move(name), std::move(def)});
            skip_newlines();
            if (accept(TokKind::Comma)) continue;
            break;
        }
    }
    expect(TokKind::RParen, "')'");
    skip_newlines();
    AstPtr body = parse_expr();
    return std::make_unique<FunctionAst>(std::move(params), std::move(body));
}

AstPtr Parser::parse_if() {
    expect(TokKind::Kw_if, "'if'");
    expect(TokKind::LParen, "'('");
    AstPtr cond = parse_expr();
    expect(TokKind::RParen, "')'");
    skip_newlines();
    AstPtr then_b = parse_expr();
    AstPtr else_b;
    // 'else' may be on the next line — but R requires that the 'else' be
    // on the same line as the closing '}' of the then-branch, OR on the
    // same line. We allow it after a newline if the next token is 'else'.
    size_t saved = pos_;
    while (peek().kind == TokKind::Newline) advance();
    if (accept(TokKind::Kw_else)) {
        skip_newlines();
        else_b = parse_expr();
    } else {
        pos_ = saved;
    }
    return std::make_unique<IfAst>(std::move(cond), std::move(then_b), std::move(else_b));
}

AstPtr Parser::parse_for() {
    expect(TokKind::Kw_for, "'for'");
    expect(TokKind::LParen, "'('");
    if (peek().kind != TokKind::Ident) error("expected variable name in for");
    std::string var = peek().text;
    advance();
    expect(TokKind::Kw_in, "'in'");
    AstPtr seq = parse_expr();
    expect(TokKind::RParen, "')'");
    skip_newlines();
    AstPtr body = parse_expr();
    return std::make_unique<ForAst>(std::move(var), std::move(seq), std::move(body));
}

AstPtr Parser::parse_while() {
    expect(TokKind::Kw_while, "'while'");
    expect(TokKind::LParen, "'('");
    AstPtr cond = parse_expr();
    expect(TokKind::RParen, "')'");
    skip_newlines();
    AstPtr body = parse_expr();
    return std::make_unique<WhileAst>(std::move(cond), std::move(body));
}

AstPtr Parser::parse_repeat() {
    expect(TokKind::Kw_repeat, "'repeat'");
    skip_newlines();
    AstPtr body = parse_expr();
    return std::make_unique<RepeatAst>(std::move(body));
}

AstPtr Parser::parse_block() {
    expect(TokKind::LBrace, "'{'");
    std::vector<AstPtr> stmts;
    skip_newlines();
    while (peek().kind != TokKind::RBrace && peek().kind != TokKind::Eof) {
        stmts.push_back(parse_expr());
        if (peek().kind != TokKind::RBrace && peek().kind != TokKind::Eof) {
            if (peek().kind == TokKind::Newline || peek().kind == TokKind::Semicolon) {
                skip_newlines();
            } else {
                error("expected newline, ';', or '}' in block");
            }
        }
    }
    expect(TokKind::RBrace, "'}'");
    return std::make_unique<BlockAst>(std::move(stmts));
}

AstPtr Parser::parse_primary() {
    Token const& t = peek();
    switch (t.kind) {
        case TokKind::Real:
            advance();
            return std::make_unique<RealAst>(t.real_val);
        case TokKind::Integer:
            advance();
            return std::make_unique<IntegerAst>(static_cast<int32_t>(t.int_val));
        case TokKind::String:
            advance();
            return std::make_unique<StringAst>(t.text);
        case TokKind::Ident:
        case TokKind::BacktickIdent:
            advance();
            return std::make_unique<SymbolAst>(t.text);
        case TokKind::Kw_NULL:    advance(); return std::make_unique<NilAst>();
        case TokKind::Kw_TRUE:    advance(); return std::make_unique<LogicalAst>(1);
        case TokKind::Kw_FALSE:   advance(); return std::make_unique<LogicalAst>(0);
        case TokKind::Kw_NA:
        case TokKind::Kw_NaInt:
        case TokKind::Kw_NaReal:
        case TokKind::Kw_NaString:
            advance();
            return std::make_unique<IntegerAst>(kNaInt);
        case TokKind::Kw_Inf:    advance(); return std::make_unique<RealAst>(__builtin_inf());
        case TokKind::Kw_NaN:    advance(); return std::make_unique<RealAst>(__builtin_nan(""));
        case TokKind::Kw_function: return parse_function();
        case TokKind::Kw_if:      return parse_if();
        case TokKind::Kw_for:     return parse_for();
        case TokKind::Kw_while:   return parse_while();
        case TokKind::Kw_repeat:  return parse_repeat();
        case TokKind::Kw_break:   advance(); return std::make_unique<BreakAst>();
        case TokKind::Kw_next:    advance(); return std::make_unique<NextAst>();
        case TokKind::Kw_return: {
            advance();
            if (peek().kind == TokKind::LParen) {
                advance();
                AstPtr v;
                skip_newlines();
                if (peek().kind != TokKind::RParen) v = parse_expr();
                expect(TokKind::RParen, "')'");
                return std::make_unique<ReturnAst>(std::move(v));
            }
            return std::make_unique<ReturnAst>(nullptr);
        }
        case TokKind::Kw_quote: {
            advance();
            // quote(x) — special: the argument is captured unevaluated.
            // For simplicity we just parse the inner expression as a
            // call to `quote` and let the interpreter handle the
            // special-form semantics.
            if (peek().kind == TokKind::LParen) {
                advance();
                std::vector<AstPtr> args;
                skip_newlines();
                if (peek().kind != TokKind::RParen) {
                    args.push_back(parse_expr());
                }
                expect(TokKind::RParen, "')'");
                auto callee = std::make_unique<SymbolAst>("quote");
                return std::make_unique<CallAst>(std::move(callee), std::move(args));
            }
            return std::make_unique<SymbolAst>("quote");
        }
        case TokKind::Kw_missing: {
            advance();
            auto callee = std::make_unique<SymbolAst>("missing");
            std::vector<AstPtr> args;
            if (peek().kind == TokKind::LParen) {
                advance();
                skip_newlines();
                if (peek().kind != TokKind::RParen) {
                    args.push_back(parse_expr());
                }
                expect(TokKind::RParen, "')'");
            }
            return std::make_unique<CallAst>(std::move(callee), std::move(args));
        }
        case TokKind::LParen: {
            advance();
            AstPtr e = parse_expr();
            expect(TokKind::RParen, "')'");
            return std::make_unique<ParenAst>(std::move(e));
        }
        case TokKind::LBrace: return parse_block();
        default:
            error("unexpected token '" + std::string(tok_name(t.kind)) + "' in expression");
    }
}

}  // namespace rjit
