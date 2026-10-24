# IF-5 — Consumer fixtures run in the compiler's CI

| | |
|---|---|
| **Status** | landed |
| **Dates** | 2026-10-24 |
| **Commits** | (filled in at commit time) |
| **Sessions** | 1 of 1 |
| **Depends on** | the query facade (`include/turf/Analysis.h`, SMP-001), the driver-interface handshake (F-PKG-1, `FREEZE.md` §6.16) |
| **Frozen decisions relied on** | none new; the ordering rule "a consumer follows the compiler, never the reverse" of `docs/planning/ROADMAP_v3.0.0.md` IF-5 |

Issue #293. The roadmap row is IF-5 in `docs/planning/ROADMAP_v3.0.0.md`.

## Why

`turf`, `turf-lsp` and `turf-fuzz` live in their own repositories and depend on
the compiler: `turf` on the driver interface `turfc --version` prints, `turf-lsp`
on the query facade and the prelude, `turf-fuzz` on the diagnostics and on the
meaning of programs. Each consumer tests against a compiler release and a
nightly build of the compiler, so a compiler change that breaks one shows up
there, a day later, as a red build on an unrelated push. The compiler's own CI
runs the consumers' fixture tests at the consumers' default branch, so the
change that breaks a consumer is the change that goes red.

## Language semantics

None.

### Is this breaking?

No. It adds a check; no program, flag or interface changes.

## Compiler architecture

None. The pieces are a script and workflow steps.

- **`scripts/check/consumer_fixtures.sh`** takes the built `build/turfc` and the
  three checkouts (`TURF_CLI_DIR`, `TURF_LSP_DIR`, `TURF_FUZZ_DIR`, by default
  `../turf-cli`, `../turf-lsp`, `../turf-fuzz`) and runs each consumer's own
  commands:
  - turf: `go vet ./... && go test ./...` in turf-cli, then the `turf` built from
    that checkout runs against `build/turfc` (`turf --version` passes the
    driver-interface handshake; `turf hello.tr` compiles a program that then runs).
  - turf-lsp: `cmake -DTURF_LANG_DIR=<this tree>` builds the server against this
    tree's `turfcore` and facade; then `turf-lsp-tests`, `scripts/check_grammar.py`
    and `scripts/smoke.py --stdlib-from <this tree>` (the real server over stdio,
    with `TURF_HOME` unset, beside this tree's `lib/std`).
  - turf-fuzz: `oracles/soundness.py` with `TURF_ROOT` set to this tree, the
    oracle whose verdicts are pinned (the F-MEM-5 escape boundary). The random
    oracles are the fuzzer's nightly and are not fixtures.
- It prints one `PASS`, `FAIL` or `SKIP` line per consumer and exits 1 if any
  failed. A missing checkout is a `FAIL` when `CI` is set and a printed `SKIP`
  otherwise. `CONSUMERS="cli lsp fuzz"` selects a subset; `CONSUMER_LOGS` is the
  directory the per-consumer logs go to.
- **`.github/workflows/test.yml`**, job `build_and_test`: three `actions/checkout`
  steps (`consumers/turf-cli`, `consumers/turf-lsp`, `consumers/turf-fuzz`, each
  at the default branch) and the step that runs the script after the compiler is
  built. The logs upload as an artifact when it fails. This is separate from the
  `turf-cli` checkout at `TURF_CLI_REVISION`, which is the front end this commit
  ships and which the smoke scripts test.
- No PowerShell twin: the job runs on Linux, and the script is in `scripts/check/`,
  which has no twins (only `scripts/smoke/` does).

## Runtime changes

None.

## Standard-library changes

None.

## New invariants

A compiler change that breaks a consumer's fixture at its default branch does not
merge green.

## Removed assumptions

- A consumer only finds out about a compiler change from its own CI.

## Tests

The script is the test. Run locally against the three checkouts it passes; with
`TurfcInterfaceMajor` in `src/driver/main.cpp` raised by one it fails the `turf`
line (the handshake refuses the compiler), and with a facade behaviour changed it
fails the `turf-lsp` line.

## Known pitfalls

- The turf-lsp build compiles the compiler's `turfcore` a second time, in
  `build/consumers/turf-lsp`, because it links the sources and not the finished
  `build/` objects. `CC`/`CXX` set to `ccache` make a rerun fast.
- The turf-lsp configure fetches nlohmann_json, so it needs the network.
- A consumer's default branch can be red for its own reasons. The failure is then
  that repository's to fix; the log (`CONSUMER_LOGS`) shows which step failed.
- `turf-lsp` targets one LLVM major version. The job's LLVM must be the one
  turf-lsp's `find_package(LLVM)` accepts.
- A consumer that adds a fixture command (a new script under `scripts/`) is not
  run until the script lists it.

## Handbook impact

None.

## Unresolved questions

- turf-fuzz has no unit-style fixtures; the soundness oracle is the nearest
  deterministic one. Its IF-2 schema checks join here once the schemas exist.

## Related records

[TDP-2-003](TDP-2-003.md)
