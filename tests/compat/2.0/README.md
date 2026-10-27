# tests/compat/2.0/ — the 2.0 compatibility corpus

The 2.0.0 test tree, copied (not moved) from the `v2.0.0` tag, plus the exit
status and output each program that compiled on 2.0.0 gave then. Every 2.x
compiler must still accept those programs and print the same bytes
(`docs/planning/ROADMAP_v3.0.0.md` V-4, 5.5.1, TDP-2-003). **Immutable**: a
minor that edits a file here to make it pass is a 3.0 change.

| File | What it is |
|---|---|
| `NN_topic/**/*.tr` | the 2.0.0 tests, byte for byte (`tests/custom`, `tests/99_known_bugs` and `tests/api` were never part of the suite and are not here) |
| `**/*.tr.expected` | what the program printed on 2.0.0 (stdout and stderr, one stream); absent when it printed nothing |
| `RUNS` | `<exit status> <path>` for every program that compiled and ran on 2.0.0 (134 is an abort) |
| `SHA256SUMS` | the hash of every file above; any edit, addition or removal fails the check |
| `RELAXED.txt` | the register of programs 2.0.0 *rejected* that a later minor deliberately accepts, each with its commit and reason |
| `REWORDED.txt` | the register of programs 2.0.0 *ran* whose output a later minor deliberately rewords: a substitution per line, after which the output must still be 2.0.0's byte for byte |

## Running it

```bash
./scripts/check/compat_corpus_check.py            # default level, what CI runs
./scripts/check/compat_corpus_check.py -O0        # any level, and -g
```

`./scripts/test_runner.sh` does not run this tree (`--exclude compat`); a
separate CI step does, once, so the suite is not run twice at every level.

## Adding a test

Never here. A new test goes under `tests/<NN_topic>/` as always. A future
`tests/compat/2.N/` is a separate frozen tree, made when that minor ships.

## If the check fails

* *edited / added / removed*: restore the file. Do not update `SHA256SUMS`.
* *no longer compiles*, *output changed*, *exit status changed*: the compiler
  broke a program that compiled on 2.0.0. Fix the compiler. If the owner
  decides the change is deliberate and only loosens what 2.0.0 rejected, that
  is a `RELAXED.txt` line. If it only rewords what an accepted program prints
  (a diagnostic or panic message says more, its exit status and everything
  else unchanged) and the owner decides so, that is a `REWORDED.txt` line,
  whose substitution must leave the rest of the output pinned. Any other
  change to what an accepted program does is a 3.0 change.

## Provenance

With the tree copied unedited from `git archive v2.0.0 tests` and a 2.0.0
`turfc` built from that tag (`./scripts/cmake_build.sh --release --no-smart`),

```bash
./scripts/check/compat_corpus_check.py --record --compiler path/to/turfc-2.0.0
git status tests/compat   # must be clean
```

rewrites `RUNS`, `SHA256SUMS` and the `.expected` files to what they already
are. `--record` refuses any compiler whose `--version` is not `turfc 2.0.0`.
