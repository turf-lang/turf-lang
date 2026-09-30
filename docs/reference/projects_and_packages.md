# Projects and Packages

A TurfLang **project** is a directory with a `turf.toml`. It holds one
**package**: a named set of modules that can be built into a program, imported
by other packages as a library, or both. `turf` reads the manifest, finds the
package's dependencies (other directories, or git repositories), pins the git
ones in a lock file, and compiles everything, every module of every package,
in one go.

You do not need a project to compile a program. `turf hello.tr` and
`turf compile hello.tr` work on a single file with no manifest, exactly as
before; see [CLI & Diagnostics](cli_and_diagnostics.md). A project is what you
want once a program has more than one file, or uses someone else's code.

Two multi-package projects you can build and run are in
[`examples/`](../../examples/README.md): a library, `geometry`, and a program,
`plotter`, that depends on it.

> [!NOTE]
> Every command, manifest and message on this page was run against the real
> `turf` and copied. In the messages, the absolute paths `turf` prints are
> shortened to `…/`, and a few long hashes to `…`. Diagnostics from the compiler
> say `turf --explain E0NN`, which is what `turfc` prints today; `turf explain
> E0NN` is the same command. (It prints the full write-up only when run from
> the root of a Turf checkout, because it looks for `docs/errors/codes/`
> relative to the current directory; anywhere else it prints the one-line
> description.)

## 1. A project

```
myapp/
  turf.toml          the manifest
  turf.lock          written by turf; commit it
  src/main.tr        present: the package is a program
  src/myapp.tr       present: the package is a library; this is its root module
  src/**.tr          the package's other (internal) modules
  build/             output: the executable and package-graph.json; not committed
```

- **A program** has `src/main.tr`. `turf build` produces `build/<name>`.
- **A library** has `src/<name>.tr`, its **root module**, named after the
  package. It is the only module another package can `import`. A library
  builds no executable; `turf build` still compiles it (as a module, so it
  needs no `main`), so its errors show even when nothing imports it.
- **A package may be both.** With `src/main.tr` and `src/both.tr`, `turf build`
  builds the program and `turf run` runs it; another package can still depend
  on it and import `both`.
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
  deletes it. `tests/` is reserved for `turf test`, which does not exist yet
  (`turf test` says `'test' is not implemented yet (REL-002)`).

### `turf new` and `turf init`

```
$ turf new hello
Created program package 'hello' in hello/
  hello/turf.toml
  hello/src/main.tr
  hello/.gitignore
$ cd hello
$ turf run
Compiled to build/hello.o
Successfully compiled to build/hello
Hello, hello!
```

`turf new geometry --lib` creates a library instead: `src/geometry.tr` in place
of `src/main.tr`. `turf init` does the same in the current directory. Its
name defaults to the directory's, an existing `src/main.tr` is never
overwritten, and `/build/` is added to an existing `.gitignore`:

```
$ turf init
Initialized package 'existing' in …/existing
  turf.toml
  src/main.tr (already there, left as it is)
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

## 2. `turf.toml`

`turf.toml` is read with a real TOML parser. Here is what `turf new` writes:

```toml
[package]
name = "hello"
version = "0.1.0"
turf = "2.0"

[dependencies]
```

There are three tables and no others.

| Table | Key | Meaning |
|---|---|---|
| `[package]` | `name` | **Required.** The package's name, spelled in `import`. It must match `[A-Za-z_][A-Za-z0-9_]*` and not be a reserved word. A library may not be named like a standard-library module. |
| | `version` | **Required.** A string, not interpreted in 2.0. Dependents pin a git tag or commit, not this. |
| | `turf` | Optional. The oldest compiler this package needs, `"<major>.<minor>"`. The major must equal the compiler's, and the minor must not be newer than it. |
| `[dependencies]` | `<name> = { … }` | One entry per dependency (section 3). The key must be the dependency's own package name. |
| `[unsafe]` | `allow = [ "<name>", … ]` | The dependencies whose `unsafe module;` declarations are honored (section 8). |

**An unknown key or table is an error, not ignored.** A silently ignored
setting is how a typo becomes a policy.

```
turf: …/turf.toml:4: unknown key `authors` in [package]: the keys are name, version and turf
turf: …/turf.toml:5: unknown key `build` at the top level: the tables are [package], [dependencies] and [unsafe]
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
turf: package 'app' (…/turf.toml) requires turf "9.0", but the compiler is turfc 2.0.0-dev (…/build/turfc): a different major version
hint: use a turfc 9.x, or change `turf` in that manifest
turf: package 'app' (…/turf.toml) requires turf "2.99", but the compiler is turfc 2.0.0-dev (…/build/turfc), which is older
hint: upgrade turfc, or lower `turf` in that manifest
```

`turf` and `turfc` are versioned separately. `turf version` shows both:

```
$ turf version
turf 1.0.0-dev  (needs turfc interface 1.1+)
turfc 2.0.0-dev  (interface 1.1, …/build/turfc)
```

The `turf` key in the manifest is about the *compiler's* version (`turfc`, the
number after `turfc` above).

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
hint: registry versions are not part of TurfLang 2.0 (FUTURE F-7c); use `{ path = "..." }` or `{ git = "...", tag = "..." }`
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
both requirers; there is no multi-version support:

```
turf: package 'libA' is required from two different directories, and a build has one package per name:
  package 'left' (…/dia/left/turf.toml) needs it at git …/up/libA.git (tag=v1, commit 11343e6dbefb)
  package 'right' (…/dia/right/turf.toml) needs it at git …/up/libA.git (tag=v2, commit 483d19a6a2f2)
```

A diamond that agrees, meaning the same source and the same commit reached by
two routes (two spellings of one commit, `tag = "v1"` and its `rev`, agree), is
one package and builds. Two directories both holding a package called `pc`
is the same error:

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
`import geometry::distanceSquared;` of a dependency's `pub` function works).
The directory of the importing file, the current directory and `lib/usr` are
**not** searched in a project: a `helper.tr` next to `turf.toml`, or a module in
`lib/usr`, is not found by `import helper;`:

```
── error at line 2, col 1 [E027]

  Could not find module 'helper'.

  Hint: Make sure the file 'helper.tr' exists in the standard library or your current directory.
```

(That hint is the single-file wording; in a project, put the file under `src/`.)

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
reports an `E031` ("isn't a known enum, struct, or module") or an `E026` ("has no
function named"); read the first diagnostic, not the ones after it.

Two **different packages** can each have an internal module named `util`, each
declaring a `Node`, without any collision: a module's identity is its package
plus its path, so they are two modules with two distinct types. (A build of
exactly this, `app` with its own `util::Node` and a dependency `shapes` with
its own, prints exit status 21 from `main` as expected.)

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

  Hint: If 'hidden' is meant to be used from other modules, mark it `pub` in geometry.tr: `pub fn hidden(...)`. Otherwise, 'geometry' offers these public functions: distanceSquared, manhattan, pick, sizeOfType.
```

So a package has two levels: private to a module, and `pub`; and `pub` in an
internal module means "visible inside this package".

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
`turf build --locked --offline`. That is what `scripts/examples_smoke.sh` runs
against the committed lock files in `examples/`.

## 6. The cache

A git dependency is fetched once into a cache and extracted from there:

```
<cache>/
  git/<16 hex digits>/          one bare mirror per repository URL (all history and refs)
  src/<name>-<full commit>/     the extracted tree a build reads
```

- The cache is `$TURF_CACHE_DIR` if set. Otherwise it is the `turf` directory
  of your user cache directory: on Linux `$XDG_CACHE_HOME/turf`, or
  `~/.cache/turf` if that is unset. (Checked on Linux with `XDG_CACHE_HOME`; the
  other platforms use their own user cache directory and are not verified.)
- It is **disposable**: deleting it only costs a re-fetch. Deleting one
  `src/<name>-<commit>/` directory re-extracts it from the mirror, offline
  (`turf build --offline` did exactly that after the tampered tree above was
  deleted).
- `git` must be on `PATH` for every git dependency, cached and locked or not,
  so that a build does not work on one machine and fail on another because of
  what happens to be cached:

  ```
  turf: git dependencies need the 'git' program, which is not on PATH ('lib1' in …/app/turf.toml)
  ```

  `turf` runs the system `git` and `turf.lock` records the URL exactly as you
  wrote it in the manifest.
- **The tree is `git archive` output, not a checkout.** A path marked
  `export-ignore` in `.gitattributes` is absent, and a submodule is an empty
  directory (submodules are not fetched). This was checked with a repository
  holding both:

  ```
  TREE/.gitmodules
  TREE/src/lib.tr
  TREE/turf.toml
  TREE/vendored          (an empty directory: vendored/data.txt is not there)
  ```

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
| `turf run [--offline] [--locked] [flags] [-- args]` | `turf build`, then run `build/<name>`, passing the arguments after `--`, in the directory you typed the command in. The exit status is the program's. A library says `turf: 'lb' is a library; there is nothing to run` (exit 1). |
| `turf check [--offline] [--locked] [flags]` | Every diagnostic, no executable. |
| `turf clean` | Delete `build/`. Nothing to delete is not an error. |
| `turf fetch [--offline] [--locked]` | Do everything `build` does up to `turf.lock`, without compiling. Prints one line per git dependency: `jsonx de7262e335ed (tag=v1.0.0)`. Needs no compiler. |
| `turf update [name…]` | Move git dependencies to what their refs point at now (section 5). |
| `turf add <name> --path <dir>` / `--git <url> (--tag\|--branch\|--rev) <ref>` | Add a dependency (section 3). |
| `turf remove <name>` | Remove one. |
| `turf publish` | Reserved. There is no registry in 2.0. |
| `turf test` | Reserved; not implemented yet (REL-002). |

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
turf: the package registry is not part of TurfLang 2.0; publish a library by pushing a git tag (FUTURE F-7c)
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
── error at line 3, col 5 [E066], in file …/ffi/src/rawmod.tr

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
  own `turf.toml` is ignored; the build above still fails with E066.
- **`--allow-unsafe-modules` does not apply in a project.** It is a single-file
  flag, and passing it to `turf build` still fails with the same E066, because
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

## 9. Distributing

- **A library is a git tag.** Push `v0.1.0`; consumers write
  `{ git = "…", tag = "v0.1.0" }`. There is nothing to upload or register, and
  a consumer's `turf.lock` pins the exact commit and tree.
- **A program is one executable**: copy `build/<name>` somewhere. The Turf
  runtime is linked into it statically, and `ldd` on an example lists only
  `libm`, `libc` and the loader. Today `turfc` still links with the system C
  toolchain; a fully static program and a compiler that needs no C toolchain
  are REL-008.

## 10. Not in 2.0

- A **registry**, semver ranges, `turf publish` (FUTURE F-7c). The pre-2.0
  `turf-cli` registry prototype was deleted and is in history at `350acd8`.
- **Workspaces** (several packages in one manifest), **build scripts**, and
  **`subdir`** (a package that lives in a subdirectory of a git repository:
  the repository's root must be the package).
- **Multiple versions of one package** in one build.
- **Separate compilation** and a binary package format (FUTURE F-7a/F-7b): a
  build compiles every module of every package from source. This is what lets a
  generic function of `geometry` be called with a type only `plotter` declares.
- **Renaming a dependency**, per-package compiler flags, and `pub(package)`.
- **`std::`-qualified imports** to spell which candidate an ambiguous import
  means (tracked as GitHub issue #264): today the E090 tells you to rename.
- `turf test` (REL-002), and a download-and-run installer (REL-008).

## Design record

The rules here are frozen in [`docs/development/FREEZE.md`](../development/FREEZE.md)
§6.16 (F-PKG-1 to F-PKG-4); how they were built, stage by stage, and what was
found on the way, is in
[`docs/development/changes/LEC-001.md`](../development/changes/LEC-001.md).
The internals of module resolution are in
[`docs/architecture/modules_and_scope_resolution.md`](../architecture/modules_and_scope_resolution.md).
