cmake_minimum_required(VERSION 3.20)
project(MiniTurf)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# ─── Version ─────────────────────────────────────────────────────────────────
# The compiler's release version, printed by `turfc --version` (line 1). The
# `turf` front end has its own, independent version (FREEZE.md 6.16, F-PKG-1);
# what the two must agree on is the driver interface in src/driver/main.cpp, not
# this. CMAKE_CURRENT_SOURCE_DIR, not
# CMAKE_SOURCE_DIR: turf-lsp consumes this file through add_subdirectory(),
# where CMAKE_SOURCE_DIR is turf-lsp's own root and has no VERSION file.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" TURF_VERSION LIMIT_COUNT 1)
# Re-run configure when VERSION changes; otherwise an edited VERSION would be
# ignored until something unrelated forced a reconfigure.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")

# ─── LLVM ────────────────────────────────────────────────────────────────────
find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

# By default the monolithic libLLVM shared library is linked, which keeps dev
# builds small and fast but leaves every binary needing LLVM installed on the
# machine it runs on. TURF_STATIC_LLVM links LLVM's static component libraries
# instead, so a binary built that way (the language server the VS Code extension
# bundles, and the releases of REL-008 stage 2) carries what it needs. The
# component list is what the compiler's sources include (IR, Support, Analysis,
# Passes, Target, TargetParser, MC, Transforms) plus the native backend; the
# libraries' own dependencies are added by llvm_map_components_to_libnames().
# Needs a LLVM installation that ships the static libraries (Debian/Ubuntu's
# llvm-dev and Homebrew's llvm do; some distributions, Arch among them, ship only
# the shared library).
option(TURF_STATIC_LLVM "Link LLVM statically (component libraries) instead of the shared libLLVM" OFF)

# REL-008 stage 2: the self-contained `turfc` of a release archive. Stricter
# than TURF_STATIC_LLVM, which it implies:
#   * every LLVM target is linked, because main.cpp calls InitializeAll*() and
#     `--target=aarch64-...` must keep producing a real object file (REL-003,
#     scripts/smoke/cross_target_smoke.sh);
#   * LLVM's archives are named by path and its own system-library list is NOT
#     used. LLVMExports.cmake hands the linker whatever the machine that built
#     LLVM linked it with (on Ubuntu, `libz3.so`, a *shared* zlib and zstd), so
#     the "static" binary would still need those. The libraries a link of the
#     compiler needs are listed here, and zlib/zstd are taken as static archives;
#   * the C++ runtime is static (-static-libstdc++ -static-libgcc; MinGW is fully
#     static apart from the C runtime, which is the system's);
#   * --smart's model runtime is a choice, and the release ships both: OFF is
#     the plain archive (`turfc --smart` says it is not part of this build); ON
#     is the "smart" archive. With ON, llama.cpp is built for a release, not
#     for this machine (see LLAMA_RELEASE_ARGS): no OpenMP (ggml then uses its
#     own threads, so no libgomp/libomp is needed at run time) and no
#     -march=native. On macOS it links Metal/Accelerate/Foundation, which are
#     the operating system's own frameworks (scripts/release/release_deps_check.sh
#     allows them for a smart archive only). No model is built in or shipped:
#     --smart runs the one .gguf the user puts in <root>/models.
# What this does NOT do: remove the dependency on the C library and the system
# linker (`cc`) that links a compiled program (stages 3 and 4).
# Dev builds stay on the shared libLLVM (link time).
option(TURF_RELEASE_STATIC "Self-contained turfc for release archives (static LLVM and C++ runtime)" OFF)
if(TURF_RELEASE_STATIC)
  set(TURF_STATIC_LLVM ON)
endif()

# REL-008 stages 3-4: lld inside turfc, so a program that names no system C
# library beyond the C runtime is linked in-process (src/codegen/BundledLink.cpp) against
# the runtime the archive carries (lib/rt/<platform>/) instead of by `cc`. Only
# the linker driver for the host's object format is linked: ELF on Linux, MinGW
# (COFF) on Windows, Mach-O on macOS. It needs the static lld archives (Ubuntu's
# liblld-22 + liblld-22-dev, MSYS2's lld, Homebrew's lld@22), which is why it
# requires TURF_RELEASE_STATIC.
option(TURF_EMBED_LLD "Embed lld in turfc for in-process linking against the bundled C runtime (release builds)" OFF)
if(TURF_EMBED_LLD AND NOT TURF_RELEASE_STATIC)
  message(FATAL_ERROR "TURF_EMBED_LLD needs TURF_RELEASE_STATIC=ON (lld's static libraries are linked against the static LLVM components).")
endif()
set(TURF_LLD_COMPONENTS "")
if(TURF_EMBED_LLD)
  # What lld's drivers use beyond what the compiler already links (LTO, object
  # files, option parsing, symbolization for its diagnostics, ...).
  set(TURF_LLD_COMPONENTS lto dtlto object option binaryformat debuginfodwarf symbolize textapi
      bitreader bitwriter linker ipo)
  if(WIN32)
    # The COFF driver also writes PDBs and merges Windows manifests and import
    # libraries.
    list(APPEND TURF_LLD_COMPONENTS debuginfopdb debuginfocodeview debuginfomsf
         windowsdriver windowsmanifest libdriver)
  endif()
endif()

if(TURF_RELEASE_STATIC)
  llvm_map_components_to_libnames(TURF_LLVM_NAMES
    core support irreader analysis passes target targetparser mc transformutils instcombine scalaropts native
    AllTargetsAsmParsers AllTargetsCodeGens AllTargetsDescs AllTargetsInfos
    ${TURF_LLD_COMPONENTS})
  # Imported targets, so CMake adds each component's own LLVM dependencies
  # (CodeGen, SelectionDAG, ...) itself; only the *system* libraries LLVMSupport
  # asks for are replaced, below.
  set(TURF_LLVM_LINK ${TURF_LLVM_NAMES})
  foreach(_n ${TURF_LLVM_NAMES})
    if(NOT TARGET ${_n})
      message(FATAL_ERROR "TURF_RELEASE_STATIC: LLVM target ${_n} does not exist in this installation.")
    endif()
    get_target_property(_type ${_n} TYPE)
    if(NOT _type STREQUAL "STATIC_LIBRARY")
      message(FATAL_ERROR "TURF_RELEASE_STATIC: ${_n} is a ${_type}, not a static library. This LLVM installation does not ship static component libraries (Debian/Ubuntu llvm-dev, Homebrew llvm and MSYS2 llvm do; Arch's llvm does not).")
    endif()
  endforeach()

  # zlib and zstd (LLVM compresses debug sections with them) must be static
  # archives; a shared one is a runtime dependency the user would lack.
  find_library(TURF_STATIC_ZSTD NAMES libzstd.a REQUIRED
    HINTS "$ENV{HOMEBREW_PREFIX}/lib" /opt/homebrew/lib /usr/local/lib)
  set(TURF_LLVM_SYSLIBS "${TURF_STATIC_ZSTD}")
  if(APPLE)
    # The SDK has no static zlib, and /usr/lib/libz.dylib is part of macOS.
    list(APPEND TURF_LLVM_SYSLIBS z)
  else()
    find_library(TURF_STATIC_ZLIB NAMES libz.a REQUIRED)
    list(APPEND TURF_LLVM_SYSLIBS "${TURF_STATIC_ZLIB}")
  endif()
  if(WIN32)
    # Imported by LLVM's Support on Windows; every Windows ships them.
    list(APPEND TURF_LLVM_SYSLIBS psapi shell32 ole32 uuid advapi32 ws2_32 ntdll version)
  elseif(APPLE)
    list(APPEND TURF_LLVM_SYSLIBS curses)
  else()
    # Support's terminal-colour probing (terminfo); static, like the others.
    find_library(TURF_STATIC_TINFO NAMES libtinfo.a REQUIRED)
    list(APPEND TURF_LLVM_SYSLIBS "${TURF_STATIC_TINFO}" dl rt)
  endif()

  # LLVMExports.cmake gives LLVMSupport the system libraries the machine that
  # built LLVM linked it with: on Ubuntu `libz3.so` and a *shared* zlib and zstd,
  # which would make this "static" binary need them. Keep its LLVM entries and
  # swap the rest for the list above.
  get_target_property(_sup LLVMSupport INTERFACE_LINK_LIBRARIES)
  set(_sup_llvm "")
  foreach(_e ${_sup})
    if(_e MATCHES "^LLVM")
      list(APPEND _sup_llvm "${_e}")
    endif()
  endforeach()
  set_property(TARGET LLVMSupport PROPERTY INTERFACE_LINK_LIBRARIES ${_sup_llvm} ${TURF_LLVM_SYSLIBS})

  if(TURF_EMBED_LLD)
    # Ubuntu's lld is built against the shared libLLVM, and its exported targets
    # say so (`INTERFACE_LINK_LIBRARIES "LLVM"`), which would pull the shared
    # library back in. Its objects only need LLVM's symbols, which the static
    # components above provide, so keep just the lld-to-lld edges.
    find_package(LLD CONFIG REQUIRED)
    # LLVMLTO (which lld's ELF driver needs) links LLVMExtensions, which on
    # Debian/Ubuntu is exported as depending on Polly, a separate package this
    # compiler has no use for. Keep LLVMExtensions, drop the edge; if a Polly
    # symbol is really referenced, the link says so.
    if(TARGET LLVMExtensions)
      set_property(TARGET LLVMExtensions PROPERTY INTERFACE_LINK_LIBRARIES LLVMSupport)
    endif()
    # LLVMWindowsManifest (lld's COFF driver merges manifests with it) is
    # exported as linking libxml2, a DLL on MSYS2 that would end up beside
    # turfc.exe. turfc never merges a manifest; src/codegen/BundledLinkWindowsShims.cpp
    # provides the few symbols the object file references.
    if(TARGET LLVMWindowsManifest)
      set_property(TARGET LLVMWindowsManifest PROPERTY INTERFACE_LINK_LIBRARIES LLVMSupport)
    endif()
    # Dependents before what they depend on: a static archive only resolves
    # symbols from archives after it. Each driver is linked to lldCommon (and
    # the MinGW one to the COFF driver it hands over to) and nothing else.
    if(WIN32)
      set(TURF_LLD_LIBS lldMinGW lldCOFF lldCommon)
    elseif(APPLE)
      set(TURF_LLD_LIBS lldMachO lldCommon)
    else()
      set(TURF_LLD_LIBS lldELF lldCommon)
    endif()
    set_property(TARGET lldCommon PROPERTY INTERFACE_LINK_LIBRARIES "")
    foreach(_l ${TURF_LLD_LIBS})
      if(_l STREQUAL "lldMinGW")
        set_property(TARGET ${_l} PROPERTY INTERFACE_LINK_LIBRARIES lldCOFF)
      elseif(NOT _l STREQUAL "lldCommon")
        set_property(TARGET ${_l} PROPERTY INTERFACE_LINK_LIBRARIES lldCommon)
      endif()
    endforeach()
    message(STATUS "Embedded linker: ${TURF_LLD_LIBS}")
  endif()
  message(STATUS "Release link: static LLVM, LLVMSupport's system libs replaced by: ${TURF_LLVM_SYSLIBS}")

  if(WIN32)
    # --allow-multiple-definition: GNU ld on MinGW fails with "multiple
    # definition of ... std::__detail::_AnyMatcher<...>::operator()::__nul"
    # (and its guard variable): a static local of an inline std::regex function
    # that both libstdc++.a and libLLVMMipsCodeGen.a (an unrelated backend,
    # linked only because every target is) carry as .bss$ COMDATs, which PE
    # linking does not merge when their sizes differ (ld also warns "duplicate
    # section ... has different size" inside libstdc++.a itself, harmlessly).
    # Both are the same libstdc++ header's definitions, so taking the first is
    # correct. Found on the first Windows run of REL-008 stage 2; reproduced
    # with MSYS2's mingw-w64-x86_64-llvm 22.1.8-3 and a MinGW GCC 16.2.0
    # cross compiler. The cost: ld no longer reports a genuine duplicate symbol
    # in these links. Needed by every executable that links turfcore (turfc,
    # turf-api-tests), so it is on the library, not on turfc.
    set(TURF_WIN_LINK_OPTIONS -Wl,--allow-multiple-definition)
  endif()
elseif(TURF_STATIC_LLVM)
  llvm_map_components_to_libnames(TURF_LLVM_LINK
    core support irreader analysis passes target targetparser mc transformutils instcombine scalaropts native)
  message(STATUS "Linking LLVM statically: ${TURF_LLVM_LINK}")
else()
  set(TURF_LLVM_LINK LLVM)
endif()

# ─── --smart's local-model runtime: optional ─────────────────────────────────
# llama.cpp is only what `--smart` runs its model on, yet it used to be a hard
# build dependency: a source build that only wanted to print hello world had to
# clone the submodule and build an ML runtime first (and link OpenMP, and on
# macOS Metal/Accelerate). OFF compiles src/slm/SLMEngineDisabled.cpp instead of
# src/slm/SLMEngine.cpp -- the only file that includes llama.h -- and `--smart`
# then says it is not built in and shows the compiler's own diagnostics, which
# is what it already does when the model file is missing. Deterministic fixes
# and every other compiler feature are unaffected.
option(TURF_SMART "Build the local-model runtime behind --smart (vendored llama.cpp)" ON)
set(LLAMA_IMPORTED_TARGETS "")
set(OPENMP_LINK_FLAGS "")
set(PLATFORM_LIBS "")
if(TURF_SMART AND NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/llama.cpp/CMakeLists.txt")
  message(FATAL_ERROR
    "TURF_SMART is ON but the llama.cpp submodule is not checked out "
    "(external/llama.cpp is empty). Either fetch it:\n"
    "  git submodule update --init external/llama.cpp\n"
    "or build without --smart's model runtime:\n"
    "  cmake -DTURF_SMART=OFF ...")
endif()
if(TURF_SMART)

# ─── llama.cpp (external static build) ───────────────────────────────────────
# NOTE: local/uncommitted fix, see https://github.com/turf-lang/turf-lang/issues/229 --
# CMAKE_SOURCE_DIR is always the top-level project's root, which breaks this
# path whenever turf-lang is consumed via add_subdirectory() (e.g. turf-lsp).
# CMAKE_CURRENT_SOURCE_DIR is this file's own directory in both cases.
set(LLAMA_BUILD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/llama.cpp/build")
# A release build of llama.cpp is a different build, in a different directory,
# so a developer's native one (OpenMP, -march=native) is never linked into an
# archive by accident:
#   GGML_OPENMP=OFF  no libgomp/libomp to find on the user's machine;
#   GGML_NATIVE=OFF  not tuned to the CPU of the machine that built it. ggml's
#                    own non-native baseline is then used: on x86-64 that is
#                    AVX2/FMA/F16C/BMI2 (2013's Haswell and later), on arm64
#                    plain ARMv8 NEON.
set(LLAMA_RELEASE_ARGS "")
if(TURF_RELEASE_STATIC)
  set(LLAMA_BUILD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/llama.cpp/build-release")
  set(LLAMA_RELEASE_ARGS -DGGML_OPENMP=OFF -DGGML_NATIVE=OFF -DCMAKE_BUILD_TYPE=Release)
endif()

# ggml/CMakeLists.txt has its own `# remove the lib prefix on win32 mingw`
# step (`set(CMAKE_STATIC_LIBRARY_PREFIX "")`) that only takes effect
# inside ggml's own subdirectory scope -- so on Windows, ggml/ggml-base/
# ggml-cpu (all built from within that scope) come out as `ggml.a`/
# `ggml-base.a`/`ggml-cpu.a` (NO "lib" prefix), while `llama` (built from
# llama.cpp's own top-level `src/` subdirectory, a sibling scope ggml's
# local `set()` never reaches) keeps the ordinary `libllama.a` name on
# every platform, Windows included. Confirmed live: a release-workflow
# dry run's Windows build produced `ggml\src\ggml-base.a` (no prefix) but
# still failed at turfc's own link step looking for the Linux/macOS-style
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
      # LLAMA_BUILD_SERVER/EXAMPLES/TOOLS, all already OFF above). turfc
      # only ever links libllama/libggml* (see LLAMA_STATIC_LIBS below) --
      # never anything from `common` -- so this has always been dead
      # weight, and on MinGW/Windows it's actively broken: cpp-httplib's
      # vendored mmap code calls `::CreateFile2`, which mingw-w64's headers
      # don't declare, so it fails to compile at all ("has not been
      # declared"). Confirmed live via a release-workflow dry run.
      -DLLAMA_BUILD_COMMON=OFF
      ${LLAMA_RELEASE_ARGS}
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
  # MinGW/Ninja release job specifically (turfc's own link step then failed
  # with "libggml.a ... missing and no known rule to make it") -- Linux
  # and macOS builds with the identical LLAMA_BUILD_COMMON=OFF configure
  # were NOT observed to have this problem, so the exact platform-specific
  # mechanism (a generator/dependency-graph quirk somewhere in ggml's own
  # CMakeLists, not fully root-caused here) isn't the same as whatever
  # used to paper over it when LLAMA_BUILD_COMMON defaulted ON. Rather
  # than chase that further, requesting exactly the targets turfc actually
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
# ggml's threading; only linked when TURF_SMART builds the model runtime, and
# never in a release build, whose llama.cpp is built without it (above).
if(TURF_RELEASE_STATIC)
elseif(APPLE)
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
# Metal/Accelerate are ggml's backends, so they too only come with TURF_SMART.
if(APPLE)
  find_library(ACCELERATE_FRAMEWORK Accelerate REQUIRED)
  list(APPEND PLATFORM_LIBS ${ACCELERATE_FRAMEWORK})
  find_library(METAL_FRAMEWORK Metal REQUIRED)
  find_library(METALKIT_FRAMEWORK MetalKit REQUIRED)
  find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
  list(APPEND PLATFORM_LIBS ${METAL_FRAMEWORK} ${METALKIT_FRAMEWORK} ${FOUNDATION_FRAMEWORK})
endif()

endif() # TURF_SMART

# The one translation unit that differs: the real engine needs llama.h, the
# stub needs nothing (src/slm/SLMEngine.h only forward-declares llama types).
if(TURF_SMART)
  set(TURF_SLM_ENGINE_SOURCE src/slm/SLMEngine.cpp)
else()
  set(TURF_SLM_ENGINE_SOURCE src/slm/SLMEngineDisabled.cpp)
endif()

# ─── Core library (everything but main.cpp) ─────────────────────────────────
# NOTE: local/uncommitted addition -- see
# https://github.com/turf-lang/turf-lang/issues/229. turf-lsp's CMakeLists.txt
# already assumes a `turfcore` target exists for it to link against; upstream
# only ever defined the monolithic compiler executable (then named `turf`, now
# `turfc`). Added here, additively,
# so external tooling (turf-lsp) can build against the real compiler frontend
# instead of duplicating it. `turfc` itself now links this instead of
# recompiling the same sources a second time.
add_library(turfcore STATIC
  src/analysis/Algorithms.cpp
  src/analysis/AliasAnalysis.cpp
  src/analysis/Analysis.cpp
  src/tooling/AnalysisFacade.cpp
  src/sema/Builtins.cpp
  src/analysis/CFG.cpp
  src/codegen/CodegenCore.cpp
  src/codegen/CodegenExpr.cpp
  src/codegen/CodegenVar.cpp
  src/codegen/CodegenControlFlow.cpp
  src/codegen/CodegenPattern.cpp
  src/codegen/CodegenFunc.cpp
  src/codegen/CodegenArray.cpp
  src/codegen/CodegenStruct.cpp
  src/tooling/Formatter.cpp
  src/tooling/MigratePub.cpp
  src/frontend/Lexer.cpp
  src/analysis/Lint.cpp
  src/sema/Monomorphize.cpp
  src/frontend/ParserCore.cpp
  src/frontend/ParserLiteral.cpp
  src/frontend/ParserStmt.cpp
  src/frontend/ParserDecl.cpp
  src/frontend/ParserModule.cpp
  src/tooling/PackageGraph.cpp
  src/slm/Sandbox.cpp
  ${TURF_SLM_ENGINE_SOURCE}
  src/slm/SLMIntegration.cpp
  src/resolve/ResolverSymbolTable.cpp
  src/resolve/ResolverScopeTable.cpp
  src/resolve/ResolverModuleTable.cpp
  src/resolve/ResolverTypeRegistry.cpp
  src/resolve/ResolverCore.cpp
  src/resolve/ResolverExpr.cpp
  src/resolve/ResolverStmt.cpp
  src/resolve/ResolverDecl.cpp
  src/resolve/ResolverCall.cpp
  src/resolve/ResolverTypeIdentity.cpp
  src/resolve/ResolverVisibility.cpp
  src/resolve/SymbolTable.cpp
  src/sema/TypeCheckerCore.cpp
  src/sema/TypeCheckerExpr.cpp
  src/sema/TypeCheckerStmt.cpp
  src/sema/TypeCheckerDecl.cpp
  src/sema/TypeCheckerCall.cpp
  src/sema/TypeCheckerVisibility.cpp
  src/sema/TypeCheckerPattern.cpp
  src/sema/TypeCheckerSlice.cpp
  src/sema/Types.cpp
  src/sema/TypeWalk.cpp
  src/analysis/ValueRange.cpp
)

# `include` carries the public API (turf/Analysis.h); `src` lets every
# subsystem header be included as "frontend/Parser.h", "sema/Types.h", ...
target_include_directories(turfcore PUBLIC
  include
  src
  ${LLVM_INCLUDE_DIRS}
)
if(TURF_SMART)
  target_include_directories(turfcore PUBLIC
    external/llama.cpp/include
    external/llama.cpp/ggml/include
  )
endif()

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

if(TURF_WIN_LINK_OPTIONS)
  target_link_options(turfcore PUBLIC ${TURF_WIN_LINK_OPTIONS})
endif()

# REL-004 stage 3: the COMPILER under ASan + UBSan. TURF_RT_SANITIZE (below)
# sanitizes the runtime that compiled programs link; it says nothing about
# turfc itself, whose parser/resolver/checker are recursive C++ over
# pointer-linked trees (the classic home of a use-after-free that a normal
# build survives by luck). The flags are PUBLIC on turfcore so turfc,
# turf-api-tests and turf-lsp's own targets all link the sanitizer runtime.
# -fno-sanitize=vptr: LLVM is built without RTTI, so its headers cannot be
# vptr-checked. -fno-sanitize-recover makes a UBSan report fatal, so the test
# runner sees a crash instead of a line in a log nobody reads. A separate
# build directory (build_san/) is expected, not build/.
# REL-004: gcov coverage of the compiler, to see what the test suite never
# reaches (a gap finder, not a gate). turfc leaves through _Exit(), so
# main.cpp flushes the counters itself when TURF_COVERAGE is defined.
option(TURF_COVERAGE "Build the compiler with gcov instrumentation (--coverage)" OFF)
if(TURF_COVERAGE)
  target_compile_options(turfcore PUBLIC --coverage)
  target_link_options(turfcore PUBLIC --coverage)
  target_compile_definitions(turfcore PUBLIC TURF_COVERAGE)
endif()
option(TURF_SANITIZE_COMPILER "Build the compiler (turfcore, turfc, turf-api-tests) with -fsanitize=address,undefined" OFF)
if(TURF_SANITIZE_COMPILER)
  set(TURF_COMPILER_SAN_FLAGS -fsanitize=address,undefined -fno-sanitize=vptr
      -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
  target_compile_options(turfcore PUBLIC ${TURF_COMPILER_SAN_FLAGS})
  target_link_options(turfcore PUBLIC ${TURF_COMPILER_SAN_FLAGS})
endif()
target_link_libraries(turfcore PUBLIC
  ${LLAMA_IMPORTED_TARGETS}
  ${TURF_LLVM_LINK}
  ${OPENMP_LINK_FLAGS}
  ${PLATFORM_LIBS}
  pthread
  m
)

# ─── libturfrt (runtime library, RT-001) ─────────────────────────────────────
# A small, standalone C static library -- NOT linked into the `turfc`
# compiler binary itself (turfcore/turfc never call into it). It is linked
# into every COMPILED TURF PROGRAM instead, via `-lturfrt` at
# src/driver/main.cpp's linkObject() -- see that function's own comment for why the
# library must land where TurfInstallRoot() (exported from
# src/frontend/ParserModule.cpp, the same anchor `import` uses for lib/std) can find
# it with zero setup in a plain dev build: TurfInstallRoot()'s fallback
# (no TURF_HOME set) resolves to `<binary-dir>/..`, i.e. this repo's own
# root for a `build/turfc` dev build -- so the archive's own output directory
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
  runtime/turfrt_abi.c
  runtime/turfrt_alloc.c
  runtime/turfrt_convert.c
  runtime/turfrt_gc.c
  runtime/turfrt_net.c
  runtime/turfrt_panic.c
  runtime/turfrt_signal.c
  runtime/turfrt_sys.c
  runtime/turfrt_types.c
  runtime/turfrt_win32.c
)
target_include_directories(turfrt PRIVATE runtime)
set_target_properties(turfrt PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/lib"
)
target_compile_options(turfrt PRIVATE -Wall -Wextra)

# ─── libturfrt_asan (FDN-005 stage 3, docs/planning/ROADMAP_v2.0.0.md) ───────
# An independently-built, ASan+UBSan-instrumented twin of `turfrt` above,
# same sources, existing ONLY to answer MEM-002's own disclosed open
# question: "the register-scanning technique has not been cross-validated
# ... before this collector is exercised under load." A hand-written C
# harness compiled directly against `turfrt`'s own .a (scripts/smoke/gc_smoke.sh)
# already gets ASan coverage informally (MEM-002's own change record
# describes exactly this, ad hoc, during that group's development) -- this
# target makes it a real, always-available, CI-exercised build product
# instead, and (via TURF_RT_ASAN_BUILD below) lets `turfc` itself produce
# ASan-instrumented COMPILED TURF PROGRAMS, not just synthetic C repros, so
# the entire existing `tests/` regression suite becomes independent
# validation for free.
#
# A SEPARATE target, not a build-time flag on `turfrt` itself: both
# archives are named distinctly (`libturfrt.a` / `libturfrt_asan.a`) and
# land in the SAME lib/ directory (ARCHIVE_OUTPUT_DIRECTORY, unchanged from
# `turfrt` above) rather than a parallel directory tree, specifically so an
# ordinary dev build and an ASan-enabled one can coexist without either
# clobbering the other's runtime archive -- `main.cpp`'s `linkObject()`
# picks the archive name (and the `-fsanitize=` flags) by which macro this
# COMPILER BINARY was itself built with, not by anything in the environment
# at link time, so a `build/turfc` and a separately-configured ASan `turfc`
# binary each always reach for their own runtime, permanently.
option(TURF_RT_SANITIZE "Build libturfrt_asan and make this turfc binary link every compiled Turf program against it with -fsanitize=address,undefined" OFF)
if(TURF_RT_SANITIZE)
  add_library(turfrt_asan STATIC
    runtime/turfrt_abi.c
    runtime/turfrt_alloc.c
    runtime/turfrt_convert.c
    runtime/turfrt_gc.c
    runtime/turfrt_net.c
    runtime/turfrt_panic.c
    runtime/turfrt_signal.c
    runtime/turfrt_sys.c
    runtime/turfrt_types.c
    runtime/turfrt_win32.c
  )
  target_include_directories(turfrt_asan PRIVATE runtime)
  set_target_properties(turfrt_asan PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/lib"
    OUTPUT_NAME turfrt_asan
  )
  target_compile_options(turfrt_asan PRIVATE -Wall -Wextra -g -fno-omit-frame-pointer
    -fsanitize=address -fsanitize=undefined)
endif()

# ─── Executable ──────────────────────────────────────────────────────────────
# `turfc` is the compiler. The name `turf` belongs to the Go front end under
# cli/ (LEC-001, FREEZE.md 6.16 F-PKG-1); the two are built into the same
# directory, so they cannot share a name.
# The test runner (REL-002, `turfc --test`) belongs to the executable, not to
# turfcore: it starts processes and is of no use to a tool linking the
# front-end library.
add_executable(turfc src/driver/main.cpp src/tooling/TestRunner.cpp
  src/tooling/TestExpectations.cpp src/codegen/BundledLink.cpp)

if(TURF_EMBED_LLD)
  target_compile_definitions(turfc PRIVATE TURF_EMBED_LLD)
  if(WIN32)
    # MSYS2's lld archives call libxml2 and llvm::demangle through import-table
    # slots a static link does not have; see the file's own comment.
    target_sources(turfc PRIVATE src/codegen/BundledLinkWindowsShims.cpp)
  endif()
  # Before turfcore: a static archive only resolves symbols from archives that
  # come after it, and lld needs LLVM's.
  target_link_libraries(turfc PRIVATE ${TURF_LLD_LIBS} turfcore)
else()
  target_link_libraries(turfc PRIVATE turfcore)
endif()
if(TURF_EMBED_LLD)
  # LLD_INCLUDE_DIRS comes from LLDConfig.cmake; fall back to LLVM's own include
  # directory, where Debian puts lld/Common/Driver.h.
  if(LLD_INCLUDE_DIRS)
    target_include_directories(turfc PRIVATE ${LLD_INCLUDE_DIRS})
  endif()
endif()
if(TURF_RELEASE_STATIC AND NOT APPLE)
  # The C++ runtime and the compiler's support library go into turfc (macOS
  # links the system's libc++, which every Mac has). MinGW is linked fully
  # static: libwinpthread is the other DLL it would bring along; the C runtime
  # stays the system's either way.
  if(WIN32)
    # -s: 216 MB unstripped, 134 MB stripped. (The duplicate-definition flag
    # every Windows release link needs is on turfcore, above.)
    # --stack: GNU ld gives an executable 2 MB of main-thread stack, a Linux
    # process 8 MB. The compiler is recursive (the parser, resolver and type
    # checker walk the tree), and tests/01_syntax/fail_deeply_nested_expression.tr
    # crashed turfc.exe with an access violation, instead of reaching the nesting
    # limit that rejects it cleanly, when run on Windows. 8 MB matches Linux.
    target_link_options(turfc PRIVATE -static -s -Wl,--stack,8388608)
  else()
    target_link_options(turfc PRIVATE -static-libstdc++ -static-libgcc)
  endif()
endif()
target_compile_definitions(turfc PRIVATE TURF_VERSION="${TURF_VERSION}")
if(TURF_RT_SANITIZE)
  target_compile_definitions(turfc PRIVATE TURF_RT_ASAN_BUILD)
  add_dependencies(turfc turfrt_asan)
endif()

# ─── The `turf` front end (LEC-001, FREEZE.md 6.16 F-PKG-1) ──────────────────
# The Go program under cli/, built into build/turf beside build/turfc (which is
# how it finds the compiler with no setup). It is versioned independently of
# TURF_VERSION -- its own version is a constant in cli/internal/version -- and
# checks the compiler's driver interface, not its release version, so nothing
# is stamped in here. Optional on purpose: the compiler and the whole test
# suite must build with no Go toolchain, so a missing `go` only skips this.
# Top-level builds only, so turf-lsp's add_subdirectory() never needs Go.
#
# The target is named `turf-cli`, NOT `turf`. Under Ninja a phony target named
# like the file it writes (build/turf) is considered up to date whenever that
# file exists, and silently never rebuilds. `ALL` is fine: Go's build cache
# makes a no-op rebuild take milliseconds.
#
# cli/ is a mirror of the turf-cli repository (cli/README.md, "Two copies"). To
# retire it, delete cli/: the build then takes the front end from TURF_CLI_DIR
# (the cache variable or the environment variable, which the smoke scripts read
# too), by default a turf-cli checkout next to this one, and skips it if there
# is none.
option(TURF_BUILD_CLI "Build the Go front end (cli/) as build/turf" ON)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cli/go.mod")
  set(_turf_cli_default "${CMAKE_CURRENT_SOURCE_DIR}/cli")
elseif(DEFINED ENV{TURF_CLI_DIR})
  file(TO_CMAKE_PATH "$ENV{TURF_CLI_DIR}" _turf_cli_default)
else()
  set(_turf_cli_default "${CMAKE_CURRENT_SOURCE_DIR}/../turf-cli")
endif()
set(TURF_CLI_DIR "${_turf_cli_default}" CACHE PATH "The turf front end's source (cli/ or a turf-cli checkout)")
if(TURF_BUILD_CLI AND PROJECT_IS_TOP_LEVEL)
  find_program(TURF_GO go)
  if(NOT EXISTS "${TURF_CLI_DIR}/go.mod")
    message(STATUS "No turf front end source at ${TURF_CLI_DIR}: build/turf (the CLI) will not be built; build/turfc is unaffected")
  elseif(TURF_GO)
    add_custom_target(turf-cli ALL
      COMMAND ${CMAKE_COMMAND} -E env CGO_ENABLED=0
        ${TURF_GO} build -trimpath
        -o "${CMAKE_BINARY_DIR}/turf${CMAKE_EXECUTABLE_SUFFIX}" ./cmd/turf
      WORKING_DIRECTORY "${TURF_CLI_DIR}"
      COMMENT "Building the turf front end (Go)"
      VERBATIM)
  else()
    message(STATUS "Go not found: build/turf (the CLI) will not be built; build/turfc is unaffected")
  endif()
endif()

# ─── Tool API tests (SMP-001) ────────────────────────────────────────────────
# Unit tests for the compiler query facade (include/turf/Analysis.h), run by
# CI next to test_runner.sh. Placed beside `turfc` so the facade finds lib/std
# the way the compiler does. Top-level builds only: turf-lsp pulls this file
# in with add_subdirectory() for `turfcore` and has no use for the tests.
if(PROJECT_IS_TOP_LEVEL)
  add_executable(turf-api-tests tests/api/analysis_test.cpp)
  target_link_libraries(turf-api-tests PRIVATE turfcore)
endif()
