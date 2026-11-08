# The `--smart` benchmark

What `--smart` is measured on (TDP-2-008: SL-3 corpus, SL-2 scoring).

## The corpus

`build_corpus.py` writes `corpus/train.jsonl`, `corpus/heldout.jsonl` and
`corpus/STATS.md`. Every case is one JSON line: the broken program
(`mutated_source`), what the compiler said (`diagnostic`, `error_code`,
`diag_line`), and its oracle:

| `oracle` | Where it comes from | A fix is right when |
|---|---|---|
| `asserts` | a passing program from `tests/`, `showcase/` or `examples/` with one line mutated (`kind` names the mutator), or a test with two (`multi:<a>+<b>`) | the fixed program prints `expected_stdout` and exits 0: the original's own `test::assert` calls still hold |
| `recompile` | a `fail_*.tr` test, for an error code no mutant reaches (`kind` `fail_test`); or a mutant of a program that compiles but is not run here (it uses the network, or its output varies) | the fixed program compiles with no error the original lacked |
| `abstain` | `hard_negatives/*.tr` (`kind` `hard_negative`): the fix needs a decision only the author can make | nothing is suggested |

Test files are mined in a fixed random order until the candidate pool is
large enough. Every `showcase/` program and every file of an `examples/`
package is mined, up to `--per-program` cases each. A case from a package has
`package` set to the program package's directory (`examples/plotter`):
`mutated_source` replaces `source_test` in it, and the program is built with
`turf build` there (or `turfc --package-graph`). For every other case
`package` is null and `mutated_source` is the whole program.

`STATS.md` lists the error codes the suite expects that no case has: warnings,
errors that need a second module or package, an error with no source line
(E119), and codes a test makes up on purpose.

The split is by source file (sha1 of its path; a quarter is held out), so a
held-out case never has a sibling mutant of the same file in train.

**Prompts, few-shot examples, Fix Score weights and thresholds are tuned on
train only.** `check_heldout.py` (CI) fails if anything under `scripts/slm/`,
`src/slm/` or `docs/slm/` other than `build_corpus.py` and `run_benchmark.py`
names the held-out split.

Regenerate after a change to the compiler's diagnostics or to the mutators:

    python3 scripts/slm/slm_benchmark/build_corpus.py --jobs 2

The run is deterministic for a seed and a compiler (`--seed`, default 20260).
A hard negative is a file in `hard_negatives/` with a `// HARD-NEGATIVE: <why>`
line and a compile error.

`corpus.json` is the original 22-case corpus `mutate.py` writes; the snapshot
INDEX and a few docs refer to cases in it by name.
