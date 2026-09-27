# The Compiler Pipeline: Lexer to Codegen

Deep internals reference for how `turf` actually turns a `.tr` file into a
native binary — aimed at contributors and technically curious readers, not
at people looking for Turf language syntax. Like
`docs/architecture/modules_and_scope_resolution.md` (read that first if
you haven't — this doc follows its style and doesn't repeat its module/
import material) and `docs/architecture/pointers_and_references.md`, this
is grounded directly in the current source with file:line citations
throughout, verified against the compiler as it stands, not against any
planning document.

**Deliberate framing constraint**: everything below is described in
ordinary computer-science terms — recursive-descent parsing, dataflow
analysis, control-flow graphs, visitor-pattern traversal — rather than in
terms of LLVM's own IR or API surface. LLVM is the backend Turf happens to
target today; the compiler's own algorithms don't know or care about that
choice until the very last step, and neither does this document, beyond
saying in passing that the type-checked AST "eventually lowers to LLVM IR
and native code."

Related docs:
- `docs/architecture/modules_and_scope_resolution.md` — the module/import
  system and `::` operator, pass by pass. Cross-referenced below rather
  than repeated.
- `docs/architecture/pointers_and_references.md` — `unsafe { }`, `&T`,
  raw pointers.
- `docs/architecture/memory_model.md` — heap/stack layout and the runtime
  representation generics compile down to (companion doc, written
  alongside this one — see §7 below for where the two meet).
- The 2026-08 split of the original monolithic
  `Parser.cpp`/`Resolver.cpp`/`TypeChecker.cpp` into the per-category files
  (`ParserCore.cpp`, `ResolverDecl.cpp`, `TypeCheckerCall.cpp`, ...) cited
  throughout this document. The split changed file organization only — every
  pass's actual algorithm is unchanged from before it. Its plan doc,
  `docs/planning/COMPILER_MODULE_BREAKDOWN.md`, was retired once the split
  landed (same convention as `PHASE_2_ISSUES.md`/`FINAL_SPRINT.md`; the text
  is in git history) — **this document is where the per-pass file layout is
  now described**, and every split file's own header doc comment points here.

---

## 0. The pipeline, end to end

```
source.tr
   │
   ▼
 Lexer   ──tokens──▶  Parser  ──AST──▶  Resolver (2 passes)  ──annotated AST──▶  TypeChecker  ──type-annotated AST──▶  Codegen  ──▶  native code
```

Five passes, each consuming the *concrete data structure* the previous one
built and producing a new one:

| Pass | Input | Output |
|---|---|---|
| Lexer | raw source bytes | a stream of `Token` values, each carrying a `SourceLocation` |
| Parser | token stream | an AST — a tree of `ExprAST` subclass instances (~42 concrete node kinds) |
| Resolver | AST | the *same* AST, mutated in place: every name reference gets a resolved symbol/candidate-list annotation written directly onto its node |
| TypeChecker | resolved AST | the *same* AST again, plus a side table (`NodeTypes_`) mapping every node to an inferred `TurfTypePtr` |
| Codegen | type-annotated AST | LLVM IR → a native object file → a linked binary (or, with `--emit-llvm`, human-readable IR text) |

This is the project's own stated design rule (`docs/planning/
ROADMAP_v1.0.0_COMPLETE.md` §1, "Multi-Pass Pipeline": *"Parser → Resolver
→ TypeChecker → Codegen. Name resolution and semantics are separated from
LLVM IR generation."*) and it is a genuine architectural constraint, not
just a description after the fact: **Codegen is only ever allowed to run
on an AST that has already passed both the Resolver and the TypeChecker
cleanly.** `src/main.cpp`'s driver (`runCompilerMain()`, `src/main.cpp:
498-1226`) enforces this literally — if `Resolver::resolveAll()` fails,
the process returns before `TypeChecker` is even constructed
(`src/main.cpp:1009-1012`); if `TypeChecker::checkAll()` fails, the
process returns before a single `AST->codegen()` call runs
(`src/main.cpp:1099-1103`). Codegen itself is written to *trust* this:
it reads fields like `ResolvedCallTarget` and inferred types with no
re-validation, on the assumption that if it's running at all, those
fields are already correct.

**Why five separate passes instead of one combined walk?** The concrete
reason is forward references. A name can be *used* in the source before
it's *declared* — two top-level functions calling each other, a struct
field referencing a type declared later in the file, a call to a function
defined further down. A single combined parse-and-check walk has no way
to answer "does `bar` exist" while still checking `foo`'s body, if `bar`
is textually declared after `foo`. Splitting name registration
(hoisting, done once up front) from name *use* (checked afterward, with
every name already registered) is the general solution — see §3 for
exactly how the Resolver does this in two passes of its own, nested
inside the larger five-pass pipeline. The same logic extends to the
later passes: TypeChecker needs every name already resolved before it can
ask "what type does this resolve to," and Codegen needs every type
already checked before it can safely choose which LLVM operation to
emit for e.g. `a + b` (integer add vs. float add vs. string concat).

### The double-parse wrinkle

`main.cpp` doesn't literally run five passes once each — it runs a
**pre-pass** (its own throwaway `Resolver`/`TypeChecker` instances,
`src/main.cpp:652-802`) before the mandatory pipeline described above.
The pre-pass exists purely so that LLVM function *prototypes* (and fully
built struct types) exist before any function *body* is compiled — this
is what makes `main()` calling a `helper()` defined later in the file
work at the Codegen layer, independent of and in addition to whatever the
Resolver's own two-pass hoisting already does at the name-resolution
layer (§3). Concretely, the pre-pass:

1. Parses every top-level declaration in the file into `PrePassASTs`
   (`src/main.cpp:563-645`).
2. Runs a throwaway `Resolver`/`TypeChecker` pass over it
   (`src/main.cpp:652-700`) — informational only; this pass's own
   diagnostics never reach the user directly (see the note below).
3. Fully codegens every struct declaration first (`src/main.cpp:
   729-737`) — struct layouts must be complete before any GEP
   (struct-field-address computation) referencing them is built, even
   from a method declared textually *above* its own struct (see the
   long comment at `src/main.cpp:702-728` for the exact LLVM-verifier
   failure this used to produce).
4. Emits every function prototype with no body (`g_protoOnlyPass = true`,
   `src/main.cpp:745-758`).
5. Fully codegens every function body (`src/main.cpp:774-802`).

The file is then **closed, reset, and re-parsed from scratch**
(`src/main.cpp:804-821`) for the real, authoritative pass: a fresh AST is
built, the *mandatory* `Resolver`/`TypeChecker` run over it
(`src/main.cpp:997-1103`), and only non-function top-level statements are
codegenned again (function bodies were already emitted in the pre-pass;
re-emitting them here would duplicate them, `src/main.cpp:1105-1120`).
The pre-pass's own Resolver/TypeChecker errors are silently discarded if
the pre-pass TypeChecker fails (`PreTCClean`, `src/main.cpp:672-700`) —
skipping straight to the real, user-facing diagnostics the *mandatory*
main-pass TypeChecker produces once it re-encounters the identical error
a few hundred lines later. This two-parse structure is a real
implementation detail worth knowing if you're reading `main.cpp` for the
first time, but it's orthogonal to the five-pass pipeline itself: from
the Resolver/TypeChecker/Codegen's own point of view, each still only
ever sees "an AST," built the normal way, once per real invocation.

---

## 1. The Lexer

`include/Lexer.h`, `src/Lexer.cpp`. Converts the raw source byte stream
into a sequence of `Token` values (`enum Token`, `include/Lexer.h:
266-351` — a flat set of negative-int constants for multi-character
tokens/keywords, plus raw ASCII values for single-character operators).

### `gettok()` / `gettokScan()`

The actual scanning is done by `gettokScan()` (`src/Lexer.cpp:469-1026`),
a straightforward hand-written switch over `LastChar`/`SourceFile->peek()`
— maximal-munch tokenization with no separate tokenizer-generator or
regex table: each branch greedily consumes the longest token it can (e.g.
the `:` branch peeks for a second `:` to decide between `TOK_COLON` and
`TOK_SCOPE`, `src/Lexer.cpp:787-798`; the `.` branch peeks up to two
characters ahead to disambiguate `.` / `..` (range) / `...` (variadic),
`src/Lexer.cpp:735-749`). Identifiers are checked against a keyword table
(`Keywords`, `src/Lexer.cpp:113-163`) *after* being scanned as an ordinary
identifier run — note that builtin/stdlib function names (`print`,
`malloc`, `abs`, ...) are deliberately **not** in this table
(`include/Lexer.h:116-122`): they're ordinary identifiers, resolved later
by the Resolver against `lib/std/*.tr`, not lexer-level keywords. This
matters architecturally — it means the lexer has zero knowledge of the
standard library at all; every "builtin-looking" name is exactly as
unprivileged as a user's own function name until the Resolver decides
otherwise.

`gettok()` itself (`src/Lexer.cpp:1047-1052`) is a thin wrapper around
`gettokScan()` that stamps the token's *end* position uniformly:

```cpp
int gettok() {
  int Tok = gettokScan();
  CurLoc.EndLine = CurLine;
  CurLoc.EndCol = CurCol;
  return Tok;
}
```

### `SourceLocation` and span tracking

```cpp
struct SourceLocation {
  int Line;
  int Col;
  std::string Filename;
  int EndLine = -1;
  int EndCol = -1;
};
```
(`include/Lexer.h:44-50`.) `Line`/`Col` mark where the token *starts*;
`EndLine`/`EndCol` (added later than the other three fields, hence
appended rather than inserted — see the field's own doc comment for why
field order matters for the hundreds of pre-existing 2-/3-argument
aggregate-init call sites across the codebase) mark one column past where
it *ends*, populated for every token kind uniformly by the `gettok()`
wrapper above. `-1` means "no end position recorded" (distinct from "same
column as the start") — still true for genuine multi-token spans (a whole
declaration, a qualified `A::B::C` path, a failing sub-expression), which
would need each AST node to track its own end location from its last
consumed token, not just a lexer-level change (open scope, issue #241
Track D). This single/multi-token distinction is what lets
`DiagnosticEngine`'s terminal renderer (`printDiag()`, `src/Lexer.cpp:
180-293`) underline a token's *full width* (`"^" + string(SpanWidth - 1,
'~')`, `src/Lexer.cpp:252-256`) instead of a single caret, whenever an end
position is available, and fall back to a bare `^` when it isn't.

`Filename` is stamped at token-scan time from the global `SourceFilename`
(`src/Lexer.cpp:491`), which is repointed at each imported module's own
path while that module is being parsed and restored afterward — this is
what lets a diagnostic raised by a *later* pass (Resolver/TypeChecker/
Codegen, all of which run after every file involved has already finished
parsing) still recover the correct source file and line for a location
that originated inside an imported module, via the persistent
`AllSourceLines` cache (`include/Lexer.h:100-112`) rather than the
transient, single-file `SourceLines`.

### Recovery machinery (brief — see `docs/architecture/errors_and_recover.md`-adjacent detail in code)

Diagnostics are collected, not thrown as C++ exceptions (LLVM is commonly
built with exceptions disabled) — `TurfError::raise()` records the
diagnostic then `longjmp()`s back to the nearest active `setjmp()`
recovery point (`g_recoverJmp`/`g_recoverActive`, `include/Lexer.h:
125-129`), used pervasively by the parser/pre-pass/main-pass loops in
`main.cpp` for per-statement recovery. `DiagnosticEngine` (`include/
Lexer.h:184-264`) is the single choke point every diagnostic — errors and
warnings alike — funnels through, and caps at one primary error per
source line (`ErrorLines`, `src/Lexer.cpp:342-343`) to avoid cascading
noise from one bad statement.

---

## 2. The Parser

`src/ParserCore.cpp`, `src/ParserDecl.cpp`, `src/ParserStmt.cpp`,
`src/ParserLiteral.cpp`, `src/ParserModule.cpp` (split by category from a
single monolithic `Parser.cpp` in the 2026-08 module split — every file's top
doc comment says so explicitly). Classic **recursive-descent** parsing: each grammar
production is one C++ function (`ParseIfStmt()`, `ParseStructDecl()`,
`ParseType()`, ...) that consumes tokens via the shared `CurTok`/
`getNextToken()` cursor and recursively calls the functions for whatever
sub-productions it contains, building an AST bottom-up as it returns.

### `getNextToken()` — the single token-advance choke point

```cpp
int getNextToken() {
  if (CurTok == '{' || CurTok == '(' || CurTok == '[')
    g_braceDepth++;
  else if ((CurTok == '}' || CurTok == ')' || CurTok == ']') && g_braceDepth > 0)
    g_braceDepth--;
  return CurTok = gettok();
}
```
(`src/ParserCore.cpp:59-65`.) Every single token advance anywhere in the
parser goes through this one function, which is why it's the natural
place to track `g_braceDepth` — the current `{`/`(`/`[` nesting depth
relative to the top level — with zero per-call-site bookkeeping. This
depth is what lets `SkipToNextTopLevelBoundary()` (`src/ParserCore.cpp:
150-208`) resynchronize correctly after a parse error at *any* nesting
depth (not just one level deep) when scanning forward for the next safe
top-level declaration to resume at.

### Expression parsing: precedence-climbing (operator-precedence parsing)

Confirmed technique: **precedence climbing** (a form of operator-precedence
parsing, in the same family as a Pratt parser), not a hand-written
grammar of separately-named productions per precedence level (no
`ParseAdditiveExpr()`/`ParseMultiplicativeExpr()` chain exists). A single
integer-keyed precedence table drives it:

```cpp
static std::map<int, int> BinopPrecedence; // Precedence table: '*' > '+'
```
(`src/ParserCore.cpp:41`, populated by `InitializePrecedence()`,
`src/ParserCore.cpp:67-133`.) Selected bindings, low to high:
assignment/compound-assignment = 10, cast (`to`) = 12, `||` = 15,
`&&` = 20, `==`/`!=` = 25, relational (`<`/`>`/`<=`/`>=`) = 30, `|` = 33,
`^` = 35, `&` = 37, `<<`/`>>` = 38, `+`/`-` = 40, `*`/`/`/`%` = 50,
`**` (exponentiation) = 60 — the tightest-binding operator in the
language.

The driving loop, `ParseBinOpRHS()` (`src/ParserCore.cpp:860-958`), is
the textbook algorithm: parse one primary/unary operand, then repeatedly
look at the next operator's precedence — if it binds *at least* as
tightly as the precedence floor this call was invoked with, consume it
and recursively parse its right-hand side (bumping the floor to
one-higher, so a same-or-lower-precedence operator that follows correctly
stops the recursion and returns control to the outer call instead of
being swallowed); otherwise return what's been built so far. Right
associativity (assignment: `a = b = c` groups as `a = (b = c)`; `**`:
`2 ** 3 ** 2` groups as `2 ** (3 ** 2)` = 512) is handled by *not*
bumping the floor for those specific operators when recursing
(`isRightAssociative`, `src/ParserCore.cpp:898-909`) — every other binary
operator is left-associative by the ordinary "bump the floor" rule.

### `ParseType()` — types are parsed by their own recursive grammar

`src/ParserCore.cpp:229-...` parses a type expression (primitives, `void`,
pointers `T*`, arrays `T[N]`, generics `list<T>`, the `!T` error-union
form, module-qualified `Module::Type`) via its own direct self-recursion
— a nested generic (`list<list<list<...>>>`) or a stacked error-union
(`!!T`) recurses `ParseType()` straight into itself with no detour back
through `ParseExpression()`. This is exactly why `ParseType()` carries its
*own* `ParseRecursionGuard` (`src/ParserCore.cpp:237-243`) rather than
relying on `ParseExpression()`'s: a deeply-nested generic type was found
via code review to be completely unbounded even after
`ParseExpression()`/`ParseBlock()` were already guarded, since neither of
those functions is ever re-entered by this kind of type-only
self-recursion (`include/Lexer.h:138-170`, `MaxParseRecursionDepth =
2000`).

### AST node types

Roughly 42 concrete node kinds, all deriving from one abstract base,
`class ExprAST` (`include/AST.h:18`) — every statement, declaration, and
expression in the language is (despite the historical `...ExprAST` naming
convention) an `ExprAST`, split by category across four headers mirroring
the parser's own file split:

| Header | Node count | Examples |
|---|---|---|
| `include/ASTExpr.h` | 16 | `NumberExprAST`, `BinaryExprAST`, `AssignmentExprAST`, `CastExprAST`, `VariableExprAST` |
| `include/ASTStmt.h` | 11 | `IfExprAST`, `WhileExprAST`, `DoWhileExprAST`, `ForEachExprAST`, `ReturnExprAST`, `RecoverExprAST`, `UnsafeBlockExprAST` |
| `include/ASTDecl.h` | 9 | `FuncDefExprAST`, `StructDeclExprAST`, `EnumDeclExprAST`, `ModuleImportExprAST`, `VarDeclExprAST` |
| `include/ASTAccess.h` | 6 | `FuncCallExprAST`, `MethodCallExprAST`, `ScopeAccessExprAST`, `FieldAccessExprAST` |

A recurring design pattern worth calling out: the parser sometimes builds
a node that's deliberately **underspecified**, leaving a later pass to
fill in what it can't yet know. `ScopeAccessExprAST` (`Left::Right`,
`include/ASTAccess.h:86-...`) is the clearest example — the parser has no
way to know at parse time whether `Left` names an enum, a module, or a
struct (that requires knowing what's been declared program-wide, which
isn't settled until hoisting, §3), so it only ever fills in `LeftName`/
`RightName`/locations and leaves `ResolvedKind = Unresolved`; the
Resolver sets the real kind once `Left`'s identity is known. See
`docs/architecture/modules_and_scope_resolution.md` §2.3 for the full
story of this one node.

---

## 3. The Resolver: two-pass name resolution

`src/ResolverCore.cpp`, `src/ResolverDecl.cpp`, `src/ResolverExpr.cpp`,
`src/ResolverStmt.cpp`, `src/ResolverCall.cpp`, plus the symbol-table
headers (`include/ResolverSymbolTable.h`, `include/ResolverScopeTable.h`,
`include/ResolverModuleTable.h`). The Resolver's job is to attach a
resolved identity — a symbol, an overload candidate list, a module/struct
owner — to every name reference in the AST, mutating the AST nodes
**in place** (writing fields like `ResolvedCallTarget`/`ResolvedKind`/
`ResolvedSymbol` directly onto the node objects) so that every later pass
can read that identity back without redoing any lookup.

### Why two passes: hoisting

`Resolver::resolveAll()` (`src/ResolverCore.cpp:47-102`) runs in exactly
two passes over the same node list:

```cpp
void Resolver::resolveAll(
    const std::vector<std::unique_ptr<ExprAST>> &Nodes,
    bool RecoverPerDeclaration) {
  // Pass 1 — Hoisting
  hoistTopLevel(Nodes);

  // Pass 2 — Full Resolution
  for (const auto &Node : Nodes)
    resolve(Node.get());
  ...
}
```

**Pass 1, hoisting** (`hoistTopLevel()`/`hoistModuleBody()`,
`src/ResolverCore.cpp:417-...`, `535-...`) walks only *top-level*
declarations — no function bodies, no nested expressions — and registers
every function signature, struct, and enum name it sees into the symbol
table (`registerFunctionSignature()`, `RegisteredStructs_`/
`RegisteredEnums_`). It never looks inside a function body at all.

**Pass 2, full resolution** (`resolve()`, `src/ResolverCore.cpp:33-44`,
dispatching via `Node->accept(*this)` into the visitor methods described
in §4's ASTVisitor discussion) walks *everything*, bodies included, and
resolves every name reference it finds — variable reads, function calls,
type annotations — against the symbol table Pass 1 already fully
populated.

This is **hoisting** in the general programming-language sense: lifting
every top-level *declaration* to the front of processing, before any
*use* is checked, so that use-before-declaration in source-text order is
never actually a problem for the resolver. Concretely, this is what makes
the following compile:

```turf
fn isEven(n: int): bool {
    if (n == 0) return true;
    return isOdd(n - 1);   // forward reference: isOdd is declared BELOW
}

fn isOdd(n: int): bool {
    if (n == 0) return false;
    return isEven(n - 1);
}
```

A single combined walk (parse-and-resolve-together, or resolve-bodies-as-
you-go) would have no entry for `isOdd` in the symbol table at the moment
it's checking `isEven`'s body, because `isOdd`'s own declaration hasn't
been reached yet in a single top-to-bottom traversal. Splitting "register
every signature" (Pass 1, bodies untouched) from "check every use" (Pass
2, full walk) means that by the time Pass 2 ever looks inside *either*
function's body, *both* signatures are already registered, regardless of
which one appears first in the source file. The same mechanism is what
lets a struct field's type annotation reference a struct declared later
in the file, and what lets a module's own function call another function
in that same module regardless of declaration order (self-reference,
`docs/architecture/modules_and_scope_resolution.md` §1.3).

Note Pass 1 itself is recursive with respect to imports:
`hoistModuleBody()` is called once per `ModuleImportExprAST` found while
hoisting, and hoists that module's own top level the same way — see the
module-resolution doc's §2.7 for the full on-demand-import story
(`registerOnDemandImports()`, module visibility gating, `TypeOwningModule_`)
rather than repeating it here; that machinery is layered on top of the
same Pass-1/Pass-2 split described above, not a separate mechanism.

### Generic substitution happens during hoisting, too

A subtlety worth flagging: `ParseType()` (§2) has no way to know at parse
time that a bare identifier like `T` in `fn<T> identity(x: T): T` names
the function's own generic type parameter rather than an ordinary struct
— it always emits a plain `NamedType("T")`. `substituteTypeParams()`
(`src/ResolverCore.cpp:131-182`) rewrites that into a real
`TurfGenericParamType` the moment a generic function's signature is
captured into its symbol-table entry (`buildFunctionSymbol()`,
`src/ResolverCore.cpp:296-...`, itself called from
`registerFunctionSignature()` during Pass-1 hoisting) — recursing through
pointer/reference/array/generic-argument wrapping so `Box<T>`,
`&T`, and `T[]` parameters all get their embedded `T` correctly
substituted too. Every downstream consumer (overload resolution here in
the Resolver, later the TypeChecker, later still Codegen) then sees a
real `TurfGenericParamType` rather than re-deriving "is this name
actually a generic parameter" itself. See §7 for what a
`TurfGenericParamType` means at the type-system level.

---

## 4. The TypeChecker: a visitor-pattern type-inference pass

`class TypeChecker : public ASTVisitor` (`include/TypeChecker.h:100`).
`ASTVisitor` (`include/ASTVisitor.h`) is the textbook **visitor pattern**:
an abstract base with one virtual `visitX(XExprAST &)` method per
concrete AST node kind, every one defaulting to a no-op —

```cpp
class ASTVisitor {
public:
  virtual void visitNumber(NumberExprAST &Node) {}
  virtual void visitBinary(BinaryExprAST &Node) {}
  virtual void visitIf(IfExprAST &Node) {}
  ...
};
```
— so a concrete pass only has to override the node kinds it actually
cares about. Every AST node class implements `accept(ASTVisitor &)`,
which calls back into the visitor's matching `visitX()` method (classic
double dispatch: which `visitX()` runs depends on both the concrete node
type *and* the concrete visitor type). `Resolver` and `TypeChecker` are
both `ASTVisitor` subclasses; so is the debug-dump tool `ASTPrinter`
(`include/ASTPrinter.h`) — the same traversal interface backs a real
compiler pass and a pure diagnostic tool with zero shared logic beyond
"know how to walk the tree."

### Where inferred types live

`TypeChecker` walks the *resolved* AST bottom-up and infers a
`TurfTypePtr` for every expression node — but, unlike the Resolver (which
writes its results directly onto AST node fields), the TypeChecker keeps
its results in a **side table**:

```cpp
std::unordered_map<const ExprAST *, TurfTypePtr> NodeTypes_;
```
(`include/TypeChecker.h:434`), read/written via `getType()`/`setType()`
(`src/TypeCheckerCore.cpp:111-124`) keyed by the node's own pointer
identity. This is a deliberate, if partial, separation of concerns —
per the header's own framing comment (`include/TypeChecker.h:15-20`):
*"The TypeChecker does NOT replace Codegen yet; it runs in parallel and
its output (inferred types stored in `NodeTypes_`) will gradually replace
the equivalent monolithic type logic in Codegen."* A small number of
values Codegen genuinely cannot re-derive on its own are still written
directly onto the AST node instead, alongside the Resolver's own
annotations — e.g. `RecoverExprAST::ResolvedSuccessType`
(`src/TypeCheckerCall.cpp:1410`, see §4.2 below), stashed there because
every `!T` value lowers to the *same* runtime shape regardless of what
`T` actually is, so Codegen has no other way to recover it.

**A genuine architectural wrinkle worth knowing, not glossing over**:
Codegen still has its *own*, separate, structurally independent
type-inference function, `getExprType()` (`src/CodegenCore.cpp:499-...`)
— a second implementation of "what type does this expression have,"
re-deriving the answer by walking the AST directly rather than consulting
`TypeChecker::NodeTypes_` at all. The module-resolution doc documents a
real, live wrong-answer bug this duality caused (`docs/architecture/
modules_and_scope_resolution.md` §2.10: a module-qualified call's mangled
name reconstruction, needed by `getExprType()`'s own `FuncCallExprAST`
branch, silently produced an inaccurate type for a string-returning
function, causing `==` to pick pointer comparison instead of `strcmp`).
Both systems are used, for different purposes, and they can drift out of
sync — this is confirmed current behavior, not a hypothetical, and worth
keeping in mind when reading Codegen source: a "what type is this" answer
inside `Codegen*.cpp` almost never comes from the TypeChecker's own
`NodeTypes_` table.

### Generic type substitution at check time

By the time the TypeChecker sees a generic function's signature, its
`TurfGenericParamType`s are already in place (§3) — overload resolution
(`visitFuncCall()`, `src/TypeCheckerCall.cpp`) matches call-site argument
types against candidate signatures using the ordinary `typesCompatible()`
type-compatibility check (`src/Types.cpp`), which has a dedicated fast
path recognizing `TurfGenericParamType` as "matches anything" — this is
what makes `identity(42)` resolve cleanly against `fn<T> identity(x: T):
T` without the TypeChecker needing any generics-specific overload-
resolution code of its own; an open type parameter is just a type that's
compatible with everything, from the type-compatibility check's point of
view. See §7 for the runtime-representation side of this (what actually
gets emitted for a generic parameter's storage).

### `!T`: the error-union type

`TurfErrorUnionType` (`include/Types.h:682-695`) is the frontend
representation of Turf's fallible-return convention, `!T` — a function
declared `fn risky(): !int` returns either something compatible with
`int`, or a value of *any* declared `error` type (an open/unenumerated
union, unlike e.g. Rust's `Result<T, E>` — there is no specific error
type named in the signature, only "any error is possible").

Two places the TypeChecker specifically understands this shape:

- **`visitReturn()`** (`src/TypeCheckerStmt.cpp:284-...`): when the
  enclosing function's declared return type is an error union, a
  `return` statement is checked as compatible if the returned value is
  *either* compatible with the success type `T`, *or* is a value of some
  declared error type (`isDeclaredErrorTypeValue()`,
  `src/TypeCheckerStmt.cpp:301-307`) — there's no specific error type to
  check against, so only "is this some declared error value at all"
  matters.
- **`visitRecoverExpr()`** (`src/TypeCheckerCall.cpp:1389-1457`) type-
  checks Turf's three `recover` forms (`recover produce <value>`,
  `recover run { }`, `recover as x run { }` — see
  `docs/architecture/errors_and_recover.md` for the full language-level
  design). It first
  rejects `recover` on anything whose type isn't actually an error union
  (`!WrappedTy->isErrorUnion()`, `src/TypeCheckerCall.cpp:1394-1404`);
  for `recover produce`, checks the produced value against the success
  type; for `recover run`/`recover as x run`, requires the block to
  diverge on every path (`AllPathsDiverge()`, a small AST-level
  structural check, `src/TypeCheckerCall.cpp:1360-1386` — not the CFG
  itself, since the CFG for this function hasn't been built yet at
  TypeChecker time; see §6) *unless* the success type is `void`, in which
  case simply running the block and falling through is valid (there's no
  value the expression needs to produce). Every form's overall type is
  the success type `T`, never `!T` — `recover` is specifically the
  operation that unwraps the union.

---

## 5. Codegen: walking the type-checked AST, function by function

`src/CodegenCore.cpp`, `src/CodegenFunc.cpp`, `src/CodegenExpr.cpp`,
`src/CodegenStruct.cpp`, `src/CodegenVar.cpp`, `src/CodegenArray.cpp`,
`src/CodegenControlFlow.cpp`. Every `ExprAST` subclass implements its own
`codegen()` virtual method (not a separate visitor this time — Codegen
predates the `ASTVisitor` infrastructure the Resolver/TypeChecker use, and
still uses old-fashioned virtual dispatch directly on the node), so the
overall walk is: for every top-level declaration, call `->codegen()`,
which recursively calls `->codegen()` on its children, bottom-up, exactly
mirroring the AST's own recursive shape.

`FuncDefExprAST::codegen()` (`src/CodegenFunc.cpp:197-...`) is itself
split into a prototype half and a body half (`g_protoOnlyPass`,
`src/CodegenFunc.cpp:191-196, 270-274`) — see §0's double-parse-wrinkle
discussion for why main.cpp calls it twice. Function identity is a
mangled name encoding the module and parameter types
(`"turf_" + module_prefix + name + "_" + paramType + "_" + paramType...`,
`src/CodegenFunc.cpp:198-216`) — this is what lets two same-named
overloads (or two same-named functions in different modules) coexist as
distinct compiled functions without colliding.

Once a function's body exists as an AST, and immediately before finishing
that function's codegen, Codegen builds a **control-flow graph** for it
and runs a battery of static-analysis passes over that graph — this is
substantial enough to be its own subsystem, covered in full in §6.

---

## 6. The CFG static-analysis subsystem

Layered into Codegen (there is no separate top-level driver stage for
this — it happens once per function, inline, inside
`FuncDefExprAST::codegen()`), but conceptually its own thing: a
**control-flow graph** is built for every function body, and roughly ten
independent static-analysis checks run over it, several of them genuine
textbook dataflow analyses. All of it runs **unconditionally on every
compile** — none of it is gated behind `--smart`/SLM or any other flag.

### The CFG itself

`include/CFG.h`. A `CFG` is the ordinary compiler-construction data
structure: a set of `TurfBasicBlock`s (`include/CFG.h:32-79` — a name, a
list of `ExprAST*` statements, predecessor/successor pointers, and a
`TerminatorKind` — `None`/`Return`/`Break`/`Continue`/`Branch`/
`Unconditional`), one designated entry block, and one **virtual exit
block** that every `return` path (and ordinary fallthrough) is connected
to. `CFGBuilder::buildCFG()` (`src/CFG.cpp:469-499`) constructs this by
walking the function body's AST once (`visitExpr()`/`visitIf()`/
`visitWhile()`/... , `src/CFG.cpp:511-...`), splitting into a new block
at every branch point exactly as a standard AST-to-CFG lowering does, and
finishes with a reachability pass (`computeReachability()`,
`src/CFG.cpp:52-77` — a plain BFS from the entry block, `include/CFG.h:
141`).

Two AST-shaped, non-dataflow diagnostics fall directly out of building
the CFG itself, both worth knowing as *two distinct* checks rather than
one, because they're raised through completely different code paths:

- **Statement after a terminator** — `CFGBuilder::visitExpr()`
  (`src/CFG.cpp:511-522`) raises `StatementAfterTerminatorError` (**E012**,
  fatal) the moment it's asked to visit a statement while the current
  block already has an explicit terminator, anchored at the terminator's
  own location (not the unreachable statement's). A **variable
  declaration** specifically after a terminator takes an entirely
  separate path: `VarDeclExprAST::codegen()` has its own, earlier check
  against a `SymbolTable`-tracked early-exit flag
  (`CurrentScopeHasEarlyExit()`) and raises `UnreachableCodeError`
  (**E013**) instead, with different wording — this CFG-block-terminator
  check never even runs for that case, since Codegen's own check fires
  first, before the CFG pass gets a chance to see the declaration at all.
- **Missing return** — `reportFlowDiagnostics()` (`src/CFG.cpp:375-400`)
  raises `MissingReturnError` (**E009**, fatal) when the function's
  declared return type is non-`void` and `allPathsReturn()`
  (`src/CFG.cpp:92-108` — every predecessor of the virtual exit block
  that's reachable must have a `Return` terminator) is false. Void
  functions are never checked for this.

### Order of passes (as wired in Codegen)

`FuncDefExprAST::codegen()` (`src/CodegenFunc.cpp:390-477`) runs, per
function, in this exact order:

```
buildCFG()
  → reportFlowDiagnostics()          (unreachable/dead-branch/missing-return)
  → calculateCyclomaticComplexity()
  → hasUnbreakableLoop()
  → DefiniteAssignmentPass
  → ValueRangePass
  → AliasAnalysisPass
  → LivenessAnalysisPass
  → allPathsRecurse()                (infinite-recursion check)
  → ConstructorFieldAssignmentPass   (only for a StructName.new constructor)
```

### Cyclomatic complexity

`CFG::calculateCyclomaticComplexity()` (`src/CFG.cpp:135-140`) — the
standard formula `M = E - N + 2P` (edges minus nodes plus 2, for a
single connected component), counted over *every* block including the
virtual exit block and its incoming edges. `src/CodegenFunc.cpp:405-407`
warns (`CyclomaticComplexityWarning`, non-fatal) when `M > 15`.

### Unreachable code / dead branches — was a real inconsistency, fixed by HYG-001

`reportFlowDiagnostics()` (`src/CFG.cpp:375-...`) calls
`getUnreachableBlocks()` (any non-exit, non-empty block the entry-block
BFS never reached) and `getDeadBranches()` (unreachable blocks whose
terminator is `Branch`). **This used to print both as plain, uncolored
`std::cerr <<` strings**, *not* routed through `DiagnosticEngine`/
`TurfWarning` like every other diagnostic in this document — confirmed
live at the time (compiling a `while true { return 1; } return 2;`
function printed exactly `Warning: Unreachable code in block 'while.end'
of function 'main'` with no `[E0NN]` code, no color, no hint, no source
snippet, and compilation still succeeded), even though the dedicated
`UnreachableBlockWarning`/`DeadBranchWarning` classes already existed in
`include/ErrorsFlow.h` and were simply never constructed by anything.

Fixed by HYG-001 (`docs/development/changes/HYG-001.md`,
2026-09-18): `reportFlowDiagnostics()` now constructs
`UnreachableBlockWarning(BB->getTerminatorLoc(), FunctionName,
BB->getName())`/`DeadBranchWarning(BB->getTerminatorLoc(), FunctionName)`
and calls `.warn()`, same as every other warning in this pass. Both
render with a real `[E0NN]` code (`E074`/`E075`), color, caret, source
snippet, and hint, and both participate in `DiagnosticEngine`'s
one-primary-error-per-line cap like everything else. Anchored at the
unreachable block's own *terminator* location, not its first statement's
— `ExprAST` has no virtual `getLoc()` (every concrete node type defines
its own, non-polymorphically), so there is no generic way to read a
location off an arbitrary statement here; this mirrors
`StatementAfterTerminatorError`'s identical precedent earlier in this
document rather than inventing new AST-wide dispatch. *When* the warnings
fire did not change — only how they render.

### Use-before-initialization — `DefiniteAssignmentPass`

A textbook **forward dataflow analysis** (`src/Analysis.cpp:252-353`).
Only symbols introduced by a `VarDeclExprAST`/`ArrayDeclExprAST` *inside*
the function body are tracked (`AllBodyDeclared`) — parameters are always
considered pre-assigned and excluded entirely. Equations:

```
OUT[b] = IN[b] ∪ Defs(b)
IN[b]  = ∩ OUT[pred]   over every predecessor of b (meet = set intersection)
```

Intersection at merge points is the semantically load-bearing choice
here: a variable is only "definitely assigned" at a program point if it
was assigned on **every** incoming control-flow path, not just some of
them. The analysis runs to a fixed point (`src/Analysis.cpp:301-330`,
worklist-style: recompute `IN`/`OUT` for every non-entry block until
nothing changes), then makes a second pass purely to report: any read of
a body-declared symbol not yet present in that block's fixed-point `IN`
set raises `DefiniteAssignmentError` (**E008**, fatal), with a
`withRelated()` note pointing back at the declaration
(`src/Analysis.cpp:336-352`). Because `CFGBuilder` deliberately adds
condition/subject expressions as statements on their own dedicated
condition blocks (`if`/`while`/`do-while`/`for`/`for-each`/`compare`
subject, `src/CFG.cpp:600` onward), a variable used only inside a loop's
own guard expression is checked by this pass too, not just inside loop
bodies.

### Dead stores and unused variables — `LivenessAnalysisPass`

The other half of the classic pair: a **backward dataflow analysis**
(`src/Analysis.cpp:373-535`), computing per-block `Gen` (upward-exposed
reads — a read not preceded by a write to the same variable earlier in
the same block) and `Kill` (writes), then the standard liveness
equations:

```
LiveOut[B] = ∪ LiveIn[S]           over every successor S of B
LiveIn[B]  = Gen[B] ∪ (LiveOut[B] − Kill[B])
```

iterated to a worklist fixed point (`src/Analysis.cpp:454-478`). A
variable declared in the body but never read *anywhere* (tracked via a
separate `EverRead` set, deliberately independent of `Gen[]`'s
upward-exposed-only semantics — see the code's own comment,
`src/Analysis.cpp:410-421`, on the bug this fixed: a plain
declare-then-use-in-the-same-block pattern is never upward-exposed, so a
naive "was it ever in some `Gen[b]`" check misflagged the single most
common local-variable pattern in the language as unused) produces
`UnusedVariableWarning`. A write whose target isn't in the live-after set
at that program point — overwritten before being read, or the function
exits before it's read — produces `DeadStoreWarning`
(`src/Analysis.cpp:480-534`). Both are non-fatal, and both exclude
parameters unconditionally. **Confirmed by direct compilation** (not just
reading the test's own comments — `tests/14_cfg_analysis/
12_dead_store_simple.tr`'s header comment claims `x = 10` is "not a dead
store because `x` is subsequently returned"; it is a dead store, because
the function actually returns the literal `0`, never `x`): compiling that
file produces three warnings — one `UnusedVariableWarning` plus **two**
separate `DeadStoreWarning`s, one for the initializer and one for the
later reassignment.

### Unbreakable-loop detection — Tarjan's SCC algorithm

`CFG::hasUnbreakableLoop()` (`src/CFG.cpp:167-243`) runs **Tarjan's
strongly-connected-components algorithm** over the reachable subgraph,
deliberately chosen over simpler path-based back-edge detection (see the
function's own doc comment for two concrete false-negative cases —
`break` nested inside another loop, and branching inside `while true` —
that a naive back-edge check gets wrong and the SCC view gets right). A
closed SCC — two or more blocks, or one block with a self-edge — that has
**no** outgoing edge to any block outside itself is an unbreakable loop:
control can enter it but can never leave. A `while true`/`do { } while
(true)` with a *literal* `true` condition deliberately omits the
condition-block-to-loop-exit edge at CFG-build time (`src/CFG.cpp:
790-809, 883-895`) — that's precisely what makes the resulting SCC
"closed" and triggers the warning; an ordinary `while(cond)` always keeps
that exit edge, so its SCC is never closed and this never fires.
Non-fatal (`InfiniteLoopWarning`). A related but genuinely separate
check, `SuspiciousInfiniteLoopWarning` (`src/Lint.cpp:476,497`), is a
plain AST pattern-match (not CFG-based, no SCC computation) with similar
wording and intent — worth not conflating the two; they're independently
implemented and could theoretically diverge.

### Infinite-recursion detection — DFS with overload-aware self-call identity

`CFG::allPathsRecurse()` (`src/CFG.cpp:321-373`) is a **depth-first
search** from entry to the virtual exit block, answering: does *every*
path pass through at least one call back to the enclosing function
itself? A back-edge (an ordinary loop cycle encountered mid-DFS) is
conservatively treated as "this path recurses," since a cycle can't prove
a base case exists. The interesting engineering detail is
`blockContainsSelfCall()` (`src/CFG.cpp:257-308`): it prefers comparing
the call's *mangled* target name (which encodes parameter types) over its
bare base name, specifically so that one overload calling a
*differently-aritied sibling overload* of the same base name — e.g.
`assert(cond)` delegating to `assert(cond, msg)` — is never
misidentified as self-recursion. Produces `RecursionWithoutBaseError`
(**E010**, fatal — flagged `Important` in `include/ErrorCodes.h`, since
it's a guaranteed stack overflow, not a style nit).

### Use-after-free / double-free — `AliasAnalysisPass`

`src/AliasAnalysis.cpp`. A forward, per-block **worklist dataflow**
tracking two things per program point: a points-to map (`VariableSymbol*
→ optional<AllocSiteID>`) and a set of already-freed `AllocSiteID`s. This
is a lightweight points-to analysis, not a full alias analysis: it
recognizes exactly three patterns — `x = malloc(n)`/`realloc(n)` as
minting a fresh allocation-site identity, a plain variable-to-variable
assignment (`p = q`) as points-to propagation (aliasing), and `free(x)`
as marking that allocation site freed — then flags any later array access
(`x[i]`) through a variable still pointing at an already-freed site.
Merging at a control-flow join is deliberately conservative ("prefer
existing mapping, do not try to join alloc sites" per the code's own
comment, `src/AliasAnalysis.cpp:70`). Tracking is keyed by
`VariableSymbol*` identity, not variable name — the code comment at
`src/AliasAnalysis.cpp:24-41` documents a real bug this fixed: name-keying
conflated a shadowed inner variable with an unrelated outer one of the
same name, producing a false double-free. Produces `DoubleFreeError`
(**E017**) / `UseAfterFreeError` (**E018**), both fatal and both flagged
`Important`, each carrying a `withRelated()` note pointing at the earlier
free.

### Constant propagation and compile-time array-bounds checking — `ValueRangePass`

`src/ValueRange.cpp`. Another forward worklist dataflow, tracking a
per-symbol `optional<long long>` known-constant value
(`evalConstExpr()`, `src/ValueRange.cpp:30-61`, folding literal integers
and simple `+`/`-`/`*`/`/` combinations of already-known constants), with
meet-at-merge-points requiring all predecessors to agree on the exact
same value (else the symbol becomes unknown again — the same
conservative "any disagreement collapses to unknown" shape
`AliasAnalysisPass`'s value-merge uses). Whenever an array access's index
expression evaluates to a statically-known constant against a
fixed-size array whose declared size is also known, `ArrayBoundsError`
(**E001**) fires immediately at *compile time* (`src/ValueRange.cpp:
120-142`) if the constant is out of range — this is genuinely distinct
from (and in addition to) any separate runtime bounds check the same
access would also get: `mut i: int = 5; arr[i]` against a smaller
fixed-size `arr` fails to compile outright, rather than merely crashing
at runtime.

### Definite field assignment for constructors — `ConstructorFieldAssignmentPass`

`include/Analysis.h:54-64`, run only when the function's internal mangled
name ends in `.new` (i.e. only for a struct's own `new()` constructor,
`src/CodegenFunc.cpp:462-476`). Structurally this is the *exact same*
forward-dataflow shape as `DefiniteAssignmentPass` above — meet as
intersection at merge points, fixed-point iteration — just tracking
`self.<field>` assignment names instead of local-variable identities, and
checking the accumulated set at the CFG's **exit** block rather than at
each individual use site (there's no "use" of a field to check here, only
whether it was written on every path that reaches the function's end).
Raises `IncompleteConstructorError`, naming every field that was missed
on at least one path — a genuine control-flow-sensitive check, not merely
"is it assigned *somewhere* in the body": a field assigned only inside
one branch of an `if` with no covering `else` is still correctly flagged.

### `CallGraphPass` — confirmed, currently, a stub

Worth stating plainly rather than implying more than exists:

```cpp
void CallGraphPass::runGlobal() {
  std::lock_guard<std::mutex> lock(g_cg_mutex);
  // Function is currently just a stub.
}
```
(`src/CallGraph.cpp:13-16`, called once per compilation from
`src/main.cpp:1123-1124`.) It takes a lock and returns — no summary is
printed, no cross-function analysis runs. Separately, `AddCallEdge()`
(`src/CallGraph.cpp:18-21`) *is* actively called during Codegen
(`src/CodegenFunc.cpp:377`, once per call site) and does populate a
module-level `caller → set<callee>` map — but nothing anywhere in the
codebase ever reads that map back out. The data is collected; nothing
consumes it. Any future whole-program call-graph analysis (recursive-cost
estimation, cross-function dead-function detection, inlining heuristics)
would have its raw edge data ready to go, but as of this writing, no such
analysis exists.

---

## 7. Generic monomorphization

Turf's generics are implemented as **monomorphization**: one fully
specialized, independently-compiled clone of a generic function, method,
or struct per distinct tuple of concrete type arguments actually used in
the program (`docs/development/FREEZE.md` §6.1, frozen). This is the
result of the Architecture Overhaul's `TYPE-002` feature group
(`docs/development/changes/TYPE-002.md`), which replaced an earlier type-erasure
implementation outright — the two headline doc comments in
`include/Types.h` (`TurfGenericParamType`: *"replaced with concrete types
during monomorphization"*; `GenericType`: *"the core of monomorphization"*)
now describe the actual implementation, not an aspiration.

**What actually happens**: a new pass, the **Monomorphizer**
(`include/Monomorphize.h`/`src/Monomorphize.cpp`), sits between the
TypeChecker and Codegen as pre-pass sub-pass 2b.5:

```
Lexer → Parser → Resolver(2) → TypeChecker → Monomorphizer → Codegen
```

While the pre-pass TypeChecker runs, every generic call site
(`visitFuncCall`/`visitMethodCall`, `TypeCheckerCall.cpp`) and every
generic-typed variable declaration or field
(`visitVarDecl`/`visitStructDecl`, `TypeCheckerDecl.cpp`) records an
`InstantiationRequest` — the generic declaration plus the concrete type
arguments the call site or declaration actually needs — into an
`InstantiationCollector`. The Monomorphizer then drains this worklist to a
fixed point: for each still-undrained request it clones the generic
declaration's AST, substitutes every occurrence of its `TurfGenericParamType`
type parameters with the concrete types the request names, renames the
clone via `Monomorphize::instantiatedDeclName()` (a pure function of base
name and type arguments — e.g. `identity` + `[int]` → `identity_gen1_int`,
`List` + `[int]` → `List_gen1_int`), clears its type parameters so it is no
longer generic, and re-resolves/re-typechecks it exactly like an ordinary,
already-concrete declaration. Draining is a fixed point rather than a
single sweep because re-typechecking one clone's body can discover *new*
generic call sites (a nested generic call, a generic struct field) that
themselves need draining — the collector's cache is scanned non-destructively
each iteration so growth mid-sweep is picked up automatically. A generic
declaration's own **template** AST node is left untouched in the tree —
`FuncDefExprAST::codegen()`/`StructDeclExprAST::codegen()` both refuse to
emit code for a node that still has type parameters — so only clones ever
reach Codegen.

The result is genuine, independent compiled code and layout per
instantiation: `List<int>` and `List<string>` are two distinct
`llvm::StructType`s with two distinct sets of compiled methods
(`turf_List.push_gen1_int_...`, `turf_List.push_gen1_string_...`), not one
shared implementation reached through a cast. `GlobalSymbolTable` holds a
separate registration for each concrete instantiation actually used,
keyed by its mangled name — never a single shared `"List"` entry standing
in for all of them. See `docs/architecture/memory_model.md` §2.7 for the
storage-layout consequences (in particular: there is no pointer-sized
"erased slot" any more, so a struct-typed `T` larger than one machine word
is not a special case).

**Instantiation depth is capped.** A genuinely unbounded recursive generic
instantiation (a generic function whose body calls itself through its own
still-open type parameter in a way that would keep generating new,
distinct concrete instantiations forever) is rejected with a dedicated
diagnostic (`E079`, `GenericInstantiationDepthError`) once the worklist's
drain depth exceeds `Monomorphize::kMaxInstantiationDepth` (64, FREEZE.md
§6.1) — a compile-time error naming the instantiation chain, not an
unbounded compile or a stack overflow in the compiler itself. `TYPE-002`'s
own `Known pitfalls`/`OVH-014` document the diagnostic's one remaining
rough edge (it currently names the whole instantiation cache rather than
just the offending chain — tracked as roadmap `LEC-005`, not a soundness gap).

**Why monomorphization over type erasure**: the project's earlier
erasure-based implementation traded away real correctness, not just
optimization opportunity — every type parameter erased to one opaque
64-bit slot, so a struct-typed `T` larger than one machine word could not
be losslessly represented at all (an ICE on any attempt), no per-`T`
operator dispatch was possible (an equality check on a generic `K`
silently compared raw pointer bits instead of calling `operator==` for the
concrete type), and `sizeof(T)` inside a generic body had no correct
answer to give. Most importantly, a precise garbage collector (F-MEM-1)
cannot trace a heap where a `T`-typed field is sometimes a real pointer
and sometimes a scalar value stored in a pointer-shaped slot purely by
convention — monomorphization, which gives every field a real, distinct,
statically-known type, is F-MEM-1's own explicit prerequisite
(`docs/development/FREEZE.md` §6.5). The real cost monomorphization pays
instead is compile-time and code-size growth proportional to the number of
*distinct concrete instantiations* a program actually uses, not just the
number of generic declarations written — bounded in practice by the depth
cap above and, per `TYPE-002`'s own measurements, offset in this compiler's
case by `OPT-001`'s dead-code stripping of now-genuinely-unreachable
generic template bodies (which erasure, by design, always kept
reachable).

**One shared type walker, not seven hand-written ones (`SMP-002`).**
Substituting a generic type parameter and detecting whether a type still
has an open one are used by every pass in this section, and used to be
implemented independently, by hand, in at least eight separate recursive
functions across four files — `Resolver::substituteTypeParams`,
`typesCompatible()`'s per-position generic-parameter leniency,
`TypeCheckerCall.cpp`'s call-site substitution recovery and its companion
declaration-order recovery, `TypeCheckerCall.cpp`'s two generic-struct
field-substitution scans, `Monomorphize::substituteConcreteTypes`, and
`visitCast`'s cast-legality bypass. Three of those eight were
independently, silently incomplete (`ARCHITECTURE_SIMPLIFICATION.md` §1;
`docs/development/changes/SMP-002.md`'s own *Removed assumptions* names a
fourth, found live during the unification itself). `include/TypeWalk.h`/
`src/TypeWalk.cpp` now hold the one implementation: `containsOpenParam()`
(parameterized by how far to look — a bare check, through pointer/array/
reference wrapping, or also through a `GenericType`'s own argument list,
since the eight original functions deliberately did not all look equally
far), `substitute()` (a name → replacement-type map walk, serving both the
declaration direction and the instantiation direction with the same code),
`unify()` (the call-site binder `collectGenericSubstitution()` used to be),
and two smaller companions for the two shapes above that don't fit a plain
`unify()` (`collectParamOrder()`, `collectOrderedFieldSubstitution()`).
Every consumer in the Resolver/TypeChecker/Monomorphizer now calls into
this file rather than re-deriving its own notion of "what a generic type
parameter looks like, and where it might be hiding." Codegen's own,
separate `getExprType()` type-inference engine (`CodegenCore.cpp`)
independently reimplements the same pattern in at least two more places
and was deliberately left out of this unification — see the change
record's *Unresolved questions* for why.

---

## Summary: file map

| Stage | Primary files |
|---|---|
| Lexer | `include/Lexer.h`, `src/Lexer.cpp` |
| Parser | `src/ParserCore.cpp`, `ParserDecl.cpp`, `ParserStmt.cpp`, `ParserLiteral.cpp`, `ParserModule.cpp`; AST node definitions in `include/AST.h`, `ASTExpr.h`, `ASTStmt.h`, `ASTDecl.h`, `ASTAccess.h` |
| Resolver | `src/ResolverCore.cpp`, `ResolverDecl.cpp`, `ResolverExpr.cpp`, `ResolverStmt.cpp`, `ResolverCall.cpp`; symbol tables in `include/ResolverSymbolTable.h`, `ResolverScopeTable.h`, `ResolverModuleTable.h` |
| TypeChecker | `include/TypeChecker.h`; `src/TypeCheckerCore.cpp`, `TypeCheckerCall.cpp`, `TypeCheckerDecl.cpp`, `TypeCheckerExpr.cpp`, `TypeCheckerStmt.cpp`; the visitor interface itself in `include/ASTVisitor.h` |
| Codegen | `src/CodegenCore.cpp`, `CodegenFunc.cpp`, `CodegenExpr.cpp`, `CodegenStruct.cpp`, `CodegenVar.cpp`, `CodegenArray.cpp`, `CodegenControlFlow.cpp` |
| CFG subsystem | `include/CFG.h`, `src/CFG.cpp`; `include/Analysis.h`, `src/Analysis.cpp`; `src/AliasAnalysis.cpp`; `include/ValueRange.h`, `src/ValueRange.cpp`; `include/CallGraph.h`, `src/CallGraph.cpp` |
| Type system | `include/Types.h`, `src/Types.cpp` |
| Generic type walker | `include/TypeWalk.h`, `src/TypeWalk.cpp` |
| Driver | `src/main.cpp` |
