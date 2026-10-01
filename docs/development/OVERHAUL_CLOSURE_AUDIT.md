# Architecture Overhaul — Final Closure Audit

| | |
|---|---|
| **Date** | 2026-09-26 |
| **Audited tree** | `refactor/architecture` @ `cde1d30` (clean), LLVM 22, x86-64 Linux (WSL2), `--release` build |
| **Specification** | `docs/planning/ARCHITECTURE_OVERHAUL.md` (§4, §6–§8, §9–§18, §22, §23, §25, §27) and `docs/development/FREEZE.md` |
| **Verdict** | **Not closable yet.** 11 Category A and 3 Category B defects, several of them silent memory corruption, plus documentation drift. More than two items and several are not small, so per the audit's implementation threshold **nothing was implemented here**; the work is handed off in `docs/planning/OVERHAUL_REMAINING_WORK.md` (items OVH-001 … OVH-015). |
| **Next documents** | `docs/planning/OVERHAUL_REMAINING_WORK.md` (temporary handoff) · `docs/planning/ROADMAP_v2.0.0.md` (what follows closure) · `docs/planning/FUTURE.md` (post-2.0 backlog) |

This record is permanent. It is the evidence base for the handoff and the
2.0 roadmap: every claim below was checked by running something, and the
reproducers are included so a later reader can re-run them.

---

## 1. Method

The audit did **not** accept "the code exists", "tests pass", "the change
record says landed" or "the doc says complete" as proof. For each claim it
tried to construct the program that would disprove it (`AGENTS.md`: *verify
claims live*). In order:

1. Read `ARCHITECTURE_OVERHAUL.md` in full, `ARCHITECTURE_SIMPLIFICATION.md`,
   `FREEZE.md`, `INVARIANTS.md`, `INDEX.md`, `handbook-sync/PENDING.md`, and
   the *Known pitfalls* / *Unresolved questions* of all 23 change records
   (the disclosed gaps are where remaining work concentrates).
2. Built the compiler; ran the full suite at default, `-O0` and `-O3`; ran
   all ten `scripts/*_smoke.sh`/`check_*.sh` scripts.
3. Direct-compiled every `lib/std/**/*.tr` (`--resolve`) and every
   `showcase/*.tr` (full compile), inspecting warnings as well as errors.
4. Wrote ~45 adversarial probe programs against §23's Definition of Done,
   the frozen decisions, the disclosed gaps, and public-release concerns
   (runtime failure modes, packaging, tooling).

## 2. What passed

| Check | Result |
|---|---|
| `./scripts/test_runner.sh` (default / `-O0` / `-O3`) | **765 / 765** at each level |
| `check_doc_links.sh`, `check_boundary.sh` | clean (581 citations; 3 boundary checks) |
| `runtime_smoke`, `mem_smoke`, `gc_smoke`, `gc_rss_smoke` (2,712 KB peak vs 20,000 KB bound), `debuginfo_smoke`, `std002_caller_location_smoke`, `opt_levels_smoke` (13 programs × 5 levels), `slm_deterministic_fix_smoke` | all clean |
| `lib/std` direct `--resolve` | 23 of 24 modules resolve; `collections/list.tr` fails (F-11) |
| `showcase/*.tr` | all 7 compile and link; warnings from `lib/std` on every one (F-12) |
| DoD 3 — a 24-byte struct in `List`/`Stack`/`Queue`/`Map` | holds (live probe, 100 elements, growth) |
| DoD 4 — `Map<string,V>` content equality with a runtime-built key | holds |
| DoD 5 — literal overflow is a compile error, runtime `int` overflow traps, `INT64_MIN` writable | holds for `int` (see F-05 for `int32`) |
| DoD 6 — `mut x = 5;`, fixed type, note at the inferred declaration | holds (`tests/37_guess_inference/`) |
| DoD 7 — one initialization rule | holds (INV-INIT-1 verified by the suite) |
| DoD 9 — `defer` on every exit path, innermost first | holds (`tests/40_defer/`) |
| DoD 10 — every §13.1 operator overloadable, never-list enforced | holds (live probe: `+ - ~ ** % <<`, compound `+=` through `operator+`) |
| DoD 11/12/13/14 — bounded RSS, FFI crossing, manual heap untouched, freeing a GC pointer diagnosed | hold as tested (but see F-01, F-02: the collector frees live objects in two classes of program) |
| DoD 15–18 — capability declared in source, closed intrinsic list, `libturfrt` linked, no path privilege | hold (`check_boundary.sh`, INV-CAP-1) |
| DoD 19/20 — five `-O` levels, `-O0 -g` DWARF | hold (`llvm-dwarfdump --verify` clean on a generic-heavy program) |
| INV-GEN-1 — no `TurfGenericParamType` reaches Codegen | holds (assertions in place; erasure branches deleted) |
| DIAG-001 — category from class, not text | holds; `extractContextKeyword()` survives only for `--explain` doc routing (`src/main.cpp:229`, `SLMIntegration.cpp:584`), not for category classification — consistent with FREEZE §6.9 |

## 3. Findings

Categories: **A** — does not satisfy an existing Overhaul requirement ·
**B** — regression introduced by the Overhaul · **C** — documentation /
architecture drift · **D** — real, deferred, outside Overhaul scope · **E** —
rejected by §25. Every A/B/C finding maps to an OVH item.

| ID | Finding | Cat. | Evidence | Item |
|---|---|---|---|---|
| F-01 | GC descriptor bitmap truncated at 64 words → reachable strings in a >512-byte collection element are freed | A | probe below; 61-word control passes | OVH-001 |
| F-02 | GC pointers stored in untraced manual-heap memory: `json` node boxes, `kv_store_server` entries, compiler-emitted `malloc` for message-carrying error values and oversized `!T` success boxes | B | `json` probe: "JsonValue.get: no such key 'name'" after a collection; error-message probe fails at `-O0`, passes at `-O2` by register luck | OVH-002 |
| F-03 | `ptr[i]` on a raw pointer is not gated; `list.data[100000] = 42` compiles in safe code and segfaults; `.data[i]` is the documented element access for `List<T≠int>` | A (F-PTR-1) | `TypeChecker::visitArrayAccess()` never calls `checkUnsafeGate()` | OVH-003 |
| F-04 | `struct A<T> { val: T } struct B { back: A<B> }` + `mut x: A<B> = A::default();` → **compiler segfault** (exit 139, no output) | A (INV-LAYOUT-1) | TYPE-002 disclosed the coverage gap but called it "not a live correctness bug" — it is | OVH-004 |
| F-05 | `int32`: `c++`/`++c`/`c--` → **ICE** (`sadd.with.overflow.i64` on an `i32`); `mut n: int32 = 5;` rejected (F-NUM-1); `fail_int32_literal_out_of_range.tr` passes via E066, not the E076 it claims to test | A | live | OVH-005 |
| F-06 | A user `struct T` (or `K`, `V`, …) breaks `List<int>`/`Map<K,V>` inside `lib/std`; stale `NamedType("T")` fields in the registered generic struct type + a name-lookup heuristic | A (F-GEN-1) | live; root cause `src/TypeCheckerDecl.cpp:68` | OVH-006 |
| F-07 | `b.inner.a` through a nested generic struct field → "Cannot access field on non-struct type"; via a local it works | A | live | OVH-007 |
| F-08 | `&Node<int>` does not widen to `&?Node<T>` in generic candidate matching (non-generic works) | A | live | OVH-008 |
| F-09 | `List<List<int>>` is a parse error (`>>` lexes as shift); the test suite writes `List<List<Big> >` | A | live | OVH-009 |
| F-10 | `mem::root`/`mem::unroot` (§14.3, §14.6, S22) never built | A | grep; MEM-002 record | OVH-010 |
| F-11 | `turf --resolve lib/std/collections/list.tr` → E034 (transitive prelude self-import); re-discovered by three change records | A (DoD 26) | live | OVH-011 |
| F-12 | `NeverReturnsErrorWarning` misses `return` inside `recover … run`; unused-variable/dead-store misses reads inside a `recover` operand; `import io;` alone prints a stdlib warning; `import json;` prints six | B/C | live (hello world) | OVH-012 |
| F-13 | Release workflow builds macOS arm64 (runtime `#error`s) and Windows x86-64 (GC scans the System V callee-saved set only — unsound under Win64); README advertises both | B | `runtime/turfrt_gc.c:73`, `release.yml` | OVH-013 |
| F-14 | Four disclosed FREEZE narrowings (typed-constant overflow traps instead of erroring; >int64 literal leaves; >int64 literal → `double`; E079 chain is a cache dump) | C | SEM-002 / TYPE-002 records; `immut` case verified live | OVH-014 |
| F-15 | `compiler_pipeline.md` §7 and `memory_model.md` §2.7 still describe erasure; overhaul header says "nothing implemented"; one live test silently excluded by the `_module.tr` filter; README at v1.0.0 | C | grep | OVH-015 |
| F-16 | DoD 8 met only in letter: the safe linked-list/tree tests use a fixed set of stack nodes; safe code cannot heap-allocate a user struct, and `return &local;` compiles into a dangling reference (probe printed 0, expected 10) | A in spirit / new language surface | live | **reclassified → ROADMAP FDN-001/FDN-002** (§5) |
| F-17 | `string` indexing `s[i]` is not bounds-checked (`"abc"[5]` reads garbage) | D (pre-existing, never in scope; §27 D17 neighbour) | live | ROADMAP FDN-004 |
| F-18 | Struct type identity is global by bare name: a program with **no imports** cannot declare `struct List` (prelude), `struct Parser` collides with `json`'s internal helper | D (pre-existing; blocks D7/D13) | live | ROADMAP FDN-003 |
| F-19 | Module search order puts `lib/std` before the project directory: a local `json.tr` is silently shadowed; `turf-cli` installs to `turf_modules/`, which the compiler never searches | D (pre-existing; D7) | live | ROADMAP LEC-001 |
| F-20 | Runtime stack overflow → bare SIGSEGV (exit 139, no message); no `--version`/`--help`; `turf-lsp` does not build against HEAD (`DefaultInitExprAST` removed by SEM-004) | D (release engineering) | live | ROADMAP REL-001/REL-002/SMP-001 |
| F-21 | `return f();` from a `!T` function where `f` returns `!T` → "function declares '!Rec' but returns '!Rec'" | D (D15 surface; misleading message) | fixed 2026-10-01 (LEC-004: pass-through `return`, `try`) | ROADMAP LEC-004 |
| F-22 | Diagnostics quality: a parse error inside a function cascades into "Statements are not allowed outside of a function"; E040's hint for a *type* name suggests `mut Node: int = 5;`; `_x` identifiers do not lex although lint hints recommend a `_` prefix; `x += 1` reports a dead store (LANG-001 pitfall); `strings::repeat` is quadratic and there is no `StringBuilder` (§15.9 relies on one) | D | live | ROADMAP LEC-005 / LEC-007 / LEC-008 |
| F-23 | MEM-002's own open questions: collector exercised only by the repository's tests; inline-asm register capture not independently validated | D | MEM-002 record | ROADMAP FDN-005 |

### Reproducers for the critical findings

**F-01** (generate `bigstruct_gc.tr`, compile, run → assertion failure, exit 134;
change `range(70)` to `range(60)` for the passing control):
```python
fields = "\n".join(f"  f{i}: int," for i in range(70))
print(f"""import list::List; import strings; import test;
struct Big {{
{fields}
  name: string
}}
fn churn(d: int): int {{ mut b: int[64]; b[0] = 0; mut i: int = 0;
  while i < 64 {{ b[i] = i + d; i = i + 1; }}
  if d > 0 {{ return churn(d - 1) + b[3]; }} return b[5]; }}
fn fill(l: &List<Big>): void {{ mut i: int = 0; while i < 50 {{
  mut b: Big = Big::default(); b.name = strings::repeat("x", 100) + (i to string);
  l.push(b); i = i + 1; }} }}
fn main(): int {{
  mut l: List<Big> = List::default(); fill(&l); churn(20); gc_collect();
  mut j: int = 0; while j < 200 {{ mut junk: string = strings::repeat("y", 100); j = j + 1; }}
  gc_collect();
  mut k: int = 0; mut bad: int = 0;
  while k < 50 {{ if l.data[k].name != strings::repeat("x", 100) + (k to string) {{ bad = bad + 1; }} k = k + 1; }}
  test::assert(bad == 0, "large-struct string fields survived collection");
  return 0;
}}""")
```

**F-02** (`json`): parse `[{"name": "item0zzzz…"}, …]` (40 objects, names
built at runtime) in a helper function, then in `main`: `churn(30);
gc_collect();` allocate 2,000 throwaway strings, `gc_collect();`, then
`v.at(k).get_string("name")` → panics `JsonValue.get: no such key 'name'`.
(Error-value variant: construct `AppError::Failed("context-" + n + …)` in a
helper, collect inside `recover as err run`, compare `err.message` — fails at
`-O0`.)

**F-03 / F-04 / F-05 / F-06 / F-07 / F-08 / F-09**: the snippets are inline in
the matching OVH items; each is under ten lines.

**F-16:**
```turf
import io;
struct Node { val: int, next: &?Node }
fn make(v: int, next: &?Node): &Node { mut n: Node = Node::default(); n.val = v; n.next = next; return &n; }
fn main(): int {
    mut head: &?Node = null; mut i: int = 0;
    while i < 5 { head = make(i, head); i = i + 1; }
    mut s: int = 0; mut c: &?Node = head;
    while (c != null) { s = s + c.val; c = c.next; }
    io::printline(s);   // prints 0; the list 4->3->2->1->0 sums to 10
    return 0;
}
```
No diagnostic of any kind. `gc_alloc(n)` returns `void*` and needs `unsafe`,
so there is no safe way to write `make()` correctly.

---

## 4. Audit matrix — every §8 feature group

Status vocabulary per the audit brief. "Action" names the follow-up.

| Group | Original requirement (short) | Implementation | Tests | Live verification | Status | Action |
|---|---|---|---|---|---|---|
| ARCH-001 | freeze doc, change records, doc-link CI, session scripts | present | scripts self-tested | scripts run clean | COMPLETE | preflight now also finds OVH/roadmap IDs (this audit) |
| TYPE-001 | reject by-value struct cycles incl. per-instantiation | template-level check present | `tests/06_structs/` | generic-instantiation cycle **segfaults** | PARTIALLY COMPLETE | OVH-004 |
| SEM-001 | exact literals, radix/exponent/`_` syntax | present | `35_numeric_literals` | holds; `_x` identifiers unrelated but noted | COMPLETE | — |
| OPT-001 | PassBuilder, five levels, Internalize+GlobalDCE, `--target` | present | CI × 5 levels, smoke | holds; `--target` links only for host (D20) | COMPLETE | D20 → REL-003 |
| TOOL-001 | `-g` DWARF, gdb works | present | `debuginfo_smoke.sh` | holds; flat local scopes disclosed | COMPLETE | — |
| DIAG-001 | category from class; lint VarDecl gap | present | `14_cfg_analysis` | holds; INV-LINT-1 still `planned` (structural enforcement) | COMPLETE BUT TEST COVERAGE GAP | INV-LINT-1 → LEC-005 |
| HYG-001 | header hygiene; CFG warnings through DiagnosticEngine | present | `14_cfg_analysis` | holds; parser TUs still reach LLVM IR via `Codegen.h` (disclosed) | COMPLETE BUT DOCUMENTATION DRIFT | noted in SMP-003 (layering) |
| LANG-004 | optional `step`, `string→ptr` at extern calls, local `extern fn` | present | `04`/`05` folders | holds | COMPLETE | — |
| SEM-002 | representability, trapping overflow, cast legality in TypeChecker | present for `int` | `36_numeric_semantics` | `int32` literal/`++` defects; typed-constant fold traps at runtime | PARTIALLY COMPLETE | OVH-005, OVH-014 |
| SEM-003 | `guess`, Model A | present | `37_guess_inference` | holds | COMPLETE | — |
| SEM-004 | one initialization rule | present | `10_default_init` | holds | COMPLETE | reference docs → REL-006 |
| TYPE-002 | monomorphization, INV-GEN-1, instantiation-site errors | present | `38_monomorphization` | name capture (F-06), nested field chain (F-07), `>>` (F-09), `&T→&?T` (F-08), noisy E079 (F-14) | PARTIALLY COMPLETE | OVH-004, 006–009, 014 |
| LANG-001 | operator completion, `List<T>[i]` | present; `[]` only for `List<int>`/`Map<string,int>`/`Set<int>` | `18_operator_overload` | "`List<T>[i]` works" true only for `int` | PARTIALLY COMPLETE | OVH-003 (safe `get`/`set`); generic operators → LEC-006 (**landed 2026-10-01**) |
| LANG-002 | `&?T`, declared capability, safe linked structures | present | `39_nullable_references` | raw-pointer indexing ungated (F-03); dynamic structures impossible (F-16) | PARTIALLY COMPLETE | OVH-003; FDN-001/002 |
| LANG-003 | `defer` | present | `40_defer` | holds | COMPLETE | — |
| RT-001 | `libturfrt`, panic, descriptors, packaging | present | `runtime_smoke.sh` | 64-word descriptor limit (F-01); packaging claims unsupported platforms (F-13); no runtime version check | PARTIALLY COMPLETE | OVH-001, OVH-013; REL-001 |
| MEM-001 | route managed allocations through the runtime, classify every site | present | `mem_smoke.sh` | classification missed `json` boxes and compiler-emitted mallocs (F-02) | INCORRECT IMPLEMENTATION (classification) | OVH-002 |
| MEM-002 | collector, retention/interior/FFI/RSS tests, `mem::root` | collector present | `41_gc`, smokes | frees live objects (F-01, F-02); `mem::root` missing (F-10) | REGRESSION (vs. pre-collector correctness of `json`) + MISSING | OVH-001, 002, 010 |
| STD-001 | boundary, intrinsic list, prelude spec, check script | present | `check_boundary.sh` | holds | COMPLETE | extend with OVH-002's malloc guard |
| STD-002 | `@caller_location`, message payloads | present | `42_caller_location`, smoke | message-carrying error value malloc'd (F-02) | REGRESSION (via MEM-002 interaction) | OVH-002 |
| STD-003 | stdlib modernization, close LIMITATIONS | present | several | `LIMITATIONS.md` closed; `data` still public raw pointer | COMPLETE | OVH-003 |
| OPT-002 | proven check elision | present | `43_check_elision` | holds | COMPLETE | — |
| SLM-001 | benchmark, deterministic fixes, cache, type context | present | `44_slm_diagnostics`, smoke | holds; corpus small (6 cases, disclosed) | COMPLETE BUT TEST COVERAGE GAP | corpus growth → FUTURE |

## 5. Definition of Done (§23) and decisions

| # | Item | Status |
|---|---|---|
| 1 | No verified-findings input → wrong answer / ICE / segfault | **fails** — new crash/ICE inputs F-04, F-05; memory corruption F-01, F-02, F-03 |
| 2 | recursive struct pairs diagnosed | holds for plain structs; generic instantiation F-04 |
| 3–7, 9–10 | see §2 | hold |
| 8 | linked list / tree / graph in safe TurfLang, with functions taking and returning them | **letter only** — see F-16 and the decision below |
| 11–14 | memory | hold as tested; F-01/F-02 break "reachable objects are never freed" |
| 15–20 | boundary, tooling | hold |
| 21 | a filled record per group | holds (23 records) |
| 22 | FREEZE.md matches the compiler | **fails** — F-03 (F-PTR-1), F-05 (F-NUM-1), F-14 |
| 23 | INVARIANTS lists every INV with enforcement | holds except INV-RT-3's 64-word caveat is now a correctness issue (OVH-001) |
| 24 | PENDING.md complete enough to resume | holds (≈25 owed rows) |
| 25 | doc links | holds |
| 26 | suite green; `lib/std` and `showcase` compile clean | **fails** — F-11, F-12 |

**Decisions this audit asks the user to make** (recorded here, answered in
the relevant change record):

1. **DoD 8 reclassification.** Proposed: close the Overhaul with DoD 8
   recorded as *met in letter* (LANG-002's own completion criteria are met),
   and move its intent — safe heap allocation of user types (FDN-001) and
   reference escape rules (FDN-002) — into the 2.0 roadmap as its first
   Foundation milestone. Rationale: closing it needs new language surface
   (F-STD-2: a new intrinsic is an explicit act) and new static rules, i.e.
   design, not a defect fix. This is an explicit A → roadmap move, not a
   silent downgrade.
2. **OVH-013** — supported platforms for the next release.
3. **OVH-014** — implement or record each FREEZE narrowing.
4. **OVH-012** — whether warnings originating in `lib/std` are suppressed by
   default.

## 6. Deferred register and Simplification

§27's reclassification (2.0 / FUTURE / superseded / rejected) and the
`ARCHITECTURE_SIMPLIFICATION.md` assessment are in
`docs/planning/ROADMAP_v2.0.0.md` Part II and Part III. Post-2.0 items, with
their original rationale preserved, are in `docs/planning/FUTURE.md`.

## 7. Remaining-work decision

**More than two remaining implementation items, several not small (OVH-002,
OVH-003, OVH-006 are M; OVH-013 option B is L) → no direct implementation.**
The single change this audit made to tooling is `scripts/session_preflight.sh`
learning to find `OVH-NNN` and roadmap IDs, so the handoff runs under the same
session protocol as every overhaul group.
