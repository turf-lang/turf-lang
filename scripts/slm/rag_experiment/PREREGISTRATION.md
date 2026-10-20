# Confirmation run: pre-registration

This file fixes the hypotheses, the analysis and the run configuration of the
confirmation run on `corpus/test_fresh.jsonl` before any model sees that set.
The earlier test set (`corpus/test.jsonl`) was read during harness development,
so its results are exploratory; this run is the confirmatory one.

## Frozen inputs

| item | value |
|---|---|
| harness | the commit that adds this file; `run_rag.py` sha256 `29128875fca9087fc5b7ccfd9caeb50b38e54426d116863a64a8dbc8419cd51d`, `rag_lib.py` sha256 `ba1a8a5c7656db724dd937cea78d2a4e34336dd113145b191cb7fa5ad77e247a`, `analyze.py` sha256 `bb3b943f941c65c117731e69edff012658adec79f3420914cbdc7a17fa09f739` |
| `corpus/train.jsonl` sha256 (retrieval bank, 320 entries) | `1e42e5537be1dad7abb947bbd7d32c07c0c44f2472c2caf483266c7fe1cdeef6` |
| `corpus/test_fresh.jsonl` sha256 (100 entries, 81 source files, none shared with train or the earlier test set) | `e3eca1493c5502818eba4714bd4a2d9ce6aa39653f7ae513b4ac8e5cc24b5aef` |
| models | Qwen2.5-Coder-Instruct 1.5B, 3B, 7B, `q4_k_m` GGUF, the same files (sha256 in the meta files) as the exploratory runs |
| platform | Apple M4, Metal, `--gpu-layers 99` |
| decoding | greedy, context budget 1200 tokens |
| patch format | `text` (production's `original_code` / `new_code` matching) |
| attempts | 3, with production's `previous_attempt_failed` feedback |
| arms | `A0_none`, `A1p_production`, `A3_examples`, `A7_examples_reretrieve` |

No change to the harness, prompts, retrieval, corpus or scoring happens
between the freeze commit and the end of the run. If a harness bug is found
during the run, the fix is committed separately, every arm is re-run under a
new run name, and both runs are reported.

## Hypotheses

**H1 (primary).** `A3_examples` has a higher final `correct` rate (after up to
three attempts) than `A1p_production`. Tested at each model size with a
two-sided exact McNemar test on paired cases; the three p-values are corrected
with Holm's method at family-wise alpha 0.05. H1 holds at a size when its
corrected p < 0.05 and A3 wins more discordant pairs.

**H2 (replication).** The same comparison on `correct@1` (the first attempt
only), same test and correction. This is the single-shot result of the
exploratory runs, on unseen cases.

**H3 (exploratory: retrieval and compiler feedback).** For each size, the gap
`(A3 − A1p)` at `correct@3` minus the gap at `correct@1`. A gap that shrinks
means compiler feedback substitutes for retrieval; one that holds or grows
means they complement each other. Reported descriptively with the counts of
cases rescued by a retry per arm; no direction is predicted.

**H4 (exploratory).** `A7_examples_reretrieve` vs `A3_examples` on final
`correct`, exact McNemar, uncorrected, reported as exploratory.

Also reported, not tested: `A0_none` per size, `correct@k` curves, mean model
calls and total latency per arm, and the foreign-syntax / classic split.

## Exclusions

None. Every entry of `test_fresh.jsonl` is scored in every arm. A run that is
interrupted is resumed with the same command; a case is never dropped.

## Commands

```
python3 scripts/slm/rag_experiment/run_rag.py --split test_fresh --patch-format text \
  --attempts 3 --arms A0_none,A1p_production,A3_examples,A7_examples_reretrieve \
  --model models/<model>.gguf --run-name confirm-<size> \
  --llama-server "$(which llama-server)" --gpu-layers 99 --threads 4
python3 scripts/slm/rag_experiment/analyze.py confirm-<size>
```
