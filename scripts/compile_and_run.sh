#!/bin/bash
set -e

BOLD="\033[1m"
GREEN="\033[0;32m"
RED="\033[0;31m"
RESET="\033[0m"

TOTAL_STEPS=5

if [ $# -eq 0 ]; then
    echo "Usage: $0 <input.kirk>"
    exit 1
fi

INPUT_FILE=$1

cleanup() {
    EXIT_CODE=$?
    
    echo -e "${GREEN}[5 / ${TOTAL_STEPS}]${RESET} ${BOLD}Cleaning up temporary files...${RESET}"
    rm -f output.o
    rm -f program

    if [ $EXIT_CODE -eq 0 ]; then
        echo -e "${GREEN}Build and run complete!${RESET}"
    else
        echo -e "${RED}Script exited with error code ${EXIT_CODE}.${RESET}"
    fi
}

trap cleanup EXIT INT TERM

COMPILER_PATH="$(dirname "$0")/../kirk"

echo -e "${GREEN}[1 / ${TOTAL_STEPS}]${RESET} ${BOLD}Compiling Kirk source:${RESET} $INPUT_FILE"
"$COMPILER_PATH" "$INPUT_FILE"

echo -e "${GREEN}[2 / ${TOTAL_STEPS}]${RESET} ${BOLD}Generating object file (PIC mode)...${RESET}"
llc -relocation-model=pic -filetype=obj output.ll -o output.o

echo -e "${GREEN}[3 / ${TOTAL_STEPS}]${RESET} ${BOLD}Linking executable...${RESET}"
clang output.o -o program -lm

echo -e "${GREEN}[4 / ${TOTAL_STEPS}]${RESET} ${BOLD}Running program output:${RESET}"
echo ""

./program

echo ""
