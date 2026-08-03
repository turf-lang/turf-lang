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
# NOTE: local/uncommitted fix, see https://github.com/turf-lang/turf-lang/issues/229 --
# CMAKE_SOURCE_DIR is always the top-level project's root, which breaks this
# path whenever turf-lang is consumed via add_subdirectory() (e.g. turf-lsp).
# CMAKE_CURRENT_SOURCE_DIR is this file's own directory in both cases.
set(LLAMA_BUILD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/llama.cpp/build")

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

set(LIBGGML_METAL "${LLAMA_BUILD_DIR}/ggml/src/ggml-metal/libggml-metal.a")
if(EXISTS "${LIBGGML_METAL}")
  list(APPEND LLAMA_STATIC_LIBS "${LIBGGML_METAL}")
endif()

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
  find_library(METAL_FRAMEWORK Metal REQUIRED)
  find_library(METALKIT_FRAMEWORK MetalKit REQUIRED)
  find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
  list(APPEND PLATFORM_LIBS ${METAL_FRAMEWORK} ${METALKIT_FRAMEWORK} ${FOUNDATION_FRAMEWORK})
endif()

# ─── Core library (everything but main.cpp) ─────────────────────────────────
# NOTE: local/uncommitted addition -- see
# https://github.com/turf-lang/turf-lang/issues/229. turf-lsp's CMakeLists.txt
# already assumes a `turfcore` target exists for it to link against; upstream
# only ever defined the monolithic `turf` executable. Added here, additively,
# so external tooling (turf-lsp) can build against the real compiler frontend
# instead of duplicating it. `turf` itself now links this instead of
# recompiling the same sources a second time.
add_library(turfcore STATIC
  src/Algorithms.cpp
  src/AliasAnalysis.cpp
  src/Analysis.cpp
  src/Builtins.cpp
  src/CFG.cpp
  src/CallGraph.cpp
  src/Codegen.cpp
  src/Lexer.cpp
  src/Lint.cpp
  src/ParserCore.cpp
  src/ParserLiteral.cpp
  src/ParserStmt.cpp
  src/ParserDecl.cpp
  src/ParserModule.cpp
  src/Sandbox.cpp
  src/SLMEngine.cpp
  src/SLMIntegration.cpp
  src/ResolverSymbolTable.cpp
  src/ResolverScopeTable.cpp
  src/ResolverModuleTable.cpp
  src/ResolverTypeRegistry.cpp
  src/ResolverCore.cpp
  src/ResolverExpr.cpp
  src/ResolverStmt.cpp
  src/ResolverDecl.cpp
  src/ResolverCall.cpp
  src/SymbolTable.cpp
  src/TypeCheckerCore.cpp
  src/TypeCheckerExpr.cpp
  src/TypeCheckerStmt.cpp
  src/TypeCheckerDecl.cpp
  src/TypeCheckerCall.cpp
  src/Types.cpp
  src/ValueRange.cpp
)

target_include_directories(turfcore PUBLIC
  include
  external/llama.cpp/include
  external/llama.cpp/ggml/include
  ${LLVM_INCLUDE_DIRS}
)

target_compile_definitions(turfcore PUBLIC ${LLVM_DEFINITIONS})

target_compile_options(turfcore PRIVATE
  -Wall -Wextra -Wpedantic
  $<$<CONFIG:Debug>:-g -O0>
  $<$<CONFIG:Release>:-O3>
)

target_link_libraries(turfcore PUBLIC
  ${LLAMA_IMPORTED_TARGETS}
  LLVM
  ${OPENMP_LINK_FLAGS}
  ${PLATFORM_LIBS}
  pthread
  m
)

# ─── Executable ──────────────────────────────────────────────────────────────
add_executable(turf src/main.cpp)

target_link_libraries(turf PRIVATE turfcore)
