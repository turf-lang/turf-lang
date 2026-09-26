# INDEX.md — every change record

The one place to look to answer *"what has changed since the handbook was
paused?"* One row per feature group, newest last. A row appears when the
record does; `status` tracks the record, not the branch.

| ID | Name | Status | Started | Landed | Subsystems | Record |
|---|---|---|---|---|---|---|
| ARCH-001 | Development change-tracking and semantic freeze | landed | 2026-09-17 | 2026-09-17 | documentation, CI | [ARCH-001](changes/ARCH-001.md) |
| TYPE-001 | Struct layout acyclicity | landed | 2026-09-18 | 2026-09-18 | Resolver, diagnostics | [TYPE-001](changes/TYPE-001.md) |
| SEM-001 | Literal representation and lexical numeric syntax | landed | 2026-09-18 | 2026-09-18 | Lexer, Parser, AST, TypeChecker, Codegen, ValueRangePass, Resolver, diagnostics | [SEM-001](changes/SEM-001.md) |
| OPT-001 | Optimization pipeline, levels, and module passes | landed | 2026-09-18 | 2026-09-18 | driver, LLVM integration | [OPT-001](changes/OPT-001.md) |
| TOOL-001 | Debug information (`-g` / DWARF) | landed | 2026-09-18 | 2026-09-18 | Codegen, driver, LLVM integration | [TOOL-001](changes/TOOL-001.md) |
| DIAG-001 | Structural error classification and the lint VarDecl gap | landed | 2026-09-18 | 2026-09-18 | diagnostics, SLM routing, Lint | [DIAG-001](changes/DIAG-001.md) |
| HYG-001 | Compiler hygiene and diagnostic-rendering consistency | landed | 2026-09-18 | 2026-09-18 | build headers, CFG diagnostics | [HYG-001](changes/HYG-001.md) |
| LANG-004 | Small language and FFI ergonomics | landed | 2026-09-18 | 2026-09-18 | Parser, Resolver, TypeChecker | [LANG-004](changes/LANG-004.md) |
| SEM-002 | Numeric semantics: representability, conversion, overflow | landed | 2026-09-19 | 2026-09-19 | TypeChecker, Codegen, diagnostics | [SEM-002](changes/SEM-002.md) |
| SEM-003 | `guess` (initializer type inference) | landed | 2026-09-20 | 2026-09-20 | Lexer, Parser, Types, TypeChecker, symbol tables | [SEM-003](changes/SEM-003.md) |
| SEM-004 | Unified initialization | landed | 2026-09-20 | 2026-09-20 | Parser, AST, Analysis (CFG), diagnostics, stdlib, tests | [SEM-004](changes/SEM-004.md) |
| TYPE-002 | Monomorphization (the keystone) | landed | 2026-09-21 | 2026-09-22 | Resolver, TypeChecker, new Monomorphizer pass, Codegen, symbol tables, driver, stdlib | [TYPE-002](changes/TYPE-002.md) |
| LANG-001 | Operator system completion | landed | 2026-09-23 | 2026-09-23 | Lexer, Parser, Resolver, TypeChecker, Codegen, stdlib | [LANG-001](changes/LANG-001.md) |
| LANG-002 | Nullable references `&?T` and declared capabilities | landed | 2026-09-23 | 2026-09-23 | Parser, Types, Resolver, Monomorphize, TypeChecker, Analysis (CFG), driver, stdlib | [LANG-002](changes/LANG-002.md) |
| LANG-003 | `defer` | landed | 2026-09-24 | 2026-09-24 | Lexer, Parser, AST, Resolver, TypeChecker, Analysis (CFG), Codegen, diagnostics | [LANG-003](changes/LANG-003.md) |
| RT-001 | `libturfrt`: the runtime library | landed | 2026-09-25 | 2026-09-25 | new `runtime/` (C), Codegen, driver, build, CMake, release packaging | [RT-001](changes/RT-001.md) |
| MEM-001 | Route managed allocations through the runtime | landed | 2026-09-25 | 2026-09-25 | `runtime/` (C), Builtins, Resolver, TypeChecker, Codegen, stdlib | [MEM-001](changes/MEM-001.md) |
| MEM-002 | The collector | landed | 2026-09-25 | 2026-09-25 | `runtime/` (C, new `turfrt_gc.c`), Builtins, Resolver, TypeChecker, Codegen, stdlib | [MEM-002](changes/MEM-002.md) |
| STD-001 | Boundary formalization | landed | 2026-09-25 | 2026-09-25 | documentation, tooling (Parser doc comments, new `boundary.md`, new `check_boundary.sh`) | [STD-001](changes/STD-001.md) |
| STD-002 | Standard-library diagnostics | landed | 2026-09-25 | 2026-09-25 | Lexer, Parser, AST, Resolver, TypeChecker, Codegen, diagnostics, stdlib | [STD-002](changes/STD-002.md) |
| STD-003 | Post-monomorphization standard-library modernization | landed | 2026-09-26 | 2026-09-26 | stdlib (new `hashing.tr`, `map.tr`/`set.tr` hash-index rewrite, `http.tr` simplification, `list.tr` generic sort/search), Resolver, TypeChecker, Monomorphizer (THREE independent compiler-bug fixes, see change record), documentation | [STD-003](changes/STD-003.md) |
| OPT-002 | Check elimination and frontend specialization | landed | 2026-09-26 | 2026-09-26 | CFG/analysis (new `ValueRangePass::markElidableChecks()`), Codegen, AST, tests, build (`CallGraphPass` deleted) | [OPT-002](changes/OPT-002.md) |
| SLM-001 | Diagnostic-assistant architecture | landed | 2026-09-26 | 2026-09-26 | diagnostics, SLM (deterministic-fix layer, type-context hints, on-disk cache), TypeChecker, Errors, new `scripts/slm_benchmark/`, tests | [SLM-001](changes/SLM-001.md) |
| OVH-001 | GC type-descriptor bitmap silently truncated at 64 words | landed | 2026-09-26 | 2026-09-26 | `runtime/` (C, `turfrt.h`/`turfrt_types.c`/`turfrt_gc.c`), Codegen (`CodegenStruct.cpp`), tests, `scripts/runtime_smoke.sh`, documentation | [OVH-001](changes/OVH-001.md) |
| OVH-002 | GC pointers hidden in untraced manual-heap memory | landed | 2026-09-26 | 2026-09-26 | stdlib (`json.tr`), showcase (`kv_store_server.tr`), Codegen (`CodegenCore.cpp`, `CodegenStruct.cpp`, `CodegenExpr.cpp`), new `scripts/check_boundary.sh` checks, tests, documentation | [OVH-002](changes/OVH-002.md) |
| OVH-003 | Raw-pointer indexing is ungated in safe code; no safe generic element access | landed | 2026-09-26 | 2026-09-26 | TypeChecker (`TypeCheckerCall.cpp`), Codegen (`CodegenStruct.cpp`, `CodegenCore.cpp` — two independent latent bugs found+fixed), stdlib (`list.tr`, `json.tr`, `http.tr`), showcase (`kv_store_server.tr`), tests, documentation | [OVH-003](changes/OVH-003.md) |
| OVH-004 | By-value cycle through a generic instantiation segfaults the compiler | landed | 2026-09-26 | 2026-09-26 | Resolver (`ResolverCore.cpp` — `byValueEdgeTarget()`), tests, documentation | [OVH-004](changes/OVH-004.md) |
| OVH-009 | `>>` closing nested generic arguments is a parse error | landed | 2026-09-26 | 2026-09-26 | Parser (`ParserCore.cpp` — `ParseType()`), tests, documentation | [OVH-009](changes/OVH-009.md) |
| OVH-010 | `mem::root`/`mem::unroot` never built | landed | 2026-09-26 | 2026-09-26 | Runtime (`turfrt.h`/`turfrt_gc.c` — pin table, `turf_rt_pin`/`turf_rt_unpin`), Intrinsics (`gc_pin`/`gc_unpin`), stdlib (`mem.tr`), tests, documentation | [OVH-010](changes/OVH-010.md) |
| OVH-011 | Direct compile of `lib/std/collections/list.tr` fails (E034) | landed | 2026-09-27 | 2026-09-27 | Parser (`ParserModule.cpp`/`main.cpp` — `ClaimEntryFileImportSlot()`), tests, `scripts/session_postflight.sh`, documentation | [OVH-011](changes/OVH-011.md) |
| OVH-012 | Lint false positives + stdlib warnings on every import | landed | 2026-09-27 | 2026-09-27 | Resolver (`ResolverCore.cpp`, `ResolverDecl.cpp` — `CollectReturnedErrorTypeNames()`, `ReturnsReboundError`), symbol tables (`ResolverSymbolBase.h`), CFG/analysis (`Analysis.cpp` — `AnalysisVisitor::visitRecoverExpr()`), diagnostics (`Lexer.cpp`/`Lexer.h` — stdlib-warning print-time gate), Parser (`ParserModule.cpp`/`Parser.h` — `IsPathUnderLibStd()`), driver (`main.cpp` — `--stdlib-warnings`), stdlib (`json.tr`), tests, documentation | [OVH-012](changes/OVH-012.md) |
| OVH-005 | `int32`: `++`/`--` ICE; untyped literal rejected; wrong-reason test | landed | 2026-09-26 | 2026-09-26 | TypeChecker (`TypeCheckerCore.cpp`, `TypeCheckerDecl.cpp`, `TypeCheckerExpr.cpp`, `TypeCheckerStmt.cpp`, `TypeCheckerCall.cpp`), Codegen (`CodegenVar.cpp` — debug assertion), tests, documentation | [OVH-005](changes/OVH-005.md) |
| OVH-006 | Generic type-parameter names captured by user type names | landed | 2026-09-26 | 2026-09-26 | Resolver (`ResolverDecl.cpp` — `visitStructDecl()`), TypeChecker/Codegen doc comments (`TypeCheckerDecl.cpp`, `TypeCheckerCall.cpp`), tests, documentation | [OVH-006](changes/OVH-006.md) |
| OVH-007 | Chained field access through a nested generic struct field fails | landed | 2026-09-26 | 2026-09-26 | Codegen (`CodegenCore.cpp` — `getExprType()`'s `FieldAccessExprAST` Subst-recovery), tests, documentation | [OVH-007](changes/OVH-007.md) |
| OVH-008 | `&T` → `&?T` widening missing in generic candidate matching | landed | 2026-09-26 | 2026-09-26 | Types (`Types.cpp` — `typesCompatible()` Rule 2.5/2.6), tests, documentation | [OVH-008](changes/OVH-008.md) |

## How to use this directory

| File | What it is |
|---|---|
| [`FREEZE.md`](FREEZE.md) | The frozen semantic decisions. **Read first, every session.** |
| [`SESSION_PROTOCOL.md`](SESSION_PROTOCOL.md) | How a session runs, start to finish. Enforced by `scripts/session_{pre,post}flight.sh`. |
| [`INVARIANTS.md`](INVARIANTS.md) | Named invariants, what enforces each, and whether it holds yet. |
| [`TEMPLATE.md`](TEMPLATE.md) | The change-record template. Copy it at the *start* of a session. |
| [`changes/`](changes/) | One record per feature group. |
| [`handbook-sync/`](handbook-sync/) | The queue of handbook/reference edits owed, and the archive of completed ones. |
| [`OVERHAUL_CLOSURE_AUDIT.md`](OVERHAUL_CLOSURE_AUDIT.md) | The 2026-09-26 final audit of the Overhaul: what passed, what failed (with reproducers), and why closure is pending on `docs/planning/OVERHAUL_REMAINING_WORK.md`. |

`docs/planning/` (roadmap, design notes, limitations) and `docs/architecture/`
(subsystem internals) keep their existing roles unchanged. This directory is
**additive** and is specifically about *change over time*, which neither of
the others tracks.

## Feature IDs not yet started

The full set is `docs/planning/ARCHITECTURE_OVERHAUL.md` Appendix B. An ID
with no row above has no record yet — and, deliberately, no stub file either:
`scripts/doc_links_allowlist.txt` carries the blueprint's forward references
until each session writes its own record, so nothing here pretends to be
documentation that has not been written.
