# Sandbox-Based SLM Auto-Fix System

## Overview

The sandbox system provides a **safe testing ground** for SLM-driven auto-fixes. Before applying any AI suggestion to the main compiler state, the fix is validated in an isolated sandbox environment.

## Architecture

### Main Components

#### 1. **Main Compiler Environment**

- **Source Code**: Original user input
- **AST**: Abstract Syntax Tree
- **Symbol Table**: Variable declarations and scope tracking
- **Semantic Analysis Results**: Type information, control flow analysis

This environment remains **stable and untouched** unless a fix is validated and committed.

#### 2. **Sandbox Environment**

An isolated replica that includes:

- **Deep Copy of Source Code**: Independent copy of source lines
- **Cloned AST**: (Stub; full cloning requires per-node clone methods)
- **Cloned Symbol Table**: (Stub; requires SymbolTable copy constructor)
- **Captured Compiler State**: Diagnostics and analysis results at the time of the error

The sandbox is used exclusively for:

- Testing SLM-generated fixes
- Running speculative compilation passes
- Validating fixes without affecting the main state

---

## Error Handling Workflow

### Step 1: Error Detection

When a compile-time error is detected:

- Error type, line number(s), and context are captured
- DiagnosticEngine collects the error
- SLM is invoked if `--smart` flag is enabled

### Step 2: Trigger SLM

The SLM is passed:

- Error message (stripped of ANSI codes)
- Error line number
- Code context (window size determined by error category)
- Relevant documentation

The SLM returns:

- `explanation`: Why the error occurred
- `fix_mode`: "MODIFY" or "NONE"
- `code_patch`: Suggested single-line fix
- `confidence`: Deterministically computed from signals (not LLM-generated)

### Step 3: Confidence Gate

Only proceed if:

- `confidence >= 0.9` (threshold, currently hardcoded in `ApplyAndValidateFix`)
- `fix_mode == "MODIFY"` (not NONE)

Otherwise, reject the fix and fall back to standard error reporting.

### Step 4: Single-Line Constraint

- Only allow automatic fixes that affect exactly **ONE line**
- Multi-line fixes are rejected with `fix_mode = "NONE"`
- Structural AST changes are not supported

### Step 5: Apply Fix in Sandbox

In the sandbox environment:

1. Replace the error line with the suggested fix
2. Update corresponding AST nodes (stub; full implementation deferred)
3. Recompute affected parts:
   - AST (localized updates only, no full rebuild)
   - Symbol Table (localized scope traversal)

**Important**: Perform only **localized updates**, never full rebuilds.

### Step 6: Validation

Re-run compilation phases inside the sandbox:

- **Syntax check**: Verify the patched line is valid Turf-Lang syntax
- **Semantic analysis**: Type checking, variable resolution, control flow
- **No codegen**: Skip LLVM IR generation for speed

Ensure:

- ✓ No new errors introduced
- ✓ Original error is resolved
- ✓ No cascading failures

### Step 7: Commit or Reject

#### ✅ If Validation Succeeds:

1. Propagate the fix to the main environment:
   - Update source code
   - Update AST
   - Update symbol table
2. Mark fix as `sandbox_validated = true`
3. Show corrected code to the user
4. Continue compilation with fixed source

#### ❌ If Validation Fails:

1. Discard all sandbox changes
2. Revert to original source
3. Set `sandbox_validated = false`
4. Show original error to the user
5. Do NOT attempt to apply the fix

---

## Implementation Details

### Confidence Scoring

Confidence is computed **deterministically** (not by LLM) from measurable signals:

```
Score Breakdown:
  +0.4  if error category was resolved by hardcoded rules (not SLM fallback)
  +0.2  if fix_mode == "MODIFY"
  +0.2  if code_patch is non-empty
  +0.1  if code_patch is a single line (no embedded newlines)
  +0.1  if code_patch mentions the relevant error keyword

Maximum: 1.0  |  Minimum: 0.0
Threshold: 0.9
```

This approach avoids the **template anchoring problem** where small models copy literal values from the prompt.

### Files and APIs

#### Core Interfaces

**`Sandbox.h`**: Sandbox class and global management

```cpp
class Sandbox {
  bool applyFix(int lineNumber, const std::string &newLine);
  bool validate();
  bool isOriginalErrorResolved(int errorLine) const;
  void commitToMain();
  void reject();
};

std::unique_ptr<Sandbox> CreateSandbox();
bool ApplyAndValidateFix(Sandbox &sandbox, int lineNumber,
                         const std::string &fixedLine,
                         const SLM::AISuggestion &suggestion);
```

**`SLMIntegration.h`**: SLM suggestion functions

```cpp
AISuggestion fetchSuggestion(const std::string &errorMessage, int errorLine);
AISuggestion validateAndApplySuggestion(const std::string &errorMessage,
                                        int errorLine);
bool verifySuggestion(const AISuggestion &suggestion,
                      const std::string &originalSource,
                      int errorLine);
```

**`Lexer.h`**: DiagnosticEngine public accessors

```cpp
class DiagnosticEngine {
  static const std::vector<Diag> &getDiagnostics();
  static void clearDiagnostics();
  static size_t getDiagnosticCount();
};
```

### Cloning Strategy (MVP)

For the MVP, cloning is **stubbed**:

1. **Source Code**: Simple deep copy (trivial)

   ```cpp
   std::vector<std::string> cloneSource(const std::vector<std::string> &source)
     return source;  // Vector copy is deep by default
   ```

2. **AST**: Deferred (requires per-node clone methods)

   ```cpp
   std::unique_ptr<ExprAST> cloneAST(const ExprAST *original)
     return nullptr;  // Stub for MVP
   ```

3. **Symbol Table**: Deferred (requires public copy interface)
   ```cpp
   std::unique_ptr<SymbolTable> cloneSymbolTable(const SymbolTable *original)
     return std::make_unique<SymbolTable>();  // Empty table for MVP
   ```

**Future Work**: Implement full cloning by adding:

- `virtual std::unique_ptr<ExprAST> clone() const` to each AST node
- Public copy constructor or `clone()` method to SymbolTable

---

## Design Goals ✓

- ✓ Avoid repeated full AST/symbol table rebuilds (localized updates only)
- ✓ Enable fast speculative execution via sandboxing
- ✓ Ensure compiler correctness is never compromised
- ✓ Restrict auto-fixes to safe, minimal edits (single-line only)
- ✓ Maintain deterministic compiler behavior
- ✓ Provide fallback hooks for human-in-the-loop correction

---

## Example Workflow

### Scenario: User code with undeclared variable

**Input source (`test.tr`):**

```turf
int x = 5;
print(y);  // <- error: 'y' is undeclared
```

**Step 1: Error Detection**

- DiagnosticEngine detects undeclared variable `y` at line 2

**Step 2: SLM Invocation**

- Error category: `UNDECLARED_VAR`
- Context window: ±5 lines (category-dependent)
- SLM returns: confidence=0.95, fix_mode="MODIFY", code_patch="print(x);"

**Step 3: Confidence Check**

- 0.95 >= 0.9 ✓
- fix_mode == "MODIFY" ✓
- Proceed to sandbox

**Step 4: Sandbox Application**

- Create sandbox with deep copy of source
- Replace line 2: "print(y);" → "print(x);"
- Update AST node for the variable reference

**Step 5: Sandbox Validation**

- Re-parse line 2: "print(x);"
- Semantic analysis: Variable `x` found in symbol table ✓
- No new errors ✓

**Step 6: Commit**

- Copy modified source to main environment
- Update main AST and symbol table
- Mark fix as validated
- Continue compilation with `print(x);`

**Output:**

```
[Sandbox] ✓ Fix validated and committed
Compilation successful!
```

---

## Configuration & Debugging

### Environment Variables

- **`TURF_SANDBOX_DEBUG`**: Enable debug logging from sandbox operations
  ```bash
  export TURF_SANDBOX_DEBUG=1
  ./turf --smart test.tr
  ```

### Confidence Threshold

Currently hardcoded to `0.9f` in `ApplyAndValidateFix()`. To make it configurable:

1. Add a global variable:

   ```cpp
   static float g_SandboxConfidenceThreshold = 0.9f;
   ```

2. Add CLI flag (e.g., `--sandbox-confidence=0.8`)

3. Update ApplyAndValidateFix:
   ```cpp
   if (suggestion.confidence < g_SandboxConfidenceThreshold) { ... }
   ```

---

## Future Enhancements

### Phase 2: Full Cloning

- Implement `clone()` methods on all AST node types
- Add SymbolTable copy constructor
- Enable true AST diffing for localized updates

### Phase 3: Incremental Validation

- Track dependencies between errors
- Skip full re-parse; validate only affected region
- Cache analysis results for unchanged code

### Phase 4: Advanced Heuristics

- Multi-line fix support (with safeguards)
- Cross-function context (for signature mismatches)
- Automatic test generation to verify fixes

### Phase 5: Human-in-the-Loop

- Interactive fix preview before auto-commit
- Partial fix acceptance (user can manually edit)
- Confidence threshold override with user approval

---

## Testing

### Test Case: `typeof_error_no_args.tr`

```turf
// Error: 'typeof' requires exactly 1 argument, but 0 were given
print(typeof())
```

**Expected output with `--smart`:**

```
[SLM] Error category: BUILTIN_FUNCTION
...
[AI Suggestion] Add an expression inside the parentheses...
Suggested fix:
  print(typeof(5));
[Sandbox] ✓ Fix validated and committed
```

### Running Tests

```bash
# Rebuild with sandbox support
./scripts/update_compiler.sh

# Test with SLM enabled
./turf tests/custom/typeof_error_no_args.tr --smart

# Enable debug logging
export TURF_SANDBOX_DEBUG=1
./turf tests/custom/typeof_error_no_args.tr --smart
```

---

## Summary

The sandbox-based validation system provides a **hybrid approach** combining:

- **Traditional compiler guarantees**: Main state remains untouched until fix is verified
- **AI-assisted flexibility**: SLM suggestions are tested speculatively before commitment
- **Safety-first validation**: No fix is applied unless it resolves the original error and introduces no new ones
- **Performance**: Only localized updates, minimal recomputation

This design ensures that **compiler correctness is never compromised** while enabling intelligent, automated error correction.
