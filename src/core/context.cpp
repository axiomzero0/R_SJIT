// rjit/core/context.cpp
#include "rjit/core/context.hpp"
#include "rjit/core/gc.hpp"
#include "rjit/core/error.hpp"
#include "rjit/core/environment.hpp"
#include "rjit/vm/interpreter.hpp"
#include "rjit/feedback/feedback.hpp"
#include "rjit/jit/tier_manager.hpp"
#include "rjit/jit/deopt.hpp"
#include <stdexcept>

namespace rjit {

namespace {
thread_local Context* tls_ctx = nullptr;
}

SymbolTable::SymbolTable() {
    // Pre-intern a few common symbols to keep ids stable for the JIT.
    intern("nil");
    intern("TRUE");
    intern("FALSE");
    intern("NA");
    intern("T");
    intern("F");
    intern("+");
    intern("-");
    intern("*");
    intern("/");
    intern("^");
    intern("<");
    intern(">");
    intern("<=");
    intern(">=");
    intern("==");
    intern("!=");
    intern("&");
    intern("|");
    intern("!");
    intern(":");
    intern("c");
    intern("list");
    intern("function");
    intern("if");
    intern("else");
    intern("for");
    intern("while");
    intern("repeat");
    intern("break");
    intern("next");
    intern("return");
    intern("in");
    intern("quote");
    intern("substitute");
    intern("missing");
    intern("<-");
    intern("<<-");
    intern("=");
    intern("->");
    intern("->>");
    intern("$");
    intern("@");
    intern("[");
    intern("]");
    intern("[[");
    intern("]]");
    intern("length");
    intern("sum");
    intern("mean");
    intern("min");
    intern("max");
    intern("sapply");
    intern("lapply");
    intern("print");
    intern("cat");
    intern("paste");
    intern("seq");
    intern("seq_along");
    intern("seq_len");
}

uint32_t SymbolTable::intern(std::string_view name) {
    auto it = map_.find(std::string(name));
    if (it != map_.end()) return it->second;
    uint32_t id = static_cast<uint32_t>(names_.size());
    std::string s(name);
    map_[s] = id;
    names_.push_back(s);
    return id;
}

std::string_view SymbolTable::name_of(uint32_t id) const {
    if (id < names_.size()) return names_[id];
    return {};
}

Context::Context() {
    gc_       = std::make_unique<GC>();
    install_gc_for_thread(gc_.get());   // must precede any heap allocation
    symbols_  = std::make_unique<SymbolTable>();
    reset_environments();
    global_env_ = get_global_env();
    base_env_   = get_base_env();
    feedback_ = std::make_unique<FeedbackEngine>();
    deopt_    = std::make_unique<DeoptContext>();
    tiers_    = std::make_unique<TierManager>(*feedback_, *deopt_);
    interp_   = std::make_unique<Interpreter>(*this);
    tls_ctx   = this;
}

Context::~Context() {
    if (tls_ctx == this) tls_ctx = nullptr;
}

Context& current_context() {
    if (!tls_ctx) {
        std::fprintf(stderr, "current_context() called with no Context installed\n");
        std::abort();
    }
    return *tls_ctx;
}

void set_current_context(Context* ctx) { tls_ctx = ctx; }

[[noreturn]] void Context::raise_error(std::string msg) {
    last_error_ = std::move(msg);
    // For now: throw a C++ exception that the interpreter catches at
    // the nearest tryCatch frame. This is simpler than setjmp/longjmp
    // and works fine for the initial implementation.
    throw RJitError(last_error_);
}

}  // namespace rjit
