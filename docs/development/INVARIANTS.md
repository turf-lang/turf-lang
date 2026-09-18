# INVARIANTS.md — named, checkable invariants

Each row is a property the compiler is supposed to keep true, the thing that
**enforces** it (an assertion, a pass, a type-level construction — a test
alone is not enforcement), and whether it holds today.

Why named invariants rather than a list of fixed bugs: a bug fix protects one
input, an invariant protects a class. `docs/planning/ARCHITECTURE_OVERHAUL.md`
§19 derives this list from the handbook's `pitfall` blocks plus the audit —
each pitfall was asked "what property, structurally enforced, would have made
this impossible?"

**Status vocabulary**

| Status | Meaning |
|---|---|
| `holds` | true today, with the enforcing mechanism named and the claim checked |
| `holds (audited)` | recorded as fixed by the §19 audit; not independently re-verified here |
| `planned` | not true yet; the feature group that will make it true is named |

An invariant moves to `holds` only in the change record of the session that
built the thing enforcing it, and that session demonstrates it — per
`SESSION_PROTOCOL.md`, by compiling the input that would prove it wrong.

---

## Type system and layout

| ID | Invariant | Enforced by | Status |
|---|---|---|---|
| **INV-LAYOUT-1** | Every registered `TurfStructType` has a finite, computable size: no struct contains itself by value, transitively. | `Resolver::checkStructLayoutAcyclicity()`, called at the end of Pass 1 hoisting (`ResolverCore.cpp`) — grey/black DFS over the by-value containment graph, raising `StructLayoutCycleError` (E069) on the first cycle | `holds` — **TYPE-001**, landed 2026-09-18. Verified live: `struct A { me: A }` (direct), `struct A { b: B } struct B { a: A }` (mutual), `struct A { bs: B[2] } struct B { a: A }` (array-mediated), and `struct Box<T> { inner: Box<T> }` (generic template-level) are all now rejected with the full cycle path named, instead of the pre-fix SIGSEGV (`mut x: A;` on the direct self-cycle exited 139). Pointer fields (`T*`/`&T`/`&?T`) remain non-edges and untested by this check — see `tests/06_structs/08_self_ref_struct.tr`/`09_linked_list.tr`, still green. |
| **INV-TYPE-1** | Type *identity* never comes from `getName()` (a display string); use `isEquivalent()`. `int[5]` and `int[10]` are not the same type. | `typesCompatible()`, `CastToType()` | `holds (audited)` — the original failure was a reproduced stack buffer overflow (80 bytes stored into a 40-byte alloca). |
| **INV-TYPE-2** | For monomorphized nodes, the AST carries concrete types, so `TypeChecker::NodeTypes_` and `Codegen::getExprType()` agree by construction rather than by coincidence. | Monomorphizer writing types onto the cloned AST (TYPE-002, §9.7) | `planned` — **TYPE-002**. Full unification of the two inference engines is explicitly rejected (§25 item 4). |
| **INV-GEN-1** | After the monomorphizer, no `TurfGenericParamType` exists anywhere; reaching Codegen with one is a compiler bug. | Assertion at the monomorphizer's final step, plus a Codegen assert | `planned` — **TYPE-002**. |

## Numbers and constants

| ID | Invariant | Enforced by | Status |
|---|---|---|---|
| **INV-NUM-1** | `bool` never participates in numeric coercion, implicit or explicit. | `typesCompatible()` Rule 4 | `holds (audited)` — deliberate, not an omission (F-NUM-4's neighbourhood; see FREEZE.md §6.2). |
| **INV-NUM-2** | A literal's value is never lost between the lexer and the type check. | Arbitrary-precision literal representation (SEM-001, landed) + representability checks at every type-fixing site (SEM-002, not yet) | `planned` — **SEM-002**. The "never lost" HALF this row names is done: the lexer/`NumberExprAST` now carry a literal's exact value (`llvm::APSInt`/`APFloat`), verified live 2026-09-18 -- `mut x: int = 999999999999999999999999999999999999999999999999;` (48 digits) still compiles (representability isn't checked yet, by design) but `--dump-ast` shows the value survives EXACTLY, unrounded. What SEM-002 still owes: turning an out-of-range value into a hard error at the point its destination type is fixed, instead of the silent 64-bit truncation Codegen still performs today (deliberately, matching pre-SEM-001 observable behavior until SEM-002 lands -- see `NumberExprAST::codegen()`'s own comment, CodegenExpr.cpp). |
| **INV-CONST-1** | All compile-time arithmetic uses checked, arbitrary-precision operations. | `evalConstExpr` on checked `APInt` ops (`sadd_ov`/`ssub_ov`/`smul_ov`/`sdiv_ov`) (SEM-001, landed) | `holds` — **SEM-001**, landed 2026-09-18. `ValueRange.cpp`'s `evalConstExpr` no longer performs `lv + rv` on plain `long long` (the confirmed signed-overflow UB this row was about) -- every binary op is now a checked 65-bit `APInt` operation, with `Overflow` (or a result that no longer fits `isSignedIntN(64)`) collapsing to the same conservative `nullopt`/"unknown" fallback this pass already used for division by zero. 65 bits, not literally unbounded, is a deliberate choice, not a shortfall: this pass's own domain (`std::optional<long long>`, array-index/loop-bound constant folding) is bounded to `long long` range by construction, so one guard bit above 64 is sufficient to detect every provable overflow in the values it ever handles -- see this fix's own comment in ValueRange.cpp for why. |

## Initialization

| ID | Invariant | Enforced by | Status |
|---|---|---|---|
| **INV-INIT-1** | Exactly one initialization rule, for every type: definite assignment. | `DefiniteAssignmentPass` over all body-declared locals (SEM-004) | `planned` — **SEM-004** (F-INIT-1). Two mechanisms exist today; the split is the language's single most commonly documented gotcha. |

## Diagnostics and analysis

| ID | Invariant | Enforced by | Status |
|---|---|---|---|
| **INV-LINT-1** | `LintExpression`'s dispatch is exhaustive over statement-shaped nodes — a newly added node kind cannot be silently skipped. | An exhaustive switch over a node-kind enum, or a visitor base that reports an unhandled kind in debug builds (DIAG-001) | `planned` — **DIAG-001**. Today there is no `VarDeclExprAST` branch, so every lint check is skipped for the whole right-hand side of every declaration. |
| **INV-LEX-1** | Maximal munch is uniform; no operator gets a special case. | Lexer | `holds (audited)` — this is why `---x` lexes as `--` then `-x`, and that is intended. |

## Capabilities and the standard library

| ID | Invariant | Enforced by | Status |
|---|---|---|---|
| **INV-CAP-1** | No capability is granted by file path. | A declared module-level capability + `--allow-unsafe-modules` (LANG-002/STD-001) | `planned` — **LANG-002/STD-001**. Today `isUnsafeGateExempt()` returns true for anything under `lib/std`. |

## Code generation

| ID | Invariant | Enforced by | Status |
|---|---|---|---|
| **INV-CG-1** | No GEP is built against a struct type whose `setBody()` has not run. | An assertion at GEP construction in `CodegenStruct.cpp` — **not** by relying on `main.cpp`'s pre-pass ordering | `planned` — the original bug is fixed, but the hazard is still only prevented by pass ordering. |

## Optimization

| ID | Invariant | Enforced by | Status |
|---|---|---|---|
| **INV-OPT-1** | Optimization levels never change a program's observable behaviour (stdout, exit code) — only compile time, run time, binary size, and debuggability. | `scripts/opt_levels_smoke.sh` (diffs a deterministic program set byte-for-byte across `-O0`..`-O3`/`-Os`) + the structural fact that Codegen never receives an opt level at all, so no language-mandated runtime check (bounds, division-by-zero) can be gated on one | `holds` — **OPT-001**, landed 2026-09-18. Verified live: `scripts/opt_levels_smoke.sh` clean (13/13 programs), and bounds-check/division-by-zero traps fire identically (exit 127 / exit 134) at every level on two throwaway probe programs. `grep -rln "OptLevel\|opts\." src/Codegen*.cpp include/Codegen*.h` returns no match, confirming Codegen cannot see the level even if a future change wanted it to. |

## Debug information

| ID | Invariant | Enforced by | Status |
|---|---|---|---|
| **INV-DEBUG-1** | `-g` never changes a program's observable behaviour (stdout, exit code) at any optimization level. | Codegen only reads existing `SourceLocation`/`TurfType` data to build debug metadata -- it never branches program logic on `g_EmitDebugInfo` -- plus `scripts/debuginfo_smoke.sh` and a manual `-g`-vs-no-`-g` byte-for-byte diff across `tests/08_e2e_stress/*.tr` | `holds` -- **TOOL-001**, landed 2026-09-18. |
| **INV-DEBUG-2** | Every object file `turf` emits with `-g` passes `llvm-dwarfdump --verify` with no errors. | `DBuilder->finalize()` running before `verifyModule()` (`main.cpp`) + `"Dwarf Version"` pinned to 4, not LLVM's newer DWARF5 default (see TOOL-001's change record, Known pitfalls, for why 5 fails to link cleanly on this toolchain's `ld`) | `holds` -- **TOOL-001**, landed 2026-09-18. Verified live against every `showcase/*.tr` program and `tests/08_e2e_stress/*.tr`. |

## Testing conventions

| ID | Invariant | Enforced by | Status |
|---|---|---|---|
| **INV-TEST-1** | A string-keyed collection test builds its key at runtime, never relying on two identical string literals being distinct objects. | Test-authoring convention, documented in the folder | `planned` — **TYPE-002's tests**. This one already let a real bug pass undetected: linker literal-merging made `tests/07_functions_modules/09_stdlib_map.tr` pass while the bug was present. |
