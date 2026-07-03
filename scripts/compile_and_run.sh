#!/bin/bash
set -e
BOLD="\033[1m"
GREEN="\033[0;32m"
RED="\033[0;31m"
RESET="\033[0m"
TOTAL_STEPS=3
if [ $# -eq 0 ]; then
    echo "Usage: $0 <input.tr>"
    exit 1
fi
INPUT_FILE=$1
OUTPUT_NAME="program"

cleanup() {
    EXIT_CODE=$?
    echo -e "${GREEN}[3 / ${TOTAL_STEPS}]${RESET} ${BOLD}Cleaning up temporary files...${RESET}"
    rm -f "$OUTPUT_NAME" "${OUTPUT_NAME}.exe"
    if [ $EXIT_CODE -eq 0 ]; then
        echo -e "${GREEN}Build and run complete!${RESET}"
    else
        echo -e "${RED}Script exited with error code ${EXIT_CODE}.${RESET}"
    fi
}
trap cleanup EXIT INT TERM

SCRIPT_DIR="$(dirname "$0")"
# Resolve the turf compiler binary in either layout:
#   - packaged release:  <dir>/turf, <dir>/scripts/compile_and_run.sh  (sibling of scripts/)
#   - dev repo:          <repo>/build/turf[.exe], <repo>/scripts/compile_and_run.sh
COMPILER_PATH=""
for candidate in \
  "$SCRIPT_DIR/../turf" \
  "$SCRIPT_DIR/../turf.exe" \
  "$SCRIPT_DIR/../build/turf" \
  "$SCRIPT_DIR/../build/turf.exe"
do
  if [ -f "$candidate" ]; then
    COMPILER_PATH="$candidate"
    break
  fi
done

if [ -z "$COMPILER_PATH" ]; then
  echo -e "${RED}Error: could not find turf compiler binary (looked next to this script and in build/).${RESET}"
  exit 1
fi

echo -e "${GREEN}[1 / ${TOTAL_STEPS}]${RESET} ${BOLD}Compiling Turf source:${RESET} $INPUT_FILE"
"$COMPILER_PATH" "$INPUT_FILE" -o "$OUTPUT_NAME"

# Resolve the compiled program binary regardless of platform (program vs program.exe)
if [ -f "$OUTPUT_NAME" ]; then
  RUN_BIN="./$OUTPUT_NAME"
elif [ -f "${OUTPUT_NAME}.exe" ]; then
  RUN_BIN="./${OUTPUT_NAME}.exe"
else
  echo -e "${RED}Error: compilation did not produce an executable.${RESET}"
  exit 1
fi

echo -e "${GREEN}[2 / ${TOTAL_STEPS}]${RESET} ${BOLD}Running program output:${RESET}"
echo ""
"$RUN_BIN"
echo ""
