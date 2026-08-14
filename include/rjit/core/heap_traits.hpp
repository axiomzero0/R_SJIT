// rjit/core/heap_traits.hpp - HeapObjectTraits specializations.
//
// Included by gc.hpp from inside namespace rjit. Forward-declares
// the heap object types and specializes HeapObjectTraits for each.

namespace rjit {

// Forward decls of all heap object types.
class Vector;
class Environment;
class Promise;
class Closure;
class Builtin;
class Symbol;
class PairList;
class Call;
class BytecodeFunction;
class JitCode;
class Attributes;

#define RJIT_TRAIT(T, TAG) \
    template <> struct HeapObjectTraits<T> { static constexpr TypeTag tag_value = TAG; }

RJIT_TRAIT(Vector,          TypeTag::kVector);
RJIT_TRAIT(Environment,     TypeTag::kEnvironment);
RJIT_TRAIT(Promise,         TypeTag::kPromise);
RJIT_TRAIT(Closure,         TypeTag::kClosure);
RJIT_TRAIT(Builtin,         TypeTag::kBuiltin);
RJIT_TRAIT(Symbol,          TypeTag::kSymbol);
RJIT_TRAIT(PairList,        TypeTag::kPairList);
RJIT_TRAIT(Call,            TypeTag::kCall);
RJIT_TRAIT(BytecodeFunction,TypeTag::kBytecodeFn);
RJIT_TRAIT(JitCode,         TypeTag::kJitCode);
RJIT_TRAIT(Attributes,      TypeTag::kExternalPtr);

#undef RJIT_TRAIT

}  // namespace rjit
