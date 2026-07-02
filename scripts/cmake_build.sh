#!/bin/bash
set -e
cd "$(dirname "$0")/.." || exit 1

BOLD="\033[1m"
GREEN="\033[0;32m"
BLUE="\033[0;34m"
YELLOW="\033[0;33m"
RESET="\033[0m"

BUILD_TYPE="Debug"
BUILD_DIR="build"

# Parse arguments
for arg in "$@"; do
  case $arg in
    --release)   BUILD_TYPE="Release" ;;
    --debug)     BUILD_TYPE="Debug" ;;
    --clean)     rm -rf "$BUILD_DIR" && echo -e "${YELLOW}Cleaned build directory.${RESET}" ;;
    --help)
      echo "Usage: $0 [--debug] [--release] [--clean]"
      exit 0
      ;;
  esac
done

echo -e "${GREEN}[1 / 3]${RESET} ${BOLD}Configuring (${BUILD_TYPE})...${RESET}"

cmake -S . -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

# Symlink compile_commands.json to project root for clangd/IDEs
if [ -f "$BUILD_DIR/compile_commands.json" ] && [ ! -L "compile_commands.json" ]; then
  ln -sf "$BUILD_DIR/compile_commands.json" compile_commands.json
fi

echo -e "${GREEN}[2 / 3]${RESET} ${BOLD}Building...${RESET}"

NPROC=$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 2)
cmake --build "$BUILD_DIR" -- -j"$NPROC"

echo -e "${GREEN}[3 / 3]${RESET} ${BOLD}Verifying build...${RESET}"

if [ -f "$BUILD_DIR/turf" ]; then
  echo -e "${GREEN}Success!${RESET} Binary is at ${BLUE}${BUILD_DIR}/turf${RESET}"
else
  echo "Error: Build failed to produce an executable."
  exit 1
fi
