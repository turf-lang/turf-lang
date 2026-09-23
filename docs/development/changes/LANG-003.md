# LANG-003 — `defer`

| | |
|---|---|
| **Status** | landed |
| **Dates** | started 2026-09-24 · landed 2026-09-24 |
| **Commits** | (see `git log --oneline -- 'include/Lexer.h' 'src/Lexer.cpp' 'include/ASTStmt.h' 'include/ASTVisitor.h' 'src/ParserCore.cpp' 'src/ParserStmt.cpp' 'src/ParserInternal.h' 'include/Resolver.h' 'src/ResolverStmt.cpp' 'include/TypeChecker.h' 'src/TypeCheckerStmt.cpp' 'src/Analysis.cpp' 'src/CodegenControlFlow.cpp' 'include/ErrorsFlow.h' 'include/ErrorCodes.h' 'tests/40_defer' 'docs/reference/control_flow.md'` on this branch) |
| **Sessions** | 1 of 1 (roadmap estimated Complexity S–M; landed in one) |
| **Depends on** | none |
| **Required before** | MEM-002 (the collector) |
| **Frozen decisions relied on** | F-MEM-2 (no finalizers) |

## Why

F-MEM-2 says there will be no finalizers — a garbage collector must never
run programmer cleanup code. That's only defensible if there is a
deterministic alternative for OS resources (file descriptors, sockets,
manual `mem::alloc` allocations) that a collector must not and will not
manage. Before this group, no scope hook of any kind existed — nothing ran
on scope exit, so every such cleanup had to be written out by hand at every
return path, silently skippable by a forgotten branch.

## Language semantics

### Old behaviour

No `defer` keyword existed at all; `defer` was an ordinary, unreserved
identifier (confirmed live before starting: `grep -rn '\bdefer\b'
lib/ tests/ showcase/` returned no hits, so reserving it breaks nothing in
this tree).

### New behaviour

```turflang
fn readConfig(path: string): int {
    mut f: File = fs::open(path);
    defer fs::close(f);

    if (!fs::exists(path)) {
        return -1;   // fs::close(f) still runs before this return completes
    }
    return fs::readInt(f);
}
```

`defer <statement>;` registers `<statement>` to run when the **enclosing
`{ }` block** exits — not the enclosing function — by any path: falling off
the end, `return`, `break`, or `continue`. Multiple `defer`s in the same
block run in reverse (LIFO) registration order. A `defer` inside a loop
body's own `{ }` runs once per iteration, since that block's basic blocks
are simply re-executed by the loop's own back edge — no runtime bookkeeping
needed for this, see "Compiler architecture" below. A panic (bounds trap,
division by zero, overflow trap, a failed `test::assert`) never runs a
pending `defer` — TurfLang has no unwinding, and this is the one place a
reader coming from Go should not assume familiar behaviour.

A deferred statement may not itself `return`, `break`, or `continue` —
there is no way for such a jump to target "just the defer," it always
escapes to the enclosing function/loop, which is rejected at compile time
(`DeferControlFlowError`, E083). The one exception: a `break`/`continue`
inside a loop that itself begins **inside** the deferred statement is
absorbed by that inner loop and never escapes the defer at all, so it's
legal (`defer while (cond) { if (done) { break; } };`).

### Is this breaking?

Yes, narrowly, exactly as §8's own "Breaking changes" note anticipated:
`defer` becomes a reserved keyword. Verified live (see above) that no
existing `.tr` file in this tree used it as an identifier, so nothing in
`lib/std`, `tests/`, or `showcase/` needed a rename.

## Compiler architecture

- **Lexer.** One new token, `TOK_DEFER` (`include/Lexer.h`), added to the
  keyword table (`src/Lexer.cpp`).
- **AST.** `DeferExprAST` (`include/ASTStmt.h`) — `Loc` plus one child,
  `Stmt` (the wrapped statement). `ASTVisitor::visitDefer()`
  (`include/ASTVisitor.h`), default no-op like every other visit method.
- **Parser.** `ParseDeferStmt()` (`src/ParserStmt.cpp`, declared
  `src/ParserInternal.h`) is dispatched from `ParsePrimaryBase()`
  (`src/ParserCore.cpp`) at the exact same level as
  `TOK_RETURN`/`TOK_BREAK`/`TOK_CONTINUE` — deliberately NOT its own
  statement-loop special case in `ParseBlock()` the way
  `TOK_MUTABLE`/`TOK_EXPORT`/`TOK_EXTERN` are, since it doesn't need to
  consume its own trailing `;`. This is what makes a stray top-level
  `defer ...;` get rejected by `main.cpp`'s existing "statements are not
  allowed outside of a function" check for free — the same protection
  return/break/continue already had, with zero new plumbing. One
  consequence, not a bug: because `defer`'s wrapped statement is parsed via
  the ordinary `ParseExpression()` (reachable through `if`/`while`/`for`
  the same way `return`/`break`/`continue` are), `defer while (cond) {
  ... };` is syntactically legal — see `06_defer_wraps_loop_break_absorbed.tr`.
- **Resolver.** `Resolver::visitDefer()` (`src/ResolverStmt.cpp`) resolves
  the wrapped statement normally, then runs
  `FindEscapingControlFlow()` — a structural, non-interprocedural scan in
  the exact recursion shape as `MutatesSelfField()`/
  `CollectReturnedErrorTypeNames()` (`src/ResolverCore.cpp`): descends
  through `Block`/`If`/`While`/`For`/`ForEach` (matching that precedent's
  own exclusion of `DoWhile`/`Compare`), tracking an `InLoop` flag so a
  `break`/`continue` absorbed by a loop that begins inside the defer is
  never flagged, while a `return` always is, regardless of nesting.
- **TypeChecker.** `TypeChecker::visitDefer()` (`src/TypeCheckerStmt.cpp`)
  type-checks the wrapped statement exactly as if it appeared directly in
  the enclosing block (a `defer` is a scheduling detail, not a new typing
  context) and sets the node's own type to `void`.
- **CFG / analysis.** **No change to `src/CFG.cpp` itself** — despite §8
  naming it as a relevant file, `CFGBuilder::visitExpr()`'s existing
  fallback ("regular statement — add to current block") already handles
  `DeferExprAST` correctly with zero changes, because a `defer <stmt>;`
  never itself branches from the CFG's point of view: its runtime effect is
  purely a question of WHEN its wrapped statement's own `codegen()` runs, a
  pure Codegen-lowering problem, not a control-flow-shape one. What DID
  need updating: four `ASTVisitor` subclasses in `src/Analysis.cpp` that
  recursively scan a statement for variable reads/defs
  (`AnalysisVisitor`, used by `DefiniteAssignmentPass`/
  `LivenessAnalysisPass`; `BodyDeclCollector`, `DefiniteAssignmentPass`'s
  "AllBodyDeclared" set; `SelfFieldDefVisitor`,
  `ConstructorFieldAssignmentPass`; `NullableRefCollector`,
  `NullabilityPass`'s tracked-symbol discovery) each gained a one-line
  `visitDefer(DeferExprAST &Node) { visitChild(Node.getStmt()); }` —
  without it, `ASTVisitor`'s default no-op `visitDefer` would make a
  variable read/defined/assigned ONLY inside a `defer` invisible to every
  one of these passes (a false `DefiniteAssignmentError` miss, a false
  `UnusedVariableWarning`/`DeadStoreWarning`, a missed
  `IncompleteConstructorError` field, or a `&?T` never getting tracked by
  `NullabilityPass`).
- **Codegen.** All new state lives in `src/CodegenControlFlow.cpp`,
  alongside the pre-existing `LoopBlocks` stack it mirrors:
  - `PendingDeferFrames` — one frame (`std::vector<ExprAST*>`) per
    currently-open `{ }` scope, pushed/popped by `BlockExprAST::codegen()`
    itself.
  - `LoopDeferDepth` — parallel to `LoopBlocks`: `PendingDeferFrames.size()`
    at the moment each loop's own body starts, so `break`/`continue` know
    how many open frames belong to blocks INSIDE the loop (must run)
    versus enclosing it (must not).
  - `FlushPendingDefers(FromDepth)` — runs every frame from `FromDepth` to
    the top of the stack, innermost first, each frame's own statements in
    reverse.
  - `DeferExprAST::codegen()` — does NOT codegen its wrapped statement.
    It only pushes the raw `ExprAST*` onto the innermost open frame.
  - `BlockExprAST::codegen()` — pushes its own frame at entry; at its
    natural (non-early-exit) end, flushes just that one frame (skipped
    when the current insert block already has a terminator — every path
    already exited early and already ran these same defers itself); pops
    its frame unconditionally.
  - `ReturnExprAST::codegen()` — flushes ALL open frames (`FromDepth = 0`)
    after the return value is computed (defer semantics: the return value
    is fixed before cleanup runs) but before the actual `ret`.
  - `BreakExprAST`/`ContinueExprAST::codegen()` — flush down to
    `LoopDeferDepth.back()` only, before their own branch.
  - The four loop-codegen methods (`WhileExprAST`, `DoWhileExprAST`,
    `ForExprAST`, `ForEachExprAST`) each gained a
    `LoopDeferDepth.push_back(PendingDeferFrames.size())` alongside every
    existing `LoopBlocks.push_back(...)`, and a matching pop at every
    existing `LoopBlocks.pop_back()` (including the early-return-on-null-
    body-codegen paths).
  - **No change needed in `RecoverExprAST::codegen()`** — its `Run`/`AsRun`
    block is an ordinary `BlockExprAST`, so `defer` support (and a nested
    `return`'s flush walking all the way out past the recover) falls out
    of the exact same machinery every other block gets, confirmed live by
    `05_recover_interaction.tr`.
- **Diagnostics.** `DeferControlFlowError` (`include/ErrorsFlow.h`, E083,
  `CONTROL_FLOW` category — no new `SLM::ErrorCategory`, per §25).

## Runtime changes

None. `defer`'s entire lowering is compile-time statement duplication
across exit edges; no new `libturfrt` entry point, no new runtime
representation.

## Standard-library changes

None. No existing `lib/std` file was touched.

## New invariants

Added to `docs/development/INVARIANTS.md` under a new "Cleanup and
`defer`" section:

- **INV-DEFER-1**: a panic never runs a pending `defer` — enforced
  structurally (every panic path lowers via `abort()`/a runtime trap call,
  never through `Return`/`Break`/`ContinueExprAST::codegen()`, the only
  three places `FlushPendingDefers()` is ever called).
- **INV-DEFER-2**: a deferred statement may never itself `return`, `break`,
  or `continue` past the `defer` — enforced by `FindEscapingControlFlow()`
  (`Resolver::visitDefer()`).

## Removed assumptions

- **"Every `ASTVisitor` subclass that recursively scans a statement for
  variable reads/writes already sees everything reachable from a block."**
  No longer safe to assume unconditionally: a node type that wraps a
  statement WITHOUT itself being one of the control-flow shapes
  `CFGBuilder` specially recognizes (a "regular statement" leaf, in
  `CFGBuilder::visitExpr()`'s own terms) is opaque to any visitor that
  doesn't explicitly override `visitDefer()` to recurse into it. `defer` is
  the first such wrapper node in this codebase; a future one would need the
  same audit across the same four `Analysis.cpp` visitor classes (and
  `TypeChecker`/`Resolver`'s own dispatch) this group performed.
- **"`Builder->GetInsertBlock()` is always non-null inside
  `BlockExprAST::codegen()`."** Was never actually a documented assumption,
  but this group's own first working revision assumed it implicitly (see
  Known pitfalls) — a MODULE's own top-level body is a `BlockExprAST` too
  (`ModuleImportExprAST::codegen()` calls `Body->codegen()` directly, with
  no enclosing function and therefore no current insert point). Any future
  change to `BlockExprAST::codegen()` that reads `Builder->GetInsertBlock()`
  must null-check it.
- **"A statement reachable via `ParsePrimaryBase()`'s switch only ever
  appears where it visually looks like it should."** `return`/`break`/
  `continue` already stretched this (technically embeddable inside an
  arbitrary expression position, tolerated rather than specially guarded
  against); `defer` inherits the identical looseness deliberately, rather
  than adding new grammar restriction machinery beyond what its siblings
  already have.

## Tests

- **Added** `tests/40_defer/01_ordering.tr` — LIFO order for three
  `defer`s in one block, observed through a struct mutated by reference.
- **Added** `tests/40_defer/02_exit_paths.tr` — one function per exit
  edge: plain fallthrough, `return`, `break` (on the very first
  iteration), `continue` (every iteration).
- **Added** `tests/40_defer/03_nesting.tr` — a `return` three blocks deep
  runs all three levels' defers, innermost first.
- **Added** `tests/40_defer/04_loop_per_iteration.tr` — three loop
  iterations with no early exit, all through the natural-end flush path.
- **Added** `tests/40_defer/05_recover_interaction.tr` — a `defer`
  registered before a `!T` call still runs whether `recover run { return
  ...; }` fires (the failure path) or not (the success/fallthrough path).
- **Added** `tests/40_defer/06_defer_wraps_loop_break_absorbed.tr` — a
  `break` inside a `while` that is ITSELF the deferred statement is legal
  (contrast the fail_ test below), directly exercising
  `FindEscapingControlFlow()`'s `InLoop` tracking.
- **Added** `tests/40_defer/fail_return_in_defer.tr` — the group's
  required negative test (§8's own "Testing scope"): a bare `return`
  directly inside a `defer` is rejected (E083).
- **Added** `tests/40_defer/fail_break_in_defer.tr` — same rule, for a bare
  `break` not absorbed by any loop.
- **Added** `tests/40_defer/fail_defer_skipped_on_panic.tr`
  (`RUNTIME_ABORT_EXPECTED`) — compiles, then aborts via a deliberately
  failing `test::assert()`. Proves the "compiles, then aborts" half of
  INV-DEFER-1 automatically; the "never prints `DEFERRED_RAN`" half was
  verified live during this session (see Known pitfalls) and is not
  mechanically re-checked by `test_runner.sh` on every run — there is
  currently no test-runner support for asserting on a program's stdout
  content.
- Full suite: 727 → 736 tests, all green (`./scripts/test_runner.sh`).
- `lib/std/**` and `showcase/**` compiled directly: all resolve (the two
  pre-existing `E034` self-import hits in `list.tr`/`mem.tr` are the
  documented, unrelated prelude-self-import bug class, unchanged).
- All six prelude modules compile directly with no ambiguity regression.
- `scripts/check_doc_links.sh` passes.

## Known pitfalls

- **The first working revision of `BlockExprAST::codegen()` crashed the
  compiler on the simplest possible program** (`fn main(): int { return
  0; }`) with a SIGSEGV inside LLVM's `BasicBlock::getTerminator()`. Root
  cause, found via `gdb`: the natural-end flush check
  (`Builder->GetInsertBlock()->getTerminator()`) dereferenced a null
  `BasicBlock*` — `ModuleImportExprAST::codegen()` (`CodegenStruct.cpp`)
  calls `Body->codegen()` directly for a module's own top-level
  declaration list, with no enclosing function and therefore no current
  insert point. Fixed by null-checking `Builder->GetInsertBlock()` before
  dereferencing it. This is exactly the kind of thing "verify claims
  live" catches and a code-review-only pass would not: the fix required
  actually compiling a program and reading the crash backtrace, not just
  reasoning about the AST shapes involved.
- **`defer`'s payload can syntactically be a compound control-flow
  construct** (`defer while (cond) { ... };`), not just a simple call/
  assignment, because it's parsed via the same `ParsePrimaryBase()` path
  `if`/`while`/`for` themselves use. This is a real, intentional
  consequence of reusing the simplest correct parsing strategy (matching
  `return`/`break`/`continue`'s own precedent) rather than a deliberately
  designed feature — `06_defer_wraps_loop_break_absorbed.tr` locks in the
  one interaction this creates (loop-absorbed break/continue) that
  actually needed thought (`FindEscapingControlFlow()`'s `InLoop`
  parameter). A future reader extending `defer` should not assume its
  payload is always a single simple statement.
- **A deferred statement's own `codegen()` can run more than once at
  compile time for a single textual `defer`** — once per exit edge that
  reaches it, plus, in the case of an early exit, one additional harmless
  duplicate at the block's own "natural end" position (which is provably
  dead code by then — nothing branches into it — but `BlockExprAST::codegen
  ()`'s own terminator check can't statically distinguish "genuinely
  live, no terminator yet" from "dead code after an early exit, also no
  terminator yet"). This is intentional, not a bug to "optimize away": it
  keeps the lowering a pure, local, single-pass AST walk with no separate
  reachability analysis duplicating `AllPathsDiverge()`/CFG logic. A
  future reader should not be alarmed to see the same deferred statement's
  LLVM IR appear more than twice in a `--emit-llvm` dump of a function
  with several early exits.
- **`recover`'s own `Run`/`AsRun` block needed zero code changes** to
  interact correctly with `defer` — worth calling out explicitly since it
  would have been easy to assume otherwise (recover's block is built with
  hand-rolled LLVM basic blocks elsewhere in `RecoverExprAST::codegen()`).
  Because that block is an ordinary `BlockExprAST` under the hood, it
  automatically gets its own defer frame and its `return`'s flush walks
  out through every enclosing frame the normal way.

## Handbook impact

| File / chapter | What is now wrong | Suggested replacement |
|---|---|---|
| `docs/reference/control_flow.md` | Never mentioned `defer` (it didn't exist) | Updated in this session — new `## defer` section between `break`/`continue` and `compare`/`matches`/`otherwise`, plus the chapter's own one-line construct list |
| `docs/reference/README.md` | Chapter 3's one-line summary omitted `defer` | Updated in this session |

### Examples that became invalid

None — `defer` is a purely additive keyword.

### Examples that should be added

Done in this session: the `defer`/`fs::close` cleanup example, the
per-iteration loop example, the LIFO-ordering example, and the
absorbed-break-inside-a-deferred-loop example, all in
`docs/reference/control_flow.md`'s new section.

### Sections that must NOT change

Every other section of `docs/reference/control_flow.md` (`if`/`elseif`/
`else`, `give`/`otherwise`, `while`, `do`/`while`, both `for` forms, plain
`break`/`continue`, `compare`/`matches`/`otherwise`) — this group is
additive; none of their semantics changed.

## Unresolved questions

None raised during this session — no frozen decision was contested.

## Related records

None yet (first session to touch `defer`). `MEM-002` (the collector) lists
this group as a prerequisite — F-MEM-2's "no finalizers" decision depends
on `defer` existing as the deterministic-cleanup alternative.
