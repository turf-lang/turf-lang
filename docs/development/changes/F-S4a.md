# F-S4a — Analysis leaves Codegen

The architecture item F-S4a (`docs/planning/ROADMAP_v3.0.0.md` Part 6, the
first step of the Turf IR, F-S4). Internal.

| | |
|---|---|
| **Status** | landed |
| **Dates** | 2026-11-02 |
| **Line** | 2.3.0 |
| **Depends on** | F-S2 (a compilation owns its state), AN-6's `AnalysisFacts` side table |

Issue #357.

## Why

Flow analysis ran inside `FuncDefExprAST::codegen()`, after each body was
generated, and its results reached Codegen as flags on AST nodes. So `--check`
had to generate code to report every diagnostic, the order of diagnostics
followed the order of code generation, and the call graph AN-3 (a) needs
existed only as a side effect of Codegen.

## How it works

- **`AnalysisFacts`** (`src/analysis/AnalysisFacts.h`), owned by a
  compilation like its symbol table, holds what analysis proves about nodes,
  keyed by node id: `ProvenNoOverflow` and `ProvenInBounds` (OPT-002 / NM-2
  check elision; Codegen asks `provenNoOverflow(id)` / `provenInBounds(id)`),
  the `int`-typed expressions (NM-2), the `int`-to-`double` conversions
  (AN-6), and which functions and declarations the phase below checked. No
  AST node carries an analysis flag.
- **The flow phase.** `runProgramFlowPasses()` (`src/analysis/Analysis.cpp`)
  runs in the driver after the TypeChecker and before any body is generated.
  Over every function reachable from the program -- top-level ones, those of
  imported modules, functions declared inside others, monomorphized clones --
  it lints the body, builds the CFG (kept in `GlobalCFGs`, which
  `MutualRecursion::report()` reads), runs `runFunctionFlowPasses()`, and
  checks shadowing (E152) with Codegen's own scope rules. A generic template
  and a `compile_error` test are left to Codegen, which checks such a function
  only when the phase did not (`AnalysisFacts::FlowChecked`,
  `ShadowChecked`).
- **`--check` generates no code.** It skips body generation and the main
  pass's top-level generation; struct layouts and prototypes are still
  registered (the flow passes look struct types up through them), and `main`'s
  prototype is the entry point it checks for.
- **Diagnostics before Codegen.** The errors Codegen raised for a program the
  TypeChecker accepted are raised earlier, so `--check` reports what a build
  does: E122 (the address of something not addressable) and E147 (a default
  value through a reference cycle) by the TypeChecker, E001 for a constant
  index written as literals, `-` and size parameters, and for a constant index
  into a reference to one value, by `ValueRangePass`. Codegen keeps its own
  checks as a backstop; the diagnostic engine reports an error at one place
  once.

## Verification

- `-O0` IR is byte-identical over the 1,638 programs of `tests/`, `showcase/`
  and `examples/` before and after, and every program reports the same
  diagnostic codes.
- `turfc --check` gives the same exit status and the same diagnostic codes as
  a build for every one of those programs.

## Known pitfalls

- A diagnostic raised only inside Codegen is invisible to `--check`. A new
  check belongs in the Resolver, the TypeChecker or the flow phase.
- The E001 cases added before Codegen read written constants only, never a
  variable: a program that compiled and panicked at run time on 2.0 must not
  become a compile error.
- Codegen's scope table and `ShadowWalker`'s must agree: a construct that
  opens a scope in Codegen (`Symbols()->EnterScope()`) opens one in the
  walker.
