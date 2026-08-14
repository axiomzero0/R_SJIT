// rjit/bytecode/module.cpp
#include "rjit/bytecode/module.hpp"
#include "rjit/core/environment.hpp"
#include <algorithm>

namespace rjit {

BytecodeFunction::BytecodeFunction()
    : HeapObject(TypeTag::kBytecodeFn) {
    debug = std::make_unique<DebugInfo>();
}

void BytecodeFunction::trace(Visitor& v) const {
    for (auto& c : const_cast<std::vector<Value>&>(constants)) v.visit(c);
}

BytecodeBuilder::BytecodeBuilder() {
    fn_ = new BytecodeFunction();
}

uint32_t BytecodeBuilder::alloc_reg() {
    return fn_->nregs++;
}

uint32_t BytecodeBuilder::emit(Op op, uint32_t rdest, uint32_t ra, uint32_t rb, uint32_t k, uint16_t flags) {
    Instr i;
    i.op = op; i.flags = flags; i.rdest = rdest; i.ra = ra; i.rb = rb; i.k = k;
    fn_->code.push_back(i);
    return static_cast<uint32_t>(fn_->code.size() - 1);
}

void BytecodeBuilder::patch_k(uint32_t instr_idx, uint32_t new_k) {
    fn_->code.at(instr_idx).k = new_k;
}

uint32_t BytecodeBuilder::add_constant(Value v) {
    for (size_t i = 0; i < fn_->constants.size(); ++i) {
        if (fn_->constants[i] == v) return static_cast<uint32_t>(i);
    }
    uint32_t idx = static_cast<uint32_t>(fn_->constants.size());
    fn_->constants.push_back(v);
    return idx;
}

void BytecodeBuilder::use_symbol(uint32_t sym_id) {
    auto it = std::find(fn_->symbols_used.begin(), fn_->symbols_used.end(), sym_id);
    if (it == fn_->symbols_used.end()) fn_->symbols_used.push_back(sym_id);
}

BytecodeFunction* BytecodeBuilder::finalize(std::string name, uint32_t nparams, uint32_t nregs) {
    fn_->name = std::move(name);
    fn_->nparams = nparams;
    fn_->nregs = std::max(fn_->nregs, nregs);
    // Ensure the function ends with HALT
    if (fn_->code.empty() || fn_->code.back().op != Op::HALT) {
        Instr h; h.op = Op::HALT;
        fn_->code.push_back(h);
    }
    // Pre-allocate inline cache arrays so the interpreter never
    // needs to resize them during execution (which would invalidate
    // any cached pointers).
    size_t code_size = fn_->code.size();
    fn_->ic_envs.resize(code_size, nullptr);
    fn_->ic_shapes.resize(code_size, 0);
    fn_->ic_slots.resize(code_size, 0);
    return fn_;
}

}  // namespace rjit
