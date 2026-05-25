#!/bin/bash
set -e

cd "$(dirname "$0")/.." || exit 1

BOLD="\033[1m"
GREEN="\033[0;32m"
BLUE="\033[0;34m"
RESET="\033[0m"

if ! command -v llvm-config &> /dev/null; then
    echo "Error: llvm-config not found. Please ensure LLVM is installed."
    exit 1
fi

echo -e "${GREEN}[1 / 2]${RESET} ${BOLD}Updating the Turf Compiler...${RESET}"
# echo -e "        ${BLUE}Sources:${RESET} src/main.cpp src/Lexer.cpp src/Parser.cpp src/Codegen.cpp src/Builtins.cpp src/Algorithms.cpp src/SymbolTable.cpp src/CFG.cpp src/Lint.cpp"

echo -e "        ${BLUE}Sources:${RESET} src/main.cpp src/Lexer.cpp src/Parser.cpp src/Codegen.cpp src/Builtins.cpp src/Algorithms.cpp src/SymbolTable.cpp src/CFG.cpp src/Lint.cpp src/SLMIntegration.cpp src/SLMEngine.cpp src/Sandbox.cpp src/Types.cpp"

# Build flags and libraries (allow optional files for newer llama builds)
LDFLAGS=(
    external/llama.cpp/build/src/libllama.a
    external/llama.cpp/build/ggml/src/libggml.a
    external/llama.cpp/build/ggml/src/libggml-base.a
    external/llama.cpp/build/ggml/src/libggml-cpu.a
)

# If llama/ggml static libs are not present, configure and build llama with CMake
if [ ! -f external/llama.cpp/build/src/libllama.a ] || [ ! -f external/llama.cpp/build/ggml/src/libggml.a ]; then
    echo -e "${BLUE}Building external/llama.cpp with CMake (may take a while)...${RESET}"
    pushd external/llama.cpp > /dev/null
    mkdir -p build
    pushd build > /dev/null

    # Configure with conservative flags: static libs, no extra tools/tests to speed up build
    cmake .. -DBUILD_SHARED_LIBS=OFF -DLLAMA_BUILD_TOOLS=OFF -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF

    # Build using available parallelism
    cmake --build . -- -j$(nproc || echo 2)

    popd > /dev/null
    popd > /dev/null
fi

# Include ggml-blas if present (older builds placed BLAS there)
if [ -f external/llama.cpp/build/ggml/src/ggml-blas/libggml-blas.a ]; then
    LDFLAGS+=(external/llama.cpp/build/ggml/src/ggml-blas/libggml-blas.a)
fi

# Assemble platform-specific framework flags (only macOS uses Accelerate)
FRAMEWORK_FLAGS=()
if [ "$(uname)" = "Darwin" ]; then
    FRAMEWORK_FLAGS+=("-framework" "Accelerate")
fi

# Disable OpenMP flags for compatibility in this environment.
OPENMP_FLAGS=()

clang++ src/main.cpp src/Lexer.cpp src/Parser.cpp src/Codegen.cpp src/Builtins.cpp src/Algorithms.cpp src/SymbolTable.cpp src/CFG.cpp src/Lint.cpp src/SLMIntegration.cpp src/SLMEngine.cpp src/Sandbox.cpp src/Types.cpp \
        -Iinclude \
        -Iexternal/llama.cpp/include \
        -Iexternal/llama.cpp/ggml/include \
        ${LDFLAGS[@]} \
        $(llvm-config --cxxflags --ldflags --system-libs --libs core native) \
        ${OPENMP_FLAGS[@]} \
        -lpthread -lm \
        ${FRAMEWORK_FLAGS[@]} \
        -o turf

echo -e "${GREEN}[2 / 2]${RESET} ${BOLD}Verifying build...${RESET}"

if [ -f "./turf" ]; then
    echo -e "${GREEN}Success!${RESET} The new compiler binary is now in effect."
else
    echo "Error: Build failed to produce an executable."
    exit 1
fi
