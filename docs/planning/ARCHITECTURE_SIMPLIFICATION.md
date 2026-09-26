# Architecture Simplification — killing ambient state, giving the frontend a real product

**Status: discussion, not a decision.** Nothing here is frozen. This document
exists so the conversation that produced it isn't lost, and so a future
session (human or AI) has a starting point instead of re-deriving it. Turning
any part of this into a tracked feature group with its own `FREEZE.md`
entries requires an explicit decision and a change record, same as anything
else in `ARCHITECTURE_OVERHAUL.md` §6.0.

Not scheduled anywhere yet. `ARCHITECTURE_OVERHAUL.md` §27 (Deferred Work
Register) does not mention this — the closest existing item is D11
("Developer tooling: LSP... worth doing in parallel with the overhaul"),
which is about *building* tooling, not about *why the compiler's own shape
makes tooling hard*. This document is the missing diagnosis for that gap.

---

## 1. The problem, with evidence

The compiler's pipeline is described as Lexer → Parser → AST → Resolver →
TypeChecker → Codegen → LLVM, but every stage reaches past its own boundary
into shared, ambient, mutable global state instead of taking explicit input
and returning explicit output. This isn't a style complaint — it's the
concrete reason `turf-lsp` is fragile and now broken.

**Evidence, gathered live during a 2026-09-20 session:**

- The Lexer alone exposes 18 `extern` globals (`include/Lexer.h`): `CurLine`,
  `CurCol`, `IdentifierStr`, `SourceLines`, `g_recoverJmp`, `g_braceDepth`,
  `g_AIEnabled`, `SourceFilename`, and more.
- Codegen adds `TheContext`, `Builder`, `TheModule`, `NamedValues`,
  `CurrentFunction`, `CurrentFuncReturnType` as more `extern` globals
  (`include/Codegen.h`).
- The Parser's precedence table (`static std::map<int,int> BinopPrecedence`,
  `src/ParserCore.cpp`) is a file-local static — not even `extern`, so
  nothing outside that translation unit can see or reset it except by
  re-calling `InitializePrecedence()`.
- `GlobalSymbolTable` (`include/SymbolTable.h`) is a single process-wide
  `std::unique_ptr<SymbolTable>`.
- **Proof this already causes real pain, not hypothetical pain:**
  `turf-lsp/src/CompileBuffer.cpp`'s first ~40 lines exist *only* to manually
  save and restore roughly ten of these globals (`GlobalSymbolTable`,
  `SourceFilename`, `SourceLines`, `g_AIEnabled`, the diagnostics vector, the
  `setjmp` recovery buffer, the parsed-top-level-ASTs list, import-cycle
  state) around every single analysis call, specifically so a second
  in-process analysis doesn't corrupt the first. That function is a
  hand-written workaround for the pipeline not being reentrant.
- **Proof it's actively breaking things, right now:** `turf-lsp`'s last
  commit is 2026-09-16; `turflang` has had 35 commits since, including this
  session's SEM-004. `turf-lsp/src/SemanticQuery.cpp:58` contains
  `void visitDefaultInit(DefaultInitExprAST &Node) override {}` — both the
  class and the virtual method it overrides were deleted in this session's
  SEM-004 commit. `turf-lsp` will not compile against current `turflang`
  HEAD. Separately, it has zero references to `GuessType`/`TOK_GUESS`
  anywhere — it has no idea the `guess` keyword (SEM-003, landed the same
  day) exists.
- `turf-lsp`'s own `plan/02-compiler-core-api-changes.md` already diagnosed
  half of this: item A proposes a `turf::query::analyze()` facade specifically
  because *"any refactor of the compiler's entry path silently breaks the
  LSP"* — written in `turf-lsp`'s own words, currently marked "not started."
  Item F (a real per-analysis context object, replacing the globals
  entirely) is explicitly deferred with the reasoning *"the single-worker
  model already meets a single-user editor's needs."* That reasoning holds
  for a hobby-scale LSP and stops holding the moment `turf-fmt`, a
  package-manager doing semantic checks, or concurrent LSP requests exist.

**Evidence, gathered live during a 2026-09-26 session (STD-003,
`docs/development/changes/STD-003.md`) — a live, demonstrated consequence
of the "split-brain" issue §6 already names, not a new problem:**

- **`GlobalSymbolTable`'s registered `TurfStructType` for a generic struct
  is a permanently-stale snapshot, and it silently breaks real code, not
  just tooling.** `src/TypeCheckerDecl.cpp`'s own `isOrWrapsGenericParam()`
  doc comment already diagnoses the mechanism in full: `Resolver::
  visitStructDecl()` rewrites a generic struct's own AST node's field
  types from `NamedType("T")` to `TurfGenericParamType("T")` in place, but
  `GlobalSymbolTable`'s registered `TurfStructType` for that same struct is
  a *separate object*, built from a parse-time snapshot, and never
  receives that rewrite — so every consumer that resolves a generic
  struct's field type by looking it up in `GlobalSymbolTable` (rather than
  reading the Resolver-mutated AST node directly) sees the stale
  `NamedType("T")` forever, indistinguishable by name from an ordinary,
  unrelated struct field.
- **Concrete failure this caused, found while trying to convert
  `mem::Pool<T>`'s `free_pool` into a real generic method
  (`fn<T> Pool.free_all(): void`):** `self.items[i]`, a field access
  reached via `self` inside a generic method body, resolves through this
  exact stale `GlobalSymbolTable` copy — so its computed type is
  `NamedType("T")`, not `TurfGenericParamType("T")`. Passing it to
  `free(p: void*)` then fails candidate matching (`No overload of 'free'
  matches argument types (T)`), because `typesCompatible()`'s Rule 1 ("an
  unresolved generic type parameter is compatible with anything") only
  ever checks for `TurfGenericParamType`, never for the `NamedType`
  representation a real generic struct's own fields actually carry outside
  the Resolver-mutated AST. The identical gap independently blocked an
  explicit cast (`self.items[i] to void*`) for the same underlying reason,
  fixed narrowly for `TypeCheckerExpr.cpp::visitCast` (STD-003's own "Fix
  3") but — deliberately, on a scope judgment call at the time — not
  chased into `typesCompatible()` itself, since the true fix looked like
  it would mean touching field-type resolution more broadly.
- **That judgment call may have been more pessimistic than necessary, on
  a second look while writing this up.** `isOrWrapsGenericParam()`
  (`TypeCheckerDecl.cpp`, external linkage via `TypeCheckerInternal.h`,
  already consumed by `TypeCheckerCall.cpp`'s own `visitFuncCall`/
  `visitMethodCall`) *already* correctly recognizes both representations —
  it treats a `NamedType` whose name is neither a known primitive nor
  anything registered in `GlobalSymbolTable` as "is-or-wraps a generic
  parameter," the exact test `typesCompatible()`'s Rule 1 (and Fix 3's own
  new bypass) would need. Reusing that existing helper in both places,
  rather than each independently re-deriving its own `dynamic_cast
  <TurfGenericParamType*>`-only check, is a small, targeted, already-
  scoped candidate fix — not investigated live, not implemented, flagged
  here specifically so the next session doesn't have to re-derive this
  starting point. See §7's new first item, below.
- **Three separate, independent compiler bugs were found and fixed in the
  same session** (STD-003's Fix 1/Fix 2/Fix 3 — `TypeCheckerCall.cpp`'s
  self-call `Subst` recovery, five call sites missing a `FuncType`
  substitution case, and `visitCast`'s missing generic-parameter cast-
  legality bypass), each independently reachable by ordinary Turf code
  with no relation to this document's own ambient-globals framing. Taken
  together with the `Pool<T>` finding above, the pattern worth naming:
  this compiler's notion of "what a generic type parameter looks like, and
  where it might be hiding" is implemented independently, by hand, in at
  least seven different recursive functions across four files
  (`Resolver::substituteTypeParams`, `typesCompatible()`,
  `collectGenericSubstitution()`/`collectGenericParamNameOrder()`,
  `Monomorphize::substituteConcreteTypes()`, `visitCast`'s legality
  whitelist, and `isOrWrapsGenericParam()`/`isFullyConcreteType()`) — none
  of them delegating to a shared, single source of truth for "does this
  type contain an open generic parameter, and if so substitute it." Three
  of those seven were independently, silently incomplete, discovered only
  by hitting them in practice, across TYPE-002's 8 sessions plus
  everything built since. This is the same root shape as §4.2's Bound/Typed
  IR argument (a decision re-derived by hand, independently, every place
  it's needed, instead of made once and trusted) — evidence for the
  general thesis, not just for phase 3's own scope.

**Evidence, gathered live during a 2026-09-26 session (SLM-001,
`docs/development/changes/SLM-001.md`) — a second, independent tool
(`Sandbox`, not `turf-lsp`) hitting the identical ambient-state shape §1
already names, this time from INSIDE the compiler's own codebase, not an
external consumer of it:**

- **`Sandbox::validate()` (`src/Sandbox.cpp`) is a hand-written save/restore
  dance around `SourceLines`/`DiagnosticEngine`'s global diagnostics list —
  structurally the same workaround `CompileBuffer.cpp`'s first ~40 lines
  already do for `turf-lsp`**, just inside the main compiler binary instead
  of an external tool: `auto savedSourceLines = SourceLines; ... SourceLines
  = Snapshot.SourceCode; ... runFullSemanticPass(...); ... SourceLines =
  savedSourceLines;` (`Sandbox.cpp:427-445`). This exists because there is no
  "compile this text, get a value back" function anywhere in the compiler —
  the only way to run the Lexer/Parser/Resolver/TypeChecker pipeline at all
  is to point the ambient globals at your text, run the pipeline for its
  side effects, and put the globals back before anyone notices. `--smart`'s
  own sandbox verification (§16 of `ARCHITECTURE_OVERHAUL.md`, load-bearing —
  it's the only thing between a model hallucination and a user's source)
  is built entirely on top of this workaround, meaning the ambient-state
  problem isn't just costing `turf-lsp` external maintenance burden — it's
  already inside the one piece of the compiler that most needs to run
  the pipeline repeatedly, in isolation, without cross-contaminating global
  state.
- **Concrete cost this imposed on a real feature, not a hypothetical one:**
  extending SLM-001 to measure "did an accepted `--smart` fix actually
  preserve the program's meaning, not just clear the compile error"
  (`ARCHITECTURE_OVERHAUL.md` §16.4's semantic-correctness rate) needed a NEW
  CLI flag (`--smart-write-fix=<path>`) that writes the Sandbox's validated
  in-memory text out to a real file, plus a benchmark tool
  (`scripts/slm_benchmark/run_benchmark.py`) that then shells out to `turf`
  **two more times** as a subprocess — once to actually compile the written
  file through real Codegen and linking (`Sandbox::validate()` never reaches
  Codegen at all; it only re-runs Lex/Parse/Resolve/TypeCheck/Lint), once
  more to execute the resulting binary and read its exit code as the oracle.
  With a real `Compilation`/`SemanticModel`+lowering API (§5's target shape),
  this would have been: take the already-validated `BoundProgram`, lower and
  JIT it in-process, run it, read the result — no subprocess, no temp files,
  no second full re-parse of a file already fully understood one call
  earlier. The workaround was successfully built (three subprocess hops
  instead of zero), but it's a direct, measured illustration of exactly the
  tax §1 already predicted this architecture would keep charging.

**One clarification, so this isn't overstated:** the AST classes having no
single `AST.cpp` is *not* part of this problem. AST nodes are deliberately
pure data + a virtual-dispatch declaration, with each pass (`Codegen`,
`TypeChecker`, `Resolver`) supplying method bodies in its own file — that's
the visitor pattern working as intended (Roslyn's syntax nodes and LLVM's IR
classes are organized the same way). The actual coupling is entirely
Lexer/Parser/Resolver/Codegen reaching for ambient globals instead of taking
state as an explicit parameter.

---

## 2. What "purity" actually means here

Not "make every function pure" in the Haskell sense — that's not how any
production compiler is built, including genuinely functional ones (GHC uses
mutable refs internally for interning and caching). The actual target,
matching Clang, Roslyn, and rustc:

> **No ambient state. No hidden dependencies. Explicit inputs. Explicit
> outputs. Stable representations. Stable APIs between representations.**

Concretely, each stage becomes a function with a real signature instead of
a side-effecting call into a shared soup of globals:

```cpp
TokenStream    Lexer::lex(SourceText source);
SyntaxTree     Parser::parse(TokenStream tokens);
SemanticModel  Resolver::resolve(const SyntaxTree& tree, const SemanticContext& ctx);
BoundProgram   TypeChecker::check(const SyntaxTree& tree, const SemanticModel& semantics);
TurfIR         Lowerer::lower(const BoundProgram& program);
LLVMModule     Codegen::generate(const TurfIR& ir, CodegenContext& ctx);
```

An LSP request against this shape needs a `Compilation` object it owns, not
`GlobalSymbolTable`/`GlobalLexerState`/`GlobalParserState`/`GlobalCodegenState`:

```cpp
Compilation compilation = compiler.createCompilation(...);
auto syntax    = compilation.syntaxTree(file);
auto semantics = compilation.semanticModel(file);
auto symbol    = semantics.symbolAt(position);
```

---

## 3. Don't confuse "explicit boundaries" with "hiding"

A real risk while fixing this: swinging to the opposite failure mode, where
each stage's output is walled off and nothing else can see it. That would
make the compiler *less* composable, not more — the same failure mode as
globals, just via encapsulation instead of ambient state.

The correct mental model, from how Roslyn actually behaves: `SemanticModel`
doesn't hide the `SyntaxTree` it's built from — a formatter reads the syntax
tree directly and never touches semantics; a "find all references" feature
reads the semantic model; both are looking at the same public data, chosen
by what each consumer actually needs. **Every stage's product should be a
fully public, richly-typed, freely-readable value** — the fix is explicit
and *complete* visibility, replacing ambient and *partial* visibility, not
narrower visibility.

This has a direct payoff for the wider tool ecosystem Turf is growing
(`turf-lsp`, a future `turf-fmt`, `turf-analyzer`, `turf-test`, a package
manager doing semantic checks): none of them need to import "half the
compiler." A formatter only needs `Lexer` + `Parser` + `SyntaxTree`. The LSP
needs `SyntaxTree` + `SemanticModel` + `Symbols`. Only the actual compiler
driver needs everything, including `Codegen`. Nothing should require the
formatter to know what a symbol table singleton is.

---

## 4. What Turf is missing that mainstream compilers have

### 4.1 A semantic API, as a first-class product

A **syntactic** API only knows text shape (tokens, a parse tree, "this is a
function call"). A **semantic** API answers questions that require actual
understanding: *what type does this expression have, what declaration does
this identifier resolve to, are these two references the same symbol, what's
in scope here.* Roslyn's `SemanticModel` (`GetTypeInfo`, `GetSymbolInfo`,
`LookupSymbols`), TypeScript's `LanguageService`, and clangd's indexing layer
are the same idea under different names.

`turf-lsp/src/SemanticQuery.cpp` is currently *trying to be this from
outside the compiler*, reconstructing what a real semantic API would hand it
directly by reaching into raw AST nodes and globals. The fix isn't a new
*pipeline stage* so much as the missing *deliverable*: right now "semantic
information" is scattered across `ResolvedSymbol` fields mutated onto AST
nodes, `TypeChecker::NodeTypes_`'s side table, and `GlobalSymbolTable` —
three places, none of them a single object a consumer can be handed.

### 4.2 A Bound/Typed IR, distinct from the SemanticModel

Don't merge these two — they solve different problems (Roslyn keeps them
conceptually separate even though both are built from the same binding
data):

| | Answers | Shape |
|---|---|---|
| **SemanticModel** | "What is this symbol? What type is this expression? What's visible here?" | A query interface. |
| **Bound/Typed IR** | The program itself, after semantic decisions are already made. | A compiler representation (`BoundCall`, `BoundBinary`, `BoundConversion`, ...). |

The value of a Bound IR: `x + y` as an AST node is pure syntax
(`BinaryExpr{lhs: Identifier("x"), op: +, rhs: Identifier("y")}`) — the
compiler still has to figure out what `+` *means* for these operand types
every time something looks at it. A `BoundBinaryExpr` has already had that
question answered once (`lhs: BoundLocal(x: int)`, `operator: IntAdd`, `rhs:
BoundLocal(y: int)`, `type: int`) — Codegen no longer re-derives "what does
this operator mean here," it just translates a known operation:

```cpp
// bad — Codegen re-discovering language semantics:
codegen(BinaryExpr* expr) {
  auto lhsType = typeOf(resolve(expr->lhs));
  auto rhsType = typeOf(resolve(expr->rhs));
  // figure out what + means, figure out conversions, figure out overload...
}

// good — Codegen trusting a decision that's already been made:
codegen(BoundBinaryExpr* expr) {
  return emitBinaryOperation(expr->operation, expr->lhs, expr->rhs);
}
```

This is the frontend/backend boundary worth naming explicitly: the
**frontend's** job is to understand what the program means; the
**backend's** job is to turn that meaning into executable representation.
LLVM codegen should never be discovering language semantics — by the time
Codegen runs, operation identity, types, symbol bindings, and conversions
should all already be known facts it trusts, not things it re-derives by
walking the original AST a second time.

### 4.3 Where the existing CFG work fits

Turf already builds something adjacent to a lowered IR and mostly throws it
away: `CFG.cpp` builds a real control-flow graph per function
(`docs/architecture/compiler_pipeline.md` §6) for the static-analysis
battery (`DefiniteAssignmentPass`, `LivenessAnalysisPass`, `ValueRangePass`,
etc.), then discards it — `Codegen*.cpp` walks the *original AST* again to
actually emit LLVM IR, re-deriving structural knowledge the CFG already
captured. Promoting that CFG from "analysis-only side artifact" to "the
representation Codegen actually lowers from" is directly precedented (Rust's
MIR unifies borrow-check analysis and codegen lowering into one shared IR)
and is a bounded, well-scoped piece of this larger idea rather than a new
invention.

---

## 5. Target shape

```
Text
 │
 ▼
Tokens                 ← syntax API   (Lexer)
 │
 ▼
SyntaxTree             ← syntax API   (Parser)
 │
 ▼
SemanticModel          ← semantic API (Resolver + TypeChecker's query side)
 │
 ▼
Bound/Turf IR          ← compiler API (TypeChecker's representation side)
 │
 ▼
Lowered IR             ← backend API  (CFG/lowering, promoted from analysis-only)
 │
 ▼
LLVM IR
 │
 ▼
Machine code
```

Dependency layering this enables:

```
                 turf-compiler-core
                         │
             ┌───────────┼────────────┐
             │           │            │
          Syntax      Semantic      Lowering
             │          Model          │
             ▼            ▼            ▼
          turf-fmt     turf-lsp     turf (driver, needs everything)
```

---

## 6. What NOT to do

- **Don't copy Roslyn's literal component set.** Turf doesn't need
  `Workspaces`, a separate `Compilation` layer distinct from everything else,
  or Roslyn's exact `LocalRewriter`/Cci emit machinery just because Roslyn
  has them. The scoped-down version in §5 is already a substantial
  improvement; the architectural *principle* (explicit representations,
  stable APIs between them) is what matters, not the component count.
- **Don't aim for "make every function pure."** See §2 — the target is
  explicit state ownership, not the elimination of mutation everywhere.
- **Don't treat LLVM's own Kaleidoscope-tutorial globals
  (`TheContext`/`Builder`/`TheModule`/`NamedValues`) as a legitimate
  precedent.** LLVM's own tutorial documentation is explicit that it uses
  globals and skips software-engineering best practice *because it is
  teaching compiler techniques*, not modeling production architecture. Turf's
  current `Codegen.h` globals are structurally the same pattern — that's a
  shared anti-pattern with a teaching example, not a design turf-lang should
  keep defending.
- **Don't do this as a bolt-on to the current SEM-*/TYPE-*/OPT-* overhaul.**
  It's comparable in size to that overhaul, possibly larger — it touches
  every stage and forces a real decision about the generic-substitution
  split-brain issue already on record (`TypeCheckerDecl.cpp`'s own comment:
  a generic struct's Resolver-mutated AST field types and its
  globally-registered `TurfStructType`'s field types can diverge — see
  `isOrWrapsGenericParam()`'s doc comment). It deserves its own tracked
  initiative, sequenced deliberately, the same way `ARCHITECTURE_OVERHAUL.md`
  sequenced TYPE-001 before TYPE-002 before SEM-004.
- **Don't do a wholesale comment-deletion pass as part of this, or as its
  own detached initiative.** Separate concern (see §8) — fold it into
  whichever phase below touches each file, don't strip first and rebuild
  later.

---

## 7. A phased, incremental path (not a rewrite)

Sequenced so each phase's output is what the next phase needs, and so the
compiler stays shippable (`test_runner.sh` green) after every phase, mirroring
how `ARCHITECTURE_OVERHAUL.md`'s own groups are sequenced by dependency.

**The first thing to do, ahead of phase 0 below, is smaller and more
concrete than any of them** — §1's 2026-09-26 evidence already diagnosed
it down to a candidate fix, unlike phases 0-5, which are still design
sketches:

| Phase | Change | Unlocks | Risk |
|---|---|---|---|
| **0★. Stop `typesCompatible()`/`visitCast()` missing the stale-`NamedType` generic-parameter case** | `typesCompatible()`'s Rule 1 and `visitCast`'s STD-003 "Fix 3" bypass both currently test only `dynamic_cast<TurfGenericParamType*>`. `isOrWrapsGenericParam()` (`TypeCheckerDecl.cpp`) already correctly recognizes the OTHER representation a generic struct's own field carries outside the Resolver-mutated AST (a `NamedType` whose name is neither a known primitive nor anything `GlobalSymbolTable` has registered) — reuse it in both places instead of each re-deriving its own narrower check. | Unblocks `self.<field>`-typed values (not just plain parameters) flowing through candidate matching and casts inside a generic method body — directly fixes the `mem::Pool<T>.free_all()` case §1 documents, and likely others never hit yet for the same reason those three were only found this session. | Low — two call sites, reusing an existing, already-tested helper; not a new mechanism. **Not yet verified live — the next session must confirm with a real repro before trusting this description**, per this document's and `AGENTS.md`'s own "verify claims live" rule. |
| **0. Lexer → `LexerState`** | Wrap `CurLine`/`CurCol`/`IdentifierStr`/etc. in an owned struct; `gettok()` takes it explicitly instead of touching 18 externs. | Concurrent tokenization of multiple open buffers; a real formatter's foundation. | Low — smallest, most self-contained stage. |
| **1. Parser → `ParserState`** | Owns `CurTok`/`g_braceDepth`/recursion depth; parse functions take it explicitly. | Safe concurrent parsing of multiple editor buffers; syntax-only tooling (highlighting, a formatter) without touching semantics. | Low-medium. |
| **2. Drop `GlobalSymbolTable` as a singleton** | An explicit, constructed `SymbolTable`/session object, passed in — this is `turf-lsp`'s own already-named, already-deferred item F. | Multiple concurrent analyses; the actual prerequisite for phase 3. | Medium. |
| **3. A real `SemanticModel`, separate from AST mutation** | Resolver/TypeChecker stop writing `ResolvedSymbol`/`setTypePtr` onto the shared AST; they produce a queryable object instead. Forces resolving the generic-substitution split-brain issue named in §6, as a side effect (worth doing regardless of this initiative). | The actual semantic API `turf-lsp` needs — `getSymbol`/`getType`/`lookup`/`getReferences` become real, stable calls instead of reimplemented heuristics. | High — the biggest conceptual change; touches Resolver and TypeChecker's core contract. |
| **4. Promote CFG into a real lowering/Turf-IR stage** | Codegen emits from the lowered IR instead of re-walking the AST; the CFG the static-analysis passes already build becomes load-bearing, not throwaway. | Removes duplicated structural knowledge between analysis and Codegen; a real place for future MIR-style optimizations. | High — Codegen's own contract changes. |
| **5. Dependency layering** | Split the frontend (through phase 3/4's outputs) into something linkable independent of Codegen, so `turf-fmt`/future tools depend only on `Syntax`+`SemanticModel`, never on Codegen or LLVM. | The actual "collection of independently usable components" goal — the reason this document exists. | Medium — mostly build-system work once 0-4 are done. |

Phases 0-2 are load-bearing for *everything* downstream and are independently
valuable even if phases 3-5 never happen — they're what makes `CompileBuffer.cpp`'s
save/restore dance unnecessary regardless of how far the SemanticModel work goes.

---

## 8. Comments, as a separate concern

Not part of this architectural work, but raised in the same conversation and
worth recording so it isn't lost: the codebase's comments are verbose but
largely *not* fluff — most genuinely explain a WHY (a bug they prevent, an
invariant, a verified-live finding), which is the project's actual
institutional memory (there's no other durable place it lives besides git
blame and the newer `docs/development/changes/*.md` records). A wholesale
delete-and-rewrite risks destroying that faster than it can be reconstructed,
and is exactly the kind of drive-by sweep the project's own `AGENTS.md`/§25
discipline warns against as a detached project.

**Recommendation:** don't do a standalone sweep. Fold comment cleanup into
whichever phase above touches each file — when `Lexer.h` gets rewritten for
phase 0, that's the natural moment to also cut anything that's pure
restatement of code (a real violation of the project's own "WHY not WHAT"
rule, fair game to delete outright) and tighten anything saying a true thing
three times instead of once.

---

## 9. Open questions

- Which fork does phase 3 take: mutate-in-place (current behavior, kept but
  made explicit) vs. fully immutable trees with all annotations in side
  structures (Roslyn's actual choice)? The immutable fork is architecturally
  cleaner and is what actually delivers "stable product other programs
  consume," but is strictly more work. Needs an explicit decision, not a
  default.
- How does the double-parse pre-pass (`main.cpp`'s throwaway
  Resolver/TypeChecker run before the real one, needed for struct-layout
  ordering — `ARCHITECTURE_OVERHAUL.md` §27.2 D19) interact with a real
  lowering IR? Possibly resolved for free once Bound IR exists (struct
  layout becomes a property of the IR, not something Codegen's pre-pass has
  to discover twice) — needs investigation before phase 4, not assumed.
- Should this become a numbered feature group (its own `ARCH-00N`) inside
  `ARCHITECTURE_OVERHAUL.md`'s existing tracking system, or live as an
  independent roadmap the way this document currently does? Recommend the
  former once phase 0 is actually decided on, so it gets the same
  session-protocol rigor (`FREEZE.md`, change records, `INVARIANTS.md`) the
  rest of the overhaul already has.

## 10. Related material

- `turf-lsp/plan/02-compiler-core-api-changes.md` — items A (facade) and F
  (per-analysis context object) are the `turf-lsp`-side view of phases 0-2
  above, already written down, currently deferred.
- `docs/architecture/compiler_pipeline.md` §6 — the existing CFG
  static-analysis subsystem this document proposes promoting into a real
  lowering stage (§4.3, §7 phase 4).
- `docs/planning/ARCHITECTURE_OVERHAUL.md` §27 (Deferred Work Register) —
  D11 is the adjacent, currently-tracked "build more tooling" item; this
  document is the missing "why the compiler's shape makes that hard" half.
- `docs/development/changes/STD-003.md` — the 2026-09-26 session §1/§7
  point to. Its own *Known pitfalls* has the full repro for the three
  compiler bugs found that session (self-call `Subst` recovery, `FuncType`
  substitution, cast legality) and for the `mem::Pool<T>.free_all()` case
  that surfaced phase 0★'s starting point.
