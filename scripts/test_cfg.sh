#!/bin/bash
# Test script for CFG (Control Flow Graph) analysis

set -e

cd "$(dirname "$0")/.."

GREEN="\033[0;32m"
RED="\033[0;31m"
BLUE="\033[0;34m"
RESET="\033[0m"

echo -e "${BLUE}=== CFG Flow Analysis Tests ===${RESET}\n"

# Test 1: Unreachable code after return
echo -e "${BLUE}Test 1: Unreachable code after return${RESET}"
if ./turf tests/cfg_test_unreachable.tr 2>&1 | grep -qi "unreachable\|impossible to reach\|will never happen"; then
  echo -e "${GREEN}✓ PASS${RESET} - Detected unreachable code after return\n"
else
  echo -e "${RED}✗ FAIL${RESET} - Should detect unreachable code after return\n"
fi

# Test 2: Missing return paths
echo -e "${BLUE}Test 2: Missing return paths${RESET}"
if ./turf tests/cfg_test_missing_return.tr 2>&1 | grep -qi "not all code paths return\|may not return\|promises to give back a result"; then
  echo -e "${GREEN}✓ PASS${RESET} - Detected missing return paths\n"
else
  echo -e "${RED}✗ FAIL${RESET} - Should detect missing return paths\n"
fi

# Test 3: Unreachable code after break
echo -e "${BLUE}Test 3: Unreachable code after break${RESET}"
if ./turf tests/cfg_test_break_unreachable.tr 2>&1 | grep -qi "impossible to reach\|after a return/break/continue"; then
  echo -e "${GREEN}✓ PASS${RESET} - Detected unreachable code after break\n"
else
  echo -e "${RED}✗ FAIL${RESET} - Should detect unreachable code after break\n"
fi

# Test 4: Unreachable code after continue
echo -e "${BLUE}Test 4: Unreachable code after continue${RESET}"
if ./turf tests/cfg_test_continue_unreachable.tr 2>&1 | grep -qi "impossible to reach\|after a return/break/continue"; then
  echo -e "${GREEN}✓ PASS${RESET} - Detected unreachable code after continue\n"
else
  echo -e "${RED}✗ FAIL${RESET} - Should detect unreachable code after continue\n"
fi

# Test 5: Valid control flow (should compile)
echo -e "${BLUE}Test 5: Valid control flow${RESET}"
if ./turf tests/cfg_test_valid.tr 2>&1 | grep -q "Successfully compiled"; then
  echo -e "${GREEN}✓ PASS${RESET} - Valid control flow compiled successfully\n"
else
  echo -e "${RED}✗ FAIL${RESET} - Valid control flow should compile\n"
fi

# Test 6: Complex control flow (should compile)
echo -e "${BLUE}Test 6: Complex control flow${RESET}"
if ./turf tests/cfg_test_complex.tr 2>&1 | grep -q "Successfully compiled"; then
  echo -e "${GREEN}✓ PASS${RESET} - Complex control flow compiled successfully\n"
else
  echo -e "${RED}✗ FAIL${RESET} - Complex control flow should compile\n"
fi

# Test 7: Early return (should compile)
echo -e "${BLUE}Test 7: Early return in branch${RESET}"
if ./turf tests/cfg_test_early_return.tr 2>&1 | grep -q "Successfully compiled"; then
  echo -e "${GREEN}✓ PASS${RESET} - Early return compiled successfully\n"
else
  echo -e "${RED}✗ FAIL${RESET} - Early return should compile\n"
fi

# Test 8: Multiple returns (should detect unreachable)
echo -e "${BLUE}Test 8: Multiple returns in sequence${RESET}"
if ./turf tests/cfg_test_multiple_returns.tr 2>&1 | grep -qi "impossible to reach\|after a return/break/continue"; then
  echo -e "${GREEN}✓ PASS${RESET} - Detected unreachable returns\n"
else
  echo -e "${RED}✗ FAIL${RESET} - Should detect unreachable code\n"
fi

# Test 9: Deeply nested if-else (should compile)
echo -e "${BLUE}Test 9: Deeply nested if-else${RESET}"
if ./turf tests/cfg_test_nested_if.tr 2>&1 | grep -q "Successfully compiled"; then
  echo -e "${GREEN}✓ PASS${RESET} - Nested if-else compiled successfully\n"
else
  echo -e "${RED}✗ FAIL${RESET} - Nested if-else should compile\n"
fi

# Test 10: Loop with multiple exits (should compile)
echo -e "${BLUE}Test 10: Loop with multiple exit points${RESET}"
if ./turf tests/cfg_test_loop_exits.tr 2>&1 | grep -q "Successfully compiled"; then
  echo -e "${GREEN}✓ PASS${RESET} - Loop exits compiled successfully\n"
else
  echo -e "${RED}✗ FAIL${RESET} - Loop exits should compile\n"
fi

# Test 11: Nested continue (should compile)
echo -e "${BLUE}Test 11: Continue in nested structure${RESET}"
if ./turf tests/cfg_test_nested_continue.tr 2>&1 | grep -q "Successfully compiled"; then
  echo -e "${GREEN}✓ PASS${RESET} - Nested continue compiled successfully\n"
else
  echo -e "${RED}✗ FAIL${RESET} - Nested continue should compile\n"
fi

# Test 12: If without else (should compile)
echo -e "${BLUE}Test 12: If without else branch${RESET}"
if ./turf tests/cfg_test_if_no_else.tr 2>&1 | grep -q "Successfully compiled"; then
  echo -e "${GREEN}✓ PASS${RESET} - If without else compiled successfully\n"
else
  echo -e "${RED}✗ FAIL${RESET} - If without else should compile\n"
fi

# Test 13: Return in nested block (should error)
echo -e "${BLUE}Test 13: Return in nested block${RESET}"
if ./turf tests/cfg_test_nested_block_return.tr 2>&1 | grep -qi "unreachable\|impossible to reach\|will never happen"; then
  echo -e "${GREEN}✓ PASS${RESET} - Detected unreachable code in nested block\n"
else
  echo -e "${RED}✗ FAIL${RESET} - Should detect unreachable code in nested block\n"
fi

# Test 14: Guaranteed break (should compile)
echo -e "${BLUE}Test 14: While with guaranteed break${RESET}"
if ./turf tests/cfg_test_guaranteed_break.tr 2>&1 | grep -q "Successfully compiled"; then
  echo -e "${GREEN}✓ PASS${RESET} - Guaranteed break compiled successfully\n"
else
  echo -e "${RED}✗ FAIL${RESET} - Guaranteed break should compile\n"
fi

# Test 15: Partial return (should warn)
echo -e "${BLUE}Test 15: Partial return coverage${RESET}"
if ./turf tests/cfg_test_partial_return.tr 2>&1 | grep -qi "not all code paths return\|may not return\|promises to give back a result"; then
  echo -e "${GREEN}✓ PASS${RESET} - Detected missing return on some paths\n"
else
  echo -e "${RED}✗ FAIL${RESET} - Should detect missing return paths\n"
fi

# Test 16: Break and continue (should compile)
echo -e "${BLUE}Test 16: Break and continue in same loop${RESET}"
if ./turf tests/cfg_test_break_continue.tr 2>&1 | grep -q "Successfully compiled"; then
  echo -e "${GREEN}✓ PASS${RESET} - Break and continue compiled successfully\n"
else
  echo -e "${RED}✗ FAIL${RESET} - Break and continue should compile\n"
fi

echo -e "${BLUE}=== All CFG tests complete! ===${RESET}"
