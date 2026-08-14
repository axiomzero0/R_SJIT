# R-SJIT: Speculative Multi-Tier R VM with JIT Compilation

A from-scratch implementation of a speculative, multi-tier R virtual
machine with a register-based bytecode interpreter, inline caches,
environment shapes, and a planned multi-tier JIT (baseline →
type-specialized → Sea-of-Nodes optimizing → tracing).

## Architecture

```
R source → Parser → AST → Lowerer → Bytecode IR → Interpreter (T0)
                                                  ↓ feedback
                                              Baseline JIT (T1)
                                                  ↓
                                          Type-Specialized JIT (T1.5)
                                                  ↓
                                          Sea-of-Nodes IR (T2)
                                                  ↓
                                              Machine Code
```

## Current Status

**Working:**
- Lexer (full R subset: numbers, strings, idents, operators, keywords)
- Parser (functions, calls, if/else, for/while/repeat, assignments,
  binary/unary ops, indexing, dollar)
- AST → register-based bytecode lowering
- T0 interpreter with:
  - Threaded dispatch
  - Inline caches for variable lookup (shape-based)
  - Type/shape/call/branch feedback recording
  - Quickening support (specialized opcodes)
  - Built-in primitives (print, cat, length, sum, c, seq, mean, etc.)
  - Closures with proper lexical scoping
  - For/while/repeat loops
  - If/else expressions
  - Recursive function calls

**In Progress:**
- T1 baseline JIT (asmjit scaffolding in place, codegen stubbed)
- Sea-of-Nodes IR (graph structure + builder skeleton)
- Optimizer passes (GVN, DCE, constant folding implemented; SCCP,
  LICM, PEA, escape/alias analysis stubbed)
- Deoptimization framework (blob structure in place, materialization
  needs work)
- OSR (scaffolding in place)

**Known Issues:**
- GC collection is disabled (no conservative stack scanning yet);
  objects leak but don't crash during execution
- Deep recursion after a prior recursive call has a state leak bug
  under investigation
- Static destructors crash at exit; `_exit(0)` is used as a workaround

## Building

```bash
mkdir build && cd build
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release ..
ninja rjit_cli
```

Requires: C++20 compiler, CMake 3.18+, Ninja, asmjit (included as
submodule under `third_party/asmjit/`).

## Usage

```bash
./rjit file.r                    # run
./rjit --dump-tokens file.r      # print tokens
./rjit --dump-ast file.r         # print AST
./rjit --dump-bytecode file.r    # print bytecode
./rjit --stats file.r            # print runtime stats
```

## Design Principles

1. **No hardcoding** — all sizes, thresholds, and limits are
   configurable or use `size_t`/`uint32_t` with no artificial caps.
2. **All in C++** — no external code generators, no embedded DSLs.
3. **Libraries for codegen and regalloc only** — asmjit for machine
   code emission; the IR, optimizer, and interpreter are hand-written.
4. **Speculative optimization first** — every assumption is guarded
   and deoptimizable.
5. **Runtime representation matters more than syntax** — the optimizer
   targets R's value/env/promise model, not surface syntax.

## Project Structure

```
rjit/
├── include/rjit/
│   ├── core/        # Value, GC, Environment, Promise, Vector, Context
│   ├── frontend/    # Lexer, Parser, AST, Lowerer
│   ├── bytecode/    # Opcodes, Module, Disassembler
│   ├── vm/          # Interpreter, Frame, InlineCache, Quickening, Primitives
│   ├── feedback/    # TypeFeedback, ShapeFeedback, CallFeedback, etc.
│   ├── ir/          # Sea-of-Nodes, Type lattice, Graph builder
│   ├── optimizer/   # GVN, SCCP, DCE, LICM, PEA, escape, alias, inlining
│   ├── jit/         # BaselineJIT, SpecializedJIT, OSR, Deopt, TierManager
│   └── codegen/     # x86_64 codegen, regalloc
├── src/             # .cpp implementations mirroring include/
├── examples/        # Test R programs
└── third_party/asmjit/
```

## References

- FastR: Optimizing R language execution via aggressive speculation
- ORBIT: Optimizing R VM via interpreter-level specialization
- Ř: Just-in-Time: Assumptions and Speculations
- Deegen: A JIT-Capable VM Generator for Dynamic Languages
- Copy-and-Patch JIT for R (D3S, 2025)
