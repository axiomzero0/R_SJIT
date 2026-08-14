// rjit/core/closure.hpp - Closures, builtins, and special forms
#pragma once
#include <cstdint>
#include <string>
#include "rjit/core/value.hpp"
#include "rjit/core/gc.hpp"
#include "rjit/core/environment.hpp"

namespace rjit {

class BytecodeFunction;
class Context;

// A user-defined function (closure): code + captured environment.
class Closure : public HeapObject {
public:
    RJIT_DECLARE_GC_NEW(Closure)

    Closure(BytecodeFunction* code, Environment* env);

    BytecodeFunction* code() const noexcept { return code_; }
    Environment*      env()  const noexcept { return env_; }

    void trace(Visitor& v) const override;

private:
    BytecodeFunction* code_;
    Environment*       env_;
};

// A C-implemented primitive. Builtins evaluate their arguments
// before being called; specials receive unevaluated argument AST.
class Builtin : public HeapObject {
public:
    RJIT_DECLARE_GC_NEW(Builtin)

    using Fn = Value (*)(Context& ctx, Value* args, uint32_t nargs);

    Builtin(std::string name, Fn fn, bool is_special = false);

    std::string const& name() const noexcept { return name_; }
    Fn   impl() const noexcept { return fn_; }
    bool is_special() const noexcept { return is_special_; }

    void trace(Visitor& v) const override {}

private:
    std::string name_;
    Fn          fn_;
    bool        is_special_;
};

}  // namespace rjit
