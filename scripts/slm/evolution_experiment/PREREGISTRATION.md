# Language-evolution study: pre-registration

Status: **draft**. The hypotheses, outcomes and analysis below are fixed now,
before any harness exists. The frozen-inputs table is filled in at the freeze
commit (step 4 of `DESIGN.md`, "Order of work"), after the dev-set runs and
before any model sees the confirmatory set. After that commit nothing in this
file changes; a deviation is recorded in a dated "Deviations" section at the
end with its reason, and both analyses are reported.

## Frozen inputs (filled at freeze)

| item | value |
|---|---|
| harness commit | |
| `evolve.py`, `run_rag.py`, `rag_lib.py`, `analyze.py`, `finetune.py` sha256 | |
| train bank per version sha256 | |
| confirmatory set per version sha256 (150 cases, 30 `stale_<id>`) | |
| base models | Qwen2.5-Coder-Instruct 1.5B (primary), 3B, 7B, `q4_k_m`, the pilot's GGUF files |
| adapters (sha256 of each fused GGUF) | |
| platform | Apple M4, Metal, `--gpu-layers 99` |
| decoding | greedy, context budget 1200 tokens |
| patch format, attempts | `text`, 3 |
| versions | `C1t C1a C2t C2a C3t C3a C4t C4a` |
| arms | `B`, `B+N`, `B+R`, `F0`, `F0+N`, `F0+R`, `F'`, `F'+R` |

## Primary outcome and unit

`correct` after up to three attempts on T1, per case. Comparisons are paired
by case within a version and pooled over the eight versions by a stratified
exact test (Mantel-Haenszel-style: discordant pairs summed across versions,
exact binomial on the total), with per-version results reported beside it.

## Hypotheses

Primary family (H1 to H3), Qwen2.5-Coder 1.5B, Holm's method across the three
at family-wise alpha 0.05, two-sided.

**H1 (staleness exists and fine-tuning deepens it).** `stale_rate` of `F0` is
higher than that of `B`, pooled over versions (paired over parsed outputs,
stratified exact test on "stale" per case).

**H2 (retrieval recovers a stale fine-tune).** `F0+R` has a higher `correct`
rate than `F0`.

**H3 (retrieval beats a release note).** `B+R` has a higher `correct` rate than
`B+N`.

Secondary (reported with exact p, no correction, labelled secondary):

**H4 (retrieval vs re-training).** `F0+R` vs `F'`: no direction predicted. A
non-significant result is reported with the 95% interval of the difference, not
as equivalence.

**H5 (direction of change).** The `stale_rate` of `B` and of `F0` is higher on
"away" versions than on "toward" versions (paired by change C1-C4 and case
source, sign of the difference per change; four of four in the predicted
direction is the reported criterion).

**H6 (both).** `F'+R` vs `F'`: retrieval adds to a current fine-tune.

Exploratory (descriptive only): `correct@k` curves; T2 stale rates; effort
curves (`k` to 90% of plateau for `B+R` and `F'`); 3B and 7B; `B+N` vs `B`;
stacked versions; real changes B-16 and B-13; per-kind tables; cost (context
tokens, latency, training minutes).

## Exclusions

None. Every confirmatory case is scored in every arm. An interrupted run
resumes with the same command. A wrapper or harness bug found during the
confirmatory run is fixed in its own commit, every arm of the affected versions
is re-run under a new run name, and both runs are reported.

## Sample size

150 cases per version, 1,200 per arm pooled. The pilot's A3-vs-A1p effect at
1.5B was 22 points with 22 discordant pairs in 100 cases. Assuming 15% of
pairs are discordant (normal approximation to the exact test): with 1,200
pooled pairs a 5-point difference is detected at about 98% power at alpha
0.05/3, and a 4-point one at about 90%; per-version tests (150 pairs, alpha
0.05) detect about 9 points at 80% power.
