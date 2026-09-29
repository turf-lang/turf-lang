# turf, the TurfLang front end

`turf` is the command-line front end of the TurfLang toolchain. It drives
`turfc`, the compiler in this repository's `src/`; both are built from the same
tree, carry the same version (the root `VERSION` file) and ship in the same
release archive.

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
(symlinks resolved), then `$TURF_HOME/bin/turfc`, then `turfc` on `PATH`. Before
driving it, `turf` runs `turfc --version` and refuses a compiler whose version
differs from its own, whichever way it was found.

## Building and testing

```bash
cmake --build build            # builds build/turfc and, if Go is installed, build/turf
cd cli && go vet ./... && go test ./...
./scripts/cli_smoke.sh         # the front end against the real compiler
```

Build it through CMake, not a bare `go build`: CMake stamps the version, and an
unstamped `turf` refuses to run. Go is optional; without it only `build/turf` is
skipped.
