# Compiler Tools and Diagnostics

The Turf-Lang compiler (`turf`) is more than just a translator; it includes a built-in Linter, advanced CLI options, and a Small Language Model (SLM) for AI-powered error correction.

---

## Command Line Interface

When invoking the compiler, you can modify its behavior with various flags.

```bash
turf <filename.tr> [-o output] [--emit-llvm] [--smart]
```

### Options

- `-o <name>`: Specifies the output executable name. Defaults to `program` (or `program.exe` on Windows).
- `--emit-llvm`: Instead of generating a native binary, the compiler will output a human-readable LLVM IR file (`<name>.ll`). This is excellent for debugging the compiler or seeing how Turf-Lang code translates to lower-level instructions.
- `--smart`: Enables the **SLM Integration**.

---

## Built-In Linter

During compilation, Turf-Lang runs a semantic Linting pass over the Abstract Syntax Tree before any code generation occurs. 

The linter detects logical bugs and emits warnings (which do not halt compilation but alert the developer).

### Shadowing Warnings
If you declare a variable with the same name as one in an outer scope, the linter warns you to prevent accidental misuse.
```text
Warning: Variable 'x' shadows a previous declaration.
```

### Unreachable Code
If the linter detects statements following an unconditional `return`, `break`, `continue`, or `raise`, it flags the dead code.
```text
Warning: Unreachable code detected after return statement.
```

### Void Function Warnings
If you attempt to assign the result of a `void` function to a variable or array, the linter and semantic pass will throw an error, preventing invalid LLVM IR generation.

---

## SLM Integration (The `--smart` Flag)

Turf-Lang features a groundbreaking integration with an onboard Small Language Model (SLM) powered by `llama.cpp`.

When you run the compiler with the `--smart` flag:
```bash
turf main.tr --smart
```
If your code contains syntax or semantic errors, the compiler does not just print the error and exit. Instead:

1. **Context Gathering:** The engine extracts the error message and the surrounding source code lines.
2. **AI Inference:** The onboard model generates an `AISuggestion` describing how to fix the bug and provides a precise code patch.
3. **Sandbox Validation:** The patch is temporarily applied to a "sandbox" version of the Abstract Syntax Tree. The compiler attempts to re-compile the sandbox.
4. **Verified Output:** If the sandboxed code compiles successfully, the compiler presents the guaranteed-to-work fix to the user. If the AI hallucinated or the fix is wrong, it is silently discarded.

*(Note: Running with `--smart` requires the language model binary to be available on your system, and it will use additional RAM/CPU during error states).*

---

## Frequently Asked Questions (FAQs)

**Q: Does `--smart` rewrite my code files automatically?**
A: No, the SLM integration only provides validated suggestions in the console output. You must manually apply the fix to your source file.

**Q: Do warnings stop compilation?**
A: No. Warnings from the Linter (like shadowing) will print to the console, but the compiler will still emit a functional binary. Only strict Errors halt the pipeline.
