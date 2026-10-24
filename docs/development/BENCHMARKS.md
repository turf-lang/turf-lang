# Benchmarks

The performance baseline (`ROADMAP_v2.0.0.md` REL-005). Not a gate on absolute
numbers; a gate on **unexplained regressions**: between the freeze tag and the
release, a benchmark more than 20% slower than its recorded baseline needs an
explanation recorded here (REL-007 runs the check on every release candidate).

## What is measured

| Benchmark | Source | What it exercises |
|---|---|---|
| `structs` | `bench/structs.tr` + `bench/structs.c` | a numeric loop over an array of structs, every `+` overflow-checked; the C twin (`clang -O3`) gives the "vs C" ratio, which tracks FUTURE F-24 (checked arithmetic vs auto-vectorization) |
| `particles` | `bench/particles.tr` + `bench/particles.c` | the 2000-particle, 20,000-step array-of-structs integrator behind #257's "53% slower than `clang -O3`" figure, reconstructed from that description; every `+` overflow-checked, the C twin (`clang -O3`) gives the "vs C" ratio |
| `strings` | `bench/strings.tr` | `StringBuilder`, `s[i]` over a 200 KB string, `substring`, `replace`, `contains` |
| `map_churn` | `bench/map_churn.tr` | `Map<string, int>` put / get / remove over a sliding window of 5000 keys |
| `gc_alloc` | `bench/gc_alloc.tr` | 3 million allocations with 1000 kept alive: the allocator and collector |
| `json_roundtrip` | `bench/json_roundtrip.tr` | build, stringify, parse and read back 2000-object arrays |
| `compile_stdlib_check` | `lib/std/**/*.tr` | `turfc --check --module` over every stdlib file |
| `compile_showcase` | `showcase/*.tr` | `turfc -O2` of every showcase program, link included |

Each program prints a checksum: it must not change between runs, and the C
twin's must equal the Turf program's. Each is sized to take about 0.5–1 s.

## How to measure

    cmake -S . -B build_release -DCMAKE_BUILD_TYPE=Release -DTURF_SMART=OFF -DTURF_BUILD_CLI=OFF
    cmake --build build_release -j$(( $(nproc) / 2 ))
    python3 scripts/dev/bench.py --out bench/results/<date>-<what>.json
    python3 scripts/dev/bench.py --compare bench/results/2026-10-13-baseline.json

`scripts/dev/bench.py` measures `build_release/turfc` (compile-time numbers from a
Debug build are not comparable), runs 9 rounds, and prints the table below.
`--compare` exits 1 if any benchmark's **minimum** time is more than 20%
(`--threshold`) slower than the baseline's. Compare only numbers from the same
machine with the same kind of build; the script warns when either differs.

**Noise.** On the baseline machine (a laptop i5-13500H under WSL), the same
commit measured back to back, unpinned and one benchmark after another, moved
up to 30%: more than the gate, so it would have flagged a change that wasn't
there. Three things bring it under 10%, and `bench.py` does all three:

- Every timed command is pinned to one CPU (`taskset -c 2`; `--cpu -1` turns
  it off). Unpinned, the scheduler moved the process between cores and the
  same program took 0.81 s one run and 1.14 s another.
- Runs are interleaved: round r runs every benchmark once. Turbo and thermal
  drift over a session then lands on every benchmark alike.
- The gate compares minimum times. The median of `compile_showcase` moved 27%
  between two runs; its minimum moved under 8%.

Measured noise after these, two comparisons of 293c535 against its own
baseline: −11.1% to +8.2%. Close the other programs (a fuzz job, a build)
before measuring: those move numbers more than any of this.

## Baselines

### 2026-10-13 — REL-005 baseline (`bench/results/2026-10-13-baseline.json`)

Commit `293c535`, `build_release` (Release), 9 rounds pinned to CPU 2,
13th Gen Intel Core i5-13500H (16 threads), Linux 6.18 under WSL2.

| Benchmark | Median (s) | Min (s) | Peak RSS (KB) | vs C | Checksum |
|---|---|---|---|---|---|
| gc_alloc | 0.982 | 0.955 | 14528 |  | 9190220 |
| json_roundtrip | 0.906 | 0.866 | 149836 |  | 842570 |
| map_churn | 0.896 | 0.848 | 14432 |  | 397107500 |
| strings | 0.878 | 0.855 | 5860 |  | 129469317 |
| structs | 0.537 | 0.529 | 2368 | 1.06x | -500001001024 |
| compile_stdlib_check | 1.160 | 1.137 |  |  | 24 files |
| compile_showcase | 2.884 | 2.700 |  |  | 7 files |

Notes:

- `structs` runs at 1.06x C's time. ROADMAP §27 D24 measured a different
  program, never checked in and rebuilt here from its description, at 53%
  slower than `clang -O3`, because the overflow check on every `+` kept LLVM
  from vectorizing the loop. This one is not that program, so 1.06x does not
  show F-24 closed; it is what this program measures, from here on.
- `json_roundtrip` peaks at 146 MB though each round's tree is garbage by the
  next round: the collector lets the heap grow to twice the live data before
  collecting (`turfrt_gc.c`, the 1 MiB-floor doubling rule), and the live data
  at a collection includes the round in progress.

### 2026-10-24 — `particles`, the program behind #257's figure (`bench/results/2026-10-24-particles.json`)

Commit `5b91dbd` plus the benchmark, `build_release` (Release, system LLVM 23),
15 rounds pinned to CPU 2, 13th Gen Intel Core i5-13500H (16 threads),
Linux 6.18 under WSL2.

| Benchmark | Median (s) | Min (s) | Peak RSS (KB) | vs C | Checksum |
|---|---|---|---|---|---|
| particles | 0.030 | 0.027 | 2360 | 1.46x | -40102000 |

Notes:

- `particles` is rebuilt from #257's description (2000 particles, 20,000
  steps, `particles[i].x + particles[i].vx`-style additions loaded from the
  array); the original program was never checked in. At #257's sizes it runs
  for about 0.03 s, far under the 0.5–1 s the other benchmarks target, and the
  sizes are kept so the figure can be reproduced.
- `bench.py` builds Turf at `-O2`. Turf `-O3` against `clang -O3`, 41 runs each
  pinned to CPU 2, process start-up included: 1.40x at the minimum and 1.42x at
  the median (Turf `-O2`: 1.41x and 1.41x). Turf is about 40–46% slower than C
  on this program, against #257's 53%: the gap is there, a little smaller than
  the figure recorded, and it is not closed.

### 2026-10-24 — NM-4: one shared cold trap block per function (dropped)

The question (roadmap NM-4, issue #296): with every overflow check in a
function branching to ONE cold, `noreturn` trap block (the file and line merged
by `phi`s, the branch weighted unlikely, `turf_rt_panic` marked `noreturn` and
`cold`) instead of a trap block per check, does LLVM vectorize the hot loop?
Measured on `roadmap/v2.2.0` plus the change, system LLVM 23.1.3, same machine,
15 interleaved rounds pinned to CPU 2, process start-up included.

| Program | Turf before | Turf after | `clang -O3` |
|---|---|---|---|
| `particles -O2` (min / median, ms) | 29.3 / 31.9 | 29.1 / 33.3 | 20.4 / 23.9 |
| `particles -O3` | 30.4 / 33.3 | 28.3 / 33.8 | |
| `structs -O2` | 654.9 / 689.0 | 648.9 / 680.4 | 582.7 / 623.7 |
| `structs -O3` | 657.4 / 705.8 | 646.6 / 682.6 | |
| packed-integer instructions in `main` | 0 | 0 | 2 (`particles`) |

Every checksum matched. LLVM's loop vectorizer gives the same reason before and
after (`opt -O3 -pass-remarks-missed=loop-vectorize`): "Early exit loop with
store but no supported condition load". Early-exit vectorization in LLVM 23
does not take a loop that stores, and these loops store every particle, so one
exit block instead of several changes nothing it looks at. The differences are
within run-to-run noise. The restructuring is not merged; NM-2 (proving checks
away) and NM-3 (`math::wrapping_*`) are what remain for this loop.

### 2026-10-24 — the SLP vectorizer at `-O2` and `-O3`

`runOptimizationPipeline()` built its `PassBuilder` with LLVM's default
`PipelineTuningOptions`, which leave the SLP vectorizer off; clang turns it on
at `-O2` and `-O3`. It is the pass that vectorized `particles.c`: one
`<2 x i64>` load of `{x, y}`, one of `{vx, vy}`, one add. Turf now turns it on
at the same levels. Same machine, `particles` at `-O3`, 21 runs pinned to CPU 2,
minimum, process start-up included:

| Program | SLP off | SLP on | `clang -O3` |
|---|---|---|---|
| `particles` (checked `+`) | 25.4 ms | 25.3 ms | 17.9 ms |
| `particles` with `math::wrapping_add` | 24.1 ms | 18.1 ms | |
| packed-integer instructions in `main` (wrapped) | 0 | 8 | 6 |

Every checksum matched. With the checks the loop stays scalar: each checked
`+` branches to its own panic block, so the `x` and `y` additions are in
different basic blocks and SLP, which works within one, cannot pair them.
`--emit-llvm` runs the pipeline without a `TargetMachine`, so its IR never
shows vectorization; count instructions in the binary (`objdump -d`).

## Fixed before the baseline

Writing these benchmarks found five performance bugs (and one compiler crash,
`s.substring(1, 3)` in method form, f602b94). They were fixed before the
baseline was taken, so that the first numbers recorded do not contain them.
Measured on the same machine; "before" is the commit before each fix.

| Commit | What was slow | Before | After |
|---|---|---|---|
| c7f231c | `Map`/`Set` under churn: removed keys left tombstones the growth check never counted, until every empty slot was one and each lookup of a missing key scanned the whole table | 20 000 churn steps over 5 000 keys: 0.37 s (cost grew with the table) | 0.00 s at 500, 5 000 and 50 000 keys |
| 7003042 | `json::parse`: each byte read through `self.s[self.pos]` ran `strlen()` over the whole document, and each value's `strings::substring()` did again | 100 KB: 0.33 s; 1 MB: about 30 s | 100 KB: 0.02 s; 1 MB: 0.27 s |
| 7003042 | `json::stringify`: `result = result + piece` copied the output so far on every element | 8 000 items: 1.22 s | 80 000 items: 0.10 s |
| 91c6a1d | every `gc_realloc` (a `List` or `StringBuilder` growing) scanned all blocks allocated since the last collection and shifted the sorted block table | stringify of 10 000 two-field objects: 0.66 s | 0.012 s |
| 321dc58 | not a speed fix: `Map`/`Set` keys were hashed by raw bytes, so equal padded-struct keys, `-0.0`/`0.0` and string fields hashed apart (FREEZE §6.8 `hash_of`) | not measured separately | `map_churn` at 40 000 steps after all five fixes: 0.04–0.05 s |

## Recorded regressions

None yet. A regression over 20% against the baseline above is recorded here:
the benchmark, both numbers, the commit, and why it is accepted (or the fix).
