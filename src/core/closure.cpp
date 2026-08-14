// rjit/core/closure.cpp
#include "rjit/core/closure.hpp"
#include "rjit/bytecode/module.hpp"

namespace rjit {

Closure::Closure(BytecodeFunction* code, Environment* env)
    : HeapObject(TypeTag::kClosure), code_(code), env_(env) {}

void Closure::trace(Visitor& v) const {
    HeapObject* p = const_cast<HeapObject*>(static_cast<HeapObject const*>(code_));
    v.visit_heap(p);
    const_cast<Closure*>(this)->code_ = static_cast<BytecodeFunction*>(p);
    p = const_cast<HeapObject*>(static_cast<HeapObject const*>(env_));
    v.visit_heap(p);
    const_cast<Closure*>(this)->env_ = static_cast<Environment*>(p);
}

Builtin::Builtin(std::string name, Fn fn, bool is_special)
    : HeapObject(is_special ? TypeTag::kSpecial : TypeTag::kBuiltin),
      name_(std::move(name)), fn_(fn), is_special_(is_special) {}

}  // namespace rjit
