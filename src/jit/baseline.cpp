// rjit/jit/baseline.cpp
//
// Tier-1 baseline JIT.
//
// This file currently contains the scaffolding for the baseline JIT.
// The actual machine-code emission uses asmjit, but the initial
// implementation simply returns nullptr (signaling "fall back to
// interpreter") so that we can validate the rest of the pipeline.
// Subsequent iterations will replace this with real per-opcode
// machine code generation.

#include "rjit/jit/baseline.hpp"
#include "rjit/core/context.hpp"
#include "rjit/vm/frame.hpp"
#include "rjit/vm/interpreter.hpp"
#include "rjit/bytecode/module.hpp"
#include "rjit/core/promise.hpp"

// asmjit (for the upcoming real implementation)
#include <asmjit/asmjit.h>
#include <asmjit/x86/x86assembler.h>

namespace rjit {

// ---------------------------------------------------------------------------
// Runtime helpers called from JIT-compiled code.
//
// These are extern "C" so that the JIT can call them with a simple
// `call` instruction without name mangling. Each helper takes a
// pointer to the current Frame (or relevant subset) and performs
// the actual operation, returning the result.
// ---------------------------------------------------------------------------

extern "C" {

Value rjit_helper_load_var(void* env_ptr, uint32_t sym_id) {
    Environment* env = static_cast<Environment*>(env_ptr);
    Value out;
    if (!env->lookup(sym_id, &out)) {
        current_context().raise_error("object not found");
    }
    return out;
}

void rjit_helper_store_var(void* env_ptr, uint32_t sym_id, Value v) {
    Environment* env = static_cast<Environment*>(env_ptr);
    env->define(sym_id, v);
}

Value rjit_helper_add(Value a, Value b) {
    a = force_if_promise(a);
    b = force_if_promise(b);
    if (a.is_real() && b.is_real()) return Value::real(a.as_real() + b.as_real());
    if (a.is_integer() && b.is_integer()) {
        int64_t r = (int64_t)a.as_integer() + (int64_t)b.as_integer();
        if (r < INT32_MIN || r > INT32_MAX) return Value::real((double)r);
        return Value::integer((int32_t)r);
    }
    double av = a.is_real() ? a.as_real() :
                a.is_integer() ? (a.as_integer() == kNaInt ? kNaReal : (double)a.as_integer()) : 0.0;
    double bv = b.is_real() ? b.as_real() :
                b.is_integer() ? (b.as_integer() == kNaInt ? kNaReal : (double)b.as_integer()) : 0.0;
    return Value::real(av + bv);
}

Value rjit_helper_call(Value callee, Value* args, uint32_t nargs, void* env_ptr) {
    Environment* env = static_cast<Environment*>(env_ptr);
    return current_context().interpreter().call_function(callee, args, nargs, env);
}

void rjit_helper_deopt(uint32_t safepoint_id) {
    (void)safepoint_id;
    current_context().raise_error("deopt not yet implemented in JIT");
}

}  // extern "C"

// ---------------------------------------------------------------------------
// BaselineJIT implementation
// ---------------------------------------------------------------------------

BaselineJIT::BaselineJIT(Context& ctx) : ctx_(ctx) {}

JitCode* BaselineJIT::compile(BytecodeFunction* fn) {
    // Initial implementation: return nullptr to signal "no JIT code
    // available, use the interpreter". A real implementation would
    // use asmjit to emit per-opcode machine code here.
    //
    // The asmjit-based code generator is sketched in comments below
    // to document the intended design:
    //
    //   JitRuntime rt;
    //   CodeHolder code;
    //   code.init(rt.environment());
    //   x86::Assembler a(&code);
    //
    //   // Prologue:
    //   //   push rbp ; mov rbp, rsp
    //   //   push rbx, r12, r13, r14, r15
    //   //   mov rbx, [rdi + offsetof(Frame, regs)]     ; Value* regs
    //   //   mov r12, [rdi + offsetof(Frame, fn)]       ; BytecodeFunction* fn
    //   //   mov r13, [rdi + offsetof(Frame, env)]      ; Environment* env
    //   //   mov r15, rdi                                ; Frame* frame
    //
    //   // Per-instruction emission:
    //   //   for each Instr in fn->code:
    //   //     bind a label
    //   //     switch on op:
    //   //       LOAD_REAL: load constant from fn->constants[k],
    //   //                  store into regs[rdest]
    //   //       ADD: load regs[ra], regs[rb], call rjit_helper_add,
    //   //            store result into regs[rdest]
    //   //       JUMP: jmp labels[k]
    //   //       ...
    //
    //   // Epilogue:
    //   //   mov rax, regs[0].bits_   ; return value
    //   //   pop r15, r14, r13, r12, rbx, rbp
    //   //   ret
    //
    //   JitCode* jc = new JitCode();
    //   jc->source = fn;
    //   rt.add(&jc->entry, &code);
    //   return jc;

    (void)fn;
    return nullptr;
}

void JitCode::trace(Visitor& v) const {
    HeapObject* p = const_cast<HeapObject*>(static_cast<HeapObject const*>(source));
    v.visit_heap(p);
    const_cast<JitCode*>(this)->source = static_cast<BytecodeFunction*>(p);
}

JitCode::JitCode() : HeapObject(TypeTag::kJitCode) {}
JitCode::~JitCode() {}

}  // namespace rjit
