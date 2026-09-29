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

The project commands (`new`, `init`, `build`, `test`, `clean`, `fetch`, `add`,
`remove`, `update`) have their names reserved but are not implemented; each says
which stage of LEC-001 builds it. `publish` is reserved: there is no registry in
2.0. The pre-2.0 registry-model command files (`cmd/turf/{init,install,add,
remove,update,publish}.go`) are kept unused for stages 5 and 6 to rework.

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
```

A plain `go build ./cmd/turf` works too, since nothing is stamped at build time.
CMake just puts the result at `build/turf`, beside `build/turfc`, where it finds
the compiler with no setup. Go is optional; without it only `build/turf` is
skipped. When turfc gains something turf will use (a new flag, the stage 4
package graph), turfc bumps its interface minor and turf raises
`version.Required` to it.
