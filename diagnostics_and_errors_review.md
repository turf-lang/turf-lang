# Turflang Diagnostics & Errors: Architecture Review and DX Roadmap

This document outlines the current state of Turflang's error handling and Small Language Model (SLM) integration, followed by strategic recommendations to make the compiler's Developer Experience (DX) world-class.

---

## Part 1: The Current "Intelligent Compiler" State

The architecture built around the SLM and `DiagnosticEngine` is absolutely state-of-the-art. It goes far beyond standard compilers by actively analyzing and testing fixes. Here is how the pipeline currently operates:

### 1. The DiagnosticEngine (The Collector)
Every time the compiler (Parser, Resolver, TypeChecker, CFG) finds an issue, it calls `DiagnosticEngine::add()`. Instead of blindly crashing, the engine:
- Prevents "cascading noise" by guaranteeing only one primary error is shown per line.
- Highlights the exact column with a `^ Here!` marker using beautiful ANSI colors.
- If the `--smart` flag is enabled, it pauses compilation and hands the error over to the SLM Integration.

### 2. SLM Context Gathering (The Intelligence)
Before asking the AI for help, the compiler builds a hyper-specific context window (`SLMIntegration.cpp`):
- It deterministically categorizes the error (e.g., `TYPE_MISMATCH`, `UNDECLARED_VAR`).
- It extracts the `errorLine` and a few lines above and below it.
- **The Genius Part:** It does a dynamic text scan (`findFunctionSignatureLine` and `findVariableDeclarationLine`) to find where the broken variable or function was originally declared (even if it's 100 lines away) and "pins" that definition to the AI's context window.

### 3. The Local AI Model (`qwen2.5-coder-1.5b`)
It feeds the error, the context, and targeted documentation to a local LLM running via `llama.cpp`. It strictly forces the model to respond in JSON with an `explanation`, a `suggested_comment`, and the exact `replacement_line` of Turf-Lang code to fix the issue.

### 4. Sandbox Validation (The Ultimate Safety Net)
**The compiler fact-checks the AI.** Before showing the AI's suggested fix to the user, `validateAndApplySuggestion()`:
- Clones the current compiler state into a `Sandbox`.
- Replaces the broken line of code with the AI's `replacement_line`.
- Runs a silent, lightweight compilation pass in the background.
- If the AI's fix *actually* compiles successfully, it assigns a high confidence score. If it fails, it silently discards the AI's fix so the user never sees a hallucination.

### 5. The Output
Finally, `DiagnosticEngine` prints the standard error, followed by a bright cyan `[AI Suggestion]` block containing a human-readable explanation and a verified code patch.

---

## Part 2: Errors.h and Developer Experience (DX) Improvements

The `Errors.h` file is in excellent shape, containing highly specific error classes (`UseBeforeDeclarationError`, `ArrayBoundsError`, etc.). However, to make the DX the main attraction of Turf-Lang, here are 5 high-impact improvements:

### 1. Source Spans instead of just Source Locations
Right now, `SourceLocation` only tracks `{Line, Col}`. If you update the Lexer to track `{Line, StartCol, EndCol}` for tokens, your `DiagnosticEngine` can underline the exact expression that is failing, rather than just pointing a single `^` at it.
**Example improvement:**
```
  7 |     io.printline(add(5 + 6, add(3, "4")));
    |                             ^^^^^^^^^^^
    |                             Type mismatch: expected 'int', got 'string'
```

### 2. Error Codes and Explainers
Every error class should have a unique, searchable Error Code (e.g., `E001` for `SyntaxError`, `E042` for `TypeError`). 
If a user hits `E042`, they should be able to run `turf --explain E042` in their terminal, and your compiler prints out a beautiful, multi-paragraph markdown explanation of how Turf's type system works with examples (similar to Rust).

### 3. Decouple Hints from Error Messages
Right now, inside `Errors.h`, the "Hint:" strings are hardcoded directly into the message body using string concatenation:
```cpp
Colors::BRIGHT_RED + "Type mismatch!\n" + Colors::RESET + "  " + Msg + "\n  " + Colors::BRIGHT_GREEN + "Hint: ..."
```
Change `TurfError` to accept a separate `Hint` string: `TurfError(Loc, Msg, Hint)`. This allows the `DiagnosticEngine` to dynamically decide how to render hints, or even pass the hint separately to the SLM to give the model more context.

### 4. Broaden the "Did You Mean?" Engine
You currently have `SuggestionIndex.h` which does fuzzy-matching to suggest correct names. Expand this beyond variables and functions to:
- **Struct Fields:** If the user types `user.pasword`, suggest `user.password`.
- **Module Methods:** If the user types `string.lenght()`, suggest `string.length()`.
- **Keywords:** If the user types `funtion`, suggest `fn`.

### 5. Separation of Warnings and Errors
Calling `raise()` on a `TurfError` usually throws a `longjmp` and halts the compilation pass. As you build more advanced CFG features (like "Unused variable"), you will need a dedicated `TurfWarning` class that registers with the `DiagnosticEngine` but **never** interrupts the execution flow.
