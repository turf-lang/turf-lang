# turf, the TurfLang front end

`turf` is the TurfLang command-line tool, the one users type. It drives
`turfc`, the compiler in this repository's `src/`, which is the interface for
tools rather than for people. The two are versioned independently: turf's
version is the constant in `internal/version/version.go`, the compiler's is the
root `VERSION` file, and `turf version` prints both. What they must agree on is
the **driver interface**, `major.minor`, which `turfc --version` prints on line
2 and `version.Required` names.

The design is frozen in [`docs/development/FREEZE.md`](../docs/development/FREEZE.md)
§6.16 (F-PKG-1); the record of how it was built is
[`docs/development/changes/LEC-001.md`](../docs/development/changes/LEC-001.md).
The rule that shapes everything here: the compiler owns whatever depends on
TurfLang source, and `turf` owns files, network and reproducibility. `turf`
never parses TurfLang and never rewrites a compiler diagnostic.

## What works today

`turf <file.tr> [flags]` is `turfc <file.tr> [flags]`: stdout, stderr and exit
status are the compiler's, unchanged. Every other argument that starts with `-`
is forwarded the same way.

| Command | Does |
|---|---|
| `turf compile <file.tr> [flags]` | `turfc <file.tr> [flags]`, spelled out; `turf compile --help` lists the flags |
| `turf run <file.tr> [flags] [-- args]` | compile to a temporary executable, run it, delete it; exits with the program's status |
| `turf check <file.tr>` | every diagnostic a compile would print, no output file (`turfc --check`) |
| `turf dump <what> <file.tr>` | `ast`, `resolved`, `typechecked`, `modules`, `symbols` or `instantiations`, no output file |
| `turf fmt [--write] <files...>` | `turfc --format` |
| `turf explain <code>` | `turfc --explain` |
| `turf migrate <name> [--write] <files...>` | `turfc --migrate=<name>` (`pub`) |
| `turf version`, `turf help [command]` | |

### Projects

A project is a directory tree with a `turf.toml` (see `FREEZE.md` §6.16 for the
layout and rules). Run these anywhere inside one; `turf` walks up to the nearest
`turf.toml`.

| Command | Does |
|---|---|
| `turf new <name> [--lib]` | create `<name>/` with `turf.toml`, `src/main.tr` (or `src/<name>.tr`, the root module, with `--lib`) and a `.gitignore` |
| `turf init [--lib] [name]` | the same, in the current directory; never overwrites a source file |
| `turf build [--offline] [--locked] [flags]` | resolve the dependencies (path and git) into a package graph (`build/package-graph.json`), keep `turf.lock` current, and run `turfc --package-graph`; the program goes to `build/<name>`, a library is only checked |
| `turf run [--offline] [--locked] [flags] [-- args]` | with no `.tr` argument: `turf build`, then run `build/<name>` |
| `turf check [--offline] [--locked] [flags]` | with no `.tr` argument: the same compiler runs as `build`, each with `--check`, and no executable |
| `turf clean` | remove `build/` |
| `turf add <name> --path <dir>` | add a path dependency (one line of `turf.toml`; comments and layout are kept) |
| `turf add <name> --git <url> (--tag T \| --branch B \| --rev R)` | fetch and check a git dependency, then add its one line |
| `turf remove <name>` | delete that dependency's line |
| `turf fetch [--offline] [--locked]` | resolve, fetch, extract and verify every git dependency and write `turf.lock`, without compiling (and without needing a compiler) |
| `turf update [name...]` | re-resolve the named git dependencies (all of them with no names) and write the new commits to `turf.lock`; the only command that moves a pin |

`-o` and `--package-graph` are refused in a project build: `turf` owns both, as
it does `--offline` and `--locked`, which are never passed to `turfc`. `test` is
REL-002 and says so and exits 2. `publish` is reserved: there is no registry in
2.0. The pre-2.0 registry-model code (semver resolver, checksums, archives,
`install`/`update`/`publish`) is deleted; it is in git history (`350acd8`).

### Git dependencies, the cache and `turf.lock`

A dependency is `{ path = "..." }` or `{ git = "<url>", tag | rev | branch = "..." }`
(exactly one of the three). The system `git` program does the fetching
(`GIT_TERMINAL_PROMPT=0`, so a private repository fails with git's message
instead of waiting for a password); any URL git accepts works, including a local
path. `turf.lock` sits next to the root `turf.toml`, is written by turf (even for
a project with no git dependency) and is meant to be committed. A dependency's
own `turf.lock` is ignored.

- **The cache** is `$TURF_CACHE_DIR`, else `<user cache dir>/turf`, and is
  disposable. `git/<sha256(url)[:16]>/` is a bare mirror of one URL, and
  `src/<name>-<full commit>/` is the extracted tree of one commit, made with `git
  archive` (so it never holds `.git`) into a temporary directory and renamed into
  place, which is what makes an interrupted fetch harmless.
- **What a build uses**: a lock entry with the same name, URL and ref string is
  used as it is, with no network when its tree is cached. A new dependency, or one
  whose URL or ref changed in the manifest, has its ref resolved (mirror fetched
  first) and pinned. So a tag re-pointed upstream, or a branch that moved, is
  invisible to `build`; only `turf update` moves it.
- **The hash** is the SHA-256 of sorted `<sha256 of content> <f|l> <path>` lines
  over the extracted tree (`internal/treehash`): names and contents only, not
  mtimes, permission bits or empty directories; a symlink counts by its target
  and is never followed; a path holding a newline is refused. **Every build
  re-hashes every cached git tree against the lock**, so a hand-edited file in the
  cache is an error naming the path and how to recover.
- **`--offline`** runs no `clone` or `fetch`: anything not both locked and
  cached is an error saying it needs the network. **`--locked`** demands that the
  lock computed for this build equal the file on disk (each added, removed or
  changed entry is named), and writes nothing; a missing `turf.lock` is an error.
- One version per package name: two requirers of one git package agree only if
  their refs name the same commit; otherwise the error names both requirers and
  both refs. A path dependency inside a git package must stay inside its
  repository and is covered by that package's hash (it has no lock entry of its
  own).

The manifest is read with `github.com/BurntSushi/toml`, the module's one
dependency; everything else is the standard library. `internal/manifest`
validates it (unknown keys are errors) and edits it as text, never by
re-encoding, so a user's comments survive; `internal/project` builds the graph;
`internal/gitdep` fetches and extracts; `internal/treehash` and
`internal/lockfile` define the hash and `turf.lock`; `internal/stdlib` lists the
stdlib module names a package may not take.

## Finding the compiler

First match wins: `$TURF_COMPILER`, then `turfc` beside the running `turf`
(symlinks resolved), then `$TURF_HOME/bin/turfc`, then `turfc` on `PATH`.
`$TURF_HOME/bin` is how a separately installed turf and turfc find each other.
Before driving it, `turf` runs `turfc --version` and refuses a compiler whose
driver interface does not fit: a different major, or an older minor than
`version.Required`. However it was found, a differing *release* version is
fine.

## Building and testing

```bash
cmake --build build            # builds build/turfc and, if Go is installed, build/turf
cd cli && go vet ./... && go test ./...
./scripts/cli_smoke.sh         # the front end against the real compiler
./scripts/project_smoke.sh     # projects: new/add/build/run/check, manifests, graph errors
```

A plain `go build ./cmd/turf` works too, since nothing is stamped at build time.
CMake just puts the result at `build/turf`, beside `build/turfc`, where it finds
the compiler with no setup. Go is optional; without it only `build/turf` is
skipped. When turfc gains something turf will use (a new flag, the stage 4
package graph), turfc bumps its interface minor and turf raises
`version.Required` to it. `Required` is 1.1 because project builds pass
`turfc --package-graph`, which interface 1.1 added; a turfc that only speaks 1.0
is refused before any command runs.
