# F-S2 — No global symbol table

The architecture item F-S2 (`docs/planning/ROADMAP_v3.0.0.md` Part 6,
`docs/planning/ARCHITECTURE_SIMPLIFICATION.md` phase 2). Internal.

| | |
|---|---|
| **Status** | landed |
| **Dates** | 2026-11-01 |
| **Line** | 2.3.0 |
| **Depends on** | F-S0 `LexerState`, F-S1 `ParserState` (the same owned-state pattern) |

Issue #356.

## Why

`GlobalSymbolTable` was one `std::unique_ptr<SymbolTable>` for the whole
process. Every in-process analysis (a compiler fix's re-check, the `--smart`
sandbox, the query facade turf-lsp calls) had to move the host compile's table
out of the way, build a new one with `InitializeCompilerGlobals()`, and move the
old one back afterwards. A concurrent LSP, F-7a's separate compilation, CC-1's
global-state audit and DX-8 all need the table to belong to a compilation.

## What changed

- `SymbolTable` instances are owned by whoever compiles: `runCompilerMain()`
  (the driver's compile), and each analysis the query facade runs
  (`GlobalStateScope` in `src/tooling/AnalysisFacade.cpp` owns one and hands it
  to the `AnalysisResult`, whose inferred types point into it).
- `Symbols()` (`src/resolve/SymbolTable.h`) returns the current compilation's
  table: the innermost live `SymbolTableScope`'s, null outside any. The
  Parser, Resolver, TypeChecker, monomorphizer, flow analysis and Codegen read
  it there. This is the `Parser()`/`ParserScope` pattern of F-S1; no instance
  is global or static.
- `InitializeCompilerGlobals()` and `InitializeSymbolTable()` are gone. An
  embedder that drives the pipeline directly declares a `SymbolTable` and a
  `SymbolTableScope` (the API tests do).
- A nested analysis gets its own table and the outer one is current again when
  it ends, so the facade no longer saves and restores the symbol table. The
  `--smart` sandbox's never-filled `SymbolTableCopy` and the dead line-edit
  updater that wrote into it are deleted.

## What did not change

The facade's `GlobalStateScope` still saves and restores the other process
globals an analysis touches (the diagnostic list, source lines, the test
registry, the package graph, the resolver's compile state). Those are F-S3's
`SemanticModel` work, not the symbol table.

## Evidence

- Emitted IR is unchanged: the rename is mechanical, and the table a compile
  reads is the one it filled, as before.
- The suite at every level, `turf-api-tests` (host state restored, two parsers
  interleaving, cancelled analyses), the compat corpus, the replay smokes and
  the consumer fixtures pass.
