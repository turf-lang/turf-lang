# TurfLang Compiler & Language Architecture Overhaul

> **Status:** architectural blueprint. Nothing in this document has been
> implemented. It is written to be consumed one feature group at a time by
> independent implementation sessions.
>
> **Written:** 2026-09-17, against commit `74e9225` (branch
> `book/turflang-textbook`), verified live against a `--release` build of
> the compiler at that commit (LLVM 22.1.8, x86_64 Linux).
>
> **How to use this document:** §8 and §22 are the entry points. Take one
> feature group, read §6 (the freeze) first, then that group's section, then
> the cross-references it names. Do not re-derive decisions §6 has already
> frozen.

---

## 0. Executive Summary

### 0.1 What TurfLang is today

TurfLang is a genuinely working, AOT, LLVM-backed, statically-typed language
with ~36,900 lines of C++ across `src/` and `include/`, ~4,200 lines of
standard library written in TurfLang itself, and 646 regression tests across
35 topic folders. It compiles a hello-world in ~0.3 s, produces native
binaries, links against C libraries, and has been used to write a working
HTTP server, an HTTP client, a JSON parser, and an LLVM-C FFI demo — all in
`showcase/`.

It is **not** a toy in the areas most people would check first:

- **Diagnostics.** 68 numbered error codes, `turf --explain EXXX`, per-code
  prose docs in `docs/errors/codes/`, hint strings decoupled from messages,
  Damerau–Levenshtein "did you mean?" across symbols/keywords/fields/modules,
  source spans, one-error-per-line noise capping, and a local-SLM-assisted
  `--smart` mode that *sandbox-compiles its own suggestions before showing
  them*.
- **Static analysis.** A real per-function CFG with ~10 analyses: forward
  dataflow definite-assignment, backward dataflow liveness (dead stores,
  unused variables), Tarjan SCC for unbreakable loops, a points-to analysis
  for use-after-free/double-free, constant propagation with compile-time
  array-bounds checking, cyclomatic complexity, infinite-recursion DFS, and
  constructor definite-field-assignment. None of this is gated behind a flag.
- **Module system.** A resolved `::` scope operator, on-demand imports,
  module-owned types, visibility gating, a prelude, and a documented,
  path-independent module index.
- **Documentation.** `docs/architecture/` is unusually good — five deep
  internals references with file:line citations that are, as far as this
  audit could check, accurate. The in-progress handbook (`book/`, 8,102
  lines of LaTeX across 12 chapters) is better still.

### 0.2 What is genuinely missing or broken

The gaps are not spread thinly; they cluster in **five** places, and four of
them are the same root cause wearing different clothes.

1. **There is no runtime.** The linker line is literally
   `cc out.o -o out -lm` plus whatever `extern from "lib"` declared. There is
   no `libturfrt`. Nothing exists to own allocation, collection, panics,
   type descriptors, or process bootstrap. Every "runtime service" in
   TurfLang today is either inlined into user IR or is a raw libc call made
   from a `.tr` file.

2. **There is no memory management, and no path to one.** Everything heap
   allocated leaks — `strings.tr`'s own `operator +` says so in a comment
   (`TURF-MEMLEAK`). `List<T>`/`Map<K,V>`/`Set<T>` never free their backing
   buffers and expose no method that could. There are no destructors, no
   scope hooks, no `defer`, no reference counts.

3. **Generics are type-erased, and the erasure is unsound above 8 bytes.**
   Every `T` compiles to one pointer-sized slot; scalars are stuffed into it
   with `IntToPtr`. A struct larger than one machine word has no valid
   encoding — and the failure mode is not a diagnostic:

   ```
   Internal compiler error: generated LLVM IR is invalid.
   Invalid bitcast
     %argcast_bit = bitcast %Big %x4 to ptr
   ```
   (verified live: a 24-byte struct pushed into a `List<T>`.)

   The same erasure is why `Map<string, V>` compares keys by pointer
   identity instead of content (`docs/planning/LIMITATIONS.md` §1), why a
   generic `sort`/`comparator` is impossible (§2), and why `sizeof(T)` does
   not exist inside a generic body.

4. **Numeric semantics are undefined in practice.** Verified live, all with
   **zero diagnostics**:
   - `mut x: int = 999999999999999999999999999999999999999999999999;` →
     `9223372036854775807` (silent `strtoll` clamp, `errno` never checked).
   - A `1` followed by 400 zeros as a `double` → `inf`.
   - `9223372036854775807 + 1` → `-9223372036854775808`.
   - `-9223372036854775808` cannot be written at all (it clamps to
     `-9223372036854775807`).
   - There is no exponent (`1e6`), hex, binary, octal, or digit-separator
     literal syntax.

5. **Two confirmed compiler crashes on plausible input.** `struct A { me: A }`
   compiles (an infinitely-sized type); instantiating it **segfaults**
   (exit 139, no output). So does the mutually-recursive
   `struct A { b: B } struct B { a: A }`.

Alongside those, three narrower but consequential gaps:

- **User code cannot declare a pointer-based data structure.** A
  `struct Node { next: Node* }` must be wrapped in a top-level `unsafe { }`
  block, and `fn walk(n: Node*)` is rejected **even inside `unsafe`** —
  only files under `lib/std`, matched *by file path*
  (`TypeChecker::isUnsafeGateExempt` → `IsPathUnderLibStd`), may take or
  return a raw pointer. There is no safe nullable reference. A linked list
  is, today, effectively a standard-library-only privilege.
- **Optimization is five function passes on the legacy pass manager**, with
  no module pass manager, no SROA, no inliner, no loop passes, no
  dead-function elimination, and no `-O` flag of any kind. Measured: an
  empty `fn main(): int { return 0; }` links a 29,968-byte binary carrying
  **84 emitted, entirely unused `turf_*` functions** (plus 2 unused data symbols) from the auto-imported
  prelude.
- **No debug information.** `DIBuilder` appears nowhere in the tree. There
  is no `-g`. Generated binaries cannot be meaningfully single-stepped.

And a set of smaller, individually cheap defects that are each scheduled
rather than waved off — a mandatory `step` clause on every range `for`, two
CFG warnings that bypass `DiagnosticEngine` entirely despite having
purpose-written classes, a C callback being unwritable in user code, and one
LLVM header costing 65% of every parser translation unit. These live in
**HYG-001**, **LANG-004** and **LANG-002**. Everything identified but
deliberately *not* scheduled is catalogued in **§27, the Deferred Work
Register**, which is the intended feed for `docs/planning/FUTURE.md` — nothing
found by this audit is left without a home.

### 0.3 The through-line

The four "big" problems are not independent. Read in dependency order they
form a single chain:

> **Precise garbage collection requires knowing, at runtime, which words of
> an object are pointers. Type erasure destroys exactly that information —
> and, worse, actively manufactures integers that look like pointers
> (`IntToPtr` bit-stuffing), which would corrupt any collector's root set.
> Therefore monomorphization is not an optimization here; it is the
> precondition for the memory model.**

That single observation orders most of this roadmap. It also means the
generic-representation decision (§9) is the true foundation stone, and
everything in §14 (GC) is downstream of it.

The numeric work (§10) and the `guess` work (§11) form a second, smaller
chain of their own: you cannot decide what `mut x = 999999999999999999999;`
means until you have decided both how a literal is represented and how
inference picks a type.

### 0.4 What this overhaul is trying to accomplish

Not "add features". Specifically:

1. **Make the compiler incapable of the failure modes found above** —
   structurally, not by adding a check per bug. An infinitely-sized struct
   should be unrepresentable, not caught; a non-representable literal should
   be a typed error, not a `strtoll` clamp; an over-8-byte generic argument
   should be an ordinary instantiation, not an ICE.
2. **Give TurfLang a runtime**, and with it a real answer to "who frees
   this?".
3. **Draw, name, and enforce the Language / Compiler / Runtime / Standard
   Library / Tooling boundary**, replacing the current path-based
   `lib/std` privilege with something declared and auditable.
4. **Freeze the semantics that multiple feature sessions would otherwise
   each decide differently.**

### 0.5 What "complete" means for TurfLang

Deliberately narrow. TurfLang is complete when a competent programmer can,
*without reaching for `unsafe` and without hitting a compiler bug*:

- write a linked list, a tree, and a graph;
- use a generic collection with any element type, including large structs;
- rely on arithmetic either producing the right answer or stopping;
- not have to think about freeing memory in ordinary code;
- get a source-accurate diagnostic from standard-library code, at their own
  call site;
- debug the resulting binary;
- and read a signature and know exactly what it does.

That list is the acceptance criterion for this whole milestone. Nothing
outside it is in scope.

### 0.6 What should explicitly stay small

- **No traits, no typeclasses, no interfaces, no inheritance, no dynamic
  dispatch.** `docs/planning/LIMITATIONS.md` proposes a trait system as the
  fix for the generic-equality gap. Monomorphization fixes that gap without
  adding any language surface at all. Do not add both.
- **No exceptions.** `error` types + `!T` + `recover` already cover it.
- **No macro system, no user-facing `comptime`.**
- **No second integer-width family** (`i8`/`u32`/...). `int` (64-bit) and
  `int32` (an explicit, documented FFI-ABI type) are enough.
- **`string` stays a NUL-terminated byte pointer.** It must remain directly
  passable to C: the standard library has **74 `extern` declarations**
  (§15.7), and `string` is the parameter type for most of the `io`, `fs`,
  `os`, `net` and `strings` ones. A fat `{ptr,len,cap}` string would require
  a conversion at every one of those call sites for a benefit (`O(1)` length)
  that a stdlib `StringBuilder`/`Bytes` type can deliver instead.
- **Single-threaded.** No concurrency in this milestone. (§14.11 states what
  the GC design would have to change if that ever stops being true.)

---

## 1. Current Architecture Audit

Every claim in this section is either cited to a file or was verified by
compiling a program with the build at `74e9225`. Where a claim came only
from reading, it is marked *(read-only)*.

### 1.1 Pipeline

`Lexer → Parser → Resolver (2 passes) → TypeChecker → Codegen`, driven by
`runCompilerMain()` in `src/main.cpp`. The pass ordering is a hard
constraint, not a description: `main.cpp` returns before constructing the
`TypeChecker` if `Resolver::resolveAll()` failed, and returns before any
`codegen()` call if `TypeChecker::checkAll()` failed. Codegen is written to
*trust* this and re-validates nothing.

**The double-parse wrinkle.** `main.cpp` runs a throwaway pre-pass
(`PrePassASTs`, its own discarded `Resolver`/`TypeChecker`) that codegens
every struct, then every function prototype (`g_protoOnlyPass`), then every
function body — then **closes, resets and re-parses the file from scratch**
for the authoritative pass, which re-runs Resolver/TypeChecker and codegens
only non-function top-level statements. The pre-pass exists so struct layouts
and function prototypes are complete before any GEP or call references them.

This is the single largest structural oddity in the compiler. It means:

- Every source file is lexed and parsed **twice** per compile.
- Two complete ASTs exist per compile, and only the second is authoritative.
- Pre-pass diagnostics are silently discarded (`PreTCClean`), relying on the
  main pass to re-raise the identical error a few hundred lines later.
- Any pass ordering bug shows up as an LLVM verifier failure rather than a
  TurfLang diagnostic.

It is *not* on this roadmap to remove — but several feature groups touch it,
and §25 says plainly not to "fix" it opportunistically.

### 1.2 Lexer — `include/Lexer.h`, `src/Lexer.cpp`

Hand-written maximal-munch scanner, no generator. Keywords in a table;
builtin/stdlib names deliberately **not** keywords, so the lexer has zero
knowledge of the standard library. `SourceLocation` carries
`Line/Col/Filename/EndLine/EndCol`; `Filename` is repointed per imported
module and cached in `AllSourceLines` so a later pass can still render a
snippet from an imported file.

Diagnostics are collected, not thrown: `TurfError::raise()` records then
`longjmp()`s to the nearest `setjmp()` recovery point (LLVM is commonly
built with exceptions off). `DiagnosticEngine` is the single choke point and
caps at one primary error per line.

**Number scanning is the weak point** (`src/Lexer.cpp:753-772`):

```cpp
if (isdigit(LastChar) || (LastChar == '.' && isdigit(SourceFile->peek()))) {
  std::string NumStr;  bool SeenDot = false;
  do { ... } while (isdigit(LastChar) || (!SeenDot && LastChar == '.' && ...));
  bool IsFloat = NumStr.find('.') != std::string::npos;
  if (IsFloat) { NumVal = strtod(NumStr.c_str(), 0); return TOK_NUMBER; }
  IntVal = strtoll(NumStr.c_str(), nullptr, 10);
  return TOK_INT_LITERAL;
}
```

`errno` is never consulted after either call. `strtoll` clamps silently to
`LLONG_MAX`/`LLONG_MIN` on overflow; `strtod` silently returns `±HUGE_VAL`.
This one function is the origin of every finding in §10.

### 1.3 Parser — `src/ParserCore/Decl/Stmt/Literal/Module.cpp`

Recursive descent; expressions by precedence climbing over a single
`BinopPrecedence` map. `ParseType()` recurses into itself directly and
carries its own `ParseRecursionGuard` (depth 2000) because a nested generic
never re-enters `ParseExpression()`.

Two structural notes that matter downstream:

- **The parser touches `GlobalSymbolTable`** — the *LLVM-aware*, Codegen-side
  symbol table — 28 times across `ParserCore.cpp`/`ParserDecl.cpp`. Audited
  site by site, this is **much less severe than the raw count suggests**, and
  the distinction matters for §3's boundary work:

  | Use | Sites | Load-bearing for parsing? |
  |---|---|---|
  | `RegisterType()` for struct / enum / machine / error, so *Codegen* can `LookupType()` later | `ParserDecl.cpp:194, 272, 389, 503` | **No** — a pure producer-side write for a later consumer |
  | `RegisterType()` / `EnterScope()` / `ExitScope()` for generic type parameters | `ParserDecl.cpp:42, 45, 98-100, 788-790` | **No** — `ParseType()` never reads it (see below) |
  | `LookupType()` to give a better error on the removed `Foo { … }` brace-init syntax | `ParserCore.cpp:583-584, 620-621` | **No** — the code's own comment says *"Best-effort, not authoritative"* |
  | `LookupSymbol()` to suppress a keyword-typo suggestion on a known variable | `ParserCore.cpp:662-663` | **No** — diagnostic quality only |

  **`ParseType()` contains zero `GlobalSymbolTable` references** — verified by
  direct grep. It always emits a `NamedType(BaseName)` placeholder and lets
  the Resolver bind it. `include/Resolver.h:22-25` documents this as a
  deliberate past cleanup: *"This two-pass design is why the compiler no
  longer needs `GlobalSymbolTable->LookupType()` in `ParseType()`."*

  So TurfLang's parser is **not** doing a C-style "lexer hack": no parsing
  decision anywhere depends on semantic state. What remains is (a) four
  registration writes that exist purely to hand data forward to Codegen, and
  (b) three best-effort diagnostic lookups. Both are ordinary layering
  shortcuts, not a context-sensitive grammar.

  The one genuine cost is **build time, not compile time**: `SymbolTable.h`
  includes `llvm/IR/Instructions.h` for a single `llvm::AllocaInst *Alloca`
  field that is only ever used as a pointer. Measured: `src/ParserDecl.cpp`
  preprocesses to **182,910 lines**, of which `llvm/IR/Instructions.h` alone
  contributes **119,047** (~65%), and the TU takes **3.07 s** to syntax-check
  at `-O2`. Forward-declaring `namespace llvm { class AllocaInst; }` would
  remove that entirely. See §3.4.
- **`ParseLetStatement()` is the only variable-declaration entry point**, and
  it requires `TOK_COLON` immediately after the name. There is no inference
  path anywhere.

### 1.4 AST — `include/AST.h` + `ASTExpr/ASTStmt/ASTDecl/ASTAccess.h`

~42 concrete node kinds, all deriving from `ExprAST` (statements and
declarations included, despite the `...ExprAST` naming). Every node
implements `accept(ASTVisitor&)` **and** `codegen()` — the Resolver and
TypeChecker use the visitor, Codegen uses direct virtual dispatch, because
Codegen predates `ASTVisitor`.

Every node also implements `clone()` (`ASTDiff.h`'s `CloneExprAST`), built
for the SLM sandbox's speculative-edit machinery. **This is the single most
important pre-existing asset for monomorphization** (§9) — the ability to
deep-copy an arbitrary declaration subtree already exists and is already
exercised.

Nodes are deliberately under-specified where the parser cannot know better
(`ScopeAccessExprAST::ResolvedKind = Unresolved` until the Resolver decides
whether `Left` is an enum, a module, or a struct).

### 1.5 Type system — `include/Types.h`, `src/Types.cpp`

An LLVM-free hierarchy rooted at `TurfType`: `IntType`, `Int32Type`,
`DoubleType`, `BoolType`, `CharType`, `StringType`, `VoidType`,
`TurfNullType`, `OpaquePointerType` (`ptr`), `NamedType`, `ArrayType`,
`TurfPointerType`, `TurfReferenceType` (`&T`, a *subclass* of
`TurfPointerType` so every existing `dynamic_cast<TurfPointerType*>` in
Codegen keeps working), `TurfGenericParamType`, `GenericType`,
`TurfStructType`, `TurfEnumType`, `TurfMachineType` (subclasses
`TurfEnumType`), `TurfUserErrorType`, `TurfErrorUnionType`, `AnyErrorType`,
`FuncType`.

Primitives are process-lifetime singletons (`TurfInt()`, …), so pointer
identity works.

`typesCompatible(From, To)` (`src/Types.cpp`) is the compatibility oracle for
the whole frontend. Its rule order, as written:

1. **Either side is `TurfGenericParamType` → `true`.** Checked first, before
   everything, so an open type parameter "matches anything".
2. Array rule: both must be arrays; sizes equal or one is `-1`; element types
   `isEquivalent()` (with the same generic-param bypass one level deeper).
3. Pointer-to-generic-param bypass.
4. `GenericType` leniency: bare `Box` ≡ `Box<Anything>`; two `GenericType`s
   of the same base compare per type argument with `isEquivalent()` (not
   recursive `typesCompatible()` — deliberately no numeric widening between
   type *arguments*).
5. Exact `getName()` match.
6. `null` ↔ pointer-like (excluding `&T`).
7. `ptr`/`void*` ↔ typed pointer (`&T` excluded as a target).
8. `&T` → `T*` widening (never the reverse).
9. `void`/`string`/enum/`bool` isolation.
10. Numeric widening: `int → double` yes; `double → int` no; `int32 → int`,
    `int32 → double` yes; `→ int32` no.

Rules 1 and 4 are the type-erasure model made explicit. Rule 1 in particular
is why a generic body type-checks once, generically — and therefore why no
`operator==(string,string)` candidate can ever match a `K` inside
`Map<K,V>`.

**Live contradiction inside this one header:** `TurfGenericParamType`'s doc
comment says parameters are *"replaced with concrete types during
monomorphization"* and `GenericType`'s says it is *"the core of
monomorphization"*, while `typesCompatible()`'s own comments in
`src/Types.cpp` say *"Under this compiler's type-erasure model…"*.
`docs/architecture/compiler_pipeline.md` §7 already flags this as a real
mismatch rather than repeating it.

### 1.6 Resolver — `src/ResolverCore/Decl/Expr/Stmt/Call.cpp`

Two passes over the same node list. **Pass 1 (hoisting)** walks only
top-level declarations — never a function body — and registers every
function signature, struct, enum, error and machine name. It recurses
through imports (`hoistModuleBody()` per `ModuleImportExprAST`). **Pass 2**
walks everything and resolves every name reference, writing results directly
onto AST node fields (`ResolvedCallTarget`, `ResolvedKind`,
`ResolvedSymbol`, `ResolvedOperatorCandidates`).

**Pass 1 never entering a body is a load-bearing invariant.** It is what
makes mutual recursion and forward references work, and it is the reason
return-type inference is architecturally excluded in §11.

`substituteTypeParams()` (`src/ResolverCore.cpp:131-182`) rewrites a
`NamedType("T")` into a real `TurfGenericParamType("T")` during hoisting,
recursing through pointer/reference/array/generic-argument wrapping. It
mutates the AST in place (`ResolverDecl.cpp:108-111`, `479-482`) so every
downstream consumer sees the substituted node.

Symbol tables: `ResolverSymbolTable` is a composite over `ResolverScopeTable`
(lexical variables), `ResolverModuleTable` (module/current-file function and
constant registries, flat imports), and `ResolverTypeRegistry` (struct/enum/
error names + owning module). It is entirely LLVM-free.

### 1.7 TypeChecker — `include/TypeChecker.h`, `src/TypeChecker*.cpp`

An `ASTVisitor` subclass. Inferred types go into a **side table**,
`std::unordered_map<const ExprAST*, TurfTypePtr> NodeTypes_`, keyed by node
pointer. A few values Codegen cannot re-derive are written onto nodes instead
(`RecoverExprAST::ResolvedSuccessType`, `BinaryExprAST::ResolvedOperatorTarget`).

Three facts about this pass matter a great deal:

1. **`NodeTypes_` is never read by Codegen.** Measured: `grep -c NodeTypes_
   src/Codegen*.cpp` → **0**. `getExprType()` (`src/CodegenCore.cpp:499-…`),
   a second, structurally independent implementation of "what type is this
   expression", is called **38** times from Codegen. The authoritative type
   information the TypeChecker computes is discarded before lowering.
   `docs/architecture/modules_and_scope_resolution.md` §2.10 documents a real
   wrong-answer bug this duality caused.
2. **The TypeChecker is not LLVM-free either.** It touches the Codegen-side
   `GlobalSymbolTable` 44 times.
3. **`to`-casts are barely checked here.** Per
   `docs/reference/variables_and_types.md`: *"The type checker itself does
   essentially no legality checking on `to` — a cast expression always
   type-checks. All real enforcement of which conversions are legal happens
   later, during code generation."* Semantic legality lives in the backend.

The `unsafe` gate lives here (`TypeCheckerCore.cpp:138-200`):

```cpp
bool TypeChecker::isUnsafeGateExempt(const SourceLocation &Loc, const ExprAST *Node) const {
  if (UnsafeDepth_ > 0) return true;
  if (Node && WasDeclaredInsideTopLevelUnsafe(Node)) return true;
  return IsPathUnderLibStd(Loc.Filename);      // <-- the standard library's privilege
}
```

and `checkNoRawPointersInSignature()` returns early *only* for
`IsPathUnderLibStd(...)`. **The standard library's extra capability is a
filesystem path prefix.** This is the most consequential undeclared privilege
in the codebase (§3, §15, LANG-002).

### 1.8 Codegen — `src/Codegen{Core,Func,Expr,Struct,Var,Array,ControlFlow}.cpp`

Direct `codegen()` virtual dispatch. Function identity is a mangled name
encoding module and parameter types
(`turf_<module>_<name>_<paramty>_<paramty>…`), sanitized for operator names.
`LLVMTypeCache` maps a mangled type name → `llvm::Type*`.

Key lowering facts (all from `docs/architecture/memory_model.md`, spot-checked):

- Primitives → `i64`/`i32`/`double`/`i1`/`i8`; `string`/`ptr`/`T*`/`&T`/
  unsized `T[]` → one opaque pointer.
- `T[N]` → a real stack `llvm::ArrayType`, memset to zero at declaration;
  passed and returned **by value** (whole-aggregate load/store).
- `T[]` (unsized parameter) → the caller's raw `AllocaInst` is passed. No
  copy, no escape analysis, no lifetime check.
- Structs → real LLVM aggregates, fields embedded inline; built opaque-then-
  `setBody()` so self-referential *pointer* fields work; value semantics
  everywhere except `self`, which is always a raw `StructName*`.
- `::new`/`::default` always `alloca` on the **stack**, never `malloc`.
  `GetDefaultValue()` recursively zero-fills struct fields.
- Generic parameter → opaque pointer; scalars stuffed in with
  `CreateIntToPtr` (doubles first `bitcast` to `i64`), read back with
  `CreatePtrToInt`. No boxing, no per-instantiation code.
- A CFG is built and ~10 analyses run **inside** `FuncDefExprAST::codegen()`,
  per function, unconditionally.

### 1.9 Optimization and object emission — `src/main.cpp`

```cpp
legacy::FunctionPassManager FPM(TheModule.get());
FPM.add(createPromoteMemoryToRegisterPass());
FPM.add(createInstructionCombiningPass());
FPM.add(createReassociatePass());
FPM.add(createGVNPass());
FPM.add(createCFGSimplificationPass());
FPM.doInitialization();
for (auto &F : *TheModule) FPM.run(F);
```

That is the entire pipeline. Then `emitObjectFile()` builds a
`TargetMachine` with `CPU = "generic"`, `Features = ""`, default
`CodeGenOptLevel`, and `Reloc::PIC_`; then `linkObject()` execs
`cc obj -o out -lm [-l…]`.

Consequences, measured rather than inferred:

| Missing | Evidence |
|---|---|
| Module pass manager | `legacy::FunctionPassManager` only; cannot see across functions |
| Inlining | none — every call is a real call |
| SROA | absent; struct locals never get scalarized (value-semantics structs pay for it) |
| Dead function elimination | empty `main` → 29,968-byte binary, **84 unused `turf_*` functions** |
| Loop optimization / vectorization | none |
| `-O` levels | no flag exists |
| Debug info | `DIBuilder` appears nowhere |
| Cross-compilation | `CPU="generic"`, host triple only |

`book/chapters/ch08_optimizations.tex` already documents all of this
accurately and honestly — it is a good pre-existing description of the
starting point.

### 1.10 Runtime

**There is none.** No `libturfrt`, no `.c`/`.o` runtime sources, no linked
support library. Panics are emitted inline by `CodegenPanic` via
`fprintf(stderr,…)` + `abort()` (`src/Builtins.cpp` gets `stderr`,
`fprintf`, `abort`, `panicfmt`). Allocation is libc `malloc` reached through
`extern fn` in `lib/std/mem/mem.tr`. Bounds checks and division-by-zero
checks are emitted inline per access site.

### 1.11 Standard library — `lib/std/`

4,208 lines of TurfLang across 23 modules. Uniform shape: raw C/POSIX bound
by `extern fn`, ordinary Turf layered on top. Module subfolders are a
browsing convenience — `IndexModuleDir()` recursively indexes `lib/std/**`
once per compile, so `import list;` doesn't care where `list.tr` lives.

Its actual privileges over user code, as implemented:

1. **Exemption from the raw-pointer gate**, by file path (§1.7). This is
   load-bearing: `List<T>`'s `data: T*` field and every `mem`/`net`/`fs`
   binding depend on it.
2. **Prelude auto-import** — `int`, `double`, `bool`, `char`, `strings`,
   `array` are imported into every top-level file unconditionally by
   `ParsePreludeImports()` (`src/Parser*.cpp`), with a self-import guard so
   compiling a prelude module directly doesn't double-register it.
3. Nothing else. There are no intrinsics reserved to it, no privileged
   attributes, no separate compilation, no ABI.

There is no separate compilation at all: a module's `.tr` source is
re-parsed and re-codegenned into the consuming module on every compile, and
every function of every imported module is emitted whether used or not.

### 1.12 Diagnostics and SLM

`DiagnosticEngine` (`include/Lexer.h`) is the single sink. `TurfError`
subclasses (`include/Errors*.h`, split by domain: Lex/Type/Scope/Struct/
Module/Flow/Array/Memory) each carry a message, a hint, related locations, a
code, and an `SLM::ErrorCategory`.

`--smart` path (`src/SLMIntegration.cpp`, `src/SLMEngine.cpp`,
`src/Sandbox.cpp`):

1. Category resolved from the raising class's own `Category_` when non-GENERAL;
   otherwise fall back to `classifyError()` **text matching** on the message.
2. Context assembled from the diagnostic's `RelatedLocation` list plus a
   category-sized sliding window — explicitly not from scanning source for
   patterns (README documents this as a deliberate replacement of an older
   heuristic).
3. Category → a doc file under `docs/slm/` (per-category prose + per-code
   `docs/slm/codes/EXXX.txt`).
4. `qwen2.5-coder-1.5b-instruct-q4_k_m.gguf` via vendored llama.cpp,
   `n_ctx = 10240`, **greedy** sampling, output constrained by
   `grammars/slm_suggestion.gbnf` so malformed JSON is unrepresentable.
5. `computeStructuralPrior()` scores shallow signals (deterministic category
   +0.4, `MODIFY` +0.2, non-empty replacement +0.2, single clean line +0.1,
   keyword on a word boundary +0.1), with a **hard zero** if the model echoed
   the original line back.
6. `Sandbox` deep-copies compiler state, applies the patch, re-parses and
   re-checks, and confirms the original error is gone.
7. `finalizeConfidence()`: validated → `max(0.9, prior)`; not validated →
   `min(prior, 0.5)`, i.e. **an unvalidated fix can never clear the 0.8
   display gate**. Retry loop feeds the sandbox's real diagnostics back in as
   `previousAttemptFailed`.

This is a better-engineered pipeline than the "LLM in a compiler" framing
suggests, and §16 recommends *keeping* its two strongest properties (grammar
constraint, sandbox verification) unchanged.

### 1.13 Tooling

`--emit-llvm`, `--dump-ast`, `--resolve`, `--dump-resolver-symbols`,
`--dump-resolved`, `--typecheck`, `--dump-typechecked`, `--dump-modules`,
`--format`/`--write` (new, indentation-only), `--explain EXXX`, `--smart`,
`-o`, `--link`. Plus `ListAvailableModules()` and type-annotation source
spans added recently for tooling. No LSP, no `turf test`, no package manager,
no `-g`, no `-O`.

---

## 2. Language Completeness Audit

Classification key: **Complete** · **Partial** · **Weak** (exists but
architecturally unsound) · **Missing-Important** · **Missing-Unnecessary** ·
**Blocked** (depends on another decision).

| Area | Status | Evidence / reasoning |
|---|---|---|
| Lexical syntax | **Partial** | No exponent/hex/binary/octal/`_` literals (verified). Everything else is fine. |
| Integer semantics | **Weak** | Silent literal clamping, silent runtime wrap, `INT64_MIN` unwritable. §10. |
| Floating-point semantics | **Weak** | Silent literal→`inf`; but `/` traps on zero so IEEE `inf`/`NaN` are largely unreachable — an undocumented, undesigned mix. §10. |
| Type inference | **Missing-Important** | None at all. Annotation mandatory. §11. |
| Default initialization | **Weak** | Two incompatible mechanisms (A: definite assignment; B: eager default) split by type. §12. |
| Structs | **Partial** | Value semantics, inline nesting, `::new`/`::default` — all good. But **no acyclicity check**: `struct A { me: A }` compiles and segfaults on use. |
| Enums | **Complete** | Ordinal-backed, `::` access, exhaustiveness checking in `compare`/`matches`, int/string casts. |
| `machine` (state machines) | **Complete** | A genuinely distinctive feature; `TurfMachineType : TurfEnumType` is a clean design. |
| Generics | **Weak** | Erasure; ICE above 8 bytes; no per-`T` operations; `sizeof(T)` impossible. §9. |
| Operator overloading | **Partial** | 10 binary operators only. No unary, `[]`, `%`, `**`, bitwise, compound. Exact-match resolution is correct and intentional. §13. |
| Function overloading | **Complete** | Arity+type keyed, mangled names, UFCS, module-qualified. |
| First-class functions | **Partial** | `FuncType`, function values, `fn(T):R` parameters all work. |
| Closures / lambdas | **Missing-Important** | Verified: `fn(x: int): int { … }` as an expression is a parse error. Blocks callback APIs; `http.tr`'s router had to be hand-rolled. **Blocked** on the memory model (a closure needs a heap-allocated, collectable environment). |
| Pattern matching | **Partial** | `compare`/`matches` is value-equality only: no destructuring, no bindings, no guards. Adequate; not a gap worth closing now. |
| Error handling | **Complete-ish** | `error` types, `!T`, three `recover` forms, divergence rule, exhaustive diagnostics, an architecture doc. **Missing:** error payloads (documented limitation) and a propagation shorthand. |
| References `&T` | **Partial** | Compile-time non-null only; no lifetime. Cannot be null → cannot terminate a list. |
| Raw pointers `T*` | **Weak** | Usable in user code only inside a top-level `unsafe` struct decl, and **never** in any user function signature. §1.7. |
| Nullable safe reference | **Missing-Important** | The single missing type that makes linked structures writable without `unsafe`. LANG-002. |
| Arrays | **Partial** | `T[N]` value semantics + bounds checks are solid. `T[]` aliases the caller's frame with no escape analysis. No slices/views. |
| Slices / views | **Missing-Important** | `T[]` is a parameter-only escape hatch, not a type. A real `Slice<T>` (ptr+len, non-owning) becomes natural once generics are monomorphized. |
| Strings | **Partial** | Correct as a C-compatible byte pointer; every producing operation leaks. §14, §15. |
| Collections | **Weak** | `List`/`Stack`/`Queue`/`Map`/`Set` exist but: leak unconditionally, break for >8-byte `T` (ICE), and compare `string` keys by pointer identity. All three are generics consequences. |
| Iterators / ranges | **Partial** | `for x in arr` over real arrays; `for i in a..b step e`. No iteration over any collection. **Blocked** on closures or an iterator protocol; deliberately not in this milestone. |
| Modules / imports | **Complete** | Genuinely good. `::`, on-demand imports, visibility, module-owned types, path-independent index. |
| Visibility | **Partial** | Module-level only; no per-declaration `pub`/`private`. Acceptable; not a priority. |
| Separate compilation / ABI | **Missing-Important** | None. Every import is recompiled from source into every binary. Not currently a *performance* problem (0.3 s compiles), but it is what blocks a package ecosystem. |
| Constructors/destructors | **Partial / Missing** | `::new`/`::default` exist. **No destructors and no scope hook of any kind** — nothing could free a `List<T>`. |
| `defer` | **Missing-Important** | Needed for OS resources (files, sockets) regardless of GC. LANG-003. |
| Memory management | **Missing-Important** | Nothing at all. §14. |
| GC | **Missing-Important** | §14. **Blocked** on monomorphization. |
| Concurrency / async | **Missing-Unnecessary (now)** | No threads, no runtime to host them. Deliberately out of scope. |
| FFI / C interop | **Complete** | `extern fn`, `extern from "lib" fn`, variadics, `ptr`, `int32`. Genuinely strong — `showcase/llvm_ffi_selfhost_demo.tr` binds LLVM-C. |
| Compile-time evaluation | **Partial** | `sizeof`, enum ordinals, `export immut` literal constants, `ValueRangePass` folding. No user-facing `comptime` — and none should be added. |
| Reflection / RTTI | **Missing-Unnecessary** | `typeof` is static-only. Runtime type descriptors *will* exist for GC (§14) but must stay a runtime implementation detail, not a language feature. |
| Compile-time diagnostics | **Complete** | 68 codes, `--explain`, spans, suggestions, hints. A real strength. |
| Runtime diagnostics | **Weak** | Panics report a bare string from the stdlib's own line, not the caller's. §15.6. |
| Static analysis | **Complete** | The other real strength. One documented gap: `LintExpression()` has no `VarDeclExprAST` branch, so **every variable declaration's initializer is skipped by every lint check** (handbook ch09 pitfall, confirmed live). |
| Optimization | **Weak** | §1.9. |
| Debug info | **Missing-Important** | No `-g`, no DWARF. |
| Tooling | **Partial** | Good CLI/diagnostics; `--format` is new; no LSP/test-runner/package manager. |
| Build integration | **Partial** | `cc`-shells-out linking; no `--target`, no `-mcpu`, no static/shared control. |
| Safety boundary | **Weak** | `unsafe` is well designed *as a gate* but its exemption is a filesystem path, and it is simultaneously too strict for users (no `T*` in any signature, ever) and unenforceable for the stdlib. |
| Performance predictability | **Partial** | Unconditional bounds and div-by-zero checks are a defensible, consistent choice; the missing optimizer means the cost is never recovered. |

### 2.1 The five things a programmer will hit first

Ordered by how quickly a new user runs into them, which is a different order
from architectural severity:

1. `mut x: int = 5;` — the mandatory annotation on every single declaration.
2. Cannot write a linked list / tree without `unsafe`, and cannot pass one to
   a function at all.
3. Generic collections break on their own structs (ICE).
4. Everything leaks.
5. No `-O`, no `-g`.

Items 1–4 are all on this roadmap. Item 5 is OPT-001/TOOL-001.

---

## 3. Compiler / Runtime / Standard-Library Boundary

### 3.1 How mature implementations actually divide this

Worth stating once, because the useful lesson is about *mechanism*, not
feature lists.

- **C** draws the line at the ABI. The compiler knows almost nothing about
  the library; `printf` is an ordinary symbol. What makes it work is that
  the *calling convention and object model* are specified independently of
  both. Lesson for TurfLang: a boundary is only real if it survives
  separate compilation — and TurfLang has none, which is exactly why its
  "the stdlib is just Turf code" claim is currently untrue.
- **Go** has a small language and a large runtime, and the compiler emits
  *calls into the runtime* for the operations the language guarantees
  (`runtime.newobject`, `runtime.mapaccess`, write barriers, goroutine
  spawn). The compiler and runtime are co-designed and versioned together;
  the standard library sits on top as ordinary Go. **This is the closest
  model to what TurfLang should become.**
- **Rust** puts an enormous amount in the library (`Vec`, `Box`, `String`)
  and a small, named set in the compiler (`core` lang items, intrinsics).
  Crucially the privileges are *declared* (`#[lang = "…"]`, `#[rustc_…]`),
  not inferred from a path. Lesson: name the privileges.
- **Java** is the extreme case of runtime-owns-everything (GC, class loading,
  JIT, reflection), with the language deliberately small. Lesson: a precise
  GC needs the compiler to emit type information the runtime consumes — the
  compiler/runtime contract is a *data format*, not just a function call.
- **Swift** pushes the most into a runtime library with a stable ABI and
  reabstraction thunks. Lesson mostly cautionary: the cost is enormous.
- **LLVM** should own: instruction selection, register allocation, generic
  scalar/loop optimization, object emission, DWARF encoding. TurfLang should
  own: anything that needs TurfLang semantics to be correct
  (monomorphization, bounds-check elimination driven by `ValueRangePass`,
  GC-safety invariants, diagnostics).

### 3.2 The target boundary for TurfLang

```
┌──────────────────────────────────────────────────────────────────────────┐
│ LANGUAGE — what the spec guarantees, independent of any implementation    │
│   value/reference semantics · type rules · numeric model (§10) ·          │
│   initialization rules (§12) · operator meaning (§13) · error model ·     │
│   "reachable GC-heap objects are never freed" (§14)                       │
└───────────────────────────────┬──────────────────────────────────────────┘
                                │
┌───────────────────────────────▼──────────────────────────────────────────┐
│ COMPILER (turf)                                                          │
│   Lexer · Parser · Resolver · TypeChecker · MONOMORPHIZER (new) ·         │
│   CFG analyses · Lowering · Type-descriptor emission (new) ·              │
│   Diagnostics + SLM · Optimization driver                                 │
│                                                                           │
│   Owns: every decision that needs TurfLang semantics to be correct.       │
│   Emits: LLVM IR + per-type GC descriptors + calls to runtime entrypoints.│
└───────────────┬──────────────────────────────────┬───────────────────────┘
                │                                  │
┌───────────────▼─────────────────┐  ┌─────────────▼───────────────────────┐
│ INTRINSICS / BUILTINS           │  │ LLVM                                │
│  A CLOSED, DECLARED LIST (§15.3)│  │  instsel · regalloc · scalar/loop   │
│  sizeof · typeof · caller_loc   │  │  opt · object emission · DWARF      │
│  checked arithmetic · gc_alloc  │  │                                     │
└───────────────┬─────────────────┘  └─────────────────────────────────────┘
                │
┌───────────────▼──────────────────────────────────────────────────────────┐
│ RUNTIME — libturfrt (NEW; does not exist today)                          │
│   allocator · garbage collector · root scanning · type-descriptor reader  │
│   panic/abort/backtrace · process bootstrap · out-of-memory policy        │
│                                                                           │
│   Written in C (not TurfLang): it must run before/below any TurfLang      │
│   invariant it is responsible for upholding. Linked into every binary.    │
└───────────────┬──────────────────────────────────────────────────────────┘
                │
┌───────────────▼──────────────────────────────────────────────────────────┐
│ STANDARD LIBRARY — lib/std/**.tr, ordinary TurfLang                      │
│   core (prelude) · collections · io/net/fs · mem · math · time · random   │
│   json · http · testing                                                   │
│                                                                           │
│   Privileges are DECLARED IN SOURCE, not inferred from a path (§15.2).    │
└───────────────┬──────────────────────────────────────────────────────────┘
                │
┌───────────────▼──────────────────────────────────────────────────────────┐
│ TOOLING — turf fmt · turf test · turf explain · LSP · (later) packages    │
└──────────────────────────────────────────────────────────────────────────┘
```

### 3.3 The three boundary rules

**Rule B1 — The compiler owns anything whose correctness depends on
TurfLang's type system.** Monomorphization, GC descriptor emission, overload
and operator resolution, bounds-check elimination. Corollary: `to`-cast
legality must move **out of Codegen and into the TypeChecker** (§1.7), because
it is a type-system question.

**Rule B2 — The runtime owns anything that must be true across the whole
process and cannot be re-derived per translation unit.** The heap, the
collector, the root set, panic behaviour, the OOM policy. The runtime is
written in C and is versioned lock-step with the compiler. It is *not*
written in TurfLang, and that is deliberate: it has to be correct below the
level of the invariants it enforces.

**Rule B3 — Everything else belongs in `lib/std` as ordinary TurfLang, and
the standard library gets no capability that a user cannot request
explicitly.** A user writing FFI bindings for a C library has exactly the
same legitimate need for raw pointers that `lib/std/net` has. The answer is a
declared capability, not a path check.

### 3.4 Header hygiene: the parser's incidental LLVM dependency

Separate from the layering question in §1.3, and much smaller. `SymbolTable.h`
pulls in `llvm/IR/Instructions.h` for a single field:

```cpp
llvm::AllocaInst *Alloca;   // include/SymbolTable.h:22
```

It is only ever used as a pointer, so a forward declaration
(`namespace llvm { class AllocaInst; }`) would suffice. Measured cost of not
doing that:

| | |
|---|---|
| `src/ParserDecl.cpp` preprocessed | 182,910 lines |
| of which `llvm/IR/Instructions.h` | 119,047 lines (~65%) |
| syntax-check time at `-O2` | 3.07 s for that one TU |

This costs **build time of the compiler**, not run time of compiled programs
— TurfLang compiles a hello-world in ~0.3 s, and a symbol-table lookup is a
hash-map probe. It is pure hygiene, worth one afternoon, and it belongs to
whichever session is already editing `SymbolTable.h`. It is deliberately
**not** given a feature ID: it is not architecture, and inventing a roadmap
item for it would overstate its importance.

Note that `ParserCore.cpp`/`ParserDecl.cpp` also include
`llvm/ADT/SmallString.h`, `llvm/Support/FileSystem.h` and
`llvm/Support/Path.h` directly for their own module-path handling, so the
parser would not become LLVM-free even after this — it would become
*LLVM-IR*-free, which is the meaningful boundary.

### 3.5 "Is the stdlib being 'just another Turf program' a problem?"

**No, and it should stay that way** — with one correction and one addition.

It is *not* a problem that `strings::to_upper` is ordinary TurfLang. That is
a strength: it means the language is expressive enough to write its own
library, it keeps the compiler small, and it means every stdlib bug is
fixable without touching C++. `strings.tr`'s `operator +` replacing Codegen's
hand-rolled `EmitStringConcat` was a genuine architectural improvement and
should not be reversed.

The **correction**: the claim is currently false in one specific, undeclared
way — `IsPathUnderLibStd`. Fix the mechanism, not the privilege (§15.2).

The **addition**: the standard library currently cannot say anything to the
compiler. It cannot report a compile-time error, cannot attribute a runtime
panic to its caller, and cannot express a requirement on a type parameter.
That is the real weakness, and §15.6 addresses it — mostly for free, as a
by-product of monomorphization.

---
## 4. Architectural Decisions That Must Be Frozen

These are the decisions that multiple, independently-run implementation
sessions would otherwise each make differently, producing an incoherent
language. §6 restates them in the form the freeze document should take.
Rationale for each lives in the section named.

| ID | Decision | Section |
|---|---|---|
| **F-GEN-1** | Generics are **monomorphized**. Type erasure is removed entirely. | §9 |
| **F-GEN-2** | No `TurfGenericParamType` may reach Codegen. Enforced by assertion. | §9 |
| **F-GEN-3** | No trait/constraint system. Requirements are reported at instantiation sites. | §9, §25 |
| **F-NUM-1** | Integer literals are **untyped and arbitrary-precision** until a destination type is known. | §10 |
| **F-NUM-2** | Assigning a literal not representable in its destination type is a **hard error**, never a clamp. | §10 |
| **F-NUM-3** | Signed integer overflow **traps at runtime**; it is never wrapping and never UB. | §10 |
| **F-NUM-4** | Floating-point overflow at runtime produces IEEE `inf` and does **not** trap; a non-representable float *literal* is a hard error. | §10 |
| **F-NUM-5** | No new integer widths, no unsigned types. `int` (64) and `int32` (FFI) only. | §10, §25 |
| **F-NUM-6** | Language semantics never vary with optimization level. | §10, §17 |
| **F-GUESS-1** | `guess` is **Model A**: initializer-only inference, type permanently fixed at the declaration. | §11 |
| **F-GUESS-2** | Model B (declare-then-infer-from-first-assignment) is **rejected and must not be added**. | §11 |
| **F-GUESS-3** | Inference is **local and one-directional**. Never for parameters, return types, or struct fields. | §11 |
| **F-INIT-1** | **One** initialization mechanism: definite assignment, for every type. The eager auto-default (Mechanism B) is removed. | §12 |
| **F-INIT-2** | Storage is still always zero-filled. Definite assignment is a static net over already-zeroed memory. | §12 |
| **F-INIT-3** | A struct may not contain itself by value, transitively. Checked in the Resolver. | §12 |
| **F-MEM-1** | The memory model is a **non-moving, mark-sweep tracing collector**, precise on the heap, conservative on stack and registers. | §14 |
| **F-MEM-2** | No finalizers, ever. Deterministic resource cleanup is `defer`. | §14 |
| **F-MEM-3** | `mem::alloc`/`malloc` memory stays **manual and untraced**. The two heaps never merge. | §14 |
| **F-MEM-4** | A `libturfrt` runtime library exists and is linked into every binary. | §14, §15 |
| **F-PTR-1** | `&T` non-null, `&?T` nullable-checked, `T*` raw-and-gated. Raw pointers are FFI-only. | §14, §13 |
| **F-OP-1** | Operator overload resolution stays **exact-match** (`sameTypeExact`). No widening, ever. | §13 |
| **F-OP-2** | Compound assignment desugars; there is no `operator+=`. | §13 |
| **F-OP-3** | Operator overloads live in the ordinary name-keyed overload map under synthesized names. No separate table. | §13 |
| **F-OP-4** | `&&`, `\|\|`, `=`, `()`, `.`, and conversion operators are **not** overloadable. | §13, §25 |
| **F-STD-1** | Standard-library privileges are **declared in source**, never inferred from a file path. | §15 |
| **F-STD-2** | The intrinsic/builtin list is **closed and enumerated** in one place. | §15 |
| **F-STD-3** | `string` remains a NUL-terminated byte pointer. | §15, §25 |
| **F-DIAG-1** | Sandbox verification of SLM suggestions is **never removed**. | §16 |
| **F-DIAG-2** | An unvalidated suggestion can never clear the display gate. | §16 |
| **F-DIAG-3** | No new `SLM::ErrorCategory` values without an explicit request. | §16 |
| **F-OPT-1** | Optimization levels change speed/size/debuggability only — never observable behaviour. | §17 |
| **F-OPT-2** | Bounds/overflow/div-by-zero checks may be *removed by proof*, never by flag. | §17 |
| **F-TRACK-1** | Every implementation session updates a change record under `docs/development/`. | §18 |

---

## 5. Current-State Contradictions and Unknowns

Each entry: what each source says, which is authoritative, and the proposed
resolution. **Nothing here is resolved by this document alone** — each is
resolved by the feature group named.

### C-1 · "Monomorphization" vs. type erasure (documentation)

- **Source (`include/Types.h:458`)**: `TurfGenericParamType` — *"replaced
  with concrete types during monomorphization."*
- **Source (`include/Types.h:474`)**: `GenericType` — *"This is the core of
  monomorphization."*
- **Source (`src/Types.cpp`, `typesCompatible()`)**: *"Under this compiler's
  type-erasure model, a generic struct's LLVM layout does not actually vary
  per type argument."*
- **Docs (`compiler_pipeline.md` §7)**: explicitly names the mismatch and
  states erasure is what happens.
- **Docs (`memory_model.md` §2.7)**: erasure, with the bit-level mechanism.
- **Authoritative**: the implementation. It is erasure.
- **Resolution**: TYPE-002 makes the *doc comments* true rather than the
  reverse. **Breaking**: yes, for any program relying on erasure's
  side-effects (see C-2).

### C-2 · Erasure's >8-byte limit: "documented limitation" vs. ICE

- **Docs (`memory_model.md` §2.7.3, `stdlib/collections.md`)**: a struct
  larger than 8 bytes "can't practically be used as `T`"; workaround is
  `List<T*>`.
- **Reality (verified)**: it is not a graceful restriction. It produces
  `Internal compiler error: generated LLVM IR is invalid. Invalid bitcast:
  bitcast %Big %x4 to ptr`.
- **Authoritative**: the compiler. The docs undersell it.
- **Resolution**: TYPE-002 removes the limit. Until then TYPE-002's first
  stage should add a *diagnostic* so the ICE is never user-visible.

### C-3 · Struct brace-literal initialization

- **`docs/planning/ROADMAP_v1.0.0_COMPLETE.md` §3**, under **"Final Syntax
  (locked in)"**: `mutable p: Point = Point { x: 10, y: 20 };`
- **`src/ParserLiteral.cpp`** comment: *"Struct initialization now uses brace
  syntax: Point { x: 10, y: 20 }"*.
- **Reality (verified)**: `Big { a: 1, b: 2, c: 3 }` →
  *"'Big { ... }' is no longer valid syntax for constructing a struct.
  Hint: use 'Big::new(...)' … or 'Big::default()'."*
- **`docs/reference/structs_and_enums.md`**: documents `::new`/`::default`
  as the only forms. Correct.
- **Authoritative**: the compiler + the reference.
- **Resolution**: the ROADMAP section is stale history. Mark it as such in
  ARCH-001; do not change the language. **Breaking**: no.

### C-4 · Default initialization: one rule or two?

- **`docs/reference/variables_and_types.md` §"Default initialization"**:
  documents **two** mechanisms and calls the split *"the single most
  important gotcha in the type system."*
- **Handbook `ch12`**: same, with a `pitfall` block.
- **Implementation (`ParseLetStatement`)**: `int`/`double`/`bool`/`string`/
  pointer get **no** initializer (→ definite assignment); everything else
  gets `SynthesizeDefault()` → `DefaultInitExprAST` (→ eager default).
- **Note**: `SynthesizeDefault()`'s own `isInt()/isDouble()/isBool()/
  isString()` branches are **dead code** on that path — the caller has
  already excluded those types.
- **Authoritative**: the implementation; docs describe it accurately.
- **Resolution**: SEM-004 unifies on definite assignment (F-INIT-1).
  **Breaking**: yes — 13 files in `tests/10_default_init/` depend on
  Mechanism B.

### C-5 · Literal syntax in the roadmap

- **ROADMAP §5** shows `mut flags: int = 0b1010 & 0b1100;` (marked
  "illustrative only").
- **`docs/reference/variables_and_types.md`**: *"There is no hex/octal/binary
  literal syntax, no digit-group separators, and no integer literal type
  suffixes."*
- **Verified**: `0b1010`, `0xFF`, `1_000` are all parse errors.
- **Authoritative**: the reference.
- **Resolution**: SEM-001 adds them, making the roadmap snippet true by
  accident. **Breaking**: no (purely additive).

### C-6 · "The stdlib is ordinary Turf with no compiler special-casing"

- **`AGENTS.md`**: *"No compiler special-casing; ordinary `.tr` files."*
- **Reality**: `TypeChecker::isUnsafeGateExempt()` returns true for any file
  under `lib/std`, and `checkNoRawPointersInSignature()` returns early for
  the same. Verified: a user `fn walk(n: Node*)` is rejected even inside
  `unsafe`; the identical signature in `lib/std` is accepted.
- **Authoritative**: the implementation. AGENTS.md is aspirational.
- **Resolution**: STD-001/LANG-002 replaces the path check with a declared
  capability, which makes AGENTS.md's sentence true again. **Breaking**: for
  stdlib sources (they gain an explicit declaration); not for user code.

### C-7 · `s[i]` is `int`, but `char` exists

- **`docs/reference/operators.md`**: *"`s[i]` has static type `int`, not
  `char`, even though it's indexing into byte data."*
- **`char` was added later** (`STRING_UFCS_OPERATOR_CHAR_RFC`), and is
  explicitly a single byte.
- **Authoritative**: the implementation.
- **Resolution**: this is a genuine design inconsistency, but changing
  `s[i]` to yield `char` is breaking and low-value relative to everything
  else here. **Deferred**, recorded, not scheduled. Listed so a future
  session does not "fix" it opportunistically.

### C-8 · `docs/planning/Errors.md` does not exist

- **43 source comments** across 22 files cite `docs/planning/Errors.md` as
  the design authority for the error/`!T`/`recover` feature.
- The file is not in the tree. Seven `docs/` paths are dangling in total:
  `COMPILER_MODULE_BREAKDOWN.md`, `Errors.md`, `FINAL_SPRINT.md`,
  `PHASE_3_ISSUES.md`, `ROADMAP_v1.0.0.md`, `detailed/modules_and_stdlib.md`,
  `patent/PATENT_BENCHMARK_DATA.md`.
- **Authoritative**: `docs/architecture/errors_and_recover.md` is the live,
  accurate replacement for `Errors.md`'s content.
- **Resolution**: ARCH-001 adds a CI check that every `docs/…md` path cited
  from source or docs exists, and re-points the 43 comments. This is the
  single clearest demonstration of why §18's change-tracking system is
  needed.

### C-9 · Lint skips every variable declaration's initializer

- **Handbook ch09 `pitfall`**: `LintExpression()` has no `VarDeclExprAST`
  branch; `mut x: int = 5 / 0;` produces zero warnings while `x = 5 / 0;`
  correctly warns. Confirmed live by the handbook's author.
- **Nothing in `docs/`** mentions it.
- **Authoritative**: the handbook is right; this is a live gap.
- **Resolution**: fold into DIAG-002 (a one-branch fix plus regression tests
  in `tests/14_cfg_analysis/`).

### C-10 · The TypeChecker is described as LLVM-free

- **`include/TypeChecker.h`** and `ResolverSymbolTable`'s doc comment frame
  the frontend as LLVM-free and unit-testable without the backend.
- **Reality**: `TypeCheckerCall.cpp` (31) + `TypeCheckerExpr.cpp` (7) +
  `TypeCheckerStmt.cpp` (4) + `TypeCheckerDecl.cpp` (2) = **44** references
  to the LLVM-side `GlobalSymbolTable`.
- **Authoritative**: the implementation.
- **Resolution**: not scheduled as its own item, but TYPE-002 will have to
  touch much of this code; record the discrepancy and reduce it
  opportunistically **without** making it a goal (§25).

### C-11 · `NodeTypes_` is authoritative but unused

- **`include/TypeChecker.h:15-20`**: inferred types *"will gradually replace
  the equivalent monolithic type logic in Codegen."*
- **Measured**: Codegen references `NodeTypes_` **zero** times and
  `getExprType()` **38** times.
- **Authoritative**: the implementation; the header is aspirational.
- **Resolution**: TYPE-002 must decide which is authoritative for
  *monomorphized* nodes. The recommendation (§9.7) is that the monomorphizer
  writes concrete types onto the cloned AST itself, so both consumers agree
  by construction. Full unification of the two inference engines is
  explicitly **out of scope** (§25).

### C-12 · `fail_*.tr` for an infinitely-sized struct does not exist

- No test anywhere covers `struct A { me: A }`. It compiles; using it
  segfaults.
- **Resolution**: TYPE-001. Regression tests in `tests/06_structs/`.

### Unknowns requiring investigation before implementation

| ID | Question | Who answers it |
|---|---|---|
| **U-1** | How many `.tr` files across `tests/`, `lib/std/`, `showcase/` actually depend on Mechanism B default init? (13 known in `tests/10_default_init/`; `lib/std` and `showcase` are **not** run by `test_runner.sh` and must be swept manually per AGENTS.md.) | SEM-004, stage 0 |
| **U-2** | What is the real instantiation count of generics across the stdlib + tests? Determines monomorphization's compile-time and binary-size cost. Measure before TYPE-002 stage 3. | TYPE-002, stage 0 |
| **U-3** | Does conservative stack scanning find all roots given the current alloca-heavy Codegen plus mem2reg? Registers must be scanned too; verify with a deliberate-retention test. | MEM-002, stage 1 |
| **U-4** | Does the double-parse pre-pass (§1.1) need to run the monomorphizer as well, or can instantiation be deferred entirely to the authoritative pass? | TYPE-002, stage 1 — **answer this before writing any code** |
| **U-5** | What fraction of `--smart` diagnostics could be fixed deterministically with no model at all? Needed to size DIAG/SLM-001's benefit. | SLM-001, stage 0 |

---

## 6. Semantic Freeze Specification

> This section is the proposed content of `docs/development/FREEZE.md`.
> Once ARCH-001 lands, that file is authoritative and this section becomes a
> historical copy. **Every implementation session reads FREEZE.md first.**

### 6.0 How to use this document

1. A decision recorded here is **not** open for reinterpretation by an
   implementation session. If a session believes a frozen decision is wrong,
   it stops, writes the argument into
   `docs/development/changes/<ID>.md` under *Unresolved questions*, and asks.
2. A decision **not** recorded here is an ordinary engineering judgement —
   make it, and record it in the change record.
3. Adding a new frozen decision requires an explicit request. Sessions do not
   freeze things on their own initiative.

### 6.1 Generics — FROZEN

- Generics are **monomorphized**: one fully-specialized copy of a generic
  function, method, or struct per distinct tuple of concrete type arguments.
- `TurfGenericParamType` exists **only** inside an uninstantiated template.
  Reaching Codegen with one is a compiler bug; Codegen asserts on it.
- `typesCompatible()`'s Rule 1 ("generic param matches anything") applies
  **only** during candidate matching, never after substitution.
- Instantiation depth is capped (default 64) with a dedicated diagnostic
  naming the instantiation chain.
- There is **no** trait/constraint syntax. A generic body that uses an
  operation the concrete `T` does not support is an error reported **at the
  instantiation site**, with a `withRelated()` note at the generic
  declaration.
- Monomorphized symbol names extend the existing mangling
  (`turf_<module>_<name>_<paramty>…`) with the type arguments; the scheme
  must remain a pure function of (module, base name, type arguments,
  parameter types).

### 6.2 Numeric semantics — FROZEN

**Six distinct concepts, never conflated:**

| # | Concept | Rule |
|---|---|---|
| 1 | Lexical validity | `[0-9_]+`, `0x…`, `0b…`, `0o…`, `[0-9_]*.[0-9_]+([eE][+-]?[0-9]+)?`. `_` is a separator, never leading or trailing. |
| 2 | Literal representability | An integer literal is held exactly (arbitrary precision) until typed. A float literal is held as the exactly-rounded `double` **plus a flag for whether rounding overflowed to `inf`**. |
| 3 | Destination representability | Checked when the literal's type is fixed. Out of range → **hard error**, never a clamp. A float literal that overflowed to `inf` → hard error. |
| 4 | Implicit conversion | `int32 → int`, `int32 → double`, `int → double`. Nothing else. No narrowing, no `bool` ↔ numeric, no `char` ↔ numeric. |
| 5 | Explicit conversion (`to`) | Per the table in `docs/reference/variables_and_types.md`, **enforced in the TypeChecker**, not Codegen. `double → int` truncates toward zero. `int → int32` truncates. |
| 6 | Arithmetic overflow | Compile-time on untyped literals: exact, arbitrary precision, error only if the *result* is not representable in the destination. Compile-time on typed constants: **error**. Runtime, signed integer: **trap** (panic). Runtime, floating point: IEEE (`inf`/`NaN`), **no trap**. |

- `-9223372036854775808` is writable: unary `-` applied directly to an
  untyped integer literal folds into the literal *before* representability is
  checked.
- Division and modulo by zero continue to trap at runtime for both `int` and
  `double` (existing behaviour, deliberately preserved — it is why IEEE
  `inf`/`NaN` are largely unreachable in TurfLang, and that asymmetry is now
  a documented decision rather than an accident).
- `int` is 64-bit signed; `int32` is 32-bit signed and exists solely for
  honest `extern fn` ABI declarations. **No further integer widths, and no
  unsigned types, are to be added.**
- Semantics **never** vary with optimization level.

### 6.3 `guess` / inference — FROZEN

- `guess` is a **type-position keyword**: `mut x: guess = 5;`. Omitting the
  annotation entirely (`mut x = 5;`) is defined as exactly `: guess`.
- **Model A only.** The type is inferred from the initializer and is
  **permanently fixed** at the declaration. `mut x = 5; x = 5.5;` is an
  error.
- **Model B is rejected.** A declaration with `guess` and no initializer is a
  compile error. There is no flow-sensitive type assignment anywhere in the
  language.
- Inference is **local, one-directional, and single-expression**. It never
  applies to a function parameter, a return type, a struct field, an
  `export immut` constant, or a generic type argument.
- Defaulting for untyped literals under `guess`: integer → `int`,
  float → `double`, `true`/`false` → `bool`, `'c'` → `char`, `"s"` →
  `string`, `[…]` → `T[N]`. **The default type is chosen first; then
  representability is checked against it.** `mut x = 999999999999999999999;`
  is therefore an error, not a promotion to a wider type (there is no wider
  type).
- `guess` never infers `void`, never infers the bare type of `null`, and
  never infers an uninstantiated generic.

### 6.4 Initialization — FROZEN

- **One** mechanism: **definite assignment**, for every type without
  exception. Reading a body-declared local before it is definitely assigned
  on every path is `DefiniteAssignmentError` (E008).
- Storage is still always zero-filled. Definite assignment is a purely static
  safety net over memory that already reads as zero — this property is
  preserved verbatim from today and is what makes the check removable in
  principle without becoming a memory-safety hazard.
- The eager auto-default (Mechanism B) is **removed**. `T::default()` is the
  explicit replacement and already exists.
- `immut`/`immutable` still require an initializer.
- **A struct may not contain itself by value, transitively.** Checked once in
  the Resolver, before any layout or default value is computed. The
  diagnostic names the full cycle.
- Struct *fields* are not subject to definite assignment; a struct value is
  either fully constructed (`::new` — already checked by
  `ConstructorFieldAssignmentPass`) or fully defaulted (`::default`).

### 6.5 Memory model — FROZEN

- **Two disjoint heaps.** The **GC heap** (reachability-managed) and the
  **manual heap** (`mem::alloc`/`malloc`, entirely the programmer's). A
  pointer never migrates between them, and the collector never traces into
  manual memory.
- The collector is **non-moving** (object addresses are stable for life),
  **mark-sweep**, **precise on the heap** (per-type pointer bitmaps emitted
  by the compiler), **conservative on stack and registers**, **stop-the-
  world**, **single-threaded**.
- **Non-moving is non-negotiable.** A `string` handed to `write(2)` or a
  buffer handed to `send(2)` must not move; TurfLang's stdlib is
  overwhelmingly FFI.
- **No finalizers.** Deterministic cleanup for OS resources is `defer`.
- What lives on the GC heap: `string` values produced by any operation,
  collection backing buffers, closure environments (if closures land), boxed
  values. What does not: stack locals, `T[N]`, struct values, `mem::alloc`
  results, string literals (static `.rodata`).
- The GC entrypoints (`turf_gc_alloc`, `turf_gc_collect`,
  `turf_gc_add_root`, `turf_gc_remove_root`) live in `libturfrt` and are
  reached through the closed intrinsic list, never by an ad-hoc `extern fn`.
- **Precise heap tracing requires monomorphization** (F-GEN-1). GC must not
  be implemented before it.

### 6.6 Pointers and references — FROZEN

| Type | Nullable | Checked | Where legal |
|---|---|---|---|
| `&T` | no | compile-time non-null | anywhere |
| `&?T` | yes | must be null-tested before use (`NullabilityPass`) | anywhere |
| `T*` | yes | none | `extern fn` signatures, and inside `unsafe { }` in a module that declares the raw-pointer capability |
| `ptr` | yes | none | same as `T*` |

- `&T` → `&?T` → `T*` widens implicitly; nothing widens back without an
  explicit `unsafe`-gated `to`-cast.
- `self` remains a raw `StructName*` internally and remains exempt. Do not
  change this.

### 6.7 Operators — FROZEN

- Overload resolution is **exact type match** (`sameTypeExact`), never
  `typesCompatible()`. This is deliberate and long-standing: it is what keeps
  `operator+(string,string)` from being reached by unrelated widened
  operands, and it makes resolution predictable.
- Overloads are stored in the **ordinary name-keyed overload map** under a
  synthesized name (`"operator+"`). No separate operator table.
- Overloadable, after this milestone: binary `+ - * / % ** == != < > <= >=
  & | ^ << >>`; unary `- ! ~`; indexing `[]` (read) and `[]=` (write).
- **Not** overloadable, deliberately: `&&`, `||` (short-circuit cannot
  survive a call), `=` (assignment), `()` (call), `.` (member access), `to`
  (conversion), and any compound assignment.
- Compound assignment **desugars**: `a op= b` ≡ `a = a op b`, and therefore
  uses `operator op`. There is no `operator+=`.
- A declaration whose operands are **all** compiler-primitive
  (`int`/`int32`/`double`/`bool`/`char`/pointer) is rejected —
  `OperatorBuiltinPairError`, existing behaviour, preserved. `string` is
  deliberately **not** in that set, because its operators are stdlib-defined
  and must stay that way.

### 6.8 Standard library, compiler, runtime boundary — FROZEN

- Rules B1/B2/B3 of §3.3.
- The intrinsic list is **closed and enumerated in one header**
  (`include/Intrinsics.h`). Adding an intrinsic is an explicit,
  change-recorded act.
- Standard-library privileges are declared in source (a module-level
  capability declaration), never inferred from a file path.
- `libturfrt` is written in C, versioned lock-step with the compiler, and
  linked into every binary.
- `string` stays a NUL-terminated byte pointer.

### 6.9 Diagnostics — FROZEN

- `DiagnosticEngine` remains the single sink; one primary error per line.
- Every `TurfError` subclass carries its own code and category. **Deriving a
  category by matching text in the message is to be deleted**, not extended.
- SLM: grammar-constrained output and sandbox verification are permanent.
  An unvalidated suggestion is capped below the display gate.
- No new `ErrorCategory` values or `docs/slm/` category files without an
  explicit request.

### 6.10 Optimization — FROZEN

- Optimization levels change compile time, run time, binary size and
  debuggability. They **never** change observable behaviour.
- Language-mandated runtime checks (bounds, division by zero, integer
  overflow trap) may be eliminated **only** by proof, never by flag.
- LLVM owns instruction selection, register allocation, generic scalar and
  loop optimization, object emission, and DWARF encoding. TurfLang owns
  anything requiring TurfLang semantics.

---

## 7. Dependency Graph

```
                         ┌──────────────┐
                         │  ARCH-001    │  change tracking + freeze doc
                         │  (do first)  │  no code; unblocks recording
                         └──────┬───────┘
                                │ (every item below records into it)
        ┌───────────────┬───────┴────────┬──────────────┬─────────────┐
        │               │                │              │             │
   ┌────▼─────┐   ┌─────▼─────┐   ┌──────▼─────┐  ┌─────▼────┐  ┌─────▼────┐
   │ TYPE-001 │   │  SEM-001  │   │  OPT-001   │  │ TOOL-001 │  │ DIAG-001 │
   │ struct   │   │ literal   │   │ new PM +   │  │ debug    │  │ category │
   │ acyclic  │   │ represent.│   │ -O levels  │  │ info -g  │  │ from cls │
   └────┬─────┘   └─────┬─────┘   └──────┬─────┘  └─────┬────┘  └──────────┘
        │               │                │              │
        │          ┌────▼──────┐         │              │
        │          │  SEM-002  │         │              │
        │          │ numeric   │         │              │
        │          │ semantics │         │              │
        │          │ + overflow│         │              │
        │          └────┬──────┘         │              │
        │               │                │              │
        │          ┌────▼──────┐         │              │
        │          │  SEM-003  │         │              │
        │          │  guess    │         │              │
        │          └────┬──────┘         │              │
        │               │                │              │
   ┌────▼───────────────▼────┐           │              │
   │        SEM-004          │           │              │
   │  unify initialization   │           │              │
   └─────────────────────────┘           │              │
                                         │              │
   ┌─────────────────────────────────────┴──────────────┴──────────┐
   │                        TYPE-002                                │
   │              MONOMORPHIZATION  (the keystone)                  │
   │  needs: TYPE-001 (finite layouts), SEM-001 (stable literals)   │
   │  benefits from: OPT-001 (GlobalDCE offsets code growth)        │
   └───────┬────────────────────┬───────────────────┬───────────────┘
           │                    │                   │
   ┌───────▼──────┐    ┌────────▼───────┐   ┌───────▼────────┐
   │   LANG-001   │    │    RT-001      │   │    STD-002     │
   │ operator     │    │ libturfrt +    │   │ stdlib         │
   │ completion   │    │ typed alloc    │   │ diagnostics    │
   │ (fixes       │    │ + panic +      │   │ (caller loc,   │
   │  Map<string> │    │ type descrs    │   │  instantiation │
   │  for free)   │    └────────┬───────┘   │  errors)       │
   └──────────────┘             │           └────────────────┘
                                │
   ┌──────────────┐    ┌────────▼───────┐
   │   LANG-002   │    │    MEM-001     │
   │ &?T nullable │    │ route string + │
   │ refs +       │    │ collections    │
   │ declared     │    │ through the    │
   │ capability   │    │ runtime alloc  │
   └──────┬───────┘    └────────┬───────┘
          │                     │
   ┌──────▼───────┐    ┌────────▼───────┐
   │   LANG-003   │    │    MEM-002     │
   │    defer     ├───►│  mark-sweep    │
   │              │    │  collector     │
   └──────────────┘    └────────┬───────┘
                                │
                       ┌────────▼───────┐      ┌────────────┐
                       │    STD-001     │      │  OPT-002   │
                       │ boundary +     │      │ BCE via    │
                       │ intrinsic list │      │ ValueRange │
                       │ + prelude spec │      │ + specializ│
                       └────────────────┘      └────────────┘

                       ┌────────────────┐
                       │    SLM-001     │  independent of everything;
                       │ benchmark +    │  schedule whenever
                       │ cache + det.   │
                       │ fix layer      │
                       └────────────────┘
```

**Not shown in the graph, because they depend on nothing but `ARCH-001` and
block nothing:** `HYG-001` (compiler hygiene + CFG warning rendering),
`LANG-004` (optional `for` `step`, `string`→`ptr` at FFI call sites,
function-local `extern fn`), and `SLM-001`. Schedule them wherever they fit.
`STD-003` (post-monomorphization stdlib modernization) sits at the far end,
after `TYPE-002` + `LANG-001` + `LANG-002`, and is the group that closes out
`docs/planning/LIMITATIONS.md`.

**Hard dependencies (a violation produces incorrect code, not just rework):**

| Edge | Why |
|---|---|
| TYPE-001 → TYPE-002 | Monomorphizing a type with no finite layout diverges. |
| TYPE-002 → RT-001 | Type descriptors cannot be emitted for an erased type. |
| TYPE-002 → MEM-002 | `IntToPtr` bit-stuffing manufactures fake pointers that would corrupt a conservative collector's root set. |
| SEM-001 → SEM-002 | Overflow rules need an exact literal value to reason about. |
| SEM-001 → SEM-003 | `guess`'s defaulting rule is defined in terms of untyped literals. |
| TYPE-001 → SEM-004 | Recursive default init is what makes the acyclicity check load-bearing. |
| RT-001 → MEM-001 → MEM-002 | Cannot collect what was not allocated through the runtime. |
| LANG-003 → MEM-002 | `defer` is the stated replacement for finalizers; GC must not ship without it. |

**Soft dependencies (ordering saves work but is not required):** OPT-001
before TYPE-002 (so monomorphization's code growth is measured against a
pipeline that can delete dead instantiations); TOOL-001 before MEM-002 (so a
GC bug can be debugged); DIAG-001 before SLM-001.

---
## 8. Implementation Roadmap

Ordered lower-effort → higher-effort **subject to** the dependency edges in
§7. Every group uses the same template so a session can be handed one
section and nothing else.

Complexity scale: **XS** (< 1 session) · **S** (1 session) · **M** (2–3) ·
**L** (4–6) · **XL** (7+, needs its own sub-plan).

---

### ARCH-001 — Development change-tracking and semantic freeze

**Priority** P0 (must be first) · **Complexity** S · **Prerequisites** none

**Why it must happen here.** Every subsequent group produces architectural
knowledge that would otherwise live only in a commit message and a diff. §5
C-8 is the proof that this already happens in this repo: 43 source comments
cite a design document that no longer exists.

**Relevant subsystems** documentation, CI.
**Relevant files** new `docs/development/**`; new `scripts/check_doc_links.sh`;
`AGENTS.md` (one added pointer, nothing else).

**Current behavior.** Planning knowledge is spread across
`docs/planning/{ROADMAP_v1.0.0_COMPLETE,DESIGN_NOTES,FUTURE,LIMITATIONS}.md`,
`docs/architecture/*.md`, source doc comments, and git history. Seven
documentation cross-references are dangling.

**Desired behavior.** §18's structure exists and is populated; a CI check
fails on a dangling `docs/…md` reference; `FREEZE.md` holds §6 verbatim; and
the §22.0 Session Start Protocol is **enforceable**, not merely written down:

- `docs/development/SESSION_PROTOCOL.md` — the canonical, authoritative copy
  of §22.0–§22.2.
- `scripts/session_preflight.sh <FEATURE-ID>` — prints that feature's reading
  list (AGENTS.md → FREEZE.md → the §8 section → the named
  `docs/architecture/*.md`) and verifies the starting state: clean worktree,
  `cmake --build build` succeeds, `./scripts/test_runner.sh` green. A session
  must never mistake a pre-existing failure for one it caused.
- `scripts/session_postflight.sh <FEATURE-ID>` — mechanically checks §22.2's
  seven items and exits non-zero on any miss. Specifically it must check that
  `changes/<ID>.md` exists **and** that its *Removed assumptions* and *Known
  pitfalls* sections are non-empty, since a stub record is the exact failure
  mode that produced C-8.
- **One pointer line in `AGENTS.md`** directing an agent to
  `SESSION_PROTOCOL.md` when the task is an overhaul feature group. AGENTS.md
  stays the source of truth for build/test conventions; this adds a pointer
  and changes nothing else.

**Semantic decisions.** None (no language change).

**Compiler / runtime / stdlib changes.** None.

**Diagnostics changes.** None.

**Testing scope.** `scripts/check_doc_links.sh` must fail on a deliberately
broken link and pass on a clean tree. Not a `.tr` test — it does not belong
in `tests/`.

**Handbook impact.** None yet; creates `docs/development/handbook-sync/PENDING.md`,
which every later group appends to.

**Change record to update.** Creates the system. Its own record is
`docs/development/changes/ARCH-001.md`.

**Breaking changes / migration.** None.

**Completion criteria.**
1. `docs/development/{FREEZE.md,INVARIANTS.md,INDEX.md,changes/,handbook-sync/}`
   exist, with `FREEZE.md` containing §6.
2. `TEMPLATE.md` exists (§18.3).
3. The doc-link check runs in CI and the tree passes it — meaning the seven
   dangling references are either restored or re-pointed. The 43
   `docs/planning/Errors.md` comments re-point to
   `docs/architecture/errors_and_recover.md`.
4. `docs/planning/ROADMAP_v1.0.0_COMPLETE.md` §3 gains a one-line note that
   its brace-literal struct syntax is superseded (C-3).
5. `SESSION_PROTOCOL.md` exists; `session_preflight.sh` and
   `session_postflight.sh` both run and both correctly fail on a deliberately
   broken input (a dirty tree, and a stub change record respectively).
6. `AGENTS.md` has the one pointer line — and nothing else changed in it.

**Risks.** Low. The only real risk is the system being created and then not
used; §22's session template makes updating it a completion criterion of
every later group.

**Must NOT reinterpret.** Do not restructure `docs/planning/` or
`docs/architecture/`. `docs/development/` is additive. **Do not rewrite
AGENTS.md** — it is the source of truth for build and test conventions and
this document deliberately does not duplicate it; add the single pointer line
and stop.

---

### TYPE-001 — Struct layout acyclicity

**Priority** P0 · **Complexity** XS–S · **Prerequisites** ARCH-001

**Why it must happen here.** It fixes two confirmed compiler segfaults, and
it is a hard prerequisite for both SEM-004 (recursive default init) and
TYPE-002 (monomorphizing a type with no finite layout diverges).

**Relevant subsystems** Resolver, type registry.
**Relevant files** `src/ResolverDecl.cpp` (`visitStructDecl`,
`validateNamedType`), `include/ResolverTypeRegistry.h`,
`src/ResolverTypeRegistry.cpp`, `include/ErrorsStruct.h`,
`include/ErrorCodes.h`, `docs/errors/codes/`, `docs/slm/codes/`.

**Current behavior (verified).**
- `struct A { me: A }` — an infinitely-sized type — **compiles**.
  `mut x: A;` then **segfaults** (exit 139, no output at all).
- `struct A { b: B } struct B { a: A }` — same: declaration compiles,
  instantiation segfaults.
- A *pointer* field (`next: Node*`) is fine and must stay fine; that is how
  recursive data structures are meant to be written.

**Desired behavior.** After all structs are hoisted (Resolver Pass 1), a
single pass computes the by-value containment graph over registered struct
types and rejects any cycle, naming the full path:

```
error: struct 'A' cannot contain itself by value [E0NN]
  cycle: A -> B -> A
  note: field 'A::b' has type 'B'
  note: field 'B::a' has type 'A'
  hint: make one of these fields a reference or pointer ('&A', '&?A', 'A*')
        -- a struct that contains itself by value has no finite size.
```

Edges in the graph: a field whose type is a struct, an **array of** a struct
(`B[4]` contains `B` by value), or a `GenericType` whose instantiation
contains one. Non-edges: `T*`, `&T`, `&?T`, `ptr`, `string`, and any array of
those.

**Semantic decisions.** F-INIT-3. **Invariant:** *every registered
`TurfStructType` has a finite, computable size.* This is the invariant every
later layout/default/descriptor computation may assume.

**Compiler changes.** One new function in the Resolver (a DFS with a
grey/black colouring, reporting the first cycle found per SCC so one bad
declaration does not produce N diagnostics). One new error class. Must run
**after** all structs are registered and **before** anything computes a
layout — i.e. at the end of Pass 1, not inside `visitStructDecl`.

**Runtime / stdlib / optimization / GC changes.** None.

**Generic interactions.** A generic struct's cycle check runs on the
*template*: `struct Box<T> { inner: Box<T> }` is a cycle regardless of `T`.
`struct Node<T> { v: T, next: Node<T>* }` is not. After TYPE-002 the check
also runs per instantiation (a `T` substituted into a by-value field can
create a cycle the template did not have) — TYPE-002 must call the same
function.

**Testing scope.**
- `tests/06_structs/fail_self_containing_struct.tr` — `struct A { me: A }`.
- `tests/06_structs/fail_mutual_struct_cycle.tr` — the A/B pair.
- `tests/06_structs/fail_struct_array_cycle.tr` — `struct A { bs: B[2] }`,
  `struct B { a: A }`.
- `tests/06_structs/self_referential_pointer_ok.tr` — `Node*` still compiles
  and runs (the good direction; AGENTS.md requires both).
- `tests/06_structs/nested_struct_by_value_ok.tr` — deep non-cyclic nesting
  still works.
- Sweep `lib/std/**` and `showcase/**` by direct compile (AGENTS.md: the test
  runner covers neither).

**Handbook impact.** `docs/reference/structs_and_enums.md`
§"Self-referential structs" gains the by-value rule. Handbook ch12's
default-init discussion is unaffected. Add to `handbook-sync/PENDING.md`.

**Change record** `docs/development/changes/TYPE-001.md`.

**Breaking changes.** Technically yes — programs that *declared* a cyclic
struct without using it now fail. In practice such a program could never run.

**Completion criteria.** The five tests above pass; both segfaults are gone
and produce the diagnostic; `--explain E0NN` prints prose; full suite green.

**Risks.** Low. Main risk is over-rejecting: be careful that `&T`/`&?T`
(which subclass `TurfPointerType`) are non-edges, and that a `NamedType`
placeholder not yet resolved to a `TurfStructType` is looked up through the
type registry rather than skipped.

**Must NOT reinterpret.** Pointer fields remain legal and are the intended
way to write recursive structures. Do not "fix" the crash by making
`GetDefaultValue()` depth-limited instead — that treats the symptom.

---

### SEM-001 — Literal representation and lexical numeric syntax

**Priority** P0 · **Complexity** M · **Prerequisites** ARCH-001

**Why it must happen here.** Everything in §10 and §11 is defined in terms of
an exact literal value, and today no exact literal value exists — `strtoll`
has already destroyed it by the time the parser sees it.

**Relevant subsystems** Lexer, Parser, AST, TypeChecker, Codegen (constant
emission), `ValueRangePass`, `Resolver::hoistExportedConstant`.
**Relevant files** `src/Lexer.cpp:753-772`, `include/Lexer.h` (`NumVal`,
`IntVal`), `src/ParserLiteral.cpp` (`ParseNumberExpr`),
`include/ASTExpr.h` (`NumberExprAST`), `src/CodegenExpr.cpp`,
`src/ValueRange.cpp` (`evalConstExpr`), `src/ResolverCore.cpp`
(`hoistExportedConstant`), `include/ResolverSymbolBase.h`
(`ResolverConstantSymbol`).

**Current behavior (verified).**
- `strtoll` with `errno` unchecked → a 48-digit literal silently becomes
  `9223372036854775807`.
- `strtod` with `errno` unchecked → `10^400` silently becomes `inf`.
- `NumberExprAST` stores `double Val` *and* `long long IntVal` *and* a
  `bool IsInteger`; the `double` copy is populated even for integers.
- No exponent, hex, binary, octal, or `_` separator syntax.
- `-9223372036854775808` is unwritable.

**Desired behavior.**
1. The lexer scans the full literal text, classifies its radix/form, and
   reports *lexical* errors (a bare `0x`, a `_` in a leading/trailing
   position, a malformed exponent) itself.
2. The literal's value is carried **exactly**: `llvm::APInt`/`APSInt` for
   integers (LLVM is already a link dependency — do not hand-roll bignums),
   `llvm::APFloat` for floats, with an explicit `overflowed` flag when the
   decimal→binary conversion rounded to infinity.
3. `NumberExprAST` becomes: `{ APSInt IntVal | APFloat FloatVal }` plus
   `IsInteger`, plus `IsUntyped` (true for a literal written in source, false
   for a compiler-synthesized constant), plus the original spelling for
   diagnostics.
4. Representability is **not** checked in this group — SEM-002 does that.
   This group's job is only to stop destroying the information.

**Semantic decisions.** F-NUM-1 and the lexical row of F-NUM-2's table.
Radix forms: `0x`/`0X` hex, `0b`/`0B` binary, `0o`/`0O` octal (**not** a bare
leading `0` — `0755` stays decimal `755`, because a silent octal
interpretation is a classic footgun and TurfLang has no legacy to preserve).

**Compiler changes.** Lexer rewrite of the number branch; new `APSInt`/
`APFloat` fields; `ParseNumberExpr` updated; `NumberExprAST::codegen()` emits
`ConstantInt::get(Ty, APInt)` / `ConstantFP::get(Ty, APFloat)` directly;
`evalConstExpr` moves to `APSInt` **with explicit overflow detection**
(`sadd_ov`/`ssub_ov`/`smul_ov`) — today it performs `lv + rv` on `long long`,
which is signed-overflow UB in the compiler's own C++ (§5, U-none: verified
by reading `src/ValueRange.cpp:43-52`).
`ResolverConstantSymbol` (which stores `IntVal`/`DoubleVal` for `export
immut`) updated to match.

**Runtime / GC / generic changes.** None.

**Optimization interactions.** `evalConstExpr`'s overflow detection changes
`ValueRangePass` from "may be wrong" to "known-correct or unknown", which
makes OPT-002's bounds-check elimination trustworthy.

**Stdlib changes.** None required. Optionally re-spell magic constants in
`lib/std/math/math.tr`, `random.tr` using the new literal forms — but do not
bundle that into this group.

**Diagnostics changes.** New lexical errors: malformed radix prefix, bad
digit for radix, misplaced separator, malformed exponent. Each needs an
`EXXX` code plus `docs/errors/codes/EXXX.txt`. **Do not** add new
`SLM::ErrorCategory` values — route to the existing `SYNTAX_ERROR`.

**Testing scope.** New folder `tests/35_numeric_literals/`:
- every radix form, positive and negative, with and without `_`;
- exponent forms (`1e6`, `1.5e-3`, `1E+10`);
- `fail_` tests for each malformed form;
- round-trip tests proving the exact value survives (`0xFFFFFFFFFFFFFFF` etc.
  *within* range — out-of-range is SEM-002);
- a test that `0755 == 755`.

**Handbook impact.** `docs/reference/variables_and_types.md` §"Literals" is
rewritten. Handbook ch03 (lexical analysis) and ch12 both need updates.
Record both in `handbook-sync/PENDING.md`.

**Change record** `docs/development/changes/SEM-001.md`.

**Breaking changes.** None intended — every literal valid today stays valid
with the same value, **except** the ones that were silently clamped, which
still compile in this group (SEM-002 turns them into errors).

**Completion criteria.** All new tests pass; full suite green; a 48-digit
literal still compiles (to be rejected in SEM-002) but now carries its exact
value, provable via `--dump-ast`.

**Risks.** Medium-low. The riskiest part is `NumberExprAST`'s shape change,
which ripples into `Lint.cpp`'s `Fingerprint()`, `ASTPrinter`,
`ASTDiff`/`clone()`, `Sandbox`, and `CompileBuffer`. Grep for
`getIntVal()`/`getDoubleVal()` before starting.

**Must NOT reinterpret.** Do not add integer literal *suffixes* (`5i32`).
F-NUM-5 forbids a wider integer family, and `int32` conversion is already
explicit. Do not make a bare leading `0` octal.

---

### OPT-001 — Optimization pipeline, levels, and module passes

**Priority** P1 · **Complexity** M · **Prerequisites** ARCH-001

**Why it must happen here.** Independent of everything else, immediately
measurable, and it changes the baseline that TYPE-002's code-growth is judged
against. Monomorphization without `GlobalDCE` would look far worse than it is.

**Relevant subsystems** driver, LLVM integration.
**Relevant files** `src/main.cpp` (`runCompilerMain` Phase 5,
`emitObjectFile`, CLI parsing), `CMakeLists.txt` (new LLVM components).

**Current behavior (measured).** §1.9. Five function passes on the legacy
pass manager; no module pass manager; empty `main` → 29,968 bytes with 84
unused emitted functions.

**Desired behavior.** A `PassBuilder`-based pipeline with real levels:

| Flag | Pipeline | Intent |
|---|---|---|
| `-O0` | verify only; `AlwaysInliner`; nothing else | fast builds, faithful debugging |
| `-O1` | `SROA`, `mem2reg`, `InstCombine`, `SimplifyCFG`, `EarlyCSE`, `GlobalDCE` + `Internalize` | sane default for iteration |
| `-O2` *(default)* | `PassBuilder::buildPerModuleDefaultPipeline(O2)` + TurfLang's own pre-passes | shipping default |
| `-Os` | `…(Os)` | size |
| `-O3` | `…(O3)` | throughput |

Plus `--target=<triple>`, `-mcpu=<cpu>`, and `CodeGenOptLevel` derived from
`-O` rather than left at default.

**Semantic decisions.** F-OPT-1, F-OPT-2. **The default must remain `-O2`**
so today's users see no performance regression. `-O0` must still produce a
*correct* program — in particular every language-mandated runtime check is
emitted by Codegen, not by an optimization pass, so `-O0` cannot lose one.

**Compiler changes.** Replace `legacy::FunctionPassManager` with
`PassBuilder` + `ModulePassManager`; add `Internalize` (every `turf_*`
function that is not `main` and not `extern`-visible becomes internal, which
is what makes `GlobalDCE` able to delete it); thread the level through to
`emitObjectFile`'s `TargetMachine`.

**Runtime / stdlib / GC / generic changes.** None. *(GC note for later: once
MEM-002 lands, `Internalize`/`GlobalDCE` must not delete a type descriptor
that is only referenced from emitted metadata. Record this in OPT-001's
change record as a forward hazard.)*

**Diagnostics changes.** `-O` with an unknown level is an error, not a
silent ignore.

**Testing scope.** Not `.tr` tests — a new `scripts/opt_levels_smoke.sh`
that compiles a representative set (`showcase/*.tr`, a handful of
`tests/08_e2e_stress/`) at every level and asserts identical program output
and exit codes across all of them. That equality *is* F-OPT-1's test. Also
record binary sizes at each level as a tracked number in the change record.

**Handbook impact.** `book/chapters/ch08_optimizations.tex` is currently an
accurate description of the old pipeline and will be **substantially
invalidated** — the biggest single handbook impact in this whole roadmap.
`docs/reference/cli_and_diagnostics.md` gains the new flags. Flag both
prominently in `handbook-sync/PENDING.md`.

**Change record** `docs/development/changes/OPT-001.md`, including the
before/after binary-size table.

**Breaking changes.** None to language semantics. Binary layout and
performance change.

**Completion criteria.** All five levels build and run the whole test suite
green; the smoke script shows identical output at every level; the empty-main
binary's 84 dead `turf_*` functions are gone at `-O1` and above; `--target`
works for at least one non-host triple.

**Risks.** Medium. `Internalize` is the sharp edge: internalizing something
the linker still needs (`main`, anything referenced by an `extern`
declaration, anything a C library calls back into) breaks the link. Build the
"must-preserve" set explicitly from `g_RequiredLibs`' call sites and
`main`, and test with `showcase/llvm_ffi_selfhost_demo.tr`, which is the most
FFI-dense program in the tree.

**Must NOT reinterpret.** Do not gate bounds checks, division-by-zero checks,
or (later) overflow traps on `-O`. Do not remove the `verifyModule()` call.

---

### TOOL-001 — Debug information (`-g` / DWARF)

**Priority** P1 · **Complexity** M · **Prerequisites** ARCH-001, OPT-001
(needs `-O0` to exist for `-g` to be useful)

**Why it must happen here.** "Can you debug the binary?" is one of §26's
maturity indicators, and the answer is currently no. It also makes MEM-002
(GC) debuggable, which materially de-risks the hardest group in this
roadmap.

**Relevant subsystems** Codegen.
**Relevant files** `src/CodegenCore.cpp` (a new `DIBuilder` alongside
`TheModule`/`Builder`), `src/CodegenFunc.cpp` (subprograms, parameters),
`src/CodegenVar.cpp` (local variables), `src/CodegenStruct.cpp` (composite
types), `src/main.cpp` (flag, module flags, `DIBuilder::finalize()`).

**Current behavior.** No `DIBuilder` anywhere; no `-g`.

**Desired behavior.** `-g` emits DWARF: a compile unit per source file,
`DISubprogram` per function with correct line/scope, `DILocalVariable` +
`llvm.dbg.declare` for locals and parameters, `DICompositeType` for structs
and enums, `DIBasicType` for primitives, and correct `DILocation` on every
instruction. `gdb`/`lldb` can set a breakpoint by TurfLang line, show a
backtrace with TurfLang function names, and print a local.

**Semantic decisions.** None. Debug info must never change behaviour.

**Compiler changes.** Non-trivial but well-trodden. Two TurfLang-specific
wrinkles worth naming up front:
1. **The double-parse pre-pass** (§1.1) emits function bodies during the
   pre-pass, from an AST that is later discarded. Debug metadata must be
   attached there, and the locations must refer to the real file — verify
   `SourceFilename` is correct at that point.
2. **`AllSourceLines`** already keys snippets by filename for imported
   modules; the same filenames must be used for `DIFile` so a stack frame
   inside `lib/std/collections/list.tr` resolves.

**Runtime interactions.** `libturfrt` (RT-001) should be built with `-g` too,
so a panic backtrace crosses the boundary.

**Optimization interactions.** At `-O2` debug info degrades normally (that is
expected and fine). `-O0 -g` is the contract that must be good.

**GC interactions.** Forward-looking: a conservative collector's root
scanning is much easier to debug with frame information available.

**Testing scope.** Not `.tr` tests. A `scripts/debuginfo_smoke.sh` that
compiles a fixture with `-O0 -g`, drives `gdb --batch` with a breakpoint and
a `print`, and greps the output. Gate it on `gdb` being present so CI without
a debugger skips rather than fails.

**Handbook impact.** New material; `docs/reference/cli_and_diagnostics.md`.

**Change record** `docs/development/changes/TOOL-001.md`.

**Breaking changes.** None.

**Completion criteria.** The smoke script passes; `-g` output survives
`llvm-dwarfdump --verify`; full suite green with and without `-g`.

**Risks.** Medium. The classic failure is an instruction with no
`DILocation` inside a function that has a `DISubprogram`, which
`verifyModule()` rejects. Budget time for that specific class of error.

**Must NOT reinterpret.** Do not restructure the pre-pass to make debug info
easier (§25).

---

### DIAG-001 — Structural error classification and the lint VarDecl gap

**Priority** P1 · **Complexity** S · **Prerequisites** ARCH-001

**Why it must happen here.** Small, self-contained, and it removes a
text-matching layer that every later group would otherwise have to feed.

**Relevant subsystems** diagnostics, SLM routing, Lint.
**Relevant files** `include/ErrorCategory.h` (`extractContextKeyword`,
`classifyError`), `include/Errors*.h`, `src/SLMIntegration.cpp`,
`src/Lint.cpp` (`LintExpression`).

**Current behavior.** `SLM::ErrorCategory` is taken from the raising
`TurfError` subclass's `Category_` when it is non-`GENERAL`, and otherwise
re-derived by **matching substrings in the rendered message text**
(`extractContextKeyword` has a long comment chain documenting several bugs
this has caused: `"for enum"` matching the `for` keyword, `"has no method
named"` needing a special case, eight names wrongly routed to
`BUILTIN_FUNCTION`). Separately, `LintExpression()` has no `VarDeclExprAST`
branch, so **every variable declaration's initializer is skipped by every
lint check** (handbook ch09, confirmed).

**Desired behavior.** Every diagnostic that reaches the SLM carries its
category and code from its own class. Text matching is deleted, not extended.
`LintExpression()` recurses into `VarDeclExprAST`'s initializer.

**Semantic decisions.** F-DIAG-3 — this group **does not add** new categories
or new `docs/slm/` files. It makes the existing ones reachable structurally.

**Compiler changes.** Audit every `TurfError` subclass for a correct
`Category_`; audit the raw `LogErrorAt()` call sites (the path with no
`TurfError` instance) and give each one a real error class or an explicit
category argument; delete `extractContextKeyword`'s message scanning once
nothing depends on it.

**Diagnostics changes.** The visible messages should not change. The routing
behind them becomes deterministic.

**Testing scope.** The lint gap gets `.tr` tests:
`tests/14_cfg_analysis/NN_lint_vardecl_div_by_zero.tr` (a `fail_`-style
warning test — check the folder's existing convention for how warnings are
asserted) plus self-comparison and redundant-boolean cases in a declaration
initializer. Classification itself is not `.tr`-testable; assert it by
compiling a corpus with `--smart` diagnostics enabled and checking the
category chosen per code.

**Handbook impact.** Handbook ch09's pitfall becomes historical — it must be
rewritten as "this *was* true and is now fixed", not deleted (§19).
`ch10_diagnostics_and_slm.tex` describes the classification path.

**Change record** `docs/development/changes/DIAG-001.md`.

**Breaking changes.** None user-visible.

**Completion criteria.** `extractContextKeyword`'s message scanning is gone;
every error code maps to a category by class; the lint tests pass; full suite
green.

**Risks.** Low–medium. The risk is silently *losing* a category for some
diagnostic that only ever got one via text matching — enumerate before
deleting.

**Must NOT reinterpret.** Do not add `ErrorCategory` values. Do not expand
`docs/slm/`. That work is deferred and user-initiated.

---

### HYG-001 — Compiler hygiene and diagnostic-rendering consistency

**Priority** P1 · **Complexity** XS · **Prerequisites** ARCH-001

**Why it must happen here.** Two small, independent defects that cost real
build time and real diagnostic quality, and that every later session pays for
if they are not fixed first. Neither is architecture; both are the kind of
thing that never gets done unless it is scheduled.

**Relevant subsystems** build headers, CFG diagnostics.
**Relevant files** `include/SymbolTable.h`, `src/CFG.cpp`
(`reportFlowDiagnostics()`), `include/ErrorsFlow.h`.

**Item 1 — the parser's incidental LLVM dependency (§3.4).**
`SymbolTable.h:6` includes `llvm/IR/Instructions.h` for one field,
`llvm::AllocaInst *Alloca` (`SymbolTable.h:22`), used only as a pointer.
Measured: `src/ParserDecl.cpp` preprocesses to **182,910 lines**, of which
that single header contributes **119,047 (~65%)**, and the TU syntax-checks
in **3.07 s** at `-O2`. Replace the include with
`namespace llvm { class AllocaInst; }` and move the include into
`src/SymbolTable.cpp`. Costs compiler *build* time only — not compile time of
user programs, which is ~0.3 s and healthy.

**Item 2 — unreachable-code and dead-branch warnings bypass
`DiagnosticEngine`.** Verified live: compiling
`fn main(): int { while true { return 1; } return 2; }` prints exactly

```
Warning: Unreachable code in block 'while.end' of function 'main'
```

— a plain, uncolored `std::cerr <<` with **no error code, no source snippet,
no caret, no hint**, sitting directly above a properly-rendered warning from
`lib/std` in the same output. `src/CFG.cpp:381-383` and `396-397` write to
`std::cerr` directly, while dedicated `UnreachableBlockWarning` and
`DeadBranchWarning` classes **already exist, fully written, at
`include/ErrorsFlow.h:58-82` and are never constructed by anything.**
`docs/architecture/compiler_pipeline.md` flags this as *"a real, live gap
between intent and implementation."* Route both through the existing classes.

**Semantic decisions.** None. Item 2 changes only rendering — the same
conditions warn, with the same meaning.

**Runtime / stdlib / GC / generic / optimization changes.** None.

**Diagnostics changes.** The two warnings gain a code, a caret, a snippet and
a hint, and start participating in `DiagnosticEngine`'s one-per-line noise
capping like every other diagnostic. Each needs an `EXXX` entry plus
`docs/errors/codes/EXXX.txt`. **Route to an existing `SLM::ErrorCategory`
(`CONTROL_FLOW`); do not add a new one.**

**Testing scope.** `tests/14_cfg_analysis/` gains a test asserting the
unreachable-code warning is emitted with its code (follow that folder's
existing convention for asserting warnings). Item 1 is covered by the build
succeeding; record the before/after preprocessed-line count in the change
record.

**Handbook impact.** `docs/architecture/compiler_pipeline.md`'s "a real
inconsistency worth documenting plainly" paragraph becomes historical —
rewrite it as fixed rather than deleting it (§19's rule). `book/ch09` if it
covers the same ground.

**Change record** `docs/development/changes/HYG-001.md`.

**Breaking changes.** None.

**Completion criteria.** `llvm/IR/Instructions.h` no longer reaches any
Parser TU; both warnings render like every other diagnostic; full suite
green.

**Risks.** Very low. The only subtlety is that routing through
`DiagnosticEngine` subjects these warnings to the one-primary-error-per-line
cap, which may change output in tests that currently expect both — check
`tests/14_cfg_analysis/` expectations.

**Must NOT reinterpret.** Do not change *when* the warnings fire, only how
they are rendered. Do not add a new `ErrorCategory` (F-DIAG-3).

---

### LANG-004 — Small language and FFI ergonomics

**Priority** P2 · **Complexity** S · **Prerequisites** ARCH-001

**Why it must happen here.** Three verified papercuts that each block a
perfectly ordinary program. All three are cheap, none interacts with a frozen
decision, and leaving them unscheduled means they persist through the whole
overhaul.

**Relevant subsystems** Parser, TypeChecker.
**Relevant files** `src/ParserStmt.cpp` (the `for` range form),
`src/ParserCore.cpp` (top-level declaration switch),
`src/TypeCheckerCall.cpp` (argument matching for `extern` calls).

**Item 1 — `step` is mandatory on a range `for`.** Verified:
`for i in 0..5 { io::printline(i); }` → *"Expected 'step' after the range end
value. Hint: Correct syntax: for i in 0..10 step i++ { }"*.
`DESIGN_NOTES.md` §4 already lists this as an open ergonomics gap. Make the
clause optional, defaulting to `step i++`. **The implementing session must
first confirm the range's inclusivity convention from
`docs/reference/control_flow.md` §"`for` — range form"** so the default
matches the existing explicit-`step` semantics exactly.

**Item 2 — `string` does not coerce to a `ptr` parameter in an FFI call
(§15.8 F3).** Verified: `extern fn puts(s: ptr): int32; puts("hi");` →
*"No overload of 'puts' matches argument types (string)"*. Documented
workaround: declare the parameter as `string`. `string`, `ptr` and `T*` are
already the identical LLVM representation (`isPointerLike()` groups them), so
this is a missing rule in argument matching, not a representation problem.
**Scope it narrowly: `string` → `ptr` at an `extern fn` call site only.** Do
not widen `typesCompatible()` generally — §6.2's conversion matrix is frozen,
and `string` ↔ pointer coercion in ordinary code would undermine it.

**Item 3 — `extern fn` may not be declared inside a function body (§15.8
F4).** Verified: → *"Expected ';' after statement"*. `extern fn` only parses
in the top-level declaration switch. Low value, but it is one switch case.

**Semantic decisions.** Item 2 is the only one that touches type
compatibility, and it is deliberately confined to the `extern` call path.
Record that confinement explicitly in the change record — a future session
must not "generalize" it.

**Runtime / GC / generic / optimization changes.** None.

**Diagnostics changes.** Item 1 removes a diagnostic. Items 2 and 3 remove
diagnostics that were rejecting valid-looking code.

**Testing scope.** `tests/04_control_flow/` — a range `for` with and without
`step`, with the same observable output, plus a `fail_` test for a malformed
`step`. `tests/05_arrays_pointers_ffi/` — a `ptr`-parameter `extern fn`
called with a string literal and with a runtime-built string; a
function-local `extern fn`.

**Handbook impact.** `docs/reference/control_flow.md` §"`for` — range form";
`docs/reference/ffi_and_unsafe.md`; `DESIGN_NOTES.md` §4's three-item list
shrinks to zero and should say so.

**Change record** `docs/development/changes/LANG-004.md`.

**Breaking changes.** None — all three are strictly more permissive.

**Completion criteria.** All three verified failures now compile and run;
full suite green; `DESIGN_NOTES.md` §4's list is closed out.

**Risks.** Low. Item 1's default `step` must match the existing semantics
exactly — an off-by-one in range inclusivity would be a silent behaviour
change in a construct used everywhere.

**Must NOT reinterpret.** Item 2 stays confined to `extern` call sites. Do
not relax `typesCompatible()` generally.

---

### SEM-002 — Numeric semantics: representability, conversion, overflow

**Priority** P0 · **Complexity** M–L · **Prerequisites** SEM-001

**Why it must happen here.** It is the payload of §10; SEM-001 only stopped
the information loss.

**Relevant subsystems** TypeChecker, Codegen, `ValueRangePass`, runtime
(panic).
**Relevant files** `src/TypeCheckerExpr.cpp`, `src/TypeCheckerDecl.cpp`,
`src/TypeCheckerStmt.cpp`, `src/Types.cpp` (`typesCompatible` numeric rules),
`src/CodegenExpr.cpp` (`BinaryExprAST::codegen`), `src/CodegenCore.cpp`
(`CastToType`), `src/ValueRange.cpp`, `src/Builtins.cpp` (`CodegenPanic`).

**Current behavior (verified).** §0.2 items under (4). Additionally: `to`-cast
legality is enforced in **Codegen**, not the TypeChecker
(`docs/reference/variables_and_types.md` says so explicitly).

**Desired behavior.** F-NUM-2 through F-NUM-6, exactly. Concretely:
- Every point where an untyped literal acquires a type (variable
  initialization, assignment, argument passing, return, array element,
  struct field init, enum value, cast target) checks representability and
  errors with the literal's exact value and the destination type.
- `-<literal>` folds before the check, making `INT64_MIN` writable.
- Compile-time arithmetic over untyped literals is exact; only the final
  value is checked.
- Compile-time arithmetic over *typed* constants that provably overflows is
  an error at compile time.
- Runtime signed integer `+ - * <<` and negation use LLVM's
  `llvm.sadd.with.overflow` family (or `nsw` plus an explicit check) and
  branch to a runtime panic on overflow. Division already has its zero check;
  add the one remaining overflow case (`INT64_MIN / -1`).
- `to`-cast legality moves from Codegen into the TypeChecker (Rule B1).

**Semantic decisions.** F-NUM-2/3/4/6.

**The genuinely contentious one, stated plainly:** trapping on signed integer
overflow costs a compare and a branch per arithmetic operation. It is
justified here because (a) TurfLang **already** emits unconditional array
bounds checks and division-by-zero checks, so trapping arithmetic is
*consistent* rather than novel; (b) the alternative (wrapping) makes
`9223372036854775807 + 1` a silent wrong answer, which is exactly the class
of bug this milestone exists to remove; (c) the checks are provably removable
by `ValueRangePass` in the common loop-counter case (OPT-002), so the cost is
recoverable. If the trap turns out to be unacceptable after measurement, the
fallback is **not** wrapping — it is a `-fwrapping-arithmetic` escape hatch
that is off by default and documented as changing semantics, which F-OPT-1
would then have to explicitly exempt. Do not make that call without numbers.

**Runtime changes.** A panic entrypoint. Before RT-001 this is the existing
inline `fprintf`+`abort` path; after RT-001 it becomes
`turf_rt_panic(kind, file, line)`.

**Stdlib changes.** Audit `lib/std/math/math.tr`, `random.tr`, `time.tr`,
`json.tr` for arithmetic that currently relies on wrapping. `random.tr` is
the likely one (LCGs wrap by design) — it will need an explicit
`math::wrapping_mul`-style helper implemented via `unsafe`/intrinsic, or a
redesign. **Find this before starting**; it is the highest-probability
surprise in this group.

**GC interactions.** None directly, but the panic path must be
GC-safe later (a panic during a collection must not re-enter the collector).

**Generic interactions.** After TYPE-002, overflow checks are emitted per
instantiation, so a `List<int32>` gets 32-bit checks. Before TYPE-002,
arithmetic inside a generic body has no concrete type — **this group must not
try to check arithmetic on a `TurfGenericParamType`-typed operand.** Leave
that to TYPE-002.

**Testing scope.** Extend `tests/35_numeric_literals/` and add
`tests/36_numeric_semantics/`:
- `fail_` for each: int literal too large, too small, float literal
  overflowing, `int → int32` implicit, `double → int` implicit, compile-time
  typed overflow.
- `INT64_MIN` literal round-trips.
- Runtime overflow tests using the `// RUNTIME_ABORT_EXPECTED` marker
  convention (AGENTS.md) for `+`, `*`, `<<`, unary `-`, and `INT64_MIN / -1`.
- Explicit-cast truncation tests for every legal pair in the `to` table.
- Regression: every existing arithmetic test in `tests/04_control_flow/`,
  `tests/11_bitwise_ops/`, `tests/27_os_and_math_stdlib/` still passes.

**Handbook impact.** Large. `docs/reference/variables_and_types.md` (casting,
literals), `docs/reference/operators.md` (arithmetic notes), handbook ch12
substantially. Record all three.

**Change record** `docs/development/changes/SEM-002.md`.

**Breaking changes.** **Yes, intentionally.** Programs relying on silent
clamping or wrapping now fail to compile or trap. This is the point.

**Completion criteria.** All of §0.2's verified numeric findings produce a
diagnostic or a trap; full suite green; `lib/std` and `showcase` compile
clean (swept directly, per AGENTS.md).

**Risks.** Medium–high. The two named risks: stdlib code relying on wrapping,
and the performance cost of checked arithmetic (measure at `-O2`; report the
number in the change record).

**Must NOT reinterpret.** Do not add unsigned types to "solve" overflow. Do
not make the overflow behaviour depend on `-O`. Do not weaken the trap into a
warning.

---

### SEM-003 — `guess` (initializer type inference)

**Priority** P1 · **Complexity** M · **Prerequisites** SEM-001, SEM-002

**Why it must happen here.** §11. It is the single most visible ergonomic
change in this roadmap, and it must land **after** SEM-002 so that
`mut x = 999999999999999999999;` produces the right error rather than a
clamp.

**Relevant subsystems** Lexer, Parser, Resolver, TypeChecker.
**Relevant files** `src/Lexer.cpp` (keyword table), `include/Lexer.h`
(new `TOK_GUESS`), `src/ParserStmt.cpp` (`ParseLetStatement`),
`src/ParserCore.cpp` (`ParseType`), `include/ASTDecl.h` (`VarDeclExprAST`),
`src/ResolverDecl.cpp` (`visitVarDecl`), `src/TypeCheckerDecl.cpp`
(`visitVarDecl`), `src/CodegenVar.cpp`.

**Current behavior (verified).** `mut x = 5;` →
*"Expected ':' after variable name."* No inference exists anywhere.

**Desired behavior.** §11's specification in full. Summary: `guess` is a
type-position keyword; omitting the annotation is sugar for it; Model A only;
local declarations only.

**Semantic decisions.** F-GUESS-1/2/3.

**Compiler changes.** The load-bearing design point: **`guess` is resolved in
the TypeChecker, not the Parser.** The parser produces a `VarDeclExprAST`
with a null `Type` (or a sentinel `GuessType`); `TypeChecker::visitVarDecl`
infers from the already-checked initializer and calls the existing
`setTypePtr()`. The Resolver must therefore **not** require a type at
`declareVariable()` time for a `guess` declaration — check
`ResolverScopeTable::declareVariable` and see whether a two-phase
(declare-then-fill) shape is needed. **This is the main design question of
this group; answer it before writing code.**

**Runtime / GC / stdlib changes.** None. Do **not** rewrite `lib/std` to use
`guess` in the same change — that is a separate, purely cosmetic pass and
would make this group's diff unreviewable.

**Generic interactions.** `mut l = List::default();` cannot infer `List<T>`'s
type argument — there is nothing to infer it from. That must be a clear
error pointing at the missing annotation, **not** a silent uninstantiated
generic. After TYPE-002, `mut l = makeList<int>();` (if explicit type
arguments at call sites exist) would infer fine; before then, require the
annotation.

**Diagnostics changes.** New errors: `guess` with no initializer; `guess`
from a `void` expression; `guess` from a bare `null`; `guess` producing an
uninstantiated generic; assignment of an incompatible type to a
`guess`-inferred variable (this one should be the *ordinary* type-mismatch
error, with an added note saying where the type was inferred and from what —
`withRelated()` at the initializer).

**Testing scope.** New folder `tests/37_guess_inference/`:
- `mut x = 5;` → `int`; `= 5.0` → `double`; `= "s"` → `string`;
  `= true` → `bool`; `= 'c'` → `char`; `= [1,2,3]` → `int[3]`;
  `= Point::default()` → `Point`; `= someFn()` → its return type;
  `= fallible()` → `!T` (then `recover`ed).
- Both spellings (`: guess` and omitted) produce identical `--dump-typechecked`
  output.
- `immut x = 5;` works.
- `fail_guess_no_init.tr`, `fail_guess_void.tr`, `fail_guess_null.tr`,
  `fail_guess_reassign_wrong_type.tr` (`mut x = 5; x = 5.5;`),
  `fail_guess_uninstantiated_generic.tr`.
- `fail_guess_literal_out_of_range.tr` — `mut x = 999999999999999999999;`
  (the §20 worked example; this test is the proof that SEM-001→SEM-002→SEM-003
  compose correctly).
- `typeof()` on a `guess` variable returns the inferred name.

**Handbook impact.** Handbook ch12 §"Declaring Variables" currently states
*"The `: Type` annotation is not optional, and there is no inference form."*
That sentence becomes **false**. It is the clearest single example of why
§18's system exists. `docs/reference/variables_and_types.md` §"Variable
declarations" likewise.

**Change record** `docs/development/changes/SEM-003.md`.

**Breaking changes.** None — `guess` becomes a reserved word, so a program
using `guess` as an identifier breaks. Grep `tests/`, `lib/std/`, `showcase/`
first.

**Completion criteria.** All tests pass; both spellings work; the out-of-range
example errors correctly; full suite green; `lib/std`/`showcase` swept.

**Risks.** Medium. The Resolver's declare-before-type ordering is the real
one. A secondary risk is scope creep into parameter/return inference —
F-GUESS-3 forbids it.

**Must NOT reinterpret.** Model B is rejected. Inference never crosses a
declaration boundary. Do not add bidirectional/unification inference — the
two-pass Resolver (§1.6) depends on every signature being knowable without
entering a body, and return-type inference would break that invariant
outright.

---

### SEM-004 — Unified initialization

**Priority** P1 · **Complexity** M · **Prerequisites** TYPE-001, SEM-003

**Why it must happen here.** After `guess`, a reader has to understand three
different things a declaration can mean (`: T = e`, `: T` with Mechanism A,
`: T` with Mechanism B, `= e` with inference). Collapsing Mechanism B is what
keeps that list short.

**Relevant subsystems** Parser, Resolver, `DefiniteAssignmentPass`, Codegen.
**Relevant files** `src/ParserStmt.cpp` (`SynthesizeDefault`,
`ParseLetStatement`), `include/ASTExpr.h` (`DefaultInitExprAST`),
`src/CodegenExpr.cpp`, `src/CodegenCore.cpp` (`GetDefaultValue`),
`src/Analysis.cpp` (`DefiniteAssignmentPass`), `src/CodegenArray.cpp`.

**Current behavior (verified).** Two mechanisms; `SynthesizeDefault()`'s
`isInt/isDouble/isBool/isString` branches are dead on the path that calls it;
`tests/10_default_init/` has 13 files depending on Mechanism B.

**Desired behavior.** F-INIT-1/2. One rule: a declaration without an
initializer leaves the variable *statically unassigned* for every type;
reading it before definite assignment is E008. Storage stays zero-filled.
`T::default()` is the explicit opt-in and already works.

**Semantic decisions.** F-INIT-1, F-INIT-2, F-INIT-3.

**Compiler changes.** Delete the `SynthesizeDefault()` call from
`ParseLetStatement()` and the type-exclusion condition around it;
`DefaultInitExprAST` survives only as the node `T::default()` lowers through
(check whether it is still needed at all). Extend
`DefiniteAssignmentPass`'s tracked set to every body-declared local including
struct-, array- and enum-typed ones. Arrays are the subtle case: today
`ArrayDeclExprAST::codegen()` memsets the whole block at declaration and
`docs/reference/arrays.md` documents `mut arr: int[5];` as a defined all-zero
array. **Decide explicitly and record it:** the recommendation is that a
*whole-array* read before assignment is an error but an *element* write
(`arr[0] = 1`) is an assignment, which means array elements are not
individually tracked — the same granularity `ConstructorFieldAssignmentPass`
already uses for fields.

**Runtime / GC / generic changes.** None. *(GC forward note: under MEM-002, a
zero-filled pointer field is a valid null reference, so the "storage is
always zeroed" guarantee remains compatible with tracing.)*

**Diagnostics changes.** E008's message should name the type and suggest
`T::default()` explicitly for aggregate types, since that is the mechanical
migration.

**Testing scope.** `tests/10_default_init/` is **rewritten**: each of the 13
Mechanism-B tests becomes either a `fail_` test (reading before assignment)
or an explicit-`::default()` test (the behaviour a user actually wanted).
Add coverage for `char`, `int32`, enum, array-of-struct, generic-typed
locals. **U-1 must be answered first**: sweep `lib/std/**` and `showcase/**`
by direct compile for Mechanism-B reliance.

**Handbook impact.** Handbook ch12 §"Default Initialization: Two Different
Behaviors" — an entire section, plus its `pitfall` block and its
`default_init.tr`/`default_init_session.txt` listings — becomes wrong.
`docs/reference/variables_and_types.md` §"Default initialization" and its
default-value table likewise. This is the **second-largest** handbook impact
after OPT-001.

**Change record** `docs/development/changes/SEM-004.md`.

**Breaking changes.** **Yes, intentionally**, and the most user-visible
source-compatibility break in this roadmap. Migration is mechanical:
`mut c: Config;` → `mut c: Config = Config::default();`.

**Completion criteria.** One mechanism; E008 fires uniformly; the rewritten
`tests/10_default_init/` passes; `lib/std` and `showcase` compile clean;
full suite green.

**Risks.** Medium. The volume of migration in `lib/std` is the unknown (U-1).
If the sweep shows it is large, split this group: first make Mechanism B emit
a deprecation warning, migrate, then remove it.

**Must NOT reinterpret.** Do not unify in the other direction (eager defaults
everywhere). That would delete E008, which is one of the compiler's genuinely
good diagnostics, and it would make `mut x: int;` silently `0`.

---
### TYPE-002 — Monomorphization (the keystone)

**Priority** P0 · **Complexity** XL (needs its own sub-plan; see §22 for the
session split) · **Prerequisites** TYPE-001, SEM-001; strongly benefits from
OPT-001

**Why it must happen here.** It is the precondition for the memory model
(§0.3), it removes an ICE, and it fixes `docs/planning/LIMITATIONS.md` items
1 and 2 as a by-product without adding any language surface.

**Relevant subsystems** Resolver, TypeChecker, a **new** monomorphizer pass,
Codegen, symbol tables, `main.cpp` driver.
**Relevant files** `src/ResolverCore.cpp` (`substituteTypeParams`,
`buildFunctionSymbol`, `hoistTopLevel`), `src/ResolverDecl.cpp`
(`visitFuncDef`, `visitStructDecl`, `validateNamedType`),
`src/TypeCheckerCall.cpp` (`collectGenericSubstitution`, overload
resolution), `src/Types.cpp` (`typesCompatible` Rules 1/3/4),
`include/Types.h` (`TurfGenericParamType`, `GenericType`),
`src/CodegenCore.cpp` (`getLLVMType`'s erasure branch at ~352,
`CastToType`'s erase/unerase branches at ~1177 and ~1208, `LLVMTypeCache`),
`src/CodegenFunc.cpp` (mangling, the ~1013 un-erase cast at call sites),
`src/CodegenStruct.cpp` (`FieldAccessExprAST::codegenLValue`'s
`GenericType`→base-name resolution at ~714), `include/ASTDiff.h`
(`CloneExprAST` — the enabling machinery), `src/main.cpp` (pre-pass
ordering).

**Current behavior.** §1.5, §1.8, §9.1. One `TurfStructType` per generic
declaration regardless of instantiation; every `T` is one opaque pointer;
scalars `IntToPtr`-stuffed; `List<int>` and `List<string>` are the same
compiled code; >8-byte structs ICE; `operator==` never resolves inside a
generic body; `sizeof(T)` does not exist; `list.tr` hardcodes `cap * 8`.

**Desired behavior.** §9.3 in full.

**Semantic decisions.** F-GEN-1, F-GEN-2, F-GEN-3.

**Compiler changes.** A new pass between TypeChecker and Codegen:

```
Lexer → Parser → Resolver(2) → TypeChecker → MONOMORPHIZER → Codegen
```

The monomorphizer:
1. Walks the type-checked AST collecting **instantiation requests** — every
   call to a generic function/method and every use of a `GenericType`
   already carries a substitution map built by
   `collectGenericSubstitution()` (`src/TypeCheckerCall.cpp`); that is the
   input, not something to rebuild.
2. Maintains a worklist and a cache keyed by (declaration identity, ordered
   concrete type-argument tuple).
3. For each cache miss: `clone()` the declaration's AST
   (`ASTDiff.h`'s `CloneExprAST` — already exists and is already exercised
   by the SLM sandbox), substitute concrete types for every
   `TurfGenericParamType`, **re-run the Resolver and TypeChecker over the
   clone**, register it under a mangled name that includes the type
   arguments, and append any new instantiation requests its body produced.
4. Runs to a fixed point, with a depth cap (default 64) producing
   `GenericInstantiationDepthError` naming the chain.
5. Asserts that no `TurfGenericParamType` survives into any node handed to
   Codegen.

Then Codegen's erasure branches are **deleted**: `getLLVMType`'s
`TurfGenericParamType`→pointer case, `CastToType`'s
`IntToPtr`/`PtrToInt` erase/unerase pair, `CodegenFunc`'s per-call-site
un-erase cast, and `FieldAccessExprAST::codegenLValue`'s
`GenericType`→base-name collapse (each instantiation now has its own real
`TurfStructType`).

`typesCompatible()` Rule 1 stays, but its scope narrows: it is a
*candidate-matching* rule used before substitution, and nothing after
substitution may rely on it.

**The open design question to resolve first (U-4).** `main.cpp` runs a
pre-pass that codegens struct declarations, then prototypes, then bodies,
from a throwaway AST, before re-parsing for the authoritative pass. The
monomorphizer needs *both* the full program's instantiation set *and* to run
before any Codegen. Two options: (a) run monomorphization only in the
authoritative pass and have the pre-pass skip generic declarations entirely
(prototypes for instantiations are emitted from the monomorphizer's own
output); (b) run it in both. Option (a) is almost certainly right and is
smaller, but it must be confirmed against why the pre-pass exists (GEP
layout completeness) before any code is written.

**Runtime changes.** None in this group — but this is what makes RT-001's
type descriptors possible.

**Standard-library changes.** Substantial and mostly *deletions*:
- `lib/std/collections/list.tr`'s `cap * 8` becomes `cap * sizeof(T)`.
  `sizeof(T)` now works inside a generic body, per instantiation.
- The `List<T*>` workaround for >8-byte structs is no longer needed anywhere
  it appears (`docs/reference/stdlib/collections.md` documents it).
- `list_sort`/`list_binary_search`/`list_find` — concrete `List<int>`-only
  free functions that exist *solely* because of the erasure gap
  (`LIMITATIONS.md` §2) — can become real generic methods.
- `Map<K,V>`/`Set<T>` key comparison starts dispatching to
  `operator==(string,string)` correctly, fixing `LIMITATIONS.md` §1 and
  `tests/99_known_bugs/set_string_dedup_broken.tr`.
- `http.tr`'s `Request.get_header()`/`Server.route_index()` non-generic
  workarounds can be simplified (but doing so is optional and should not be
  bundled here).

Do **not** attempt all of that in the same change. Land monomorphization
first with the stdlib unchanged, then a follow-up group for the cleanups.

**Diagnostics changes.** Three new classes:
- `GenericInstantiationDepthError` — naming the chain, like a C++ template
  backtrace.
- **Instantiation-site errors**: when a monomorphized body fails to
  type-check, the error is reported at the *instantiation site* with a
  `withRelated()` note at the generic declaration and one at the offending
  line inside the template. This is the mechanism that gives the standard
  library real compile-time diagnostics (§15.6) — it is the single largest
  diagnostic improvement in this roadmap and it costs nothing extra.
- A diagnostic replacing the `bitcast %Big to ptr` ICE if any erasure path
  survives during the transition.

**Optimization interactions.** Code size grows with instantiation count.
`Internalize` + `GlobalDCE` (OPT-001) removes instantiations that end up
unused; per-instantiation constant propagation and inlining recover
performance the erased version could never get. **Measure both** (U-2) and
put the numbers in the change record.

**GC interactions.** This is the enabling dependency. After this group, a
`List<int>` and a `List<string>` are distinct types with distinct layouts, so
the compiler can emit a per-type pointer bitmap (RT-001) and the collector
can trace precisely. Equally important: the `IntToPtr` bit-stuffing that
would have manufactured fake pointers in the heap is **gone**.

**Testing scope.** This group needs the most test work in the roadmap.
- **Regression**: every existing generic test must still pass —
  `tests/07_functions_modules/` (123 files, the generics-heaviest folder),
  `tests/23_stdlib_primitives/`, `tests/32_set_and_sort_stdlib/`.
- New `tests/38_monomorphization/`:
  - `List<Big>` with a 24-byte struct — the exact program that ICEs today.
  - `List<int>`, `List<double>`, `List<string>`, `List<Point>`,
    `List<Point*>` in one program; assert independent behaviour.
  - `Map<string,int>` content equality (the `LIMITATIONS.md` §1 repro, using
    a **runtime-built** string, never a literal — literals can be linker-
    merged and give a false pass; `LIMITATIONS.md` warns about exactly this).
  - `tests/99_known_bugs/set_string_dedup_broken.tr` **moves** into the live
    suite and must pass.
  - `sizeof(T)` inside a generic body.
  - Generic methods on generic structs; nested generics (`List<List<int>>`);
    a generic function generic in only some arguments
    (`fn<V> f(m: Map<string,V>)` — `tests/07_functions_modules/generic_partial_type_arg_match.tr`).
  - `fail_generic_recursion_depth.tr` — an unbounded instantiation chain.
  - `fail_generic_requires_operator.tr` — a `T` lacking an operation the body
    needs; assert the error points at the **instantiation site**.
- Binary-size and compile-time measurements recorded, not asserted.

**Handbook impact.** The largest in the roadbook after OPT-001.
`docs/architecture/compiler_pipeline.md` §7 ("Generic type erasure") is
**entirely** about the old model and must be rewritten.
`docs/architecture/memory_model.md` §2.7 likewise. `docs/planning/LIMITATIONS.md`
items 1–3 are resolved. `docs/reference/generics_and_operators.md` and
`docs/reference/stdlib/collections.md` change. Handbook chapters 05, 06, 07
all discuss erasure. Record every one.

**Change record** `docs/development/changes/TYPE-002.md` — this record should
be the longest in the repository.

**Breaking changes.** Language-level: none intended (a program that compiles
today should compile and behave identically). Implementation-level:
everything. `List<T*>`-workaround code still works, so the migration is
opt-in.

**Completion criteria.**
1. No `TurfGenericParamType` reaches Codegen (assertion, enabled in debug
   builds and never removed).
2. The 24-byte-struct `List<Big>` program compiles, runs, and prints the
   right values.
3. `set_string_dedup_broken.tr` passes and has moved out of
   `tests/99_known_bugs/`.
4. `sizeof(T)` works inside a generic body.
5. Full suite green; `lib/std` and `showcase` swept directly.
6. Compile-time and binary-size deltas measured and recorded.

**Risks.** **High** — the highest in this roadmap. Named risks:
- *Re-resolution of cloned ASTs.* The clone must be re-run through Resolver
  and TypeChecker in a scope that sees the right symbols. `clone()`'s own
  doc comments say resolved fields are deliberately **not** copied and cloned
  ASTs "must be re-resolved" — good, that is the intended contract, but it
  means the monomorphizer must set up scope correctly.
- *The double-parse pre-pass* (U-4).
- *Instantiation explosion* in the stdlib (U-2). Measure early.
- *Diagnostic quality* — a monomorphization error with no instantiation
  backtrace is worse than today's behaviour. Build the backtrace from the
  start, not as a follow-up.

**Must NOT reinterpret.** Do not keep a "hybrid" erasure path "for
compatibility" — a second representation is exactly the ambiguity this
milestone removes. Do not add a trait/constraint system (F-GEN-3). Do not
change the existing mangling *scheme*; extend it.

---

### LANG-001 — Operator system completion

**Priority** P2 · **Complexity** M · **Prerequisites** TYPE-002

**Why it must happen here.** Adding `operator[]` before monomorphization
would immediately hit the same "no candidate matches a `K`" wall that
`LIMITATIONS.md` §1 documents, and the fix would be the wrong one (a trait
system). After TYPE-002 it simply works.

**Relevant subsystems** Lexer, Parser, Resolver, TypeChecker, Codegen.
**Relevant files** `include/ResolverSymbolBase.h`
(`isOverloadableBinaryOp`, `operatorFunctionName`, `sanitizeSymbolName`),
`src/ParserDecl.cpp` (`ParseOperatorDecl`), `src/ResolverCore.cpp`
(`validateOperatorDecl`), `src/TypeCheckerExpr.cpp` (`visitBinary`'s
resolution at ~207-240), `src/CodegenExpr.cpp` (`BinaryExprAST::codegen`'s
`ResolvedOperatorTarget` dispatch), `src/CodegenArray.cpp`
(`ArrayAccessExprAST` — where `operator[]` has to hook in).

**Current behavior (verified).** Exactly ten binary operators are
overloadable (`+ - * / == != < > <= >=`). Unary is a dedicated "not supported
yet" message. No `%`, `**`, bitwise, `[]`, or compound. `validateOperatorDecl`
rejects a declaration only when **both** operands are in
`{int,int32,double,bool,char,pointer}` — `string` is deliberately absent from
that set, which is what allows `strings.tr`'s overloads.

**Desired behavior.** F-OP-1 through F-OP-4, plus:
- Binary set extended with `% ** & | ^ << >>`.
- Unary `-`, `!`, `~` overloadable (arity 1 distinguishes them).
- `operator[](self_ty, index_ty): T` and
  `operator[]=(self_ty, index_ty, value_ty): void`.
- Compound assignment **desugars** (`a += b` ≡ `a = a + b`), so it picks up
  `operator+` automatically and there is no `operator+=`.
- `validateOperatorDecl` generalizes: at least one operand type must be
  declared in the same module as the operator (an orphan rule), *and* the
  existing all-primitive rejection is kept.

**Semantic decisions.** F-OP-1 (exact match — **do not** relax this to
`typesCompatible()`; it is the single most important existing invariant in
the operator system and relaxing it would make `operator+(string,string)`
reachable by widened operands), F-OP-2, F-OP-3, F-OP-4.

**Compiler changes.** Mostly table extension, plus:
- `operatorFunctionName()` gains the new tokens and unary spellings
  (`operator-unary` vs `operator-` must be distinguishable — the recommendation
  is to key by (spelling, arity) rather than inventing a second name).
- `sanitizeSymbolName()` gains entries for the new operators (`%` → `mod`,
  `<<` → `shl`, `[]` → `index`, …) — the linker cannot accept those
  characters in a symbol.
- `ArrayAccessExprAST` must consult an `operator[]` candidate when the base
  is not a real array/pointer/string, and fall through to the built-in path
  otherwise.
- Prefix `++`/`--` currently desugar to `x = x + 1`, so they inherit
  `operator+` for free. Postfix does not desugar; decide and record whether
  it should.

**Runtime / GC changes.** None.

**Generic interactions.** The payoff: inside a monomorphized `Map<string,int>`,
`self.keys[i] == key` has operands of concrete type `string`, so
`operator==(string,string)` resolves by exact match. `LIMITATIONS.md` §1/§2
are closed by TYPE-002 + this group together.

**Stdlib changes.** `List<T>` gains `operator[]`/`operator[]=` (closing
`LIMITATIONS.md` §4's `get`/`set` gap in a better form). `Map`/`Set` likewise.
Keep the public `data` field for now to avoid a second breaking change.

**Diagnostics changes.** The "unary operator overloading is not supported
yet" message is deleted. New: orphan-rule violation; ambiguous operator
overload (the existing duplicate-candidate path already handles this —
verify).

**Testing scope.** `tests/18_operator_overload/` expands substantially:
one test per newly overloadable operator, on a user struct; `operator[]` read
and write; compound assignment reaching `operator+`; unary `-`/`!`/`~`;
`fail_` tests for the all-primitive rule, the orphan rule, `&&`/`||`
overload attempts, and an `operator=` attempt. Plus a test proving exact-match
is still exact (an `operator+(Vec,Vec)` must **not** be reached by an `int`
operand that would widen).

**Handbook impact.** `docs/reference/operators.md` (the overloadable set),
`docs/reference/generics_and_operators.md`. Handbook ch12's operator section.

**Change record** `docs/development/changes/LANG-001.md`.

**Breaking changes.** None (purely additive), unless the orphan rule rejects
an existing declaration — check `lib/std` first.

**Completion criteria.** All new operators overloadable; `List<T>[i]` works;
`Map<string,V>` content equality passes with runtime-built strings; exact-match
regression test passes; full suite green.

**Risks.** Low–medium. The `operator[]` hook into `ArrayAccessExprAST` is the
fiddly part because that node already handles arrays, pointers and strings.

**Must NOT reinterpret.** F-OP-1 (exact match) and F-OP-4 (the
never-overloadable list). Do not add `operator+=`.

---

### LANG-002 — Nullable references `&?T` and declared capabilities

**Priority** P1 · **Complexity** M–L · **Prerequisites** TYPE-001 (helpful),
independent of TYPE-002

**Why it must happen here.** It closes the most visible *language*
incompleteness found in this audit: **user code cannot write a linked list**.
Verified: `struct Node { next: Node* }` requires a top-level `unsafe { }`,
and `fn walk(n: Node*)` is rejected even inside `unsafe` — only `lib/std`,
matched by file path, may take or return a raw pointer. It also replaces
`IsPathUnderLibStd` with something auditable (C-6).

**Relevant subsystems** Lexer, Parser, type system, Resolver, TypeChecker,
a new `NullabilityPass`, Codegen.
**Relevant files** `include/Types.h` (a new `TurfNullableReferenceType`),
`src/Types.cpp` (`typesCompatible` pointer rules), `src/ParserCore.cpp`
(`ParseType`), `src/TypeCheckerCore.cpp` (`isUnsafeGateExempt`,
`checkUnsafeGate`, `checkNoRawPointersInSignature`), `include/Analysis.h` +
`src/Analysis.cpp` (the new pass), `src/CFG.cpp` (branch condition blocks —
already built, which is what makes narrowing possible), `src/CodegenCore.cpp`
(`getLLVMType` — `&?T` lowers to the same opaque pointer, no new
representation).

**Current behavior (verified).** §1.7, §1.11. `&T` cannot be null so it
cannot terminate a list; `T*` can, but is unusable in any user signature.

**Desired behavior.**
1. A new type `&?T` — a nullable, compiler-checked reference. Same runtime
   representation as `&T` and `T*` (an opaque pointer), zero Codegen cost.
2. `null` is assignable to `&?T` and to nothing else safe.
3. Dereferencing or field-accessing a `&?T` is an error **unless** a
   `NullabilityPass` has narrowed it to non-null on that path. Narrowing
   sources: `if (p != null) { … }` / `if (p == null) { return; }` … , and the
   existing CFG already places condition expressions on their own blocks
   (`src/CFG.cpp:600+`), which is precisely what this pass needs.
4. `&T` → `&?T` implicitly; `&?T` → `&T` only after narrowing; `&?T` → `T*`
   implicitly (widening to raw is always sound); nothing goes back without
   `unsafe`.
5. `IsPathUnderLibStd` is deleted. In its place, a module-level declaration
   in source — e.g. a first-line `unsafe module;` — grants the raw-pointer
   capability. The compiler honours it for files under the resolved install
   root (`TurfInstallRoot()`, the same anchor imports already use)
   unconditionally, and for any other file only with an explicit
   `--allow-unsafe-modules` flag. The privilege becomes visible in the file
   that uses it and greppable across the tree.

**Semantic decisions.** F-PTR-1, F-STD-1.

**Compiler changes.** The `NullabilityPass` is the substantial part. It is a
forward dataflow analysis over the existing CFG with a three-state lattice
(`NonNull` / `Nullable` / `Null`), meet = join-to-`Nullable` on disagreement
— structurally the same shape as `DefiniteAssignmentPass` and
`AliasAnalysisPass`, both of which already exist and can be copied.
`DESIGN_NOTES.md` §1 already specifies this pass; this group implements it.

**Runtime / GC changes.** None now. Under MEM-002, `&?T` and `&T` are both
traced as ordinary heap references; `T*` is not traced.

**Stdlib changes.** `lib/std` files gain the explicit capability declaration.
Opportunistically, data-structure-shaped stdlib code can migrate `T*` →
`&?T` — but do **not** bundle that migration into this group.

**Diagnostics changes.** New: "possible null dereference of `&?T`" with a
note at the last point it was known nullable, and a hint naming the null
check that would fix it. Plus the errors for the capability system.

**Also in scope (folded in from §15.8):**
- **F1** — a module that declares the raw-pointer capability may write
  `ptr`-taking functions, which is what a **C callback** requires. Today
  `fn cmpints(a: ptr, b: ptr): int32` is rejected even inside `unsafe { }`,
  so no user can write a `qsort` comparator, a signal handler, or any other
  C callback. This is the same root cause as the linked-list gap.
- **F2** — a parser bug: a *second* `extern fn` inside a top-level
  `unsafe { }` block fails with *"Expected ';' after statement"*. Verified;
  two at top level outside `unsafe` are fine.

*(§15.8's remaining two FFI gaps, **F3** `string`→`ptr` coercion and **F4**
function-local `extern fn`, are scheduled separately in **LANG-004** — they
are ergonomics, not capability, and do not depend on this group.)*

**Testing scope.** New `tests/39_nullable_references/`:
- a **linked list written entirely in safe TurfLang**, with `push`, `pop`,
  traversal and a `fn` taking `&?Node` — this test is the proof that the gap
  is closed.
- in `tests/05_arrays_pointers_ffi/`: a **real `qsort` round-trip** with a
  TurfLang comparator, and two `extern fn`s inside one top-level `unsafe { }`
  block.
- a binary tree.
- narrowing through `if`, `else`, early return, `while` condition, `&&` chains.
- `fail_` tests: deref without a check; `&?T` → `&T` without narrowing;
  assigning `null` to `&T`; a raw `T*` signature in a non-capability module;
  a capability declaration in a user module without the flag.

**Handbook impact.** `docs/architecture/pointers_and_references.md` gains a
whole type (Part 1 §1.1's "three pointer-shaped types" becomes four).
`docs/reference/structs_and_enums.md` §"Self-referential structs" —
currently documents the `unsafe { }` wrapping requirement as the way to do
it — is superseded. `docs/reference/ffi_and_unsafe.md`. `AGENTS.md`'s
"no compiler special-casing" claim becomes true again.

**Change record** `docs/development/changes/LANG-002.md`.

**Breaking changes.** `lib/std` files must declare their capability (a
one-line addition per file). No user-code break; `unsafe { struct … Node* }`
keeps working.

**Completion criteria.** The safe linked-list test compiles and runs;
`IsPathUnderLibStd` no longer exists; full suite green.

**Risks.** Medium. The `NullabilityPass`'s precision is the risk: too
conservative and it rejects reasonable code (a check in one function, a use in
another). Start deliberately local (intraprocedural, narrowing from direct
comparisons only) and document the limits rather than over-engineering.

**Must NOT reinterpret.** `&T` stays non-null — do not merge the two types.
`self` stays a raw `StructName*` internally and stays exempt. Raw pointers
remain legal for FFI; this group narrows *who* may use them, not whether they
exist.

---

### LANG-003 — `defer`

**Priority** P2 · **Complexity** S–M · **Prerequisites** none; **required
before** MEM-002

**Why it must happen here.** F-MEM-2 says there will be no finalizers. That
is only defensible if there is a deterministic cleanup mechanism for OS
resources — file descriptors, sockets, manual allocations — which a garbage
collector must not and will not manage.

**Relevant subsystems** Lexer, Parser, AST, CFG, Codegen.
**Relevant files** `include/Lexer.h` (`TOK_DEFER`), `src/Lexer.cpp`,
`src/ParserStmt.cpp`, `include/ASTStmt.h` (a new `DeferExprAST`),
`src/CFG.cpp` (every scope exit edge), `src/CodegenControlFlow.cpp`
(`return`, `break`, `continue`), `src/CodegenFunc.cpp` (function exit).

**Current behavior.** No scope hook of any kind exists. Nothing runs on scope
exit.

**Desired behavior.** `defer <statement>;` registers the statement to run
when the enclosing **block** exits, by any path (fallthrough, `return`,
`break`, `continue`), in reverse registration order. Block-scoped (not
function-scoped), which is simpler to reason about and simpler to lower.

**Semantic decisions.**
- Reverse (LIFO) order within a block.
- Runs on every exit edge including early ones.
- A `defer` inside a loop body runs per iteration.
- A deferred statement may not `return`, `break` or `continue` out of itself.
- A panic does **not** run deferred statements (TurfLang has no unwinding;
  a panic aborts). Document this explicitly — it is the main semantic
  difference from Go's `defer` and a reader will assume otherwise.

**Compiler changes.** Purely a lowering problem: at every block-exit edge,
emit the block's pending deferred statements in reverse. The CFG already
models every exit edge explicitly, so the information is available. The
subtlety is nesting: a `return` from inside three nested blocks must run all
three levels' defers, innermost first.

**Runtime / GC changes.** None. This is the mechanism `mem::free`,
`io::close` and socket cleanup use.

**Testing scope.** New `tests/40_defer/`: ordering; every exit path;
nesting; in a loop; interaction with `recover`; `fail_` for `return` inside a
deferred statement.

**Handbook impact.** New material in `docs/reference/control_flow.md`.

**Change record** `docs/development/changes/LANG-003.md`.

**Breaking changes.** `defer` becomes a keyword — grep first.

**Completion criteria.** All ordering and exit-path tests pass; full suite
green.

**Risks.** Low–medium. The nested-scope lowering is the only real one.

**Must NOT reinterpret.** Not function-scoped. Not run on panic. Do not turn
this into RAII/destructors — F-MEM-2 is explicit that automatic destructors
are not part of this design.

---

### RT-001 — `libturfrt`: the runtime library

**Priority** P0 (for the memory track) · **Complexity** L ·
**Prerequisites** TYPE-002

**Why it must happen here.** TurfLang has no runtime (§1.10). Everything in
the memory track needs one, and type descriptors cannot be emitted for an
erased type — hence the TYPE-002 dependency.

**Relevant subsystems** a new `runtime/` directory, Codegen, driver, build.
**Relevant files** new `runtime/turfrt.h`, `runtime/turfrt_alloc.c`,
`runtime/turfrt_panic.c`, `runtime/turfrt_types.c`; `CMakeLists.txt`;
`src/main.cpp` (`linkObject` — add `-lturfrt`); `src/Builtins.cpp`
(`CodegenPanic` rewritten to call the runtime); `src/CodegenStruct.cpp`
(descriptor emission); new `include/Intrinsics.h`.

**Current behavior.** No runtime. Panics inline `fprintf`+`abort`. Allocation
is libc `malloc` via `extern fn` from `lib/std/mem/mem.tr`. Linker line is
`cc obj -o out -lm`.

**Desired behavior.**
1. A C library `libturfrt`, built by the same CMake project, linked into
   every binary, versioned lock-step with the compiler.
2. It provides, at minimum:
   - `void *turf_rt_alloc(size_t bytes, const TurfTypeDesc *desc)` — the
     typed allocation entrypoint. In this group it is a thin `malloc`
     wrapper that records nothing; MEM-002 gives it a real heap.
   - `void turf_rt_panic(const char *kind, const char *msg, const char *file,
     int line)` — replacing every inline panic.
   - process bootstrap (`turf_rt_init` / `turf_rt_shutdown`), called from a
     compiler-emitted wrapper around `main`.
   - the out-of-memory policy, in one place.
3. **Type descriptors.** For every monomorphized aggregate the compiler emits
   a constant `TurfTypeDesc { size, name, pointer_word_bitmap }` into the
   module, and every `turf_rt_alloc` call site passes the right one. In this
   group nothing reads the bitmap; MEM-002 does. Emitting it now, while
   TYPE-002's changes are fresh, is far cheaper than retrofitting it.
4. `include/Intrinsics.h` — the closed, enumerated intrinsic list (F-STD-2),
   naming every compiler-known symbol: `sizeof`, `typeof`, `caller_location`,
   the checked-arithmetic builtins from SEM-002, and the `turf_rt_*` set.

**Semantic decisions.** F-MEM-4, F-STD-2.

**Compiler changes.** Descriptor emission; panic lowering rerouted; the
`main` wrapper; `linkObject` gains `-lturfrt` and the library search path
anchored the same way `TurfInstallRoot()` anchors `lib/std`.

**Standard-library changes.** `lib/std/mem/mem.tr`'s `malloc`/`free`
bindings stay exactly as they are — F-MEM-3 keeps the manual heap separate
and untouched. Nothing in `lib/std` should be rerouted to `turf_rt_alloc` in
this group; MEM-001 does that.

**Optimization interactions.** Forward hazard recorded in OPT-001:
`Internalize`/`GlobalDCE` must not delete a descriptor referenced only from
metadata. Mark descriptors `used` or reference them from the allocation call
site (the latter is simpler and is what the design does).

**Diagnostics changes.** OOM and panic messages get a consistent format and a
documented exit code.

**Testing scope.** Mostly not `.tr`. Add
`scripts/runtime_smoke.sh` verifying the runtime links, `turf_rt_init` runs,
a panic produces the documented message and exit code, and a descriptor's
bitmap is correct for a handful of representative types (dump it with
`--emit-llvm` and check). One `.tr` test with `// RUNTIME_ABORT_EXPECTED`
confirming the new panic path still aborts.

**Handbook impact.** Entirely new material — there is no runtime chapter
because there was no runtime. `docs/architecture/` should gain
`runtime.md`.

**Change record** `docs/development/changes/RT-001.md`.

**Breaking changes.** Build-system and distribution: a TurfLang installation
now ships a library, not just a binary. `TURF_HOME`'s layout gains `lib/`.

**Completion criteria.** Every binary links `libturfrt`; every panic goes
through it; descriptors are emitted and verifiably correct; full suite green;
`showcase/` programs still run.

**Risks.** Medium. Distribution and the CI packaging action
(`.github/actions/package-release`) both change. Cross-compilation
(`--target`, OPT-001) now needs a runtime built for the target — scope that
explicitly or declare cross-compilation unsupported until it is.

**Must NOT reinterpret.** The runtime is C, not TurfLang. It is not a place
to move stdlib functionality into (F-STD-3, Rule B3). Its API surface is the
intrinsic list, and adding to that list is an explicit act.

---

### MEM-001 — Route managed allocations through the runtime

**Priority** P1 · **Complexity** M · **Prerequisites** RT-001

**Why it must happen here.** A collector can only collect what it allocated.
Doing this as its own group means the (large, mechanical) rerouting is
reviewed separately from the (subtle, risky) collector.

**Relevant subsystems** stdlib, Codegen.
**Relevant files** `lib/std/core/strings.tr` (every buffer-returning
function), `lib/std/collections/{list,map,set,stack,queue}.tr`,
`lib/std/json/json.tr`, `lib/std/http/http.tr`, `lib/std/io/net.tr`.

**Current behavior.** Every managed allocation is a direct `malloc` from
TurfLang source. `strings.tr:50-53` documents the leak as a deliberate
`TURF-MEMLEAK` tradeoff.

**Desired behavior.** Every allocation that produces a value the *language*
is responsible for — string results, collection backing buffers — goes
through `turf_rt_alloc` with the correct type descriptor. Every allocation
the *programmer* is responsible for (`mem::alloc`, and anything they pair
with `mem::free`) keeps using `malloc`, untraced (F-MEM-3).

**Semantic decisions.** F-MEM-3. The line between the two heaps is drawn here
and must be stated in the change record explicitly, function by function.

**Compiler / stdlib changes.** Mostly stdlib edits, plus whatever intrinsic
surface is needed so a `.tr` file can request a typed allocation
(`@gc_alloc(T, n)` or equivalent, from the closed list).

**GC interactions.** After this group, everything a collector would need to
manage is on the GC heap, but nothing collects yet. Programs still leak —
identically to today. That is the point: this group is behaviour-preserving
and therefore reviewable.

**Testing scope.** No behaviour change, so the existing suite is the test.
Add an allocation-accounting hook (`turf_rt_stats()`) and a test asserting
that a program's managed allocation count is what it should be — that becomes
MEM-002's regression harness.

**Handbook impact.** `docs/architecture/memory_model.md` Part 1 §1.4/§1.7 and
Part 2 §2.4/§2.6.

**Change record** `docs/development/changes/MEM-001.md`.

**Breaking changes.** None observable.

**Completion criteria.** Full suite green with identical behaviour;
`turf_rt_stats()` accounts for every managed allocation; `mem::alloc` still
returns untraced memory and `mem::free` still works on it.

**Risks.** Low–medium. The risk is mis-classifying an allocation — putting a
programmer-owned buffer on the GC heap (where a later `mem::free` would be a
double-free against the collector) or vice versa. Enumerate every `malloc`
call site in `lib/std` and classify it explicitly in the change record.

**Must NOT reinterpret.** The two heaps do not merge. `mem::free` on a GC
pointer must be a diagnosable error, not undefined.

---

### MEM-002 — The collector

**Priority** P1 · **Complexity** XL · **Prerequisites** TYPE-002, RT-001,
MEM-001, LANG-003, ideally TOOL-001

**Why it must happen here.** Last, because every one of its prerequisites is
load-bearing (§7) and because it is the single hardest and least reversible
piece of work in this roadmap.

**Relevant subsystems** `libturfrt` only, plus a small amount of Codegen for
global roots.
**Relevant files** `runtime/turfrt_gc.c` (new), `runtime/turfrt_alloc.c`,
`src/CodegenCore.cpp` (global-root registration).

**Current behavior.** Nothing is collected, ever.

**Desired behavior.** §14 in full: non-moving mark-sweep, precise heap
tracing via descriptors, conservative stack and register root scanning,
stop-the-world, single-threaded, no finalizers.

**Semantic decisions.** F-MEM-1, F-MEM-2, F-MEM-3.

**Compiler changes.** Minimal, by design — which is the whole point of
choosing conservative root scanning. Global/static roots must be registered
(`turf_rt_add_global_root`) at startup. No safepoints, no stack maps, no
barriers.

**Runtime changes.** All of them. §14.4–§14.8.

**Optimization interactions.** The one real interaction: an optimizer may
keep the only live reference to an object exclusively in a callee-saved
register or spill slot. Conservative scanning of the whole stack **and**
the register file (via `setjmp` to spill registers, the standard Boehm
technique) covers this. Interior pointers must retain their containing object
(`List<T>.data` points into a heap block; a loop induction variable may point
mid-buffer), so the heap must support "find the object containing this
address" — a page/block table.

**Generic interactions.** None post-TYPE-002. Pre-TYPE-002 it would be fatal:
`IntToPtr` manufactures integers that look like heap addresses, causing both
false retention and (worse) a traced "pointer" that is actually a `double`'s
bit pattern.

**Testing scope.** The hardest testing problem in the roadmap.
- `turf_rt_stats()`-based tests: allocate N, drop all references, force a
  collection, assert live count.
- A retention test: keep a reference only in a local; force collection; use
  the object afterwards and assert its contents (this is U-3's answer).
- An interior-pointer retention test (hold only `list.data + k`).
- An FFI-crossing test: pass a GC string to `strlen`/`write` around a
  collection.
- A global-root test.
- A stress test in `tests/08_e2e_stress/` allocating in a loop and asserting
  bounded peak RSS — the observable proof that leaks are gone.
- Every existing test must pass unchanged.

**Handbook impact.** `docs/architecture/memory_model.md` is largely rewritten
— it is currently an accurate description of a language with no memory
management. Its one-sentence summary (*"there is no automatic memory
management of any kind today"*) becomes false. Also
`docs/reference/stdlib/mem.md`, `stdlib/collections.md`.

**Change record** `docs/development/changes/MEM-002.md`.

**Breaking changes.** Programs that `mem::free` something they got from
`strings::*` or a collection now free GC memory — that must become a
diagnosable runtime error. Sweep `showcase/` and `tests/` for it.

**Completion criteria.** The stress test's RSS is bounded; every retention
test passes; FFI-crossing is safe; full suite green; a documented, measured
pause-time and throughput number in the change record.

**Risks.** **Highest in the roadmap.** Named:
- *False retention* from conservative scanning — acceptable and expected;
  quantify it rather than trying to eliminate it.
- *Missed roots* — catastrophic and hard to reproduce. The register-spill
  technique must be right, and it is platform-specific.
- *Interior pointers* — `List<T>.data` arithmetic makes these common in
  TurfLang specifically. Do not skip the block table.
- *Debuggability* — this is why TOOL-001 should land first.

**Must NOT reinterpret.** Non-moving (FFI depends on it). No finalizers. The
two heaps do not merge. Do not add write barriers "for a future generational
collector" — that is speculative complexity for a collector that does not
exist (§25).

---

### STD-001 — Boundary formalization

**Priority** P2 · **Complexity** M · **Prerequisites** RT-001, LANG-002

**Scope.** Finish what LANG-002 and RT-001 start: the closed intrinsic list
is enforced (no compiler-known symbol outside `include/Intrinsics.h`); the
prelude's membership is specified rather than being a hardcoded list in
`Parser.cpp`'s `PreludeModuleNames`; `AGENTS.md`'s "no compiler
special-casing" claim is made true and *tested* (a script asserting no
path-based privilege remains). **Handbook impact**: `docs/architecture/` gains
a `boundary.md`.

---

### STD-002 — Standard-library diagnostics

**Priority** P2 · **Complexity** M · **Prerequisites** TYPE-002, RT-001

**Scope.** §15.6 in full: `@caller_location` so a stdlib panic reports the
*user's* line; error-type payloads so a `!T` failure can carry a message;
and the instantiation-site error reporting that TYPE-002 delivers. **Testing**:
a test asserting that `test::assert`'s failure message names the caller's
file and line, not `lib/std/testing/test.tr`'s.

---

### STD-003 — Post-monomorphization standard-library modernization

**Priority** P2 · **Complexity** M · **Prerequisites** TYPE-002, LANG-001,
LANG-002

**Why it must happen here.** TYPE-002 deliberately lands with the standard
library *unchanged*, so that monomorphization is reviewable on its own. That
leaves a set of workarounds in `lib/std` that exist only because of erasure
and that are now dead weight. Scheduling them as their own group is what
stops them from being quietly forgotten — every one of them is currently
documented in `LIMITATIONS.md` or a source comment as "works around a
compiler gap", and those gaps will be closed.

**Scope, item by item — each traceable to a now-removed limitation:**

| # | Workaround | Where | Now possible |
|---|---|---|---|
| 1 | `self.cap * 8` with the comment *"In Type Erasure, all T are 64 bits"* | `lib/std/collections/list.tr:27-28` (and the same shape in `map`/`set`/`stack`/`queue`/`Pool`) | `self.cap * sizeof(T)` — real per-instantiation sizes |
| 2 | `List<T*>` for any struct over 8 bytes | `docs/reference/stdlib/collections.md` and every caller following it | `List<T>` directly |
| 3 | `list_sort` / `list_binary_search` / `list_find` as concrete `List<int>`-only **free functions** (`LIMITATIONS.md` §2) | `lib/std/collections/list.tr` | genuine generic `List<T>` methods with a comparator |
| 4 | `Request.get_header()` / `Server.route_index()` / `has_route()` / `get_route()` hand-rolled on a concrete struct to dodge generic equality (`LIMITATIONS.md` §1) | `lib/std/http/http.tr` | plain `Map<string,string>` lookups |
| 5 | `List<T>` exposes a public `data: T*` and has no `get`/`set` (`LIMITATIONS.md` §4) | `lib/std/collections/list.tr` | `operator[]` / `operator[]=` from LANG-001; `data` can become private-by-convention or a safe type |
| 6 | `Map`/`Set` lookup is a **linear scan**, because *"there is no generic `hash(T)` in the language yet"* (`map.tr`'s own header comment) | `lib/std/collections/map.tr`, `set.tr` | a real `hash(T)` is writable once `T` is concrete per instantiation — this changes `Map`/`Set` from O(n) to O(1) lookup |
| 7 | `mem::Pool<T>` is a **deliberate duplicate** of `List<T>`'s growth logic because `list.tr` imports `mem.tr` and circular imports are unsupported | `lib/std/mem/mem.tr:155-214` | unchanged by this milestone — **leave it**; the circular-import constraint is real and out of scope |
| 8 | Five collection modules need the raw-pointer capability only because `data: T*` is raw | all of `lib/std/collections/` | after LANG-002, `data` can be a safe type and those modules drop the capability entirely (§15.7) |

**Semantic decisions.** None new. This group *consumes* frozen decisions
rather than making any.

**Compiler changes.** None — that is the point. If this group needs a
compiler change, something in TYPE-002 or LANG-001 was incomplete; report it
rather than working around it again.

**Diagnostics changes.** None.

**GC interactions.** Item 6 (hashing) changes `Map`/`Set`'s allocation
pattern. If this lands after MEM-002, re-run the RSS stress test.

**Testing scope.** Every touched module's existing tests must pass unchanged
— that is the primary safety net. Add: `List<Big>` with a >8-byte struct used
through `push`/`[]`/`length` (item 2); a generic `List<T>.sort()` over at
least `int`, `double` and `string` (item 3); `Map<string,V>` with enough keys
to make the O(n)→O(1) change observable (item 6). **Per `LIMITATIONS.md`'s
own warning, every string-keyed test must use a runtime-built string, never a
literal — identical literals are linker-merged and can make a broken test
pass** (INV-TEST-1, §19 pitfall 14).

**Handbook impact.** `docs/reference/stdlib/collections.md` (the `List<T*>`
workaround section is deleted), `docs/reference/stdlib/http.md`,
`docs/reference/stdlib/mem.md`. **`docs/planning/LIMITATIONS.md` should be
closed out entirely and archived** — every item in it is resolved by
TYPE-002 + LANG-001 + this group.

**Change record** `docs/development/changes/STD-003.md`.

**Breaking changes.** Item 5 (if `data` stops being public) and item 3 (if
the free functions are removed rather than kept as deprecated wrappers).
Prefer keeping wrappers for one cycle.

**Completion criteria.** No `* 8` size arithmetic remains in `lib/std`; no
`List<T*>` workaround remains; `LIMITATIONS.md` is closed; `Map`/`Set` lookup
is no longer linear; full suite green; `showcase/` still runs.

**Risks.** Low–medium, and mostly concentrated in item 6 — introducing
hashing is a real algorithmic change, not a cleanup, and could reasonably be
split into its own group if it grows.

**Must NOT reinterpret.** Do not "fix" `mem::Pool<T>`'s duplication (item 7).
Do not add a compiler feature to make any of this easier.

---

### OPT-002 — Check elimination and frontend specialization

**Priority** P3 · **Complexity** M · **Prerequisites** OPT-001, SEM-002,
TYPE-002

**Scope.** Use `ValueRangePass`'s proven ranges to elide array bounds checks
and SEM-002's overflow traps where provably unnecessary — `DESIGN_NOTES.md`
§1 already specifies the bounds case and notes the data is already computed
but unused. This is what recovers the cost of F-NUM-3. **F-OPT-2 applies:
removal by proof only.** Also: `CallGraphPass::runGlobal()` is still a literal
stub while `AddCallEdge()` already populates a caller→callee map nothing
reads; either use it or delete it.

---

### SLM-001 — Diagnostic-assistant architecture

**Priority** P3 · **Complexity** M · **Prerequisites** DIAG-001;
independent of everything else

**Scope.** §16 in full. Schedule whenever there is appetite; nothing blocks
on it.

---
## 9. Generic Representation — the decision

### 9.1 What exists today, precisely

`Resolver::substituteTypeParams()` rewrites `NamedType("T")` into a real
`TurfGenericParamType("T")` during Pass-1 hoisting, mutating the AST in
place. `typesCompatible()` then treats a `TurfGenericParamType` on either
side as compatible with **anything** (Rule 1, checked before every other
rule), so overload resolution needs no generics-specific code. The
TypeChecker checks a generic body **once**, generically.

Codegen erases:

```cpp
// CodegenCore.cpp ~352 — every type parameter, one pointer
if (dynamic_cast<const TurfGenericParamType *>(T))
  return LLVMTypeCache[Key] = llvm::PointerType::get(*TheContext, 0);

// CodegenCore.cpp ~1177 — stuffing a scalar in
if (ValTy->isDoubleTy()) Val = Builder->CreateBitCast(Val, Int64Ty, "dbl_to_i64");
if (Val->getType()->isIntegerTy()) return Builder->CreateIntToPtr(Val, VoidPtrTy, "erased_ptr");
```

`FieldAccessExprAST::codegenLValue()` resolves any `GenericType`-typed base
straight back to its single shared base-name struct type, with the comment
*"a single layout is correct for every instantiation."* `GlobalSymbolTable`
only ever holds `"Box"`, never `"Box<int>"`.

### 9.2 What that costs, measured

| Consequence | Evidence |
|---|---|
| **ICE above one machine word** | `List<Big>` (24-byte struct) → `Internal compiler error: … Invalid bitcast: bitcast %Big %x4 to ptr`. Verified. |
| **No per-`T` operator dispatch** | `LIMITATIONS.md` §1: inside `Map<K,V>`, both operands of `self.keys[i] == key` have static type `K`, so no `operator==(string,string)` candidate can exact-match; the "same-name" fallback lets a raw pointer compare through. `tests/99_known_bugs/set_string_dedup_broken.tr`. |
| **No generic comparator or sort** | `LIMITATIONS.md` §2. `list_sort`/`list_binary_search`/`list_find` are concrete `List<int>`-only free functions *because of this*. |
| **No `sizeof(T)`** | `list.tr:27-28` hardcodes `self.cap * 8` with the comment *"In Type Erasure, all T are 64 bits"*. |
| **No per-instantiation optimization** | A `List<int>.push` can never be specialized or inlined against a concrete element type. |
| **Fake pointers in the heap** | `CreateIntToPtr` on an `int` value `5` stores the pointer value `0x5`. A `double` is stored as its raw bit pattern reinterpreted as an address. |

### 9.3 The decision — FROZEN

> **TurfLang generics are monomorphized. Type erasure is removed entirely.
> There is no hybrid, no fallback erasure path, and no trait/constraint
> system.**

One fully-specialized copy of a generic function, method, or struct is
generated per distinct tuple of concrete type arguments. `TurfGenericParamType`
exists only inside an uninstantiated template and may never reach Codegen.

### 9.4 Why — and why not the alternatives

**Why not keep erasure.** The last row of §9.2 is decisive on its own: a
precise garbage collector must be able to look at a heap word and know
whether it is a pointer. Under erasure, `List<int>` and `List<string>` are
*the same type* with *the same descriptor*, and the `int` case has
deliberately manufactured values that are indistinguishable from addresses.
A conservative collector would suffer false retention from them; a precise
one is simply impossible. Since the memory model (§14) is the headline goal
of this milestone, erasure is disqualified before any other argument is
needed.

The remaining rows are not small either. Every one of `LIMITATIONS.md`'s open
generics items, plus the ICE, plus `sizeof(T)`, is the same root cause — and
`LIMITATIONS.md` itself says so: *"Every item below traces back to the same
root cause: Type Erasure with no monomorphization."*

**Why not a trait/constraint system instead.** `LIMITATIONS.md` proposes
`fn<T: Eq>` as *"probably the right shape."* It would fix the operator
dispatch problem — but not the >8-byte ICE, not `sizeof(T)`, not the GC
descriptor problem, and not specialization. It is also a large, permanent
addition to the language surface (constraint syntax, a trait declaration
form, coherence rules, and eventually associated types, because every trait
system grows them). Monomorphization fixes the same problem *and* the four
others *and* adds no syntax at all. **Do not add both.** (F-GEN-3.)

**Why not a hybrid.** The obvious hybrid — monomorphize when `T` is large,
erase when it fits in a word — means two representations, two codegen paths,
two GC stories, and a rule every programmer has to learn. Every ambiguity
this milestone exists to remove would come back as "which one am I getting?"

**Why monomorphization is tractable here specifically.** Three pieces of
enabling machinery already exist and are already exercised:

1. **`clone()` on every AST node** (`include/ASTDiff.h`'s `CloneExprAST`),
   built for the SLM sandbox. Deep-copying an arbitrary declaration subtree
   is a solved problem in this codebase, and `clone()`'s own doc comments
   already establish the correct contract: *resolved fields are deliberately
   not copied; cloned ASTs must be re-resolved.*
2. **`substituteTypeParams()`** already walks a type recursively through
   pointer/reference/array/generic-argument wrapping. Monomorphization needs
   the same walk with a concrete map instead of a name list.
3. **`collectGenericSubstitution()`** (`src/TypeCheckerCall.cpp`) already
   builds the concrete substitution map at every generic call site. That map
   *is* the instantiation request.

**What it costs.** Compile time and binary size grow with the number of
distinct instantiations, not declarations. Current compile times are 0.28–
0.52 s including a full re-parse of the stdlib, so there is real headroom.
Binary size is currently *worse* than it needs to be for an unrelated reason
— an empty `main` carries 84 unused functions because there is no
`GlobalDCE` — so OPT-001 landing first will very likely make binaries
*smaller* even after monomorphization. **U-2 requires measuring this before
stage 3 of TYPE-002, and recording the numbers.**

### 9.5 The architecture

A new pass, between TypeChecker and Codegen:

```
Lexer → Parser → Resolver(2) → TypeChecker → Monomorphizer → Codegen
```

**Data structures.**

```
InstantiationKey = (DeclarationID, ordered vector<concrete TurfTypePtr>)
InstantiationCache : InstantiationKey -> MonomorphizedDecl*
Worklist           : queue<InstantiationRequest>
```

**Algorithm.**

1. Seed the worklist from every generic call site and every `GenericType`
   occurrence in the type-checked AST, using the substitution maps the
   TypeChecker already built.
2. Pop a request. If its key is in the cache, rewrite the requesting site to
   point at the cached instantiation and continue.
3. On a miss: `clone()` the template declaration; substitute concrete types
   for every `TurfGenericParamType` (the recursive walk of
   `substituteTypeParams()`, generalized); re-run the Resolver and
   TypeChecker over the clone; run TYPE-001's acyclicity check on any
   resulting struct layout; register it under the extended mangled name; push
   any new requests its body produced.
4. Repeat to a fixed point. Cap instantiation depth at 64, reporting
   `GenericInstantiationDepthError` with the chain.
5. Assert no `TurfGenericParamType` remains in any node Codegen will see.

**Mangling.** Extend the existing scheme. It must stay a pure function of
(module, base name, type arguments, parameter types) so two translation units
would agree — even though there is no separate compilation today, keeping the
property costs nothing and is what a future package system would need.

**Where errors are reported.** When a monomorphized body fails to type-check,
the diagnostic is anchored at the **instantiation site**, with
`withRelated()` notes at (a) the generic declaration and (b) the offending
line inside the template. This is the mechanism that gives the standard
library real compile-time diagnostics (§15.6), and it must be built from the
start — a monomorphization failure with no instantiation backtrace is a worse
user experience than today's behaviour.

### 9.6 What `typesCompatible()` Rule 1 becomes

It stays, unchanged in code, with a narrowed *contract*: "a
`TurfGenericParamType` matches anything" is a **candidate-matching** rule,
valid only before substitution. After the monomorphizer runs, no
`TurfGenericParamType` exists, so the rule is unreachable — which is exactly
the assertion in step 5. Document this on the rule itself; it is the kind of
invariant that silently rots otherwise.

### 9.7 Which type table is authoritative (C-11)

Codegen re-derives types via `getExprType()` and never reads `NodeTypes_`.
Rather than unifying the two engines (explicitly out of scope, §25), the
monomorphizer **writes concrete types onto the cloned AST nodes themselves**.
Both consumers then agree by construction: `getExprType()` walking a
monomorphized clone sees concrete types because that is what the nodes say.

### 9.8 Interactions

| With | Interaction |
|---|---|
| **GC** | Enabling. Per-instantiation types → per-type pointer bitmaps → precise heap tracing. Also removes the `IntToPtr` fake pointers. |
| **Numerics** | `List<int32>` gets genuine 32-bit arithmetic with 32-bit overflow checks. SEM-002 must skip arithmetic on a `TurfGenericParamType` operand; after this group there are none. |
| **`guess`** | `mut l = List::default();` still cannot infer a type argument — there is nothing to infer from. Must be a clear error (F-GUESS-3). |
| **Operators** | Fixes `LIMITATIONS.md` §1/§2 with no operator-system change at all. LANG-001 then adds the operators the collections want. |
| **Optimization** | Per-instantiation specialization and inlining become possible; `GlobalDCE` deletes unused instantiations. |
| **Stdlib** | `cap * 8` → `cap * sizeof(T)`; `List<T*>` workaround retired; `list_sort` et al. can become generic. |
| **Initialization** | Each instantiation's layout is checked for acyclicity separately (a substituted `T` can create a by-value cycle a template did not have). |

---

## 10. Numeric Representation and Range Semantics

### 10.1 The nine questions, answered separately

The prompt asks these to be distinguished. They are, and the distinction is
the whole design.

| # | Question | TurfLang's answer |
|---|---|---|
| 1 | **Lexical validity** | Is the token well-formed? Decimal, `0x`, `0b`, `0o`, `_` separators, `.`, exponent. A malformed token is a lexer error. Independent of any type. |
| 2 | **Literal representability** | Can the written value be held exactly? Integers: yes, always — held as arbitrary-precision `APSInt`. Floats: held as the exactly-rounded `APFloat`, with an `overflowed` flag if rounding produced `inf`. |
| 3 | **Destination-type representability** | Does the exact value fit the type it is being given? Checked at the moment the type is fixed. Out of range → **hard error**. |
| 4 | **Implicit conversion** | `int32→int`, `int32→double`, `int→double`. Nothing else, ever. |
| 5 | **Explicit conversion (`to`)** | The documented table, **enforced in the TypeChecker** (moved out of Codegen). `double→int` truncates toward zero; `int→int32` truncates. |
| 6 | **Compile-time arithmetic overflow** | On untyped literals: exact, arbitrary precision; only the final value is range-checked. On typed constants: provable overflow is a **compile error**. |
| 7 | **Runtime arithmetic overflow** | Signed integer: **trap** (panic). Never wraps, never UB. |
| 8 | **Floating-point overflow** | At runtime: IEEE `inf`, **no trap**. In a literal: hard error. |
| 9 | **Precision loss** | `int→double` above 2^53 loses precision and stays **implicitly allowed** (removing it would be gratuitously disruptive and `int→double` is by far the most common implicit conversion in real code). An untyped *literal* → `double` must be exactly representable. An opt-in lint may flag the runtime case later; do not make it an error. |

### 10.2 The specific cases from the prompt

```turf
mut x: int = 999999999999999999999999999999999999999999999999;
```
**Today**: silently `9223372036854775807`. **After**: hard error —

```
error: integer literal is not representable in type 'int' [E0NN]
  the literal's value is 999999999999999999999999999999999999999999999999
  'int' holds -9223372036854775808 .. 9223372036854775807
  hint: 'int' is TurfLang's only general-purpose integer type; there is no
        wider one. If you need arbitrary precision, use a library type.
```

```turf
mut x = 999999999999999999999;   // with guess
```
Inference picks the *default* type for an untyped integer literal — `int` —
and then representability is checked against it. Same error. **The default
type is never widened to fit the literal**, because there is no wider type,
and even if there were, silently changing a variable's type based on a
literal's magnitude is exactly the kind of implicit behaviour §6.3 forbids.

```turf
mut d: double = <1 followed by 400 zeros>.0;
```
**Today**: silently `inf`. **After**: hard error (the `overflowed` flag).

```turf
mut d: int = -9223372036854775808;
```
**Today**: `-9223372036854775807` — because `9223372036854775808` clamps
first, then is negated. **After**: correct. Unary `-` applied *directly* to
an untyped integer literal folds into the literal before representability is
checked.

```turf
mut c: int = 9223372036854775807 + 1;
```
**Today**: silently `-9223372036854775808`. **After**: a **compile** error —
both operands are typed constants and the overflow is provable.

```turf
mut a: int = f(); mut b: int = g(); mut c: int = a + b;
```
**After**: a runtime overflow **trap** if it overflows. Emitted as
`llvm.sadd.with.overflow` plus a branch to `turf_rt_panic`, removable by
OPT-002 when `ValueRangePass` proves the ranges.

### 10.3 The trapping decision, argued

Trapping costs a compare and a branch per arithmetic operation. It is the
right default for TurfLang specifically because:

1. **It is consistent with what the language already does.** TurfLang
   already emits unconditional array bounds checks and unconditional
   division/modulo-by-zero checks — for `double` operands too, which is why
   `inf` from division is unreachable. A language that traps on `arr[10]` in
   a 5-element array but silently wraps on `INT64_MAX + 1` is incoherent.
2. **Wrapping is a silent wrong answer**, which is the exact class of defect
   this milestone exists to eliminate.
3. **The cost is recoverable.** `ValueRangePass` already computes the
   information OPT-002 needs; `DESIGN_NOTES.md` §1 already specifies the
   bounds-check-elimination version of the same idea.
4. **UB is not an option.** A compiler that has UB in its own semantics
   cannot give trustworthy diagnostics about it.

If measurement later shows the cost is unacceptable, the fallback is **not**
wrapping-by-default. It is an explicit, off-by-default
`-fwrapping-arithmetic` that is documented as changing language semantics and
that F-OPT-1 explicitly exempts. Do not make that call without numbers in a
change record.

### 10.4 What is deliberately not added

- **No unsigned types.** They would double the implicit-conversion matrix,
  introduce the signed/unsigned comparison trap, and buy very little in a
  language whose integer is 64-bit and whose bit-twiddling surface is small.
  `int32` exists purely to describe a C ABI honestly, and that is the only
  justification a second integer type gets. (F-NUM-5.)
- **No literal suffixes** (`5i32`). `to int32` already exists and is explicit.
- **No bare-leading-zero octal.** `0755 == 755`.
- **No arbitrary-precision runtime integer type in the language.** If one is
  ever wanted it is a stdlib type over the manual heap, not a primitive.
- **No change to `bool`'s isolation** or `char`'s non-numeric status. Both are
  deliberate, both are documented, both are load-bearing (`CharType`
  deliberately not overriding `isNumeric()` is what keeps `char` out of every
  coercion path with no extra exclusion logic).

### 10.5 The `int → double` precision question, explicitly

`typesCompatible()` allows `int → double` implicitly. Above 2^53 that loses
precision silently. Three options were considered:

- *Remove the implicit conversion.* Correct in principle; enormously
  disruptive in practice (`2 ** 10` already produces `double`; arithmetic
  mixes constantly). Rejected.
- *Error only above 2^53.* Impossible in general — the value is not known at
  compile time.
- **Keep it, and make the literal case exact.** An untyped literal assigned
  to a `double` must be exactly representable; a runtime `int` value keeps
  converting as today. A future opt-in lint can flag the runtime case.

That is the decision. It is a genuine, acknowledged soft spot, recorded here
rather than papered over.

---

## 11. `guess` Type Specification

### 11.1 The contradiction, stated first

| Source | Says |
|---|---|
| **Implementation** (`ParseLetStatement`) | `TOK_COLON` is required immediately after the name. Verified: `mut x = 5;` → *"Expected ':' after variable name."* |
| **Tests** | Nothing anywhere exercises an omitted annotation; the entire 646-file suite annotates every declaration. |
| **`docs/reference/variables_and_types.md`** | Documents the mandatory annotation. Accurate. |
| **Handbook ch12** | *"The `: Type` annotation is not optional, and there is no inference form — TurfLang has no `mut x = 5;` that guesses `x`'s type from its initializer."* Accurate today; **this sentence is what SEM-003 invalidates.** |
| **RFC / design material** | No `guess` design exists anywhere in the tree. `grep -r guess` finds only the English word in comments. |

So: there is no contradiction between sources *today* — they agree there is
no inference. The contradiction is between all of them and the intended
feature. That makes the handbook impact unusually clean to describe, and it
is the clearest possible demonstration of why §18's system is needed before
this lands.

### 11.2 Syntax — FROZEN

`guess` is a **type-position keyword**:

```turf
mut x: guess = 5;
```

Omitting the annotation entirely is defined as **exactly** that:

```turf
mut x = 5;          // identical to  mut x: guess = 5;
```

Both spellings produce the same AST after parsing and the same
`--dump-typechecked` output. The explicit spelling exists because it is
greppable, because it makes the inference visible in code review, and because
it is the only spelling available if inference is ever extended to another
type position. The short spelling exists because it is the entire point of
the feature.

*(This is the one place in §6 where two spellings for one thing are
deliberately allowed. It is justified by the above, and by the precedent that
`mut`/`mutable` and `immut`/`immutable` are already exact synonyms.)*

### 11.3 Semantics — Model A, FROZEN

The type is inferred from the initializer and **permanently fixed** at the
declaration.

```turf
mut x = 5;
x = 10;      // ok      -- int
x = 5.5;     // ERROR   -- cannot assign 'double' to 'int'
             //            note: 'x' was inferred as 'int' here
```

### 11.4 Why Model B is rejected — FROZEN

Model B (`mut x;` … `x = 5;` infers `int` from the first assignment) is
rejected, and must not be added later. Four reasons, in order of weight:

1. **It is irreconcilable with definite assignment.** `DefiniteAssignmentPass`
   exists to reject a read before assignment. Model B makes the *type* also
   depend on flow, so a variable's type would be a dataflow fact with a
   meet operation — and there is no correct meet for "assigned `int` on one
   branch, `string` on the other" except an error, which means the feature's
   entire benefit evaporates in exactly the cases it would be used.
2. **It contradicts F-INIT-1.** SEM-004 unifies on definite assignment
   precisely so that a declaration without an initializer means one simple
   thing. Model B gives it a second meaning.
3. **Control-flow joins and loops have no good answer.** A first assignment
   inside a loop body, or inside one arm of an `if`, has no single
   "first".
4. **It buys nothing.** `mut x: int;` already exists and is one token longer.

### 11.5 Scope — FROZEN

Inference is **local, one-directional, and single-expression**. It applies to
local variable declarations and nothing else.

| Position | Inference? | Why |
|---|---|---|
| Local `mut`/`immut` declaration | **yes** | The feature. |
| Function parameter | **no** | A signature must be readable without its body. |
| Function return type | **no** | **Architectural**: `Resolver::hoistTopLevel()` registers every signature in Pass 1 *without ever entering a function body*. Return-type inference would require entering bodies during hoisting, which would break forward references and mutual recursion — the invariant the two-pass design exists to provide. This is not a stylistic preference; it is load-bearing. |
| Struct field | **no** | A struct's layout must be computable from its declaration alone (and TYPE-001's acyclicity check depends on that). |
| `export immut` constant | **no** | Hoisted in Pass 1, same reason as return types. |
| Generic type argument | **no** | F-GEN-3; there is no inference engine to run. |

### 11.6 Inference rules — the complete table

For `mut x = <expr>;` the inferred type is `typeof(<expr>)` as the
TypeChecker computes it, with these specific resolutions:

| Initializer | Inferred | Note |
|---|---|---|
| untyped integer literal | `int` | default type; representability then checked against `int` |
| untyped float literal | `double` | |
| `true` / `false` | `bool` | |
| `'c'` | `char` | |
| `"s"` | `string` | |
| `[e1, e2, e3]` | `T[3]` | `T` = the array literal's element type; a mixed-type literal is already an error |
| `Point::default()` / `Point::new(…)` | `Point` | |
| a function call | its declared return type | |
| a call returning `!T` | `!T` | must then be `recover`ed like any other `!T` |
| `&expr` | `&T` | |
| a variable of type `T` | `T` | |
| an `int32`-typed expression | `int32` | **not** widened to `int` — inference takes the expression's own type, never a widened one |

**Errors:**

| Case | Diagnostic |
|---|---|
| `mut x: guess;` (no initializer) | "`guess` needs an initializer to infer from." |
| `mut x = someVoidFn();` | "cannot infer a type from a `void` expression." |
| `mut x = null;` | "cannot infer a type from `null`; annotate the reference type explicitly." |
| `mut x = List::default();` | "cannot infer the type arguments of a generic type; write `mut x: List<int> = …`." |
| `mut x = 999999999999999999999;` | the SEM-002 representability error, reported against `int` |
| `mut x = 5; x = 5.5;` | the ordinary type-mismatch error, plus `withRelated()` at the declaration: "`x` was inferred as `int` here" |

### 11.7 `typeof` / `sizeof`

`typeof(x)` on a `guess`-inferred variable returns the inferred type's name,
exactly as for an annotated one. `sizeof` takes a type and is unaffected. No
new interaction.

### 11.8 Implementation note (the one real design question)

`guess` must be resolved in the **TypeChecker**, not the Parser. The parser
emits a `VarDeclExprAST` with a null/sentinel type;
`TypeChecker::visitVarDecl` infers from the already-checked initializer and
calls the existing `setTypePtr()`. The consequence is that
`Resolver::visitVarDecl` → `ResolverScopeTable::declareVariable()` runs
*before* the type is known, so either `declareVariable` must accept a null
type and be filled in later, or the Resolver must defer. **Answer this before
writing code** — it is the only genuinely open question in SEM-003.

---

## 12. Recursive Default Initialization

### 12.1 The current model, stated exactly

Two mechanisms, split by type, decided in `ParseLetStatement`:

```cpp
if (CurTok != TOK_ASSIGN) {                       // no initializer
  std::unique_ptr<ExprAST> Init = nullptr;
  if (!Type->isInt() && !Type->isDouble() && !Type->isBool() && !Type->isString() &&
      !Type->isPointer() && !Type->isNamed("ptr") && !Type->isNamed("void*") && !Type->isNull())
    Init = SynthesizeDefault(Loc, Type);          // Mechanism B
  ...
}
```

- **Mechanism A** — `int`, `double`, `bool`, `string`, pointers: no
  initializer at all; `DefiniteAssignmentPass` rejects a read before
  assignment (E008).
- **Mechanism B** — `char`, `int32`, structs, enums, arrays, generics: a
  `DefaultInitExprAST` is synthesized and the variable is immediately
  readable.

`docs/reference/variables_and_types.md` calls the split *"the single most
important gotcha in the type system"*; handbook ch12 gives it a whole section
and a `pitfall`. `SynthesizeDefault()`'s own `isInt/isDouble/isBool/isString`
branches are **dead code** on that path, because the caller already excluded
those types — a small but telling sign that the split was not designed so
much as accreted.

Underneath both, storage is always zeroed: `ArrayDeclExprAST::codegen()`
memsets; `GetDefaultValue()` builds a recursive zero constant; struct
`::default()` allocas and stores a zero aggregate.

### 12.2 The decision — FROZEN

> **One mechanism: definite assignment, for every type. Mechanism B is
> removed. Storage remains zero-filled.**

`T::default()` is the explicit opt-in and already works today, so the
migration is mechanical: `mut c: Config;` → `mut c: Config = Config::default();`.

### 12.3 Why this direction and not the other

Unifying the other way (eager defaults everywhere) would be a smaller diff.
It is rejected because:

1. It would **delete E008**, which is one of the compiler's genuinely good
   diagnostics and is backed by a real forward dataflow analysis.
2. It would make `mut x: int;` silently `0`, which is precisely the class of
   silent-wrong-answer behaviour §10 is eliminating elsewhere.
3. Mechanism B is the mechanism that *recurses into struct fields*, and that
   recursion is what makes an infinitely-sized type crash the compiler
   (§12.4). Keeping the recursive path as the default keeps the hazard as the
   default.

The cost is real: 13 test files and an unknown amount of `lib/std` change
(U-1). If the sweep shows the `lib/std` volume is large, SEM-004 splits into
deprecate → migrate → remove.

### 12.4 Recursive types — the structural fix

Verified today:
- `struct A { me: A }` **compiles**. Instantiating it **segfaults**.
- `struct A { b: B } struct B { a: A }` — same.

Neither is caught anywhere. `GetDefaultValue()` recurses field-by-field into
struct types with no cycle detection, and `getLLVMType()` would have no
finite layout to produce even if it did.

**The fix is not a depth limit.** It is an acyclicity check in the Resolver
(TYPE-001), producing the invariant:

> **INV-LAYOUT-1: every registered `TurfStructType` has a finite, computable
> size.**

Once that holds, `GetDefaultValue()`'s recursion terminates by construction,
`getLLVMType()` always succeeds, and no depth limit is needed anywhere. This
is the §18-style "structural prevention over a test" pattern applied
literally.

Containment edges: struct-typed field, array-of-struct field, generic
instantiation containing one. Non-edges: `T*`, `&T`, `&?T`, `ptr`, `string`,
arrays of any of those. A self-referential *pointer* field remains the
supported, intended way to build recursive data — and after LANG-002 it can
be a safe `&?Node` rather than a raw `Node*`.

### 12.5 Default values — the complete table (post-SEM-004)

Under F-INIT-1, "default value" only ever means "what `T::default()` and
zero-filled storage produce". No variable silently acquires one.

| Type | Zero value | Notes |
|---|---|---|
| `int` | `0` | |
| `int32` | `0` | |
| `double` | `0.0` | |
| `bool` | `false` | |
| `char` | `'\0'` | |
| `string` | `""` (a static empty literal) | not a heap allocation |
| `T*` / `ptr` | `null` | |
| `&T` | **none** | a non-null reference has no zero value; `T::default()` on a struct containing a `&T` field is an **error** |
| `&?T` | `null` | |
| `T[N]` | `N` zero elements, recursively | |
| struct | every field's zero, recursively | terminates by INV-LAYOUT-1 |
| enum | the ordinal-0 variant | already true |
| `machine` | the ordinal-0 state | already true |
| generic `T` | the instantiated type's zero | post-TYPE-002 this is well-defined per instantiation |

The `&T` row is new and important: it is the one type with no zero value, and
it must produce a clear diagnostic rather than a null reference. LANG-002
must handle this when it lands.

### 12.6 Arrays — the granularity decision

`docs/reference/arrays.md` documents `mut arr: int[5];` as a defined all-zero
array (Mechanism B). Under F-INIT-1 it becomes definitely-unassigned. The
recommended granularity, to be recorded explicitly in SEM-004:

- A **whole-array** read before assignment is an error.
- An **element write** (`arr[0] = 1`) counts as assigning the array.
- Elements are **not** tracked individually.

That is the same granularity `ConstructorFieldAssignmentPass` already uses
for `self.<field>`, so it is consistent with an existing precedent rather
than a new rule.

### 12.7 Interactions

| With | Interaction |
|---|---|
| **`guess`** | `mut x = …` always has an initializer, so it is never subject to definite assignment. The two features are complementary, not overlapping. |
| **GC** | "storage is always zeroed" (F-INIT-2) means a pointer field always reads as a valid null before assignment, which a tracing collector requires. Preserving that guarantee is why F-INIT-2 exists as a separate frozen decision from F-INIT-1. |
| **Generics** | Each instantiation's default is computed from its own concrete layout. TYPE-002 re-runs the acyclicity check per instantiation. |
| **Optimization** | An always-zeroed alloca that is then fully overwritten is a redundant store `SROA`/`DSE` will remove — which is another reason to keep the zero-fill (it is free at `-O1`+) rather than making the static check load-bearing for memory safety. |
| **Numerics** | None. |

---

## 13. Operator and Symbol-Table Specification

### 13.1 The current matrix

| Category | Built-in | Overloadable today | After LANG-001 |
|---|---|---|---|
| Arithmetic | `+ - * / %` | `+ - * /` | `+ - * / %` |
| Exponentiation | `**` (right-assoc, always yields `double`) | no | **yes** |
| Comparison | `< > <= >=` | yes | yes |
| Equality | `== !=` | yes | yes |
| Logical | `&& \|\| !` (short-circuiting) | no | **never** (F-OP-4) |
| Bitwise | `& \| ^ ~ << >>` (int-only) | no | **yes** (binary + unary `~`) |
| Unary | `- ! ~` | no | **yes** (`-`, `!`, `~`) |
| Assignment | `=` | no | **never** |
| Compound | 11 forms | no | **desugars** to `a = a op b` |
| Indexing | `[]` on array/pointer/string | no | **yes** (`[]` and `[]=`) |
| Call | `()` | no | **never** |
| Member | `.` | no | **never** |
| Cast | `to` | no | **never** |
| Increment/decrement | `++ --` prefix and postfix | no | prefix inherits `operator+` via its existing desugaring; postfix — decide and record |
| Ternary | `give`/`otherwise` | no | **never** |

### 13.2 Symbol-table design — FROZEN, unchanged

Operator overloads are stored in the **ordinary name-keyed overload map** that
ordinary functions use (`ResolverModule::Functions` / `TurfModule::Functions`),
under a synthesized name from `operatorFunctionName()` (`'+'` → `"operator+"`).
This is safe because `operator` is a keyword token and no legal identifier can
contain `+ - * / = < >`, so the synthesized name can never collide with a user
symbol. **Do not introduce a separate operator table.** The existing design
means hoisting, overload resolution, mangling, UFCS candidacy and module
visibility all work for operators with zero extra code, and that property is
worth preserving.

`sanitizeSymbolName()` maps the synthesized name to a linker-safe spelling
(`turf_strings_operator_plus_string_string`). Every newly overloadable
operator needs an entry — `%` → `mod`, `**` → `pow`, `<<` → `shl`, `>>` →
`shr`, `&` → `band`, `|` → `bor`, `^` → `bxor`, `~` → `bnot`, `[]` → `index`,
`[]=` → `index_set`, and a disambiguator for unary `-` vs binary `-`.

**Unary vs binary** is distinguished by **arity**, not by a different name.
`operator-` with one parameter is unary negation; with two, it is
subtraction. The existing overload map is already arity-aware.

### 13.3 Resolution — FROZEN, unchanged

`TypeChecker::visitBinary()` resolves an operator by **exact type match**
(`sameTypeExact`), never `typesCompatible()`:

```cpp
if (sameTypeExact(C->Params[0].Type.get(), LhsTy) &&
    sameTypeExact(C->Params[1].Type.get(), RhsTy)) { ... }
```

**This is an intentional invariant and must not be relaxed.** If operator
resolution used `typesCompatible()`, then `int → double` widening would make
an `operator+(double,double)` reachable from `int` operands, an
`operator+(string,string)` reachable through the `ptr`/pointer coercion
rules, and a `TurfGenericParamType` reachable from *anything* via Rule 1.
Exact matching is what keeps operator dispatch predictable and is what makes
the string operators safe to define in the standard library at all. A
duplicate exact match is an ambiguity error, which is correct.

### 13.4 Declaration constraints

**Kept:** a declaration whose operands are all compiler-primitive
(`int`/`int32`/`double`/`bool`/`char`/pointer) is rejected
(`OperatorBuiltinPairError`). Note that `string` is deliberately **not** in
that set — that exemption is what allows `lib/std/core/strings.tr`'s
`operator +`/`==`/`<`/… to exist, and it should be documented as a deliberate
decision rather than left as an accident of which types the predicate happens
to list.

**Added — an orphan rule:** at least one operand type must be declared in the
same module as the operator. This prevents a third-party module from
redefining an operator on two types it does not own, which would make operator
meaning depend on import order. Check `lib/std` for violations before landing
it.

### 13.5 Compound assignment — FROZEN

`a op= b` ≡ `a = a op b`. There is no `operator+=`. Consequences to record:
- A user type that defines `operator+` automatically supports `+=`.
- `a` is evaluated twice in the desugaring, which matters if `a` is a call or
  has side effects. Today's built-in compound assignment does not have that
  problem. **Specify and test this**: the desugaring must evaluate the
  left-hand *location* once, not the expression twice.
- `**=`'s existing special behaviour (accepts any numeric, computes through a
  `double` intermediate, narrows back to the LHS type) is built-in and stays;
  a user `operator**` overload is used only when the operands are not numeric.

### 13.6 What must never be overloadable, and why

| Operator | Why not |
|---|---|
| `&&`, `\|\|` | Short-circuit evaluation cannot survive a function call — the right operand would have to be evaluated to be passed. Overloading them would silently break `(e != 0) && (100 / e > 5)`, a pattern `docs/reference/operators.md` explicitly documents as safe to rely on. |
| `=` | Assignment semantics (copy, immutability checking, definite assignment) are language rules, not a function. |
| `()` | Would require a call-operator dispatch mechanism that does not exist and would collide with first-class function values. |
| `.` | Member access is resolved at compile time against a known layout; making it dynamic would require a dispatch mechanism TurfLang deliberately does not have. |
| `to` | A user-defined conversion operator reintroduces implicit conversion chains, which is the exact thing §10's conversion matrix exists to keep finite and auditable. |
| `give`/`otherwise` | Same short-circuit reasoning as `&&`. |

### 13.7 Interactions

| With | Interaction |
|---|---|
| **Generics** | The whole point of sequencing LANG-001 after TYPE-002: inside a monomorphized `Map<string,int>`, `K` *is* `string`, so exact match finds `operator==(string,string)`. `LIMITATIONS.md` §1/§2 close. |
| **Numerics** | `operator%`/`operator**` on user types do not inherit the built-in numeric rules; a user `operator/` does not get the built-in division-by-zero check. Document this. |
| **GC** | An `operator+` that allocates (as `strings.tr`'s does) allocates on the GC heap after MEM-001. |
| **Optimization** | An operator call is an ordinary call and becomes inlinable at `-O2` after OPT-001. Today, with no inliner, every `"a" + "b"` is a real call. |
| **Stdlib** | `List<T>`/`Map<K,V>`/`Set<T>` gain `operator[]`/`[]=`, which is the better form of `LIMITATIONS.md` §4's requested `get`/`set`. |

---
## 14. Memory Model and Garbage Collection

### 14.1 The current memory model, stated exactly

From `docs/architecture/memory_model.md`, spot-checked against source:

| Thing | Where | Copied or aliased | Who frees it |
|---|---|---|---|
| primitives | stack/register | copied | automatic |
| `T[N]` | stack | copied (whole-aggregate load/store) | automatic |
| `T[]` parameter | **aliases the caller's frame** | not copied | n/a — no escape analysis at all |
| struct value | stack | copied | automatic |
| `self` | raw `StructName*` to the original | aliased | n/a |
| string literal | `.rodata` | pointer copied | never — lives forever |
| string from `+`/`to_upper`/… | heap (`malloc`) | aliased | **nobody** |
| `List`/`Map`/`Set`/`Stack`/`Queue`/`Pool` buffer | heap (`malloc`/`realloc`) | aliased | **nobody** |
| `mem::alloc` result | heap | aliased | the programmer |

`memory_model.md` §1.7 states it plainly: *"Turflang has no automatic memory
management today"* — no GC, no refcounting, no borrow checker, no destructors.
`strings.tr:50-53` documents the leak as a deliberate, named tradeoff
(`TURF-MEMLEAK`). There is no scope hook of any kind that could free a
`List<T>`'s buffer, and `List<T>` provides no method that would.

### 14.2 The decision — FROZEN

> **A non-moving, mark-sweep tracing collector: precise on the heap
> (compiler-emitted per-type pointer bitmaps), conservative on stack and
> registers, stop-the-world, single-threaded, with no finalizers. The GC
> heap and the manual (`mem::alloc`) heap are disjoint and never merge.**

### 14.3 Why each of those words

**Tracing, not reference counting.** `DESIGN_NOTES.md` §5 and `FUTURE.md` §5
both propose ARC. ARC is rejected here for three concrete reasons:
(a) it requires the compiler to inject retain/release at *every* assignment,
pass and scope exit — a far larger and more invasive Codegen change than a
collector, in a compiler whose Codegen is already the least-layered part;
(b) it does not collect cycles, and the first data structure users will write
once LANG-002 lands is a linked structure; (c) retain/release traffic across
TurfLang's extremely FFI-heavy stdlib would need a pinning discipline at
every boundary. A tracing collector needs *almost no Codegen changes at all*,
which is the decisive engineering argument in this codebase.

**Non-moving — non-negotiable.** The standard library crosses into C at 74
declared boundaries (§15.7), and the GC-heap types are exactly what crosses
them: a `string` is handed to `strlen`, `strcpy`, `send`, `write`, `fopen`;
a buffer is handed to `recv`. If the collector could move an object, every
one of those boundaries would need pinning. Non-moving also
means no read/write barriers are needed for correctness and no interior
pointer ever needs updating. The cost is fragmentation, which is acceptable
for TurfLang's workloads and is the right trade to make here.

**Precise on the heap.** Every monomorphized aggregate gets a compiler-emitted
`TurfTypeDesc { size, name, pointer_word_bitmap }` (RT-001), passed at the
allocation site. The collector walks an object's bitmap rather than guessing.
**This is only possible after TYPE-002** — under erasure, `List<int>` and
`List<string>` share one type and one descriptor, and the `IntToPtr`
bit-stuffing actively manufactures words that look like pointers.

**Conservative on stack and registers.** The alternative is precise stack
maps via LLVM's `gc.statepoint`/`gc.relocate` infrastructure, which requires
inserting safepoints and relocating every live pointer across them — a very
large Codegen change, and one that only pays off for a *moving* collector,
which this is not. Conservative scanning (Boehm's technique: `setjmp` to spill
callee-saved registers, then scan the stack range word by word, treating
anything that looks like a heap address as a root) requires **zero Codegen
changes for root finding**. Its cost is bounded false retention, which is
measurable and acceptable. This single choice is what makes the whole design
tractable.

**Stop-the-world, single-threaded.** There are no threads in TurfLang. A
concurrent or incremental collector would be complexity with no consumer.
§14.11 records what would change if threads ever arrive.

**No finalizers — FROZEN.** Finalizers are a well-understood design mistake:
object resurrection, unspecified ordering, unbounded latency, and the
temptation to use them for resources they cannot reliably manage. TurfLang's
answer to "close this file" is `defer` (LANG-003), which is deterministic and
which the programmer can see. **GC must not ship before `defer` does.**

**Two disjoint heaps — FROZEN.** `mem::alloc`/`malloc` memory stays entirely
manual and untraced (F-MEM-3). The collector never traces into it, and a GC
pointer stored *only* inside manual memory is not a root — which is a real
unsoundness, addressed by an explicit `mem::root()`/`mem::unroot()` API and
documented loudly. Merging the heaps would mean taking responsibility for
every `extern fn`-allocated byte in the ecosystem, which is not a promise
TurfLang can keep.

### 14.4 What lives where

**GC heap:** strings produced by any operation; collection backing buffers;
closure environments if closures ever land; any future boxed value.

**Not GC heap:** stack locals, `T[N]`, struct values, string literals
(`.rodata`), `mem::alloc` results, and anything a C library allocated.

### 14.5 The allocation API

```c
// runtime/turfrt.h
typedef struct { size_t size; const char *name; const uint8_t *ptr_bitmap; } TurfTypeDesc;

void *turf_rt_alloc(size_t bytes, const TurfTypeDesc *desc);
void  turf_rt_collect(void);              // explicit; also called on allocation pressure
void  turf_rt_add_global_root(void **slot);
void  turf_rt_remove_global_root(void **slot);
TurfRtStats turf_rt_stats(void);          // live objects/bytes — the test harness
```

Reached through the closed intrinsic list (`include/Intrinsics.h`), never via
an ad-hoc `extern fn` from a `.tr` file.

### 14.6 Root discovery

1. **Stack and registers** — conservative. `setjmp` spills callee-saved
   registers into a buffer on the stack, then the collector scans from the
   current stack pointer to the recorded stack base, word by word. Any word
   that falls inside a known heap block is treated as a reference to that
   block's containing object.
2. **Globals and statics** — precise, registered at startup via
   `turf_rt_add_global_root`. `export immut` constants are literal values with
   no storage (`CodegenConstantValue` inlines them), so they are not roots.
3. **Explicitly registered roots** — `mem::root()` for the manual-heap
   escape case.

**Interior pointers must retain.** This matters more in TurfLang than in most
languages, because `List<T>.data[i]` arithmetic is the *documented* way to
index a collection (`LIMITATIONS.md` §4). The heap therefore needs a block
table mapping any address to its containing object. Do not skip it.

### 14.7 What happens to today's manual-memory assumptions

| Today | After |
|---|---|
| `strings::*` results leak | collected |
| `List<T>` buffer leaks | collected |
| `mem::alloc` → you `mem::free` | **unchanged** |
| `mem::Pool<T>` / `free_pool()` | **unchanged** — still a manual-heap convenience |
| `free(someStringResult)` | must become a **diagnosable runtime error** (freeing GC memory). Sweep `showcase/` and `tests/`. |
| a GC pointer passed to C for the duration of a call | safe: it is still in a register or stack slot, so conservative scanning sees it, and nothing moves |
| a GC pointer stored in C-owned memory across a collection | **unsound**; requires `mem::root()`. Document prominently. |

### 14.8 Interaction with the optimizer

The one real hazard: the optimizer may keep the only live reference to an
object exclusively in a callee-saved register or a spill slot. Scanning the
register file (via the `setjmp` spill) plus the whole stack range covers
both. A second, subtler hazard is a pointer that has been transformed into an
integer and back — which is exactly what erasure's `IntToPtr` does, and one
more reason TYPE-002 must land first.

`Internalize`/`GlobalDCE` (OPT-001) must not delete a `TurfTypeDesc`. The
design avoids this by having every allocation site reference its descriptor
directly, so the descriptor is always reachable from live code.

### 14.9 Interaction with generics

Post-TYPE-002: every instantiation is a distinct type with a distinct
descriptor, so `List<int>` (no pointer words in its element array) and
`List<string>` (all pointer words) trace differently and correctly. Pre-
TYPE-002 this is impossible, and F-GEN-1 → F-MEM-1 is a hard dependency edge
in §7 for that reason.

### 14.10 Staging, and the honest alternative

MEM-002 is the highest-risk item in this roadmap. It is staged deliberately:

- **RT-001** — the runtime exists; `turf_rt_alloc` is a `malloc` wrapper;
  descriptors are emitted but unread. Nothing collects. Low risk.
- **MEM-001** — managed allocations are routed through it. Behaviour is
  identical to today (still leaks). Reviewable in isolation.
- **MEM-002** — the collector.

**The honest alternative, stated explicitly.** If MEM-002's risk proves
unacceptable, stopping after MEM-001 + LANG-003 (`defer`) + an arena type in
`mem` would still remove most real-world leaks: `defer mem::free_pool(p);`
around a request handler solves the server case, and the collection types
could gain explicit `.free()` methods. That is roughly 10% of the work for
perhaps 70% of the benefit, at the cost of the language never being able to
claim automatic memory management. This document recommends the collector —
but a decision to stop at that fallback is a legitimate engineering choice,
not a failure, and it should be made deliberately with MEM-002's measured
risk in hand rather than by drift.

### 14.11 What would change if threads ever arrive

Recorded now so a future reader does not have to re-derive it: a
multi-threaded TurfLang would need per-thread stack root scanning, a
stop-the-world handshake (signal- or polling-based), an allocation lock or
per-thread allocation buffers, and a decision about whether the collector
itself is parallel. None of that changes the *choice* of a non-moving
mark-sweep collector; all of it changes the runtime's implementation. Nothing
in this design forecloses it.

---

## 15. Standard Library Architecture

### 15.1 Is "the stdlib is just another Turf program" a problem?

**Mostly no — and the parts of it that are true are a strength.**
`strings::to_upper` being ordinary TurfLang means the language is expressive
enough to write its own library, the compiler stays small, and stdlib bugs
are fixable without touching C++. Moving `EmitStringConcat` out of Codegen
and into `strings.tr` as an `operator +` was a genuine architectural
improvement and must not be reversed.

Three things *are* wrong, and they are specific:

1. The claim is **false in one undeclared way** — the `IsPathUnderLibStd`
   privilege (§15.2).
2. The library **cannot talk to the compiler** — no compile-time diagnostic,
   no caller attribution, no way to express a requirement (§15.6).
3. There is **no runtime beneath it**, so it has nowhere to put things that
   genuinely are not library concerns (§15.4).

### 15.2 The privilege problem, and the fix

```cpp
bool TypeChecker::isUnsafeGateExempt(const SourceLocation &Loc, const ExprAST *Node) const {
  if (UnsafeDepth_ > 0) return true;
  if (Node && WasDeclaredInsideTopLevelUnsafe(Node)) return true;
  return IsPathUnderLibStd(Loc.Filename);
}
```

Verified consequence: a user's `fn walk(n: Node*)` is rejected *even inside
`unsafe`*, while the identical signature under `lib/std` is accepted. The
privilege is a filesystem path prefix. It is invisible in the file that uses
it, ungreppable, untestable, and unavailable to a user writing perfectly
legitimate FFI bindings for a C library — which is exactly what `lib/std/net`
and `lib/std/fs` are.

**Fix (LANG-002/STD-001):** a module-level capability declared in source —
`unsafe module;` on the first line — honoured unconditionally for files under
the resolved install root (`TurfInstallRoot()`, the same anchor `import`
already uses) and for any other file only with `--allow-unsafe-modules`. The
privilege becomes visible where it is used, greppable across the tree, and
available to a user who genuinely needs it. `AGENTS.md`'s "No compiler
special-casing; ordinary `.tr` files" becomes true again.

### 15.3 The intrinsic list — closed and enumerated

Today "what does the compiler know about by name?" has no single answer:
`src/Builtins.cpp` knows `typeof`, `abort`, `fprintf`, `stderr`, `panicfmt`;
`ErrorCategory.h`'s comment chain documents eight *former* intrinsics that
have since become ordinary stdlib functions; `PreludeModuleNames` in the
parser hardcodes six module names.

**Fix (RT-001/STD-001):** one header, `include/Intrinsics.h`, enumerating
every compiler-known symbol, with a one-line justification each. Adding to it
is an explicit, change-recorded act. The expected contents after this
milestone:

| Intrinsic | Why it cannot be a library function |
|---|---|
| `sizeof(T)` | takes a *type*, not a value |
| `typeof(expr)` | returns a compile-time property of an expression |
| `@caller_location` | must expand at the *call site* (§15.6) |
| checked arithmetic (`sadd_ov` …) | must lower to specific LLVM intrinsics with a trap edge (§10) |
| `turf_rt_alloc` + descriptor | the compiler must supply the type descriptor |
| `turf_rt_panic` | must be reachable from compiler-emitted check code |
| `turf_rt_add_global_root` | compiler-emitted at startup |

Everything else is ordinary TurfLang or an ordinary `extern fn`.

### 15.4 Division of responsibility — the table

| Concern | Owner | Why |
|---|---|---|
| Value/reference semantics, type rules, numeric model | **Language** | definitional |
| Name resolution, overloads, monomorphization, layout | **Compiler** | needs the type system |
| Type descriptors | **Compiler emits**, runtime consumes | only the compiler knows layouts |
| Bounds / div-zero / overflow checks | **Compiler emits**, runtime handles the trap | check placement needs type info; the panic is process-wide |
| Allocation, collection, roots | **Runtime** | process-wide state that cannot be per-translation-unit |
| Panic, abort, exit code, OOM policy | **Runtime** | must be identical everywhere |
| Instruction selection, regalloc, scalar/loop opt, DWARF | **LLVM** | not TurfLang's problem |
| Strings, collections, I/O, net, fs, json, http, math, time, random, testing | **Standard library** | ordinary TurfLang; nothing needs compiler knowledge |
| `mem::alloc`/`free`/`Pool` | **Standard library** over libc | the manual heap is a library concern |
| Formatting | **Standard library** | no compiler involvement; see §15.5 |
| `fmt`, `test`, `explain`, LSP | **Tooling** | |

### 15.5 What should *not* move into the compiler

- **String operators.** They are stdlib `operator` overloads today and that
  is correct. Do not move them back.
- **Formatting.** TurfLang has no `printf`-style formatting in the language
  and should not gain one. `io::print` overloads plus `to string` casts cover
  it; a `StringBuilder` type in `strings` covers the rest.
- **Collections.** After TYPE-002 they need nothing from the compiler except
  `sizeof(T)` and `operator[]`, both of which are general features.
- **`assert`/`fail`.** They were compiler intrinsics once and are now ordinary
  overloaded functions in `lib/std/testing/test.tr`
  (`ErrorCategory.h`'s comment documents the move). That was the right
  direction. They need `@caller_location`, not re-intrinsification.

### 15.6 Standard-library diagnostics — the real gap

Today the standard library has exactly two ways to report a problem:
`sys::panic` at runtime (with a bare message and **the stdlib's own source
line**, not the caller's), or nothing. It cannot produce a compile-time
diagnostic, cannot attribute a runtime failure to its caller, and cannot
express a requirement on a type parameter.

Four fixes, in increasing order of cost:

**(a) `@caller_location` — small, high value.** A compiler intrinsic that
expands, at each call site, to the caller's file and line. Then
`test::assert`, `sys::panic`, `List.get`'s bounds check and every stdlib
guard report *the user's* line. This is Rust's `#[track_caller]` in a much
simpler form (TurfLang has no unwinding and no inlining-through-frames
problem at `-O0`). It is the single highest-value item in this subsection.

**(b) Instantiation-site errors — free with TYPE-002.** Once a generic body
is type-checked per instantiation, an operation the concrete `T` does not
support produces a real compile error **at the user's call site**, with notes
at the generic declaration and the offending template line. That is exactly
the diagnostic a trait system would have produced, without the trait system.
`Map<K,V>` no longer silently pointer-compares strings; it either works or it
tells the user why not.

**(c) Error payloads — medium.** `TurfUserErrorType`'s variants are bare
names with no fields today (a documented, deliberate scope decision in
`errors_and_recover.md` §5.1). Adding a single `message: string` payload plus
a caller location would let `!T` failures carry real information. Scope it to
exactly that — do not build a general payload/destructuring feature.

**(d) A `@compile_error` intrinsic — optional.** Only worth adding if (b)
turns out not to cover enough cases in practice. Do not build it
speculatively.

### 15.7 Is the standard library "just a bunch of FFI wrappers"? — measured

**No.** An earlier draft of this document said so, and the measurement does
not support it. Counted across all 23 modules in `lib/std/`:

| | Count |
|---|---|
| `extern` declarations | **74** |
| native TurfLang functions / operators / methods | **233** |
| native structs | 15 |
| total lines of TurfLang | 4,208 |

Per module:

| Module | extern | native | Character |
|---|---:|---:|---|
| `list` `map` `set` `stack` `queue` | 0 | 26 | **pure TurfLang** (they call `mem`, which does the FFI) |
| `array` `bool` `char` `double` `int` | 0 | 40 | **pure TurfLang** |
| `json` | 0 | 29 | **pure TurfLang** — a hand-written 590-line parser/serializer |
| `http` | 0 | 12 | **pure TurfLang** over `net` |
| `test` | 0 | 21 | **pure TurfLang** |
| `strings` | 9 | 27 | mixed — libc for `strlen`/`strcpy`, real logic on top |
| `fs` | 5 | 10 | mixed |
| `net` | 11 | 11 | thin — a socket API is inherently a syscall wrapper |
| `io` | 13 | 18 | thin-to-mixed |
| `mem` | 10 | 10 | thin by design (checked wrappers over `malloc`/`free`) |
| `math` | 13 | 14 | **genuinely thin** — `fn abs(v: double): double { return fabs(v); }` |
| `os` `time` `random` `sys` | 13 | 15 | thin |

So the accurate statement is: **roughly a quarter of the standard library is
FFI declarations, concentrated in the modules where a wrapper is the correct
design (syscalls, libm, the allocator), and the majority is ordinary
TurfLang** — including the two largest modules, `json` (590 lines) and
`strings` (557 lines), and every collection.

This matters for three decisions elsewhere in this document:

1. **§15.9 (`string` stays a byte pointer)** still holds — the FFI surface is
   smaller than claimed but `string` is still the parameter type for most of
   it, and `io`/`fs`/`os`/`net` are exactly the modules that are thin.
2. **§14.3 (non-moving GC)** still holds, for the same reason.
3. **The "privileged stdlib" problem (§15.2) is narrower than it looks.**
   Only the modules with `extern` declarations genuinely need the raw-pointer
   capability. The collections need it only because `List<T>`'s `data: T*`
   field is a raw pointer — and **after TYPE-002 + LANG-002 that field can
   become a safe type**, at which point five modules stop needing any
   privilege at all.

### 15.8 Where FFI *is* actually a hurdle

FFI is not slowing TurfLang down as a design. Declaring externs works well,
including cases many young languages get wrong — verified live: function-
pointer parameters, struct-by-value parameters and returns, `extern from
"lib" fn` with automatic `-l` linking, and `int32` for honest 32-bit C ABI.

Four specific gaps are real, and three of them are cheap:

| # | Gap | Evidence | Severity |
|---|---|---|---|
| **F1** | **A C callback cannot be written in user code.** A comparator for `qsort` must have `ptr` parameters, and `checkNoRawPointersInSignature()` rejects `ptr` in *any* non-`extern` user signature — `isRawPointer()` is `isPointer() && !isReference()`, which includes `OpaquePointerType`. Verified: `fn cmpints(a: ptr, b: ptr): int32` → E066 *"Raw pointer in function signature!"*, even inside `unsafe { }`. | direct compile | **High** — it makes every callback-taking C API (`qsort`, `bsearch`, signal handlers, `pthread_create`, most GUI and event libraries) unusable from user code |
| **F2** | **A second `extern fn` inside a top-level `unsafe { }` block fails to parse** — *"Expected ';' after statement"* on the second declaration. The first parses fine; two at top level outside `unsafe` are fine. | direct compile | **Medium** — a real parser bug, previously undocumented |
| **F3** | `string` does not coerce to a `ptr` parameter — `extern fn puts(s: ptr); puts("hi");` → *"No overload of 'puts' matches argument types (string)"*. Workaround: declare the parameter as `string`. | `DESIGN_NOTES.md` §4 + direct compile | **Low** — documented, with a working workaround |
| **F4** | `extern fn` may not be declared inside a function body. | `DESIGN_NOTES.md` §4 + direct compile | **Low** — style only |

**F1 is the one that matters**, and it is the FFI half of the same problem
LANG-002 already exists to solve: TurfLang has no way for user code to
legitimately hold a raw pointer. The fix is the same declared-capability
mechanism (§15.2) — a module that declares the raw-pointer capability may
write `ptr`-taking functions, which is precisely what a callback needs.
**Fold F1 and F2 into LANG-002** rather than creating a separate group; add
a `tests/05_arrays_pointers_ffi/` test that round-trips a real `qsort`
callback as the completion criterion.

### 15.9 `string`'s representation — FROZEN

`string` stays a NUL-terminated byte pointer, identical in representation to
`ptr`/`T*`. A fat `{data,len,cap}` string would give `O(1)` length and safe
slicing — and would require a conversion at every one of the hundreds of
`extern fn` boundaries that make up the standard library, breaking the
"first-class C interoperability" core principle the roadmap lists first.
`strings::length` stays `O(n)`; a `StringBuilder` in the stdlib covers
incremental building; `Bytes`/`Slice<T>` (natural after TYPE-002) covers
non-owning views. Under MEM-001 a `string` becomes GC-allocated, which is
where the actual problem (leaking) gets solved.

---

## 16. SLM Diagnostic Architecture

### 16.1 Current architecture

§1.12. The parts that are genuinely well-designed and must be kept:

- **Grammar-constrained output** (`grammars/slm_suggestion.gbnf`) makes
  malformed JSON unrepresentable rather than handled.
- **Sandbox verification** (`src/Sandbox.cpp`) actually compiles the
  suggested patch and confirms the original error is gone before showing it.
- **Confidence cannot be faked.** `confidence` is deliberately *not* part of
  the JSON the model emits (a pre-filled field would be echoed verbatim);
  it is computed after parsing, and `finalizeConfidence()` caps an
  unvalidated fix at 0.5 against a 0.8 display gate, so **an unproven
  suggestion can never auto-apply**.
- **Retry with real feedback** — the sandbox's own diagnostics from a failed
  attempt are fed back as `previousAttemptFailed` rather than re-prompting
  blind.
- **Echo detection** — a patch identical to the original text scores a hard
  zero.
- **Context from structured data, not source scanning** — the README
  documents the replacement of an older pattern-matching approach.

### 16.2 Weaknesses

| Weakness | Evidence |
|---|---|
| **Classification partly by text matching** | `extractContextKeyword()`'s comment chain documents several bugs: `"for enum"` matching the `for` keyword, `"has no method named"` needing a special case, eight former intrinsics wrongly routed. |
| **No caching** | Every invocation reloads and re-infers. Identical errors across a build pay full latency each time. |
| **No deterministic fixer** | Many diagnostics already carry a `SuggestionIndex` "did you mean X" with a *provably correct* replacement. Today they still go to the model. |
| **No type context in the prompt** | The prompt is textual; `NodeTypes_` holds the inferred types of exactly the expressions in question and is never consulted. |
| **No measurement** | There is no benchmark corpus and no reported accuracy, so "is it getting better?" is unanswerable. |

### 16.3 Proposed changes

**Keep, unchanged and frozen:** grammar constraint, sandbox verification, the
confidence gate, the retry-with-diagnostics loop.

1. **Deterministic-fix layer, before the model** (DIAG-001 + SLM-001). Where a
   diagnostic already carries a unique, high-confidence suggestion (a single
   Damerau–Levenshtein match, a missing `;` at a known position, a missing
   `import` for a name that exists in exactly one module), synthesize the
   patch directly, sandbox-verify it, and never invoke the model. **Expected
   to handle a large fraction of real errors at zero latency** — U-5 measures
   which.
2. **Structural classification** (DIAG-001). Delete text matching; every
   `TurfError` subclass supplies its code and category.
3. **Type context in the prompt.** Add the inferred types of the expressions
   at the error location from `NodeTypes_`. For a type-mismatch error — the
   most common category — this is the single most relevant fact and it is
   currently absent.
4. **Caching.** Key on (error code, normalized message, hash of pinned
   context lines) → suggestion, persisted under `~/.cache/turf/slm/`.
   Invalidate on compiler version.
5. **A benchmark corpus and reported metrics.** `DESIGN_NOTES.md` §6 already
   describes a procedural mutator (take passing tests, break them
   mechanically, capture the real diagnostic). Build that as the benchmark
   *first*, independently of any fine-tuning plan.

### 16.4 Measurable success criteria

Report these per release, on the fixed corpus:

| Metric | Definition | Target direction |
|---|---|---|
| **Deterministic-fix rate** | % fixed with no model invocation | up |
| **Sandbox-validation rate** | % of model suggestions that compile and clear the original error | up |
| **Semantic-correctness rate** | of validated fixes, % where the mutated test's own asserts still pass — i.e. the fix restored *meaning*, not just compilability | up; this is the metric that matters most and is currently unmeasured |
| **No-suggestion rate** | % where nothing is shown | acceptable if honest |
| **False-confidence rate** | % of *displayed* suggestions that were wrong | must stay ~0 |
| **p50 / p95 latency** | wall clock per diagnostic | down |
| **Cache hit rate** | | up |

The semantic-correctness metric is the important addition: a patch that
compiles and silences the error but changes behaviour is currently
indistinguishable from a good one, and the mutator-based corpus makes it
measurable for free because the original, correct program is known.

### 16.5 What must not change

- **Sandbox verification stays.** Even with a better model. It is the only
  thing standing between a hallucination and a user's source file.
- **The confidence gate stays.** Unvalidated ≤ 0.5, gate at 0.8.
- **No new `ErrorCategory` values and no new `docs/slm/` files** without an
  explicit request. That expansion is deliberately deferred.

---

## 17. Optimization Architecture

### 17.1 Where each responsibility belongs

| Layer | Owns |
|---|---|
| **TurfLang frontend** | Constant folding on untyped literals (SEM-001); acyclicity and layout (TYPE-001); nothing else — the frontend should not "optimize", it should produce correct, simple IR. |
| **TurfLang middle-end** (new) | Monomorphization and the per-instantiation specialization it enables (TYPE-002); bounds-check and overflow-check elimination driven by `ValueRangePass` (OPT-002); dead-instantiation pruning. These are the things that need TurfLang semantics. |
| **LLVM IR generation** | Emit checks explicitly (never rely on a pass to add them); emit `nsw`/`nuw` only where semantics permit; emit debug locations (TOOL-001). |
| **LLVM optimization** | Everything general: SROA, mem2reg, instcombine, GVN, LICM, inlining, unrolling, vectorization, DCE, CFG simplification. Do not reimplement any of it. |
| **Runtime** | An allocator fast path worth optimizing in C, not in IR. |
| **Standard library** | Algorithmic choices (`Map`'s linear scan should become hashed — an algorithmic fix, not an optimizer's job). |

### 17.2 Levels

| Flag | Contents | Intent |
|---|---|---|
| `-O0` | verify + `AlwaysInliner` only | fast builds; faithful `-g` |
| `-O1` | SROA, mem2reg, InstCombine, SimplifyCFG, EarlyCSE, Internalize + GlobalDCE | iteration |
| `-O2` *(default)* | `PassBuilder::buildPerModuleDefaultPipeline(O2)` + TurfLang middle-end passes | shipping |
| `-Os` | `…(Os)` | size |
| `-O3` | `…(O3)` | throughput |

`-O2` stays the **default** so no current user sees a regression.
`CodeGenOptLevel` is derived from the flag rather than left at its default.
`--target` and `-mcpu` replace the hardcoded host triple and `CPU="generic"`.

### 17.3 The correctness contract — FROZEN

> Optimization levels change compile time, run time, binary size and
> debuggability. They **never** change observable behaviour.

Concretely, and this is the part that is easy to get wrong:

- Array bounds checks, division-by-zero checks and integer-overflow traps are
  **language semantics**, emitted by Codegen at every level including `-O0`.
- They may be removed **only by proof** — `ValueRangePass` showing the index
  is in range, or the operands' ranges precluding overflow. Never by flag.
- `nsw`/`nuw` may be attached **only** where the trap has already been
  emitted or proved unnecessary. Attaching `nsw` and then relying on UB would
  contradict F-NUM-3 and is forbidden.
- The `-O`-equality smoke test (OPT-001) is the executable form of this
  contract: the same programs must produce identical output and exit codes at
  every level.

*"A faster miscompilation is not an optimization"* — and in this codebase the
specific way that would happen is a well-meaning `nsw` annotation.

### 17.4 Measured baseline to beat

| Metric | Today |
|---|---|
| empty `main` binary | 29,968 bytes |
| unused `turf_*` functions in it | 84 |
| hello-world compile | ~0.3 s |
| module-level passes | 0 |
| inlining | none |
| SROA | absent |

`Internalize` + `GlobalDCE` alone should remove the 84 dead functions. SROA
matters disproportionately here because TurfLang structs are value types
lowered to whole-aggregate loads and stores (`memory_model.md` §2.5.3) —
exactly the pattern SROA exists to scalarize, and it is currently not run at
all.

### 17.5 Things deliberately not planned

- **PGO** — no consumer, and no benchmark suite to profile against yet.
- **LTO** — there is no separate compilation, so the whole program is already
  one module. LTO becomes meaningful only if a package system arrives.
- **Devirtualization** — TurfLang has no dynamic dispatch at all. Say so
  rather than leaving a speculative slot.
- **A TurfLang-level inliner** — LLVM's is better and already exists.

---

## 18. Handbook Synchronization System

### 18.1 The problem, with evidence

The handbook is paused *because* the compiler is about to change. When it
resumes, a writer must not have to re-read 36,900 lines of C++ to find out
what changed. And knowledge in this repo demonstrably does get lost:
**43 source comments across 22 files cite `docs/planning/Errors.md` as the
design authority for the error/`!T`/`recover` feature, and that file does not
exist.** Six other `docs/` paths are dangling too.

This is not a generic changelog problem. A git log answers "what changed in
this file"; a handbook writer needs "what does the language *mean* now, what
did it mean before, which chapter is now wrong, and which examples no longer
compile."

### 18.2 Structure

```
docs/development/
    FREEZE.md              §6 — the frozen decisions. Read first, every session.
    INVARIANTS.md          named, checkable invariants (INV-LAYOUT-1, …)
    INDEX.md               table of every change record: ID, date, status, subsystems
    TEMPLATE.md            the record template (§18.3)
    changes/
        ARCH-001.md
        TYPE-001.md
        SEM-001.md
        ...
    handbook-sync/
        PENDING.md         queue of handbook edits owed, keyed by chapter/file
        DONE.md            archive, with the commit that did each one
```

`docs/planning/` (roadmap, design notes, limitations) and
`docs/architecture/` (subsystem internals) keep their current roles
unchanged. `docs/development/` is **additive** and is specifically about
*change over time*, which neither of the others tracks.

### 18.3 The record template

```markdown
# <FEATURE-ID> — <name>

| | |
|---|---|
| **Status** | planned / in-progress / landed / abandoned |
| **Dates** | started YYYY-MM-DD · landed YYYY-MM-DD |
| **Commits** | <sha> … |
| **Sessions** | 1 of 3 |
| **Depends on** | <FEATURE-ID>, … |
| **Frozen decisions relied on** | F-GEN-1, F-NUM-3, … |

## Why
The problem, in the user's terms. What was broken or missing, with a
reproduction if there was one.

## Language semantics
### Old behaviour
Exact, with an example that compiled and what it did.
### New behaviour
Exact, with the same example.
### Is this breaking?
yes/no · who it breaks · the mechanical migration

## Compiler architecture
Pass-by-pass. Only the passes that changed.
- **Lexer** ·  **Parser** ·  **AST** ·  **Types** ·  **Resolver** ·
  **Symbol tables** ·  **TypeChecker** ·  **Monomorphizer** ·  **Codegen** ·
  **CFG/analysis** ·  **Diagnostics** ·  **Optimization**

## Runtime changes
## Standard-library changes
## New invariants
Added to INVARIANTS.md, with what enforces each one.
## Removed assumptions
Things that used to be safe to assume and no longer are. **The most
valuable section for a future reader.**

## Tests
Added · changed · deleted, with paths and what each proves.

## Known pitfalls
Anything a future implementer could plausibly get wrong here. Written the way
the handbook's `pitfall` blocks are written — concrete, with the real failure.

## Handbook impact
| File / chapter | What is now wrong | Suggested replacement |
|---|---|---|
### Examples that became invalid
### Examples that should be added
### Sections that must NOT change
(Guarding against over-correction — e.g. a chapter's *reasoning* staying
correct while only its code listing changed.)

## Unresolved questions
## Related records
```

### 18.4 How sessions use it

A session's work is **not complete** until:

1. It has read `FREEZE.md` before starting.
2. `changes/<ID>.md` exists and is filled in — not a stub.
3. `INDEX.md` has its row.
4. `handbook-sync/PENDING.md` has an entry per affected chapter/file.
5. `INVARIANTS.md` has any new invariant, with what enforces it.
6. The doc-link check passes.

§22's session template repeats this as an explicit completion criterion so it
cannot be quietly skipped.

### 18.5 Enforcement

- `scripts/check_doc_links.sh` in CI — every `docs/…md` path referenced from
  `src/`, `include/`, or `docs/` must exist. This is what would have caught
  the `Errors.md` problem years earlier.
- A CI check that every feature ID mentioned in a commit message has a record
  in `changes/`.
- `INDEX.md` is the one place to look to answer "what has changed since the
  handbook was paused?"

### 18.6 Worked example (abbreviated)

```markdown
# SEM-003 — `guess` type inference

| **Status** | landed | **Frozen decisions relied on** | F-GUESS-1/2/3, F-NUM-1 |

## Why
Every declaration required an explicit type annotation. `mut x = 5;` was a
parse error ("Expected ':' after variable name."). This is the single most
commonly noticed ergonomic gap.

## Language semantics
### Old
`mut x: int = 5;` required. `mut x = 5;` → parse error.
### New
`mut x = 5;` infers `int`, permanently. `mut x: guess = 5;` is the same
thing, spelled explicitly.
### Breaking?
Only for a program using `guess` as an identifier (now a keyword). Swept
`tests/`, `lib/std/`, `showcase/` — zero occurrences.

## Removed assumptions
- **"Every `VarDeclExprAST` has a non-null `Type` after parsing" is no longer
  true.** Anything reading `getType()` between Parser and TypeChecker must
  handle null. `ASTPrinter`, `Lint`, `CompileBuffer` and `Sandbox` were all
  audited.

## Handbook impact
| File | Now wrong | Replacement |
|---|---|---|
| `book/ch12` §Declaring Variables | *"The `: Type` annotation is not optional, and there is no inference form"* | Describe `guess`, Model A, and why Model B was rejected |
| `docs/reference/variables_and_types.md` §Variable declarations | same | same |
### Sections that must NOT change
ch12's `immut` initializer-required rule — unaffected by inference.
```

---

## 19. Handbook Pitfall Conversion Plan

The handbook's `pitfall` blocks are real engineering input. Each is
classified, and — per the prompt's instruction to prefer structural
prevention — each gets an **invariant** and a **place that enforces it**,
not just a test.

| # | Pitfall (source) | Status | Why it happened | Invariant | Enforced in | Test category | Handbook |
|---|---|---|---|---|---|---|---|
| 1 | **Generic substitution skipped** — a `NamedType("T")` never became a `TurfGenericParamType`, so `typesCompatible()`'s `dynamic_cast` fast path missed it and `identity(42)` failed overload resolution (ch05) | **Architecturally superseded** | Two representations for "a type parameter" | **INV-GEN-1**: after the monomorphizer, no `TurfGenericParamType` exists anywhere | Monomorphizer's final assertion (TYPE-002 step 5) | `tests/38_monomorphization/` | Rewrite: the pitfall becomes historical |
| 2 | **Array type identity ignored size** — `ArrayType::getName()` omits the size, so `int[5]` and `int[10]` compared equal, and Codegen's identical `getName()` fast path stored 80 bytes into a 40-byte alloca — **a reproduced stack buffer overflow** (ch06) | **Fixed** | Type *identity* derived from a *display* string | **INV-TYPE-1**: type identity never uses `getName()`; use `isEquivalent()` | `typesCompatible()`, `CastToType()` | `tests/03_typechecker/` array-size mismatch, both directions | Keep as-is; it is a good pitfall |
| 3 | **Repeated `setBody()` / GEP against an opaque struct** — a method compiled before its struct's layout existed baked "zero fields" into a GEP; `verifyModule()` rejected it, but only *after* that call was fixed to check its return value (ch07) | **Fixed, hazard remains** | Ordering dependency between two codegen stages, enforced only by `main.cpp`'s pre-pass structure | **INV-CG-1**: no GEP may be built against a struct type whose `setBody()` has not run | An assertion at GEP construction in `CodegenStruct.cpp`, **not** by relying on pre-pass ordering | `tests/06_structs/` method-before-struct | Keep |
| 4 | **`LintExpression()` has no `VarDeclExprAST` branch** — every lint check is silently skipped for the entire right-hand side of every declaration; `mut x: int = 5 / 0;` warns about nothing (ch09) | **Still present** | A visitor dispatch with a silently missing case | **INV-LINT-1**: `LintExpression`'s dispatch is exhaustive over statement-shaped nodes | DIAG-001; prefer a switch over a node-kind enum, or a visitor base that logs an unhandled kind in debug builds, so the *next* missing case is loud | `tests/14_cfg_analysis/` | Rewrite as historical once fixed |
| 5 | **Two default-initialization mechanisms** (ch12) | **Being redesigned** | Accretion; the split was never designed | **INV-INIT-1**: exactly one initialization rule, for every type | SEM-004 (F-INIT-1) | `tests/10_default_init/` rewritten | Rewrite the whole section |
| 6 | **`bool` has almost no cast surface** — classified numeric internally for `&&`/`\|\|` but isolated from coercion, and `to` refuses to bridge it either (ch12) | **Intentionally retained** | A deliberate design decision | **INV-NUM-1**: `bool` never participates in numeric coercion, implicit or explicit | `typesCompatible()` Rule 4 | existing `tests/03_typechecker/` | Keep; strengthen the wording from "surprising" to "deliberate" |
| 7 | **`--x` lexes as one token**, so `---x` is `--` then `-x` (ch12) | **Intentionally retained** | Maximal munch, as designed | **INV-LEX-1**: maximal munch is uniform; no operator gets a special case | Lexer | `tests/01_syntax/` | Keep |

**Additional pitfalls found by this audit and not yet in the handbook** —
these should become `pitfall` blocks when the handbook resumes:

| # | Pitfall | Status | Invariant | Enforced in |
|---|---|---|---|---|
| 8 | **Numeric literal overflow silently clamps** (`strtoll`, `errno` unchecked) | Live bug | **INV-NUM-2**: a literal's value is never lost between lexer and type check | SEM-001/002 |
| 9 | **Infinitely-sized structs compile, then segfault** | Live bug | **INV-LAYOUT-1**: every `TurfStructType` has a finite size | TYPE-001 |
| 10 | **Erasure ICEs above 8 bytes** (`Invalid bitcast`) | Live bug | INV-GEN-1 | TYPE-002 |
| 11 | **`evalConstExpr` overflows `long long`** — signed-overflow UB in the compiler's own C++ | Live bug | **INV-CONST-1**: all compile-time arithmetic uses checked, arbitrary-precision operations | SEM-001 |
| 12 | **The stdlib's raw-pointer privilege is a file path** | Live design flaw | **INV-CAP-1**: no capability is granted by path | LANG-002/STD-001 |
| 13 | **`NodeTypes_` is authoritative and unread** — Codegen re-derives every type via a second engine | Live duality | **INV-TYPE-2**: for monomorphized nodes, the AST carries concrete types so both engines agree | TYPE-002 §9.7 |
| 14 | **Two same-string-literal keys can pass a `Map<string,…>` test by linker merging** — `LIMITATIONS.md` warns about this explicitly | Live test hazard | **INV-TEST-1**: any string-keyed collection test uses a runtime-built string | test convention, documented in the folder | TYPE-002's tests |

Pitfall 14 is worth singling out: it is a *testing* invariant, and it already
caused a real bug to pass undetected
(`tests/07_functions_modules/09_stdlib_map.tr` passed with the bug present).

---
## 20. Cross-Feature Interaction Matrix

Only genuine interactions are listed; blank means "no meaningful
interaction", which is itself useful information.

| ↓ affects → | Generics (TYPE-002) | GC (MEM-002) | `guess` (SEM-003) | Numeric (SEM-001/2) | Init (SEM-004) | Operators (LANG-001) | Optimization (OPT-001/2) | SLM | Stdlib | Runtime (RT-001) |
|---|---|---|---|---|---|---|---|---|---|---|
| **Generics** | — | **Enables it.** Per-instantiation types → pointer bitmaps. Removes `IntToPtr` fake pointers. **Hard dependency.** | Cannot infer a type argument; must error, not silently leave a generic open | `List<int32>` gets real 32-bit arithmetic + 32-bit overflow checks | Acyclicity re-checked per instantiation (a substituted `T` can create a by-value cycle) | **Fixes operator dispatch inside generic bodies** — `LIMITATIONS.md` §1/§2 close for free | Code growth; offset by GlobalDCE; enables per-instantiation specialization + inlining | Instantiation-backtrace diagnostics become a new error shape the SLM must handle | `cap*8`→`cap*sizeof(T)`; `List<T*>` workaround retired; `list_sort` can become generic | Type descriptors are impossible without it |
| **GC** | needs it | — | | | zero-fill guarantee (F-INIT-2) is what makes a null pointer field valid pre-assignment | an allocating `operator+` allocates on the GC heap | conservative scanning must survive register allocation and spilling; descriptors must survive GlobalDCE | | strings/collections become collected; `mem::*` does not | lives entirely in it |
| **`guess`** | | | — | inference picks the *default* type first, **then** representability is checked — this ordering is the whole of the `mut x = 999…9` answer | complementary: `guess` always has an initializer so definite assignment never applies to it | | | a new error shape ("inferred here") with a `withRelated()` note | do **not** rewrite `lib/std` to use it in the same change | |
| **Numeric** | | | | — | zero values per type (§12.5) | user operators do **not** inherit built-in numeric rules (no free div-by-zero check on `operator/`) | overflow traps are the main new cost; OPT-002's range analysis is how it is recovered | new lexical + representability error classes | `random.tr`'s LCG almost certainly relies on wrapping — **find this first** | trap edge calls `turf_rt_panic` |
| **Init** | | | | | — | | a zeroed-then-overwritten alloca is removed by SROA/DSE at `-O1`+ | E008's message should suggest `T::default()` | migration volume in `lib/std` is **U-1**, unknown | |
| **Operators** | needs it | | | | | — | operator calls become inlinable (today every `"a"+"b"` is a real call) | | collections gain `[]`/`[]=` | |
| **Optimization** | | must not delete descriptors; must not break conservative roots | | must not add `nsw` where a trap is the semantics | | | — | | | runtime built `-g` for backtraces |
| **SLM** | | | | | | | | — | | |
| **Stdlib** | | | | | | | | needs `@caller_location` to attribute panics | — | sits on it |
| **Runtime** | | is it | | panic target | | | | | | — |

### The interactions that actually decide the ordering

1. **Generics → GC.** Precise tracing is impossible under erasure, and
   erasure's `IntToPtr` actively produces integers that look like heap
   addresses. This is the single edge that shapes the whole roadmap.
2. **Numeric → `guess`.** `mut x = 999999999999999999999;` requires knowing
   *both* how a literal is represented *and* how a type is chosen, and the
   order between them. The answer (default type first, then representability)
   is frozen in F-GUESS-1 + F-NUM-2 together, not in either alone.
3. **Init → GC.** "Storage is always zeroed" is why a pointer field always
   reads as a valid null before assignment. If SEM-004 had removed the
   zero-fill along with Mechanism B, MEM-002 would have inherited a tracing
   hazard. F-INIT-2 exists as a *separate* frozen decision precisely to stop
   that.
4. **Generics → Operators.** Sequencing LANG-001 after TYPE-002 is what
   prevents the wrong fix (a trait system) from looking necessary.
5. **`defer` → GC.** F-MEM-2's "no finalizers" is only defensible if `defer`
   exists first.

---

## 21. Milestone Plan

Internal identifiers only. **No release version is predicted or implied.**

### Milestone M1 — "No silent wrong answers"
`ARCH-001` · `TYPE-001` · `SEM-001` · `SEM-002` · `DIAG-001` · `HYG-001`

*Characteristic:* the compiler no longer produces a silently wrong value or
crashes on plausible input. Every verified finding in §0.2 items (4) and (5)
produces a diagnostic or a trap.

*Observable:* `mut x: int = 999…9;` errors. `struct A { me: A }` errors.
`INT64_MIN` is writable. Hex/binary/exponent literals work.

### Milestone M2 — "Comfortable to write"
`OPT-001` · `TOOL-001` · `SEM-003` · `SEM-004` · `LANG-003` · `LANG-004`

*Characteristic:* the language is pleasant. Inference, one initialization
rule, `defer`, real optimization levels, and a debuggable binary.

*Observable:* `mut x = 5;` works. `turf -O0 -g` produces something `gdb` can
step. The empty-`main` binary loses its 84 dead functions. `for i in 0..5 { }`
compiles without a `step` clause.

### Milestone M3 — "Generics that work"
`TYPE-002` · `LANG-001` · `LANG-002`

*Characteristic:* the foundation stone. Monomorphization, a complete operator
set, and safe nullable references.

*Observable:* `List<Big>` with a 24-byte struct works. `Map<string,V>`
compares by content. `sizeof(T)` works in a generic body. A linked list can be
written in safe TurfLang. `set_string_dedup_broken.tr` moves out of
`tests/99_known_bugs/`.

### Milestone M4 — "A managed language"
`RT-001` · `MEM-001` · `MEM-002` · `STD-001` · `STD-002`

*Characteristic:* TurfLang has a runtime and does not leak. The
compiler/runtime/library boundary is declared and enforced.

*Observable:* a server loop's RSS is bounded. `test::assert` reports the
caller's line. `IsPathUnderLibStd` no longer exists.

### Milestone M5 — "Polish"
`OPT-002` · `SLM-001` · `STD-003`

*Characteristic:* the cost of M1's safety is recovered; the diagnostic
assistant is measurable.

### What distinguishes the resulting generation from the current one

Not a feature count. Four structural properties the current generation does
not have:

1. **It has a runtime.** Today there is no `libturfrt` and no process-wide
   owner of anything.
2. **It has a defined numeric and initialization semantics**, rather than
   behaviour that falls out of `strtoll` and a type-exclusion list in the
   parser.
3. **It has one generic representation, and it is sound for every type** —
   rather than one that ICEs above a machine word and silently breaks
   equality.
4. **Its capability boundaries are declared**, rather than inferred from a
   filesystem path.

A useful shorthand: the current generation is a compiler with a library; the
resulting one is a *language implementation*. Whether that warrants a major
version number is not this document's call.

---

## 22. Claude Code Session Breakdown

Each session below is independently executable. Hand a session **this
document's §6 plus its own feature-group section from §8**, and nothing else
should be needed.

### 22.0 The Session Start Protocol — mandatory, in this order

This exists because a session that starts by opening source files has already
skipped the two things that matter most. It is deliberately ordered, and
**step 1 comes before any repository exploration at all.**

`ARCH-001` makes this enforceable rather than aspirational, by creating:

- **`docs/development/SESSION_PROTOCOL.md`** — the canonical copy of the
  steps below. This document's copy is a snapshot; that file is authoritative
  once it exists.
- **`scripts/session_preflight.sh <FEATURE-ID>`** — prints the exact reading
  list for that feature ID, and *verifies the starting state* (clean tree,
  build succeeds, suite green) so a session never mistakes a pre-existing
  failure for one it caused.
- **`scripts/session_postflight.sh <FEATURE-ID>`** — mechanically checks the
  completion criteria below and exits non-zero if any is unmet.
- **one pointer line in `AGENTS.md`** directing any agent to
  `docs/development/SESSION_PROTOCOL.md` when the task is an overhaul feature
  group. AGENTS.md remains the source of truth for build/test conventions;
  this adds a pointer, nothing more.

#### Step 1 — Read `AGENTS.md`, in full, first

Before anything else. It is the source of truth for build commands, the test
runner's conventions, commit-message format, and the repository's own stated
engineering rules. **This document deliberately does not restate any of it**,
so a session that skips AGENTS.md will get the workflow wrong.

Three things in it bite hardest and are worth re-reading even if you think
you know them:

- `scripts/test_runner.sh` **silently excludes** any filename containing
  `_module.tr` as a bare substring. A real test named that way is skipped
  with no failure and no output.
- The runner covers **only** `tests/`. `lib/std/**` and `showcase/**` are not
  exercised by it and can sit broken indefinitely with the suite green. AGENTS.md
  records a confirmed instance of exactly that.
- Compiling one of the six prelude modules directly (`turf --resolve
  lib/std/core/char.tr`) is the **only** regression coverage for the
  prelude-self-import bug class.

#### Step 2 — Read `docs/development/FREEZE.md` (§6 of this document)

Every decision in it is settled. If you believe one is wrong, **stop**: write
the argument into `docs/development/changes/<ID>.md` under *Unresolved
questions* and ask. Do not implement a different interpretation and explain
it afterwards.

#### Step 3 — Read your feature group's section in §8, and every section it names

Each group ends with a **"Must NOT reinterpret"** list. That list is the
short version of everything above.

#### Step 4 — Read the relevant `docs/architecture/*.md`

AGENTS.md's own instruction: *"Read the relevant one before making
non-trivial changes to that subsystem — don't re-derive from scratch."*
Map: `compiler_pipeline.md` for pass structure and CFG analysis;
`memory_model.md` for storage and lifetime; `modules_and_scope_resolution.md`
for `::`/imports; `pointers_and_references.md` for `unsafe`/`&T`;
`errors_and_recover.md` for `error`/`!T`/`recover`.

#### Step 5 — Run `scripts/session_preflight.sh <FEATURE-ID>`

Confirms a clean tree, a successful build, and a green suite **before** you
change anything.

### 22.1 While working

Follow the repository's stated conventions, which AGENTS.md owns: doc
comments explain **why** (a hidden invariant, a past bug this prevents) and
never restate the code; every fix gets a regression test in `tests/<NN_topic>/`
covering both directions where it makes sense; small scoped commits, one
logical change each; `type(scope): summary (@username)` titles; **no
`Co-Authored-By` or other AI-attribution trailer**.

And the rule this project holds hardest: **verify claims live.** If you are
about to write "X is enforced" in a doc comment or a change record, first
compile the specific input that would prove it wrong.

### 22.2 Before declaring done — the completion checklist

`scripts/session_postflight.sh` checks all of these:

1. `./scripts/test_runner.sh` green.
2. `lib/std/**` and `showcase/**` compiled **directly** (the runner covers
   neither).
3. The `*_module.tr` naming sweep run in **both** directions, if any test file
   was added or renamed.
4. `docs/development/changes/<ID>.md` filled in — **not a stub**. The
   *Removed assumptions* and *Known pitfalls* sections in particular, since
   those are what a future reader cannot reconstruct from a diff.
5. `INDEX.md` row added; `handbook-sync/PENDING.md` entry per affected
   chapter or reference file; `INVARIANTS.md` updated with any new INV-*.
6. `scripts/check_doc_links.sh` passes.
7. The group's own **Completion criteria** from §8 are each individually
   demonstrated, not asserted.

---

**S1 — ARCH-001** · no prerequisites · *"Set up change tracking."*
Scope: create `docs/development/**`; write `FREEZE.md` from §6; write
`TEMPLATE.md` from §18.3; write `check_doc_links.sh`; fix the 7 dangling
references (re-point the 43 `Errors.md` comments to
`docs/architecture/errors_and_recover.md`); note C-3 in the ROADMAP.
Next session may assume: the freeze exists and is authoritative.

**S2 — TYPE-001** · after S1 · *"Reject infinitely-sized structs."*
Stages: (1) build the containment graph at the end of Resolver Pass 1;
(2) the cycle DFS + error class + code + `docs/errors/codes/`;
(3) the five tests in §8; (4) stdlib/showcase sweep.
Next may assume: **INV-LAYOUT-1**.

**S3 — SEM-001** · after S1 · *"Stop losing literal values."*
Stages: (1) grep every `getIntVal()`/`getDoubleVal()`/`NumVal`/`IntVal`
consumer, including `Lint::Fingerprint`, `ASTPrinter`, `clone()`, `Sandbox`,
`CompileBuffer`, `ResolverConstantSymbol`; (2) lexer rewrite with the new
forms + lexical errors; (3) `NumberExprAST` → `APSInt`/`APFloat`;
(4) `evalConstExpr` → checked `APSInt`; (5) `tests/35_numeric_literals/`.
Next may assume: **INV-NUM-2**, **INV-CONST-1**.

**S4 — OPT-001** · after S1 · *"Real optimization levels."*
Stages: (1) `PassBuilder` migration at the current effective level, suite
green — no behaviour change; (2) levels + CLI + `CodeGenOptLevel`;
(3) `Internalize`+`GlobalDCE` with an explicit must-preserve set, validated
against `showcase/llvm_ffi_selfhost_demo.tr`; (4) `--target`/`-mcpu`;
(5) `scripts/opt_levels_smoke.sh`; (6) record the size table.
Next may assume: F-OPT-1 is testable.

**S5 — TOOL-001** · after S4 · *"Debug info."*
Stages: (1) `DIBuilder` + compile unit + subprograms, `-g` flag;
(2) locals/parameters/`llvm.dbg.declare`; (3) composite types;
(4) `DILocation` on every instruction (`verifyModule` will tell you what is
missing); (5) `scripts/debuginfo_smoke.sh`.

**S6 — DIAG-001** · after S1 · *"Structural classification + lint gap."*
Stages: (1) enumerate every diagnostic's current category, including how it
is currently derived; (2) fix the classes; (3) delete text matching;
(4) `LintExpression`'s `VarDeclExprAST` branch, ideally via an exhaustive
dispatch so the next missing kind is loud (**INV-LINT-1**); (5) tests.

**S6b — HYG-001** · after S1 · *"Hygiene."*
Stages: (1) forward-declare `llvm::AllocaInst` in `SymbolTable.h`, move the
include into `SymbolTable.cpp`, record the preprocessed-line delta;
(2) route the unreachable-code and dead-branch warnings through the existing
`UnreachableBlockWarning`/`DeadBranchWarning` classes; (3) check
`tests/14_cfg_analysis/` expectations against the one-per-line cap.

**S7 — SEM-002** · after S3 · *"Numeric semantics."*
Stages: (0) **audit `lib/std` for wrapping-dependent arithmetic first** —
`random.tr` is the likely one; (1) representability at every type-fixing
site; (2) unary-minus literal folding (`INT64_MIN`); (3) compile-time typed
overflow; (4) runtime checked arithmetic + trap; (5) move `to`-legality into
the TypeChecker; (6) `tests/36_numeric_semantics/`; (7) measure the `-O2`
cost and record it.

**S8 — SEM-003** · after S7 · *"`guess`."*
Stages: (0) **resolve the declare-before-type question** (§11.8);
(1) `TOK_GUESS` + `ParseType` + optional annotation in `ParseLetStatement`;
(2) Resolver accommodation; (3) `TypeChecker::visitVarDecl` inference;
(4) the error set; (5) `tests/37_guess_inference/`.
Do **not** rewrite `lib/std` to use it.

**S9 — SEM-004** · after S2 and S8 · *"One initialization rule."*
Stages: (0) **answer U-1** — sweep `tests/`, `lib/std/`, `showcase/` for
Mechanism-B reliance and record the count; if large, split into
deprecate→migrate→remove; (1) remove the `SynthesizeDefault` call;
(2) extend `DefiniteAssignmentPass` to all body-declared locals with the
array granularity from §12.6; (3) rewrite `tests/10_default_init/`;
(4) migrate `lib/std`/`showcase`.
Next may assume: **INV-INIT-1**.

**S10 — LANG-003** · anytime after S1 · *"`defer`."*
Stages: (1) token/AST/parse; (2) lowering at every block-exit edge, innermost
first; (3) `tests/40_defer/`.

**S10b — LANG-004** · after S1 · *"Ergonomics papercuts."*
Stages: (0) confirm the range-`for` inclusivity convention from
`docs/reference/control_flow.md`; (1) optional `step`, defaulting to `i++`;
(2) `string` → `ptr` coercion **at `extern` call sites only**;
(3) function-local `extern fn`; (4) tests in `tests/04_control_flow/` and
`tests/05_arrays_pointers_ffi/`; (5) close out `DESIGN_NOTES.md` §4's list.

**S11–S14 — TYPE-002** · after S2, S3; ideally after S4 · **four sessions.**

- **S11 — design + scaffold.** Answer **U-4** (does the pre-pass need the
  monomorphizer?) and **U-2** (measure the instantiation count across
  `tests/` + `lib/std`). Build the pass skeleton, the cache, the worklist,
  and the extended mangling. **No Codegen changes.** Deliverable: a pass that
  *collects and reports* every instantiation without acting on it, plus a
  written sub-plan for S12–S14 in the change record.
- **S12 — generic functions.** Clone → substitute → re-resolve → re-typecheck
  → register, for free functions and methods only. Keep the erasure Codegen
  path alive for structs. Instantiation-site diagnostics built here, not
  later.
- **S13 — generic structs.** Per-instantiation `TurfStructType` and LLVM
  layout; remove `FieldAccessExprAST::codegenLValue`'s base-name collapse.
  This is where the >8-byte ICE dies.
- **S14 — remove erasure + stdlib.** Delete `getLLVMType`'s and
  `CastToType`'s erasure branches and the per-call-site un-erase; add the
  no-`TurfGenericParamType` assertion; `cap*8`→`cap*sizeof(T)`; move
  `set_string_dedup_broken.tr` into the live suite; measure and record size +
  compile time.

Next may assume: **INV-GEN-1**, per-instantiation types, working
`sizeof(T)`, working operator dispatch in generic bodies.

**S15 — LANG-001** · after S14 · *"Operator completion."*
Stages: (1) extend the token/name/sanitize tables incl. arity-keyed unary;
(2) `%`, `**`, bitwise; (3) unary; (4) `[]`/`[]=` incl. the
`ArrayAccessExprAST` hook; (5) compound desugaring with single evaluation of
the LHS location; (6) the orphan rule (check `lib/std` first);
(7) `tests/18_operator_overload/`; (8) `List`/`Map`/`Set` subscripts.

**S16–S17 — LANG-002** · two sessions.
- **S16** — `TurfNullableReferenceType`, parsing, `typesCompatible()` rules,
  Codegen (no new representation), and the `NullabilityPass` with narrowing
  from direct comparisons only.
- **S17** — delete `IsPathUnderLibStd`; add the declared capability +
  `--allow-unsafe-modules`; annotate `lib/std`; the safe linked-list and tree
  tests.
Next may assume: **INV-CAP-1**; user code can write linked structures safely.

**S18 — RT-001** · after S14 · *"The runtime."*
Stages: (1) `runtime/` + CMake + `-lturfrt` + `TURF_HOME` layout;
(2) panic moved into it; (3) `turf_rt_alloc` as a `malloc` wrapper;
(4) type-descriptor emission + `include/Intrinsics.h`; (5) the `main` wrapper
and `turf_rt_init`; (6) `scripts/runtime_smoke.sh`; (7) update the release
packaging action.

**S19 — MEM-001** · after S18 · *"Route managed allocations."*
Stages: (1) enumerate and classify **every** `malloc` call site in `lib/std`
into GC-heap or manual-heap, in the change record; (2) reroute the GC-heap
ones; (3) `turf_rt_stats()`; (4) suite green with identical behaviour.

**S20–S22 — MEM-002** · after S19, S10; ideally after S5 · **three sessions.**
- **S20** — heap structure: block table (interior pointers!), allocation,
  free lists, `turf_rt_stats`. No collection yet.
- **S21** — mark phase: precise heap tracing from descriptors; conservative
  stack+register root scanning (`setjmp` spill); global roots. Answer **U-3**
  with a deliberate retention test.
- **S22** — sweep, collection triggering, `mem::root`/`unroot`, the
  free-a-GC-pointer diagnostic, the RSS stress test, and measured pause/
  throughput numbers.

**S23 — STD-001** · after S17, S18 · boundary enforcement, prelude
specification, and a script asserting no path-based privilege remains.

**S24 — STD-002** · after S14, S18 · `@caller_location`, error payloads, and
the assert-reports-caller-line test.

**S24b — STD-003** · after S14, S15, S17 · post-monomorphization stdlib
modernization: `sizeof(T)` sizing, retire the `List<T*>` workaround, generic
`sort`/`find`, `http.tr` simplification, `Map`/`Set` hashing, and **close out
`docs/planning/LIMITATIONS.md` entirely**.

**S25 — OPT-002** · after S4, S7, S14 · check elimination from
`ValueRangePass`; resolve `CallGraphPass::runGlobal()` (use it or delete it).

**S26 — SLM-001** · after S6 · benchmark corpus, deterministic-fix layer,
cache, type context, metrics.

---

## 23. Definition of Done

The architecture is complete when **all** of the following are true.

**Correctness**
1. No input in this document's verified-findings list produces a silent wrong
   answer, an ICE, or a segfault.
2. `struct A { me: A }` and the mutually-recursive pair are diagnosed.
3. A 24-byte struct works as `T` in every stdlib collection.
4. `Map<string,V>`/`Set<string>` compare by content, proved with
   runtime-built strings.
5. Integer overflow traps; literal overflow is a compile error; `INT64_MIN`
   is writable.

**Language**
6. `mut x = 5;` works; the type is fixed; reassigning a `double` errors with
   a note at the inferred declaration.
7. Exactly one initialization rule, for every type.
8. A linked list, a tree and a graph are writable in safe TurfLang, with
   functions taking and returning them.
9. `defer` runs on every exit path, innermost first.
10. Every operator in §13.1's "after" column is overloadable; the
    never-overloadable list is enforced.

**Memory**
11. A loop allocating strings and collection entries has bounded RSS.
12. A GC pointer survives an FFI call across a collection.
13. `mem::alloc`/`mem::free` still work, untraced, unchanged.
14. Freeing a GC pointer is diagnosed.

**Boundary**
15. `IsPathUnderLibStd` does not exist; capabilities are declared in source.
16. `include/Intrinsics.h` enumerates every compiler-known symbol.
17. `libturfrt` is linked into every binary.
18. AGENTS.md's "no compiler special-casing" claim is true and is tested.

**Tooling**
19. `-O0/-O1/-O2/-Os/-O3` exist; the equality smoke test passes at all of them.
20. `-O0 -g` produces DWARF that `gdb` can step and `llvm-dwarfdump --verify`
    accepts.

**Process**
21. Every feature group has a filled-in record in `docs/development/changes/`.
22. `FREEZE.md` matches what the compiler actually does.
23. `INVARIANTS.md` lists every INV-* in this document with what enforces it.
24. `handbook-sync/PENDING.md` is complete enough that the handbook can be
    resumed without re-reading the compiler.
25. `scripts/check_doc_links.sh` passes; no dangling doc references.
26. `./scripts/test_runner.sh` green; `lib/std` and `showcase` compile clean
    directly.

---

## 24. Risks

### Architectural

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| **Monomorphization's instantiation count explodes** in the stdlib | medium | high (compile time, binary size) | U-2 measures it in S11 before any Codegen change. `GlobalDCE` from OPT-001 offsets it. If it is genuinely bad, cache instantiations across the double-parse rather than instantiating twice. |
| **The double-parse pre-pass interacts badly with the monomorphizer** | medium | high | U-4 is an explicit S11 deliverable. Do not start coding before it is answered. |
| **Conservative GC misses a root** | low–medium | **critical** (silent corruption) | S21's deliberate retention tests; `setjmp` register spill done correctly; TOOL-001 first so it is debuggable; a `turf_rt_stats`-based harness from S19 onward. |
| **Two heaps confuse users** | medium | medium | One rule, stated everywhere: *if you called `mem::alloc`, you free it; otherwise you do not.* A diagnostic on freeing GC memory. |
| **The runtime becomes a dumping ground** | medium | medium | F-STD-2's closed intrinsic list; adding to it is an explicit, change-recorded act. |

### Semantic

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| **Overflow trapping costs too much** | medium | medium | Measure at `-O2` in S7 and record. OPT-002 recovers it. Fallback is an explicit opt-out flag, **never** wrapping-by-default. |
| **Mechanism B's removal breaks more than expected** | medium | medium | U-1 first. Split into deprecate→migrate→remove if large. |
| **`guess` invites scope creep** into parameters/returns | high | medium | F-GUESS-3, with the *architectural* reason (Pass-1 hoisting never enters a body) written down so it is not re-litigated as taste. |
| **Exact-match operator resolution gets "relaxed" by a well-meaning session** | medium | high | F-OP-1, restated in §13.3 with the concrete failures it prevents. |
| **`nsw` attached where a trap is the semantics** | medium | **high** (miscompilation) | Explicit in F-OPT-1 and §17.3. Add it to the review checklist for S7 and S25. |

### Implementation

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| **`NumberExprAST`'s shape change ripples widely** | high | low–medium | S3 stage 1 is a grep of every consumer before any edit. |
| **`Internalize` breaks FFI linking** | medium | medium | Explicit must-preserve set; validate against the most FFI-dense showcase program. |
| **Re-resolving cloned ASTs in the wrong scope** | medium | high | `clone()`'s contract (resolved fields not copied) is already correct and documented; the monomorphizer must establish scope explicitly. S12 is a whole session for the function case alone. |
| **Debug info trips `verifyModule`** | high | low | Expected; budget for it. |

### Multi-session coordination

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| **A session reinterprets a frozen decision** | medium | **high** | Every §8 group ends with an explicit "Must NOT reinterpret". `FREEZE.md` is read first, every session. |
| **Change records degrade into stubs** | high | high (this is how `Errors.md` happened) | A filled record is a completion criterion, not a nicety. `INDEX.md` makes a stub visible. |
| **Sessions drift on testing conventions** | medium | medium | AGENTS.md is the single source of truth and this document deliberately does not restate it. The `*_module.tr` trap is called out in the session template. |
| **The handbook falls further behind** | high | medium | `handbook-sync/PENDING.md` per chapter, per change. |
| **A session "fixes" something out of scope** | high | medium | §25. |

### Performance and compatibility

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| **Cumulative slowdown** (overflow checks + GC + bounds checks) | medium | medium | Establish a benchmark suite at M1 and track it per milestone. Right now there is no benchmark suite at all — building one is a prerequisite for any performance claim. |
| **Distribution gets harder** (a runtime library now ships) | high | low–medium | S18 updates `.github/actions/package-release`. Cross-compilation needs a target runtime — scope it or declare it unsupported. |
| **Source compatibility breaks** (SEM-002, SEM-004) | certain | medium | Both are intentional, both are mechanical, both are documented in their change records with the exact migration. |

---

## 25. Things We Should NOT Do

Tempting and wrong, each with the specific reason.

1. **Do not add a trait/constraint system.** `LIMITATIONS.md` proposes
   `fn<T: Eq>` as the fix for generic equality. Monomorphization fixes that
   *and* the ICE *and* `sizeof(T)` *and* the GC descriptor problem, with zero
   new syntax. Adding both would mean permanent language surface for a
   problem already solved.

2. **Do not keep a hybrid erasure path "for compatibility".** Two
   representations means two Codegen paths, two GC stories, and a rule users
   must learn. One representation is the point.

3. **Do not "fix" the double-parse pre-pass opportunistically.** It is odd,
   it is documented, and several groups touch code near it. Removing it is a
   separate, deliberate project with its own risk profile — not something to
   do while implementing something else.

4. **Do not unify the two type-inference engines** (`NodeTypes_` vs.
   `getExprType()`). It is a real duality with a real documented bug, but
   unifying it is a large refactor with no user-visible benefit, and §9.7
   gives a cheaper answer: make the AST carry concrete types so both agree.

5. **Do not make `string` a fat pointer.** `O(1)` length is not worth a
   conversion at every FFI boundary in a language whose stdlib *is* FFI
   wrappers. Use `StringBuilder`/`Slice<T>` instead.

6. **Do not move string operators back into Codegen.** `strings.tr` owning
   `operator +`/`==`/`<` was an architectural improvement.

7. **Do not add unsigned integer types** to "solve" overflow. Trapping solves
   overflow. Unsigned doubles the conversion matrix and adds the
   signed/unsigned comparison trap.

8. **Do not add `-fwrapping-arithmetic` (or any flag that changes semantics)
   before measuring.** And if one is ever added, it must be explicitly
   exempted from F-OPT-1 in writing.

9. **Do not implement a moving or generational collector**, and do not add
   write barriers "in preparation" for one. FFI requires non-moving;
   barriers for a collector that does not exist are pure speculative cost.

10. **Do not add finalizers.** `defer` is the answer. Finalizers bring
    resurrection, ordering and latency problems that no language has solved
    well.

11. **Do not merge the GC heap and the manual heap.** It would mean taking
    responsibility for every byte any `extern fn` ever allocated.

12. **Do not add Model B inference** (`mut x;` then infer from first
    assignment). §11.4.

13. **Do not extend inference to parameters or return types.** It would break
    the Pass-1-never-enters-a-body invariant that forward references and
    mutual recursion depend on.

14. **Do not relax operator resolution to `typesCompatible()`.** §13.3.

15. **Do not make `&&`/`||` overloadable.** Short-circuiting cannot survive a
    call, and `docs/reference/operators.md` documents short-circuiting as safe
    to rely on.

16. **Do not add new `SLM::ErrorCategory` values or `docs/slm/` category
    files** as a side effect of another change. That expansion is deliberately
    deferred and user-initiated.

17. **Do not remove sandbox verification from the SLM path**, however good the
    model gets. It is the only thing between a hallucination and a user's
    source.

18. **Do not add `@slm { }` in-source blocks in this milestone.**
    `FUTURE.md` §2 specifies them, and they are genuinely interesting — but
    they would be built against an architecture that is about to change
    underneath them.

19. **Do not attempt self-hosting in this milestone.** `FUTURE.md` is already
    right that it should wait.

20. **Do not add threads, async, or a concurrency model.** Nothing here is
    designed for it, the GC would need redesigning, and there is no consumer.

21. **Do not add a macro system or user-facing `comptime`.** The compile-time
    surface TurfLang has (`sizeof`, enum ordinals, `export immut`, constant
    folding) is proportionate.

22. **Do not add inheritance, vtables, or dynamic dispatch.** TurfLang has
    value semantics, composition and generics. Adding dispatch would
    invalidate §17.5's "no devirtualization needed" and change the memory
    model.

23. **Do not build LTO or PGO.** No separate compilation; no benchmark suite
    to profile against.

24. **Do not "clean up" `docs/planning/` or `docs/architecture/`.** They are
    accurate and load-bearing. `docs/development/` is additive.

25. **Do not change `s[i]`'s type from `int` to `char`** as a drive-by
    (C-7). It is a real inconsistency, it is recorded, and it is not
    scheduled.

26. **Do not write the runtime in TurfLang.** It must be correct below the
    level of the invariants it enforces.

---

## 26. The "Toy Compiler" Question

> *"If TurfLang is only ~50,000 lines of C++, is that evidence that it is
> missing important compiler/language engineering?"*

**No. Line count is not the signal, and in this case it actively misleads in
both directions.**

(The real figure is ~36,900 lines in `src/` + `include/`, plus ~4,200 lines of
standard library written in TurfLang and 646 regression tests.)

### What line count does not tell you

A compiler's size is dominated by three things that have nothing to do with
maturity: how many targets it supports, how many language features it has,
and how much of its optimizer it wrote itself. TurfLang targets one backend
(LLVM), has a deliberately small feature set, and writes none of its own
optimizer. Clang is ~2M lines mostly because C++ is enormous and because it
carries decades of compatibility. TCC compiles C in ~30k lines. Neither number
says anything about quality.

### The indicators that actually distinguish a mature implementation

Here is what to measure instead, with TurfLang's honest score on each.

| Indicator | TurfLang | Notes |
|---|---|---|
| **Can you write the basic data structures?** | ✗ | Verified: no linked list without `unsafe`, and no function may take one. The most damning single finding in this audit. |
| **Does the compiler ever crash or ICE on plausible input?** | ✗ | Two segfaults and one ICE found within an hour of probing. |
| **Are the semantics defined?** | partly | Control flow, modules, errors, pointers: yes, and documented. Numerics and initialization: no. |
| **Is there a runtime?** | ✗ | None at all. |
| **Is there a memory model?** | ✗ | Everything leaks by design. |
| **Type-system consistency** | good | One coherent hierarchy, one compatibility oracle, deliberate isolation decisions (`bool`, `char`) with stated reasons. |
| **Diagnostics** | **excellent** | 68 codes, `--explain`, spans, hints, suggestions, noise capping, sandbox-verified assisted fixes. Better than many production compilers. |
| **Static analysis** | **excellent** | Real dataflow: definite assignment, liveness, Tarjan SCC, points-to, value ranges — unconditional, not behind a flag. |
| **Module system** | **strong** | Resolved `::`, visibility, module-owned types, path-independent index, a 499-line architecture doc. |
| **Error handling** | strong | `error` types, `!T`, three `recover` forms, a divergence rule, exhaustive diagnostics. |
| **FFI** | **strong** | `extern from "lib" fn`, variadics, `int32` for honest ABI. A showcase program binds LLVM-C. |
| **Optimization** | ✗ | 5 function passes, no module pass manager, no `-O`. |
| **Debuggability** | ✗ | No DWARF. |
| **ABI / separate compilation** | ✗ | None. Blocks any package ecosystem. |
| **Test coverage** | **strong** | 646 tests, topic-organized, with a documented convention for compile-fail and runtime-abort cases. |
| **Documentation** | **excellent** | `docs/architecture/` is file:line-cited and, where checked, accurate. The handbook is better still, and is *honest about gaps* — ch08 and ch09 document real weaknesses rather than hiding them. |
| **Implementation-boundary hygiene** | weak | Semantic checks in Codegen; two symbol tables; two type-inference engines; capability by file path. |
| **Maintainability** | good | Clean pass split, recently modularized, and the doc-comment culture (*explain WHY, not what*) is genuinely unusual and genuinely valuable. |

### The honest summary

**TurfLang is not a toy.** A toy compiler does not have a points-to analysis,
a Tarjan SCC loop check, 68 documented error codes, a sandbox-verified
diagnostic assistant, or a 941-line internals document with accurate line
citations. Several parts of it are better than production compilers'
equivalents — the diagnostics in particular.

**TurfLang is an incomplete language implementation**, and the incompleteness
is concentrated, not diffuse. It is missing a runtime, a memory model, a
sound generic representation, defined numeric semantics, and debug info.
Those five gaps are what this roadmap addresses, and they are the difference
between "a compiler with a library" and "a language implementation".

**Some of it is simply smaller by design, and should stay that way**: no
traits, no inheritance, no exceptions, no macros, no concurrency, one integer
type plus an ABI escape hatch, one backend. That restraint is a feature.
Nothing in this document proposes making TurfLang bigger for its own sake; the
one genuinely additive language feature it proposes (`&?T`) exists to close a
verified hole, and the rest is either replacing a broken mechanism
(monomorphization, numeric semantics, unified initialization) or building
infrastructure that has no substitute (a runtime).

**What should remain small**: the compiler's own optimizer (LLVM's is
better), the intrinsic list (closed and enumerated), the runtime's API
surface, the language's type count, and the number of ways to do anything.

The right way to read the line count is this: ~36,900 lines is a reasonable
size for what TurfLang currently does, and this roadmap will grow it —
mostly in the monomorphizer, the runtime, and the numeric/check machinery.
If it ends up around 50,000–60,000 lines plus a few thousand lines of C
runtime, that will be a reasonable size for what it will then do. Neither
number is the point.

---

## 27. Deferred Work Register

Everything this audit identified that is **real** but **not scheduled in this
milestone**, in one place, so nothing is lost by being called "out of scope".

This is the feed for `docs/planning/FUTURE.md`. It is deliberately separate
from §25 (*Things We Should NOT Do*): §25 is **rejected**, this is
**deferred**. Nothing may be quietly moved between the two lists — that is a
decision, and it gets a change record.

**How to use it:** when the overhaul completes, each row becomes a
`FUTURE.md` entry with its unblocked-by column checked. Rows whose blocker
landed during the overhaul are the ones to pick up first, which is exactly
what the *Unblocked by* column is for.

### 27.1 Unblocked by this overhaul — pick these up first

| # | Item | Why deferred now | Unblocked by | Est. |
|---|---|---|---|---|
| D1 | **Closures / lambdas.** Verified: `fn(x: int): int { … }` as an expression is a parse error, so there are no anonymous functions and no captures. This is what forced `http.tr`'s router to be a hand-rolled `if`/`elseif` chain (`DESIGN_NOTES.md` §4 item 2). | A closure needs a heap-allocated, collected environment. Building one before the memory model exists means building it twice. | **MEM-002** | L |
| D2 | **Iteration over collections** — `for x in someList` does not exist; only real arrays and numeric ranges are iterable. Needs either closures or an explicit iterator protocol. | Same blocker as D1, plus a design choice (protocol vs. closure-based) that should be made *after* seeing how generics feel post-TYPE-002. | **TYPE-002 + D1** | M |
| D3 | **`Slice<T>` — a real non-owning view type.** Today `T[]` is a parameter-only escape hatch that aliases the caller's frame with **no escape analysis whatsoever**; nothing stops the callee casting it to a `ptr` and returning it. | A `{ptr, len}` slice is trivially expressible once generics are monomorphized, and needs GC for its lifetime story. | **TYPE-002 + MEM-002** | M |
| D4 | **`Map`/`Set` hashing.** Lookup is a linear scan because *"there is no generic `hash(T)` in the language yet"* (`map.tr`'s own header). | A generic `hash(T)` is unwritable under erasure. | **TYPE-002** — and it is already **scheduled** as STD-003 item 6; listed here only if that item is split out | M |
| D5 | **Full pointer-nullability analysis.** LANG-002 ships a deliberately *intraprocedural* `NullabilityPass` narrowing only from direct comparisons. `DESIGN_NOTES.md` §1 specifies more. | Starting local and documenting the limit is the right first cut; widening it is a separate, measurable improvement. | **LANG-002** | M |
| D6 | **Memory-leak detection** (*"malloc'd and never freed"*, distinct from the use-after-free/double-free tracking that already ships). `DESIGN_NOTES.md` §1. | Only meaningful for the **manual** heap after GC lands, since the GC heap no longer leaks. Its scope shrinks a lot as a result — do it after, not before. | **MEM-002** | M |

### 27.2 Deliberately out of scope, no blocker

| # | Item | Why deferred | Est. |
|---|---|---|---|
| D7 | **Separate compilation / a stable ABI / a package manager.** Every import is re-parsed and re-codegenned into the consuming binary on every compile. Not a performance problem today (0.3 s compiles), but it is the single thing blocking a package ecosystem. | A genuine major project, orthogonal to everything here, and the monomorphized mangling scheme (§9.5) is deliberately kept a pure function of its inputs so it stays viable. | XL |
| D8 | **Concurrency / threads / async.** | Nothing here is designed for it; the GC would need per-thread root scanning and a stop-the-world handshake (§14.11 records exactly what changes). No consumer yet. | XL |
| D9 | **`@slm { }` in-source blocks** (`FUTURE.md` §2) and **LoRA fine-tuning** (`DESIGN_NOTES.md` §6). | Both would be built against an architecture that is about to change underneath them. SLM-001's benchmark corpus is the prerequisite for judging either. | L |
| D10 | **Self-hosting** (`FUTURE.md`). | `FUTURE.md` is already right that it waits. It additionally needs D1 (callbacks/closures) and D7. | XL |
| D11 | **Developer tooling: LSP, `turf test`, `tpm`** (`FUTURE.md` §1). | Independent of everything here. Recent work (`ListAvailableModules()`, type-annotation source spans, `--format`) is already laying groundwork. Worth doing **in parallel** with the overhaul if there is appetite — it blocks nothing and nothing blocks it. | L |
| D12 | **Networking: TLS/HTTPS, UDP, non-blocking I/O.** | TLS is large and security-sensitive; non-blocking needs D8. | L |
| D13 | **Per-declaration visibility (`pub`/`private`).** Visibility is module-level only today. | Real but low-pressure; interacts with D7, so decide them together. | M |
| D14 | **Richer pattern matching** — destructuring, bindings, guards in `compare`/`matches`. | Today it is value-equality only, which is adequate. Enum exhaustiveness already works. | M |
| D15 | **Error payloads beyond a message string**, and a propagation shorthand (`errors_and_recover.md` §5.1, §5.2). STD-002 adds only `message` + caller location. | A general payload + destructuring feature is really D14 wearing a different hat; do them together. | M |
| D16 | **Circular imports**, which currently force `mem::Pool<T>` to be a deliberate duplicate of `List<T>`'s growth logic (`mem.tr:155-214`). | A real module-system change. STD-003 item 7 explicitly says **do not** work around it. | M |
| D24 | **Checked-overflow arithmetic (`EmitCheckedIntArith`, `src/CodegenCore.cpp`) blocks LLVM auto-vectorization on hot numeric loops.** Confirmed live by disassembly on the `structs` benchmark (2026-09-20, `refactor/architecture`, post-OPT-001): the per-operation branch to the overflow-trap block prevents SLP/loop vectorization outright, even though the surrounding GEPs/struct layout are unremarkable. Removing only that branch (an IR-level, throwaway edit — nothing else changed) let the exact same LLVM `-O3` pipeline vectorize the loop (`paddq`) and closed a 53%-slower-than-`clang -O3` gap down to parity. This is the live measurement §25 item 8 was waiting on before any `-fwrapping-arithmetic`-style escape hatch could even be discussed — it supplies the number, it does **not** decide the question. | Two independent directions exist and are not decided between: (a) widen the compiler's existing static range-proof elision (it already drops the check when provably safe — e.g. `(i % 5) - 2` in this same benchmark — so extending that analysis to more loop-carried values needs no semantic change at all); (b) an explicit, opt-in unchecked/wrapping construct for a hot loop, à la Swift's `&+`/`&-`/`&*` or Rust's `wrapping_add` (default stays checked; nothing implicit changes). Revisiting §25 item 8 with this measurement in hand is a decision for a future session, not this one. | M |

### 27.3 Known and accepted, recorded so they are not "discovered" again

| # | Item | Decision |
|---|---|---|
| D17 | **`s[i]` on a `string` yields `int`, not `char`** (C-7), even though `char` exists and is exactly one byte. | A genuine inconsistency. Breaking to change, low value relative to everything else. **Recorded, not scheduled.** §25 item 25 says do not fix it as a drive-by. |
| D18 | **Two type-inference engines** — `TypeChecker::NodeTypes_` (authoritative, read 0 times by Codegen) and `Codegen::getExprType()` (read 38 times) (C-11). | §9.7 makes them agree *by construction* for monomorphized nodes. Full unification is **explicitly rejected** for now (§25 item 4) as a large refactor with no user-visible benefit. Revisit only if a second wrong-answer bug of the `modules_and_scope_resolution.md` §2.10 kind appears. |
| D19 | **The double-parse pre-pass** — every file is lexed and parsed twice, and pre-pass diagnostics are discarded (§1.1). | Odd, documented, and load-bearing for struct-layout ordering. Removing it is a deliberate project with its own risk profile, **not** something to do while implementing something else (§25 item 3). TYPE-002's U-4 will produce the best understanding of it anyone has had — capture that in its change record so a future removal starts from real knowledge. |
| D20 | **Cross-compilation needs a runtime built for the target.** OPT-001 adds `--target`/`-mcpu`; RT-001 adds a linked `libturfrt`. Those two combine into a real problem. | RT-001's risk section flags it. The expected resolution is to **declare cross-compilation unsupported** until the runtime is cross-buildable, rather than shipping a `--target` that silently links a host runtime. Decide explicitly in RT-001. |
| D21 | **Postfix `++`/`--` and operator overloading.** Prefix desugars to `x = x + 1` and so inherits `operator+` for free; postfix does not desugar. | LANG-001 must **decide and record** whether postfix picks up an overload. Listed here so the decision is not silently skipped. |
| D22 | **`int → double` above 2^53 loses precision silently** and stays implicitly allowed (§10.5). | An acknowledged soft spot, argued in §10.5. A future opt-in lint is the intended answer — never an error. |
| D23 | **`T[]` parameters alias the caller's stack frame with no escape analysis.** | Superseded in practice by D3 (`Slice<T>`). Until then it is documented behaviour, not a bug. |

### 27.4 Rejected, not deferred

Everything in §25. Those are decisions, not backlog. In particular: no trait
system, no moving/generational GC, no finalizers, no unsigned integer types,
no fat `string`, no Model B inference, no macros, no inheritance. Moving any
of them into this register requires an explicit decision and a change record.

---

## Appendix A — Verified findings

Everything below was confirmed against a `--release` build at `74e9225`
(LLVM 22.1.8, x86_64 Linux, 2026-09-17), not inferred from reading.

1. `mut x: int = 999999999999999999999999999999999999999999999999;` prints
   `9223372036854775807`. No diagnostic.
2. `9223372036854775808` → the same clamp. `0 - 9223372036854775808` prints
   `-9223372036854775807`, so `INT64_MIN` is unwritable.
3. A `1` followed by 400 zeros as a `double` prints `inf`. No diagnostic.
4. `9223372036854775807 + 1` prints `-9223372036854775808`. No diagnostic.
5. `1.0e400`, `0xFF`, `0b1010`, `1_000` are all parse errors — no exponent,
   hex, binary, or separator syntax exists.
6. `mut x = 5;` → *"Expected ':' after variable name."*
7. `struct A { me: A }` compiles. `mut x: A;` → SIGSEGV (exit 139, no output).
8. `struct A { b: B } struct B { a: A }` — same.
9. `struct Node { next: Node* }` → E066 outside `unsafe`; works inside a
   **top-level** `unsafe { }` block.
10. `fn walk(n: Node*)` → E066 **even inside `unsafe`**. Only `lib/std`
    (matched by file path) may use `T*` in a signature.
11. `List<Big>` with a 24-byte struct →
    `Internal compiler error: generated LLVM IR is invalid. Invalid bitcast:
    bitcast %Big %x4 to ptr`.
12. `Big { a: 1, b: 2, c: 3 }` → *"no longer valid syntax"*, contradicting
    `ROADMAP_v1.0.0_COMPLETE.md` §3's "Final Syntax (locked in)".
13. `fn main(): int { return 0; }` → a 29,968-byte binary containing 84
    emitted, unused `turf_*` symbols.
14. Compile times: 0.28–0.52 s including the full stdlib re-parse. Compile
    time is **not** currently a problem.
15. Link line: `cc obj -o out -lm` plus `extern from` libraries. No runtime.
16. Optimization: `legacy::FunctionPassManager`, five passes, no module pass
    manager, no SROA, no inliner, no loop passes, no `-O` flag.
17. `DIBuilder` appears nowhere in the tree.
18. Codegen references `NodeTypes_` **0** times and `getExprType()` **38**
    times.
19. `GlobalSymbolTable` (the LLVM-side table) is referenced by Parser 28×,
    TypeChecker 44×, Codegen 135×, `main.cpp` 11×.
20. Seven dangling `docs/` cross-references; `docs/planning/Errors.md` is
    cited from **43** source comments across 22 files and does not exist.
21. `ValueRange.cpp`'s `evalConstExpr` performs `lv + rv` on `long long` —
    signed-overflow UB in the compiler's own C++.
22. Overloadable operators: `+ - * / == != < > <= >=` only.
23. `fn(x: int): int { … }` as an expression → *"Expected a function name
    after 'fn'"*. No lambdas, therefore no closures.
24. `include/Types.h`'s doc comments say "monomorphization"; `src/Types.cpp`'s
    say "type-erasure model". `compiler_pipeline.md` §7 already flags this.
25. 646 test `.tr` files across 35 topic folders; 4,208 lines of stdlib
    TurfLang; 36,894 lines of C++ in `src/` + `include/`; 8,102 lines of
    LaTeX in `book/`.
26. `lib/std` contains **74 `extern` declarations and 233 native TurfLang
    functions**. Five collection modules, five core modules, `json`, `http`
    and `test` have **zero** externs. (§15.7)
27. `ParseType()` contains **zero** `GlobalSymbolTable` references. No
    parsing decision anywhere depends on semantic state; all 28 parser
    symbol-table sites are either forward-registrations for Codegen or
    best-effort diagnostics. (§1.3)
28. `src/ParserDecl.cpp` preprocesses to 182,910 lines, of which
    `llvm/IR/Instructions.h` is 119,047 (~65%); it syntax-checks in 3.07 s at
    `-O2`. The dependency comes from one `llvm::AllocaInst *` field that
    could be forward-declared. (§3.4)
29. `fn cmpints(a: ptr, b: ptr): int32` → E066 *"Raw pointer in function
    signature!"* **even inside `unsafe { }`**. User code cannot write a C
    callback. (§15.8 F1)
30. A **second** `extern fn` inside a top-level `unsafe { }` block fails to
    parse: *"Expected ';' after statement"*. (§15.8 F2)
31. FFI features that **do** work, verified by compile: function-pointer
    parameters in an `extern fn`, struct-by-value parameters and returns,
    `extern from "lib" fn` with automatic `-l`, `double` parameters.
32. `for i in 0..5 { … }` → *"Expected 'step' after the range end value."*
    The `step` clause is mandatory on every range `for`. (LANG-004 item 1)
33. Unreachable-code and dead-branch warnings are written with a raw
    `std::cerr <<` (`src/CFG.cpp:381-383, 396-397`) — no code, no caret, no
    snippet, no hint — while fully-written `UnreachableBlockWarning` and
    `DeadBranchWarning` classes sit unused at `include/ErrorsFlow.h:58-82`.
    Confirmed live: `while true { return 1; } return 2;` prints
    `Warning: Unreachable code in block 'while.end' of function 'main'`
    directly above a correctly-rendered `lib/std` warning in the same output.
    (HYG-001 item 2)

## Appendix B — Feature ID index

| ID | Name | Milestone | Complexity | Sessions |
|---|---|---|---|---|
| ARCH-001 | Change tracking + freeze | M1 | S | S1 |
| TYPE-001 | Struct layout acyclicity | M1 | XS–S | S2 |
| SEM-001 | Literal representation + lexical syntax | M1 | M | S3 |
| DIAG-001 | Structural classification + lint gap | M1 | S | S6 |
| HYG-001 | Compiler hygiene + diagnostic rendering | M1 | XS | S6b |
| SEM-002 | Numeric semantics | M1 | M–L | S7 |
| OPT-001 | Pass manager, levels, module passes | M2 | M | S4 |
| TOOL-001 | Debug information | M2 | M | S5 |
| SEM-003 | `guess` | M2 | M | S8 |
| SEM-004 | Unified initialization | M2 | M | S9 |
| LANG-003 | `defer` | M2 | S–M | S10 |
| LANG-004 | Small language + FFI ergonomics | M2 | S | S10b |
| TYPE-002 | **Monomorphization** | M3 | XL | S11–S14 |
| LANG-001 | Operator completion | M3 | M | S15 |
| LANG-002 | `&?T` + declared capabilities | M3 | M–L | S16–S17 |
| RT-001 | `libturfrt` | M4 | L | S18 |
| MEM-001 | Route managed allocations | M4 | M | S19 |
| MEM-002 | The collector | M4 | XL | S20–S22 |
| STD-001 | Boundary formalization | M4 | M | S23 |
| STD-002 | Standard-library diagnostics | M4 | M | S24 |
| STD-003 | Post-monomorphization stdlib modernization | M5 | M | S24b |
| OPT-002 | Check elimination + specialization | M5 | M | S25 |
| SLM-001 | Diagnostic assistant | M5 | M | S26 |
