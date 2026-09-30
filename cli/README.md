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
| `turf build [flags]` | resolve path dependencies into a package graph (`build/package-graph.json`) and run `turfc --package-graph`; the program goes to `build/<name>`, a library is only checked |
| `turf run [flags] [-- args]` | with no `.tr` argument: `turf build`, then run `build/<name>` |
| `turf check [flags]` | with no `.tr` argument: the same compiler runs as `build`, each with `--check`, and no executable |
| `turf clean` | remove `build/` |
| `turf add <name> --path <dir>` | add a path dependency (one line of `turf.toml`; comments and layout are kept) |
| `turf remove <name>` | delete that dependency's line |

`-o` and `--package-graph` are refused in a project build: `turf` owns both.
Dependencies are `path` dependencies for now. `fetch`, `update` (git
dependencies, the cache and `turf.lock`) are stage 6 of LEC-001 and `test` is
REL-002; each says so and exits 2. `publish` is reserved: there is no registry in
2.0. The pre-2.0 registry-model code (semver resolver, checksums, archives,
`install`/`update`/`publish`) is deleted; it is in git history (`350acd8`).

The manifest is read with `github.com/BurntSushi/toml`, the module's one
dependency. `internal/manifest` validates it (unknown keys are errors) and
edits it as text, never by re-encoding, so a user's comments survive;
`internal/project` builds the graph; `internal/stdlib` lists the stdlib module
names a package may not take.

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
