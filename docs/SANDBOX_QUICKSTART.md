# Sandbox System Quick Reference

## Building with Sandbox Support

```bash
cd /mnt/c/Coding/turf_lang
./scripts/update_compiler.sh
```

The build script now includes `src/Sandbox.cpp` alongside other compiler sources.

---

## Using the Sandbox System

### Basic Usage: Enable SLM with Sandbox Validation

```bash
./turf <source.tr> --smart
```

When `--smart` is enabled:

1. SLM is invoked on errors
2. High-confidence suggestions are automatically validated in a sandbox
3. If validation passes, the fix is committed to source
4. If validation fails, original error is shown

### Debug Mode: See Sandbox Operations

```bash
export TURF_SANDBOX_DEBUG=1
./turf <source.tr> --smart
```

Output will include:

```
[Sandbox] Sandbox created: 5 lines of source captured.
[Sandbox] Applied fix to line 2
[Sandbox] Validation PASSED
[Sandbox] ✓ Fix validated and committed
```

---

## Example: Testing Error Correction

### Scenario 1: Undeclared Variable

```turf
# test_undeclared.tr
int x = 5;
print(y);  // Error: y is undeclared
```

```bash
./turf test_undeclared.tr --smart
```

Expected:

- SLM suggests: `print(x);`
- Sandbox validates the fix
- Source is automatically corrected
- Compilation continues

### Scenario 2: Wrong Function Arguments

```turf
# test_typeof.tr
print(typeof())  // Error: typeof requires 1 argument
```

```bash
./turf test_typeof.tr --smart
```

Expected:

- SLM suggests: `print(typeof(5));`
- Sandbox validates syntax and semantics
- Fix is applied and committed

---

## Confidence Scoring Breakdown

The sandbox uses **deterministic confidence scoring**:

| Signal                       | Points | Condition                                      |
| ---------------------------- | ------ | ---------------------------------------------- |
| Error category deterministic | +0.4   | Category resolved by hardcoded rules (not SLM) |
| Fix mode is MODIFY           | +0.2   | Suggestion includes a patch                    |
| Patch is non-empty           | +0.2   | code_patch has content                         |
| Single-line patch            | +0.1   | No embedded newlines or bare braces            |
| On-topic patch               | +0.1   | Patch mentions the error keyword               |

**Threshold**: 0.9 (fixes below this are rejected)

**Example**: A fix with all signals would score 1.0 and be committed.

---

## API Reference

### C++ API for Integration

#### Sandbox Class

```cpp
#include "Sandbox.h"

// Create sandbox from current compiler state
auto sandbox = CreateSandbox();

// Apply a single-line fix
bool success = sandbox->applyFix(2, "print(x);");

// Validate the fix in the sandbox
bool isValid = sandbox->validate();

// Check if original error is resolved
bool resolved = sandbox->isOriginalErrorResolved(2);

// Commit fix to main compiler state
sandbox->commitToMain();
```

#### SLM Integration

```cpp
#include "SLMIntegration.h"

// Get SLM suggestion (without sandbox validation)
auto suggestion = SLM::fetchSuggestion(errorMsg, errorLine);

// Get SLM suggestion with sandbox validation
auto suggestion = SLM::validateAndApplySuggestion(errorMsg, errorLine);

// Check if sandbox validated the fix
if (suggestion.sandbox_validated) {
  // Fix was successfully applied to main state
}
```

#### DiagnosticEngine Public Accessors

```cpp
#include "Lexer.h"

// Read diagnostics
const auto &diags = DiagnosticEngine::getDiagnostics();

// Clear diagnostics
DiagnosticEngine::clearDiagnostics();

// Check diagnostic count
size_t count = DiagnosticEngine::getDiagnosticCount();
```

---

## Configuration

### Confidence Threshold

Currently hardcoded to `0.9f` in `src/Sandbox.cpp`:

```cpp
const float CONFIDENCE_THRESHOLD = 0.9f;
```

To make configurable:

1. Add CLI flag: `./turf <file> --smart --sandbox-confidence=0.85`
2. Update `ApplyAndValidateFix()` to read from global variable

### Debug Logging

Set `TURF_SANDBOX_DEBUG=1` environment variable to see:

- Sandbox creation/destruction
- Fix application and validation results
- Commit/reject decisions

---

## File Structure

```
include/
  Sandbox.h              ← Sandbox class definition
  SLMIntegration.h       ← Updated with sandbox API
  Lexer.h                ← Updated with DiagnosticEngine accessors

src/
  Sandbox.cpp            ← Implementation (new)
  SLMIntegration.cpp     ← Updated with sandbox integration

docs/
  SANDBOX_ARCHITECTURE.md ← Design document (comprehensive)
```

---

## Testing Checklist

- [ ] Rebuild with `./scripts/update_compiler.sh`
- [ ] Test error detection: `./turf tests/custom/typeof_error_no_args.tr`
- [ ] Test with SLM: `./turf tests/custom/typeof_error_no_args.tr --smart`
- [ ] Enable debug: `export TURF_SANDBOX_DEBUG=1 && ./turf ... --smart`
- [ ] Check confidence scoring: Look for "Computed Confidence" in output
- [ ] Verify validation: Look for "[Sandbox]" messages in stderr

---

## Troubleshooting

### Build Fails with "Sandbox.h not found"

```bash
# Ensure -Iinclude is in compiler flags
./scripts/update_compiler.sh
```

### No Sandbox Messages in Output

```bash
# Enable debug logging
export TURF_SANDBOX_DEBUG=1
./turf test.tr --smart 2>&1 | grep Sandbox
```

### Confidence Too Low

- Check error category: may be GENERAL (low confidence fallback)
- Verify SLM is returning MODIFY fix_mode
- Check if code_patch is non-empty and single-line

### Fix Not Applied

- Confidence may be below 0.9 threshold
- Validation may have failed (check error output)
- Check TURF_SANDBOX_DEBUG for details

---

## Performance Notes

- **MVP Implementation**: Source code fully deep-copied; AST/SymbolTable stubs
- **Re-validation**: Currently re-parses entire source (future optimization: region-only)
- **Memory**: Sandbox holds duplicate source; negligible for typical files
- **Speed**: Validation adds ~10-50ms per error (depends on file size and complexity)

---

## Future Enhancements

### Phase 2: Full Cloning

- Implement AST clone methods on all ExprAST subclasses
- Add SymbolTable copy constructor
- Enable incremental validation

### Phase 3: Advanced Heuristics

- Multi-line fix support (with extra safeguards)
- Cross-function type tracking
- Automatic test generation for fixes

### Phase 4: User Interaction

- Interactive fix preview ("Apply? [Y/n]")
- Partial fix acceptance
- Confidence threshold override

---

## References

- **Architecture**: [docs/SANDBOX_ARCHITECTURE.md](../SANDBOX_ARCHITECTURE.md)
- **Headers**: [include/Sandbox.h](../../include/Sandbox.h)
- **Implementation**: [src/Sandbox.cpp](../../src/Sandbox.cpp)
- **Integration**: [src/SLMIntegration.cpp](../../src/SLMIntegration.cpp)
