// rjit/bytecode/disassembler.cpp
#include "rjit/bytecode/module.hpp"
#include "rjit/core/context.hpp"
#include <cstdio>

namespace rjit {

const char* op_name(Op op) noexcept {
    switch (op) {
        case Op::NOP:               return "NOP";
        case Op::HALT:              return "HALT";
        case Op::JUMP:              return "JUMP";
        case Op::JUMP_IF_FALSE:     return "JUMP_IF_FALSE";
        case Op::JUMP_IF_TRUE:      return "JUMP_IF_TRUE";
        case Op::JUMP_IF_NA:        return "JUMP_IF_NA";
        case Op::LOAD_NIL:          return "LOAD_NIL";
        case Op::LOAD_TRUE:         return "LOAD_TRUE";
        case Op::LOAD_FALSE:        return "LOAD_FALSE";
        case Op::LOAD_NA:           return "LOAD_NA";
        case Op::LOAD_REAL:         return "LOAD_REAL";
        case Op::LOAD_INT:          return "LOAD_INT";
        case Op::LOAD_STRING:       return "LOAD_STRING";
        case Op::LOAD_VAR:          return "LOAD_VAR";
        case Op::STORE_VAR:         return "STORE_VAR";
        case Op::STORE_SUPER:       return "STORE_SUPER";
        case Op::LOAD_LOCAL:        return "LOAD_LOCAL";
        case Op::STORE_LOCAL:       return "STORE_LOCAL";
        case Op::RM_VAR:            return "RM_VAR";
        case Op::ADD:               return "ADD";
        case Op::SUB:               return "SUB";
        case Op::MUL:               return "MUL";
        case Op::DIV:               return "DIV";
        case Op::POW:               return "POW";
        case Op::MOD:               return "MOD";
        case Op::NEG:               return "NEG";
        case Op::ADD_REAL_REAL:     return "ADD_REAL_REAL";
        case Op::ADD_INT_INT:       return "ADD_INT_INT";
        case Op::ADD_REAL_INT:      return "ADD_REAL_INT";
        case Op::ADD_INT_REAL:      return "ADD_INT_REAL";
        case Op::ADD_SCALAR_VEC:    return "ADD_SCALAR_VEC";
        case Op::ADD_VEC_SCALAR:    return "ADD_VEC_SCALAR";
        case Op::ADD_VEC_VEC:       return "ADD_VEC_VEC";
        case Op::SUB_REAL_REAL:     return "SUB_REAL_REAL";
        case Op::SUB_INT_INT:       return "SUB_INT_INT";
        case Op::MUL_REAL_REAL:     return "MUL_REAL_REAL";
        case Op::MUL_INT_INT:       return "MUL_INT_INT";
        case Op::DIV_REAL_REAL:     return "DIV_REAL_REAL";
        case Op::DIV_INT_INT:       return "DIV_INT_INT";
        case Op::LT:                return "LT";
        case Op::LE:                return "LE";
        case Op::GT:                return "GT";
        case Op::GE:                return "GE";
        case Op::EQ:                return "EQ";
        case Op::NE:                return "NE";
        case Op::LT_REAL_REAL:      return "LT_REAL_REAL";
        case Op::LE_REAL_REAL:      return "LE_REAL_REAL";
        case Op::GT_REAL_REAL:      return "GT_REAL_REAL";
        case Op::GE_REAL_REAL:      return "GE_REAL_REAL";
        case Op::EQ_REAL_REAL:      return "EQ_REAL_REAL";
        case Op::NE_REAL_REAL:      return "NE_REAL_REAL";
        case Op::LT_INT_INT:        return "LT_INT_INT";
        case Op::LE_INT_INT:        return "LE_INT_INT";
        case Op::GT_INT_INT:        return "GT_INT_INT";
        case Op::GE_INT_INT:        return "GE_INT_INT";
        case Op::EQ_INT_INT:        return "EQ_INT_INT";
        case Op::NE_INT_INT:        return "NE_INT_INT";
        case Op::AND:               return "AND";
        case Op::OR:                return "OR";
        case Op::NOT:               return "NOT";
        case Op::AND_SCALAR:        return "AND_SCALAR";
        case Op::OR_SCALAR:         return "OR_SCALAR";
        case Op::CALL:              return "CALL";
        case Op::CALL_NAMED:        return "CALL_NAMED";
        case Op::CALL_BUILTIN:      return "CALL_BUILTIN";
        case Op::CALL_CLOSURE:      return "CALL_CLOSURE";
        case Op::RETURN:            return "RETURN";
        case Op::RETURN_NULL:       return "RETURN_NULL";
        case Op::MAKE_PROMISE:      return "MAKE_PROMISE";
        case Op::FORCE_PROMISE:     return "FORCE_PROMISE";
        case Op::MAKE_VECTOR:       return "MAKE_VECTOR";
        case Op::MAKE_SEQ:          return "MAKE_SEQ";
        case Op::LENGTH:            return "LENGTH";
        case Op::INDEX:             return "INDEX";
        case Op::INDEX2:            return "INDEX2";
        case Op::INDEX_NAMED:       return "INDEX_NAMED";
        case Op::INDEX_ASSIGN:      return "INDEX_ASSIGN";
        case Op::INDEX2_ASSIGN:     return "INDEX2_ASSIGN";
        case Op::INDEX_NAMED_ASSIGN:return "INDEX_NAMED_ASSIGN";
        case Op::SUBSET:            return "SUBSET";
        case Op::COERCE_REAL:       return "COERCE_REAL";
        case Op::COERCE_INT:        return "COERCE_INT";
        case Op::COERCE_LOGICAL:    return "COERCE_LOGICAL";
        case Op::TYPEOF:            return "TYPEOF";
        case Op::IS_NA:             return "IS_NA";
        case Op::NEW_ENV:           return "NEW_ENV";
        case Op::GET_ENV:           return "GET_ENV";
        case Op::MAKE_CLOSURE:      return "MAKE_CLOSURE";
        case Op::GUARD_TYPE:        return "GUARD_TYPE";
        case Op::GUARD_SHAPE:       return "GUARD_SHAPE";
        case Op::GUARD_LEN:         return "GUARD_LEN";
        case Op::DEOPT:             return "DEOPT";
        case Op::IC_LOAD_VAR:       return "IC_LOAD_VAR";
        case Op::IC_CALL:           return "IC_CALL";
        case Op::LOOP_HEADER:       return "LOOP_HEADER";
        case Op::LOOP_BACKEDGE:     return "LOOP_BACKEDGE";
        case Op::PRINT:             return "PRINT";
        case Op::DUMP:              return "DUMP";
        case Op::kCount:            return "?count?";
    }
    return "?";
}

void disassemble(BytecodeFunction const* fn) {
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
