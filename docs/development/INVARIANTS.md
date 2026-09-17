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
| **INV-LAYOUT-1** | Every registered `TurfStructType` has a finite, computable size: no struct contains itself by value, transitively. | Containment-graph cycle check at the end of Resolver Pass 1 (TYPE-001) | `planned` — **TYPE-001**. Verified still broken 2026-09-17: `struct A { me: A }` alone compiles fine; adding `mutable x: A;` crashes **the compiler itself** with SIGSEGV (exit 139), and does so as early as `turf --resolve`, so the fix belongs in the Resolver, before any layout or default value is computed. |
| **INV-TYPE-1** | Type *identity* never comes from `getName()` (a display string); use `isEquivalent()`. `int[5]` and `int[10]` are not the same type. | `typesCompatible()`, `CastToType()` | `holds (audited)` — the original failure was a reproduced stack buffer overflow (80 bytes stored into a 40-byte alloca). |
| **INV-TYPE-2** | For monomorphized nodes, the AST carries concrete types, so `TypeChecker::NodeTypes_` and `Codegen::getExprType()` agree by construction rather than by coincidence. | Monomorphizer writing types onto the cloned AST (TYPE-002, §9.7) | `planned` — **TYPE-002**. Full unification of the two inference engines is explicitly rejected (§25 item 4). |
| **INV-GEN-1** | After the monomorphizer, no `TurfGenericParamType` exists anywhere; reaching Codegen with one is a compiler bug. | Assertion at the monomorphizer's final step, plus a Codegen assert | `planned` — **TYPE-002**. |

## Numbers and constants

| ID | Invariant | Enforced by | Status |
|---|---|---|---|
| **INV-NUM-1** | `bool` never participates in numeric coercion, implicit or explicit. | `typesCompatible()` Rule 4 | `holds (audited)` — deliberate, not an omission (F-NUM-4's neighbourhood; see FREEZE.md §6.2). |
| **INV-NUM-2** | A literal's value is never lost between the lexer and the type check. | Arbitrary-precision literal representation (SEM-001) + representability checks at every type-fixing site (SEM-002) | `planned` — **SEM-001/002**. Verified still broken 2026-09-17: `mutable x: int = 999999999999999999999999999999;` compiles without a diagnostic and prints `9223372036854775807`. |
| **INV-CONST-1** | All compile-time arithmetic uses checked, arbitrary-precision operations. | `evalConstExpr` on `APSInt` (SEM-001) | `planned` — **SEM-001**. Today `ValueRange.cpp` does `lv + rv` on `long long`: signed-overflow UB in the compiler's own C++. |

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

## Testing conventions

| ID | Invariant | Enforced by | Status |
|---|---|---|---|
| **INV-TEST-1** | A string-keyed collection test builds its key at runtime, never relying on two identical string literals being distinct objects. | Test-authoring convention, documented in the folder | `planned` — **TYPE-002's tests**. This one already let a real bug pass undetected: linker literal-merging made `tests/07_functions_modules/09_stdlib_map.tr` pass while the bug was present. |
