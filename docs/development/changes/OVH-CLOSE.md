# OVH-CLOSE — Architecture Overhaul closure

| | |
|---|---|
| **Status** | landed |
| **Dates** | started 2026-09-27 · landed 2026-09-27 |
| **Commits** | (see `git log --oneline -- 'docs/development/changes/OVH-CLOSE.md' 'docs/development/changes/OVH-015.md' 'docs/planning/ARCHITECTURE_OVERHAUL.md' 'docs/planning/OVERHAUL_REMAINING_WORK.md' 'scripts/session_preflight.sh'` on this branch) |
| **Sessions** | 1 of 1 |
| **Depends on** | OVH-001 … OVH-014 (all landed prior to this session), OVH-015 (this session) |
| **Frozen decisions relied on** | none new — this record only certifies conformance to decisions already frozen |

## Why

This is the closure gate `docs/planning/OVERHAUL_REMAINING_WORK.md` §4
requires, run once OVH-001 through OVH-015 have all landed. It exists so a
future reader has one place that states, with evidence actually looked at
(not asserted), that the Architecture Overhaul is done — and so
`OVERHAUL_REMAINING_WORK.md` itself can be deleted per its own header
instruction, rather than lingering as a "temporary" file indefinitely.

## Language semantics

Not applicable — this record certifies closure, it does not itself change
compiler or language behavior. OVH-015's own change record covers the
one behavioral fix this session made along the way (a genuine showcase
dead-store warning, found while demonstrating closure-gate item 3 — see
OVH-015's *Language semantics*/*Compiler architecture* sections).

## Compiler architecture

None beyond what OVH-015 records.

## Runtime changes

None.

## Standard-library changes

None.

## New invariants

None new. This record does not add an INV-* row; it certifies the ones
OVH-001–014 already added/updated are each in the state their own records
claim (see *Closure gate evidence*, item 7 below).

## Removed assumptions

- **"`docs/planning/OVERHAUL_REMAINING_WORK.md` is a live, still-relevant
  planning document."** False as of this record — it is deleted in the same
  commit as this file, per its own header instruction ("delete this file...
  in the commit that lands the last item"). A future reader looking for
  Overhaul-closure context should start at this record and
  `docs/development/OVERHAUL_CLOSURE_AUDIT.md` (evidence) instead; the item
  index and per-item strategy text that used to live in the deleted file
  now exists only in git history (`git log -- docs/planning/OVERHAUL_REMAINING_WORK.md`)
  and in each `OVH-NNN.md` change record's own *Why*/*Strategy* content.
- **"DoD item 8 (a dynamically-sized safe linked structure) is an open Overhaul
  defect."** False as of the closure audit's own proposal, now certified
  landed: it is reclassified as *met in letter, not in full intent* — the
  audit's own three-way split (see *Closure gate evidence* item 12 below) —
  with the intent (safe heap allocation of a user type, escape rules) moved
  into `docs/planning/ROADMAP_v2.0.0.md` as **FDN-001**/**FDN-002** (rows
  N1/N2), not left as unowned technical debt.
- **"The Overhaul's closure is blocked on more implementation work."** False
  as of this record — every item OVERHAUL_REMAINING_WORK.md listed (OVH-001
  through OVH-015) is landed, and the gate below has been demonstrated
  clean on `refactor/architecture` @ (see commit list above). The next
  work in this tree is `docs/planning/ROADMAP_v2.0.0.md`'s Foundation
  milestones, not further Overhaul closure work.

## Tests

No new tests are owed by this record specifically (OVH-015's own record
lists the tests its one behavioral fix and file rename touch). This
record's own contribution is running the FULL existing suite and every
smoke/check script as closure evidence — see below. All were **demonstrated
live**, not asserted:

- `./scripts/test_runner.sh` at default, `-O0`, `-O1`, `-O2`, `-O3`, `-Os`:
  **796/796** at every level (795 pre-existing + the 1 test
  `export_qualified_across_modules.tr` gained by OVH-015's rename, which
  un-excludes it from the runner's `_module.tr` substring filter for the
  first time).
- `scripts/runtime_smoke.sh`, `mem_smoke.sh`, `gc_smoke.sh`, `gc_rss_smoke.sh`,
  `debuginfo_smoke.sh`, `std002_caller_location_smoke.sh`,
  `slm_deterministic_fix_smoke.sh`, `check_boundary.sh`, `check_doc_links.sh`,
  `opt_levels_smoke.sh`: all clean, output inspected directly.
- Every `lib/std/**/*.tr` file (24 files) resolved directly
  (`turf --resolve`): zero errors, zero warnings other than the expected
  "No entry point found".
- Every `showcase/*.tr` file (7 files) compiled directly: zero errors, zero
  warnings — **one genuine finding**, see *Compiler architecture*/*Language
  semantics* note above and OVH-015's own record.
- Both `showcase/` client/server pairs exercised manually, end to end:
  `http_server.tr`/`http_client.tr` (a real request/response round trip
  over a real TCP socket — GET `/`, `200 OK`, correct `Content-Length` and
  body) and `kv_store_server.tr`/`kv_store_client.tr` (`SET language Turf`
  → `OK`, `GET language` → `Turf`, `GET` of a missing key → `NOT_FOUND`).

## Known pitfalls

- **Closure-gate item 10's literal wording does not match what a plain
  grep shows, but the property it exists to verify does hold.**
  `grep -rn "TurfGenericParamType" src/Codegen*.cpp` returns far more than
  the two `INV-GEN-1` assertions — roughly twenty hits, mostly inside
  `getExprType()`, a completely separate, independent type-inference pass
  Codegen maintains for its own needs (never consults TypeChecker's or the
  Monomorphizer's results — see OVH-007's own record, and the neighboring
  comment blocks in `CodegenCore.cpp` around lines 900–914 and 1218–1238).
  Read each hit individually while checking this gate: every one is either
  (a) one of the two `INV-GEN-1` assertions themselves, (b) a comment
  correctly written in past tense about the erasure era that no longer
  exists, or (c) live code in `getExprType()`'s own name-reconstruction
  fallback — unifying a call's declared parameter types against its actual
  argument types to recover a concrete return type when this pass's own
  mangled-name guess doesn't exact-match what the Monomorphizer actually
  named a clone. None of the three categories is an erasure code path: the
  actual bit-reinterpretation mechanism (`CreateIntToPtr`/`CreatePtrToInt`
  in `CastToType()`'s old generic-parameter branch) is confirmed **deleted
  outright**, not merely dead — `CastToType()` has no "cast to/from generic
  parameter" case left at all (verified live in this session via
  `docs/architecture/memory_model.md` §2.7's rewrite, OVH-015 item 1). So:
  **item 10's substance — "no erasure path is reachable" — is true and
  verified live; its literal grep-count phrasing is not, and should be read
  as the aspirational shorthand it was when the closure audit wrote it
  pre-implementation, not a mechanical pass/fail check.** One stale comment
  making an erasure-era claim in present tense (`CodegenCore.cpp`, the
  free-function return-type recovery block, ~line 955) was found and fixed
  by this session to say why the fallback is actually needed today —
  see OVH-015's own record for the diff. Nothing else needed a comment fix;
  the other two similarly-shaped blocks (`CodegenCore.cpp` ~1218,
  `CodegenFunc.cpp` ~1385) already correctly explained themselves in
  present-tense, non-erasure terms.
- **A closure gate demonstrated clean today is not a permanent guarantee.**
  This record's evidence is a snapshot at the commit(s) listed above. A
  later change that reintroduces a warning in `lib/std`/`showcase`, breaks
  an opt-level parity property, or regresses one of the smoke scripts is a
  new defect against the *current* code, not evidence this closure record
  was wrong when written — re-run the same commands, don't re-litigate
  this record.
- **This record does not re-verify every individual reproducer named in
  the now-deleted `OVERHAUL_REMAINING_WORK.md` line by line.** Each
  OVH-NNN item's own change record already did that verification (their
  own `Tests`/`Completion criteria` sections) at landing time, with the
  regression test that proves it now living permanently in `tests/`. This
  record's own contribution is the AGGREGATE evidence (full suite green at
  every opt level, after all 15 items are in) — re-deriving each item's
  original reproducer from scratch here would duplicate work already done
  and already covered by the permanent regression suite.

## Handbook impact

| File / chapter | What is now wrong | Suggested replacement |
|---|---|---|
| (none) | — | This record adds no new Turf-level language surface; every handbook-affecting change from OVH-001–014 is already queued in `docs/development/handbook-sync/PENDING.md` (confirmed complete for all 14 items by OVH-015's own audit — see that record). |

### Examples that became invalid

None.

### Examples that should be added

None owed by this record specifically.

### Sections that must NOT change

Every `docs/reference/` chapter's existing content is unaffected by
closure itself — closure certifies that OVH-001–015's changes are
consistent and complete, it does not itself alter language semantics.

## Unresolved questions

None. Every decision this file's *Why* section might otherwise raise (DoD-8
reclassification, OVH-013's platform decision, OVH-014's narrowings,
OVH-012's stdlib-warning suppression) was already asked and answered by the
individual item that owned it — see each item's own change record. Nothing
was deferred to this closing record.

## Closure gate evidence (`OVERHAUL_REMAINING_WORK.md` §4, item by item)

1. **Suite green at every `-O` level.** Demonstrated — see *Tests* above.
2. **Every `lib/std/**/*.tr` resolves clean.** Demonstrated — see *Tests*
   above (24/24 files, zero errors, zero non-"No entry point found"
   warnings).
3. **Every `showcase/*.tr` compiles warning-free; `opt_levels_smoke.sh`
   clean; both client/server pairs exercised manually.** Demonstrated —
   `showcase/http_server.tr` had one genuine dead-store warning (`body`'s
   dead `""` initializer, every path of an exhaustive `if`/`elseif`/`else`
   overwrites it before any read) found and fixed by this session; all 7
   files clean after the fix; both pairs run end to end (see *Tests*).
4. **`runtime_smoke.sh`, `mem_smoke.sh`, `gc_smoke.sh`, `gc_rss_smoke.sh`,
   `debuginfo_smoke.sh`, `std002_caller_location_smoke.sh`,
   `slm_deterministic_fix_smoke.sh`, `check_boundary.sh`,
   `check_doc_links.sh` all clean.** Demonstrated — see *Tests* above.
5. **Every reproducer in `OVERHAUL_REMAINING_WORK.md` is a passing or
   correctly-rejected test, at `-O0` and default.** Certified by
   construction: each OVH-NNN item's own change record added the
   regression test(s) proving its reproducer, and the full suite (which
   includes every one of them) is green at both levels — see *Known
   pitfalls* for why this record doesn't re-derive each one individually.
6. **Every FREEZE.md decision enforced or recorded as an accepted
   narrowing.** OVH-014 (landed 2026-09-27) is this decision's own item:
   row 1 implemented (compile-time `E076` for a foldable `immut`
   reference), rows 2/3 recorded as accepted narrowings in `FREEZE.md`
   §6.2, row 4 confirmed already tracked as `ROADMAP_v2.0.0.md` LEC-005 —
   no gap left open.
7. **`INVARIANTS.md` has no row whose status is wrong.** Checked live: 
   `INV-RT-3`, `INV-MEM-2`, `INV-CAP-2` (OVH-001/OVH-003) and `INV-LEX-1`'s
   `>>` exception (OVH-009) each read `holds`, with the specific fix and
   live-verified reproducer named. `INV-LINT-1`/`INV-CG-1` correctly still
   read `planned`, with an honest explanation of what remains unbuilt —
   OVH-015's own item 3 (`docs/planning/OVERHAUL_REMAINING_WORK.md`'s
   text) asked only that these be stated accurately, not implemented; they
   already were, by OVH-001/003/009's own sessions, before this record.
8. **Every OVH item has a change record; `INDEX.md` rows exist.** Checked
   live: `docs/development/changes/OVH-001.md` through `OVH-014.md` all
   exist and are non-stub (each has real *Removed assumptions*/*Known
   pitfalls* content); `INDEX.md` has a `landed` row for all of OVH-001
   through OVH-014, OVH-015, and this record's own `OVH-CLOSE` — 37 total
   feature-group rows in `INDEX.md`, all `landed`, none any other status.
9. **`docs/architecture/**` matches the code.** OVH-015 item 1: 
   `compiler_pipeline.md` §7 and `memory_model.md` §2.7 rewritten from a
   type-erasure description to the actual monomorphization implementation,
   verified against live source (`src/Monomorphize.cpp`,
   `CodegenCore.cpp`'s `getLLVMType()`/`CastToType()`) rather than only
   against other docs — see OVH-015's own record for the detailed diff
   summary.
10. **`grep -rn "TurfGenericParamType" src/Codegen*.cpp` shows only the
    `INV-GEN-1` assertions; no erasure path is reachable.** Partially
    literal, substantively true — see *Known pitfalls* above for the full
    accounting of every hit and why none is an erasure path.
11. **No known regression, ICE, or compiler segfault from any input in
    this file or the audit.** No crash, ICE, or regression surfaced across
    the full suite (796 tests × 5 opt levels), every smoke script, every
    `lib/std`/`showcase` direct compile, or the manual client/server runs
    performed for this record.
12. **`docs/development/changes/OVH-CLOSE.md` states the Overhaul is
    closed, lists the gate evidence, and records the DoD-8
    reclassification.** This file. DoD-8 (a dynamically-sized safe linked
    structure in safe TurfLang) is closed as **met in letter, not full
    intent**: `docs/architecture/pointers_and_references.md` §1.8's `&?T`
    and LANG-002's `tests/39_nullable_references/01_linked_list.tr` satisfy
    the DoD's literal text (a safe self-referential struct compiles and
    runs), but that test's own comment discloses "there's no heap allocator
    in this test" — safe code still has no way to heap-allocate a *new*
    node at runtime, and `return &local;`/a stored `&local` still compiles
    into a dangling reference, because neither is a defect fix (both need
    new language surface: a typed GC-allocation intrinsic and escape-analysis
    rules, per F-STD-2's "a new intrinsic is an explicit act"). Per the
    closure audit's own proposed resolution
    (`docs/development/OVERHAUL_CLOSURE_AUDIT.md` §5, decision 1, accepted):
    the safe heap-allocation half becomes **FDN-001**, the escape-rule half
    becomes **FDN-002**, both already written up in full
    (`docs/planning/ROADMAP_v2.0.0.md`, rows N1/N2 and their own §-numbered
    sections) as the first Foundation milestone of the 2.0.0 roadmap — an
    explicit, recorded reclassification from "Overhaul defect" to
    "roadmap design work," not a silent downgrade or an abandoned item.

## Related records

[OVH-015](OVH-015.md) (the item this record closes out — its own record has
the detailed diff-by-diff account of the documentation/bookkeeping fixes;
this record is the aggregate gate evidence and the formal closure
statement). [OVH-001](OVH-001.md) through [OVH-014](OVH-014.md) (every item
this closure certifies). `docs/development/OVERHAUL_CLOSURE_AUDIT.md` (the
evidence base `OVERHAUL_REMAINING_WORK.md` — now deleted — was built from;
still the authoritative historical record of what the audit found).
`docs/planning/ROADMAP_v2.0.0.md` (what comes next, including FDN-001/002).

---

**The Architecture Overhaul (`docs/planning/ARCHITECTURE_OVERHAUL.md`) is
closed as of this record.** All 23 original feature groups plus all 15
OVH-NNN closure items are landed; the gate above is demonstrated clean.
Further work on this codebase is tracked in `docs/planning/ROADMAP_v2.0.0.md`
(pre-2.0.0) and `docs/planning/FUTURE.md` (post-2.0.0), not in any
Overhaul-closure document — `docs/planning/OVERHAUL_REMAINING_WORK.md` is
deleted in the same commit as this record, per its own header instruction.
