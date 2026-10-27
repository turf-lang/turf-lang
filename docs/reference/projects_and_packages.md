# Projects and Packages

A TurfLang **project** is a directory with a `turf.toml`. It holds one
**package**: a named set of modules that can be built into a program, imported
by other packages as a library, or both. `turf` reads the manifest, finds the
package's dependencies (other directories, or git repositories), pins the git
ones in a lock file, and compiles everything, every module of every package,
in one go.

You do not need a project to compile a program. `turf hello.tr` and
`turf compile hello.tr` work on a single file with no manifest; see
[CLI & Diagnostics](cli_and_diagnostics.md). A project is what you
want once a program has more than one file, or uses someone else's code.

Two multi-package projects you can build and run are in
[`examples/`](../../examples/README.md): a library, `geometry`, and a program,
`plotter`, that depends on it.

> [!NOTE]
> In the messages on this page, the absolute paths `turf` prints are shortened
> to `…/`, and a few long hashes to `…`. Diagnostics from the compiler say
> `turf --explain E0NN`; `turf explain E0NN` is the same command.

## 1. A project

```
myapp/
  turf.toml          the manifest
  turf.lock          written by turf; commit it
  src/main.tr        present: the package is a program
  src/myapp.tr       present: the package is a library; this is its root module
  src/**.tr          the package's other (internal) modules
  tests/**.tr        the package's tests, run by `turf test`
  build/             output: the executable and package-graph.json; not committed
```

- **A program** has `src/main.tr`: the package's one entry point.
  - `turf build` produces `build/<name>` from it, and `turf run` runs it and
    nothing else.
  - No other file of a package can be its program: the compiler refuses one
    in a package build. `turf run <file.tr>` runs a single file on its own,
    outside the package.
- **A library** has `src/<name>.tr`, its **root module**, named after the
  package. It is the only module another package can `import`.
  - `turf build` always checks it (as a module, so it needs no `main`), so
    its errors show even when nothing imports it.
  - A library without `src/main.tr` builds no executable, and `turf run`
    there says how to add one (below).
- **A package may be both,** and `turf new --lib` makes it so. With
  `src/main.tr` and `src/both.tr`, `turf run` runs the program, and another
  package can still depend on it and import `both`. The dependency sees only
  the root module; its `src/main.tr` is ignored.
- **`turf run` with no `src/main.tr`:**

  ```
  $ turf run
  turf: package 'geometry' has no src/main.tr, so there is nothing to run.
  hint: `turf run` runs src/main.tr, the package's entry point. Create it, for example:

          // src/main.tr: this package's entry point. `turf run` builds and runs it.
          // The library itself is src/geometry.tr; other packages import that, never this.
          import io;
          import geometry;

          fn main(): int {
              io::printline("Hello, TurfLang! geometry::answer() is " + (geometry::answer() to string) + ".");
              return 0;
          }
        To run some other file on its own instead: turf run <file.tr>
  ```
- **A package with neither** cannot be built:

  ```
  turf: package 'empty' has neither src/main.tr (a program) nor src/empty.tr (a library)
  ```

- **Internal modules** are any other `.tr` file under `src/`, in any
  subdirectory. `import shouter;` finds `src/util/text/shouter.tr`; the
  directory path is not part of the name. Two files with the same name anywhere
  under `src/` are an ambiguity (section 4).
- `turf.lock` is created by the first `turf build`, `run`, `check` or `fetch`,
  even when there is nothing to pin. `build/` is disposable: `turf clean`
  deletes it. `tests/` is what `turf test` runs (see *Tests* below).

### `turf new` and `turf init`

```
$ turf new hello
Created program package 'hello' in hello/
  hello/turf.toml
  hello/src/main.tr
  hello/README.md
  hello/.vscode/extensions.json
  hello/.gitignore
$ cd hello
$ turf run
Compiled to build/hello.o
Successfully compiled to build/hello
Hello, TurfLang! This is hello.
```

Every new package runs straight away. The generated `README.md` lists the
everyday commands (`turf run`, `build`, `check`, `add`, `remove`, `update`,
`clean`, `fmt`, `explain`, `help`) and the project's layout.
`.vscode/extensions.json` recommends the Turf extension, so VS Code (and its
forks) offers to install it when the project is opened.

`turf new geometry --lib` also creates the root module `src/geometry.tr`. Its
`src/main.tr` imports it, so the library can be tried with `turf run`:

```
$ turf new geometry --lib
Created library package 'geometry' in geometry/
  geometry/turf.toml
  geometry/src/main.tr
  geometry/src/geometry.tr
  geometry/README.md
  geometry/.vscode/extensions.json
  geometry/.gitignore
```

`turf init` does the same in the current directory. Its name defaults to the
directory's; an existing `src/main.tr`, root module, `README.md` or
`.vscode/extensions.json` is never overwritten; and `/build/` is added to an
existing `.gitignore`:

```
$ turf init
Initialized package 'existing' in …/existing
  turf.toml
  src/main.tr (already there, left as it is)
  README.md
  .vscode/extensions.json
  .gitignore
```

Both refuse a name that could not be imported:

```
$ turf new list --lib
turf: 'list' is the name of a standard-library module, so a library called that could never be imported (`import list;` would name the stdlib module)
hint: pick another name
$ turf new fn
turf: 'fn' is a reserved word, so `import fn;` could never be written
$ turf new my-app
turf: 'my-app' is not a valid package name: it must match [A-Za-z_][A-Za-z0-9_]* because it is spelled in `import`
```

A *program* named like a stdlib module is fine (`turf new json` works), since
nothing can import a program.

### Tests: `turf test`

`turf test` compiles and runs every `.tr` file under the project's `tests/`
directory, at any depth, and every [`@test`](functions_and_modules.md#test)
function in the files of its `src/`, in parallel, and exits 0 only if all of
them pass. **A project with no test anywhere is an error**: no `tests/`
directory and no `@test` function under `src/`, or a `tests/` that holds no
tests. A run of zero tests that reports success is how an untested project goes
unnoticed.

```
$ turf test
Error: no tests: 'tests' does not exist, and no file under src/ declares a `@test` function.
  hint: write a `@test` function beside the code it checks, or create 'tests' and put test files (`.tr`) in it.
```

A test is an ordinary program with a `main` that returns 0, and the stdlib's
`test` module (`import test;`) has the assertions. What decides pass or fail is
the file's name:

| File | Passes when |
|---|---|
| `name.tr` | it compiles and exits with status 0 |
| `fail_name.tr` | it **fails to compile** (the compiler crashing does not count) |
| `fail_name.tr` containing the comment `// RUNTIME_ABORT_EXPECTED` | it compiles and then aborts (exits non-zero) |
| `name_module.tr` | never run: a companion for a test to `import` |

A file with a line starting `@test` declares tests beside the code they check
(see [`@test`](functions_and_modules.md#test)): it needs no `main`, and each
`@test` function is a test of its own, reported as `file.tr: test_name`. A
file under `tests/` that also has a `main` is run as an ordinary program too.
The root package's `src/` files that declare tests are run as well (their
`main` is not a test); `turf test <path>...` runs only the paths it names, so no
`src/` test then. A file whose tests do not build is one failure, `COMPILE
FAILED (its tests did not run)`.

Comments can pin the reason, not only the outcome, so a test cannot keep passing
because it broke in some other way:

| Comment | Holds when |
|---|---|
| `// EXPECT-DIAG: E011` | the compiler's output has the error code `E011` |
| `// EXPECT-DIAG: some text` | ... or this text (for a warning, whose code also works) |
| `// EXPECT-NO-DIAG: some text` | the compiler's output does **not** have this text |
| `// EXPECT-FIX: replace 'nmae' with 'name'` | the compiler printed that `Fix:` line (see [Fixes](cli_and_diagnostics.md#fixes)) |
| `// EXPECT-RUNTIME: some text` | the aborting program printed this text |
| `// EXPECT-EXIT: 134` | the aborting program exited with this status |

An `EXPECT-FIX` line matches the text after `Fix: ` exactly. When a test has
one, the runner also applies the printed fixes to a copy of the program and
checks that the copy has fewer errors and none the original lacked. A test
without the marker is not affected by the fixes its errors print.

Each test runs in its own directory (so it can open a data file beside it) with
no standard input, and is stopped after 5 seconds. A test imports what the
package itself can: its own modules under `src/`, its direct dependencies and
the standard library, plus the `*_module.tr` helpers in `tests/`. A test may
share its file's name with the module it tests: in `tests/route.tr`,
`import route;` is `src/route.tr`. The
internal-module rules of section 4 apply to a test exactly as to `src/`: it
cannot import another package's internal module (E091).

`turf test <path>...` runs only the tests it names: each path is a test file or
a directory of tests under `tests/`, written from the current directory
(`turf test tests/route.tr` from the project root). A path that does not exist,
is outside `tests/` or holds no test is an error. With no path, every test runs.

`turf test` takes `--offline` and `--locked` (it resolves the project first,
like `turf build`), and hands the rest to the compiler's runner:

| Flag | Meaning |
|---|---|
| `--jobs <n>` | tests running at once (default: every processor, or `$TURF_TEST_JOBS`) |
| `--timeout <seconds>` | how long one test program may run (default 5, or `$TURF_TEST_TIMEOUT_SEC`) |
| `--exclude <path>` | skip a file or directory, relative to `tests/`; repeatable |
| `-O0`..`-Os`, `-g`, `--link <lib>` | passed to every test's compile |

`-o` and `--package-graph` are refused, as for `turf build`. The exit status is 0
when every test passed and 1 when one failed or nothing could run. A failing
test is listed with its exit status and what it printed:

```
$ turf test
Running Turf Test Suites...

[tests] bad.tr ... RUNTIME FAILED (Exit code 134)
[tests] good.tr ... PASS

=== Test Summary ===
Total Tests: 2
Passed:      1
Failed:      1

Failed Tests Details:

--- tests/bad.tr (Runtime Error exit 134):
bad.tr:3: math
 ---
```

The same runner is `turfc --test [<dir>] [--package-graph <file>] ...`, for a
directory of tests that is not a project (it needs no manifest).

## 2. `turf.toml`

`turf.toml` is read with a real TOML parser. Here is what `turf new` writes:

```toml
[package]
name = "hello"
version = "0.1.0"
turf = "2.0"

[dependencies]
```

There are four tables and no others.

| Table | Key | Meaning |
|---|---|---|
| `[package]` | `name` | **Required.** The package's name, spelled in `import`. It must match `[A-Za-z_][A-Za-z0-9_]*` and not be a reserved word. A library may not be named like a standard-library module. |
| | `version` | **Required.** A string, not interpreted in 2.0. Dependents pin a git tag or commit, not this. |
| | `turf` | Optional. The oldest compiler this package needs, `"<major>.<minor>"`. The major must equal the compiler's, and the minor must not be newer than it. |
| `[dependencies]` | `<name> = { … }` | One entry per dependency (section 3). The key must be the dependency's own package name. |
| `[unsafe]` | `allow = [ "<name>", … ]` | The dependencies whose `unsafe module;` declarations are honored (section 8). |
| `[lints]` | `E163 = "allow"` | The level of a warning, by code: `allow`, `warn` or `deny` ([the `[lints]` table](#the-lints-table)). |
| | `lint-set = "2.N"` | Optional. Pins the set of warnings to the ones release 2.N has. |

**An unknown key or table is an error, not ignored.** A silently ignored
setting is how a typo becomes a policy.

```
turf: …/turf.toml:4: unknown key `authors` in [package]: the keys are name, version and turf
turf: …/turf.toml:5: unknown key `build` at the top level: the tables are [package], [dependencies], [unsafe] and [lints]
turf: …/turf.toml:6: unknown key `deny` in [unsafe]: the only key is allow
```

The other mistakes have their own messages, each naming the file and, where it
can be recovered, the line:

```
turf: …/turf.toml:1: [package] is missing the required key `name`
turf: …/turf.toml:1: [package] is missing the required key `version`
turf: …/turf.toml:3: `version` in [package] must be a string
turf: …/turf.toml:2: invalid `name` in [package]: 'fn' is a reserved word, so `import fn;` could never be written
turf: …/turf.toml:2: invalid `name` in [package]: 'my-app' is not a valid package name: it must match [A-Za-z_][A-Za-z0-9_]* because it is spelled in `import`
turf: …/turf.toml:4: invalid `turf` in [package]: '2' is not "<major>.<minor>"
hint: write the minimum compiler version as "<major>.<minor>", for example "2.0"
turf: …/turf.toml: missing [package] table
```

A `turf` requirement the compiler does not meet:

```
turf: package 'app' (…/turf.toml) requires turf "9.0", but the compiler is turfc 2.2.0 (…/build/turfc): a different major version
hint: use a turfc 9.x, or change `turf` in that manifest
turf: package 'app' (…/turf.toml) requires turf "2.99", but the compiler is turfc 2.2.0 (…/build/turfc), which is older
hint: upgrade turfc, or lower `turf` in that manifest
```

`turf` and `turfc` are versioned separately. `turf version` shows both:

```
$ turf version
turf 1.1.0  (needs turfc interface 1.1+)
turfc 2.2.0  (interface 1.12, …/bin/turfc)
  …
```

The `turf` key in the manifest is about the *compiler's* version (`turfc`, the
number after `turfc` above). The remaining lines of `turf version` are
described in [CLI & Diagnostics](cli_and_diagnostics.md#versions).

### The `[lints]` table

A warning has a code and a level (`allow`, `warn`, `deny`; see [Lint levels](cli_and_diagnostics.md#lint-levels)). The table sets them for the whole build of the project, for `turf build`, `run`, `check` and `test` alike:

```toml
[lints]
lint-set = "2.1"
E163 = "allow"   # unused variable: do not report it
E158 = "deny"    # function too complex: fail the build
```

Each key is a diagnostic code (`E` and three digits) and each value is `"allow"`, `"warn"` or `"deny"`. `lint-set` is the only other key, and its value is `"2.N"`. `turf` checks the shape of the table and passes it to the compiler as `--lint-set 2.1 --allow E163 --deny E158`, ahead of the flags you type, so `turf build --deny E163` beats `E163 = "allow"`. The compiler checks that each code exists and is a warning; a table naming an unknown code, or an error's code with `allow` or `warn`, fails the build with the compiler's message. A manifest with a `[lints]` table needs a compiler with driver interface 1.8 or later and says so otherwise. The table has no effect in a single-file compile, which has no manifest: pass the flags.

```
turf: …/turf.toml:5: unknown key `unused` in [lints]: the keys are lint-set and diagnostic codes such as E163
turf: …/turf.toml:5: invalid level for `E163` in [lints]: 'forbid' is not allow, warn or deny
turf: …/turf.toml:4: invalid `lint-set` in [lints]: '3.0' is not "2.N"
```

## 3. Dependencies

A dependency is an inline table with a `path`, or a `git` URL and one ref:

```toml
[dependencies]
geometry = { path = "../geometry" }
jsonx    = { git = "https://github.com/someone/jsonx", tag = "v1.2.0" }
```

The key is the dependency's package name, and `turf` checks it against the
dependency's own `turf.toml`:

```
turf: dependency 'geo' (../../geometry) in …/turf.toml is the package 'geometry'; the key must be its package name
```

**`path`** is a directory, relative to the manifest that declares it. It is
live source: `turf` reads it as it is on disk, and `turf.lock` records the
path but no hash. The directory must hold a `turf.toml` for that package and a
root module `src/<name>.tr`:

```
turf: dependency 'tool' in …/turf.toml is not a library package: it has no src/tool.tr
```

**`git`** needs exactly one of `tag`, `rev` or `branch`:

- `tag = "v1.2.0"` is what a release is. Once locked, the commit does not move
  unless you run `turf update`.
- `rev = "<commit>"` names a commit and never moves.
- `branch = "main"` follows a branch, but only when you run `turf update`;
  `turf build` keeps the commit in `turf.lock`.

```
turf: …/turf.toml:6: git dependency 'm' needs exactly one of `tag`, `rev` or `branch`, and has 2
hint: pin the dependency to a release with `tag`, or to a commit with `rev`
turf: …/turf.toml:6: dependency 'm' has both `path` and `git`: it is one or the other
turf: …/turf.toml:6: dependency 'm' needs `path`, or `git` with one of `tag`, `rev` or `branch`
```

A registry-style entry is refused, with a pointer to what works:

```
turf: …/turf.toml:6: dependency 'math' is a version string, not a table
hint: registry versions are not part of TurfLang 2.0 (…); use `{ path = "..." }` or `{ git = "...", tag = "..." }`
```

**A dependency may not be named like a standard-library module**, because
`import list;` would never reach it (section 4):

```
turf: dependency 'list' (…/libs/list/turf.toml) has the name of the standard-library module 'list', so `import list;` could never reach it
hint: rename the package
```

### Direct dependencies only

A package imports its own modules, its **direct** dependencies and the
standard library, and nothing else. If `plotter` depends on `geometry` and
`geometry` depends on `basepkg`, `plotter` cannot `import basepkg;` until it
lists `basepkg` itself (section 4 shows the message). Dependencies of
dependencies are fetched and built, but they are not in `plotter`'s namespace.

### One version per package name

A build has one package per name. If two packages need the same name at
different places or commits (a diamond that disagrees), it is an error naming
both requirers; there is no multi-version support. The first line says what
differs: two versions (tag, rev or commit) of one git repository, two
directories, two git repositories, or a git repository against a directory:

```
turf: package 'libA' is required at two different versions of git …/up/libA.git, and a build has one package per name:
  package 'left' (…/dia/left/turf.toml) needs it at git …/up/libA.git (tag=v1, commit 11343e6dbefb)
  package 'right' (…/dia/right/turf.toml) needs it at git …/up/libA.git (tag=v2, commit 483d19a6a2f2)
```

A diamond that agrees, meaning the same source and the same commit reached by
two routes (two spellings of one commit, `tag = "v1"` and its `rev`, agree), is
one package and builds. Two directories both holding a package called `pc`
is the same error, worded for directories:

```
turf: package 'pc' is required from two different directories, and a build has one package per name:
  package 'pa' (…/pcs/pa/turf.toml) needs it at …/pcs/one/pc
  package 'pb' (…/pcs/pb/turf.toml) needs it at …/pcs/two/pc
```

A dependency cycle is refused too:

```
turf: dependency cycle between packages: pa -> pb -> pa
```

### `turf add` and `turf remove`

```
$ turf add geometry --path ../geometry
Added dependency 'geometry' (../geometry) to …/turf.toml
$ turf add jsonx --git https://github.com/someone/jsonx --tag v1.2.0
turf: fetching jsonx (https://github.com/someone/jsonx)
Added dependency 'jsonx' (git https://github.com/someone/jsonx, tag=v1.2.0, commit 0858be2466c2) to …/turf.toml
```

`--path` is relative to where you type the command (it is written relative to
the manifest). `add` checks what it is about to write with the same code as
`turf build`, and writes nothing if the build would reject it:

```
turf: cannot add 'ghost' (nothing was written): dependency 'ghost' in …/turf.toml: cannot use path "../../ghost": no such file or directory
turf: dependency 'geometry' is already in [dependencies] of …/turf.toml
turf: a git dependency needs exactly one of --tag, --branch or --rev, and got 0
hint: pin a release with --tag, or a commit with --rev
```

`add --git` fetches the repository first, so it needs the network, and checks
its root manifest and module. It does not write `turf.lock`; the next
`turf build` (or `turf fetch`) does.

`add` and `remove` edit **one line** of `turf.toml` and touch nothing else.
Your comments, blank lines and key order stay, and an `add` followed by a
`remove` restores the file byte for byte:

```
# tool, my first program
[package]
name = "tool"
version = "0.1.0"
turf = "2.0"

# what I depend on
[dependencies]
geometry = { path = "../../geometry" }
```

`remove` deletes the line for that name in `[dependencies]` and nothing else.
A dependency written over several lines, or as a `[dependencies.name]` table,
is refused (edit it by hand), and a name that is not there is an error:

```
turf: dependency 'geometry' is not in [dependencies] of …/turf.toml
```

A name also listed in `[unsafe] allow` gets a note, because that line is left
for you to change and the next build rejects it until you do:

```
Removed dependency 'geometry' from …/plotter/turf.toml
turf: note: 'geometry' is still listed in [unsafe] allow in …/plotter/turf.toml; the next build will reject it until you remove it there
```

## 4. How imports resolve

In a project, `import foo;` inside package `P` names exactly one of:

1. **P's own module** `foo`: a file `src/**/foo.tr` of `P`.
2. **A direct dependency** named `foo`: its root module.
3. **The standard-library module** `foo`.

If more than one exists, that is an error listing every candidate: there is no
shadowing and "your own file wins" is not a rule. If none does, it is the usual
"module not found". `import foo::bar;` finds `foo` the same way (a flat
`import geometry::distance_squared;` of a dependency's `pub` function works).
The directory of the importing file, the current directory and `lib/usr` are
**not** searched in a project: a `helper.tr` next to `turf.toml`, or a module in
`lib/usr`, is not found by `import helper;`:

```
── error at line 1, col 1 [E027]

  Could not find module 'helper'.

  Hint: In a project, `import helper;` names one of three things: a module of this package (a file 'helper.tr' anywhere under hello's src/), a package listed under [dependencies] in its turf.toml, or a standard-library module. Check the spelling, create src/helper.tr, or add the dependency (`turf add helper --path <dir>`).
```

### E090: two things answer to one name

```
── error at line 1, col 8 [E090]

  Module 'list' is ambiguous: 2 different files answer to that name.
    standard library            …/lib/std/collections/list.tr
    module of package 'shapes'  ./src/list.tr

  Hint: A module may not share its name with a standard-library module. Rename ./src/list.tr (for example to 'my_list.tr') and import it by the new name.

  1 │ import list;
    │        ^~~~ Here!
```

The other collisions read the same way:

```
  Module 'geometry' is ambiguous: 2 different files answer to that name.
    module of package 'shapes'  ./src/geometry.tr
    dependency 'geometry'       …/geometry/src/geometry.tr

  Hint: A module may not share its name with a dependency. Rename ./src/geometry.tr (for example to 'my_geometry.tr') and import it by the new name.
```

```
  Module 'shouter' is ambiguous: 2 different files answer to that name.
    module of package 'deep'  ./src/other/shouter.tr
    module of package 'deep'  ./src/util/text/shouter.tr

  Hint: These are different files that both answer to 'shouter'. Rename one of them, or delete the one you did not mean.
```

The E090 is the real error. Each later use of the module (`list::n()`) also
reports an `E145` ("Module 'list' is not imported in this file"); read the first
diagnostic, not the ones after it.

Two **different packages** can each have an internal module named `util`, each
declaring a `Node`, without any collision: a module's identity is its package
plus its path, so they are two modules with two distinct types.

### E091: the module exists but is out of reach

A **transitive dependency**, or another package's **internal module**, exists
in the build but is not yours to import. This is a different error from "not
found" because the fix is different:

```
── error at line 2, col 8 [E091]

  Package 'basepkg' is not a dependency of 'plotter'.
    It is in this build because 'geometry' depends on it.

  Hint: A package can import only its own modules, its direct dependencies and the standard library. Add 'basepkg' to [dependencies] in plotter's turf.toml to import it here.

  2 │ import basepkg;
    │        ^~~~~~~ Here!
```

```
── error at line 2, col 8 [E091]

  Module 'vecmath' is internal to package 'geometry'.
    …/geometry/src/vecmath.tr

  Hint: Only a package's root module can be imported from outside it. Import 'geometry' and use what its root module makes `pub`; if 'geometry' should offer this, its root module has to expose it.

  2 │ import vecmath;
    │        ^~~~~~~ Here!
```

### Internal modules are private to their package

A package's API is its root module, and inside that module, what is marked
`pub`. Anything else in the package, `vecmath.tr` in `examples/geometry`, can
be imported only from the same package, whatever its own items say. Within a
module, visibility is the ordinary rule (everything is private unless `pub`),
which applies across packages too:

```
── error at line 4, col 22 [E088]

  Function 'hidden' is private to module 'geometry'.
  Note: 'hidden' is declared in module 'geometry' without `pub` (line 44, col 4)

  Hint: If 'hidden' is meant to be used from other modules, mark it `pub` in geometry.tr: `pub fn hidden(...)`. Otherwise, 'geometry' offers these public functions: distance_squared, manhattan, pick, size_of_type.
```

So a package has two levels: private to a module, and `pub`; and `pub` in an
internal module means "visible inside this package". For the same reason the
root module may not put a type of an internal module in a `pub` signature
(E089): other packages could receive its values but never write its name.
Declare the types of a package's API in its root module.

### Single-file mode

Outside a project (`turf hello.tr`, `turf compile`, `turf check file.tr`), the
compiler has no packages. `import foo;` looks in the standard library, in
`lib/usr`, next to the importing file and in the current directory, and it
examines **all** of them. If more than one distinct file answers, it is the
same E090:

```
── error at line 1, col 8 [E090]

  Module 'list' is ambiguous: 2 different files answer to that name.
    standard library  …/lib/std/collections/list.tr
    next to b.tr      ./list.tr

  Hint: A module may not share its name with a standard-library module. Rename ./list.tr (for example to 'my_list.tr') and import it by the new name.
```

The same file found twice (the source directory *is* the current directory) is
one candidate, not two.

A single-file compile inside a project cannot see the project's dependencies.
When an import fails there and a `turf.toml` exists in the file's directory or
above, the error says so:

```
$ turf compile src/main.tr
── error at line 1, col 1 [E027]

  Could not find module 'geometry'.

  Hint: Make sure the file 'geometry.tr' exists in the standard library or your current directory. This file is inside a TurfLang project (…/sf/turf.toml). Its dependencies are only visible to `turf build`, which compiles the project with its package graph.
```

The compiler only checks that a manifest file *exists*; it never reads it.
Use `turf build`, `turf run` or `turf check` (with no file) in a project.

## 5. `turf.lock`

`turf.lock` belongs to the **root** package (the one you build). A dependency's
own `turf.lock` is ignored. It is generated, and you commit it.

```toml
# This file is generated by turf. Commit it; do not edit it.
version = 1

[[package]]
name = "jsonx"
source = "git+…/up/jsonx.git"
ref = "tag=v1.0.0"
commit = "dec803e73fd04df31be79d859828efe595c14c38"
hash = "sha256:c2a61e5dbd09d27985855ab6c1ffacbe45ab24789738592e07dbbec29c1e8d4a"
```

- A **git** entry pins the requested ref, the resolved commit and a SHA-256 of
  the fetched tree (independent of file times and permissions). A **path**
  entry (`source = "path+../geometry"`) records where, and no hash, because a
  path dependency is live source. Packages are listed sorted by name.
- **The hash is checked on every build.** If a file in the cached tree has
  been edited, or the cache is corrupt, the build stops:

  ```
  turf: the cached tree of 'jsonx' (…/cache/src/jsonx-1d0f2d73…) does not match turf.lock (expected sha256:da455b69…, found sha256:618cac84…): it was modified or corrupted
  hint: delete …/cache/src/jsonx-1d0f2d73… and build again to re-extract it; if turf.lock itself is wrong, 'turf update jsonx'
  ```

- **`turf build` never moves a pin.** A `tag` that was re-pointed upstream, or a
  `branch` with new commits, is not noticed: the build keeps using the locked
  commit. Only `turf update` moves it:

  ```
  $ turf update
  turf: fetching jsonx (…/up/jsonx.git)
  jsonx de7262e335ed -> 1d0f2d7317cf
  $ turf update
  turf: fetching jsonx (…/up/jsonx.git)
  jsonx unchanged
  ```

  `turf update jsonx` updates just that dependency. A `rev` resolves to the
  same commit, so updating it changes nothing (`lib1 unchanged`). A `path`
  dependency, the root package and an unknown name are refused:

  ```
  turf: 'geometry' is a path dependency; it is always used as it is on disk
  turf: 'app' is the project's own package, not a dependency
  turf: 'nosuch' is not a package in this project's dependency graph
  ```

  With no git dependency at all, `turf update` prints
  `no git dependencies to update`. It needs the network, so `--offline` is not
  accepted.
- If a dependency is added to `turf.toml`, the next `turf build` locks it.
  `turf.lock` is written *before* the compiler runs, so a program that then
  fails to compile still leaves the updated lock behind.

### `--locked` and `--offline`

Both apply to `build`, `run`, `check` and `fetch`.

- **`--locked`**: `turf.lock` must already be exactly what this build needs,
  or it is an error and nothing is written. This is the CI flag.

  ```
  turf: turf.lock is out of date, and --locked forbids changing it (nothing was written):
    added 'extra' (git+…/up/extra.git branch=main)
  hint: run `turf build` or `turf fetch` without --locked, and commit the new turf.lock
  turf: --locked needs turf.lock, and there is none at …/app/turf.lock
  hint: run `turf build` or `turf fetch` once without --locked and commit turf.lock
  ```

- **`--offline`**: run no `git` network command. A dependency that is not both
  locked and cached is an error naming it:

  ```
  turf: dependency 'extra' (…/up/extra.git, branch=main) is not in turf.lock, so it needs the network; run without --offline
  turf: dependency 'jsonx' (…/up/jsonx.git, tag=v1.0.0) is locked (commit 1d0f2d7317cf) but neither its tree nor the commit is cached, so it needs the network; run without --offline
  ```

**`--locked` is not `--offline`.** With a manifest the lock does not cover,
`--locked` alone may still clone or fetch (it fetched `extra` above) before it
reports the difference. A CI check that must not use the network passes both:
`turf build --locked --offline`. The projects in [`examples/`](../../examples/README.md)
build that way against their committed lock files.

## 6. The cache

A git dependency is fetched once into a cache and extracted from there:

```
<cache>/
  git/<16 hex digits>/          one bare mirror per repository URL (all history and refs)
  src/<name>-<full commit>/     the extracted tree a build reads
```

- The cache is `$TURF_CACHE_DIR` if set. Otherwise it is the `turf` directory
  of your platform's user cache directory: on Linux `$XDG_CACHE_HOME/turf`, or
  `~/.cache/turf` if that is unset.
- It is **disposable**: deleting it only costs a re-fetch. Deleting one
  `src/<name>-<commit>/` directory re-extracts it from the mirror, offline: after
  the tampered tree above is deleted, `turf build --offline` rebuilds it.
- `git` must be on `PATH` for every git dependency, cached and locked or not,
  so that a build does not work on one machine and fail on another because of
  what happens to be cached:

  ```
  turf: git dependencies need the 'git' program, which is not on PATH ('lib1' in …/app/turf.toml)
  ```

  **On macOS, `git` is not there until you install it.** `/usr/bin/git` exists
  on every Mac, but it is a placeholder for the Xcode Command Line Tools and
  does nothing without them. Compiling and running Turf programs needs neither
  Xcode nor a C toolchain; git dependencies need a real `git`
  (`xcode-select --install`, or `brew install git`). `turf` tells the two apart:

  ```
  turf: git dependencies need the 'git' program, and /usr/bin/git is only macOS's placeholder for the Xcode Command Line Tools, which are not installed; install git with `xcode-select --install` or `brew install git` ('lib1' in …/app/turf.toml)
  ```

  `turf` runs the system `git` and `turf.lock` records the URL exactly as you
  wrote it in the manifest.
- **The tree is `git archive` output, not a checkout.** A path marked
  `export-ignore` in `.gitattributes` is absent, and a submodule is an empty
  directory (submodules are not fetched). For a repository with both, the
  extracted tree is:

  ```
  TREE/.gitattributes
  TREE/.gitmodules
  TREE/src/lib.tr
  TREE/vendored          (an empty directory: vendored/data.txt is not there)
  ```
  (`src/skip.tr`, marked `export-ignore`, is missing.)

  A package that marks a file it needs `export-ignore`, or depends on a
  submodule, is fine as a checkout and broken as a dependency.
- Every build reads every byte of every git dependency's tree to re-hash it, so
  the cost grows with the dependencies' size.

## 7. Commands

Project commands work anywhere inside a project (they walk up to the nearest
`turf.toml`) and run the compiler from the project root, so `src/main.tr` in a
diagnostic means the same file wherever you typed the command. Outside a
project they say so:

```
$ turf build
turf: not inside a TurfLang project (no turf.toml here or in any parent directory); use 'turf new <name>', or 'turf compile <file.tr>' for a single file
```

That is exit status 2, the status of a usage error; a build or compile failure
is 1.

| Command | What it does |
|---|---|
| `turf new <name> [--lib]` | Create `<name>/` with a manifest, a starter source file and a `.gitignore`. |
| `turf init [--lib] [name]` | The same in the current directory. |
| `turf build [--offline] [--locked] [flags]` | Resolve dependencies, write `build/package-graph.json`, compile. A program becomes `build/<name>`; a library is checked. |
| `turf run [--offline] [--locked] [flags] [-- args]` | `turf build`, then run `build/<name>`, passing the arguments after `--`, in the directory you typed the command in. The exit status is the program's. A library without `src/main.tr` says there is nothing to run and how to add one (section 1; exit 1). |
| `turf check [--offline] [--locked] [flags]` | Every diagnostic, no executable. |
| `turf clean` | Delete `build/`. Nothing to delete is not an error. |
| `turf graph [--project <dir>] [--unsafe] [--offline] [--locked]` | Print the package graph (the JSON `turf build` writes to `build/package-graph.json`) on stdout, including the root's `tests/` when the project has one. Writes nothing (no `turf.lock`, no `build/`) and needs no compiler. This is what an editor asks for, so that `import` there resolves as in `turf build`. `--unsafe` adds each package's unsafe surface (an `"unsafe"` object per package and a top-level `"stdlib"`, section 8); that form needs a compiler, and is a report: `turfc` does not accept it back as a graph. |
| `turf audit [--project <dir>] [--files] [--offline] [--locked]` | Show where the project's unsafe code is, one row per package and the standard library (section 8). Writes nothing. |
| `turf fetch [--offline] [--locked]` | Do everything `build` does up to `turf.lock`, without compiling. Prints one line per git dependency: `jsonx de7262e335ed (tag=v1.0.0)`. Needs no compiler. |
| `turf update [name…]` | Move git dependencies to what their refs point at now (section 5). |
| `turf add <name> --path <dir>` / `--git <url> (--tag\|--branch\|--rev) <ref>` | Add a dependency (section 3). |
| `turf remove <name>` | Remove one. |
| `turf publish` | Reserved. There is no registry in 2.0. |
| `turf test [path...] [--offline] [--locked] [--jobs n] [--timeout s] [--exclude path]` | Compile and run every test under `tests/` and every `@test` function in `src/`, or only the files and directories of `tests/` named; an error if there is no test at all (section 1, *Tests*). |
| `turf fmt`, `turf explain`, `turf migrate`, `turf dump`, `turf compile` | Work on files, in or out of a project ([CLI & Diagnostics](cli_and_diagnostics.md)). `turf fmt --write` with no path formats the project's `src/` and `tests/`. |

**Compiler flags are forwarded.** `turf build -O2`, `turf build -g` and
`turf run -O0` reach the compiler unchanged. **`-o` and `--package-graph` are
refused** in a project, because `turf` owns both; `--offline` and `--locked`
are `turf`'s own and are not forwarded:

```
$ turf build -o x
turf: a project build writes build/<name> itself, so -o is not accepted; use 'turf compile -o …' to name the executable
$ turf build --package-graph x.json
turf: turf writes the package graph itself (build/package-graph.json); --package-graph is not accepted here
```

Every `turf build` recompiles everything, because the compiler has no
incremental mode, and prints the compiler's own `Compiled to …` lines. When a
program does not compile, the first failing compiler status ends the command
and is its exit status. A compiler diagnostic is never rewritten or
re-rendered by `turf`.

`turf publish` and a mistyped command:

```
$ turf publish
turf: the package registry is not part of TurfLang 2.0; publish a library by pushing a git tag (…)
$ turf biuld
turf: unknown command 'biuld'
  did you mean 'build'?
run `turf help` for the list of commands
```

## 8. `unsafe module;` in dependencies

A module that begins `unsafe module;` may use raw pointers (`int*`) outside an
`unsafe { }` block. In a project, a dependency's `unsafe module;` is honored
**only if the root manifest lists that dependency** in `[unsafe] allow`:

```
$ turf build
../ffi/src/rawmod.tr (1 error)

── error at line 3, col 5 [E130]

  Unsafe pointer use!
  Raw pointer type 'int*' cannot be used outside an 'unsafe' block.

  Hint: Wrap this in 'unsafe { }', or use a safe reference ('&int') instead if you don't actually need a possibly-null raw pointer.
```

```toml
[dependencies]
ffi = { path = "../ffi" }

[unsafe]
allow = ["ffi"]
```

With that line the same build compiles.

- **The root package's own modules are honored without listing.** You wrote
  them, so there is nothing to consent to. (A program with its own
  `unsafe module;` builds with no `[unsafe]` table.)
- **Listing is not transitive.** The root lists every package, direct or
  indirect, that needs it.
- **A dependency cannot allow itself.** An `[unsafe]` table in a dependency's
  own `turf.toml` is ignored; the build above still fails with E130.
- **`--allow-unsafe-modules` does not apply in a project.** It is a single-file
  flag, and passing it to `turf build` still fails with the same E130, because
  one global switch would grant every dependency at once, which is the consent
  `[unsafe] allow` makes explicit.
- A name in `allow` that is not in the graph, or the root package's own name,
  is an error:

  ```
  turf: …/turf.toml: [unsafe] allow names 'nope', which is not a package in this project's dependency graph
  hint: list only packages the build actually depends on, directly or transitively
  turf: …/turf.toml: [unsafe] allow lists the root package 'app': its own modules need no listing
  ```

The standard library is always trusted.

### Seeing what you grant: `turf audit`

`turf audit` shows where the unsafe code is in the project and everything it
depends on, so that what `[unsafe] allow` grants is something you can look at:

```
$ turf audit
package   granted  files  unsafe module  unsafe { }  extern fn  C libraries
app       own          1              0           0          0  -
fastmath  no           1              1           0          1  m
ffi       yes          2              2           1          3  z
std       always      25             12           0         75  m

note: 1 file declares `unsafe module;` in 'fastmath', which [unsafe] allow in /work/app/turf.toml does not list: the compiler rejects its raw-pointer uses (E130)
```

One row per package: the project's own first, its dependencies by name, then
the standard library. `files` is how many `.tr` files are under the package's
`src/` (every one is counted, imported or not), then how many of them declare
`unsafe module;`, the `unsafe { }` blocks, the `extern fn` declarations and
the C libraries they name with `extern from`. `granted` says whether the
package's `unsafe module;` is honored: `own` for the project's own package,
`yes` or `no` for a dependency as `[unsafe] allow` says, `always` for the
standard library. `--files` adds a table of each file that has something to
report:

```
$ turf audit --files
...
package   file                    unsafe module  unsafe { }  extern fn  C libraries
fastmath  fastmath.tr             yes                     0          1  m
ffi       ffi.tr                  yes                     1          2  z
ffi       rawmod.tr               yes                     0          1  -
std       collections/hashing.tr  yes                     0          0  -
...
```

The counts come from the compiler (`turfc --unsafe-report`, which reads tokens
with the real lexer, so a keyword in a comment or a string is not counted), so
`turf audit` needs a compiler of driver interface 1.12 or later. It compiles
nothing and writes nothing. `turf graph --unsafe` prints the same as JSON, for
a tool. Native sources join the report with the `[native]` table (LB-1), which
`turf.toml` does not have yet.

## 9. Distributing

- **A library is a git tag.** Push `v0.1.0`; consumers write
  `{ git = "…", tag = "v0.1.0" }`. There is nothing to upload or register, and
  a consumer's `turf.lock` pins the exact commit and tree.
- **A program is one executable**: copy `build/<name>` somewhere. The Turf
  runtime is linked into it statically, and `ldd` on one lists only `libm`,
  `libc` and the loader. How the program was linked depends on your install:
  `turf version` prints a `linker` line. With `embedded-lld` and a
  `bundled-runtime`, a program that uses no C library beyond the C runtime is
  linked without any C toolchain on the machine; with `system-cc`, `turfc` runs
  the machine's `cc` (a build from source).

## 10. Not in 2.0

- A **registry**, semver ranges, `turf publish`. Publish a library by pushing a
  git tag.
- **Workspaces** (several packages in one manifest), **build scripts**, and
  **`subdir`** (a package that lives in a subdirectory of a git repository:
  the repository's root must be the package).
- **Multiple versions of one package** in one build.
- **Separate compilation** and a binary package format: a build compiles every
  module of every package from source. This is what lets a generic function of
  `geometry` be called with a type only `plotter` declares.
- **Renaming a dependency**, per-package compiler flags, and `pub(package)`.
- **`std::`-qualified imports** to spell which candidate an ambiguous import
  means: today the E090 tells you to rename.
- Cross-linking for another machine ([CLI & Diagnostics](cli_and_diagnostics.md#other-targets-and-object-files))
  and a filter for `turf test` that selects tests by name rather than by path.

The internals of module resolution are in
[`docs/architecture/modules_and_scope_resolution.md`](../architecture/modules_and_scope_resolution.md).
