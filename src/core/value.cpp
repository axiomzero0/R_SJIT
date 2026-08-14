// rjit/core/value.cpp
#include "rjit/core/value.hpp"
#include "rjit/core/vector.hpp"
#include "rjit/core/environment.hpp"
#include "rjit/core/promise.hpp"

namespace rjit {

const char* tag_name(TypeTag t) noexcept {
    switch (t) {
        case TypeTag::kNil:         return "nil";
        case TypeTag::kLogical:     return "logical";
        case TypeTag::kInteger:     return "integer";
        case TypeTag::kReal:        return "real";
        case TypeTag::kString:      return "string";
        case TypeTag::kVector:      return "vector";
        case TypeTag::kEnvironment: return "environment";
        case TypeTag::kPromise:     return "promise";
        case TypeTag::kClosure:     return "closure";
        case TypeTag::kBuiltin:     return "builtin";
        case TypeTag::kSpecial:     return "special";
        case TypeTag::kCall:        return "call";
        case TypeTag::kSymbol:      return "symbol";
        case TypeTag::kPairList:    return "pairlist";
        case TypeTag::kBytecodeFn:  return "bytecode_fn";
        case TypeTag::kJitCode:     return "jit_code";
        case TypeTag::kExternalPtr: return "external_ptr";
        case TypeTag::kMissing:     return "missing";
    }
    return "<unknown>";
}

bool is_na(Value const& v) noexcept {
    switch (v.tag()) {
        case TypeTag::kLogical: return v.as_logical() == kNaLogical;
        case TypeTag::kInteger: return v.as_integer() == kNaInt;
        case TypeTag::kReal: {
            uint64_t bits; std::memcpy(&bits, &kNaReal, sizeof(bits));
            return v.as_real() != v.as_real() &&  // NaN check
                   std::memcmp(&bits, &v, sizeof(bits)) == 0;
        }
        default: return false;
    }
}

Value from_scalar_double(double d) noexcept { return Value::real(d); }
Value from_scalar_int(int32_t i) noexcept { return Value::integer(i); }
Value from_scalar_logical(int32_t l) noexcept { return Value::logical(l); }

}  // namespace rjit
