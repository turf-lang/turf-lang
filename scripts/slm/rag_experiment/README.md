# RAG experiment: does retrieval help a small model fix Turf compile errors?

Turflang is a language the model has never seen in training. The question is
whether giving `qwen2.5-coder-1.5B` (Q4_K_M) retrieved documentation or
retrieved worked examples lets it repair a broken program better than the
production `--smart` behaviour (one whole documentation file chosen by error
category), and whether any gain comes from relevance or only from seeing any
Turf at all.

## Task

Each corpus case is a passing Turf program with one line mutated so that it no
longer compiles. The model sees the numbered program and the compiler's
diagnostic and must answer with a single JSON object, enforced by a JSON
schema on the server:

```
{"line": <int>, "new_code": <string>}      replace line `line` with `new_code`
{"line": 0,     "new_code": ""}            no fix
```

Decoding is greedy (`--temp 0`, `top_k 1`, `seed 0`). Compiler `Fix:` blocks
and the `--explain` pointer are removed from the diagnostic before it is shown.
Programs over 1300 tokens are cut to a window around the diagnostic line, the
same window in every arm.

## Corpus

`corpus/{train,test}.jsonl`, one object per line: `id, split, source_test, kind,
line, original_line, mutated_line, mutated_source, diagnostic, error_code,
error_message, diag_line, expected_stdout, expected_exit`.
`fixture/` holds a tiny hand-built corpus for smoke tests
(`python3 make_fixture.py` rebuilds it from `scripts/slm/slm_benchmark/corpus.json`).

## Arms

The user prompt is identical across arms except for its CONTEXT block. Every
context block is capped at the same budget (`--budget`, default 1200 tokens,
measured with the server's `/tokenize`).

| arm | CONTEXT |
|---|---|
| `A0_none` | nothing |
| `A1_category` | production routing: the error code gives the category (the `AllErrorCodes` table in `src/diagnostics/ErrorCodes.h`), `extractContextKeyword` on the message may pick a struct, enum or module page, and that one `docs/slm/*.txt` file is injected, truncated to the budget |
| `A1p_production` | what production `--smart` assembles: `docs/slm/syntax_card.txt`, then `THIS ERROR (Exxx):` with `docs/slm/codes/Exxx.txt`, then `GENERAL REFERENCE:` with the `A1_category` file; cut from the tail to the budget. The baseline a change to `--smart` has to beat |
| `A2_bm25_docs` | `docs/slm/*.txt` split into 100-250 token chunks; BM25 over error message + the diagnostic line + its identifiers; best chunks until the budget is full |
| `A3_examples` | top 3 train examples: same `error_code` first, then BM25 over error message + mutated line; shown as Broken line / Compiler said / Fixed line |
| `A4_examples_docs` | A3's examples, then A2's chunks in the budget that remains |
| `A5_random_examples` | 3 random train examples (seeded by case id): the control that separates "any example teaches the format" from "relevant retrieval helps" |
| `A6_production_examples` | `A3_examples`'s examples followed by `A1p_production`'s text, sharing the budget |

Retrieval never reads the test split and never returns an example with the
query's `source_test` (asserted on every retrieval; `--selftest` checks the
assertion fires on crafted bad examples). A3 and A5 stay well under the budget
because three examples are short.

Differences from production `--smart`, by design: A1 injects only the category
page (no `docs/slm/codes/Exxx.txt` page, no syntax card), the typeof construct
hint is not available (it comes from the raise site, not the message), and
`error_categories.txt` (the classifier reference) is not retrieval material.

## Scoring

The patch replaces line `line` with `new_code`; a multi-line `new_code` or a
line out of range is rejected. Per (case, arm) the result row records:

- `parsed` valid JSON of the right shape; `abstained` line is 0
- `right_line` line equals the corpus line
- `compiles` `turfc` accepts the patched file
- `correct` compiles, runs (10 s timeout) with `exit == expected_exit` and `stdout == expected_stdout`
- `exact` `new_code.strip() == original_line.strip()` (the results file stores this text-only
  comparison; `analyze.py` reports it as `exact_text` and reports `exact` as the same text on
  the mutated line, so `exact_text` minus `exact` counts right fixes placed on the wrong line)
- `prompt_tokens`, `ctx_tokens`, `latency_s`, `raw`, `retrieved` (chunk or example ids)

## Run

```
python3 scripts/slm/rag_experiment/run_rag.py --run-name full --split test
python3 scripts/slm/rag_experiment/analyze.py full
```

Flags: `--arms A0_none,A3_examples`, `--limit N`, `--corpus-dir`, `--split`,
`--server-url URL` (use a running server instead of starting one),
`--turfc`, `--llama-server`, `--model`, `--threads` (8), `--gpu-layers` (0),
`--port`, `--budget`, `--scratch` (default: system temp dir), `--cache-prompt`,
`--dump` (print retrieved context per case and arm without calling the model),
`--selftest`.

Results append to `results/<run_name>.jsonl`; a rerun with the same
`--run-name` skips the (id, arm) pairs already present. Arms are interleaved per
case, so a partial run is a balanced comparison. `results/<run_name>.meta.json`
records the platform, model sha256 and llama-server version; resuming a run on
a different platform, model or server build is refused, so runs from different
machines are never mixed.

`analyze.py` reports, over the cases with every arm complete: rates of parsed,
abstained, right_line, compiles, correct, exact_text and exact with Wilson 95% intervals, mean latency
and prompt tokens, paired exact McNemar tests of `correct` for each arm against
`A1p_production`, `A1_category` and `A0_none` (discordant counts b and c, p-value), `correct`
by subset, and `correct` by `kind`. The subsets are foreign-syntax vs classic
mutations, and "pair seen in train" vs "novel pair": a test case whose exact
(broken line, fixed line) pair also occurs in the train bank can be answered by
copying a retrieved example, so a retrieval gain that holds only on seen pairs
is recall, not generalisation. Output goes to stdout and `results/<run_name>.md`.

## Running on macOS

The harness needs `turfc` only to compile and run programs, so a build without
the model runtime is enough:

```
./scripts/cmake_build.sh --release --no-smart
```

For `llama-server`, either `brew install llama.cpp` or build the submodule
(`external/llama.cpp`, `cmake -B build && cmake --build build -j --target llama-server`;
Metal is on by default on Apple Silicon). Put the GGUF at
`models/qwen2.5-coder-1.5b-instruct-q4_k_m.gguf`. Then:

```
python3 scripts/slm/rag_experiment/run_rag.py --run-name full-mac --split test \
  --llama-server "$(which llama-server)" --gpu-layers 99 --threads 4
python3 scripts/slm/rag_experiment/analyze.py full-mac
```

Add `--turfc`, `--model` or `--llama-server` when the binaries live elsewhere.
Only the Python standard library is used.
