// Quick test: dump the fib function's bytecode
#include "rjit/core/context.hpp"
#include "rjit/core/error.hpp"
#include "rjit/core/vector.hpp"
#include "rjit/frontend/lexer.hpp"
#include "rjit/frontend/parser.hpp"
#include "rjit/frontend/ast.hpp"
#include "rjit/frontend/lower.hpp"
#include "rjit/bytecode/module.hpp"
#include "rjit/bytecode/disassembler.hpp"
#include "rjit/vm/primitives.hpp"
#include <cstdio>
#include <fstream>
#include <sstream>

using namespace rjit;

int main() {
    std::string source = R"(
fib <- function(n) {
    if (n < 2) { n } else { fib(n - 1) + fib(n - 2) }
}
print(fib(10))
)";
    Context ctx;
    register_primitives(ctx);
    Lexer lex(source);
    auto toks = lex.lex_all();
    Parser parser(std::move(toks));
    AstPtr ast = parser.parse_program();
    Lowerer lower(ctx);
    BytecodeFunction* fn = lower.lower_program(*ast);

    // The fib closure is in constant 0
    disassemble(fn);
    printf("\n--- fib function ---\n");
    if (!fn->constants.empty() && fn->constants[0].is_bytecode_fn()) {
        BytecodeFunction* fib_fn = fn->constants[0].as_bytecode_fn();
        disassemble(fib_fn);
    }
    return 0;
}
