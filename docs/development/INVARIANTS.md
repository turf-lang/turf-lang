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
| **INV-TYPE-2** | For monomorphized nodes, the AST carries concrete types, so `TypeChecker::NodeTypes_` and `Codegen::getExprType()` agree by construction rather than by coincidence. | Monomorphizer writing types onto the cloned AST (TYPE-002, §9.7) | `holds` — **TYPE-002**, S14a (2026-09-21). Every declaration that reaches Codegen is now either ordinarily concrete (never generic to begin with) or a monomorphized clone whose Params/ReturnType/body were fully substituted by `Monomorphize.cpp` before being appended to `PrePassASTs` — a generic TEMPLATE never reaches Codegen at all (`FuncDefExprAST::codegen()`/`StructDeclExprAST::codegen()` both skip a non-empty `TypeParams` unconditionally, `CodegenFunc.cpp`/`CodegenStruct.cpp`). Full unification of the two inference engines is still not attempted (§25 item 4 is about a different thing than this) — this invariant only claims agreement for monomorphized nodes specifically, which is what it names. |
| **INV-GEN-1** | After the monomorphizer, no `TurfGenericParamType` exists anywhere; reaching Codegen with one is a compiler bug. | Assertion at the monomorphizer's final step, plus a Codegen assert | `holds` — **TYPE-002**, S14a (2026-09-21), with a FOURTH gap closed by **S15/D6** (2026-09-22). Asserted in three places: `Monomorphize.cpp`'s `finishInstantiation()` (a monomorphized clone's own Params/ReturnType, right after substitution), `CodegenCore.cpp`'s `getLLVMType()`, and `CodegenCore.cpp`'s `CastToType()` — all debug-build `assert()`s, never removed. Erasure's own `CastToType()`/`getLLVMType()` branches (the ones that used to silently tolerate a `TurfGenericParamType` by erasing it to a pointer) are deleted outright, not left dead. Verified live: closing this invariant required TWO real, previously-latent gaps to surface and get fixed first (not anticipated by the original session plan) — `StructName::new(...)` constructors and a method generic in its own right on a non-generic struct were both still reaching Codegen un-monomorphized; see `docs/development/changes/TYPE-002.md`'s own *Removed assumptions* for the full story, including a THIRD, narrower gap (a generic function calling another generic function through its own still-open type parameter recorded a non-concrete "instantiation" that was harmless under erasure but crashed the new assertion once erasure was gone). **S15 found and closed a fourth**, independently: a method's OWN type parameter declared ALONGSIDE its generic struct's (`fn<T, U> Wrap.pick(other: U): U` on `struct Wrap<T>`) tripped this exact assertion in a debug build (`Internal compiler error: generated LLVM IR is invalid` in release) — the recorded Method-kind instantiation request only ever carried the RECEIVER's own type arguments (`[int]` for `Wrap<int>`), silently dropping `U`'s value, so the clone's own `U` parameter never got substituted. This was NOT a hole in S14a's own three assertion sites (they fired correctly, catching it) — it was a gap in what fed them, now closed; the row's `holds` status was briefly, unknowingly false for this specific shape between S14a and this fix. See TYPE-002.md's own D6 entry for the full mechanism. |

## Numbers and constants

| ID | Invariant | Enforced by | Status |
|---|---|---|---|
| **INV-NUM-1** | `bool` never participates in numeric coercion, implicit or explicit. | `typesCompatible()` Rule 4 | `holds (audited)` — deliberate, not an omission (F-NUM-4's neighbourhood; see FREEZE.md §6.2). |
| **INV-NUM-2** | A literal's value is never lost between the lexer and the type check, and an out-of-range value is a hard error at the point its destination type is fixed. | Arbitrary-precision literal representation (SEM-001, landed) + `TypeChecker::checkLiteralRepresentable()` at every type-fixing site (SEM-002, landed) | `holds` — **SEM-002**, landed 2026-09-19. Verified live: `mut x: int = 999999999999999999999999999999999999999999999999;` is now `E076` (was: silent 64-bit truncation); `9223372036854775807 + 1` (both untyped literals) is `E076` too, via `tryFoldUntypedIntConstant()`'s arbitrary-precision fold, checked only at the point the result's destination is fixed (FREEZE.md §6.2 row 6). Narrower than the frozen ideal in one disclosed way: a literal nested inside a larger literal-only arithmetic expression is rejected the moment it fails to fit int64 on its own, rather than deferred to "maybe the whole expression's final value still fits" — see SEM-002's change record, Known pitfalls, for why (Codegen truncates every literal through 64 bits before any arithmetic runs, unconditionally, since SEM-001). |
| **INV-CONST-1** | All compile-time arithmetic uses checked, arbitrary-precision operations. | `evalConstExpr` on checked `APInt` ops (`sadd_ov`/`ssub_ov`/`smul_ov`/`sdiv_ov`) (SEM-001, landed) | `holds` — **SEM-001**, landed 2026-09-18. `ValueRange.cpp`'s `evalConstExpr` no longer performs `lv + rv` on plain `long long` (the confirmed signed-overflow UB this row was about) -- every binary op is now a checked 65-bit `APInt` operation, with `Overflow` (or a result that no longer fits `isSignedIntN(64)`) collapsing to the same conservative `nullopt`/"unknown" fallback this pass already used for division by zero. 65 bits, not literally unbounded, is a deliberate choice, not a shortfall: this pass's own domain (`std::optional<long long>`, array-index/loop-bound constant folding) is bounded to `long long` range by construction, so one guard bit above 64 is sufficient to detect every provable overflow in the values it ever handles -- see this fix's own comment in ValueRange.cpp for why. |
| **INV-NUM-3** | Runtime signed integer arithmetic on `int` never wraps silently: `+ - * <<` and unary negation trap (panic) on overflow; `/` (and, alongside its existing zero-check, `%`) traps on the one signed-division overflow case, `INT64_MIN / -1`. | `EmitCheckedIntArith`/`EmitCheckedIntNegate`/`EmitCheckedShl`/`EmitSignedDivOverflowGuard` (`CodegenCore.cpp`), called from `BinaryExprAST`/`UnaryExprAST` (`CodegenExpr.cpp`) and independently from `CompoundAssignmentExprAST`/`PostfixExprAST` (`CodegenVar.cpp`, which do not desugar through the former) | `holds` — **SEM-002**, landed 2026-09-19. Verified live for all four codegen paths (a plain expression, `+=`-family compound assignment, `x++`/`x--`, and unary `-`) and all five operators, plus a same-session regression pass confirming ordinary in-range arithmetic across every one of them does not trap (`tests/36_numeric_semantics/03_no_false_positive_overflow_traps.tr`). Scoped to `int` only — `int32` arithmetic overflow is not checked (SEM-002's own text names only `int`; not in this group's file list). `double` deliberately keeps wrapping to IEEE `inf`/`-inf` with no trap (FREEZE.md §6.2 row 7's asymmetry, on purpose). Does not cover a *typed constant reference* whose value is compile-time-knowable (e.g. `immut a: int = INT64_MAX; immut b: int = a + 1;`) as a COMPILE-time error the way FREEZE.md §6.2 row 6 ("typed constants: error") frames it — that case is still safely caught, just at runtime via this same trap, since `a` is an ordinary `VariableExprAST` to `tryFoldUntypedIntConstant()`, not a literal; full `immut`-value constant-folding across variable references is a disclosed, deferred gap (see change record). |
| **INV-CAST-1** | `to`-cast legality is enforced in the TypeChecker, not Codegen — Codegen only ever receives a cast it can already dispatch on cleanly. | `TypeChecker::visitCast()`'s pairwise legality check (`TypeCheckerExpr.cpp`, mirrors `CastExprAST::codegen()`'s dispatch pair-for-pair) | `holds` — **SEM-002**, landed 2026-09-19 (Rule B1). Codegen's own final "Everything else is unsupported" `TypeError` (`CodegenExpr.cpp`) is kept as a defensive, should-be-unreachable fallback rather than deleted — seeded from the same two-type-inference-engines duality risk `docs/architecture/compiler_pipeline.md` §4 documents (TypeChecker's `NodeTypes_` vs. Codegen's own `getExprType()`); if the two ever disagree about an operand's type for some cast shape neither this session's full-suite/`lib/std`/`showcase` sweep nor its own new tests exercise, Codegen's fallback is what stops a silently-wrong cast rather than a crash. |

## Initialization

| ID | Invariant | Enforced by | Status |
|---|---|---|---|
| **INV-INIT-1** | Exactly one initialization rule, for every type: definite assignment. | `DefiniteAssignmentPass` over all body-declared locals (`Analysis.cpp`) — `ParseLetStatement()` (`ParserStmt.cpp`) no longer synthesizes a default `Init` for any type, so every body-declared local without an explicit initializer falls through to the pass uniformly | `holds` — **SEM-004**, landed 2026-09-20. Verified live: `mut c: char;`/`mut n: int32;`/`mut e: SomeEnum;`/`mut s: SomeStruct;`/`mut l: List<int>;` all now raise `E008` on a read before assignment (previously silently zero-initialized and immediately readable — Mechanism B), matching what already held for `int`/`double`/`bool`/`string`/pointers. `DefaultInitExprAST` (the AST node Mechanism B lowered through) is deleted outright, not left dead — nothing constructs it any more. Arrays get the coarser granularity §12.6 specifies: a whole-array read with no prior element write or initializer is `E008`, but a single element write (`arr[0] = 1`) counts as assigning the WHOLE array (elements are not tracked individually, mirroring `ConstructorFieldAssignmentPass`'s existing field-level granularity for `self.<field>`) — verified live for both a primitive array and an array-of-struct. `lib/std/**` and `showcase/**` swept clean by direct compile (`http.tr`/`json.tr`/`kv_store_server.tr` needed migrating to explicit `T::default()`); the generic-typed-local migration path is the bare, unqualified `List::default()`/`Map::default()` (type arguments come from the declared LHS type — type erasure means one shared struct backs every instantiation) since `List<T>::default()` with explicit type arguments on the call itself does not parse (`<...>` is only understood by `ParseType()`, not by expression parsing) — a disclosed, pre-existing parser gap this group did not fix (out of scope; see change record). |

## Diagnostics and analysis

| ID | Invariant | Enforced by | Status |
|---|---|---|---|
| **INV-LINT-1** | `LintExpression`'s dispatch is exhaustive over statement-shaped nodes — a newly added node kind cannot be silently skipped. | An exhaustive switch over a node-kind enum, or a visitor base that reports an unhandled kind in debug builds | `planned` — the dispatch is still a sequence of independent `dynamic_cast` checks, not an exhaustive switch/visitor, so a *future* missing node kind would still be silently skipped with nothing to catch it; that refactor remains unbuilt. The one concrete instance this invariant was written to describe (`VarDeclExprAST`'s initializer silently skipped, `mutable x: int = 5 / 0;` warning about nothing) is fixed as of **DIAG-001**, landed 2026-09-18 — verified live — but is a point fix, not the structural enforcement this row names. See DIAG-001's change record, *Known pitfalls*. |
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
| **INV-TEST-1** | A string-keyed collection test builds its key at runtime, never relying on two identical string literals being distinct objects. | Test-authoring convention, documented in the folder | `holds` — **TYPE-002**, S14b (2026-09-21). `tests/38_monomorphization/README.md` documents the rule; `10_set_string_dedup.tr`/`11_map_string_key_equality.tr` are its own worked examples, both building their duplicate key/item via `strings::trim()`, never a second string literal. This invariant already let a real bug pass undetected once: linker literal-merging made `tests/07_functions_modules/09_stdlib_map.tr` pass while the underlying bug (`Map`/`Set` generic string equality, `docs/planning/LIMITATIONS.md` item 1) was still present — that bug is now itself fixed (TYPE-002), independently of this invariant, but the convention remains load-bearing for catching any regression. |
