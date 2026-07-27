# Turflang CFG & Static Analysis Roadmap

This document outlines the strategic roadmap for expanding Turflang's Control Flow Graph (CFG) to support advanced static analysis, safety guarantees, and god-tier optimizations. 

Because implementing all of these simultaneously would be chaotic and difficult to debug, they should be implemented sequentially. Each feature builds upon the infrastructure of the previous one.

> **STATUS (2026-07 review): Real CFG infrastructure exists (`include/CFG.h`/`src/CFG.cpp`,
> `include/Analysis.h` `DefiniteAssignmentPass`, `include/ValueRange.h`/`src/ValueRange.cpp`
> `ValueRangePass`, `src/AliasAnalysis.cpp` `AliasAnalysisPass`, `include/CallGraph.h`/
> `src/CallGraph.cpp`), wired into `Codegen.cpp` per function — but it solves a
> DIFFERENT, more targeted set of problems than this doc originally proposed, not
> the specific checks below. None of the 4 phases as WRITTEN are complete;
> what actually shipped instead:
> - `CFG`: unreachable-code warnings, dead-branch warnings, missing-return errors,
>   statement-after-terminator errors. No cyclomatic complexity metric, no
>   infinite-loop-without-break detection (Phase 1 as specced: not done).
> - `DefiniteAssignmentPass`: use-before-initialization errors. Related to but
>   distinct from Phase 2's "dead store / unused variable" liveness pass
>   (which does not exist) — this checks reads-before-writes, not
>   writes-without-reads.
> - `AliasAnalysisPass`: use-after-free and double-free errors (tracks a
>   malloc'd pointer's points-to/freed state through the CFG). Stricter and
>   arguably more valuable than Phase 3's originally-proposed "warn if a
>   malloc is never freed", but it's a different check — no leak warning,
>   no `NullabilityPass`/nullable-pointer-dereference tracking exists.
> - `ValueRangePass`: constant-propagation only right now, per its own doc
>   comment ("currently used for diagnostics and later passes") — Phase 4's
>   bounds-check-elimination optimization is not built on top of it yet.
> - `CallGraphPass::runGlobal()` is a literal empty stub ("Function is
>   currently just a stub.") — infinite-recursion detection (Phase 2, item 2)
>   is not implemented.
>
> Left as an open roadmap, not closed — genuinely still future work, just
> re-scope against what's already built before picking it back up.

---

## Phase 1: Complexity & Basic Traversal
*Goal: Expand basic graph reading capabilities without complex data-flow mathematics.*

### 1. Cyclomatic Complexity Monitor
**Description:** Warn the user when a function becomes too difficult to read or maintain.
**Implementation:**
- Add `int CFG::calculateCyclomaticComplexity() const`.
- Use the standard formula: `M = E - N + 2P` 
  - `E` = Number of edges (count all `Successors` across all blocks)
  - `N` = Number of nodes (count of `Blocks`)
  - `P` = 1 (Connected components, which is always 1 for a single function CFG).
- **Trigger:** In `Codegen.cpp`, after building the CFG, call this method. If `M > 15`, trigger a `CompilerWarning(Loc, "Function is too complex...")`.

### 2. Infinite Loop Detection
**Description:** Catch `while(true)` loops that have no exit path.
**Implementation:**
- Add `bool CFG::hasUnbreakableLoops() const`.
- Traverse the graph to find cycles (using Depth-First Search for back-edges).
- Check if any block within the detected cycle contains a `TerminatorKind::Break` or `TerminatorKind::Return`.
- **Trigger:** If a cycle has no such terminator, emit a `CompilerWarning`.

---

## Phase 2: Liveness & Definite Assignment
*Goal: Track the lifecycle of variables to prevent uninitialized usage and catch dead code.*

### 1. The Liveness Pass (Dead Stores & Unused Variables)
**Description:** Warn the user about variables that are assigned but never read.
**Implementation:**
- Create `class LivenessAnalysisPass`.
- **Data Structure:** Each `TurfBasicBlock` needs a `Gen` set (variables read before being written) and a `Kill` set (variables written to).
- **Algorithm:** Run backward data-flow analysis to compute `LiveIn` and `LiveOut` sets for every block until they converge.
- **Trigger:**
  - If a variable is in the `Kill` set of a statement but not in the `LiveOut` set of the block (and never used later in the block), it is a **Dead Store** (assigned but never read).
  - If a variable is declared but its global `LiveIn`/`LiveOut` tracks show it is never read anywhere, it is an **Unused Variable**.

### 2. Infinite Recursion Detection (Call Graph + CFG)
**Description:** Prevent stack overflows at compile-time.
**Implementation:**
- During CFG construction, tag basic blocks that contain a function call to the *current* function.
- Compute all paths from `EntryBlock` to `ExitBlock`.
- If *every single path* passes through a recursive-call block, then there is no base case.
- **Trigger:** Emit a fatal error: *"Infinite recursion detected: no exit path exists without recursing."*

---

## Phase 3: Memory Safety (Alias Analysis)
*Goal: Prevent C-style memory leaks and null-pointer crashes without needing a Garbage Collector.*

### 1. Pointer Nullability State
**Description:** Track if a pointer might be null to prevent unsafe dereferencing.
**Implementation:**
- Create `class NullabilityPass`.
- Track the state of variables as `NonNull`, `Nullable`, or `Null`.
- If an `IfExprAST` checks `ptr != null`, update the state to `NonNull` for the `then` branch, and `Null` for the `else` branch.
- **Trigger:** If an `ArrayAccessExprAST` or dereference operation occurs while the state is `Nullable` or `Null`, throw an error: *"Potential null pointer dereference."*

### 2. Memory Leak Detection
**Description:** Ensure every `malloc` is paired with a `free`.
**Implementation:**
- When `malloc` is called, track the returned pointer variable.
- Follow the pointer's state through the CFG.
- If the CFG reaches the `ExitBlock` and the pointer state hasn't transitioned to `Freed` (via `free(ptr)`) and isn't being returned to the caller, throw a *"Memory leak detected"* warning.

---

## Phase 4: Value Range & Bounds Check Elimination (Optimization)
*Goal: Maximize performance by safely removing runtime overhead.*

### 1. Advanced Value Range Propagation
**Description:** Track the mathematical min/max boundaries of integer variables.
**Implementation:**
- Create `class ValueRangePass`.
- If `x = 5`, state is `[5, 5]`.
- If `if (x < 10)`, inside the `then` block, state is `[MinInt, 9]`.
- Inside `for (int i = 0; i < arr.length; i++)`, `i` is proven to be `[0, arr.length - 1]`.

### 2. Bounds Check Elimination
**Description:** Make arrays as fast as C pointers.
**Implementation:**
- When `Codegen.cpp` generates an `ArrayAccessExprAST`, it currently injects an `EmitRuntimeBoundsCheck`.
- Before emitting, query the `ValueRangePass` for the index variable.
- If the pass mathematically proves that `Range.Min >= 0` and `Range.Max < ArraySize`, completely skip emitting the bounds check.

---

## How to Proceed
I highly recommend keeping this document in your repository and tackling these phases one by one. 
- **Phase 1** is highly accessible and can be done in a few hours.
- **Phase 2** requires reading up on classic compiler Data-Flow Analysis (specifically the worklist algorithm).
- **Phases 3 & 4** are advanced enterprise-grade compiler features that will make Turflang incredibly robust.
