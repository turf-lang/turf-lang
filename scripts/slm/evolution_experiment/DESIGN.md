# Language-evolution study: design

What happens to a small local model's Turflang repairs when the language makes
a breaking syntax change, and which remedy recovers it at what cost: retrieval
of new-version examples, fine-tuning, or both.

This is the main study that follows the RAG pilot in `../rag_experiment/`
(closed; outcome in `../rag_experiment/results/confirm-summary.md`). The
literature sweep that motivates it is `../../../../paper-revision/LITERATURE_LANGUAGE_EVOLUTION.md`.
The hypotheses and the analysis are fixed in `PREREGISTRATION.md`.

## Why controlled changes, not the real 3.0 changes

The 3.0 breaking register (`../../../../TURFLANG_3.0.0_ORDER.md`) holds only two
syntax changes, B-16 (`recover` binds tighter than `=` and `+=`) and B-13
(raw-receiver method calls need `unsafe`). Two changes give too few cases and no
control over the kind of change. The open gap in the literature is a change the
researchers design and can vary, which only a language owner can do. The study
therefore builds synthetic language versions, and uses B-16 and B-13 as a small
real-world check (section "Real changes").

## Language versions without touching the compiler

A version `v'` is defined by a source-to-source mapping to current Turflang
(`v0`, the 2.x syntax). The `v'` compiler is a wrapper, `evolve.py`:

1. Lex the `v'` program (a Python lexer for Turflang tokens; strings and
   comments are opaque).
2. Reject every `v0`-only form the change removed, with a diagnostic in
   `turfc`'s exact format (header, code, message, snippet, caret). These are the
   stale-syntax errors the study is about, so the wording follows what `turfc`
   prints for the nearest native error (for a removed keyword, E014 "Hmm, I
   don't know the word ..." with a `Did you mean` hint naming the `v'` form).
3. Translate `v'` to `v0`, line-preserving, and run `turfc`.
4. Map `turfc`'s diagnostics back: the snippet line is re-rendered from the
   `v'` source, columns are moved through the token alignment, and `v0`
   spellings in the message are rewritten to `v'` spellings through the same
   table.

Nothing changes in `turfc`, `docs/` or the test suite; the study stays outside
the language (Constitution and decoupling rules). The wrapper is the only
compiler the models see, in every arm and for every version, including `v0`
(identity mapping) so that wrapper effects are constant across arms.

Wrapper checks, run before any model call and kept as `evolve.py --selftest`:

- Round trip: every `.tr` under `tests/` and `tests/compat/2.0/` translated
  `v0 -> v' -> v0` is byte-identical, and the `v'` program compiles and runs with
  the same stdout and exit code through the wrapper.
- Stale rejection: inserting each removed form into a `v'` program gives the
  wrapper's diagnostic, never a silent accept.
- Diagnostic parity: for 50 mutated programs per change, the wrapper's
  diagnostic for the `v'` program equals `turfc`'s diagnostic for the `v0`
  program after the spelling map (code, line, message).

## The changes

Four changes of increasing reach, each in two directions relative to what a
pretrained code model already expects. "Toward" adopts a spelling common in
mainstream languages; "away" adopts one that no mainstream language uses. The
direction is the variable only a language owner can randomise: it separates
"the model has to learn something new" from "the model has to unlearn its
pretraining prior".

| id | change | `v0` | toward | away | reach |
|---|---|---|---|---|---|
| C1 | function keyword | `fn f(): int` | `func f(): int` | `proc f(): int` | every function |
| C2 | mutable declaration | `mut x: int = 0;` | `var x: int = 0;` | `cell x: int = 0;` | most statements |
| C3 | return type | `fn f(a: int): int` | `fn f(a: int) -> int` | `fn f(a: int) gives int` | every signature, structural |
| C4 | module path | `io::printline(x)` | `io.printline(x)` | `io~printline(x)` | every call into a module |

Each change is applied alone, giving eight versions (`C1t`, `C1a`, ... `C4a`).
The "away" spellings are chosen so they collide with no Turflang keyword or
standard-library name (`proc`, `cell` and `gives` occur in `lib/std` only in
comments); `evolve.py --selftest` asserts it over every `.tr` file. `~` is
Turflang's unary bitwise not and has no binary form, so `name~name` with an
imported module on the left is unambiguous.

Phase 2 (exploratory, after the primary analysis): stacked versions
`v1 = C1t`, `v2 = v1 + C2a`, `v3 = v2 + C4t`, to test whether updates
accumulate or collapse (Chhetri et al. 2025 report collapse under successive
edits).

## Tasks

- **T1 repair (primary).** The pilot task in `v'`: a passing program translated
  to `v'`, one line mutated so it no longer compiles, the wrapper's diagnostic,
  the production text patch format (`original_code`/`new_code`), three attempts
  with production's feedback. Mutations are the pilot's mutators run on `v'`
  source, plus a new kind per change, `stale_<id>`: the line rewritten in its
  `v0` form, which is the error a stale user or model actually makes.
- **T2 next line (secondary).** A `v'` program cut after a line that opens a
  construct the change touches (a signature, a declaration, a module call); the
  model writes the next line. Scored only for stale form, not correctness.

## Outcomes

- `correct`: compiles under the wrapper, runs, same stdout and exit (pilot
  definition).
- `stale`: the model's `new_code` (every attempt, T1) or line (T2) contains a
  `v0` form the change removed. Detected by the wrapper's stale lexer, not by
  regex on text.
- `stale_rate` = stale outputs / parsed outputs, per arm and version.
- Cost: mean context tokens, wall time, and for fine-tuning arms the training
  examples and training minutes.

## Arms

| arm | weights | context |
|---|---|---|
| `B` | base | none |
| `B+N` | base | release note: the change in one sentence and one before/after pair (about 60 tokens) |
| `B+R` | base | top-3 retrieved `v'` broken-to-fixed examples (pilot `A3`, bank in `v'`) |
| `F0` | LoRA on `v0` repair data | none |
| `F0+N` | LoRA on `v0` | release note |
| `F0+R` | LoRA on `v0` | retrieved `v'` examples |
| `F'` | LoRA on `v'` repair data | none |
| `F'+R` | LoRA on `v'` | retrieved `v'` examples |

`F0` is the realistic stale model: a fine-tune that was right before the
release. `F'` is the cost of re-training for every release. `B+N` is the
"tell it in context" baseline of CodeUpdateArena and Ashik et al. 2026.

Fine-tuning: LoRA (rank 16, alpha 32, attention and MLP projections), 3
epochs, learning rate 2e-4, on the same 320 train cases the retrieval bank
holds, in the chat format and patch format used at test time, with the
attempt-1 prompt (no context block) as input and the corpus fix as target. On
the M4 with `mlx-lm`; the fused model is quantised to `q4_k_m` GGUF so every
arm runs through the same `llama-server` build. One `F0` and eight `F'`
adapters per model size.

## Effort curves

For `B+R` and `F'`, the `v'` data size `k` takes 0, 8, 32, 128 and 320
examples (nested subsets, stratified by kind, seed fixed). Retrieval uses the
first `k` as its bank; fine-tuning trains on them. The outcome is the `k` at
which each reaches 90% of its own `k = 320` `correct` rate, at the primary
model size.

## Models

Qwen2.5-Coder-Instruct 1.5B (primary; the `--smart` size), 3B and 7B
(secondary), the same GGUF files as the pilot for the base arms. The 7B
fine-tunes run with 4-bit base weights (QLoRA). If 7B fine-tuning does not fit
the M4's memory, 7B runs only the base arms and that is reported.

## Data

- Train and retrieval bank: the pilot's `corpus/train.jsonl` (320 cases, 280
  source files), translated per version. `stale_<id>` cases are added at 1 in 5.
- Dev set: the pilot's `corpus/test.jsonl` (72 cases), translated. Used to
  debug the wrapper and the fine-tuning recipe; results are exploratory.
- Confirmatory set: new, built with `build_corpus.py`'s mutators on a fresh
  seed from source files used by none of `train`, `test` or `test_fresh`
  (the remaining test-bucket files, `tests/compat/2.0/`, and suites added since
  2026-10-20), 150 cases per version with 30 of them `stale_<id>`. Its sha256
  is written into `PREREGISTRATION.md` at the freeze commit and no one reads it
  before the confirmatory run.

`test_fresh.jsonl` is not reused: the confirmation run has scored it.

## Real changes

B-16 and B-13 are run as two extra versions with the real 2.9 behaviour as
`v0` and the 3.0 rule as `v'` (using `turf migrate 3.0` output once it ships;
until then the wrapper parenthesises `recover` and wraps raw-receiver calls).
Arms `B`, `B+R`, `F0`, `F0+R` only. Reported descriptively; too few programs
use either construct for a test.

## Order of work

1. `evolve.py` with C1-C4 and `--selftest`; round trip over the whole suite.
2. Translate train and dev; extend `run_rag.py` with `--version` (wrapper as
   compiler, `stale` scoring) and the `B+N` arm. Pilot arms keep their
   behaviour when `--version v0`, checked by re-scoring a pilot results file.
3. Fine-tuning script (`finetune.py`, mlx-lm) and the GGUF conversion; one
   `F0` on 1.5B and a dev-set run of all arms on `C1t` as the end-to-end check.
4. Build the confirmatory set; freeze (`PREREGISTRATION.md` gets the hashes).
5. Confirmatory runs on the Mac; analysis with `analyze.py --evolution`.
6. Exploratory: effort curves, 3B/7B, stacked versions, real changes.

## Threats to validity

- The wrapper writes the stale diagnostics. Their quality decides how
  recoverable a stale error is. Diagnostic parity covers the mapped errors; the
  stale errors are written to the same pattern as `turfc`'s nearest native
  error and reviewed by hand. The pilot shows native diagnostics vary too: a
  C-style `let x: int = 0;` gives E102 "Expected ';' after statement", and no
  arm at any size fixed that kind.
- Synthetic changes are smaller and more regular than a real release. The
  "toward" vs "away" contrast and the real-change check are the controls.
- One model family. A second family at 1.5B (for example Llama-3.2-1B or
  DeepSeek-Coder-1.3B) is an exploratory extension if time allows.
- Fine-tuning hyperparameters favour neither arm by design, but one recipe is
  used throughout; it is tuned only on the dev set and only for `F0`.
