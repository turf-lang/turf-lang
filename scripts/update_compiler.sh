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

echo -e "        ${BLUE}Sources:${RESET} src/main.cpp src/Lexer.cpp src/Parser.cpp src/Codegen.cpp src/Builtins.cpp src/Algorithms.cpp src/SymbolTable.cpp src/CFG.cpp src/Lint.cpp src/SLMIntegration.cpp src/SLMEngine.cpp"

clang++ src/main.cpp src/Lexer.cpp src/Parser.cpp src/Codegen.cpp src/Builtins.cpp src/Algorithms.cpp src/SymbolTable.cpp src/CFG.cpp src/Lint.cpp src/SLMIntegration.cpp src/SLMEngine.cpp \
    -Iinclude \
    -Iexternal/llama.cpp/include \
    -Iexternal/llama.cpp/ggml/include \
    external/llama.cpp/build/src/libllama.a \
    external/llama.cpp/build/ggml/src/libggml.a \
    external/llama.cpp/build/ggml/src/libggml-base.a \
    external/llama.cpp/build/ggml/src/libggml-cpu.a \
    `llvm-config --cxxflags --ldflags --system-libs --libs core native` \
    -lpthread -lm \
    -o turf

echo -e "${GREEN}[2 / 2]${RESET} ${BOLD}Verifying build...${RESET}"

if [ -f "./turf" ]; then
    echo -e "${GREEN}Success!${RESET} The new compiler binary is now in effect."
else
    echo "Error: Build failed to produce an executable."
    exit 1
fi
