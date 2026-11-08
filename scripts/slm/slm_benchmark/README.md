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

## Scoring

`run_benchmark.py` compiles every case of one split once per path:

| Path | What runs |
|---|---|
| `rules` | the compiler alone: its checked `Fix:` lines, no model |
| `model` | the same compile with `--smart`: a rule fix where there is one, otherwise the model or its cache |

    python3 scripts/slm/slm_benchmark/run_benchmark.py --split train --paths rules,model

What was shown comes from `--diagnostics=json` (each error's `fix` edits,
applied as `--smart-write-fix` applies them) and from the program
`--smart-write-fix` writes for a model fix; stdout is never read. Provenance is
`rule`, `model`, `cache` (the model's answer from the suggestion cache) or
`none`.

| Outcome | Utility |
|---|---|
| a shown fix the oracle accepts | +1 |
| nothing shown | 0 |
| a shown fix the oracle rejects, one that does not compile, or any fix on a hard negative | −K (K = 3, D-12) |

The report gives, per path: the mean utility with a bootstrap 95% interval
(the headline), how many fixes were shown and the share of them that were
wrong (false confidence), the rate of silence on hard negatives, the Brier
score of the confidence printed with each fix (a rule fix counts as 1.0, a
model fix as its post-sandbox confidence from `TURF_SLM_DEBUG=1`), the correct
rate and p50/p95 latency and peak RSS by provenance, the correct rate by Fix
Score tier (`proven`, `verified`, `plausible`; SL-1) with a bootstrap interval, and mean utility by error
category, code and mutation kind. With both paths it adds model − rules with
a paired bootstrap interval, and says whether the two paths' intervals
separate: a change is an improvement only when they do.

Each run writes one JSON record, `bench/results/smart/<date>-<commit>-<split>.json`:
the compiler's `--version --json`, the model file and its SHA-256, the corpus
hash, K, the summaries and every case's outcome. `--no-record` skips it;
`--limit`, `--ids` and `--cases` score a part or another file (the record
says so).

The model path needs exactly one `.gguf` in `models/` and runs one case at a
time; at tens of seconds a case, a split takes hours. `TURF_SLM_REPLAY=<file>`
answers from recorded responses instead. `scripts/slm/slm_benchmark_smoke.sh`
(CI) checks the scorer on four cases with no model.

### Rules-only baseline

| Commit | Split | Cases | Utility | 95% interval | Fixes shown | Wrong | Silent on hard negatives |
|---|---|---|---|---|---|---|---|
| ec03fa91 | train | 446 | −0.038 | [−0.099, +0.018] | 47 | 16 | 5 of 5 |
| ec03fa91 | heldout | 130 | +0.023 | [−0.077, +0.108] | 15 | 3 | 1 of 1 |
| ad291df3 (SL-1) | train | 446 | +0.043 | [+0.004, +0.081] | 35 | 4 | 5 of 5 |
| ad291df3 (SL-1) | heldout | 130 | +0.023 | [−0.077, +0.108] | 15 | 3 | 1 of 1 |

At ec03fa91, 12 of the 16 wrong rule fixes on train were fixes for errors
that follow a parse error (a `;` after `}`, `mut` to `immut`) while the first
error had none. SL-1 gives no fix while the parser recovers, which removed
those 12 and no right fix. The 4 left are two-error cases where the first fix
is right and the second error remains. Every rule fix shown is `proven`.
