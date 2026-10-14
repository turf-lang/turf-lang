# Working on a TurfLang project

This guide follows one program, `walk`, from `turf new` to a release build. You
create it, split it into modules, give it a dependency, keep it formatted, read
its diagnostics, and build it for debugging and for release. Each step uses
commands you will type every day.

`walk` follows a list of steps along a line and reports where the walker ends,
how far from the start it ever got, and how that compares with the point it
started from. It is small on purpose: the commands matter, not the program.

You need `turf` on your `PATH` (see the README for installing it). Git
dependencies, covered below, also need `git`. Every rule this guide mentions is
stated in full in [Projects and packages](../reference/projects_and_packages.md);
the flags are in [CLI and diagnostics](../reference/cli_and_diagnostics.md).

## Create the project

```text
$ turf new walk
Created program package 'walk' in walk/
  walk/turf.toml
  walk/src/main.tr
  walk/README.md
  walk/.vscode/extensions.json
  walk/.gitignore
$ cd walk
$ turf run
Compiled to build/walk.o
Successfully compiled to build/walk
Hello, TurfLang! This is walk.
```

Every package runs straight away. `turf init` does the same in a directory that
already exists, and never overwrites a file you have there:

```text
$ cd sketch
$ turf init
Initialized package 'sketch' in /home/me/code/sketch
  turf.toml
  src/main.tr (already there, left as it is)
  README.md
  .vscode/extensions.json
  .gitignore (added /build/)
```

The name is spelled in `import`, so it must be a plain identifier: `turf new
my-app` is refused, and so is a library named like a standard-library module.

## The layout

```text
walk/
  turf.toml       the manifest: name, version, dependencies
  turf.lock       written by turf; commit it
  src/main.tr     the entry point: what `turf run` runs
  build/          output; not committed
  README.md
  .gitignore
```

`turf.toml` is short. The generated one has no dependencies:

```toml
[package]
name = "walk"
version = "0.1.0"
turf = "2.0"

[dependencies]
```

`turf = "2.0"` is the oldest compiler the package needs. `version` is yours to
bump; `turf` does not interpret it.

`src/main.tr` is the package's one program. Replace it with the real thing.
This version needs no other file:

```turflang
// src/main.tr: this package's entry point. `turf run` builds and runs it.
import io;

fn main(): int {
    mut steps: int[5] = [3, -8, 4, -1, 5];
    mut position: int = 0;
    mut farthest: int = 0;
    mut i: int = 0;
    while i < steps.length {
        position = position + steps[i];
        farthest = max(farthest, abs(position));
        i = i + 1;
    }
    io::printline("ended at " + (position to string));
    io::printline("farthest from the start: " + (farthest to string));
    return 0;
}
```

## Run, build and check

Three commands cover most of a day:

| Command | What it does |
|---|---|
| `turf run` | Builds, then runs `build/walk`. The exit status is the program's. |
| `turf build` | Builds `build/walk` and stops. |
| `turf check` | Reports every error and warning and writes no executable. |

```text
$ turf run
Compiled to build/walk.o
Successfully compiled to build/walk
ended at 3
farthest from the start: 5
$ ./build/walk
ended at 3
farthest from the start: 5
```

`turf check` prints nothing when the code is clean, and exits 0. Use it as the
loop while you write: it reports everything a build would, and leaves no
executable behind.
Pass program arguments after `--`: `turf run -- first second`.

All of these work from any directory inside the project; `turf` walks up to the
nearest `turf.toml`. Every build compiles the whole package again from source.

## Split the code into modules

Any `.tr` file under `src/` other than `main.tr` is a module of the package.
Import it by its bare file name, and mark what other files may use with `pub`.
Put the walking in `src/route.tr`:

```turflang
// A module of this package: `import route;` finds src/route.tr by its name.

// Not `pub`: only this file can call it.
fn follow(steps: int[], upto: int): int {
    mut position: int = 0;
    mut i: int = 0;
    while i < upto {
        position = position + steps[i];
        i = i + 1;
    }
    return position;
}

pub fn end_position(steps: int[]): int {
    return follow(steps, steps.length);
}

pub fn farthest(steps: int[]): int {
    mut best: int = 0;
    mut i: int = 1;
    while i <= steps.length {
        best = max(best, abs(follow(steps, i)));
        i = i + 1;
    }
    return best;
}
```

Modules can live in subdirectories. The directory is only for your own
tidiness: it is not part of the module's name. Put the output formatting in
`src/util/report.tr`:

```turflang
// src/util/report.tr is imported as `report`: the directory is not part of
// a module's name.

pub fn line(label: string, value: int): string {
    return label + ": " + (value to string);
}
```

The layout:

```text
walk/
  turf.toml
  turf.lock
  src/
    main.tr
    route.tr
    util/
      report.tr
```

`main.tr` imports both modules and calls them with `route::` and `report::`:

<!-- doc-example: skip imports modules of the walk project -->
```turflang
// src/main.tr: this package's entry point. `turf run` builds and runs it.
import io;
import route;
import report;

fn main(): int {
    mut steps: int[5] = [3, -8, 4, -1, 5];
    io::printline(report::line("ended at", route::end_position(steps)));
    io::printline(report::line("farthest", route::farthest(steps)));
    return 0;
}
```

```text
$ turf run
Compiled to build/walk.o
Successfully compiled to build/walk
ended at: 3
farthest: 5
```

Three things to remember:

- **Names are unique.** Two files called `report.tr` anywhere under `src/` make
  `import report;` ambiguous (E090), and so does a module named like a
  standard-library module (`list.tr`, `json.tr`). Rename one file; the error
  says which two answer to the name.
- **Everything is private unless it says `pub`.** That includes struct fields.
  Calling `route::follow` from `main.tr` is an error that names the function and
  lists what `route` does offer (see the next sections).
- **A file next to `turf.toml` is not a module.** `import helper;` looks in
  `src/`, your dependencies and the standard library, and nowhere else.

## Keep the code formatted

`turf fmt` fixes indentation, trailing spaces, runs of blank lines and the final
newline, and changes nothing else. Inside a project it needs no path: it covers
`src/` and `tests/`.

```text
$ turf fmt --check
would reformat src/main.tr
Would reformat 1 file; 2 already formatted.
$ turf fmt --write
Reformatted 1 file; 2 already formatted.
$ turf fmt --check
Would reformat 0 files; 3 already formatted.
```

`--check` changes nothing and exits 1 when a file would change, so it is the
form for CI. `--write` rewrites the files that differ. Run `--write` before you
commit and the check never fails.

## Read the diagnostics

When the code is wrong, `turf build`, `turf run` and `turf check` print the
compiler's diagnostic and exit 1. Calling a private function from another module
looks like this:

```text
$ turf check
src/main.tr (1 error)

── error at line 12, col 51 [E088 · declaration]

  Function 'follow' is private to module 'route'.
  Note: 'follow' is declared in module 'route' without `pub` (line 4, col 4)

  Hint: If 'follow' is meant to be used from other modules, mark it `pub` in route.tr: `pub fn follow(...)`. Otherwise, 'route' offers these public functions: end_position, farthest.

  12 │     io::printline(report::line("farthest", route::follow(steps, 2)));
     │                                                   ^~~~~~ Here!

  Worth reading in full -- run `turf --explain E088` before you patch this one.
```

Read it from the top. The header gives the file, the position and the error
**code**, `E088`. The first lines say what is wrong, the hint says what to do,
and the source line shows where. Paths are relative to the project root, wherever
you typed the command.

The code is how you get more. `turf explain E088` prints a page for that error:
what it means, a wrong program and the fix.

```text
$ turf explain E088
E088  PrivateSymbolAccessError (DECLARATION)

A name you used belongs to another module, which did not mark it `pub`

    // util.tr
    pub fn api(): int { return helper() + 1; }
    fn helper(): int { return 41; }
    ...
```

A *warning* does not stop the build or change the exit status, but read it. An
unused variable is reported like this, and the build goes ahead:

```text
$ turf check
src/main.tr (1 warning)

── warning at line 11, col 5

  Warning: Variable 'unused' is declared but never used.

  Hint: If the variable is intentionally unused, prefix its name with '_' by convention. Otherwise, remove the declaration or use its value.

  11 │     mut unused: int = 0;
     │     ^ Here!
```

One mistake can cause others below it (a failed import makes every later use of
the module fail), and a line reports only one error. Fix the first error and run
`turf check` again before chasing the rest.

## Add a dependency on a local package

A dependency is another package: a directory with its own `turf.toml` and a
**root module** named after it. The TurfLang repository's `examples/` directory
has one, `geometry`, with a `Point` type and distance functions. Copy it beside
`walk`:

```text
code/
  geometry/
  walk/
```

Add it:

```text
$ turf add geometry --path ../geometry
Added dependency 'geometry' (../geometry) to /home/me/code/walk/turf.toml
```

`turf add` edits one line of `turf.toml` and leaves your comments and key order
alone:

```toml
[dependencies]
geometry = { path = "../geometry" }
```

The path is relative to `turf.toml`. A path dependency is live source: `turf`
reads it as it is on disk, so an edit to `geometry` shows up in the next build.

Import it by the package name, and use what its root module makes `pub`. Update
`main.tr` to measure how far the walker is from where it began:

<!-- doc-example: skip imports modules of the walk project and the geometry package -->
```turflang
// src/main.tr: this package's entry point. `turf run` builds and runs it.
import io;
import geometry;
import route;
import report;

fn main(): int {
    mut steps: int[5] = [3, -8, 4, -1, 5];
    mut start: geometry::Point = geometry::Point::new(0, 0);
    mut finish: geometry::Point = geometry::Point::new(route::end_position(steps), steps.length);
    io::printline(report::line("ended at", finish.x));
    io::printline(report::line("farthest", route::farthest(steps)));
    io::printline(report::line("manhattan", geometry::manhattan(start, finish)));
    return 0;
}
```

```text
$ turf run
Compiled to build/walk.o
Successfully compiled to build/walk
ended at: 3
farthest: 5
manhattan: 8
```

`geometry` has its own internal module, `vecmath`. You cannot import it from
`walk`, whatever it marks `pub`: only a package's root module is visible outside
it.

```text
$ turf check
src/main.tr (1 error)

── error at line 3, col 8 [E091 · declaration]

  Module 'vecmath' is internal to package 'geometry'.
    /home/me/code/geometry/src/vecmath.tr

  Hint: Only a package's root module can be imported from outside it. Import 'geometry' and use what its root module makes `pub`; if 'geometry' should offer this, its root module has to expose it.

  3 │ import vecmath;
    │        ^~~~~~~ Here!
```

`turf.lock` lists the dependency:

```toml
# This file is generated by turf. Commit it; do not edit it.
version = 1

[[package]]
name = "geometry"
source = "path+../geometry"
```

`turf remove geometry` takes the line out again.

## Depend on a git repository

A package that lives in a git repository is a dependency too. Put the package
in a repository, tag a release, and name the repository and the tag. Here
`geometry` is a repository on disk with a `v0.1.0` tag; for a published one the
value is an `https://` URL. Replace the path dependency:

```text
$ turf remove geometry
Removed dependency 'geometry' from /home/me/code/walk/turf.toml
$ turf add geometry --git /home/me/code/geometry --tag v0.1.0
turf: fetching geometry (/home/me/code/geometry)
Added dependency 'geometry' (git /home/me/code/geometry, tag=v0.1.0, commit 3d89a0bef802) to /home/me/code/walk/turf.toml
```

```toml
[dependencies]
geometry = { git = "/home/me/code/geometry", tag = "v0.1.0" }
```

A git dependency needs exactly one of `tag`, `rev` (a commit) or `branch`. Use a
tag for a release. A `branch` follows later commits, but only when you ask.

The next build fetches the repository into a cache that is shared by all your
projects, and writes what it found into `turf.lock`:

```text
$ turf build
Compiled to build/walk.o
Successfully compiled to build/walk
$ cat turf.lock
# This file is generated by turf. Commit it; do not edit it.
version = 1

[[package]]
name = "geometry"
source = "git+/home/me/code/geometry"
ref = "tag=v0.1.0"
commit = "3d89a0bef8023da0081bfd2bf15d580318232a04"
hash = "sha256:f3d5bdd412caae6729869efda772df3510ad040f24ef0c104ae90299512cd4d3"
```

The lock records the exact commit and a hash of the files. **Commit
`turf.lock`.** It is what makes your build the same on every machine and every
day: the hash is checked on every build, so a changed or corrupted cached copy
is an error instead of a silent change, and a tag that someone re-points
upstream does not move your build.

Only `turf update` moves a pin. It re-reads each `tag` or `branch` and reports
what changed:

```text
$ turf update
turf: fetching geometry (/home/me/code/geometry)
geometry unchanged
```

`turf update geometry` updates just one dependency. When a branch has moved the
line reads `geometry 3a093a8854ce -> ee8f9cfaaba6`; review the diff of
`turf.lock` and commit it. To move a tag-pinned dependency to another release,
change the tag in `turf.toml` (for example `v0.1.0` to `v0.2.0`) and run `turf
build`, which locks the new commit.

`turf fetch` does the part of a build that needs the network, without compiling:

```text
$ turf fetch
geometry 3d89a0bef802 (tag=v0.1.0)
```

A package can import its own modules, its **direct** dependencies and the
standard library. A dependency's own dependencies are built but are not in your
namespace: to import one, list it in your `turf.toml` too. And a build has one
package per name, so two dependencies that need different versions of the same
package are an error that names both of them.

## Build in CI

A CI job should fail when something is out of date, never repair it. Two flags
on `build`, `run`, `check`, `fetch` and `test` do that:

| Flag | What it enforces |
|---|---|
| `--locked` | `turf.lock` must already be exactly what the build needs. Otherwise it is an error and nothing is written. |
| `--offline` | Run no `git` network command. A dependency that is not both locked and cached is an error. |

`--locked` is not `--offline`: with a manifest the lock does not cover, `--locked`
alone may still reach the network before it reports the difference. Give both
when a step must stay off the network. A fresh checkout has an empty cache, so
fetch once first:

```text
$ git clone /home/me/code/walk ci && cd ci
$ turf build --locked --offline
turf: dependency 'geometry' (/home/me/code/geometry, tag=v0.1.0) is locked (commit 3d89a0bef802) but neither its tree nor the commit is cached, so it needs the network; run without --offline
$ turf fetch --locked
turf: fetching geometry (/home/me/code/geometry)
geometry 3d89a0bef802 (tag=v0.1.0)
$ turf build --locked --offline
Compiled to build/walk.o
Successfully compiled to build/walk
```

A reasonable job runs these in order:

```text
turf fetch --locked
turf fmt --check
turf build --locked --offline
turf test --locked --offline
```

If you edit `turf.toml` and forget to commit the new `turf.lock`, the `--locked`
step fails and says what changed (here the tag was moved from `v0.1.0` to
`v0.2.0`):

```text
turf: turf.lock is out of date, and --locked forbids changing it (nothing was written):
  changed 'geometry': ref tag=v0.1.0 -> tag=v0.2.0; commit 3d89a0bef8023da0081bfd2bf15d580318232a04 -> 8da85a9b82ec1bae44c8412654ac0443618233ff; hash sha256:f3d5bdd412caae6729869efda772df3510ad040f24ef0c104ae90299512cd4d3 -> sha256:96c5441d33cb030f985dc9aabaffa9f0571b8a31a2b578c29a306a0ce2619e86
hint: run `turf build` or `turf fetch` without --locked, and commit the new turf.lock
```

`turf test` runs every `.tr` file under `tests/`; see [Testing](testing.md).
Name a test file differently from every module under `src/`, or the test's own
`import` becomes ambiguous.

## Build for debugging and for release

`turf build` and `turf run` hand compiler flags to the compiler unchanged. Two
decide what you get: the optimization level and debug information.

| Flag | Meaning |
|---|---|
| `-O0` `-O1` `-O2` `-O3` `-Os` | How much the optimizer does. `-O2` is the default; `-Os` favors small code. A level that is not on the list is an error. |
| `-g` | Adds DWARF debug information. It never changes what the program does. |

There are no profiles in `turf.toml`; you choose per command. Two combinations
are worth knowing:

- **Release:** `turf build`, or `turf build -O3`. This is `-O2` without
  debug information, and it is what you copy or ship. The program is one
  executable, `build/walk`, with the Turf runtime linked in.
- **Debugging:** `turf build -O0 -g`. At `-O0` nothing is moved or removed, so
  a debugger steps through your lines in order and every variable is there.

On one x86-64 Linux machine `walk` is about 91 KB at `-O0`, 111 KB at `-O0 -g`
and 38 KB at `-O2`. Neither the level nor `-g` changes what a program prints
or returns.

With a debug build, a breakpoint takes the module's file name and a line, and
the backtrace shows your function names across modules:

```text
$ turf build -O0 -g
$ gdb -q build/walk
(gdb) break route.tr:8
Breakpoint 1 at 0x9e15: file route.tr, line 8.
(gdb) run
Breakpoint 1, follow (steps=..., upto=5) at route.tr:8
8	        position = position + steps[i];
(gdb) print position
$1 = 0
(gdb) continue
Breakpoint 1, follow (steps=..., upto=5) at route.tr:8
8	        position = position + steps[i];
(gdb) print position
$2 = 3
(gdb) backtrace
#0  follow (steps=..., upto=5) at route.tr:8
#1  0x000055555555df30 in end_position (steps=...) at route.tr:15
#2  0x000055555555e0f1 in main () at main.tr:10
#3  0x000055555555e4f5 in main ()
```

The last frame is the program's entry point, which wraps `main`. `lldb` works
too. The details are in [CLI and
diagnostics](../reference/cli_and_diagnostics.md#debugging-with--g).

## Clean up, and what to commit

`turf clean` deletes `build/`. It is the only thing it touches, and a project
with nothing to delete is not an error. `build/` holds the executable and
`package-graph.json`, which `turf` writes on every build; all of it is
regenerated by the next build.

`turf new` writes a `.gitignore` with one line:

```text
/build/
```

That is all a project needs. Commit everything else:

| Commit | Why |
|---|---|
| `turf.toml` | The manifest. |
| `turf.lock` | The pins. A fresh clone builds exactly what you built. |
| `src/`, `tests/` | The code. |
| `README.md`, `.vscode/extensions.json` | Optional; `.vscode/extensions.json` suggests the editor extension to everyone who opens the project. |

The cache of fetched git dependencies lives outside the project, in your user
cache directory (`~/.cache/turf` on Linux, or `$TURF_CACHE_DIR`). It is safe to
delete; the next build fetches again.

## Where next

- [Writing a library](libraries.md): make `geometry`, or a package like it, and
  publish it as a git tag.
- [Testing](testing.md): `turf test` and the `test` module.
- [Projects and packages](../reference/projects_and_packages.md): every rule and
  error message on this page, in full.
