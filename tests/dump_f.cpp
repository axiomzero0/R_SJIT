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
using namespace rjit;
int main() {
    std::string source = "f <- function(n) { n + 1 }\nprint(f(5))\nprint(f(10))\n";
    Context ctx;
    register_primitives(ctx);
    Lexer lex(source);
    auto toks = lex.lex_all();
    Parser parser(std::move(toks));
    AstPtr ast = parser.parse_program();
    Lowerer lower(ctx);
    BytecodeFunction* fn = lower.lower_program(*ast);
    disassemble(fn);
    printf("\n--- f function ---\n");
    if (!fn->constants.empty() && fn->constants[0].is_bytecode_fn()) {
        disassemble(fn->constants[0].as_bytecode_fn());
    }
    return 0;
}
