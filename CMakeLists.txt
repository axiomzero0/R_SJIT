cmake_minimum_required(VERSION 3.18)
project(rjit CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Architecture detection
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
    set(RJIT_ARCH_X86_64 ON)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
    set(RJIT_ARCH_AARCH64 ON)
else()
    message(FATAL_ERROR "Unsupported architecture: ${CMAKE_SYSTEM_PROCESSOR}")
endif()

# Build options
option(RJIT_ENABLE_JIT          "Enable JIT code generation"            ON)
option(RJIT_ENABLE_OPTIMIZER    "Enable Sea-of-Nodes optimizing JIT"    ON)
option(RJIT_ENABLE_TRACING      "Enable tracing JIT"                    ON)
option(RJIT_ENABLE_TESTS        "Build test binaries"                   ON)
option(RJIT_ENABLE_ASSERTIONS   "Enable debug assertions"               OFF)
option(RJIT_USE_LTO             "Enable link-time optimization"         OFF)

# Compiler flags
add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-missing-field-initializers
    -fno-omit-frame-pointer
    -fvisibility=hidden
    -fvisibility-inlines-hidden
)

if(RJIT_ENABLE_ASSERTIONS)
    add_compile_options(-DRJIT_ASSERTIONS=1 -O0 -g)
else()
    add_compile_options(-DRJIT_ASSERTIONS=0 -O3 -g)
endif()

if(RJIT_USE_LTO)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
endif()

# Required modern CPU features for the JIT target (so asmjit can emit SSE4.2+ / AVX)
if(RJIT_ARCH_X86_64)
    add_compile_options(-msse4.2 -mavx2 -mbmi2)
endif()

# asmjit: build as static library
set(ASMJIT_STATIC ON CACHE BOOL "" FORCE)
set(ASMJIT_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/asmjit)

# Main library target
add_library(rjit STATIC)

target_include_directories(rjit
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/asmjit/src
)

target_link_libraries(rjit
    PUBLIC
        asmjit::asmjit
)

target_sources(rjit PRIVATE
    # core
    src/core/value.cpp
    src/core/vector.cpp
    src/core/environment.cpp
    src/core/promise.cpp
    src/core/gc.cpp
    src/core/attributes.cpp
    src/core/closure.cpp
    src/core/context.cpp
    # frontend
    src/frontend/lexer.cpp
    src/frontend/parser.cpp
    src/frontend/ast_print.cpp
    src/frontend/lower.cpp
    # bytecode
    src/bytecode/module.cpp
    src/bytecode/disassembler.cpp
    # vm
    src/vm/interpreter.cpp
    src/vm/quickening.cpp
    src/vm/inline_cache.cpp
    src/vm/primitives.cpp
    # feedback
    src/feedback/feedback.cpp
    # ir
    src/ir/sea_of_nodes.cpp
    src/ir/types.cpp
    src/ir/graph_builder.cpp
    # optimizer
    src/optimizer/passes.cpp
    src/optimizer/pea.cpp
    src/optimizer/escape.cpp
    src/optimizer/alias.cpp
    src/optimizer/inlining.cpp
    src/optimizer/vectorize.cpp
    # jit
    src/jit/baseline.cpp
    src/jit/specialized.cpp
    src/jit/osr.cpp
    src/jit/deopt.cpp
    src/jit/tier_manager.cpp
    # codegen
    src/codegen/x86_64.cpp
    src/codegen/regalloc.cpp
)

# CLI driver
add_executable(rjit_cli src/cli/main.cpp)
target_link_libraries(rjit_cli PRIVATE rjit)
set_target_properties(rjit_cli PROPERTIES OUTPUT_NAME rjit)

if(RJIT_ENABLE_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Install
install(TARGETS rjit rjit_cli
    RUNTIME DESTINATION bin
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)
install(DIRECTORY include/rjit DESTINATION include)
