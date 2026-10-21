# Confirmation run: outcome against the pre-registration

Run: `confirm-1.5b`, `confirm-3b`, `confirm-7b` on `corpus/test_fresh.jsonl`
(100 cases, every case scored in every arm). Configuration as frozen in
`PREREGISTRATION.md`; `confirm-logs/00_environment.txt` records the sha256 of
`run_rag.py`, `rag_lib.py`, `analyze.py`, `train.jsonl` and `test_fresh.jsonl`,
and all five match the frozen values. No harness change was made during the run.

## H1 (primary): A3_examples vs A1p_production, final `correct`

| size | A1p | A3 | b (A3 only) | c (A1p only) | exact McNemar p | Holm p |
|---|---|---|---|---|---|---|
| 1.5B | 21 | 43 | 22 | 0 | 4.8e-7 | 1.3e-6 |
| 3B | 34 | 59 | 26 | 1 | 4.2e-7 | 1.3e-6 |
| 7B | 47 | 73 | 29 | 3 | 2.6e-6 | 2.6e-6 |

H1 holds at all three sizes. Retrieved examples beat production docs by
+22, +25 and +26 points.

## H2 (replication): same comparison on `correct@1`

| size | A1p | A3 | b | c | p | Holm p |
|---|---|---|---|---|---|---|
| 1.5B | 18 | 38 | 23 | 3 | 8.8e-5 | 8.8e-5 |
| 3B | 33 | 55 | 25 | 3 | 2.7e-5 | 5.5e-5 |
| 7B | 46 | 71 | 27 | 2 | 1.6e-6 | 4.9e-6 |

H2 holds at all three sizes: the exploratory single-shot result replicates on
unseen cases.

## H3 (exploratory): does compiler feedback substitute for retrieval?

| size | gap at @1 | gap at @3 | change |
|---|---|---|---|
| 1.5B | +20 | +22 | +2 |
| 3B | +22 | +25 | +3 |
| 7B | +25 | +26 | +1 |

The gap holds and grows slightly, so feedback does not substitute for
retrieval. Retries add little in any arm (+1 to +8 cases out of 100), and
the docs arm gains least (+3, +1, +1). Retrieval also halves the number of
cases that need a retry (7B: 27 retried with A3 vs 53 with A1p).

## H4 (exploratory): A7_examples_reretrieve vs A3_examples, final `correct`

| size | A3 | A7 | b (A7 only) | c (A3 only) | p (uncorrected) |
|---|---|---|---|---|---|
| 1.5B | 43 | 44 | 2 | 1 | 1.0 |
| 3B | 59 | 63 | 4 | 0 | 0.125 |
| 7B | 73 | 73 | 1 | 1 | 1.0 |

No detectable effect. Re-retrieving on the new diagnostic is not worth the
extra index lookup at this sample size.

## Descriptive results

- Production docs (`A1p`, about 1,194 context tokens) are indistinguishable
  from no context (`A0`) at every size (p = 0.27, 1.0, 0.38) and roughly double
  the total latency. Examples use about 145 context tokens.
- The gain is in foreign syntax (7B: 38/80 to 63/80). Classic mutations
  (typos, dropped braces, semicolons) barely move (9/20 to 10/20).
- 22 of 100 cases fail in every arm at 7B. The bank holds 7 to 8 training
  entries for each of the never-fixed kinds (`drop_closing_brace`,
  `foreign_import`, `foreign_decl_let`, `foreign_break`), so these failures
  are not a coverage gap in the index.

## Conclusion

The confirmatory run supports the pilot's finding: for Qwen2.5-Coder 1.5B to
7B fixing Turflang compiler errors, about 145 tokens of retrieved
broken-to-fixed examples beat the production documentation prompt, with or
without production's retry loop. As the literature sweep notes, this
replicates known results for unseen languages; it is the baseline for the
language-evolution study, not its contribution.
