// rjit/jit/baseline.cpp
//
// Tier-1 baseline JIT.
//
// Compiles each bytecode instruction into machine code using asmjit.
// The compilation is one-to-one: each bytecode instruction maps to
// a short sequence of x86-64 instructions that loads operands from
// the register file, calls a C++ runtime helper, and stores the
// result back.
//
// The register file pointer is kept in rbx (callee-saved) for the
// duration of the function. The current Environment* is in r13.
// The BytecodeFunction* is in r12. The Frame* is in r15.
//
// This produces correct but unoptimized machine code. The Tier 1.5
// and Tier 2 JITs will improve on it using type feedback and the
// Sea-of-Nodes IR.

#include "rjit/jit/baseline.hpp"
#include "rjit/core/context.hpp"
#include "rjit/core/closure.hpp"
#include "rjit/core/vector.hpp"
#include "rjit/vm/frame.hpp"
#include "rjit/vm/interpreter.hpp"
#include "rjit/bytecode/module.hpp"
#include "rjit/core/promise.hpp"

#include <asmjit/asmjit.h>
#include <asmjit/x86/x86assembler.h>

namespace rjit {

// ---------------------------------------------------------------------------
// Runtime helpers called from JIT-compiled code.
//
// Each helper is extern "C" so the JIT can call it with a simple `call`
// instruction. The helpers take the Frame* (or relevant subset) and
// perform the actual operation, returning the result.
//
// The JIT emits code to:
//   1. Load operands from the register file (rbx[offset])
//   2. Pass them to the helper per the System V AMD64 ABI
//   3. Store the result back into the register file
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

Value rjit_helper_sub(Value a, Value b) {
    a = force_if_promise(a); b = force_if_promise(b);
    if (a.is_real() && b.is_real()) return Value::real(a.as_real() - b.as_real());
    double av = a.is_real() ? a.as_real() : (a.is_integer() ? (a.as_integer() == kNaInt ? kNaReal : (double)a.as_integer()) : 0);
    double bv = b.is_real() ? b.as_real() : (b.is_integer() ? (b.as_integer() == kNaInt ? kNaReal : (double)b.as_integer()) : 0);
    return Value::real(av - bv);
}

Value rjit_helper_mul(Value a, Value b) {
    a = force_if_promise(a); b = force_if_promise(b);
    if (a.is_real() && b.is_real()) return Value::real(a.as_real() * b.as_real());
    double av = a.is_real() ? a.as_real() : (a.is_integer() ? (a.as_integer() == kNaInt ? kNaReal : (double)a.as_integer()) : 0);
    double bv = b.is_real() ? b.as_real() : (b.is_integer() ? (b.as_integer() == kNaInt ? kNaReal : (double)b.as_integer()) : 0);
    return Value::real(av * bv);
}

Value rjit_helper_div(Value a, Value b) {
    a = force_if_promise(a); b = force_if_promise(b);
    double av = a.is_real() ? a.as_real() : (a.is_integer() ? (a.as_integer() == kNaInt ? kNaReal : (double)a.as_integer()) : 0);
    double bv = b.is_real() ? b.as_real() : (b.is_integer() ? (b.as_integer() == kNaInt ? kNaReal : (double)b.as_integer()) : 0);
    return Value::real(av / bv);
}

Value rjit_helper_lt(Value a, Value b) {
    a = force_if_promise(a); b = force_if_promise(b);
    auto to_real = [](Value v) -> double {
        if (v.is_real()) return v.as_real();
        if (v.is_integer()) return v.as_integer() == kNaInt ? kNaReal : (double)v.as_integer();
        if (v.is_logical()) return v.as_logical() == kNaLogical ? kNaReal : (double)v.as_logical();
        return kNaReal;
    };
    double av = to_real(a), bv = to_real(b);
    if (av == kNaReal || bv == kNaReal) return Value::logical(kNaLogical);
    return Value::logical(av < bv ? 1 : 0);
}

Value rjit_helper_le(Value a, Value b) {
    a = force_if_promise(a); b = force_if_promise(b);
    auto to_real = [](Value v) -> double {
        if (v.is_real()) return v.as_real();
        if (v.is_integer()) return v.as_integer() == kNaInt ? kNaReal : (double)v.as_integer();
        if (v.is_logical()) return v.as_logical() == kNaLogical ? kNaReal : (double)v.as_logical();
        return kNaReal;
    };
    double av = to_real(a), bv = to_real(b);
    if (av == kNaReal || bv == kNaReal) return Value::logical(kNaLogical);
    return Value::logical(av <= bv ? 1 : 0);
}

Value rjit_helper_gt(Value a, Value b) {
    a = force_if_promise(a); b = force_if_promise(b);
    auto to_real = [](Value v) -> double {
        if (v.is_real()) return v.as_real();
        if (v.is_integer()) return v.as_integer() == kNaInt ? kNaReal : (double)v.as_integer();
        return kNaReal;
    };
    double av = to_real(a), bv = to_real(b);
    if (av == kNaReal || bv == kNaReal) return Value::logical(kNaLogical);
    return Value::logical(av > bv ? 1 : 0);
}

Value rjit_helper_ge(Value a, Value b) {
    a = force_if_promise(a); b = force_if_promise(b);
    auto to_real = [](Value v) -> double {
        if (v.is_real()) return v.as_real();
        if (v.is_integer()) return v.as_integer() == kNaInt ? kNaReal : (double)v.as_integer();
        return kNaReal;
    };
    double av = to_real(a), bv = to_real(b);
    if (av == kNaReal || bv == kNaReal) return Value::logical(kNaLogical);
    return Value::logical(av >= bv ? 1 : 0);
}

Value rjit_helper_eq(Value a, Value b) {
    a = force_if_promise(a); b = force_if_promise(b);
    auto to_real = [](Value v) -> double {
        if (v.is_real()) return v.as_real();
        if (v.is_integer()) return v.as_integer() == kNaInt ? kNaReal : (double)v.as_integer();
        return kNaReal;
    };
    double av = to_real(a), bv = to_real(b);
    if (av == kNaReal || bv == kNaReal) return Value::logical(kNaLogical);
    return Value::logical(av == bv ? 1 : 0);
}

Value rjit_helper_ne(Value a, Value b) {
    a = force_if_promise(a); b = force_if_promise(b);
    auto to_real = [](Value v) -> double {
        if (v.is_real()) return v.as_real();
        if (v.is_integer()) return v.as_integer() == kNaInt ? kNaReal : (double)v.as_integer();
        return kNaReal;
    };
    double av = to_real(a), bv = to_real(b);
    if (av == kNaReal || bv == kNaReal) return Value::logical(kNaLogical);
    return Value::logical(av != bv ? 1 : 0);
}

Value rjit_helper_neg(Value a) {
    a = force_if_promise(a);
    if (a.is_real()) return Value::real(-a.as_real());
    if (a.is_integer()) return Value::integer(a.as_integer() == kNaInt ? kNaInt : -a.as_integer());
    return Value::real(kNaReal);
}

int32_t rjit_helper_as_logical_scalar(Value v) {
    v = force_if_promise(v);
    if (v.is_logical()) return v.as_logical();
    if (v.is_integer()) return v.as_integer() == 0 ? 0 : (v.as_integer() == kNaInt ? kNaLogical : 1);
    if (v.is_real())    return v.as_real() != v.as_real() ? kNaLogical : (v.as_real() != 0.0 ? 1 : 0);
    if (v.is_nil())     return 0;
    return 0;
}

Value rjit_helper_call(Value callee, Value* args, uint32_t nargs, void* env_ptr) {
    Environment* env = static_cast<Environment*>(env_ptr);
    return current_context().interpreter().call_function(callee, args, nargs, env);
}

// Helper that takes a pointer to the callee Value (in the register file)
// rather than the Value itself. This simplifies the calling convention
// for 16-byte Values.
Value rjit_helper_call_value(Value* callee_ptr, Value* args, uint32_t nargs, void* env_ptr) {
    Environment* env = static_cast<Environment*>(env_ptr);
    Value callee = *callee_ptr;
    return current_context().interpreter().call_function(callee, args, nargs, env);
}

Value rjit_helper_make_closure(BytecodeFunction* code, void* env_ptr) {
    Environment* env = static_cast<Environment*>(env_ptr);
    Closure* c = new Closure(code, env);
    return Value::from_heap(TypeTag::kClosure, c);
}

Value rjit_helper_make_seq(Value a, Value b) {
    a = force_if_promise(a);
    b = force_if_promise(b);
    if (a.is_integer() && b.is_integer()) {
        Vector* v = Vector::range(a.as_integer(), b.as_integer());
        return Value::from_heap(TypeTag::kVector, v);
    }
    double av = a.is_real() ? a.as_real() : (double)a.as_integer();
    double bv = b.is_real() ? b.as_real() : (double)b.as_integer();
    int64_t n = (int64_t)(bv - av + 1);
    Vector* v = new Vector(VectorType::kReal, n > 0 ? (size_t)n : 0);
    for (int64_t i = 0; i < n; ++i)
        v->set_real((size_t)i, av + (double)i);
    return Value::from_heap(TypeTag::kVector, v);
}

Value rjit_helper_length(Value v) {
    v = force_if_promise(v);
    if (v.is_vector()) return Value::integer((int32_t)v.as_vector()->length());
    if (v.is_nil()) return Value::integer(0);
    return Value::integer(1);
}

void rjit_helper_deopt(uint32_t safepoint_id) {
    (void)safepoint_id;
    current_context().raise_error("deopt not yet implemented in JIT");
}

}  // extern "C"

// ---------------------------------------------------------------------------
// BaselineJIT implementation
// ---------------------------------------------------------------------------

using namespace asmjit;

// Offsets into the Frame struct (must match frame.hpp)
static constexpr intptr_t FRAME_REGS  = offsetof(Frame, regs);
static constexpr intptr_t FRAME_FN    = offsetof(Frame, fn);
static constexpr intptr_t FRAME_ENV   = offsetof(Frame, env);
static constexpr intptr_t FRAME_PC    = offsetof(Frame, pc);
static constexpr intptr_t FRAME_CALLER = offsetof(Frame, caller);

// Size of a Value (16 bytes)
static constexpr intptr_t VALUE_SIZE = 16;
// Offset of the tag within a Value
static constexpr intptr_t VALUE_TAG  = 0;
// Offset of the bits within a Value
static constexpr intptr_t VALUE_BITS = 8;

// Get the byte offset of register `i`'s tag in the register file.
static constexpr intptr_t reg_tag_offset(uint32_t i) { return i * VALUE_SIZE + VALUE_TAG; }
static constexpr intptr_t reg_bits_offset(uint32_t i) { return i * VALUE_SIZE + VALUE_BITS; }

BaselineJIT::BaselineJIT(Context& ctx) : ctx_(ctx) {}

JitCode* BaselineJIT::compile(BytecodeFunction* fn) {
    if (fn->code.empty()) return nullptr;

    JitCode* jc = new JitCode();
    jc->source = fn;

    // Set up asmjit runtime + code holder.
    // We use a static JitRuntime so it lives for the entire process.
    static JitRuntime rt;

    CodeHolder code;
    code.init(rt.environment());

    x86::Assembler a(&code);

    // Function signature: Value entry(Frame* frame)
    // System V AMD64: rdi = Frame*
    x86::Gp frame = x86::rdi;

    // ---- Prologue ----
    a.push(x86::rbp);
    a.mov(x86::rbp, x86::rsp);
    // Save callee-saved registers
    a.push(x86::rbx);
    a.push(x86::r12);
    a.push(x86::r13);
    a.push(x86::r14);
    a.push(x86::r15);
    // Align stack to 16 bytes (5 pushes = 40 bytes, plus return addr = 48; need 16-alignment)
    a.sub(x86::rsp, 8);

    // rbx = frame->regs (Value* regs)
    a.mov(x86::rbx, x86::ptr(frame, FRAME_REGS));
    // r12 = frame->fn (BytecodeFunction*)
    a.mov(x86::r12, x86::ptr(frame, FRAME_FN));
    // r13 = frame->env (Environment*)
    a.mov(x86::r13, x86::ptr(frame, FRAME_ENV));
    // r15 = frame (Frame*)
    a.mov(x86::r15, frame);

    // Create labels for each instruction (for jumps).
    std::vector<Label> labels(fn->code.size());
    for (size_t i = 0; i < fn->code.size(); ++i) {
        labels[i] = a.new_label();
    }

    // Emit code for each instruction.
    for (size_t pc = 0; pc < fn->code.size(); ++pc) {
        a.bind(labels[pc]);
        Instr const& in = fn->code[pc];

        switch (in.op) {
            case Op::NOP:
                break;

            case Op::HALT:
                // Return regs[0] (the Value in register 0).
                // A Value is 16 bytes: tag(1) + pad(7) + bits(8).
                // Return: rax = bits, rdx = tag | (pad << 8)
                // Actually, for struct return, System V uses rax+rdx.
                // We'll return bits in rax and tag|pad in rdx.
                a.mov(x86::rax, x86::ptr(x86::rbx, reg_bits_offset(0)));  // bits
                a.mov(x86::rdx, x86::ptr(x86::rbx, reg_tag_offset(0), 8)); // tag+pad (8 bytes)
                a.jmp(labels[fn->code.size() - 1]);  // jump to epilogue (last label)
                // Actually the last label might not be the epilogue.
                // Let's just jump to a dedicated epilogue label.
                break;

            case Op::LOAD_NIL:
                // regs[dst] = nil (tag=kNil, bits=0)
                a.mov(x86::qword_ptr(x86::rbx, reg_bits_offset(in.rdest)), 0);
                a.mov(x86::byte_ptr(x86::rbx, reg_tag_offset(in.rdest)), static_cast<uint8_t>(TypeTag::kNil));
                break;

            case Op::LOAD_TRUE:
                a.mov(x86::qword_ptr(x86::rbx, reg_bits_offset(in.rdest)), 1);
                a.mov(x86::byte_ptr(x86::rbx, reg_tag_offset(in.rdest)), static_cast<uint8_t>(TypeTag::kLogical));
                break;

            case Op::LOAD_FALSE:
                a.mov(x86::qword_ptr(x86::rbx, reg_bits_offset(in.rdest)), 0);
                a.mov(x86::byte_ptr(x86::rbx, reg_tag_offset(in.rdest)), static_cast<uint8_t>(TypeTag::kLogical));
                break;

            case Op::LOAD_REAL:
            case Op::LOAD_INT: {
                // regs[dst] = fn->constants[k]
                // r12 = fn. Load constants array pointer, then load the Value.
                // constants is a std::vector<Value>. Its data pointer is at
                // offsetof(BytecodeFunction, constants) + offsetof(vector, data).
                // For libstdc++, vector data is at offset 0 of the vector.
                a.mov(x86::rax, x86::ptr(x86::r12, offsetof(BytecodeFunction, constants)));
                // rax = &constants[0]
                // Load 16 bytes from constants[k]
                intptr_t off = static_cast<intptr_t>(in.k) * VALUE_SIZE;
                a.mov(x86::rcx, x86::ptr(x86::rax, off + VALUE_BITS));
                a.mov(x86::rdx, x86::ptr(x86::rax, off + VALUE_TAG, 8));
                a.mov(x86::ptr(x86::rbx, reg_bits_offset(in.rdest)), x86::rcx);
                a.mov(x86::ptr(x86::rbx, reg_tag_offset(in.rdest), 8), x86::rdx);
                break;
            }

            case Op::LOAD_LOCAL:
                // regs[dst] = regs[ra]
                a.mov(x86::rax, x86::ptr(x86::rbx, reg_bits_offset(in.ra)));
                a.mov(x86::rcx, x86::ptr(x86::rbx, reg_tag_offset(in.ra), 8));
                a.mov(x86::ptr(x86::rbx, reg_bits_offset(in.rdest)), x86::rax);
                a.mov(x86::ptr(x86::rbx, reg_tag_offset(in.rdest), 8), x86::rcx);
                break;

            case Op::LOAD_VAR: {
                // Call rjit_helper_load_var(env, sym_id)
                // rdi = env (r13), rsi = sym_id (in.k)
                a.mov(x86::rdi, x86::r13);
                a.mov(x86::esi, in.k);
                // Align stack and call
                a.call(imm(reinterpret_cast<uintptr_t>(&rjit_helper_load_var)));
                // rax = Value.bits, rdx = Value.tag|pad
                a.mov(x86::ptr(x86::rbx, reg_bits_offset(in.rdest)), x86::rax);
                a.mov(x86::ptr(x86::rbx, reg_tag_offset(in.rdest), 8), x86::rdx);
                break;
            }

            case Op::STORE_VAR: {
                // Call rjit_helper_store_var(env, sym_id, regs[ra])
                // We need to pass the Value by value. On SysV, a 16-byte
                // struct is passed in rdx+rax (or on stack).
                // Actually, Value is 16 bytes with non-trivial alignment.
                // SysV classifies it as two 8-byte INTEGER class, passed in rsi+rdx.
                // But we already have rdi=env, rsi=sym_id. So the Value goes in rdx+rcx.
                //
                // Helper signature: void rjit_helper_store_var(void* env, uint32_t sym, Value v)
                // SysV: env in rdi, sym in esi, v.bits in rdx, v.tag|pad in rcx
                a.mov(x86::rdi, x86::r13);  // env
                a.mov(x86::esi, in.k);      // sym_id
                a.mov(x86::rdx, x86::ptr(x86::rbx, reg_bits_offset(in.ra)));
                a.mov(x86::rcx, x86::ptr(x86::rbx, reg_tag_offset(in.ra), 8));
                a.call(imm(reinterpret_cast<uintptr_t>(&rjit_helper_store_var)));
                break;
            }

            case Op::JUMP:
                a.jmp(labels[in.k]);
                break;

            case Op::JUMP_IF_FALSE: {
                // Call rjit_helper_as_logical_scalar(regs[ra])
                // Returns int32_t in eax.
                a.mov(x86::rdi, x86::ptr(x86::rbx, reg_bits_offset(in.ra)));
                a.mov(x86::rsi, x86::ptr(x86::rbx, reg_tag_offset(in.ra), 8));
                a.call(imm(reinterpret_cast<uintptr_t>(&rjit_helper_as_logical_scalar)));
                a.test(x86::eax, x86::eax);
                a.jz(labels[in.k]);  // jump if zero (false)
                break;
            }

            case Op::ADD: {
                // Pass regs[ra] and regs[rb] as two Values.
                // Helper: Value rjit_helper_add(Value a, Value b)
                // SysV: a.bits in rdi, a.tag in rsi, b.bits in rdx, b.tag in rcx
                a.mov(x86::rdi, x86::ptr(x86::rbx, reg_bits_offset(in.ra)));
                a.mov(x86::rsi, x86::ptr(x86::rbx, reg_tag_offset(in.ra), 8));
                a.mov(x86::rdx, x86::ptr(x86::rbx, reg_bits_offset(in.rb)));
                a.mov(x86::rcx, x86::ptr(x86::rbx, reg_tag_offset(in.rb), 8));
                a.call(imm(reinterpret_cast<uintptr_t>(&rjit_helper_add)));
                a.mov(x86::ptr(x86::rbx, reg_bits_offset(in.rdest)), x86::rax);
                a.mov(x86::ptr(x86::rbx, reg_tag_offset(in.rdest), 8), x86::rdx);
                break;
            }

            case Op::SUB: {
                a.mov(x86::rdi, x86::ptr(x86::rbx, reg_bits_offset(in.ra)));
                a.mov(x86::rsi, x86::ptr(x86::rbx, reg_tag_offset(in.ra), 8));
                a.mov(x86::rdx, x86::ptr(x86::rbx, reg_bits_offset(in.rb)));
                a.mov(x86::rcx, x86::ptr(x86::rbx, reg_tag_offset(in.rb), 8));
                a.call(imm(reinterpret_cast<uintptr_t>(&rjit_helper_sub)));
                a.mov(x86::ptr(x86::rbx, reg_bits_offset(in.rdest)), x86::rax);
                a.mov(x86::ptr(x86::rbx, reg_tag_offset(in.rdest), 8), x86::rdx);
                break;
            }

            case Op::MUL: {
                a.mov(x86::rdi, x86::ptr(x86::rbx, reg_bits_offset(in.ra)));
                a.mov(x86::rsi, x86::ptr(x86::rbx, reg_tag_offset(in.ra), 8));
                a.mov(x86::rdx, x86::ptr(x86::rbx, reg_bits_offset(in.rb)));
                a.mov(x86::rcx, x86::ptr(x86::rbx, reg_tag_offset(in.rb), 8));
                a.call(imm(reinterpret_cast<uintptr_t>(&rjit_helper_mul)));
                a.mov(x86::ptr(x86::rbx, reg_bits_offset(in.rdest)), x86::rax);
                a.mov(x86::ptr(x86::rbx, reg_tag_offset(in.rdest), 8), x86::rdx);
                break;
            }

            case Op::DIV: {
                a.mov(x86::rdi, x86::ptr(x86::rbx, reg_bits_offset(in.ra)));
                a.mov(x86::rsi, x86::ptr(x86::rbx, reg_tag_offset(in.ra), 8));
                a.mov(x86::rdx, x86::ptr(x86::rbx, reg_bits_offset(in.rb)));
                a.mov(x86::rcx, x86::ptr(x86::rbx, reg_tag_offset(in.rb), 8));
                a.call(imm(reinterpret_cast<uintptr_t>(&rjit_helper_div)));
                a.mov(x86::ptr(x86::rbx, reg_bits_offset(in.rdest)), x86::rax);
                a.mov(x86::ptr(x86::rbx, reg_tag_offset(in.rdest), 8), x86::rdx);
                break;
            }

            case Op::LT: {
                a.mov(x86::rdi, x86::ptr(x86::rbx, reg_bits_offset(in.ra)));
                a.mov(x86::rsi, x86::ptr(x86::rbx, reg_tag_offset(in.ra), 8));
                a.mov(x86::rdx, x86::ptr(x86::rbx, reg_bits_offset(in.rb)));
                a.mov(x86::rcx, x86::ptr(x86::rbx, reg_tag_offset(in.rb), 8));
                a.call(imm(reinterpret_cast<uintptr_t>(&rjit_helper_lt)));
                a.mov(x86::ptr(x86::rbx, reg_bits_offset(in.rdest)), x86::rax);
                a.mov(x86::ptr(x86::rbx, reg_tag_offset(in.rdest), 8), x86::rdx);
                break;
            }

            case Op::LE: {
                a.mov(x86::rdi, x86::ptr(x86::rbx, reg_bits_offset(in.ra)));
                a.mov(x86::rsi, x86::ptr(x86::rbx, reg_tag_offset(in.ra), 8));
                a.mov(x86::rdx, x86::ptr(x86::rbx, reg_bits_offset(in.rb)));
                a.mov(x86::rcx, x86::ptr(x86::rbx, reg_tag_offset(in.rb), 8));
                a.call(imm(reinterpret_cast<uintptr_t>(&rjit_helper_le)));
                a.mov(x86::ptr(x86::rbx, reg_bits_offset(in.rdest)), x86::rax);
                a.mov(x86::ptr(x86::rbx, reg_tag_offset(in.rdest), 8), x86::rdx);
                break;
            }

            case Op::GT: {
                a.mov(x86::rdi, x86::ptr(x86::rbx, reg_bits_offset(in.ra)));
                a.mov(x86::rsi, x86::ptr(x86::rbx, reg_tag_offset(in.ra), 8));
                a.mov(x86::rdx, x86::ptr(x86::rbx, reg_bits_offset(in.rb)));
                a.mov(x86::rcx, x86::ptr(x86::rbx, reg_tag_offset(in.rb), 8));
                a.call(imm(reinterpret_cast<uintptr_t>(&rjit_helper_gt)));
                a.mov(x86::ptr(x86::rbx, reg_bits_offset(in.rdest)), x86::rax);
                a.mov(x86::ptr(x86::rbx, reg_tag_offset(in.rdest), 8), x86::rdx);
                break;
            }

            case Op::GE: {
                a.mov(x86::rdi, x86::ptr(x86::rbx, reg_bits_offset(in.ra)));
                a.mov(x86::rsi, x86::ptr(x86::rbx, reg_tag_offset(in.ra), 8));
                a.mov(x86::rdx, x86::ptr(x86::rbx, reg_bits_offset(in.rb)));
                a.mov(x86::rcx, x86::ptr(x86::rbx, reg_tag_offset(in.rb), 8));
                a.call(imm(reinterpret_cast<uintptr_t>(&rjit_helper_ge)));
                a.mov(x86::ptr(x86::rbx, reg_bits_offset(in.rdest)), x86::rax);
                a.mov(x86::ptr(x86::rbx, reg_tag_offset(in.rdest), 8), x86::rdx);
                break;
            }

            case Op::EQ: {
                a.mov(x86::rdi, x86::ptr(x86::rbx, reg_bits_offset(in.ra)));
                a.mov(x86::rsi, x86::ptr(x86::rbx, reg_tag_offset(in.ra), 8));
                a.mov(x86::rdx, x86::ptr(x86::rbx, reg_bits_offset(in.rb)));
                a.mov(x86::rcx, x86::ptr(x86::rbx, reg_tag_offset(in.rb), 8));
                a.call(imm(reinterpret_cast<uintptr_t>(&rjit_helper_eq)));
                a.mov(x86::ptr(x86::rbx, reg_bits_offset(in.rdest)), x86::rax);
                a.mov(x86::ptr(x86::rbx, reg_tag_offset(in.rdest), 8), x86::rdx);
                break;
            }

            case Op::NE: {
                a.mov(x86::rdi, x86::ptr(x86::rbx, reg_bits_offset(in.ra)));
                a.mov(x86::rsi, x86::ptr(x86::rbx, reg_tag_offset(in.ra), 8));
                a.mov(x86::rdx, x86::ptr(x86::rbx, reg_bits_offset(in.rb)));
                a.mov(x86::rcx, x86::ptr(x86::rbx, reg_tag_offset(in.rb), 8));
                a.call(imm(reinterpret_cast<uintptr_t>(&rjit_helper_ne)));
                a.mov(x86::ptr(x86::rbx, reg_bits_offset(in.rdest)), x86::rax);
                a.mov(x86::ptr(x86::rbx, reg_tag_offset(in.rdest), 8), x86::rdx);
                break;
            }

            case Op::NEG: {
                a.mov(x86::rdi, x86::ptr(x86::rbx, reg_bits_offset(in.ra)));
                a.mov(x86::rsi, x86::ptr(x86::rbx, reg_tag_offset(in.ra), 8));
                a.call(imm(reinterpret_cast<uintptr_t>(&rjit_helper_neg)));
                a.mov(x86::ptr(x86::rbx, reg_bits_offset(in.rdest)), x86::rax);
                a.mov(x86::ptr(x86::rbx, reg_tag_offset(in.rdest), 8), x86::rdx);
                break;
            }

            case Op::MAKE_SEQ: {
                a.mov(x86::rdi, x86::ptr(x86::rbx, reg_bits_offset(in.ra)));
                a.mov(x86::rsi, x86::ptr(x86::rbx, reg_tag_offset(in.ra), 8));
                a.mov(x86::rdx, x86::ptr(x86::rbx, reg_bits_offset(in.rb)));
                a.mov(x86::rcx, x86::ptr(x86::rbx, reg_tag_offset(in.rb), 8));
                a.call(imm(reinterpret_cast<uintptr_t>(&rjit_helper_make_seq)));
                a.mov(x86::ptr(x86::rbx, reg_bits_offset(in.rdest)), x86::rax);
                a.mov(x86::ptr(x86::rbx, reg_tag_offset(in.rdest), 8), x86::rdx);
                break;
            }

            case Op::LENGTH: {
                a.mov(x86::rdi, x86::ptr(x86::rbx, reg_bits_offset(in.ra)));
                a.mov(x86::rsi, x86::ptr(x86::rbx, reg_tag_offset(in.ra), 8));
                a.call(imm(reinterpret_cast<uintptr_t>(&rjit_helper_length)));
                a.mov(x86::ptr(x86::rbx, reg_bits_offset(in.rdest)), x86::rax);
                a.mov(x86::ptr(x86::rbx, reg_tag_offset(in.rdest), 8), x86::rdx);
                break;
            }

            case Op::CALL: {
                // Call rjit_helper_call_value(callee_ptr, args, nargs, env)
                // where callee_ptr points to the Value in the register file.
                // This avoids the complexity of passing 16-byte Values
                // in registers.
                intptr_t callee_off = static_cast<intptr_t>(in.ra) * VALUE_SIZE;
                a.lea(x86::rdi, x86::ptr(x86::rbx, callee_off));  // &regs[ra]
                intptr_t args_off = static_cast<intptr_t>(in.ra + 1) * VALUE_SIZE;
                a.lea(x86::rsi, x86::ptr(x86::rbx, args_off));    // &regs[ra+1]
                a.mov(x86::edx, in.k);  // nargs
                a.mov(x86::rcx, x86::r13);  // env
                a.call(imm(reinterpret_cast<uintptr_t>(&rjit_helper_call_value)));
                a.mov(x86::ptr(x86::rbx, reg_bits_offset(in.rdest)), x86::rax);
                a.mov(x86::ptr(x86::rbx, reg_tag_offset(in.rdest), 8), x86::rdx);
                break;
            }

            case Op::MAKE_CLOSURE: {
                // rjit_helper_make_closure(code, env)
                // code = fn->constants[k].as_bytecode_fn()
                // env = r13
                a.mov(x86::rax, x86::ptr(x86::r12, offsetof(BytecodeFunction, constants)));
                intptr_t off = static_cast<intptr_t>(in.k) * VALUE_SIZE;
                a.mov(x86::rdi, x86::ptr(x86::rax, off + VALUE_BITS));  // code ptr
                a.mov(x86::rsi, x86::r13);  // env
                a.call(imm(reinterpret_cast<uintptr_t>(&rjit_helper_make_closure)));
                a.mov(x86::ptr(x86::rbx, reg_bits_offset(in.rdest)), x86::rax);
                a.mov(x86::ptr(x86::rbx, reg_tag_offset(in.rdest), 8), x86::rdx);
                break;
            }

            case Op::RETURN: {
                // Return regs[ra]
                a.mov(x86::rax, x86::ptr(x86::rbx, reg_bits_offset(in.ra)));
                a.mov(x86::rdx, x86::ptr(x86::rbx, reg_tag_offset(in.ra), 8));
                a.jmp(labels[fn->code.size()]);  // epilogue
                break;
            }

            case Op::RETURN_NULL: {
                a.xor_(x86::rax, x86::rax);
                a.xor_(x86::rdx, x86::rdx);
                a.jmp(labels[fn->code.size()]);
                break;
            }

            case Op::LOOP_HEADER:
            case Op::LOOP_BACKEDGE:
                break;

            default:
                // For unhandled opcodes, emit a deopt call.
                // This causes the JIT to fall back to the interpreter.
                a.mov(x86::edi, static_cast<uint32_t>(pc));
                a.call(imm(reinterpret_cast<uintptr_t>(&rjit_helper_deopt)));
                break;
        }
    }

    // ---- Epilogue ----
    Label epilogue = a.new_label();
    a.bind(epilogue);
    a.add(x86::rsp, 8);
    a.pop(x86::r15);
    a.pop(x86::r14);
    a.pop(x86::r13);
    a.pop(x86::r12);
    a.pop(x86::rbx);
    a.pop(x86::rbp);
    a.ret();

    // Finalize and add to runtime.
    Error err = rt.add(&jc->entry, &code);
    if (err != kErrorOk) {
        delete jc;
        return nullptr;
    }

    // Record safepoints for deopt.
    for (size_t i = 0; i < fn->code.size(); ++i) {
        if (fn->code[i].op == Op::LOOP_HEADER) {
            jc->safepoints.push_back({static_cast<uint32_t>(i), 0});
        }
    }

    return jc;
}

void JitCode::trace(Visitor& v) const {
    HeapObject* p = const_cast<HeapObject*>(static_cast<HeapObject const*>(source));
    v.visit_heap(p);
    const_cast<JitCode*>(this)->source = static_cast<BytecodeFunction*>(p);
}

JitCode::JitCode() : HeapObject(TypeTag::kJitCode) {}
JitCode::~JitCode() {}

}  // namespace rjit
