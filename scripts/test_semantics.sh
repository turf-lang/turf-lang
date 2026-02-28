#!/bin/bash
# Test script for semantic symbol table validation

set -e

cd "$(dirname "$0")/.."

GREEN="\033[0;32m"
RED="\033[0;31m"
BLUE="\033[0;34m"
RESET="\033[0m"

echo -e "${BLUE}=== Symbol Table Semantic Tests ===${RESET}\n"

# Test 1: Use-before-declaration (should error)
echo -e "${BLUE}Test 1: Use-before-declaration${RESET}"
if ./turf tests/scope_test_use_before_decl.tr 2>&1 | grep -q "Use of undeclared identifier"; then
  echo -e "${GREEN}✓ PASS${RESET} - Detected use-before-declaration\n"
else
  echo -e "${RED}✗ FAIL${RESET} - Should detect use-before-declaration\n"
fi

# Test 2: Duplicate declaration (should error)
echo -e "${BLUE}Test 2: Duplicate declaration${RESET}"
if ./turf tests/scope_test_duplicate.tr 2>&1 | grep -q "Redeclaration of variable"; then
  echo -e "${GREEN}✓ PASS${RESET} - Detected duplicate declaration\n"
else
  echo -e "${RED}✗ FAIL${RESET} - Should detect duplicate declaration\n"
fi

# Test 3: Shadowing (should warn but compile)
echo -e "${BLUE}Test 3: Shadowing detection${RESET}"
OUTPUT=$(./turf tests/scope_test_shadowing.tr 2>&1)
if echo "$OUTPUT" | grep -q "shadows variable" && echo "$OUTPUT" | grep -q "Successfully compiled"; then
  echo -e "${GREEN}✓ PASS${RESET} - Detected shadowing and compiled\n"
else
  echo -e "${RED}✗ FAIL${RESET} - Should warn about shadowing but still compile\n"
fi

# Test 4: Unreachable code (should error)
echo -e "${BLUE}Test 4: Unreachable declaration${RESET}"
if ./turf tests/scope_test_unreachable.tr 2>&1 | grep -q "Unreachable"; then
  echo -e "${GREEN}✓ PASS${RESET} - Detected unreachable code\n"
else
  echo -e "${RED}✗ FAIL${RESET} - Should detect unreachable code\n"
fi

# Test 5: Valid scopes (should compile successfully)
echo -e "${BLUE}Test 5: Valid nested scopes${RESET}"
if ./turf tests/scope_test_valid.tr 2>&1 | grep -q "Successfully compiled"; then
  echo -e "${GREEN}✓ PASS${RESET} - Valid scopes compiled successfully\n"
else
  echo -e "${RED}✗ FAIL${RESET} - Valid code should compile\n"
fi

# Test 6: Control flow scopes (should compile successfully)
echo -e "${BLUE}Test 6: Control flow scopes${RESET}"
if ./turf tests/scope_test_control_flow.tr 2>&1 | grep -q "Successfully compiled"; then
  echo -e "${GREEN}✓ PASS${RESET} - Control flow scopes compiled successfully\n"
else
  echo -e "${RED}✗ FAIL${RESET} - Control flow code should compile\n"
fi

echo -e "${BLUE}=== All semantic tests complete! ===${RESET}"
