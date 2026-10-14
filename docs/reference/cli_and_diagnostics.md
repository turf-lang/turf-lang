# CLI, Diagnostics & Static Analysis

Reference for the `turf` command line, the diagnostics the compiler prints, the analyses it runs on every build, and what a compiled program does when it fails at run time.

- [Part 1: The command line](#part-1--the-command-line)
- [Part 2: Diagnostics and error codes](#part-2--diagnostics-and-error-codes)
- [Part 3: Compile-time static analysis](#part-3--compile-time-static-analysis)
- [Part 4: When a compiled program fails](#part-4--when-a-compiled-program-fails)

Projects, `turf.toml` and dependencies have their own page: [Projects & Packages](projects_and_packages.md).

Diagnostics in this page are shown without colour. The compiler colours them (errors red, warnings yellow, hints green, notes blue).

---

## Part 1 — The command line

### `turf` and `turfc`

There are two programs, released separately:

- **`turf`** is the command you type. It compiles single files, and in a project it reads `turf.toml`, fetches dependencies and drives the compiler.
- **`turfc`** is the compiler. It is the interface other tools use (an editor plugin, a build system, a script), and ["For tool authors"](#for-tool-authors-turfc) at the end of this part documents it. You are never asked to type it.

`turf` finds the compiler by itself and checks that the two agree on the *driver interface* before it runs anything (see [Versions](#versions)). Everything on this page that says `turf` works the same on a single file with no manifest.

```text
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
  graph     print the package graph as JSON
  fetch     download dependencies
  add       add a dependency
  remove    remove a dependency
  update    update git dependencies

Other:
  publish   reserved: there is no registry in 2.0
  version   print the turf and turfc versions
  help      show help: turf help <command>

Run 'turf help <command>' for a command's options.

Exit status: 0 success, 1 an error was reported (a compile error, a bad
manifest, a failed fetch), 2 the command line is wrong. `turf run` exits with
the program's own status once the program has started.
```

Every command has its own help: `turf help build`, `turf help fmt`, `turf help test` and so on print its options. `turf --version` is `turf version`, and `turf --explain E001` is `turf explain E001`. `turfc` has no `--help`; it says so and points at `turfc help`.

The project commands (`new`, `init`, `build`, `run` and `check` without a file, `test`, `clean`, `graph`, `fetch`, `add`, `remove`, `update`) are documented in [Projects & Packages](projects_and_packages.md). The rest of this part is about the commands that need no manifest and work on any `.tr` file.

### Compiling a file

```text
$ turf hello.tr -o hello
Compiled to hello.o
Successfully compiled to hello
$ ./hello
hi
```

`turf <file.tr> [flags]` and `turf compile <file.tr> [flags]` are the same thing: when the first argument is not a command name, `turf` runs the compiler with **the same arguments**, and the compiler's output and exit status come back unchanged. The input file is a required argument and its name **must end in `.tr`**. If `-o` is omitted, the output name is the input's file name without `.tr` (`program` if that would be empty). The compile writes `<output>.o` and links it into `<output>`.

```text
$ turf compile notes.txt
Error: Input file must have a '.tr' extension (got: notes.txt).
$ turf compile hello.tr --chek
Unknown option: --chek
  did you mean '--check'?
Run `turfc help` for the options.
```

A flag that `turf compile` does not know is passed to the compiler unchanged, so a flag a newer compiler adds works through an older `turf`. The compiler rejects an option it does not recognise with `Unknown option: <arg>` and exit status 2, suggesting the nearest known flag when one is close.

A bare word that neither starts with `-` nor ends in `.tr` is an unknown *command*, with a spelling suggestion:

```text
$ turf biuld
turf: unknown command 'biuld'
  did you mean 'build'?
run `turf help` for the list of commands
```

### Flags

| Flag | What it does |
|---|---|
| `-O0` `-O1` `-O2` `-O3` `-Os` | Optimization level; `-O2` is the default. Any other spelling is an error. See [Optimization levels](#optimization-levels). |
| `-g` | Emits DWARF debug information for `gdb` and `lldb`. Off by default. See [Debugging with `-g`](#debugging-with--g). |
| `--target=<triple>` | Compiles for that target triple (default: this machine). For another machine it writes the object file and stops with an error. See [Other targets](#other-targets-and-object-files). |
| `-mcpu=<cpu>` | Target CPU (default: `generic`). A name the target does not know is an error, exit status 2. |
| `--emit-obj` | Writes the object file `<output>.o` and does not link. |
| `--emit-llvm` | Writes human-readable LLVM IR to `<output>.ll` instead of compiling and linking. Not combinable with `--emit-obj`. |
| `-o <name>` | Sets the output name. |
| `--link <lib>` | Adds `-l<lib>` to the final link, on top of any library a program's own `extern from "libname" fn` declarations already name. Repeatable. |
| `--check` | Reports every diagnostic and writes no file. Also `turf check`. |
| `--module` | With `--check`: the file is a module that is only ever imported, so it needs no `main`. Without `--check` it is an error. |
| `--smart` | Asks a local language model to suggest fixes for errors. See [`--smart`](#--smart). `--smart-write-fix=<path>` writes the first validated fix to that file. |
| `--stdlib-warnings` | Shows warnings that point into the standard library, which are hidden by default. See [Warnings in the standard library](#warnings-in-the-standard-library). |
| `--allow-unsafe-modules` | Honors `unsafe module;` in a module outside the install root, in a single-file compile. It does not apply in a project, where the manifest's `[unsafe] allow` decides ([Projects & Packages](projects_and_packages.md#8-unsafe-module-in-dependencies)). |
| `--dump-ast`, `--resolve`, `--typecheck`, `--dump-resolved`, `--dump-typechecked`, `--dump-resolver-symbols`, `--dump-modules`, `--dump-instantiations` | Print what the compiler knows at each stage. `turf dump` is the friendly spelling of these. |

The name resolver and the type checker run on every compile whatever flags you pass. `--resolve`, `--typecheck` and the `--dump-*` flags only add output; if either pass finds an error, no code is generated.

### Optimization levels

`-O0` does the least optimization, so it compiles fastest and is the easiest to debug. `-O1`, `-O2` (the default), `-O3` and `-Os` turn on progressively more of LLVM's optimizer; `-Os` favours small code over speed. An unrecognised level is an error, never a silent fallback to the default:

```text
$ turf hello.tr -O4
Error: unknown optimization level '-O4' (expected one of -O0, -O1, -O2, -O3, -Os).
$ echo $?
2
```

**Optimization never changes what your program does.** Its output and exit status are identical at every level; only compile time, run time, binary size and debuggability change. This holds for the runtime checks too: bounds checks, division by zero and integer overflow traps stay in the program at every level. A check is removed only when the compiler can *prove* it never fires, never because of a flag.

```text
$ for o in O0 O1 O2 O3 Os; do turf prog.tr -$o -o prog_$o >/dev/null; ./prog_$o; echo "exit $?"; done
```

prints the same output and the same exit status five times. The size difference is real: on one x86-64 Linux build, a hello-world executable is about 78 KB at `-O0` and about 34 KB at `-O1` and above.

The proof works at every level, `-O0` included. A check the compiler can see will never fire, such as an in-range index into an array with a constant index, or checked arithmetic on constants that cannot overflow, is not emitted at all. Compiling this at `-O0` with `--emit-llvm` leaves no bounds check in `main`; the same indexing through a function parameter keeps its check:

```turflang
fn main(): int {
    immut a: int[3] = [1, 2, 3];
    return a[1];
}
```

### Debugging with `-g`

`-g` adds DWARF debug information (version 4) to the executable. A debugger can then set a breakpoint on a TurfLang source line, show a backtrace with TurfLang function names, and print a parameter or local by name. It is off by default, it never changes what the program does, and it works with every `-O` level. Pair it with `-O0` for debugging: at higher levels the optimizer moves, merges and removes code, so stepping jumps around and some variables are gone.

```turflang
import io;

fn factorial(n: int): int {
    mut result: int = 1;
    mut i: int = 2;
    while i <= n {
        result = result * i;
        i = i + 1;
    }
    return result;
}

fn main(): int {
    immut value: int = factorial(5);
    io::printline("5! = " + (value to string));
    return 0;
}
```

Saved as `fact.tr`:

```text
$ turf fact.tr -O0 -g -o fact
$ gdb -q ./fact
(gdb) break fact.tr:7
Breakpoint 1 at 0x8fcb: file fact.tr, line 7.
(gdb) run
Breakpoint 1, factorial (n=5) at fact.tr:7
7	        result = result * i;
(gdb) print i
$1 = 2
(gdb) print result
$2 = 1
(gdb) continue 3
Will ignore next 2 crossings of breakpoint 1.  Continuing.
Breakpoint 1, factorial (n=5) at fact.tr:7
7	        result = result * i;
(gdb) print result
$3 = 24
(gdb) backtrace
#0  factorial (n=5) at fact.tr:7
#1  0x000055555555d07f in main () at fact.tr:14
#2  0x000055555555d455 in main ()
```

The last frame is the program's entry point, which wraps `main`. A breakpoint on a function name (`break factorial`) stops at the function's first line, before its parameters are stored, so `n` can read as 0 there; break on the first body line instead.

A type declared in an imported module appears in the debugger as `module.Name`, and a type declared in your own file keeps its bare name:

```text
(gdb) whatis l
type = struct list.List_gen1_int
(gdb) whatis it
type = struct Item
```

`--emit-llvm` shows the same names. The generic `List<int>` is `list.List_gen1_int`, the instance of `List` for `int`.

### Other targets and object files

`--target=<triple>` compiles for another machine, `-mcpu=<cpu>` selects a CPU of that target, and `--emit-obj` stops after writing the object file. Compiling for a machine other than this one produces an object but cannot produce a program: **cross-linking is not supported in 2.0.** The compiler keeps the object, says so, and exits 1:

```text
$ turf hello.tr --target=aarch64-linux-gnu -o hello
Compiled to hello.o
Error: cannot link a program for 'aarch64-linux-gnu': cross-linking is not supported in 2.0; the runtime (libturfrt) is built for x86_64-pc-linux-gnu.
  The object file for 'aarch64-linux-gnu' was kept as hello.o. Link it with a toolchain and a runtime built for that target, or compile for this machine by leaving --target out.
```

To get the object for another machine without the error, ask for the object explicitly. This exits 0:

```text
$ turf hello.tr --target=aarch64-linux-gnu --emit-obj -o hello
Compiled to hello.o
$ file hello.o
hello.o: ELF 64-bit LSB relocatable, ARM aarch64, version 1 (SYSV), not stripped
```

The vendor part of a triple is ignored when deciding whether a target is this machine, so `--target=x86_64-linux-gnu` on an `x86_64-pc-linux-gnu` host links and runs normally. A different C library counts as a different machine (`x86_64-linux-musl` on a glibc host). A triple LLVM does not know is an error (`No available targets are compatible with triple "foo"`, exit 1).

`--emit-obj` also works for the host. It writes `<output>.o` and nothing else, so you can link it yourself. A program has to be linked against the matching runtime library; one built for a different compiler release fails at link time (see [the runtime ABI stamp](#the-runtime-abi-stamp)).

### Other single-file commands

These need no manifest and work on any `.tr` file. Each runs the compiler in one mode and adds nothing of its own to what the compiler says.

**`turf check <file.tr>`** reports every diagnostic and writes no file, with the same words and the same exit status as a compile: 0 when there is no error (warnings do not count), 1 on any error. The file is checked as a *program*, so a missing `main` is an error; `--module` says the file is only ever imported:

```text
$ turf check mod.tr
mod.tr (1 error)

── error [E119 · declaration]

  No entry point found. Please define 'fn main(): int' in your file.
  Hint: checking a module that is only ever imported? add --module: `turf check --module <file.tr>`

  Run `turf --explain E119` for more details.
$ turf check --module mod.tr
$ turf check hello.tr
```

**`turf run <file.tr> [flags] [-- args…]`** compiles to a temporary executable, runs it, and deletes it. The arguments after `--` are the program's; it reads them with `os::argc()` and `os::argv(i)` from the standard library's `os` module. The exit status is the program's own, or 128+N if signal N killed it (a program that calls `sys::abort()` gives 134). A failed compile exits with the compiler's status. `-o` is refused, because the executable is deleted afterwards.

**`turf dump <what> <file.tr>`** prints an intermediate form without producing an executable; a file with no `main` dumps the same as a program. `<what>` is `ast`, `resolved`, `typechecked`, `modules`, `symbols` or `instantiations`.

**`turf fmt`** formats source files:

```text
turf fmt <file.tr>                print the formatted file to stdout
turf fmt --write [<path>...]      rewrite, in place, the files that differ
turf fmt --check [<path>...]      change nothing; name each file that would
                                  change, and exit 1 if there is any
```

A path is a `.tr` file or a directory (every `.tr` file under it, skipping hidden directories and `build/`); `--exclude <path>` skips a file or a directory. With no path, inside a project, `--write` and `--check` apply to the project's `src/` and `tests/`; one of the two is required then. Without `--write` or `--check`, exactly one file is accepted, because the output goes to stdout. A path that does not exist is an error. Every file is attempted, and the exit status is 1 if any failed.

The formatter fixes **indentation** (each line's depth follows its brackets), trailing whitespace, runs of blank lines and the final newline, and nothing else. It does not re-space or reflow a line: `fn   main( ):int{return   0;}` stays exactly as written. Before it writes or reports a result it checks that no non-whitespace byte changed, that the lexer reads the same tokens from both texts, and that formatting the result again changes nothing; a file that fails a check is left alone and reported. A file with a lexical error (an unterminated string, a malformed number) cannot be formatted: if you named it that is a failure, and if it was only found under a directory it is listed as skipped and is not.

```text
$ turf fmt messy.tr
fn main(): int {
    if true {
        return 1;
    }
    return 0;

}
$ turf fmt --check .
would reformat messy.tr
Would reformat 1 file; 6 already formatted.
$ turf fmt --write messy.tr
Reformatted 1 file; 0 already formatted.
```

**`turf migrate pub [--write] <files…>`** adds `pub` to every top-level declaration and struct field, which keeps the 1.x behaviour where everything a module declared was importable (see [Functions & Modules](functions_and_modules.md)). Without `--write` it only reports what it would change:

```text
$ turf migrate pub old.tr
would migrate old.tr (2 `pub`)
  old.tr:1:
    - fn f(): int {
    + pub fn f(): int {
  old.tr:5:
    - fn main(): int {
    + pub fn main(): int {
Would migrate 2 declarations/fields in 1 file. Re-run with --write to apply.
$ turf migrate pub --write old.tr
migrated old.tr (2 `pub`)
Migrated 2 declarations/fields in 1 file.
```

### `turf explain`

```text
turf explain E001
```

`turf explain E001` and `turf --explain E001` are the same command (the second spelling is what error footers print). It takes a code and no source file.

- No code: `turf: explain: expected exactly one error code, for example: turf explain E001`, exit 2.
- An unknown code: `Unknown error code: E999`, then a line saying what a code looks like, exit 2.
- Otherwise it prints a header with the code, the error's class name and its category, then a write-up of what the error means, a wrong example and the fix, and exits 0.

The write-up is read from the installation (the directory that holds `lib/std`), not from the current directory, so it is the same wherever you run it:

```text
$ cd /tmp && turf explain E091
E091  ModuleNotImportableError (DECLARATION)

A module exists in this build, but this package may not import it

    // turf.toml of myapp:  [dependencies] plotter = { path = "../plotter" }
    ...
```

### `--smart`

`--smart` asks a small language model, running locally, to suggest a fix for the first error of a compile. The compiler checks the suggestion in an isolated copy of your program, and shows it only if it removes the error without introducing another; otherwise you see the compiler's own diagnostic, unchanged. Nothing runs on a clean compile, warnings never start it, and no network is involved.

```text
$ turf prog.tr --smart -o prog
prog.tr (1 error)

── error at line 1, col 1 [E009 · control flow]

  [AI Suggestion]
  Explanation: The function never returns a value.
  Suggested fix:
    + 2 |     return 42;
  ...
```

It needs two things.

- **The smart build.** Every release has two archives per platform: the plain one, and a `-smart` one that is the same compiler with the model runtime inside (`install.sh --smart` installs it). In the plain build `--smart` prints one note saying so and you get the compiler's diagnostics. A build from source has the runtime unless you build it with `--no-smart`. `turfc --version` ends with `smart built-in` or `smart not-built`.
- **One model, which you choose.** No model is shipped or downloaded. Put exactly one `.gguf` file in the `models` folder of your install (`<root>/models`; the root is the line `turf version` prints). Its name does not matter.

What it does with the folder:

| `<root>/models` holds | `--smart` |
|---|---|
| no `.gguf` file | prints `note: --smart has no model to run. Put one .gguf file in <root>/models …; showing the compiler's own diagnostics.` and carries on with the compiler's diagnostics |
| one `.gguf` file | loads it and uses it |
| more than one | uses none of them, because it cannot know which you meant; prints `note: --smart uses exactly one model and found 2 in <root>/models (a.gguf, b.gguf). Leave one .gguf file there; showing the compiler's own diagnostics.` |

A model has to load and run on the CPU, so a suggestion can take from several seconds to a minute or more per error. A suggestion the model cannot get validated after three attempts is dropped, and you see the compiler's diagnostic. The prompt design and sandbox are internals, described under [`docs/architecture/`](../architecture/).

### Exit status

`turf` and `turfc` use the same three statuses:

| Status | Meaning |
|---|---|
| **0** | It did what was asked. |
| **1** | It ran and reported a problem: a compile error in your program, a file it could not read or write, a bad manifest, a failed fetch, a test that failed. |
| **2** | The command line is wrong, so nothing was attempted: an unknown command or option, a missing value or input file, a bad `-O` level, a project command outside a project, `--emit-obj` together with `--emit-llvm`. |

`turf compile`, `turf check` and `turf <file.tr>` return the compiler's status unchanged. `turf run` is the exception: once your program has started it returns **the program's own** status, so a program that returns 2 looks like a usage error there. Anything above 2 (126, 127, 128 plus a signal) comes from the shell or the operating system.

### Versions

`turf` and `turfc` have separate version numbers, and `turf version` (or `turf --version`) shows both, then the facts a bug report needs:

```text
$ turf version
turf 1.0.0-dev  (needs turfc interface 1.1+)
turfc 2.0.0-dev  (interface 1.6, /opt/turf/bin/turfc)
  llvm 22.1.8
  runtime turf_rt_abi_2_0
  root /opt/turf
  linker system-cc
  smart built-in
```

The numbers are those of your installation. The indented lines are the compiler's own: the LLVM it was built against, the [runtime ABI stamp](#the-runtime-abi-stamp), the install root it reads `lib/std` and the runtime from, how it links a program, and whether `--smart` is built in. `linker system-cc` means it runs the machine's `cc`; `linker embedded-lld` means it has the `lld` linker built in, and a `bundled-runtime <dir>` line, present when that install has a complete runtime for the platform, means a program that names no C library beyond the C runtime is linked in-process, statically, with no `cc` on the machine. A program that does name one (`extern from "z"`, `--link z`) is linked by the system toolchain, which then needs that library's development files. `TURF_LINKER=system` forces the system toolchain for every program, an escape hatch if the embedded linker misbehaves; when the embedded linker fails, the message says so and does not fall back by itself.

The two programs need not have the same version. What must fit is the **driver interface**, a `major.minor` pair that `turfc` reports and `turf` needs: the same major and at least the minor `turf` names. Before it runs anything, `turf` checks this and refuses a compiler that does not fit, naming both sides and which one to upgrade:

```text
turf: this compiler does not speak the interface this turf needs
  turf  1.0.0-dev  needs interface 1.1 or a later 1.x  (/opt/turf/bin/turf)
  turfc 9.0.0  speaks interface 2.0  (/opt/fake/turfc)
hint: upgrade turf: this compiler's interface is newer than this turf understands
```

A differing *release* version is never an error on its own. In a project, a manifest's `turf = "2.0"` is about the compiler's release version and is checked separately ([Projects & Packages](projects_and_packages.md#2-turftoml)).

### Finding the compiler

`turf` looks for `turfc` in this order and takes the first match:

1. `$TURF_COMPILER`, a path to the binary.
2. `turfc` in the directory of the running `turf`, with symlinks resolved (an installed pair, or two binaries side by side).
3. `$TURF_HOME/bin/turfc`.
4. `turfc` on `PATH`.

Nothing found is an error that lists what was looked at:

```text
turf: cannot find the compiler (turfc)
  looked in:
    $TURF_COMPILER (not set)
    /somewhere/alone/turfc
    $TURF_HOME/bin/turfc (TURF_HOME not set)
    turfc on PATH
hint: set TURF_COMPILER to the turfc binary, or build it with `cmake --build build`
```

A `TURF_COMPILER` naming a file that does not exist is its own error. An unpacked release archive needs no environment variable at all: both programs find their install root (`lib/std`, the runtime library) from their own location. Set `TURF_HOME` only for an install you have moved apart; a stale `TURF_HOME` pointing at another installation makes every compile use that installation's library, and typically fails with "Could not find module" (E027).

### For tool authors: `turfc`

Everything above is `turf`. This section is for whoever drives the compiler directly: an editor plugin, a build system, a script. `turfc` is a complete compiler on its own. It never touches the network, never reads `turf.toml` or `turf.lock`, and never searches for them. (An in-process query interface for tools that want parse and type information without a command line is in `include/turf/Analysis.h`.)

```text
$ turfc help
Usage: turfc <filename.tr> [-o output] [--emit-llvm | --emit-obj] [--smart]
         [--smart-write-fix=<path>] [--link <libname>]
         [--allow-unsafe-modules] [--stdlib-warnings]
         [-O0|-O1|-O2|-O3|-Os] [-g] [--target=<triple>] [-mcpu=<cpu>]
         [--check [--module]] [--package-graph <file>]
         [--resolve] [--typecheck] [--dump-ast] [--dump-resolver-symbols]
         [--dump-resolved] [--dump-typechecked] [--dump-modules]
         [--dump-instantiations]
       turfc --format [--write | --check [--skip-unlexable]] <filename.tr>...
       turfc --migrate=pub [--write] <module.tr>...
       turfc --test [<tests-dir>] [--package-graph <file>] [--exclude <path>]
         [-O0|-O1|-O2|-O3|-Os] [-g] [--link <libname>] [--jobs <n>]
         [--timeout <seconds>] [--diag-register <file>]
         [--allow-unsafe-modules] [--stdlib-warnings]
       turfc --explain E0NN
       turfc --version
       turfc help
```

**The driver interface.** `turfc --version` starts with two lines: the release version, then the interface `turf` checks.

```text
$ turfc --version
turfc 2.0.0-dev
interface 1.6
llvm 22.1.8
runtime turf_rt_abi_2_0
root /opt/turf
linker system-cc
smart built-in
```

The interface covers what `turf` relies on: the flags it passes, the two-line start of `--version`, the exit statuses, and every other argument being accepted exactly as a direct invocation accepts it. A new flag or input raises the **minor**; removing a flag or changing its meaning raises the **major**. A tool that needs `--package-graph` requires interface 1.1 or later, `turfc --test` 1.3, and `--emit-obj` 1.6.

**Modes a tool uses:**

- `turfc --check <file.tr>` runs every diagnostic and writes nothing; the output and status match a compile of the same file. It checks a **program**, so it wants a `main`; `turfc --check --module <file.tr>` checks a file that is only ever imported. This is what `turf check` and `turf dump` run.
- `turfc --package-graph <file.json> <file.tr>` compiles as a package build, and is the only way the compiler learns about packages. The graph lists each package's source root, its **direct** dependencies and whether its `unsafe module;` declarations are honored. `turf build` writes it as `build/package-graph.json`, and `turf graph` prints it:

  ```json
  {
    "format": 1,
    "root": "plotter",
    "packages": {
      "geometry": {
        "src": "/work/geometry/src",
        "deps": [],
        "allow_unsafe": false
      },
      "plotter": {
        "src": "/work/plotter/src",
        "deps": [
          "geometry"
        ],
        "allow_unsafe": true
      }
    }
  }
  ```

  A bad graph is one line naming the file and exit 1. Without the flag `turfc` is in single-file mode. In a package build the file you name must be inside the root package's `src`.
- `turfc --format …`, `turfc --migrate=pub …`, `turfc --explain E0NN` and `turfc --test …` are what `turf fmt`, `turf migrate`, `turf explain` and `turf test` run.

**`turf` never rewrites a compiler diagnostic.** A tool that runs `turf check` or `turfc --check` sees the same text and the same `[E0NN]` codes either way, and can match on them.

---

## Part 2 — Diagnostics and error codes

### How a diagnostic is printed

```text
$ turf check oob.tr
oob.tr (1 error)

── error at line 4, col 18 [E001 · array error]

  Array index out of bounds!
  You tried to access 'primes[5]', but 'primes' only has 3 elements (indices 0 to 2, or -1 to -3 counting from the end).

  Hint: Array indices start at 0 and go up to size - 1; a negative index counts back from the end (-1 is the last element).

  4 │     return primes[i];
    │                  ^ Here!

  Run `turf --explain E001` for more details.
```

Top to bottom:

1. **File header.** The path of the file the diagnostics below belong to, then a count: `src/main.tr (2 errors, 1 warning)`. Diagnostics are batched **per file**: the file you compiled comes first, then each imported module, and within a file they are sorted by line and column. The path is relative to the directory you ran the command from; a standard-library file is shown from `lib/std/` on.
2. **Diagnostic header.** `── error at line 4, col 18 [E001 · array error]`: the position, the **code** (`E001`) and the **category** (`array error`). The code is what you give to `turf explain`. The categories are type mismatch, undeclared variable, control flow, syntax error, arithmetic, string conversion, array error, declaration, function signature, builtin function, memory error and general. A diagnostic with no source position (a missing `main`) omits "at line, col". Many warnings carry no code and no category.
3. **The message**, in plain words.
4. Zero or more `Note:` lines, one per related location (`Note: declared here (line 2, col 5)`).
5. An optional **hint**: one line saying how to fix it.
6. A **source snippet**: the line, with an underline under the offending token and the word `Here!`. A single-token span is underlined `^~~~`; where only a position is known, it is a bare `^`.
7. A **footer**, when the diagnostic has a code: ``Run `turf --explain E0NN` for more details.`` For the codes that most need reading in full, it is the stronger `Worth reading in full -- run `turf --explain E0NN` before you patch this one.`

The same mistake never produces a wall of follow-on errors. Once a line has a diagnostic, every later diagnostic on that same line is dropped, so one typo does not report itself three times.

### Errors and warnings

- **Errors** stop the compile. The compiler still reports every error it can find (it recovers at the next statement or declaration and goes on), prints them all, and exits with status 1 without producing a program.
- **Warnings** never stop the compile and never change the exit status. They say a thing is probably a mistake: a variable nobody reads, a loop that never ends, code that cannot run. A program with warnings compiles, links and runs. There is no flag that turns warnings into errors, and none that silences them one by one; the way to silence a warning is to fix what it points at.

Warnings are printed in the same format as errors, in yellow, with `warning` in the header.

### Warnings in the standard library

A warning whose position is inside the installed standard library is recorded but not printed. It is a warning about code you did not write and cannot change, so a plain `import json;` or `import http;` does not print warnings that are not yours. This is the same rule as a system header in a C compiler. `--stdlib-warnings` shows them:

```text
$ turf check server.tr --stdlib-warnings
lib/std/http/http.tr (1 warning)

── warning at line 353, col 5

  Warning: Function 'http::Server.listen' contains a loop with no exit path. This will run forever.
  ...
```

The rule applies to every warning class alike, and your own code is never affected by it. Errors are never hidden.

### "Did you mean?" suggestions

When you use a name the compiler does not know (a variable, a type, a field, a method, a function in a module, an enum variant), it looks for close matches among the names in scope and suggests them:

```text
$ turf check typo.tr
typo.tr (1 error)

── error at line 3, col 12 [E040 · undeclared variable]

  I can't find anything named 'contaner'.

  Hint: It may be from the current file: 'container'

  3 │     return contaner;
    │            ^~~~~~~~ Here!

  Run `turf --explain E040` for more details.
```

A match is a small edit distance away, and swapping two adjacent letters counts as one edit. The allowed distance grows with the length of the name: 1 for four characters or fewer, 2 for up to seven, about a third of the length beyond that. When nothing is close, the hint explains how to declare the thing instead.

### Error codes

Every error has a stable code, `E001` to `E148`: **148 codes** with no gaps. A code is assigned once and never reused or renumbered, and it is an identifier, not a ranking of severity. Every one has a page, `turf explain E0NN`, that says what the error means, shows a wrong program and the fix. The same pages are in the repository as `docs/errors/codes/E0NN.txt`, and the release archives carry them.

Six codes are **retired**: the mistakes they described are now reported under another code, and the number stays so old references still resolve. `turf explain` on them says which code replaced them. They are E006 (an unsized array in a `for` loop; arrays carry their length now), E013 (merged into E012), E015 (syntax errors, now E099–E123), E037 (unknown functions are E041), and E055 and E063 (now E056).

The codes by category (the category is the word in the diagnostic header):

| Category | Codes |
|---|---|
| array error | E001–E006, E123, E140–E144 |
| control flow | E007, E009–E010, E012–E013, E016, E058, E074–E075, E083, E092–E095, E120, E132–E133 |
| undeclared variable | E008, E037, E040–E041, E043, E054, E068, E131 |
| syntax error | E014–E015, E070–E073, E099–E111, E118, E139 |
| declaration | E019–E024, E027–E034, E039, E042, E045–E046, E048–E053, E060–E061, E069, E079–E080, E084–E085, E087–E091, E096–E097, E112–E117, E119, E128–E129, E136, E138, E145–E148 |
| function signature | E025, E038, E044, E047, E067, E125, E135, E137 |
| type mismatch | E035–E036, E056, E059, E062, E064, E066, E082, E098, E124, E126, E134 |
| arithmetic | E057, E076–E078, E127 |
| string conversion | E065 |
| builtin function | E055, E063 |
| memory error | E017–E018, E081, E086, E130 |
| general | E011, E026, E121–E122 |

Eleven codes are flagged **important**: concepts the one-line message alone does not teach. They get the stronger footer, `Worth reading in full`: **E010** (infinite recursion), **E017** (double free), **E018** (use after free), **E035** and **E036** (invalid state transitions), **E069** (a struct that contains itself), **E086** (a reference that would outlive its target), **E088** (a private item used from another module), **E089** (a private type in a public signature), **E121** (an internal compiler error: report it with the program that triggers it), and **E130** (raw pointer outside `unsafe`).

**Modules and packages.** Five codes concern how an `import` resolves, and the fix differs for each:

- **E027**: nothing answers to that module name. In a single-file compile inside a project, the hint adds that the project's dependencies are only visible to `turf build`.
- **E090**: two or more different files answer to one name (your own module and the standard library, your own module and a dependency, or two of your own). The message lists every candidate with its kind and path, and the hint says which to rename.
- **E091**: the module exists in this build but this package may not import it: it belongs to a *transitive* dependency or is another package's *internal* module.
- **E145**: `m::name` was written in a file that has no plain `import m;` of its own.
- **E088** and **E089**: the module is reachable but the item is not `pub`.

Real examples of each are in [Projects & Packages](projects_and_packages.md#4-how-imports-resolve).

---

## Part 3 — Compile-time static analysis

The compiler builds a control-flow graph for every function and runs a fixed set of analyses over it, on every compile, in addition to type checking. No flag turns them on or off. The analyses are per function; there is no whole-program analysis. Each example below was compiled; the programs that show an error carry the code the compiler gives.

### Statement after a terminator (E012)

A statement that follows a `return`, `break` or `continue` in the same block can never run, and is an error. This covers every kind of statement, a declaration included. The error points at the terminator:

<!-- doc-example: fails E012 -->
```turflang
fn main(): int {
    return 0;
    immut x: int = 5;
}
```

```text
── error at line 2, col 5 [E012 · control flow]

  This code comes after a return/break/continue, so it's impossible to reach it!

  Hint: Delete this code or move it before the 'return/break/continue' statement.

  2 │     return 0;
    │     ^~~~~~ Here!
```

### Unreachable code (E074)

A *block* that no path can reach is a warning. The classic case is a loop that only ever leaves through a `return`:

```turflang
fn pick(): int {
    while true {
        return 1;
    }
    return 2;
}

fn main(): int {
    return pick();
}
```

```text
── warning at line 5, col 5 [E074 · control flow]

  Warning: The code in 'while.end' inside 'pick' can never be reached!

  Hint: Check your 'if' conditions. Are they impossible to be true, or did you 'return' before this block?

  5 │     return 2;
    │     ^~~~~~ Here!

  Run `turf --explain E074` for more details.
```

E075 is the same finding for an `if` or similar branch point inside already-dead code, and E094 is a `compare` arm that can never run because an earlier arm without a guard matches everything.

### Missing return (E009)

A function that declares a result must return on every path:

<!-- doc-example: fails E009 -->
```turflang
fn pick(flag: bool): int {
    if flag {
        return 1;
    }
}

fn main(): int {
    return pick(true);
}
```

```text
── error at line 1, col 1 [E009 · control flow]

  The function 'pick' promises to give back a result, but sometimes it doesn't!

  Hint: Make sure every path in your function (like inside 'if' and 'else') has a 'return' statement.
```

A function returning `void` is never checked for this.

### Use before assignment (E008)

A local variable that is declared without a value must be assigned on **every** path that reaches a read of it. Parameters are always assigned.

<!-- doc-example: fails E008 -->
```turflang
fn main(): int {
    mut y: int;
    if 1 > 2 {
        y = 5;
    }
    return y;
}
```

```text
── error at line 6, col 12 [E008 · undeclared variable]

  Oops! You're using 'y' (type 'int') before it has a value.
  Note: declared here (line 2, col 5)

  Hint: Make sure every path assigns to 'y' before you use it.
```

The same analysis requires a struct's `new` method to assign every field on every path (E049), described in [Structs & Enums](structs_and_enums.md#constructing-a-struct).

### Unused variables and dead stores

Two warnings from a liveness analysis. A local variable that is never read is *unused*; a value written to a local that is overwritten, or the function ends, before anything reads it is a *dead store*. Parameters are excluded from both, and a name that starts with `_` is the convention for "intentionally unused".

```turflang
fn main(): int {
    mut x: int = 5;
    x = 10;
    mut unused: int = 1;
    return x + 1;
}
```

```text
── warning at line 2, col 5

  Warning: Variable 'x' is assigned but the value is never read (dead store).

  Hint: The assigned value is overwritten or the function returns before this variable is ever used. Remove the assignment or use the value.

── warning at line 4, col 5

  Warning: Variable 'unused' is declared but never used.
```

The first `x` (`5`) is overwritten before it is read; the second (`10`) is read by `x + 1`, so it is not reported.

### Infinite loops (warning)

A loop whose condition is a literal `true` and that has no way out is a warning, because the code after it can never run:

```turflang
fn spin(): int {
    mut x: int = 0;
    while true {
        x = x + 1;
    }
    return x;
}

fn main(): int {
    return 0;
}
```

This prints three warnings: one on the function (`contains a loop with no exit path. This will run forever.`), one on the loop (`This looks like an infinite loop! The condition is always true and the body has no 'break', 'continue', or 'return'.`) and the unreachable `return x;` (E074). A `while` whose condition is not a literal is never reported this way. A server's accept loop is a legitimate infinite loop; the warning is advisory.

### Infinite recursion (E010)

A function in which *every* path through the body calls the function again has no base case:

<!-- doc-example: fails E010 -->
```turflang
fn foo(): int {
    return foo();
}

fn main(): int {
    return foo();
}
```

```text
── error at line 1, col 1 [E010 · control flow]

  Infinite recursion detected in 'foo'.

  Hint: Every recursive function needs a base case — a path that returns without calling itself.

  Worth reading in full -- run `turf --explain E010` before you patch this one.
```

A call to a different overload of the same name is not self-recursion.

### Double free and use after free (E017, E018)

For memory you manage yourself, a pattern-based analysis follows which pointer variables refer to which allocations. It knows an allocator and a releaser only because you say so with `@ownership` on the `extern` declaration; there is no built-in list of names. It reports a second release of the same allocation (E017) and an array access through a variable whose allocation has already been released (E018), each with a note at the release:

<!-- doc-example: fails E018 -->
```turflang
@ownership(acquires)
extern fn malloc(size: int): int*;

@ownership(releases: p)
extern fn free(p: int*): void;

fn main(): int {
    mut r: int = 0;
    unsafe {
        mut p: int* = malloc(sizeof(int) * 4);
        free(p);
        r = p[0];
    }
    return r;
}
```

```text
── error at line 12, col 14 [E018 · memory error]

  Oops! You're using memory from 'p' after it was freed.
  Note: freed here (line 11, col 13)

  Worth reading in full -- run `turf --explain E018` before you patch this one.
```

When only some paths release the allocation, the same findings are warnings (`'p' may already have been freed on some path reaching this use.`). The analysis is not a full alias analysis: it follows plain variables and copies between them, and an allocation that escapes into a struct or another function is not tracked. Memory management is covered in [Memory & Safety](memory_and_safety.md).

### Constant array indexes (E001)

When an index is a value the compiler can compute, and the array has a fixed size, an out-of-range index is an error at compile time, with the same E001 a run-time violation would give:

<!-- doc-example: fails E001 -->
```turflang
fn main(): int {
    immut primes: int[3] = [2, 3, 5];
    mut i: int = 5;
    return primes[i];
}
```

An index that depends on a parameter or on input is checked at run time instead (see [Part 4](#part-4--when-a-compiled-program-fails)). A constant literal that does not fit its type (`9223372036854775807 + 1`) is E076.

### Complexity and other warnings

A function whose cyclomatic complexity (the number of independent paths through it) is above 15 gets an advisory warning, `Function 'f' has cyclomatic complexity 17 (threshold: 15). Consider breaking it into smaller functions.` Sixteen separate `if` statements, as here, are enough:

```turflang
fn f(x: int): int {
    if x == 1 { return 1; }
    if x == 2 { return 2; }
    if x == 3 { return 3; }
    if x == 4 { return 4; }
    if x == 5 { return 5; }
    if x == 6 { return 6; }
    if x == 7 { return 7; }
    if x == 8 { return 8; }
    if x == 9 { return 9; }
    if x == 10 { return 10; }
    if x == 11 { return 11; }
    if x == 12 { return 12; }
    if x == 13 { return 13; }
    if x == 14 { return 14; }
    if x == 15 { return 15; }
    if x == 16 { return 16; }
    return 0;
}

fn main(): int {
    return f(3);
}
```

A few more warnings come from simpler checks that look at one expression or statement:

| Warning | Fires on |
|---|---|
| `You're using '/' with zero — this will crash at runtime!` | a literal `0` as the divisor of `/` or `%` |
| `You're assigning 'k' to itself — this doesn't do anything!` | `k = k;` |
| `You're comparing 'x' with itself!` | `x == x`, `x < x` and so on |
| `Just so you know, you're creating a new 'n' that hides an older one!` | a local that shadows another in an enclosing scope |
| `The loop variable 'i' is never changed by this loop's 'step' or its body, so it will loop forever` | a `for` whose variable nothing advances |
| `The 'then' branch is empty` | an `if` or `else` with no statements |
| `'f' is declared to return a fallible type ('!T') but never actually returns an error.` | a `!T` function with no error path |
| `Machine 'M' declares state 'S' with no incoming transitions.` | a state no edge leads into (expected for the initial state) |

---

## Part 4 — When a compiled program fails

A program that compiles can still fail while it runs. Every failure the language itself detects is a **panic**: the program prints one line to standard error and terminates with exit status **134**, the status of `SIGABRT`. It never carries on with a corrupt value, and it never produces a bare crash with no message.

```text
panic: runtime check: <what happened> (<file>:<line>)
```

| Failure | Message | Exit |
|---|---|---|
| Array index out of range | `panic: runtime check: Array index out of bounds for 'a' (oob.tr:2)` | 134 |
| Division or remainder by zero | `panic: runtime check: Division by zero (div.tr:2)` | 134 |
| Integer overflow in `+`, `-`, `*` or negation | `panic: runtime check: Integer overflow (ovf.tr:2)` | 134 |
| A `double` converted to `int` when the value does not fit (NaN, infinity, too large) | `panic: runtime check: Integer overflow (castf.tr:2)` | 134 |
| Stack overflow (unbounded recursion) | `panic: stack overflow: the call stack exceeded its limit -- check for unbounded or missing-base-case recursion` | 134 |
| Out of memory (an allocation that cannot be satisfied) | `panic: out of memory: …` | 134 |
| A memory fault, which only unsafe code can cause | `panic: segmentation fault (this is a bug in unsafe code or in TurfLang) at address 0x…` | 134 |
| A checked standard-library call, such as `List.get` with a bad index | `prog.tr:6: List.get: index out of bounds`, the line of the call in your code | 134 |
| `sys::panic("msg")` | the message you gave | 134 |
| `sys::abort()` | none | 134 |
| `sys::exit(n)` | none | `n` |

The `(file:line)` is the line of the failing operation in your source. A failure inside a standard-library function is reported at the line of the call in *your* code that led to it.

`sys::exit(n)` is the one way out that is not a failure: it stops the program at once with exit status `n`, and returning `n` from `main` does the same. These checks are on at every optimization level; see [Optimization levels](#optimization-levels). The standard library's `!T` results, `try` and `recover` are the way for a program to handle failure it can anticipate; see [Error Handling](error_handling.md).

A panic status of 134 is what `turf run` gives back too (`turf run` returns the program's own status, or 128 plus the signal that killed it), and what `turf test` expects of a test marked as an expected abort.

### The runtime ABI stamp

Every program the compiler produces calls a stamp function in the runtime library, `turf_rt_abi_2_0`, and `turf version` prints the stamp its compiler carries (`runtime turf_rt_abi_2_0`). A runtime library that does not provide it, because it was built for a different compiler generation, fails at **link time** instead of misbehaving at run time:

```text
$ cc hello.o -o hello
/usr/bin/ld: hello.o: in function `main': undefined reference to `turf_rt_abi_2_0'
```

A normal `turf` build links the runtime that belongs to its compiler and never meets this. You can only see it if you link an object by hand (`--emit-obj`) against the wrong library. The fix is to link against the `libturfrt.a` from the same installation as the `turfc` that wrote the object.
