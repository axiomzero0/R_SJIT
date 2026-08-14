// rjit/cli/main.hpp - CLI driver
#pragma once
#include <string>
#include <string_view>

namespace rjit {

struct CliOptions {
    std::string input_file;
    bool        dump_ast        = false;
    bool        dump_bytecode   = false;
    bool        dump_graph      = false;
    bool        disable_jit     = false;
    bool        show_stats      = false;
    bool        print_tokens    = false;
    int         jit_tier        = -1;  // -1 = auto
};

int run_cli(int argc, char** argv);

}  // namespace rjit
