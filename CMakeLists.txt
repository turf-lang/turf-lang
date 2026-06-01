cmake_minimum_required(VERSION 3.20)
project(MiniTurf)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# ─── LLVM ────────────────────────────────────────────────────────────────────
find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

# ─── llama.cpp (external static build) ───────────────────────────────────────
set(LLAMA_BUILD_DIR "${CMAKE_SOURCE_DIR}/external/llama.cpp/build")

# Auto-build llama.cpp if the static libs aren't present yet
set(LIBLLAMA   "${LLAMA_BUILD_DIR}/src/libllama.a")
set(LIBGGML    "${LLAMA_BUILD_DIR}/ggml/src/libggml.a")

if(NOT EXISTS "${LIBLLAMA}" OR NOT EXISTS "${LIBGGML}")
  message(STATUS "llama.cpp static libs not found — building now (this may take a while)...")
  execute_process(
    COMMAND ${CMAKE_COMMAND}
      -S "${CMAKE_SOURCE_DIR}/external/llama.cpp"
      -B "${LLAMA_BUILD_DIR}"
      -DBUILD_SHARED_LIBS=OFF
      -DLLAMA_BUILD_TOOLS=OFF
      -DLLAMA_BUILD_TESTS=OFF
      -DLLAMA_BUILD_EXAMPLES=OFF
    RESULT_VARIABLE LLAMA_CONFIG_RESULT
  )
  if(NOT LLAMA_CONFIG_RESULT EQUAL 0)
    message(FATAL_ERROR "llama.cpp CMake configure failed")
  endif()

  include(ProcessorCount)
  ProcessorCount(NPROC)
  if(NPROC EQUAL 0)
    set(NPROC 2)
  endif()

  execute_process(
    COMMAND ${CMAKE_COMMAND} --build "${LLAMA_BUILD_DIR}" -- -j${NPROC}
    RESULT_VARIABLE LLAMA_BUILD_RESULT
  )
  if(NOT LLAMA_BUILD_RESULT EQUAL 0)
    message(FATAL_ERROR "llama.cpp build failed")
  endif()
endif()

# Collect llama.cpp static libs (core set + optional extras)
set(LLAMA_STATIC_LIBS
  "${LLAMA_BUILD_DIR}/src/libllama.a"
  "${LLAMA_BUILD_DIR}/ggml/src/libggml.a"
  "${LLAMA_BUILD_DIR}/ggml/src/libggml-base.a"
  "${LLAMA_BUILD_DIR}/ggml/src/libggml-cpu.a"
)

# Optional: ggml-blas (present in older builds)
set(LIBGGML_BLAS "${LLAMA_BUILD_DIR}/ggml/src/ggml-blas/libggml-blas.a")
if(EXISTS "${LIBGGML_BLAS}")
  list(APPEND LLAMA_STATIC_LIBS "${LIBGGML_BLAS}")
endif()

# Wrap each static lib in an IMPORTED target so CMake links them correctly
set(LLAMA_IMPORTED_TARGETS "")
set(_idx 0)
foreach(_lib ${LLAMA_STATIC_LIBS})
  set(_tgt "llama_static_${_idx}")
  add_library(${_tgt} STATIC IMPORTED)
  set_target_properties(${_tgt} PROPERTIES IMPORTED_LOCATION "${_lib}")
  list(APPEND LLAMA_IMPORTED_TARGETS ${_tgt})
  math(EXPR _idx "${_idx} + 1")
endforeach()

# ─── OpenMP (conditional, mirroring the build script logic) ──────────────────
set(OPENMP_LINK_FLAGS "")
if(APPLE)
  find_package(OpenMP QUIET)
  if(OpenMP_CXX_FOUND)
    set(OPENMP_LINK_FLAGS OpenMP::OpenMP_CXX)
  endif()
else()
  # Linux: only add OpenMP when libomp is discoverable (matches ldconfig check)
  find_package(OpenMP QUIET)
  if(OpenMP_CXX_FOUND)
    set(OPENMP_LINK_FLAGS OpenMP::OpenMP_CXX)
  endif()
endif()

# ─── Platform-specific frameworks ────────────────────────────────────────────
set(PLATFORM_LIBS "")
if(APPLE)
  find_library(ACCELERATE_FRAMEWORK Accelerate REQUIRED)
  list(APPEND PLATFORM_LIBS ${ACCELERATE_FRAMEWORK})
endif()

# ─── Executable ──────────────────────────────────────────────────────────────
add_executable(turf
  src/main.cpp
  src/Algorithms.cpp
  src/AliasAnalysis.cpp
  src/Analysis.cpp
  src/Builtins.cpp
  src/CFG.cpp
  src/CallGraph.cpp
  src/Codegen.cpp
  src/Lexer.cpp
  src/Lint.cpp
  src/Parser.cpp
  src/Sandbox.cpp
  src/SLMEngine.cpp
  src/SLMIntegration.cpp
  src/ResolverSymbolTable.cpp
  src/SymbolTable.cpp
  src/TypeChecker.cpp
  src/Types.cpp
  src/ValueRange.cpp
)

# ─── Standalone unit test: ResolverSymbolTable (no LLVM required) ─────────────
add_executable(test_resolver_symbol_table
  tests/test_resolver_symbol_table.cpp
  src/ResolverSymbolTable.cpp
  src/Types.cpp
)
target_include_directories(test_resolver_symbol_table PRIVATE include)
target_compile_options(test_resolver_symbol_table PRIVATE -Wall -Wextra -std=c++20)

# ─── Standalone unit test: Type Hierarchy — Issue #3 (no LLVM required) ───────
add_executable(test_type_hierarchy
  tests/test_type_hierarchy.cpp
  src/Types.cpp
)
target_include_directories(test_type_hierarchy PRIVATE include)
target_compile_options(test_type_hierarchy PRIVATE -Wall -Wextra -std=c++20)

# ─── Standalone unit test: TypeChecker — Issue #7/#135 ───────────────────────
# The test builds real AST nodes (NumberExprAST etc.) which requires AST.h
# (→ llvm/IR/Value.h), so we include LLVM headers and link LLVM.
# TYPECHECKER_NO_ERRORS suppresses Errors.h so we can install a custom
# throwing error handler without pulling in the full Codegen/LLVM IR chain.
# Lexer.cpp provides globals (SourceFilename, g_recoverJmp, etc.) referenced
# by AST.h.  ast_stubs.cpp provides ExprAST::NextNodeID / ResetNodeIDCounter
# (normally in Sandbox.cpp, which we cannot link here).
add_executable(test_type_checker
  tests/test_type_checker.cpp
  tests/ast_stubs.cpp
  tests/codegen_stubs.cpp
  src/TypeChecker.cpp
  src/ResolverSymbolTable.cpp
  src/Types.cpp
  src/Lexer.cpp
)
target_include_directories(test_type_checker PRIVATE
  include
  ${LLVM_INCLUDE_DIRS}
)
target_compile_definitions(test_type_checker PRIVATE
  TYPECHECKER_NO_ERRORS
  ${LLVM_DEFINITIONS}
)
target_compile_options(test_type_checker PRIVATE -Wall -Wextra -std=c++20)
target_link_libraries(test_type_checker PRIVATE LLVM)

target_include_directories(turf PRIVATE
  include
  external/llama.cpp/include
  external/llama.cpp/ggml/include
  ${LLVM_INCLUDE_DIRS}
)

target_compile_definitions(turf PRIVATE ${LLVM_DEFINITIONS})

target_compile_options(turf PRIVATE
  -Wall -Wextra -Wpedantic
  $<$<CONFIG:Debug>:-g -O0>
  $<$<CONFIG:Release>:-O3>
)

target_link_libraries(turf PRIVATE
  ${LLAMA_IMPORTED_TARGETS}
  LLVM
  ${OPENMP_LINK_FLAGS}
  ${PLATFORM_LIBS}
  pthread
  m
)
