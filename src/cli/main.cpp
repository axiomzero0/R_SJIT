// rjit/cli/main.cpp
#include "rjit/cli/main.hpp"
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
#include "rjit/vm/interpreter.hpp"
#include "rjit/jit/baseline.hpp"
#include "rjit/jit/tier_manager.hpp"
#include "rjit/ir/sea_of_nodes.hpp"
#include "rjit/ir/graph_builder.hpp"
#include "rjit/optimizer/passes.hpp"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <chrono>

namespace rjit {

static std::string read_file(const char* path) {
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "rjit: cannot open '%s'\n", path);
        std::exit(1);
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static void usage() {
    std::fprintf(stderr,
        "Usage: rjit [options] <file.r>\n"
        "Options:\n"
        "  --dump-tokens      Print tokens and exit.\n"
        "  --dump-ast         Print AST and exit.\n"
        "  --dump-bytecode    Print bytecode and exit.\n"
        "  --dump-graph       Print Sea-of-Nodes IR and exit.\n"
        "  --no-jit           Disable JIT (interpreter only).\n"
        "  --tier <n>         Force JIT tier (0-5).\n"
        "  --stats            Print runtime statistics at end.\n"
        "  -h, --help         Show this help.\n");
}

int run_cli(int argc, char** argv) {
    CliOptions opts;
    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (a == "--dump-tokens")    opts.print_tokens  = true;
        else if (a == "--dump-ast")       opts.dump_ast      = true;
        else if (a == "--dump-bytecode")  opts.dump_bytecode = true;
        else if (a == "--dump-graph")     opts.dump_graph    = true;
        else if (a == "--no-jit")         opts.disable_jit   = true;
        else if (a == "--stats")          opts.show_stats    = true;
        else if (a == "--tier") {
            if (++i >= argc) { usage(); return 1; }
            opts.jit_tier = std::atoi(argv[i]);
        }
        else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "rjit: unknown option '%s'\n", a.c_str());
            return 1;
        }
        else pos.push_back(a);
    }
    if (pos.empty()) { usage(); return 1; }
    opts.input_file = pos[0];

    std::string source = read_file(opts.input_file.c_str());

    try {
        Context ctx;
        register_primitives(ctx);

        // Lex
        Lexer lex(source);
        auto toks = lex.lex_all();
        if (opts.print_tokens) {
            for (auto& t : toks) {
                std::printf("[%u:%u] %-20s %s\n", t.line, t.column,
                            tok_name(t.kind), t.text.c_str());
            }
            return 0;
        }

        // Parse
        Parser parser(std::move(toks));
        AstPtr ast = parser.parse_program();
        if (opts.dump_ast) {
            ast->print(0);
            return 0;
        }

        // Lower
        Lowerer lower(ctx);
        BytecodeFunction* fn = lower.lower_program(*ast);
        if (opts.dump_bytecode) {
            disassemble(fn);
            return 0;
        }

        if (opts.dump_graph) {
            GraphBuilder gb;
            Graph* graph = gb.build(fn);
            run_all_passes(*graph);
            graph->dump();
            return 0;
        }

        // Execute
        auto t0 = std::chrono::high_resolution_clock::now();
        Value result = ctx.interpreter().execute(fn, ctx.global_env());
        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        // Print result if it's not NULL and not a print side-effect.
        if (result.is_real())         std::printf("[result] %g\n", result.as_real());
        else if (result.is_integer()) std::printf("[result] %d\n", result.as_integer());
        else if (result.is_logical()) std::printf("[result] %s\n", result.as_logical() == 1 ? "TRUE" : (result.as_logical() == 0 ? "FALSE" : "NA"));
        else if (result.is_nil())     { /* NULL: silent */ }
        else if (result.is_vector()) {
            Vector* v = result.as_vector();
            if (v->vtype() == VectorType::kReal) {
                std::printf("[result]");
                for (size_t i = 0; i < v->length() && i < 10; ++i)
                    std::printf(" %g", v->real_at(i));
                if (v->length() > 10) std::printf(" ...");
                std::printf("\n");
            }
        }

        if (opts.show_stats) {
            std::fprintf(stderr, "--- stats ---\n");
            std::fprintf(stderr, "execution time: %.2f ms\n", ms);
            std::fprintf(stderr, "GC live objects: %zu\n", ctx.gc().live_objects());
            std::fprintf(stderr, "GC live bytes:   %zu\n", ctx.gc().live_bytes());
            std::fprintf(stderr, "GC peak bytes:   %zu\n", ctx.gc().peak_bytes());
            std::fprintf(stderr, "symbols interned: %zu\n", ctx.symbols().size());
        }
        std::fflush(stdout);
        std::fflush(stderr);
        // _exit skips static destructors, which crash because the GC's
        // heap tracking is not yet wired up for cleanup. This is a
        // known issue and will be fixed when conservative stack scanning
        // is added.
        _exit(0);
    } catch (RJitError const& e) {
        std::fprintf(stderr, "Error: %s\n", e.what());
        std::fflush(stdout);
        return 1;
    } catch (std::exception const& e) {
        std::fprintf(stderr, "Internal error: %s\n", e.what());
        std::fflush(stdout);
        return 2;
    }
    std::fprintf(stderr, "[rjit] normal exit\n");
    std::fflush(stdout);
    std::fflush(stderr);
    _exit(0);
}

}  // namespace rjit

int main(int argc, char** argv) {
    return rjit::run_cli(argc, argv);
}
