# CLI, Diagnostics & Static Analysis

Reference for the `turf` command line, its error/warning reporting system, and the compile-time control-flow (CFG) analyses that run on every build. Projects, `turf.toml` and dependencies have their own page: [Projects & Packages](projects_and_packages.md).

---

## Part 1 — The command line

### `turf` and `turfc`

There are two programs, released separately:

- **`turf`** is the command you type. It is the front end: it compiles single files, and in a project it reads `turf.toml`, fetches dependencies and drives the compiler.
- **`turfc`** is the compiler. It is the interface tools use (an editor plugin, a build system, the compiler's own test scripts), and section "For tool authors" at the end of this part documents it. You are never asked to type it.

`turf` finds the compiler by itself and checks that the two agree on the *driver interface* before it runs anything (see "Versions" below). Everything on this page that says `turf` works the same on a single file with no manifest.

```
$ turf help
turf - the TurfLang toolchain front end

Usage:
  turf <command> [args]
  turf <file.tr> [flags]      same as: turfc <file.tr> [flags]  (flags forwarded unchanged)

Any file:
  compile   compile a file to an executable
  run       compile to a temporary executable and run it
  check     report every diagnostic, write no file
  fmt       format source files
  explain   explain an error code
  migrate   migrate source across a language change
  dump      print an intermediate form (ast, resolved, ...)

Project:
  new       create a project
  init      make the current directory a project
  build     build the project
  test      run the project's tests
  clean     remove build output
  fetch     download dependencies
  add       add a dependency
  remove    remove a dependency
  update    update git dependencies

Other:
  publish   reserved: there is no registry in 2.0
  version   print the turf and turfc versions
  help      show help: turf help <command>

Run 'turf help <command>' for a command's options.
```

`turf help <command>` prints each command's options. The project commands (`new`, `init`, `build`, `run` without a file, `check` without a file, `clean`, `fetch`, `add`, `remove`, `update`) are documented in [Projects & Packages](projects_and_packages.md). `turf test` runs the project's `tests/` (an error if there is none; see [Projects & Packages](projects_and_packages.md)); the same runner is `turfc --test [<dir>]`. `turf publish` is reserved because there is no registry in 2.0.

### Compiling a file

```
turf hello.tr -o hello
./hello
```

`turf <file.tr> [flags]` and `turf compile <file.tr> [flags]` are the same thing: when the first argument is not a command name, `turf` runs `turfc` with **the same arguments**, and the compiler's output and exit status come back unchanged. `compile` is the spelled-out form. The input file is a required positional argument and its name **must end in `.tr`**; anything else is a hard error. If `-o` is omitted, the output name defaults to the input filename's basename with `.tr` stripped (falling back to `program` if that would be empty).

```
$ turf hello.tr -o hello_a
Compiled to hello_a.o
Successfully compiled to hello_a
$ turf compile notes.txt
Error: Input file must have a '.tr' extension (got: notes.txt).
$ turf compile hello.tr --chek
Unknown option: --chek
  did you mean '--check'?
Run `turfc help` for the options.
```

Flags that `turf compile` does not know about are forwarded to the compiler unchanged, so a new compiler flag works through `turf` the day it lands. Any other token starting with `-` that the *compiler* does not recognize is rejected with `Unknown option: <arg>` and exit code 2, with the nearest known flag suggested when one is close. `turfc help` lists every flag; `turfc --help` is deliberately not one, and says so.

A bare word that neither starts with `-` nor ends in `.tr` is not passed to the compiler as a missing file; it is an unknown command, with a spelling suggestion:

```
$ turf notes.txt
turf: unknown command 'notes.txt'
run `turf help` for the list of commands
$ turf biuld
turf: unknown command 'biuld'
  did you mean 'build'?
run `turf help` for the list of commands
```

### Flags

| Flag | Implies | What it does |
|---|---|---|
| `-O0` `-O1` `-O2` `-O3` `-Os` | — | Optimization level; `-O2` is the default. Any other spelling is a hard error (`Error: unknown optimization level '-O4' (expected one of -O0, -O1, -O2, -O3, -Os).`), never a silent fallback. Levels change speed and size only, never what a program does. |
| `-g` | — | Emits DWARF debug info. |
| `--target=<triple>` | — | Cross-compiles for that target triple (default: the host). |
| `-mcpu=<cpu>` | — | Target CPU (default: `generic`). |
| `--check` | — | Reports every diagnostic and writes no file. Also `turf check`. |
| `--module` | `--check` only | With `--check`: the file is a module that is only ever imported, so it needs no `main`. Without `--check` it is an error (`Error: --module only applies with --check (a module has no entry point, so it cannot be linked).`). |
| `--dump-instantiations` | — | Prints the generic-instantiation census (`-- monomorphization instantiation census (N distinct requests) --`). |
| `--stdlib-warnings` | — | Shows warnings anchored inside the standard library, which are hidden by default. |
| `--allow-unsafe-modules` | — | Honors `unsafe module;` outside the install root, in a single-file compile. It does not apply in a project, where the manifest's `[unsafe] allow` decides ([Projects & Packages](projects_and_packages.md)). |
| `--emit-llvm` | — | Writes human-readable LLVM IR to `<output>.ll` instead of compiling and linking a native binary. |
| `--smart` | — | Enables the SLM (AI-assisted) fix-suggestion pipeline. See below — deliberately not documented in depth here. `--smart-write-fix=<path>` writes the first validated fix there. |
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

### Other single-file commands

These need no manifest and work on any `.tr` file. Each one runs the compiler in a particular mode and adds nothing of its own to what the compiler says.

**`turf check <file.tr>`** reports every diagnostic and writes no file, with the same words and the same exit status (0 clean, 1 on any error) as a compile. Silence and exit 0 mean the file is clean. The file is checked as a *program*, so a missing `main` is an error, exactly as it is for a compile; `--module` says the file is only ever imported:

```
$ turf check mod.tr
Error: No entry point found. Please define 'fn main(): int' in your file.
  (checking a module that is only ever imported? add --module: `turf check --module <file.tr>`)
$ turf check --module mod.tr
$ turf check hello.tr
```

**`turf run <file.tr> [flags] [-- args…]`** compiles to a temporary executable, runs it with the arguments after `--`, and deletes it. The exit status is the program's (`turf run three.tr`, whose `main` returns 3, exits 3), or 128+N if signal N killed it; a failed compile exits with the compiler's status. `-o` is refused, because the executable is deleted afterwards.

**`turf dump <what> <file.tr>`** prints an intermediate form without producing an executable, and a file with no `main` (a module) dumps the same as a program. `<what>` is `ast`, `resolved`, `typechecked`, `modules`, `symbols` or `instantiations`, which are `--dump-ast`, `--dump-resolved`, `--dump-typechecked`, `--dump-modules`, `--dump-resolver-symbols` and `--dump-instantiations`.

**`turf fmt <file.tr>`** prints the formatted file to stdout, and `turf fmt --write <path>...` rewrites in place the files that differ (a file that is already formatted is not touched). `turf fmt --check <path>...` changes nothing: it names each file that would change (`would reformat src/main.tr`) and exits 1 if there is any, which is what CI wants. A path is a file or a directory (every `.tr` file under it, not hidden directories and not a `build/`); `--exclude <path>` skips a file or a directory. With no path, inside a project, `--write` and `--check` apply to the project's `src/` and `tests/`; one of the two is required then, because the output would otherwise be many files at once. Without `--write` or `--check` exactly one file is accepted, because the output goes to stdout. A path that does not exist is an error, never skipped. Every file is attempted, and the exit status is 1 if any failed.

The formatter re-derives **indentation** from the bracket structure, strips trailing whitespace, collapses runs of blank lines and ends the file with one newline. It never changes anything else on a line, and it does not normalize spacing inside a line: `fn   main( ):int{return   0;}` is left exactly as written. These are checked, not assumed: before a result is written or reported, `turfc` verifies that no non-whitespace byte changed, that the lexer reads the same tokens from both texts, and that formatting the result again changes nothing. A file that fails a check is left alone and reported as a formatter bug. A file with a lexical error (an unterminated string, a malformed number) cannot be formatted and is reported with the lexer's reason; the other files are still done.

```
$ turf fmt messy.tr
fn main(): int {
    if true {
        return 1;
    }
    return 0;

}
$ turf fmt a.tr b.tr
turf: fmt: without --write or --check the formatted text goes to stdout, so exactly one file is accepted; pass --write to reformat files in place, or --check to only report
$ turf fmt --check src
would reformat src/main.tr
Would reformat 1 file; 3 already formatted.
```

**`turf migrate pub [--write] <files…>`** adds `pub` to every top-level declaration and struct field, which keeps the pre-2.0 behaviour where everything a module declared was importable (see [Functions & Modules](functions_and_modules.md)). Without `--write` it only reports what it would change:

```
$ turf migrate pub oldmod.tr
would migrate oldmod.tr (2 `pub`)
  oldmod.tr:1:
    - fn f(): int {
    + pub fn f(): int {
  oldmod.tr:4:
    - fn main(): int {
    + pub fn main(): int {
Would migrate 2 declarations/fields in 1 file. Re-run with --write to apply.
$ turf migrate pub --write oldmod.tr
migrated oldmod.tr (2 `pub`)
Migrated 2 declarations/fields in 1 file.
```

### `turf explain E0NN`

```
turf explain E001
```

`turf explain E001` and `turf --explain E001` are the same command (the second is the compiler's own spelling, and it is what error footers print). It takes a code and no `.tr` input file at all.

- No code → `turf: explain: expected exactly one error code, for example: turf explain E001`, exit 2 (through `turfc --explain` with no code it is `Usage: turfc --explain E0NN`, also exit 2).
- Unknown code → `Unknown error code: E999`, then a line saying what a code looks like, exit 2.
- On success: prints a bold-cyan `E0NN  ClassName (CATEGORY)` header, then the explanation text, then exits 0.

The explanation text is read from the **installation** (the directory holding `lib/std`: `$TURF_HOME`, or the directory above the compiler binary), not from the current directory, so it is the same wherever you run the command. It uses a three-tier fallback, most specific first:
1. a per-code doc file (`docs/errors/codes/E0NN.txt`);
2. a construct-specific doc, routed by keyword classification of the error's short description;
3. a plain per-category doc, as the coarsest fallback.

Only if the installation is missing its docs is the code's short auto-generated description printed instead. Release archives ship `docs/errors/` and `docs/slm/` for this reason.

```
$ cd /tmp && turf explain E091
E091  ModuleNotImportableError (DECLARATION)

A module exists in this build, but this package may not import it
```

(Before 2026-09-30 the lookup was relative to the current directory, so only a run from a checkout's root got the full text.)

Rendering: the first non-blank line is a bold cyan title; any line indented four or more spaces is rendered as a green "code sample" line; everything else is body prose, with `` `backtick` `` spans highlighted in bold bright-green (backticks stripped).

```
$ turf --explain E001
E001  ArrayBoundsError (ARRAY_ERROR)

You tried to reach past the end of an array
...
```

### `--smart`

`--smart` enables an AI-assisted fix-suggestion pipeline: when the first *error* in a compile pass fires, the compiler asks a small language model for a suggested fix, sandbox-validates it, and — if the model is confident — replaces the plain error message with an `[AI Suggestion]` block (explanation, optional code patch, optional warning) instead of the default diagnostic. Subsequent errors in the same pass just note that they may be a cascading effect of the first one, rather than triggering another model call. Warnings never invoke this pipeline. The prompt design, sandbox mechanics, and model internals are out of scope for this reference.

### Exit status

`turf` and `turfc` use the same three statuses (frozen in 2.0, `FREEZE.md` §6.21):

| Status | Meaning |
|---|---|
| **0** | It did what was asked. |
| **1** | It ran and reported a problem: a compile error in your program, a file it could not read or write, a bad manifest, a failed fetch. |
| **2** | The command line is wrong, so nothing was attempted: an unknown command or option, a missing value or input file, a bad `-O` level, a project command outside a project. |

`turf compile`, `turf check` and `turf <file.tr>` return the compiler's status unchanged. `turf run` is the exception: once your program has started it returns **the program's own** status, so a program that returns 2 looks like a usage error there. Anything above 2 (126, 127, 128 plus a signal) comes from the shell or the operating system.

### Versions

`turf` and `turfc` have separate version numbers, and `turf version` (or `turf --version`) shows both:

```
$ turf version
turf 1.0.0-dev  (needs turfc interface 1.1+)
turfc 2.0.0-dev  (interface 1.2, …/build/turfc)
  llvm 22.1.8
  runtime turf_rt_abi_2_0
  root …
```

The indented lines are the compiler's own: the LLVM it was built against, the runtime ABI stamp every program it compiles carries (a `libturfrt.a` without it fails at link time rather than misbehaving), and the install root it reads `lib/std` and the runtime from. Paste this output into a bug report. `turfc --version` prints the same facts without the `turf` line.

The two need not match. What must fit is the **driver interface**, a `major.minor` pair that `turfc` reports and `turf` needs: the same major and at least the minor `turf` names. Before it runs anything, `turf` checks this and refuses a compiler that does not fit, naming both sides and which one to upgrade (`TURF_COMPILER` does not bypass the check):

```
turf: this compiler does not speak the interface this turf needs
  turf  1.0.0-dev  needs interface 1.1 or a later 1.x  (…/build/turf)
  turfc 9.0.0  speaks interface 2.0  (…/fake/turfc)
hint: upgrade turf: this compiler's interface is newer than this turf understands
```

A differing *release* version is never an error on its own. In a project, a manifest's `turf = "2.0"` is about the compiler's release version and is checked separately ([Projects & Packages](projects_and_packages.md)).

### Finding the compiler

`turf` looks for `turfc` in this order and takes the first match:

1. `$TURF_COMPILER`, a path to the binary (this is also what the compiler's own test scripts set).
2. `turfc` in the directory of the running `turf`, with symlinks resolved (an installed pair, or `build/turf` beside `build/turfc` in a checkout).
3. `$TURF_HOME/bin/turfc`.
4. `turfc` on `PATH`.

Nothing found is an error that lists what was looked at:

```
turf: cannot find the compiler (turfc)
  looked in:
    $TURF_COMPILER (not set)
    …/alone/turfc
    $TURF_HOME/bin/turfc (TURF_HOME not set)
    turfc on PATH
hint: set TURF_COMPILER to the turfc binary, or build it with `cmake --build build`
```

A `TURF_COMPILER` naming a file that does not exist is its own error (`turf: TURF_COMPILER is set to "…/nonexistent", which is not an existing file`). Do not export `TURF_HOME` in a development checkout: both binaries find their install root (`lib/std`, the runtime library) from their own location, and a `TURF_HOME` pointing at one checkout silently makes every other build use that checkout's library.

### For tool authors: `turfc`

Everything above is `turf`. This section is for whoever drives the compiler directly: an editor plugin, a build system, a script. (The language server uses the in-process facade `include/turf/Analysis.h`, not the command line.) `turfc` is a complete compiler on its own. It never touches the network, never reads `turf.toml` or `turf.lock`, and never searches for them. The one exception is a hint: when an `import` fails in single-file mode and a `turf.toml` exists beside the file or above it, the error names `turf build`, and the manifest is only checked for existence, never opened.

**The driver interface.** `turfc --version` prints two lines. Line 1 is the release version, line 2 is the interface `turf` checks:

```
$ turfc --version
turfc 2.0.0-dev
interface 1.1
```

The interface covers exactly what `turf` relies on: the flags it passes (`--check`, `--module`, `--dump-*`, `--format`, `--write`, `--migrate=`, `--explain`, `-o`, and `--package-graph`), the two-line `--version` format, the exit statuses (0 success, 1 a reported error), and every other argument being accepted exactly as a direct invocation accepts it. An additive change (a new flag or input) bumps the **minor**; removing or changing the meaning of anything covered bumps the **major**. So a tool that needs `--package-graph` requires interface 1.1 or later.

**Modes a tool uses:**

- `turfc --check <file.tr>` runs every diagnostic and writes nothing; the output and status match a normal compile of the same file. It checks a **program**, so it wants a `main`; `turfc --check --module <file.tr>` checks a file that is only ever imported (`--module` alone is an error). This is what `turf check`, `turf dump` and a library's `turf build` run.
- `turfc --package-graph <file.json> <file.tr>` compiles as a package build, and is the only way the compiler learns about packages. The graph lists each package's source root, its **direct** dependencies and whether its `unsafe module;` declarations are honored; its exact format is documented in `include/PackageGraph.h`. `turf` writes it for you as `build/package-graph.json`:

  ```json
  {
    "format": 1,
    "root": "plotter",
    "packages": {
      "geometry": {
        "src": "…/ex/geometry/src",
        "deps": [],
        "allow_unsafe": false
      },
      "plotter": {
        "src": "…/ex/plotter/src",
        "deps": [
          "geometry"
        ],
        "allow_unsafe": true
      }
    }
  }
  ```

  Given that file, `turfc --package-graph build/package-graph.json src/main.tr -o direct` builds the program by itself, and a bad graph is one line naming the file, exit 1 (`Error: package graph bad1.json: "format" must be 1 (this turfc reads package-graph format 1)`). Without the flag `turfc` is in single-file mode. In a package build the file you name must be inside the root package's `src` (otherwise: `Error: package graph …: the file being compiled (…) is not inside the root package's src …`); which file is the program is up to the caller, and `turf` always passes `src/main.tr`.
- `turfc --format [--write | --check] <file.tr>...`, `turfc --migrate=pub [--write] <files…>` and `turfc --explain E0NN` are what `turf fmt`, `turf migrate` and `turf explain` run.

**`turf` never rewrites a compiler diagnostic.** A tool that runs `turf check` or `turfc --check` sees the same text and the same `[E0NN]` codes either way, and can match on them. The footer of a coded diagnostic still spells the command as ``Run `turf --explain E0NN` ``, which works through the passthrough.

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

Every registered error class has a stable `E0NN` code, assigned once and never reassigned. As of this writing there are **91 codes, E001–E091** (`turf explain E091` works and `turf explain E092` is `Unknown error code: E092`).

> **Note:** the comment at the top of `include/ErrorCodes.h` still says "currently 67" registered call sites. It is stale; the table has 91 rows.

Assignment order is alphabetical by (source file, class name) at the time each code was first added, and later codes are appended — it's an opaque identifier, not a severity or category ranking.

A small curated subset of codes is flagged `Important` — genuinely non-obvious concepts where the one-line message alone probably isn't enough:

- **E010** `RecursionWithoutBaseError`
- **E017** `DoubleFreeError`
- **E018** `UseAfterFreeError`
- **E035** `InvalidTransitionError`
- **E036** `InvalidTransitionTargetError`
- **E069** `StructLayoutCycleError`
- **E086** `DanglingReferenceEscapeError`
- **E088** `PrivateSymbolAccessError`
- **E089** `PrivateInPublicError`

These get a stronger, differently-colored footer nudge (see below) urging you to read the full `turf explain` entry before patching the code.

**Modules and packages.** Four codes concern how an `import` resolves, and the fix differs for each:

- **E027** `ModuleNotFoundError`: nothing answers to that module name. Inside a project, a single-file compile adds a hint that a dependency is only visible to `turf build`.
- **E090** `AmbiguousModuleError`: two or more different files answer to one name (your own module and the standard library, your own module and a dependency, or two of your own). The message lists every candidate with its kind and path, and the hint says which to rename.
- **E091** `ModuleNotImportableError`: the module exists in this build but this package may not import it, because it belongs to a *transitive* dependency or is another package's *internal* module.
- **E088** `PrivateSymbolAccessError` (and **E089** `PrivateInPublicError`): the module is reachable but the item is not `pub`.

Real examples of each, and the rules behind them, are in [Projects & Packages](projects_and_packages.md#4-how-imports-resolve); E090 and E091 have `turf explain` pages.

### How a diagnostic is printed

```
$ turf check oob.tr
oob.tr (1 error)

── error at line 3, col 27 [E001]

  Array index out of bounds!
  You tried to access 'primes[3]', but 'primes' only has 3 elements (indices 0 to 2, or -1 to -3 counting from the end).
  Note: array declared here (line 2, col 11)

  Hint: Array indices start at 0 and go up to size - 1; a negative index counts back from the end (-1 is the last element).

  3 │     mut last: int = primes[3];
    │                           ^ Here!

  Run `turf --explain E001` for more details.
```

Top to bottom:

1. **File header** — the path of the file the diagnostics below belong to, then a count: `src/main.tr (2 errors, 1 warning)`. Diagnostics are batched **per file**: the file you compiled comes first, then each imported module in path order, and within a file they are sorted by line and column. The path is relative to the directory you ran the compiler from (an absolute path would differ per machine); a standard-library file is shown from `lib/std/` on, and a file outside the working directory keeps the spelling it was given with. A file whose only diagnostics were suppressed standard-library warnings gets no header.
2. **Diagnostic header** — `── error at line X, col Y [E0NN]` (bright red for errors, bright yellow for warnings). The `[E0NN]` tag only appears if the diagnostic resolved to a real code. There is no per-diagnostic file name: the file header above it already names the file, whether it is your own or an imported module.
3. **Message body**.
4. Zero or more blue `Note: ...` lines, one per related location.
5. An optional green `Hint: ...` line.
6. A **source snippet** with a `│` gutter, showing the exact source line with an underline beneath the offending span, suffixed `" Here!"`.
7. A footer (only when the diagnostic has a resolved code):
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
$ turf check typo.tr
── error at line 3, col 12 [E040]

  I can't find anything named 'contuner'.

  Hint: It may be from the current file: 'container'

  3 │     return contuner;
    │            ^~~~~~~~ Here!

  Run `turf --explain E040` for more details.
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

**Diagnostic:** `StatementAfterTerminatorError` (**E012**, fatal) — *"This code comes after a return/break/continue, so it's impossible to reach it!"*

Triggered as soon as the CFG builder visits a statement in a block that already has a terminator. This covers every kind of statement, variable declarations included:

```turflang
fn main(): int {
    return 0;
    print_thing();   // [E012], reported at the `return 0;` line
    mut x: int = 5;  // [E012] as well
}
```

> **Note:** the error is anchored at the *terminator's* location (the `return` line), not at the unreachable statement's own line — confirmed by compiling the example above.

> **History:** a declaration in this position used to be reported as a separate code, **E013**, from a Codegen backstop. That was the same mistake under a second number, and the CFG pass always got there first, so E013 is retired (the row stays in the registry; the Codegen backstop now raises E012 itself).

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
