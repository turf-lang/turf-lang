# Results

> **Status: placeholder — to be regenerated.** The entire benchmark suite
> (Part A and Part B) will be re-run on the project MacBook and every table in
> this section replaced with the fresh numbers, plus full per-run statistics
> and a per-category chart. The tables below are the digest from the previous
> run (*Turf-Lang Compiler — Benchmark Report* [21], August 2026), kept here only so
> the section structure, metrics, and prose are ready to drop the new figures
> into. Do not cite these numbers as final.

## Test environment

| Parameter | Value |
|---|---|
| Dates measured | 2026-08-19 – 2026-08-21 |
| Hardware | Apple M4 |
| OS | macOS 26.5.2 (Darwin 25.5.0, arm64) |
| Compiler backend | LLVM / Clang 22.1.5 (Homebrew) |
| Small language model | `qwen2.5-coder-1.5b-instruct-q4_k_m.gguf`, run locally via the bundled `llama.cpp` backend, no network calls |

Part B uses the **same** LLVM 22.1.5 backend for both the Turflang compiler and
the C reference compiler, isolating front-end / codegen differences from
LLVM-version differences.

---

## Part A — SLM-assisted diagnostic repair pipeline (`--smart`)

**Sample.** N = 99 cases, fixed-seed random sample (`random.seed(42)`) from a
regression corpus of 241 compile-time diagnostic test files spanning 19 error
categories. One further sampled file is a runtime-only assertion test (compiles
successfully, binary aborts on execution) and is outside `--smart`'s
compile-time scope; it is excluded from the N = 99 figures.

**System under test.** The compiled binary, invoked as an end user would:
`turf <file>.tr --smart -o <out>`.

**Acceptance criterion.** A case is scored as *validated fix surfaced* iff the
confidence gate fires (`confidence ≥ 0.8 ∧ fix_mode ≠ NONE`). This gate
requires the candidate patch to have passed sandboxed re-validation — a full
re-parse, re-resolve, re-type-check of the patched buffer plus a structural
lint pass — with up to three sandbox-retry attempts per case before a fix is
withheld. Re-validation fails only on error-level diagnostics; a residual
benign warning is still accepted.

**Measurement.** Wall-clock time and peak resident set size (RSS) via
`/usr/bin/time -l` around each invocation, 30-second cooldown between cases.

### Headline results (N = 99)

| Metric | Value |
|---|---|
| Validated fix surfaced (sandbox-passed, confidence ≥ 0.8) | **45 / 99 (45.5 %)** |
| Correctly withheld (`NONE`, or failed re-validation) | 54 / 99 (54.5 %) |
| Process crashes | **0 / 100** |
| Peak RSS — mean | 1495 MB |
| Peak RSS — range | 1487–1515 MB |
| Mean wall time, error → verdict | 13.8 s |
| Median wall time, error → verdict | 13.1 s |
| Wall time range | 5.5 s – 36.9 s |
| Mean wall time — accepted cases only (n = 45) | 10.0 s |
| Mean wall time — rejected cases only (n = 54) | 16.9 s |

Peak RSS excludes the single runtime-only test, which never invokes the SLM and
measures 55 MB. Rejected cases consistently take longer than accepted ones,
consistent with the gate exhausting the full three-attempt sandbox-retry loop
before declining rather than failing fast.

### Accuracy by error category (N = 99)

| Category | Validated / Total | Rate |
|---|---|---|
| 10_default_init | 1/1 | 100 % |
| 02_resolver | 3/3 | 100 % |
| 15_enums | 4/5 | 80 % |
| 05_arrays_pointers_ffi | 5/7 | 71 % |
| 14_cfg_analysis | 2/3 | 67 % |
| 04_control_flow | 5/10 | 50 % |
| 17_char_type | 1/2 | 50 % |
| 24_first_class_functions | 1/2 | 50 % |
| 26_state_machines | 1/2 | 50 % |
| 03_typechecker | 6/13 | 46 % |
| 07_functions_modules | 7/16 | 44 % |
| 11_bitwise_ops | 3/7 | 43 % |
| 06_structs | 3/11 | 27 % |
| 25_errors | 2/8 | 25 % |
| 01_syntax | 1/5 | 20 % |
| 13_string_stdlib | 0/1 | 0 % |
| 16_sys_and_test_stdlib | 0/1 | 0 % |
| 18_operator_overload | 0/1 | 0 % |
| 19_int32_type | 0/1 | 0 % |
| **Total** | **45/99** | **45.5 %** |

The four 0 % categories are single-case negative tests where the compiler is
designed to reject the code outright; trace inspection (`TURF_SLM_DEBUG=1`)
confirms the model correctly declines with `fix_mode: NONE` in each, rather
than proposing an incorrect patch — i.e. these are *correct withholds*, not
failures.

### Latency — independent sub-sample (N = 18)

An 18-case fixed-seed sub-sample from the same pool, same 30-second-cooldown
protocol.

| Metric | Value |
|---|---|
| Mean wall time, error → verdict | 12.2 s |
| Median wall time, error → verdict | 10.8 s |
| Wall time range | 6.1 s – 35.8 s |
| Mean wall time — accepted (n = 10) | 9.4 s |
| Mean wall time — rejected (n = 8) | 15.6 s |
| Peak RSS — mean | 1490 MB |
| Peak RSS — range | 1472–1506 MB |
| Crashes | 0 / 18 |

Agrees closely with the full-sample headline (mean 12.2 s vs. 13.8 s),
supporting that both measure the pipeline's steady-state latency.

### Reading of Part A

- **Zero crashes across 100 invocations** of a compiler linking an in-process
  LLM backend, with a bounded, flat memory footprint (~1.5 GB, range < 30 MB).
- The gate's asymmetry is by design: **no incorrect fix was surfaced** in the
  sample. Every 0 %-category case was a correct refusal. The 45.5 % headline is
  a *validated-fix yield*, not an accuracy — the complement is dominated by
  correct withholds, not wrong answers shown to the user.
- Categories where the compiler supplies a precise ground-truth category and a
  related location (resolver, default-init, enums, arrays/FFI) yield the
  highest validated-fix rates; open-ended syntax and struct errors yield the
  lowest, matching the expectation that a 1.5-B local model is strongest when
  the compiler has already narrowed the problem.

---

## Part B — compiled-program performance vs. C (same LLVM backend)

Six programs, each implemented identically in Turflang and C: `fib(38)`
(recursive, call-bound), `loopsum` (3×10⁸-iteration scalar loop), `bubblesort`
(n = 6000, O(n²)), `matrixmul` (250×250, O(n³)), `structs` (2000-element
array-of-structs over 20 000 timesteps), `sieve` (Sieve of Eratosthenes over
200 000, ×60). Output was verified byte-for-byte identical across all 24
Turf-vs-clang comparisons (6 programs × 4 opt levels) before any timing.
Each binary run 15 times after a warm-up; median is the primary metric.

### Median wall-clock time (ms)

| Benchmark | Turflang | clang -O0 | clang -O1 | clang -O2 | clang -O3 |
|---|---|---|---|---|---|
| fib(38) | 79.26 | 136.11 | 54.76 | 80.33 | 64.52 |
| loopsum | 139.89 | 169.58 | 117.73 | 92.26 | 92.20 |
| bubblesort | 16.52 | 24.00 | 14.84 | 8.55 | 8.51 |
| matrixmul | 9.52 | 14.25 | 6.82 | 4.75 | 4.82 |
| structs | 13.43 | 41.82 | 12.36 | 9.10 | 9.07 |
| sieve | 19.20 | 43.58 | 16.61 | 13.65 | 13.60 |

### Turflang median relative to each clang level (ratio = Turf / C; < 1 = Turf faster)

| Benchmark | vs -O0 | vs -O1 | vs -O2 | vs -O3 |
|---|---|---|---|---|
| fib(38) | 0.58× | 1.45× | 0.99× | 1.23× |
| loopsum | 0.82× | 1.19× | 1.52× | 1.52× |
| bubblesort | 0.69× | 1.11× | 1.93× | 1.94× |
| matrixmul | 0.67× | 1.40× | 2.00× | 1.98× |
| structs | 0.32× | 1.09× | 1.48× | 1.48× |
| sieve | 0.44× | 1.16× | 1.41× | 1.41× |

### Reading of Part B

Turflang beats unoptimised C by 18–68 % on every benchmark and is within ±20 %
of `clang -O1` on four of six. It does not match `-O2`/`-O3` on the
numeric-loop benchmarks (`loopsum`, `bubblesort`, `matrixmul`), where clang's
auto-vectoriser and loop unroller — passes absent from Turflang's fixed
five-pass pipeline — give a real structural advantage. The two struct/branchy
benchmarks (`structs`, `sieve`) land in the *best* relative position (9 % and
16 % slower than `-O1`; 68 % and 56 % faster than `-O0`), indicating the
`-O2`/`-O3` gap is specifically a vectorisation/unrolling gap on tight numeric
loops, not a general codegen weakness. This is the expected profile for a
compiler whose optimiser runs one fixed, moderate (`-O1`-class) pass sequence.

**What Part B supports:** Turflang is a genuine AOT compiler emitting native
machine code of the same order as production C through the same LLVM backend,
across six distinct codegen shapes, with byte-identical output.
**What it does not claim:** parity with a mature optimising compiler at its
most aggressive setting — it currently does not on numeric-loop-heavy code, and
the gap is attributable to specific missing passes, not a structural flaw.
