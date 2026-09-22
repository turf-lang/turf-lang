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

## How to use this directory

| File | What it is |
|---|---|
| [`FREEZE.md`](FREEZE.md) | The frozen semantic decisions. **Read first, every session.** |
| [`SESSION_PROTOCOL.md`](SESSION_PROTOCOL.md) | How a session runs, start to finish. Enforced by `scripts/session_{pre,post}flight.sh`. |
| [`INVARIANTS.md`](INVARIANTS.md) | Named invariants, what enforces each, and whether it holds yet. |
| [`TEMPLATE.md`](TEMPLATE.md) | The change-record template. Copy it at the *start* of a session. |
| [`changes/`](changes/) | One record per feature group. |
| [`handbook-sync/`](handbook-sync/) | The queue of handbook/reference edits owed, and the archive of completed ones. |

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
