# CLI, Diagnostics & Static Analysis

Reference for the `turf` compiler's command-line interface, its error/warning reporting system, and the compile-time control-flow (CFG) analyses that run on every build.

---

## Part 1 — CLI Usage

### Basic invocation

```
turf <input.tr> -o <output_name>
./<output_name>
```

The compiler binary is named `turf`. The input file is a required positional argument and its name **must end in `.tr`** — anything else is a hard error (`Input file must have a '.tr' extension`). If `-o` is omitted, the output name defaults to the input filename's basename with `.tr` stripped (falling back to `program` if that would be empty).

Two usage forms exist:

```
turf <filename.tr> [-o output] [--emit-llvm] [--smart] [--dump-ast] [--resolve]
     [--dump-resolver-symbols] [--dump-resolved] [--typecheck]
     [--dump-typechecked] [--dump-modules] [--link <libname>]
turf --explain E0NN
```

Any other token starting with `-` that isn't recognized is rejected with `Unknown option: <arg>` and exit code 1.

### Flags

| Flag | Implies | What it does |
|---|---|---|
| `--emit-llvm` | — | Writes human-readable LLVM IR to `<output>.ll` instead of compiling and linking a native binary. |
| `--smart` | — | Enables the SLM (AI-assisted) fix-suggestion pipeline. See below — deliberately not documented in depth here. |
| `--dump-ast` | — | Prints the parsed AST (functions/structs, then top-level statements) via `ASTPrinter`. Runs *before* the Resolver, so nodes show no resolution annotations. |
| `--resolve` | — | Prints a `[Resolver] ...` banner confirming the Resolver pass succeeded, plus the final scope depth. |
| `--dump-resolver-symbols` | `--resolve` | Same banner, plus a full dump of the symbol table. |
| `--dump-resolved` | `--resolve` | Same banner, plus an AST dump annotated with a green `[resolved ✓]` / red `[NOT resolved ✗]` badge per node. |
| `--typecheck` | `--resolve` | Prints a `[TypeChecker] ...` banner confirming the TypeChecker pass's pass/fail status. |
| `--dump-typechecked` | `--resolve` + `--typecheck` | Same banner, plus an AST dump annotated with each node's inferred type (`[: type]`) and module qualifiers. Only printed if type-checking succeeded. |
| `--dump-modules` | `--resolve` | Dumps module-system introspection (imports, type ownership, flat function tables). |
| `-o <name>` | — | Sets the output binary/IR base name. |
| `--link <lib>` | — | Adds `-l<lib>` to the final linker invocation, on top of any libraries a program's own `extern from "libname" fn` declarations already pull in. Repeatable. |

> **Note — these flags don't turn the passes on.** The Resolver and TypeChecker run **unconditionally on every compile**, regardless of any flag. `--resolve`, `--typecheck`, and the `--dump-*` family only control whether extra banners or AST dumps are printed — never whether the underlying pass runs. If either pass fails, compilation aborts before code generation ever sees the AST, flags or no flags.

### `--explain E0NN`

```
turf --explain E001
```

A separate mode, handled before normal argument parsing — it takes no `.tr` input file at all.

- Missing code argument → `Usage: turf --explain E0NN`, exit 1.
- Unknown code → `Unknown error code: E0NN`, exit 1.
- On success: prints a bold-cyan `E0NN  ClassName (CATEGORY)` header, then the explanation text, then exits 0.

The explanation text is resolved from `docs/errors/codes/` on disk using a three-tier fallback (most specific first): a per-code doc file (`E0NN.txt`), then a construct-specific doc (routed by keyword classification of the error's short description), then a plain per-category doc as the coarsest fallback. If no doc file can be found on disk at all, the code's short auto-generated description is printed instead.

Rendering: the first non-blank line is a bold cyan title; any line indented four or more spaces is rendered as a green "code sample" line; everything else is body prose, with `` `backtick` `` spans highlighted in bold bright-green (backticks stripped).

```
$ turf --explain E001
E001  ArrayBoundsError (ARRAY_ERROR)

You tried to reach past the end of an array
...
```

### `--smart`

`--smart` enables an AI-assisted fix-suggestion pipeline: when the first *error* in a compile pass fires, the compiler asks a small language model for a suggested fix, sandbox-validates it, and — if the model is confident — replaces the plain error message with an `[AI Suggestion]` block (explanation, optional code patch, optional warning) instead of the default diagnostic. Subsequent errors in the same pass just note that they may be a cascading effect of the first one, rather than triggering another model call. Warnings never invoke this pipeline. The prompt design, sandbox mechanics, and model internals are out of scope for this reference.

### Compilation pipeline (context, not a flag)

The compiler does a two-pass parse: a "pre-pass" that hoists all function/struct/enum/import declarations and codegens struct layouts and function prototypes, followed by a "main pass" that runs the mandatory Resolver → TypeChecker → Codegen pipeline and performs the CFG-based static analyses (Part 3) as each function is generated. The `--dump-*` flags all act on the main-pass AST.

---

## Part 2 — Diagnostics & Errors

### Errors vs. warnings

Every diagnostic is either a `TurfError` (fatal) or `TurfWarning` (non-fatal):

- **Errors** record the diagnostic and then abort the current recovery unit: if the compiler is mid-recovery (e.g. skipping to the next statement after a parse error), it jumps back to that recovery point and keeps going; if not, it flushes all collected diagnostics and exits with status 1.
- **Warnings** record the diagnostic and always return normally — they never interrupt compilation.

Diagnostics can carry:
- A **hint** — an optional one-line remediation suggestion, rendered as a green `Hint: ...` line.
- **Related locations** — zero or more secondary source positions (e.g. "declared here", "first freed here") rendered as blue `Note: ...` lines.

### Error codes

Every registered error class has a stable `E0NN` code, assigned once and never reassigned. As of this writing there are **68 codes, E001–E068**.

> **Note:** the source's own header comment claims "67" registered call sites — that count is stale by one. `E068` (`UnknownStructFieldError`) was appended after the comment was written. Treat 68 as the current, correct count.

Assignment order is alphabetical by (source file, class name) at the time each code was added — it's an opaque identifier, not a severity or category ranking.

A small curated subset of codes is flagged `Important` — genuinely non-obvious concepts where the one-line message alone probably isn't enough:

- **E010** `RecursionWithoutBaseError`
- **E017** `DoubleFreeError`
- **E018** `UseAfterFreeError`
- **E035** `InvalidTransitionError`
- **E036** `InvalidTransitionTargetError`

These get a stronger, differently-colored footer nudge (see below) urging you to read the full `--explain` entry before patching the code.

### How a diagnostic is printed

```
── error at line 4, col 12 [E001], in file geometry.tr

You tried to reach past the end of an array

Note: declared here (line 2, col 5)
Hint: array size is fixed at declaration — did you mean a smaller index?
   4 │ mut last: int = primes[3];
     │                        ^ Here!

Run `turf --explain E001` for more details.
```

Top to bottom:

1. **Header** — `── error at line X, col Y [E0NN], in file <name>` (bright red for errors, bright yellow for warnings). The `[E0NN]` tag only appears if the diagnostic resolved to a real code. The `, in file ...` clause only appears when the diagnostic came from an imported module, not the file you compiled directly.
2. **Message body**.
3. Zero or more blue `Note: ...` lines, one per related location.
4. An optional green `Hint: ...` line.
5. A **source snippet** with a `│` gutter, showing the exact source line with an underline beneath the offending span, suffixed `" Here!"`.
6. A footer (only when the diagnostic has a resolved code):
   - Ordinary codes: dim `Run \`turf --explain E0NN\` for more details.`
   - `Important`-flagged codes: a bolder, bright-cyan `Worth reading in full -- run \`turf --explain E0NN\` before you patch this one.`

### Source-span underlining

The underline spans the **full width of the offending token**, not just a single caret, whenever the token's end position was recorded during lexing — computed as `^` followed by `~` repeated for the rest of the token's width. If no end position is available (this can happen for larger, multi-token spans such as a whole sub-expression), the underline falls back to a bare `^`.

### Cascade suppression

If a diagnostic has already been recorded on a given source line, every subsequent diagnostic on that same line — error or warning — is silently dropped. This keeps one root-cause typo from producing a wall of downstream noise. All collected diagnostics are sorted by (line, column) before being printed, so output order reflects the source file, not the order in which compiler passes happened to raise them.

### "Did you mean?" suggestions

When you reference an unknown name — a variable, type, keyword, struct field, struct method, module symbol, or enum variant — the compiler tries to find close matches among the names actually in scope and suggests them.

Matching uses restricted Damerau-Levenshtein edit distance, meaning an adjacent-character transposition (`itn` vs. `int`) counts as a single edit rather than two substitutions. The distance threshold scales with query length: 1 for queries of four characters or fewer, 2 for up to seven characters, and roughly a third of the string's length beyond that.

```
$ turf typo.tr
── error [E0NN]
Undeclared variable 'contuner'.
Hint: did you mean 'container'?
```

Categories covered: undeclared variables, unknown type names, misspelled keywords, unknown struct fields, unknown struct methods, unknown module functions/symbols, and unknown enum variants. (Keyword matching deliberately excludes `static` — it sits at edit-distance 2 from the very common identifier `state`, which produced constant false-positive "did you mean 'static'?" suggestions on ordinary parameter names.)

---

## Part 3 — Compile-Time Static Analysis (CFG)

Turf builds a control-flow graph (CFG) for every function body and runs a fixed sequence of analyses over it — unconditionally, on every compile, regardless of any flag. This is separate from (and runs in addition to) ordinary type checking.

### Cyclomatic-complexity warning

**Triggers when:** a function's cyclomatic complexity — `M = E - N + 2`, counted over the CFG's blocks and edges including the virtual exit block — exceeds **15**.

**Diagnostic:** `CyclomaticComplexityWarning` (non-fatal) — *"Function 'X' has cyclomatic complexity N (threshold: 15). Consider breaking it into smaller functions."*

```turflang
fn complex(x: int): int {
    if x == 1 { return 1; }
    if x == 2 { return 2; }
    if x == 3 { return 3; }
    // ... a dozen more independent branches ...
    return 0;
}
```

A function like this still compiles and runs — the warning is advisory only.

### Unreachable code / dead branches

**Triggers when:** a block in the CFG is never reachable by a walk from the function's entry block, but still contains statements (unreachable code) or ends in a branch (dead branch).

```turflang
fn main(): int {
    while true {
        return 1;
    }
    return 2;   // unreachable — nothing ever exits the loop to reach here
}
```

> **Note — these bypass the normal diagnostic pipeline entirely.** Unlike every other diagnostic in this document, unreachable-code and dead-branch warnings are printed as **raw, uncolored lines directly to stderr** — no `[E0NN]` code, no color, no hint, no source-line snippet:
> ```
> Warning: Unreachable code in block 'while.end' of function 'main'
> ```
> Dedicated `UnreachableBlockWarning`/`DeadBranchWarning` classes exist in the source but are not actually used for this — this diagnostic path was verified live against the compiled binary. Compilation still succeeds.

### Missing return

**Triggers when:** a function's declared return type is non-`void`, but some path through the CFG reaches the exit block without an explicit `return`.

**Diagnostic:** `MissingReturnError` (**E009**, fatal) — *"The function 'X' promises to give back a result, but sometimes it doesn't!"*

```turflang
fn maybeGetValue(flag: bool): int {
    if flag {
        return 1;
    }
    // ERROR: no return on the else path
}
```

`void`-returning functions are never checked for this.

### Statement after terminator

Turf actually has **two separate diagnostics** for dead code following a `return`/`break`/`continue`, depending on what the dead code is.

**Anything except a variable declaration** — triggered as soon as the CFG builder visits a statement in a block that already has a terminator:

**Diagnostic:** `StatementAfterTerminatorError` (**E012**, fatal) — *"This code comes after a return/break/continue, so it's impossible to reach it!"*

```turflang
fn main(): int {
    return 0;
    print_thing();   // [E012], reported at the `return 0;` line
}
```

> **Note:** the error is anchored at the *terminator's* location (the `return` line), not at the unreachable statement's own line — confirmed by compiling the example above.

**A variable declaration**, specifically, takes a completely different, earlier code path — a `SymbolTable`-tracked "current scope has early exit" flag, not CFG block-terminator state:

**Diagnostic:** `UnreachableCodeError` (**E013**, fatal) — *"This declaration will never happen because you used 'return' before it!"*

```turflang
fn main(): int {
    return 0;
    mut x: int = 5;   // [E013], NOT [E012]
}
```

> **Verified live**: this exact snippet produces `[E013]`, not `[E012]`. Don't treat "statement after terminator" as one warning class — it's two distinct diagnostics, chosen based on whether the dead statement is a declaration or anything else.

### Use-before-initialization

**Triggers when:** a forward dataflow pass over the CFG (definite assignment) finds a read of a locally-declared variable that isn't guaranteed to have been assigned on *every* path reaching that read. Function parameters are always considered pre-assigned and excluded from this check.

**Diagnostic:** `DefiniteAssignmentError` (**E008**, fatal) — *"Oops! You're using 'X' before it has a value."*, with a related-location note pointing at the declaration.

```turflang
fn example(): int {
    mut y: int = 0;
    do {
        if y > 100 {
            y = 5;
        }
    } while y > 0;   // ERROR: on the first pass, `y` isn't definitely assigned
                      // yet when the condition is checked
    return y;
}
```

This check also applies inside loop/branch/`compare` condition expressions themselves, not just ordinary statement bodies.

### Dead-store / unused-variable warnings

**Triggers when:** a backward liveness dataflow pass over the CFG finds either a local variable that's never read at all (unused), or a write to a local whose value is never read afterward — overwritten before the next read, or the function exits first (dead store). Parameters are excluded from both checks.

**Diagnostics** (both non-fatal):
- `UnusedVariableWarning` — *"Variable 'X' is declared but never used."*
- `DeadStoreWarning` — *"Variable 'X' is assigned but the value is never read (dead store)."*

```turflang
fn main(): int {
    mut x: int = 5;
    x = 10;
    return 0;
}
```

> **Verified live** against the compiled binary: this exact snippet produces **three** warnings — a `DeadStoreWarning` on `x = 5` (line with the initializer), a second `DeadStoreWarning` on `x = 10`, and one `UnusedVariableWarning` for `x` overall. All three are non-fatal; the program still compiles and links. (Note: a comment in this repo's own test suite claims `x = 10` is *not* a dead store "because `x` is subsequently returned" — that's incorrect for this snippet, since the function returns the literal `0`, never `x`. Trust the live behavior above, not that comment.)

### Infinite-loop detection

**Triggers when:** the CFG contains a closed strongly-connected component reachable from entry (found via Tarjan's SCC algorithm) with no outgoing edge to anything outside itself. In practice this fires on `while true { ... }` / `do { ... } while(true);` loops with a literal `true` condition and no `break` on every path — a literal-`true` condition omits the loop's cond→exit edge at CFG-build time, which is what makes the component "closed." An ordinary `while(cond)` with a non-literal condition always keeps its exit edge and is never flagged this way.

**Diagnostic:** `InfiniteLoopWarning` (non-fatal) — *"Function 'X' contains a loop with no exit path. This will run forever."*

```turflang
fn spin(): int {
    mut x: int = 0;
    while true {
        x = x + 1;
    }
    return x;   // unreachable, but the warning fires on the loop itself
}
```

> **Note:** there is a second, unrelated warning with near-identical intent and wording — `SuspiciousInfiniteLoopWarning`, produced by a separate, simpler AST pattern-match pass (not CFG-based) that also looks for `while true` with no visible exit. The two overlap in what they catch but are genuinely different implementations and could in principle diverge on edge cases — don't treat them as the same diagnostic.

### Infinite-recursion detection

**Triggers when:** a depth-first search from entry to exit finds that *every* path through the CFG passes through at least one self-call — i.e. there is no path that reaches the function's exit without recursing. A cycle encountered mid-search is conservatively treated as "this path recurses," since a cycle can't prove a base case exists. Self-call identity is resolved by mangled name where available, specifically so a call to a sibling *overload* of the same base name isn't mistaken for self-recursion.

**Diagnostic:** `RecursionWithoutBaseError` (**E010**, `Important`, fatal) — *"Infinite recursion detected in 'X'."*

```turflang
fn foo(): int {
    return foo();   // every path self-calls, no base case
}
```

### Use-after-free / double-free

**Triggers when:** a forward, per-block dataflow pass tracks which variables point at which heap allocations (from `malloc`/`realloc`) and which allocations have been `free`d, and finds either a second `free()` of an already-freed allocation, or an array access (`x[i]`) through a variable pointing at an already-freed allocation.

**Diagnostics** (both `Important`, fatal):
- `DoubleFreeError` (**E017**) — *"Double free detected for 'X'."*, with a related-location note at the first free.
- `UseAfterFreeError` (**E018**) — *"Oops! You're using memory from 'X' after it was freed."*, with a related-location note at the free site.

```turflang
fn bad(): int {
    mut arr: int* = malloc(4);
    free(arr);
    free(arr);        // [E017] double free
    return arr[0];     // would also be [E018] use-after-free, on its own
}
```

Detection is limited to array-access use sites through raw-pointer-typed variables and plain-variable aliasing (`p = q`) — it's a pattern-based, points-to-lite analysis rather than a full alias analysis. Variables are tracked by symbol identity, not by name, so a shadowed variable in an inner scope is never confused with an outer one sharing the same name.

### Constructor field-assignment

**Triggers when:** in a struct's `.new` constructor specifically, some declared field of the struct isn't assigned via `self.<field> = ...` on every path from entry to the constructor's exit.

**Diagnostic:** `IncompleteConstructorError` (fatal) — *"'StructName.new' doesn't assign every field on all paths."*, naming exactly which fields weren't definitely assigned.

```turflang
struct Point {
    x: int;
    y: int;

    fn new(x: int): Point {
        mut p: Point = Point{};
        p.x = x;
        return p;   // ERROR: field `y` is never assigned
    }
}
```

### Compile-time array-bounds checking

A constant-propagation pass tracks known-constant integer values through the CFG. Whenever an array index expression is a statically-known constant and the target array has a fixed compile-time size, an out-of-bounds constant index is caught **at compile time**, not just at runtime.

**Diagnostic:** `ArrayBoundsError` (**E001**), the same error a runtime bounds violation would eventually surface, but raised during compilation:

```turflang
fn main(): int {
    immut primes: int[3] = [2, 3, 5];
    mut i: int = 5;
    return primes[i];   // `i` is a known constant 5, primes has size 3 — E001 at compile time
}
```

If the index isn't statically known (e.g. it depends on a function parameter or runtime input), this pass has nothing to say and the usual runtime check applies instead.

---

## A note on what doesn't run

`CallGraphPass::runGlobal()` is wired into the compilation pipeline (invoked once per compile, after codegen, before diagnostics are flushed) but is currently a **confirmed no-op**: it takes a lock and returns, performing no analysis and printing no summary. A separate mechanism, `AddCallEdge()`, *is* actively called during codegen and does populate an internal caller→callee map — but nothing in the compiler ever reads that map back out. In short: per-function call edges are collected, but no whole-program call-graph analysis is actually performed today, despite the pass existing in the pipeline.
