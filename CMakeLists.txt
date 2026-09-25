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

# ggml/CMakeLists.txt has its own `# remove the lib prefix on win32 mingw`
# step (`set(CMAKE_STATIC_LIBRARY_PREFIX "")`) that only takes effect
# inside ggml's own subdirectory scope -- so on Windows, ggml/ggml-base/
# ggml-cpu (all built from within that scope) come out as `ggml.a`/
# `ggml-base.a`/`ggml-cpu.a` (NO "lib" prefix), while `llama` (built from
# llama.cpp's own top-level `src/` subdirectory, a sibling scope ggml's
# local `set()` never reaches) keeps the ordinary `libllama.a` name on
# every platform, Windows included. Confirmed live: a release-workflow
# dry run's Windows build produced `ggml\src\ggml-base.a` (no prefix) but
# still failed at turf's own link step looking for the Linux/macOS-style
# `libggml.a` name specifically.
if(WIN32)
  set(GGML_LIB_PREFIX "")
else()
  set(GGML_LIB_PREFIX "lib")
endif()

# Auto-build llama.cpp if the static libs aren't present yet
set(LIBLLAMA   "${LLAMA_BUILD_DIR}/src/libllama.a")
set(LIBGGML    "${LLAMA_BUILD_DIR}/ggml/src/${GGML_LIB_PREFIX}ggml.a")

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
      # LLAMA_BUILD_COMMON defaults ON for a standalone configure (which
      # this always is, per -S above) and pulls in BOTH llama.cpp's
      # `common/` utils library AND vendor/cpp-httplib (see llama.cpp's
      # own CMakeLists.txt -- httplib is gated on LLAMA_BUILD_COMMON, not
      # LLAMA_BUILD_SERVER/EXAMPLES/TOOLS, all already OFF above). turf
      # only ever links libllama/libggml* (see LLAMA_STATIC_LIBS below) --
      # never anything from `common` -- so this has always been dead
      # weight, and on MinGW/Windows it's actively broken: cpp-httplib's
      # vendored mmap code calls `::CreateFile2`, which mingw-w64's headers
      # don't declare, so it fails to compile at all ("has not been
      # declared"). Confirmed live via a release-workflow dry run.
      -DLLAMA_BUILD_COMMON=OFF
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

  # Explicit --target list, not a plain default-target build. A plain
  # default build (`cmake --build <dir> -- -jN`, no --target) was
  # confirmed LIVE to leave `ggml/src/libggml.a` unbuilt on the Windows/
  # MinGW/Ninja release job specifically (turf's own link step then failed
  # with "libggml.a ... missing and no known rule to make it") -- Linux
  # and macOS builds with the identical LLAMA_BUILD_COMMON=OFF configure
  # were NOT observed to have this problem, so the exact platform-specific
  # mechanism (a generator/dependency-graph quirk somewhere in ggml's own
  # CMakeLists, not fully root-caused here) isn't the same as whatever
  # used to paper over it when LLAMA_BUILD_COMMON defaulted ON. Rather
  # than chase that further, requesting exactly the targets turf actually
  # needs (LLAMA_STATIC_LIBS below) sidesteps the question of what is or
  # isn't in the default set on any given platform/generator entirely.
  execute_process(
    COMMAND ${CMAKE_COMMAND} --build "${LLAMA_BUILD_DIR}"
      --target llama --target ggml --target ggml-base --target ggml-cpu
      -- -j${NPROC}
    RESULT_VARIABLE LLAMA_BUILD_RESULT
  )
  if(NOT LLAMA_BUILD_RESULT EQUAL 0)
    message(FATAL_ERROR "llama.cpp build failed")
  endif()
endif()

# Collect llama.cpp static libs (core set + optional extras)
set(LLAMA_STATIC_LIBS
  "${LLAMA_BUILD_DIR}/src/libllama.a"
  "${LLAMA_BUILD_DIR}/ggml/src/${GGML_LIB_PREFIX}ggml.a"
  "${LLAMA_BUILD_DIR}/ggml/src/${GGML_LIB_PREFIX}ggml-base.a"
  "${LLAMA_BUILD_DIR}/ggml/src/${GGML_LIB_PREFIX}ggml-cpu.a"
)

set(LIBGGML_METAL "${LLAMA_BUILD_DIR}/ggml/src/ggml-metal/${GGML_LIB_PREFIX}ggml-metal.a")
if(EXISTS "${LIBGGML_METAL}")
  list(APPEND LLAMA_STATIC_LIBS "${LIBGGML_METAL}")
endif()

# Optional: ggml-blas (present in older builds)
set(LIBGGML_BLAS "${LLAMA_BUILD_DIR}/ggml/src/ggml-blas/${GGML_LIB_PREFIX}ggml-blas.a")
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
  src/CodegenCore.cpp
  src/CodegenExpr.cpp
  src/CodegenVar.cpp
  src/CodegenControlFlow.cpp
  src/CodegenFunc.cpp
  src/CodegenArray.cpp
  src/CodegenStruct.cpp
  src/CompileBuffer.cpp
  src/Formatter.cpp
  src/Lexer.cpp
  src/Lint.cpp
  src/Monomorphize.cpp
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

# LLVM_DEFINITIONS (from LLVMConfig.cmake) is a single space-separated
# STRING of "-Dname" / "-Dname=value" flags, not a proper CMake
# semicolon-separated list -- confirmed via compile_commands.json: passing
# it straight to target_compile_definitions() (which expects a LIST of
# bare "name"/"name=value" entries, each getting its own "-D" prepended)
# instead merges the whole string into the FIRST definition's value, e.g.
# `-D_GLIBCXX_USE_CXX11_ABI="1 -D_GNU_SOURCE -D__STDC_CONSTANT_MACROS ..."`.
# This silently "worked" on every GCC/Clang tried on Linux/macOS (nothing
# in the actual translation units did a `#if _GLIBCXX_USE_CXX11_ABI` numeric
# check that choked on the garbage string value), but is fatal on MinGW's
# GCC 16 libstdc++, which DOES `#if _GLIBCXX_USE_CXX11_ABI` internally --
# confirmed live via a release-workflow dry run: "token '=' is not valid in
# preprocessor expressions", for every single one of turfcore's translation
# units. separate_arguments(NATIVE_COMMAND) splits the string the same way
# the target platform's own shell would (handling the embedded `="..."`
# quoting correctly), then each split flag's leading "-D" is stripped
# before handing the resulting clean list to target_compile_definitions(),
# which adds its own "-D" back per entry.
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND "${LLVM_DEFINITIONS}")
set(LLVM_DEFINITIONS_CLEAN "")
foreach(_llvm_def ${LLVM_DEFINITIONS_LIST})
  string(REGEX REPLACE "^-D" "" _llvm_def "${_llvm_def}")
  list(APPEND LLVM_DEFINITIONS_CLEAN "${_llvm_def}")
endforeach()
target_compile_definitions(turfcore PUBLIC ${LLVM_DEFINITIONS_CLEAN})

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

# ─── libturfrt (runtime library, RT-001) ─────────────────────────────────────
# A small, standalone C static library -- NOT linked into the `turf`
# compiler binary itself (turfcore/turf never call into it). It is linked
# into every COMPILED TURF PROGRAM instead, via `-lturfrt` at
# src/main.cpp's linkObject() -- see that function's own comment for why the
# library must land where TurfInstallRoot() (exported from
# src/ParserModule.cpp, the same anchor `import` uses for lib/std) can find
# it with zero setup in a plain dev build: TurfInstallRoot()'s fallback
# (no TURF_HOME set) resolves to `<binary-dir>/..`, i.e. this repo's own
# root for a `build/turf` dev build -- so the archive's own output directory
# is pointed at `<repo-root>/lib/` below, the exact sibling of `lib/std/`
# that anchor already resolves to. `*.a` is already git-ignored
# (.gitignore), so this build artifact living inside the otherwise
# source-controlled `lib/` tree is never accidentally committed. The
# release packaging action (.github/actions/package-release/action.yml)
# needs no separate change for this: it already does a blanket `cp -r lib`
# into the release archive, which picks up `lib/libturfrt.a` automatically
# once it exists there post-build -- verified live in the RT-001 change
# record.
#
# Static, not shared: every compiled Turf program is meant to be a
# standalone binary (matching how `-lm` is used today) -- a shared
# libturfrt.so would need every compiled program to carry an rpath or
# LD_LIBRARY_PATH setup on top of `import`'s own TURF_HOME-anchored search,
# for no benefit RT-001 needs.
add_library(turfrt STATIC
  runtime/turfrt_alloc.c
  runtime/turfrt_panic.c
  runtime/turfrt_types.c
)
target_include_directories(turfrt PRIVATE runtime)
set_target_properties(turfrt PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/lib"
)
target_compile_options(turfrt PRIVATE -Wall -Wextra)

# ─── Executable ──────────────────────────────────────────────────────────────
add_executable(turf src/main.cpp)

target_link_libraries(turf PRIVATE turfcore)
