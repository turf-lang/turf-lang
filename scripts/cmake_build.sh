#!/bin/bash
set -e
cd "$(dirname "$0")/.." || exit 1
BOLD="\033[1m"
GREEN="\033[0;32m"
BLUE="\033[0;34m"
YELLOW="\033[0;33m"
RED="\033[0;31m"
RESET="\033[0m"
BUILD_TYPE="Debug"
BUILD_DIR="build"
# --no-smart: build without --smart's model runtime (llama.cpp). No submodule,
# no ML build, no OpenMP -- the fast path for anyone who just wants the
# compiler. See TURF_SMART in CMakeLists.txt.
SMART="ON"
# --release-static: the self-contained release `turfc` (REL-008 stage 2): a
# Release build with LLVM and the C++ runtime linked statically and --smart's
# model runtime off. Needs an LLVM that ships static libraries; CI uses it.
# --smart (with --release-static): the same build WITH the model runtime, for
# the "smart" archive (needs the external/llama.cpp submodule).
RELEASE_STATIC="OFF"
WITH_SMART="OFF"
# --build-dir=DIR: build somewhere other than build/ (the plain and the smart
# release compilers are two builds of one checkout). Give it before --clean.
# --embed-lld (with --release-static): embed lld so a program naming no system C
# library is linked in-process against the bundled runtime (REL-008 stage 3).
EMBED_LLD="OFF"
# Parse arguments
for arg in "$@"; do
  case $arg in
    --release)   BUILD_TYPE="Release" ;;
    --debug)     BUILD_TYPE="Debug" ;;
    --no-smart)  SMART="OFF" ;;
    --release-static) BUILD_TYPE="Release"; SMART="OFF"; RELEASE_STATIC="ON" ;;
    --embed-lld) EMBED_LLD="ON" ;;
    --smart)     WITH_SMART="ON" ;;
    --build-dir=*) BUILD_DIR="${arg#--build-dir=}" ;;
    --clean)     rm -rf "$BUILD_DIR" && echo -e "${YELLOW}Cleaned build directory.${RESET}" ;;
    --help)
      echo "Usage: $0 [--debug] [--release] [--release-static [--embed-lld] [--smart]] [--no-smart] [--build-dir=DIR] [--clean]"
      exit 0
      ;;
  esac
done
# After the loop, so the order of --release-static and --smart does not matter.
[ "$WITH_SMART" = "ON" ] && SMART="ON"
echo -e "${GREEN}[1 / 3]${RESET} ${BOLD}Configuring (${BUILD_TYPE}, --smart runtime ${SMART})...${RESET}"
cmake -S . -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DTURF_SMART="$SMART" \
  -DTURF_RELEASE_STATIC="$RELEASE_STATIC" \
  -DTURF_EMBED_LLD="$EMBED_LLD" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
# Symlink compile_commands.json to project root for clangd/IDEs
if [ -f "$BUILD_DIR/compile_commands.json" ] && [ ! -L "compile_commands.json" ]; then
  ln -sf "$BUILD_DIR/compile_commands.json" compile_commands.json
fi
echo -e "${GREEN}[2 / 3]${RESET} ${BOLD}Building...${RESET}"
NPROC=$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 2)
cmake --build "$BUILD_DIR" --config "$BUILD_TYPE" -- -j"$NPROC"
echo -e "${GREEN}[3 / 3]${RESET} ${BOLD}Verifying build...${RESET}"
# On Windows/MinGW the output binary is turfc.exe, not turfc. This verifies the
# compiler, not build/turf (the Go front end), which is optional: it is only
# built when Go is installed.
# Also check multi-config generator layouts (build/<Config>/turfc[.exe]).
BIN=""
for candidate in \
  "$BUILD_DIR/turfc" \
  "$BUILD_DIR/turfc.exe" \
  "$BUILD_DIR/$BUILD_TYPE/turfc.exe" \
  "$BUILD_DIR/$BUILD_TYPE/turfc"
do
  if [ -f "$candidate" ]; then
    BIN="$candidate"
    break
  fi
done

if [ -n "$BIN" ]; then
  echo -e "${GREEN}Success!${RESET} Binary is at ${BLUE}${BIN}${RESET}"
else
  echo -e "${RED}Error: Build failed to produce an executable.${RESET}"
  exit 1
fi
