# The Compiler Pipeline: Lexer to Codegen

How `turfc` turns a `.tr` file into a native binary, for contributors. This
is not a language reference. It is written to be read next to the source:
every function, file and class named here exists, and the text says which
pass owns which decision.

Related documents:

- `docs/architecture/modules_and_scope_resolution.md`: the module/import
  system and the `::` operator, pass by pass. This document does not repeat
  it.
- `docs/architecture/pointers_and_references.md`: `unsafe { }`, `&T`, raw
  pointers.
- `docs/architecture/memory_model.md`: heap and stack layout, and the layout
  consequences of monomorphized generics (its section 2.7).
- `docs/architecture/errors_and_recover.md`: the `!T` error-union design that
  sections 4 and 6 refer to.

Each source file under `src/` opens with a header comment that points back to
this document for the per-pass file layout. That layout is summarized in the
file map at the end.

---

## 0. The pipeline, end to end

```
source.tr
   |
   v
 Lexer --tokens--> Parser --AST--> Resolver (2 passes) --annotated AST--> TypeChecker
                                                                              |
                                  Codegen <--(clones appended)-- Monomorphizer <--+
```

| Pass | Input | Output |
|---|---|---|
| Lexer | source bytes | a stream of tokens, each carrying a `SourceLocation` |
| Parser | token stream | an AST: a tree of `ExprAST` subclasses (every node kind listed in `TURF_AST_NODE_LIST`, `src/frontend/ASTVisitor.h`) |
| Resolver | AST | the same AST, mutated in place: every name reference gets its resolved identity (symbol, overload candidates, owning module) written onto the node |
| TypeChecker | resolved AST | a side table (`NodeTypes_`) mapping each node to its inferred `TurfTypePtr`, plus a few values written onto nodes |
| Monomorphizer | type-checked AST + the instantiation requests the TypeChecker recorded | fully concrete clones of every generic declaration actually used (section 7) |
| Codegen | type-annotated AST | LLVM IR, then a native object file, then a linked binary (`--emit-llvm` stops at IR text) |

The rule the structure enforces: **Codegen only runs on an AST that the
Resolver and the TypeChecker accepted.** `runCompilerMain()`
(`src/driver/main.cpp`) returns before the next stage when a stage reports an
error. Codegen reads `ResolvedCallTarget` and the inferred types without
re-validating them.

**Why separate passes.** The reason is forward references. A function may
call one declared further down, a struct field may name a type declared
later, two functions may call each other. Registering every declaration
first (hoisting, section 3) and checking every use afterwards is the general
solution, and each later pass relies on the one before it the same way: the
TypeChecker asks "what type does this resolve to" and needs every name
resolved, Codegen picks `a + b` as integer add, float add or string
concatenation and needs every type known.

### The driver: a pre-pass and a main pass

`runCompilerMain()` runs the front end twice over the entry file, because
Codegen needs every function prototype and every struct layout to exist
before any function body is compiled.

**Pre-pass** (its own `Resolver` and `TypeChecker` instances):

1. *Parse.* Every top-level declaration of the entry file is parsed into
   `PrePassASTs`. The entry file's leading `unsafe module;` pragma goes
   through `ParseLeadingUnsafeModuleDecl()`, the prelude imports are injected
   by `ParsePreludeImports()` (as if they were the first lines of the entry
   file only), and `ClaimEntryFileImportSlot()` makes sure the entry file is
   not parsed a second time when a prelude module transitively imports it
   (see the modules document). A top-level `unsafe { }` block is spliced into
   its children by `AppendFlattenedTopLevelDecl()`. A parse error is recovered
   per declaration with `SkipToNextTopLevelBoundary()`.
2. *Resolve* (sub-pass 2a). `Resolver::resolveAll()` over `PrePassASTs`. If it
   fails, the driver flushes the diagnostics and returns.
3. *Type-check* (2b). `TypeChecker::checkAll()` with an
   `Monomorphize::InstantiationCollector` attached. The result is kept in a
   flag (`PreTCClean`); the diagnostics themselves are already in
   `DiagnosticEngine`.
4. *Monomorphize* (2b.5), only when `PreTCClean`. The four `drain...Requests`
   functions run in a loop until none reports progress (section 7). Each clone
   is appended to `PrePassASTs`.
5. *Structs first* (2c). Every `StructDeclExprAST` is codegenned before any
   function, so that a field-address computation built inside a method that is
   textually above its struct sees the complete layout.
6. *Prototypes* (3a). With `g_protoOnlyPass` set, every function, enum, extern
   declaration and module import is codegenned prototype-only, so forward
   calls resolve. An uninstantiated generic template is skipped: only its
   clones reach Codegen.
7. *Bodies* (3b), only when `PreTCClean`. Every function body is codegenned,
   which is where the per-function CFG analyses of section 6 run. Structs
   (already done in 2c), `export immut` constants (no storage) and generic
   templates are skipped.

**Main pass.** The file is reset and parsed again from scratch into a fresh
AST (`FnASTs` for declarations and imports, `TopLevelASTs` for anything
else). The mandatory `Resolver` and `TypeChecker` run over that AST; failure
of either returns before any `codegen()` call of this pass. Only
`TopLevelASTs` is codegenned here, because function bodies were already
emitted by the pre-pass. A statement that is not a declaration is rejected
as "Statements are not allowed outside of a function".

What follows is shared by both: after the diagnostics are flushed, `--check`
returns (after the entry-point check), and otherwise the driver wraps `main`
with the runtime bootstrap (`WrapMainWithRuntimeBootstrap()`), emits type
descriptors (`EmitAllTurfTypeDescs()`), runs `verifyModule()` on the whole
module, runs the optimization pipeline, and emits an object file or IR text
and links.

Two consequences worth knowing. The pre-pass type-check failure does not lose
a diagnostic: errors are recorded in `DiagnosticEngine` when raised, and the
main-pass checker raising the same error on the same line is suppressed by
the engine's one-primary-error-per-line cap. And the pass structure is
invisible to each pass: the Resolver, TypeChecker and Codegen only ever see
an AST built in the normal way.

---

## 1. The Lexer

`src/frontend/Lexer.h`, `src/frontend/Lexer.cpp`. Converts source bytes into
`Token` values (`enum Token`: negative constants for multi-character tokens
and keywords, raw ASCII values for single-character operators).

### `LexerState`

Everything the lexer reads and writes lives in a `LexerState` (F-S0, issue
#301): the stream (`Source`, an owned `File` or a caller's buffer), the
`Filename` stamped into every location, the cursor (`LastChar`, `CurLine`,
`CurCol`), the current token (`CurLoc`, `IdentifierStr`, `StringVal`, the
`NumLit*` fields, `BoolVal`, `CharVal`), the previous token's end and the
lookahead. `gettok(LexerState &)` works on the one it is given, so two
buffers can be tokenized in turn without either seeing the other's cursor
(`tests/api/analysis_test.cpp`, `testTwoLexersInterleave`). `Keywords` is a
constant table.

The parser reads tokens from `Lex()`, the lexer of the current `ParserState`
(see [`ParserState`](#parserstate) below). With no parser current, or one
with no lexer, `Lex()` is an empty lexer that yields `TOK_EOF`.

Not part of `LexerState`, because `gettok()` never reads them:
`SourceFilename`, `SourceLines`, `AllSourceLines` and `MainSourceFilename`,
the compile's record of which file is being read and what its lines say,
which diagnostics and the passes after parsing use.

### `gettok()` and `gettokScan()`

`gettokScan()` is a hand-written maximal-munch scanner over `LastChar` and
`Source->peek()`: each branch consumes the longest token it can. The `:`
branch peeks for a second `:` to choose between `TOK_COLON` and `TOK_SCOPE`;
the `.` branch looks ahead to separate `.`, `..` (`TOK_RANGE`) and `...`
(`TOK_ELLIPSIS`). Identifiers are scanned as an ordinary run and then looked
up in the `Keywords` table. A lone `_` is `TOK_WILDCARD`; `_name` is an
ordinary identifier.

Standard-library names (`print`, `malloc`, `abs`, ...) are deliberately not
in `Keywords`. The lexer knows nothing about the library; such a name is as
unprivileged as a user's function until the Resolver looks it up.

`gettok()` wraps `gettokScan()` and stamps the token's end position (and the
previous token's end, `PrevTokEndLine`/`PrevTokEndCol`, which the
missing-semicolon hint needs).

### `SourceLocation`

```cpp
struct SourceLocation {
  int Line;
  int Col;
  std::string Filename;
  int EndLine = -1;
  int EndCol = -1;
};
```

`Line`/`Col` mark where the token starts; `EndLine`/`EndCol` are one column
past its end, filled for every single token by `gettok()`. `-1` means "no end
recorded", which is different from "one column wide". A span covering several
tokens (a whole declaration, a qualified `A::B::C` path) has no end position:
that would need each AST node to record its own last token. The terminal
renderer (`printDiag()`) underlines the full width of a token when an end is
known and prints a single caret otherwise. The new fields are appended after
`Filename` so the many `{Line, Col}` and `{Line, Col, Filename}`
aggregate-initializers keep compiling.

`Filename` is stamped at scan time from the lexer's own `Filename`, which is
each import's path while that module is parsed. A diagnostic raised by a
later pass therefore still finds the right file, through the persistent
`AllSourceLines` cache (the single-file `SourceLines` is transient).

### Diagnostics and recovery

Diagnostics are collected, and recovery uses a C++ exception. While
`g_recoverActive` is set, `TurfError::raise()` records the diagnostic and
throws `RecoverUnwind`; each recovery point (the driver's per-declaration
loops, the Resolver's and TypeChecker's per-declaration loops, `ParseFile()`,
the query facade) catches it, abandons that declaration, and continues.
Without an armed recovery point `raise()` exits the process. `RecoverUnwind`
deliberately does not derive from `std::exception`, so the Resolver's
`catch (const std::runtime_error &)` blocks cannot swallow it. An exception
(rather than `longjmp`) is used because unwinding runs destructors: a
`longjmp` leaked the temporaries of every raising expression.

`DiagnosticEngine` (`src/frontend/Lexer.h`) is the one choke point every
diagnostic passes through. It caps output at one primary error per source
line (`ErrorLines`) to avoid cascades, unless an embedder sets
`CollectAllDiagnostics`.

---

## 2. The Parser

`src/frontend/ParserCore.cpp`, `ParserDecl.cpp`, `ParserStmt.cpp`,
`ParserLiteral.cpp`, `ParserModule.cpp` (with the cross-file declarations in
`Parser.h` and `ParserInternal.h`). Recursive descent: each production is one
function (`ParseIfExpr()`, `ParseStructDecl()`, `ParseType()`, ...) that
consumes tokens through the current parser's `CurTok`/`getNextToken()`
cursor.

### `ParserState`

Everything the parser reads and writes while it parses one file lives in a
`ParserState` (`Parser.h`, F-S1, issue #302): the lexer it reads (`Lexer`),
the current token (`CurTok`), the bracket depths (`BraceDepth`,
`CurlyDepth`), the recursion depth `ParseRecursionGuard` counts
(`RecursionDepth`), the flags one construct sets for the ones nested in it
(`AllowGuessType`, `SizeParamsInScope`, `InPatternHeader`,
`PatternHeadCallPending`, `FuncUsesTry`) and the file's module aliases
(`ModuleAliases`, see `ModuleKeyFor()`). The parse functions work on
`Parser()`, the innermost live `ParserScope`'s parser.

The driver makes one current for the input file. `ParseFile()` makes a fresh
`ParserState`, with a fresh `LexerState`, current for each import, and the
query facade, the formatter and `--migrate=pub` do the same for their
buffers, so the importer's or host's parser is never touched and nothing has
to be saved and restored field by field
(`tests/api/analysis_test.cpp`, `testTwoParsersInterleave`). With no scope
live, `Parser()` is an idle parser with no lexer.

Not part of `ParserState`: what one compile accumulates across every file it
parses (the import caches, the import stack, the parsed top-level
declarations, the files that declared `unsafe module;`, the source overlays:
`ParserCompileState` in `ParserModule.cpp`, which
`PushParserCompileState()`/`PopParserCompileState()` stack for an isolated
compile), `g_recoverActive` (error recovery, which the Resolver and
TypeChecker arm too) and the compile's file bookkeeping (`SourceFilename`,
`SourceLines`, `AllSourceLines`). `BinopPrecedence` is a constant table.

### `getNextToken()`: the one advance point

```cpp
int getNextToken() {
  ParserState &P = Parser();
  if (P.CurTok == '{' || P.CurTok == '(' || P.CurTok == '[')
    P.BraceDepth++;
  else if ((P.CurTok == '}' || P.CurTok == ')' || P.CurTok == ']') && P.BraceDepth > 0)
    P.BraceDepth--;
  /* ... the same for P.CurlyDepth, '{' and '}' only ... */
  if (takeLookaheadToken(P.CurTok))
    return P.CurTok;
  return P.CurTok = gettok(Lex());
}
```

Every token advance goes through it, which is why bracket depth is tracked
here with no per-call-site bookkeeping. `SkipToNextTopLevelBoundary()` uses
`BraceDepth` and `CurlyDepth` to resynchronize after an error at any
nesting depth.

### Expressions: precedence climbing

A single constant table (`BinopPrecedence`) drives
`ParseBinOpRHS()`; there is no per-level function chain. Low to high:
assignment and compound assignment 10, cast (`to`) 12, `||` 15, `&&` 20,
`==` `!=` 25, relational 30, `|` 33, `^` 35, `&` 37, shifts 38, `+` `-` 40,
`*` `/` `%` 50, `**` 60. The loop parses one operand, then while the next
operator binds at least as tightly as the current floor it consumes the
operator and recurses for the right-hand side. All assignment forms and `**`
are right-associative (`a = b = c` is `a = (b = c)`; `2 ** 3 ** 2` is 512):
`isRightAssociative` recurses with the same floor instead of floor + 1. The
cast operator is handled inline: its right side is a type, parsed with
`ParseType()`. Cast binds tighter than assignment so `y = x to int` casts `x`.

### `ParseType()`

Types have their own recursive grammar, in `ParserCore.cpp`:

- primitives, `void`, `ptr`, and `guess` (only where a local variable
  declaration permits it, via `g_AllowGuessType`);
- `!T`, the error-union type; `&T` and `&?T`, non-null and nullable safe
  references; `(&T)[N]`, an array of references (the parentheses are the only
  place a parenthesized type is accepted);
- `fn(T1, T2): R`, a function-value type;
- `Name`, `Module::Name`, and `Name<Args>` generics; trailing `*` for
  pointers and `[N]` / `[]` for arrays.

When it sees `Module::Name` it records the qualifier in the `NamedType` or
`GenericType` (`Module` field) and defers validation: module registration is
a Resolver pass-1 job, and the parser has no table of modules.

Because lexing is maximal munch, `List<List<int>>` ends with a `>>` token and
`List<T>=` with `>=`. `ParseType()` splits one `>` off such a token by
rewriting `CurTok` to the remainder without advancing, so the enclosing level
or the following `=` gets it.

`ParseType()` recurses into itself directly (nested generics, stacked `!`), so
it carries its own `ParseRecursionGuard`. The four guarded entry points are
`ParseExpression()`, `ParseBlock()`, `ParseUnary()` and `ParseType()`; past
`MaxParseRecursionDepth` (2000) they report `NestingTooDeep` instead of
overflowing the C++ stack.

### AST nodes

All nodes derive from `class ExprAST` (`src/frontend/AST.h`), which declares
`codegen()`, `codegenLValue()`, `clone()` and `accept(ASTVisitor &)`. The
concrete classes are split across `ASTExpr.h` (literals, operators, patterns,
`compare`), `ASTStmt.h` (control flow, `unsafe`, `recover`, `try`, `defer`),
`ASTDecl.h` (functions, structs, enums, machines, errors, variables, module
imports) and `ASTAccess.h` (calls, scope access, field/array access, slices).
`TURF_AST_NODE_LIST` in `ASTVisitor.h` is the authoritative list; adding a
node there breaks every exhaustive visitor until it is handled (section 4).

Some nodes are deliberately underspecified, leaving a later pass to complete
them. `ScopeAccessExprAST` (`Left::Right`) is the clearest case. At parse time
`Left` could be an enum, an error type, a struct, or a module; that depends on
program-wide declarations the parser has not seen. The node therefore carries
names and locations with `ResolvedKind = Unresolved`, and the Resolver fills
in the kind. The modules document describes this node in full.

---

## 3. The Resolver: two-pass name resolution

`src/resolve/`: `ResolverCore.cpp` (entry points and hoisting),
`ResolverTypeIdentity.cpp` (type identity), `ResolverDecl.cpp`,
`ResolverExpr.cpp`, `ResolverStmt.cpp`, `ResolverCall.cpp`,
`ResolverVisibility.cpp` (the `pub` checks); the class is declared in
`Resolver.h` and the symbol tables in `ResolverSymbolTable.h` (a composite over
`ResolverScopeTable.h`, `ResolverModuleTable.h`, `ResolverTypeRegistry.h`). The
Resolver attaches a resolved identity to every name reference, mutating the
nodes in place (`ResolvedCallTarget`, `ResolvedKind`, `ResolvedSymbol`, ...)
so later passes read it back without a second lookup.

### Pass 1 hoists, pass 2 resolves

`Resolver::resolveAll()` takes the node list through two passes:

```cpp
hoistTypes(Nodes);                  // 1a: every type declaration
resolveDeclarationSpellings(Nodes); // 1b: declaration-level type spellings
hoistTopLevel(Nodes);               // 1c: function and constant signatures
checkStructLayoutAcyclicity();      // by-value containment cycles
// Pass 2: resolve(Node) for every node, bodies included
```

Pass 1 looks only at top-level declarations, never inside a function body.
It registers each type under its identity (the modules document, section 2.6),
resolves the type spellings written in struct fields and signatures, and
registers every function signature (`registerFunctionSignature()`), every
`export immut` constant, and every import. Pass 2 then visits everything
through the `ASTVisitor` interface. With `RecoverPerDeclaration` set (the
analysis facade does this) each top-level node gets its own recovery point.

This is hoisting in the usual sense, and it is what makes forward references
work:

```turflang
fn is_even(n: int): bool {
    if (n == 0) { return true; }
    return is_odd(n - 1);   // declared below
}

fn is_odd(n: int): bool {
    if (n == 0) { return false; }
    return is_even(n - 1);
}
```

Pass 1 recurses through imports: `hoistModuleBody()` is called for each
`ModuleImportExprAST`, followed by `registerOnDemandImports()`. That
machinery (visibility gating, flat imports, type ownership) is in the modules
document; it is layered on this same two-pass split.

A struct that contains itself by value has no size. `checkStructLayoutAcyclicity()`
finds such a cycle with a three-colour depth-first search over by-value field
edges (`StructLayoutCycleError`). It runs at the end of pass 1, before anything
computes a layout, a default value or a GC descriptor.
`checkStructLayoutAcyclicityForClone()` repeats the check for one freshly
substituted generic clone, where a cycle can exist that the template does not
show (`struct A<T> { val: T }` instantiated as `A<B>` where `B` holds an `A<B>`).

### Generic parameters are marked during hoisting

`ParseType()` cannot tell that a bare `T` in `fn<T> identity(x: T): T` names
the function's own type parameter and not a struct; it emits `NamedType("T")`.
When a generic signature is captured into its symbol-table entry
(`buildFunctionSymbol()`, called from `registerFunctionSignature()`),
`substituteTypeParams()` rewrites those names into `TurfGenericParamType`,
recursing through pointers, references, arrays, `!T` and generic arguments.
It is a thin wrapper over the shared walker `TypeWalk::substitute()`
(section 7). From then on overload resolution, the TypeChecker and the
Monomorphizer all see a real parameter type and never re-derive "is this name
a type parameter".

`buildFunctionSymbol()` also computes the summaries later passes consult:
`MutatesSelf` (whether an instance method writes into `self`'s own storage),
`ReturnsParam`, `PossibleErrorTypeNames`, and `Decl` (the exact declaration,
which generic-operator instantiation uses to tell overloads apart).

---

## 4. The TypeChecker: a visitor-based inference pass

`class TypeChecker : public ASTVisitor` (`src/sema/TypeChecker.h`), split over
`TypeCheckerCore.cpp`, `TypeCheckerCall.cpp`, `TypeCheckerDecl.cpp`,
`TypeCheckerExpr.cpp`, `TypeCheckerStmt.cpp`, `TypeCheckerPattern.cpp`,
`TypeCheckerSlice.cpp` and `TypeCheckerVisibility.cpp`.

`ASTVisitor` (`src/frontend/ASTVisitor.h`) is the visitor pattern. The
`TURF_AST_NODE_LIST` macro declares one virtual `visit<Name>(<Class> &)` per
node kind, defaulting to a no-op, so a pass overrides only the nodes it cares
about. Every node's `accept()` calls the matching `visit` method (double
dispatch). `Resolver`, `TypeChecker` and the `NarrowingVisitor` of the
nullability pass derive from it directly. A pass whose correctness needs every
node (a lint, a printer, a child walker) derives from `ExhaustiveASTVisitor`
instead, where each `visit` is pure virtual: a new node kind is a compile
error until the pass decides what to do with it. `ASTChildWalker`
(`ASTChildWalker.h`) is the exhaustive visitor that simply recurses into
children, for passes that only intercept a few node kinds. Codegen is not a
visitor: each node implements its own `codegen()`.

### Where inferred types live

The TypeChecker walks the resolved AST bottom-up and records a type for every
expression in a side table, keyed by node pointer:

```cpp
std::unordered_map<const ExprAST *, TurfTypePtr> NodeTypes_;
```

read with `getType()` and written with `setType()`. A few results that Codegen
cannot re-derive are written onto the node instead, next to the Resolver's own
annotations: for example `RecoverExprAST::ResolvedSuccessType`, because every
`!T` lowers to the same runtime shape whatever `T` is.

**Codegen has its own type inference.** `getExprType()` (`CodegenCore.cpp`)
re-derives "what type is this expression" by walking the AST and does not
consult `NodeTypes_`. The two systems can disagree, and the disagreement has
produced real wrong-answer bugs: a module-qualified call whose mangled name
was reconstructed without its module prefix gave a wrong type for a
string-returning function, and `==` then compared pointers instead of calling
`strcmp`. When you read Codegen, assume a "what type is this" answer does not
come from the TypeChecker, and when you change one inference, check the other.

### Generic signatures in the checker

A generic function's signature carries `TurfGenericParamType`s (section 3).
`typesCompatible()` (`src/sema/Types.cpp`) treats an open parameter as
compatible with anything, which lets the checker type-check a template body
once, generically, and match `identity(42)` against `fn<T> identity(x: T): T`
without generics-specific overload code. That rule is reachable only for
templates: a monomorphized clone has every parameter substituted before it is
re-checked, so no open `TurfGenericParamType` reaches `typesCompatible()` for
a clone, and none reaches Codegen (section 7). An operator, field access or
method call whose operand is an open parameter itself (`a < b`, `a.x`,
`a.get()` with `a: T`; `TypeChecker::isOpenTypeParam()`) is not judged in the
template at all: there are no constraints (`FREEZE.md` section 6.1), so it is
accepted with a permissive type (`bool` for a comparison, otherwise the open
parameter), and the clone's re-check decides it for the concrete type. An error
there is reported at the instantiating call with that error's own code. At each
call, `TypeWalk::unify()`
binds the declared parameter types against the argument types to recover the
type arguments, and the checker records an `InstantiationRequest` for the
Monomorphizer.

### `!T`: the error-union type

`TurfErrorUnionType` (`src/sema/Types.h`) is the type of a fallible return,
`!T`: either a value of `T` or a value of any declared `error` type (an open
set; the signature names no specific error). Two checks understand it:

- `visitReturn()` (`TypeCheckerStmt.cpp`): when the enclosing function returns
  an error union, a `return` is accepted if its value fits `T` or is a value of
  a declared error type (`isDeclaredErrorTypeValue()`).
- `visitRecoverExpr()` (`TypeCheckerCall.cpp`) checks the three `recover`
  forms (`recover produce <value>`, `recover run { }`, `recover as x run { }`;
  see `errors_and_recover.md`). It rejects `recover` on an expression that is
  not an error union, checks a produced value against `T`, and for the block
  forms requires every path to diverge (`AllPathsDiverge()`, an AST-level
  structural test, because the CFG does not exist yet at this stage) unless
  `T` is `void`. The expression's type is `T`, never `!T`.

---

## 5. Codegen: walking the checked AST

`src/codegen/`: `CodegenCore.cpp`, `CodegenFunc.cpp`, `CodegenExpr.cpp`,
`CodegenStruct.cpp`, `CodegenVar.cpp`, `CodegenArray.cpp`,
`CodegenControlFlow.cpp`, `CodegenPattern.cpp`; `Codegen.h` and
`CodegenInternal.h` hold the shared declarations. For each top-level
declaration the driver calls `codegen()`, which recurses into children
bottom-up.

`FuncDefExprAST::codegen()` (`CodegenFunc.cpp`) has two halves selected by
`g_protoOnlyPass`: the first call creates the LLVM prototype, the second fills
the body. A function with non-empty `TypeParams` is a generic template and
returns immediately; only its clones are emitted. A function's symbol name is
a mangled string:

```
"turf_" + <module>_ + sanitizeSymbolName(name) + "_" + <param type> ...
```

so same-named overloads, and same-named functions in different modules, are
distinct symbols (`main` is exempt). A parameter type's mangled form and a
method's name carry the type's identity verbatim, e.g.
`turf_json_json::Parser.at_json::Parser_ptr`: `::` is legal in an LLVM
symbol and cannot occur in a source spelling, so it cannot collide.

`ModuleImportExprAST::codegen()` enters the module and codegens its body. The
pre-pass invokes it twice (prototype pass, body pass), so
`StructDeclExprAST::codegen()` returns immediately when the cached
`llvm::StructType` is already non-opaque; without that guard a second call
created a renamed duplicate type and corrupted `LLVMTypeCache`.

Just before finishing a function, Codegen builds its control-flow graph and
runs the analyses of the next section.

---

## 6. The CFG static-analysis subsystem

It runs once per function, inline in `FuncDefExprAST::codegen()`, on every
compile. None of it depends on `--smart` or any flag. It is also reachable
without Codegen: the query facade calls the same `runFunctionFlowPasses()`
(section 8).

### The CFG

`src/analysis/CFG.h`, `CFG.cpp`. A `CFG` is a set of `TurfBasicBlock`s (a
name, a list of `ExprAST *` statements, predecessors, successors, and a
`TerminatorKind` of `None`, `Return`, `Break`, `Continue`, `Branch` or
`Unconditional`), an entry block, and a virtual exit block that every return
path and fallthrough reaches. `CFGBuilder::buildCFG()` walks the body once,
starting a new block at every branch point, and finishes with
`computeReachability()` (a breadth-first search from the entry).

Two diagnostics come out of construction itself:

- *Statement after a terminator*: `CFGBuilder::visitExpr()` raises
  `StatementAfterTerminatorError` (E012, fatal), anchored at the terminator.
  Variable declarations go through the same check. A backstop in
  `VarDeclExprAST::codegen()` (`CurrentScopeHasEarlyExit()`) raises E012 as
  well; E013 is retired.
- *Missing return*: `reportFlowDiagnostics()` raises `MissingReturnError`
  (E009, fatal) when the return type is not `void` and `allPathsReturn()` is
  false.

`reportFlowDiagnostics()` also reports unreachable blocks and dead branches
as warnings through the normal diagnostic engine: `UnreachableBlockWarning`
(E074) for a reachable-by-text block the entry search never reaches, and
`DeadBranchWarning` (E075) when that block ends in a conditional branch. Both
are anchored at the block's terminator, because `ExprAST` has no virtual
`getLoc()` and a block's first statement cannot be asked where it is.

### Order of passes

`runFunctionFlowPasses()` (`src/analysis/Analysis.cpp`) is the single sequence:

```
reportFlowDiagnostics()            unreachable, dead branch, missing return
calculateCyclomaticComplexity()    warns above 15
hasUnbreakableLoop()               InfiniteLoopWarning
DefiniteAssignmentPass
ValueRangePass
AliasAnalysisPass
NullabilityPass                    &?T dereference and narrowing
EscapePass                         references that outlive their frame
LivenessAnalysisPass
allPathsRecurse()                  RecursionWithoutBaseError
ConstructorFieldAssignmentPass     only for `fn S.new`
```

A fatal error raises and ends the sequence for that function. Before the body
is generated, `FuncDefExprAST::codegen()` also lints it (`LintExpression()`)
and builds a throwaway early CFG for `ValueRangePass::markElidableChecks()`,
so Codegen can leave out bounds and overflow checks the analysis proves
redundant. That early pass never raises a diagnostic.

### Cyclomatic complexity

`calculateCyclomaticComplexity()` is `M = E - N + 2P` over every block,
including the virtual exit. `CyclomaticComplexityWarning` (non-fatal) fires
above 15.

### Use before initialization: `DefiniteAssignmentPass`

A forward dataflow analysis (`Analysis.cpp`) over the symbols declared by
`VarDeclExprAST` inside the body. Parameters are always assigned and not
tracked. A fixed-size array (`ArrayDeclExprAST`) is assigned from its
declaration: codegen zero-fills it, so a read of it is never E008 and
elements are not tracked.

```
OUT[b] = IN[b] union Defs(b)
IN[b]  = intersection of OUT[pred] over all predecessors
```

Intersection is the load-bearing choice: a variable is definitely assigned
only if it is assigned on every incoming path. The pass iterates to a fixed
point and then reports each read of a body-declared symbol that is not in the
block's `IN` set as `DefiniteAssignmentError` (E008, fatal), with a note at
the declaration. `CFGBuilder` places condition and subject expressions as
statements in their own blocks, so a variable used only in a loop guard is
checked too.

### `recover run` blocks in the CFG

`CFGBuilder::branchToRecoverBlocks()` builds the `recover run { }` and
`recover as e run { }` blocks of an ordinary statement or a `return` as CFG
blocks. It branches from just before the statement: one edge to the run block
(built like any block, so loops, `return`, `break` and `continue` inside it
have their edges), one edge to the block that holds the statement. The run
block therefore sees only what was assigned before the statement. It sets
`RecoverExprAST::BlockInCFG`, and the statement walkers (`AnalysisVisitor` for
liveness and definite assignment, `EscapeCheckVisitor`, `NarrowingVisitor`)
skip a block the CFG holds. A `recover` block in a loop condition, and a
`defer`'s statement, are not split; they are walked inside their statement.

### Dead stores and unused variables: `LivenessAnalysisPass`

A backward dataflow analysis. Per block it computes `Gen` (upward-exposed
reads) and `Kill` (writes), then iterates

```
LiveOut[B] = union of LiveIn[S] over successors S
LiveIn[B]  = Gen[B] union (LiveOut[B] - Kill[B])
```

to a fixed point. A body-declared variable that is never read anywhere is
`UnusedVariableWarning`. That test uses a separate `EverRead` set, because
`Gen` records only upward-exposed reads and a declare-then-use inside one block
is never upward-exposed. A write whose target is not live afterwards is
`DeadStoreWarning`; when that write is a declaration's initializer the message
names the initial value instead. A write to one element of an array does not
kill the array, so it never makes an earlier store to the array dead. Both are warnings, and both exclude parameters and
variables whose name begins with `_`.

### Unbreakable loops

`hasUnbreakableLoop()` runs Tarjan's strongly-connected-components algorithm
over the reachable subgraph. A closed SCC (two or more blocks, or one block
with a self edge) with no edge leaving it can be entered but not left. A
`while true` or `do { } while (true)` with a literal `true` condition omits
the condition-to-exit edge when the CFG is built, which is what closes the
SCC. An ordinary `while (cond)` keeps that edge. `InfiniteLoopWarning` is
non-fatal. `SuspiciousInfiniteLoopWarning` (`Lint.cpp`) is a separate,
AST-pattern check with similar wording; the two are independent.

### Infinite recursion

`allPathsRecurse()` is a depth-first search from entry to exit asking whether
every path passes through a call to the enclosing function. A back edge counts
as "recurses", since a cycle proves no base case. `blockContainsSelfCall()`
compares the call's mangled target name (which encodes parameter types) rather
than the bare name, so `assert(cond)` calling `assert(cond, msg)` is not
mistaken for self-recursion. The result is `RecursionWithoutBaseError` (E010,
fatal).

### Use after free and double free: `AliasAnalysisPass`

`src/analysis/AliasAnalysis.cpp`. A forward worklist dataflow over a
points-to map (`VariableSymbol *` to an optional allocation-site id) and a set
of freed sites. It is deliberately small: `x = malloc(n)`/`realloc` mints a
site, `p = q` copies the points-to fact, `free(x)` marks the site freed, and a
later access through a variable that still points at a freed site is an error.
At a join it keeps an existing mapping and does not try to merge sites.
Tracking is keyed by `VariableSymbol *`, not by name, so a shadowing inner
variable is not confused with an outer one. The errors are `DoubleFreeError`
(E017) and `UseAfterFreeError` (E018), both fatal, each with a note at the
earlier free.

### Reference escape: `EscapePass`

`src/analysis/Analysis.h`, `Analysis.cpp`. A forward dataflow pass shaped like
`NullabilityPass`. Per block it keeps a map `VariableSymbol *` to
`RefProvenance`; absence from the map means `Unknown`, the permissive default.
`meetProv()` keeps an entry only where every predecessor agrees. The
transfer function is a recursive `EscapeCheckVisitor`, and, after the
dataflow reaches its fixed point, a second phase checks every sink.
`classifyProvenance()` and `classifyAddressOfProvenance()` recognize a closed
set of expression shapes (`&x`, a variable read, a `heap()` call, a cast);
anything else is `Unknown` and never produces an error.

A value classified `Stack` raises `DanglingReferenceEscapeError` (E086, fatal)
at one of five sinks: a `return`; an assignment into a field of a base that is
a parameter or heap value; a call to a collection mutator (`push`, `set`,
`put`, `add`, `enqueue`, matched by the receiver's struct name); an
`operator[]=` dispatch; and the operand of `heap()`. Casting an unsized `T[]`
parameter to a raw pointer turns its `Param` provenance into `Stack`, because
that cast erases the aliasing information. Every other cast into a raw
pointer type leaves tracking: `unsafe` is the user opting out of this net.

### Constant propagation and array bounds: `ValueRangePass`

`src/analysis/ValueRange.cpp`. A forward worklist dataflow of known-constant
values (`evalConstExpr()` folds integer literals and `+ - * /` of known
constants). At a join, all predecessors must agree on the value or the symbol
becomes unknown. When an array index evaluates to a constant against an array
of known fixed size, an out-of-range index is `ArrayBoundsError` (E001) at
compile time, in addition to any runtime check.

### Constructor fields: `ConstructorFieldAssignmentPass`

Run only for a function whose name ends in `.new`. It has the shape of
`DefiniteAssignmentPass` but tracks `self.<field>` assignments, and checks the
set at the exit block instead of at uses. A field written on only some paths
is reported: `IncompleteConstructorError` names every field missed on at least
one path.

There is no whole-program call-graph pass.

### Pattern matching through the passes

`compare` with patterns (language rules in `docs/development/FREEZE.md`,
section 6.17) touches every pass. A `matches` header is an `ExprAST` in
`MatchesArm::Values` (with parallel `Guards` and the arm's `Bindings`): an
ordinary expression is the original equality pattern, and
`WildcardPatternExprAST`, `BindingPatternExprAST`, `StructPatternExprAST` and
`ErrorVariantPatternExprAST` (`ASTExpr.h`) are the destructuring forms.
Patterns are never evaluated as expressions (`codegen()` on one is fatal);
each pass handles them explicitly.

| Pass | What it does with a pattern |
|---|---|
| Lexer | `_` alone is `TOK_WILDCARD`; `_name` is an identifier. |
| Parser | `ParsePattern()` (`ParserStmt.cpp`) reads a header with the ordinary expression parser and reinterprets it: an expression followed by `{` was a struct name; in a nested position a bare identifier becomes a binding. `ParsingPatternHeader` switches off the "struct brace-literal is removed" hint for exactly that call. |
| Resolver | `resolveArmPattern()` (`ResolverExpr.cpp`): one arm scope, each binding declared once (immutable, placeholder type), a struct pattern's name validated by `validateNamedType()` like any annotation, stacked headers' binding sets compared. `normalizeHeaderPattern()` turns a bare `E::V` over an error type into a pattern. |
| TypeChecker | `visitCompare()` owns arm order, guards, unreachable arms and exhaustiveness. `checkPattern()` (`TypeCheckerPattern.cpp`) types one pattern against the subject, using `instantiatedFieldType()` (shared with `p.field`) so a generic instance's field type is the same either way, and records `FieldPattern::FieldType` and `StructPatternExprAST::MatchedType` for Codegen. For error patterns the subject must be an error, the field list complete or `..`, and sub-patterns are typed against the variant's declared fields. |
| CFG | `CFGBuilder::visitCompare()` builds a chain, not a fan-out: one test block per header holding the pattern and then the guard as its last statement, the match edge going to the arm body first and the fail edge to the next header. A pattern is a block statement, so `AnalysisVisitor::visitBindingPattern()` makes each binding a definition on the edge into the arm, and `NullabilityPass` narrows through a guard as through an `if`. The last header's fail edge is omitted when `CompareExprAST::Exhaustive` proves nothing falls through. |
| Monomorphizer | A guard is substituted like any body expression (`substituteTypesInBody()`); a pattern over a generic struct records the instantiation as a field access would. `StructPatternExprAST::clone()` shares its type spelling, so a clone keeps the template's resolution. |
| Codegen | `needsPatternCodegen()` (any struct pattern, `_` or guard) selects `CompareExprAST::codegenPatterns()` (`CodegenPattern.cpp`); otherwise the original `switch`/`strcmp` lowering runs. The pattern lowering snapshots the subject once into an entry-block slot, emits the ordered test chain, and registers bindings as entry-block allocas for the guard and body only. An error pattern compares the tag with `strcmp` and reads each field out of the payload struct. It is never a catch-all. |

---

## 7. Generic monomorphization

Generics are monomorphized: one fully specialized, independently compiled
clone of a generic function, method, constructor or struct per distinct tuple
of concrete type arguments the program uses (`docs/development/FREEZE.md`,
section 6.1). There is no type erasure: a type parameter is never represented
as an opaque pointer-sized slot. The invariant (INV-GEN-1) is that no
`TurfGenericParamType` reaches Codegen; it is asserted in Codegen and in the
Monomorphizer, and `FuncDefExprAST::codegen()` refuses to emit a node that still
has type parameters. The doc comments in `src/sema/Types.h` say the same
(`TurfGenericParamType`: replaced with concrete types during monomorphization;
`GenericType`: the core of monomorphization).

### The pass

`src/sema/Monomorphize.h`, `Monomorphize.cpp`. The driver runs it in the
pre-pass, after the TypeChecker and before struct codegen (sub-pass 2b.5, section
0). The facade does not run it.

While the pre-pass `TypeChecker` runs, every generic call site
(`visitFuncCall()`, `visitMethodCall()`, `TypeCheckerCall.cpp`) and every
generic-typed variable or field (`visitVarDecl()`, `visitStructDecl()`,
`TypeCheckerDecl.cpp`) records an `InstantiationRequest`: the generic
declaration plus the concrete type arguments. They go into an
`InstantiationCollector` that the driver attached with
`setMonomorphizationCollector()`.

The driver then loops over four drain functions until none reports progress:

```
drainFreeFunctionRequests   generic free functions and operators
drainMethodRequests         generic methods
drainConstructorRequests    generic StructName::new
drainStructRequests         generic struct layouts
```

Each one, for every undrained request, clones the generic declaration's AST,
substitutes every `TurfGenericParamType` with the request's concrete type
(`cloneAndSubstituteSelfBound()` for struct-bound declarations), renames the
clone with `Monomorphize::instantiatedDeclName()`, clears its type
parameters, and re-resolves and re-type-checks it as an ordinary concrete
declaration (`finishInstantiation()`; the Resolver side is
`resolveMonomorphizedClone()`, which resolves the clone as if it were
written inside its template's module). The clone is appended to `PrePassASTs`
and registered, so sub-passes 2c, 3a and 3b codegen it like any other
declaration.

A loop rather than one sweep is needed because re-checking one clone can
discover new requests (a nested generic call, a generic field), and because
the four kinds feed each other: a method clone can call a generic free
function, and a free function can construct a generic struct. The template's
own AST node stays in the tree and is skipped by sub-passes 3a and 3b.

`instantiatedDeclName()` is a pure function of base name and type arguments,
inserting a `_gen<N>_` marker right after the base: `identity` with `[int]` is
`identity_gen1_int`; the struct `list::List` with `[int]` is
`list::List_gen1_int` (its LLVM name `list.List_gen1_int`); a method clone is
`list::List.push_gen1_int`. The driver uses the `_gen<N>_` marker to find a
struct instantiation whose request was recorded but never drained.

The result is independent code and layout per instantiation. `List<int>` and
`List<string>` are two `llvm::StructType`s with two sets of methods;
`GlobalSymbolTable` has one registered `TurfStructType` per instantiation,
keyed by the clone's identity, never one shared `List` entry. See
`docs/architecture/memory_model.md` section 2.7 for the layout consequences.

### Instantiation chains and the depth cap

Every `InstantiationRequest` records its `Parent`: the cache index of the
instantiation whose body was being re-checked when this one was found, or `-1`
for ordinary code. `finishInstantiation()` sets it around the nested
`TypeChecker` with `InstantiationCollector::setCurrentParent()`, as a
set/restore pair rather than an RAII guard because the nested checker reports
by exception and the restore must be explicit. Following `Parent` to `-1`
(`InstantiationCollector::chainTo()`) gives the real chain, for instance user
call, `Set<Big>.add`, `Set<Big>.contains`.

- *Where an instantiation error points.* An error found while re-checking a
  clone is anchored at the outermost user-code request in its chain
  (`anchorIndex()`: the first hop whose site is not under `lib/std`). It
  carries one note per hop (`'Set<Big>.add' requires 'Set<Big>.contains'
  here`) and one for the error itself, so an unsupported type argument is
  reported on the user's call and not inside `set.tr`.
- *The cap.* A generic that requests ever-larger instances of itself is
  rejected with E079 (`GenericInstantiationDepthError`) once a chain is longer
  than `Monomorphize::kMaxInstantiationDepth` (64), printing the chain with long
  names clipped. A generic that fans out (`f<T>` requesting both `f<Box<T>>`
  and `f<Pair<T>>`) reaches depth 64 only after enumerating exponentially many
  instances, so `kMaxTotalInstantiations` (4096) backstops it through the same
  error. Calling one generic function with any number of different types is
  fine: the limit is chain length, not the count.

### Why not type erasure

Erasing every parameter to one opaque 64-bit slot cannot represent a
struct-typed `T` wider than a machine word, cannot dispatch `operator==` per
`T` (an equality check on a generic key would compare raw bits), and gives
`sizeof(T)` no answer. A precise garbage collector (F-MEM-1) also needs every
field to have a distinct, statically known type, which erasure cannot give;
monomorphization is its prerequisite (`FREEZE.md`, section 6.5). The cost is
compile time and code size proportional to the number of distinct
instantiations used (templates themselves are never emitted).

### One type walker

Substituting a type parameter and detecting an open one are needed by every
pass in this section. `src/sema/TypeWalk.h` and `TypeWalk.cpp` hold the one
implementation:

- `containsOpenParam()`, parameterized by how far to look (a bare check;
  through pointer, array and reference wrapping; or also through a
  `GenericType`'s arguments);
- `substitute()`, a name-to-type map walk that serves both the declaration
  direction and the instantiation direction;
- `unify()`, the call-site binder that recovers type arguments from argument
  types;
- `collectParamOrder()` and `collectOrderedFieldSubstitution()` for the two
  shapes `unify()` does not fit.

The Resolver, TypeChecker and Monomorphizer call these and keep no private
walker. `scripts/check/check_boundary.sh` enforces that a `dynamic_cast` to
`TurfGenericParamType` appears only in `TypeWalk`, in an INV-GEN-1 assertion,
or in the one documented exception in `CodegenCore.cpp`. Codegen's
`getExprType()` is a separate engine and reimplements the same pattern in a few
places; it is not unified with `TypeWalk`.

### Generic operators

An `operator<T> ...` declaration is a `FuncDefExprAST` with non-empty
`TypeParams`, so it is a template like any `fn<T>` (`FREEZE.md`, section
6.19). The difference is the requester: an operator node
(`BinaryExprAST`, `UnaryExprAST`, `CompoundAssignmentExprAST`,
`PostfixExprAST`, `ArrayAccessExprAST`), not a `FuncCallExprAST`.

- *Matching.* `TypeChecker::matchOperatorOverload()` (`TypeCheckerCall.cpp`) is
  the one place a candidate meets concrete operands, for every operator
  resolution site. For a generic candidate it `unify()`s each declared operand
  with its operand, requires every type parameter bound, substitutes, then
  requires `sameTypeExact()` per operand. The last step matters: `unify()`
  skips a structural mismatch silently, so without it `Cell<int> == Cell<string>`
  would match `operator<T> ==(a: Cell<T>, b: Cell<T>)` with `T = int`.
- *Request identity.* A request carries `Decl`, the exact template
  (`ResolverFunctionSymbol::Decl`, set in `buildFunctionSymbol()`).
  `(SourceModule, DeclKey)` alone cannot separate overloads, and every operator
  spelling is overloaded. `drainFreeFunctionRequests()` instantiates `Decl`
  directly and falls back to the name registry only without one; the method and
  constructor drains do the same, so overloaded generic methods
  (`Box.put(x)` and `Box.put(x, y)`) are separate templates.
- *Rewriting sites.* `retargetOperatorSite()` writes the clone into the node's
  own target field. `DeclKey` separates the two things one node can need: `a[i]`
  is the site of both an `operator[]` and an `operator[]=` request, and a
  user-written `!x` keeps its `operator!` target apart from its `!=` one.
- *Open parameters are not requested.* Checking the template body itself sees
  `List<T>`; only a fully concrete tuple is recorded, the same gate
  `visitFuncCall()` uses. The clone's own re-check, at a concrete type, finds
  the nested operator use.
- *Codegen.* An `operator[]` read returns its element by value, so
  `ArrayAccessExprAST::codegenLValue()` answers "no address", and
  `FieldAccessExprAST`/`MethodCallExprAST` spill the r-value, which is what
  `list[i].field` needs. That spill is sound for a read only: writing into the
  copy would lose the write. The TypeChecker therefore rejects every write into
  it (E098, `operatorIndexTemporaryRoot()` and `rejectOperatorTemporaryWrite()`
  in `TypeCheckerExpr.cpp`), from assignment, compound assignment, `++`/`--`, a
  call to a method that mutates `self` (directly or through `self.m()` calls,
  `ResolverSymbolTable::mutatesSelfTransitively()`, whose call graph the
  Resolver records during pass 2), and a by-reference `[]=`; the check stops at a
  pointer or reference base. The Codegen diagnostic in
  `AssignmentExprAST::codegen()` for the nested `[]=` shape is an unreachable
  backstop. A new write path that calls `codegenLValue()` on an index result
  must go through that check.

### Size parameters

`fn<T, N: size> fill(value: T): T[N]` is a template whose `N` is an array
length, not a type. It is monomorphized like a type parameter, with a number
where a type would be. Which array dimensions mention `N` and what value `N`
takes is decided per pass:

- *Parser.* `ParseFuncDef()` (`ParserDecl.cpp`) reads `N: size` into
  `FuncDefExprAST::SizeParams`, a subset of `TypeParams` in declaration order
  (`getTypeOnlyParams()` is the rest). Anything but `size` after the colon, and
  a size parameter on a method or an operator, is E150 here. While the
  function's signature and body are parsed, its names are in
  `g_SizeParamsInScope`, so `TakeSizeParamDim()` (`ParserCore.cpp`) turns an
  identifier inside `[ ]` that is one of them into a symbolic dimension, and
  `WrapArrayDim()` builds the `ArrayType` from it. Any other name there is E123.
- *Types.* A symbolic dimension is an `ArrayType` with
  `Size == ArrayType::kSymbolicSize` (-2) and the name in `getSizeParam()`;
  `hasSymbolicSize()` tells it from a real length. A fixed length is positive
  and an unsized `T[]` is -1, so code that tests `getSize() > 0` or `< 0` never
  sees a symbolic one as either. `TurfSizeValueType` carries a bound length (its
  mangled name is `n<value>`). It is never the type of a value; it travels where
  a type argument does, in a substitution map and in an instantiation's
  `TypeArgs`.
- *TypeWalk.* `unify()` binds `N` when it meets a symbolic dimension against an
  array with a real length, and binds nothing against an unsized `T[]`.
  `substitute()` replaces a symbolic dimension with the bound value, and
  `collectParamOrder()` lists `N` after the element type's parameters.
- *Resolver.* `validateGenericParamsInferable()` (`ResolverCore.cpp`) lets a size
  parameter be inferred from the return type as well as the parameters, and
  reports E150 for `N` spelled as a type or sharing a parameter's name, and E129
  for one no array uses. The function's scope declares each size parameter as an
  immutable `int` (`VariableSymbol::IsSizeParam`), so the body's reads of `N`
  resolve, and `visitVariable()` marks them (`VariableExprAST::IsSizeParamRef`).
- *TypeChecker.* A template with a size parameter has its body skipped
  (`visitFuncDef()`); each instantiation's clone is checked with real lengths, so
  a body error is reported at the call that needs it, under the usual "In this
  instantiation" notes. `visitFuncCall()` unifies the arguments first. A size
  parameter still unbound is taken from the type the result goes into:
  `visitWithExpectedType()` records the call being checked and the type
  expected of it (`ExpectedCallSite_`, `ExpectedCallType_`), and `visitFuncCall()`
  unifies the return type with it. The callers that supply an expected type are
  `visitVarDecl()`, `visitArrayDecl()` for its initializer, assignment, a call
  argument against a non-generic parameter, and `return`. Still unbound is E129.
  The request's `TypeArgs` follow the declaration's `TypeParams` order for a
  size-generic function, since the return type can bind one the parameters do not.
  Writing to `N` (assignment, `++`/`--`, `&`) is E150, found when the clone is
  checked, where the read carries its value (`rejectSizeParamPlace()`).
- *Monomorphizer.* `substituteTypesInBody()` sets `VariableExprAST::SizeValue`
  on each marked read and `ArrayDeclExprAST`'s size from its `SizeParam`, then
  substitutes types as for any clone. The instantiation's name is mangled with
  `n<value>`, so `fill` at 3 and at 4 are two functions.
- *Codegen.* `VariableExprAST::codegen()` emits `SizeValue` as an `i64`
  constant. Arrays have their real sizes by then, so nothing else changes.

A size-generic function calling another with its own `N`
(`reversed(reversed(a))`) works because the clone is checked at a real length:
by the time the inner call is checked, its argument is an `int[3]`.

---

## 8. Running the front end from a tool: the query facade

Everything above runs inside the driver, once per process, on state held in
process-wide globals: the lexer cursor, `GlobalSymbolTable`, the diagnostic
list, the import caches, the recovery flag. Anything that runs the front end a
second time in one process (an editor re-checking a buffer on every keystroke,
the SLM Sandbox validating a fix while the compile that raised the diagnostic
is still in flight) must set that state up fresh and put the originals back.

`include/turf/Analysis.h` is the one way to do that.
`turf::analyze(buffers, options)` parses the first buffer as the entry file
(the other buffers stand in for the on-disk text of any module an import
loads), runs the Resolver and TypeChecker with per-declaration recovery, never
runs Codegen or the Monomorphizer, and returns an `AnalysisResult`. The result
owns the AST, the Resolver, the TypeChecker's inferred types and the
diagnostics, and answers `typeAt`, `definitionOf` and `symbolsInScope` from a
position index built while the Resolver walked the program (through its
scope-snapshot callback). `turf::resolveModuleFile()` answers which file an
`import` would load. `AnalysisOptions` selects the optional extras:
`RunLint`, `RunFlowAnalysis` (the section 6 passes, per non-generic function
without errors, via `runFunctionFlowPasses()`), `SkipPrelude`, `InstallRoot`,
`PackageGraphFile`, `ShouldCancel`, `AllowUnsafeModules`,
`IncludeStdlibWarnings`. The declarations in the header are the semver-stable
2.x tool API, except the members grouped under "Internal representation".
Analyses are serialized by a mutex, and an `AnalysisResult` needs no global
state after `analyze()` returns.

The save and restore is `GlobalStateScope` in `src/tooling/AnalysisFacade.cpp`,
composed from one hook per subsystem that keeps state across calls:
`PushParserCompileState()`/`PopParserCompileState()` (`Parser.h`),
`PushResolverCompileState()`/`PopResolverCompileState()` (`Resolver.h`) and
`DiagnosticEngine::saveState()`/`restoreState()` (`Lexer.h`). Those hooks have
no other caller (`scripts/check/check_boundary.sh`, check 7, INV-API-1). A
second hand-written save/restore is what kept breaking before. Code that has
to drive the lexer on text outside a compile, such as the formatter, uses
`turf::detail::runIsolated()` (`include/turf/FrontEndIsolation.h`), which sits
on the same scope.

Two consequences when you add anything to the front end:

- **New per-compile state needs a home in one of those hooks.** A global, or a
  function-local `static`, that one compile fills and a later compile reads
  leaks between analyses. `tests/api/analysis_test.cpp` (built as
  `build/turf-api-tests`) runs different programs back to back and compares
  against clean runs.
- **Diagnostics from Codegen and the Monomorphizer are not in an analysis.**
  The CFG passes of section 6 run only if `RunFlowAnalysis` is set, and the
  instantiation-time errors of section 7 never. A tool therefore sees fewer
  diagnostics than `turfc` prints for the same file. `RunLint` adds the linter,
  the one Codegen-time check that does not need Codegen.

---

## File map

Headers sit beside their sources, one directory per subsystem; only the query
facade is public (`include/turf/`), and an include is written with its
directory (`#include "frontend/Parser.h"`).

| Stage | Primary files |
|---|---|
| Lexer | `src/frontend/Lexer.h`, `src/frontend/Lexer.cpp` |
| Parser | `src/frontend/Parser.h`, `ParserInternal.h`, `ParserCore.cpp`, `ParserDecl.cpp`, `ParserStmt.cpp`, `ParserLiteral.cpp`, `ParserModule.cpp`; AST in `AST.h`, `ASTExpr.h`, `ASTStmt.h`, `ASTDecl.h`, `ASTAccess.h`; visitors in `ASTVisitor.h`, `ASTChildWalker.h`, `ASTPrinter.h` (all under `src/frontend/`) |
| Resolver | `src/resolve/Resolver.h`; `ResolverCore.cpp`, `ResolverTypeIdentity.cpp`, `ResolverDecl.cpp`, `ResolverExpr.cpp`, `ResolverStmt.cpp`, `ResolverCall.cpp`, `ResolverVisibility.cpp`; tables in `ResolverSymbolTable.h`, `ResolverScopeTable.h`, `ResolverModuleTable.h`, `ResolverTypeRegistry.h`, `ResolverSymbolBase.h` |
| TypeChecker | `src/sema/TypeChecker.h`; `TypeCheckerCore.cpp`, `TypeCheckerCall.cpp`, `TypeCheckerDecl.cpp`, `TypeCheckerExpr.cpp`, `TypeCheckerStmt.cpp`, `TypeCheckerPattern.cpp`, `TypeCheckerSlice.cpp`, `TypeCheckerVisibility.cpp` |
| Type system | `src/sema/Types.h`, `Types.cpp` |
| Monomorphizer, type walker | `src/sema/Monomorphize.h`, `Monomorphize.cpp`; `src/sema/TypeWalk.h`, `TypeWalk.cpp` |
| Codegen | `src/codegen/Codegen.h`, `CodegenInternal.h`, `CodegenCore.cpp`, `CodegenFunc.cpp`, `CodegenExpr.cpp`, `CodegenStruct.cpp`, `CodegenVar.cpp`, `CodegenArray.cpp`, `CodegenControlFlow.cpp`, `CodegenPattern.cpp` |
| CFG subsystem | `src/analysis/CFG.h`, `CFG.cpp`; `Analysis.h`, `Analysis.cpp`; `AliasAnalysis.cpp`; `ValueRange.h`, `ValueRange.cpp`; `Lint.h`, `Lint.cpp`; `Algorithms.h` |
| Diagnostics | `src/diagnostics/Errors.h` and the `Errors*.h` family, `ErrorCodes.h`, `ErrorCategory.h` |
| Driver | `src/driver/main.cpp` |
| Query facade | `include/turf/Analysis.h`, `include/turf/FrontEndIsolation.h`, `src/tooling/AnalysisFacade.cpp`; tests in `tests/api/analysis_test.cpp` |
