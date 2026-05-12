# Sandbox Validation Fix Changelog

**Date:** May 12, 2026
**Focus:** Fix incremental validation bug where missing baseline AST skips semantic linting

## Summary

Fixed critical bug in sandbox incremental validation where semantic linting was being skipped when the original AST snapshot was missing. The fix ensures that full-tree node collection is performed as fallback, enabling proper semantic error detection even when baseline AST is null.

---

## Changes Made

### 1. include/AST.h

**Purpose:** Add full-tree node ID collection utility for incremental validation fallback

**Changes:**

- Added `CollectAllNodeIDsInternal()` recursive function (112 lines of inline code)
  - Traverses all AST node types: Binary, Unary, Assignment, VarDecl, Cast, Block, If, While, For, BuiltinCall, FunctionCall, FunctionDef, Return, ArrayDecl, ArrayAccess, ArrayAssign
  - Collects node IDs into a set for later reference
  - Used when baseline AST is missing to mark entire available tree as changed nodes
- Exposed `VarDeclExprAST::getInitializer()` accessor method
  - Required for proper inspection of variable declarations in semantic validation

**File:** [include/AST.h](include/AST.h)

---

### 2. src/Sandbox.cpp

**Purpose:** Implement fallback to full-tree validation when baseline AST is missing

**Changes:**

- Modified `computeChangedNodes()` method fallback logic:
  - **Before:** When baseline AST missing, inserted only root node ID into `ChangedNodeIDs`
  - **After:** Calls `CollectAllNodeIDsInternal()` to collect all available node IDs, ensuring semantic linting has complete coverage
  - Added conditional logic to handle both missing-baseline and missing-current cases
  - Added debug marker: `logDebug("AST diffing: missing baseline AST, marked the full tree as changed");`

- Added debug logging around semantic linting step:
  - Log before `LintChangedNodesOnly()`: Indicates entry to semantic validation phase
  - Log after `LintChangedNodesOnly()`: Indicates completion of semantic validation
  - Controlled by `TURF_SANDBOX_DEBUG` environment variable for visibility without noise

**File:** [src/Sandbox.cpp](src/Sandbox.cpp)

**Impact:** Semantic linting now always happens even when incremental validation has missing baseline, preventing silent skipping of error detection.

---

### 3. scripts/test_ai.sh (New File)

**Purpose:** Regression test runner for AI suggestion validation path

**Created:** New file with 66 lines
**Functionality:**

- Iterates over all `.tr` test files in `tests/custom/aggressive/`
- Runs each with `TURF_SANDBOX_DEBUG=1 ./turf --smart` for AI suggestion generation
- Captures full compiler output and sandbox validation logs to timestamped directories
- **Original behavior:** Checked compiler exit code (0 = pass)
- **Updated behavior (latest):** Checks for success string `✓ Fix validated and committed` in logs

**Test Output Structure:**

```
ai_outputs/
└── runs/
    └── YYYYMMDD_HHMMSS/
        ├── broken_typo.log
        ├── undeclared_identifier.log
        ├── bad_builtin_usage.log
        ├── nested_scope_undeclared.log
        ├── array_index_undeclared.log
        └── nested_function_parse_error.log
```

**File:** [scripts/test_ai.sh](scripts/test_ai.sh)

---

### 4. Aggressive Regression Test Suite

**Purpose:** Targeted test cases exercising sandbox validation path with various error types

**Test Files Created in tests/custom/aggressive/:**

#### 4a. broken_typo.tr

- **Error Type:** Builtin function typo
- **Content:** `praintln("hello")` (should be `printline`)
- **Expected:** SLM suggests correct fix; sandbox validates
- **Status:** Currently fails - fix application in sandbox has issue

#### 4b. undeclared_identifier.tr

- **Error Type:** Undeclared variable usage
- **Content:** Multi-line code using `missing_value` without declaration
- **Expected:** SLM suggests declaration; sandbox validates fix
- **Status:** ✓ PASSES - sandbox validation succeeded

#### 4c. bad_builtin_usage.tr

- **Error Type:** Builtin with wrong argument count
- **Content:** `lengthof()` called with no arguments
- **Expected:** SLM suggests providing argument; sandbox validates
- **Status:** ✓ PASSES - sandbox validation succeeded

#### 4d. nested_scope_undeclared.tr

- **Error Type:** Undeclared variable in nested scope + empty else block
- **Content:** If-else block with undefined variable in condition
- **Expected:** Multiple errors need fixing; single SLM suggestion insufficient
- **Status:** FAILS - multiple unresolved errors prevent validation pass

#### 4e. array_index_undeclared.tr

- **Error Type:** Array access with undefined index variable
- **Content:** Array indexing using undefined variable
- **Expected:** SLM suggests declaring index; sandbox validates
- **Status:** ✓ PASSES - sandbox validation succeeded

#### 4f. nested_function_parse_error.tr

- **Error Type:** Parse error in function definition
- **Content:** Function definition containing `praintln()` typo
- **Expected:** SLM identifies typo; sandbox validates fix
- **Status:** ✓ PASSES - sandbox validation succeeded

**Directory:** [tests/custom/aggressive/](tests/custom/aggressive/)

---

### 5. Test Runner Update

**Purpose:** Change test success metric from exit code to sandbox validation success string

**File:** [scripts/test_ai.sh](scripts/test_ai.sh)

**Original Logic:**

```bash
if [ $status -eq 0 ]; then
    PASSED=...
```

- Treated compiler exit code 0 as pass
- Problem: Compiler returns 1 even when original code has errors (expected behavior)

**Updated Logic:**

```bash
if grep -q "✓ Fix validated and committed" "$log_file"; then
    PASSED=...
```

- Checks for sandbox success string in transcript
- Correctly identifies when sandbox validation actually succeeded
- Ignores compiler exit code (irrelevant for original broken code)

**Test Results:**

- Total: 6 tests
- Passed: 4 (undeclared_identifier, bad_builtin_usage, array_index_undeclared, nested_function_parse_error)
- Failed: 2 (broken_typo - fix application bug, nested_scope_undeclared - multiple unresolved errors)

---

## Bug Status

**Primary Bug (FIXED):** ✅

- **Issue:** Missing baseline AST caused `computeChangedNodes()` to mark only root node as changed, resulting in selective linting that skips most of the tree
- **Root Cause:** Fallback logic inserted only `Root->getNodeID()` instead of collecting all available nodes
- **Fix:** Use `CollectAllNodeIDsInternal()` to traverse entire AST and collect all node IDs for `ChangedNodeIDs`
- **Verification:** 4/6 tests show `✓ Fix validated and committed` in sandbox logs, proving semantic linting is now executing

**Secondary Issues (Identified):**

1. **Sandbox fix application bug:** Single-line expression fix for `praintln` → `printline` not being applied correctly in sandbox (broken_typo.log shows error still present after fix)
2. **Multiple errors in one file:** Tests with multiple unrelated errors may fail sandbox validation even if AI fix is correct (nested_scope_undeclared.tr has empty else block + undeclared var)

---

## Verification Commands

**Run regression tests:**

```bash
bash scripts/test_ai.sh
```

**Enable debug output in compilation:**

```bash
TURF_SANDBOX_DEBUG=1 ./turf --smart tests/custom/aggressive/undeclared_identifier.tr
```

**Inspect test transcripts:**

```bash
grep "Fix validated" ai_outputs/runs/*//*.log
```

---

## Files Modified

- ✏️ [include/AST.h](include/AST.h) - Added CollectAllNodeIDsInternal helper
- ✏️ [src/Sandbox.cpp](src/Sandbox.cpp) - Implemented full-tree fallback + debug markers
- ✏️ [scripts/test_ai.sh](scripts/test_ai.sh) - Updated to check success string instead of exit code
- ✨ [tests/custom/aggressive/broken_typo.tr](tests/custom/aggressive/broken_typo.tr) - New test
- ✨ [tests/custom/aggressive/undeclared_identifier.tr](tests/custom/aggressive/undeclared_identifier.tr) - New test
- ✨ [tests/custom/aggressive/bad_builtin_usage.tr](tests/custom/aggressive/bad_builtin_usage.tr) - New test
- ✨ [tests/custom/aggressive/nested_scope_undeclared.tr](tests/custom/aggressive/nested_scope_undeclared.tr) - New test
- ✨ [tests/custom/aggressive/array_index_undeclared.tr](tests/custom/aggressive/array_index_undeclared.tr) - New test
- ✨ [tests/custom/aggressive/nested_function_parse_error.tr](tests/custom/aggressive/nested_function_parse_error.tr) - New test

---

## Next Steps

1. **Investigate fix application bug** - Debug why sandbox's `ApplyFix` doesn't correctly replace `praintln` with `printline` in single-line edits
2. **Add multi-error resolution tests** - Create tests where AI suggestions resolve all errors in file
3. **Full compiler rebuild** - Resolve unrelated LLVM intrinsic error in src/Codegen.cpp for complete build verification
4. **Performance profiling** - Verify full-tree collection in incremental path doesn't significantly impact compile times
