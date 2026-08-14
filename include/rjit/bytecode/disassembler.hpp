// rjit/bytecode/disassembler.hpp
#pragma once
#include "rjit/bytecode/module.hpp"
#include <cstdio>

namespace rjit {

const char* op_name(Op op) noexcept;

inline void disassemble(BytecodeFunction const* fn) {
    std::printf("--- %s (nparams=%u, nregs=%u, constants=%zu) ---\n",
                fn->name.c_str(), fn->nparams, fn->nregs, fn->constants.size());
    for (size_t i = 0; i < fn->code.size(); ++i) {
        Instr const& in = fn->code[i];
        std::printf("[%4zu] %-22s r%u = r%u, r%u, k=%u", i, op_name(in.op),
                    in.rdest, in.ra, in.rb, in.k);
        if (in.flags) std::printf(" flags=0x%x", in.flags);
        std::printf("\n");
    }
}

}  // namespace rjit
