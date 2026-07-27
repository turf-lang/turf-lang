# The Module System and `::` Namespace Resolution

Complete internal + external reference for Turflang's module/import system and the `::` scope-resolution operator, as they exist after the 2026-07 rewrite. This document describes both **what a Turflang programmer observes** (Part 1) and **how the compiler actually implements it, pass by pass** (Part 2), with file:line references throughout so the two halves can be cross-checked against the real source.

Related, narrower docs:
- `docs/slm/modules.txt` — the user-facing quick-reference used by the SLM diagnostic assistant (shorter, example-driven, no internals).

`docs/detailed/modules_and_stdlib.md` (which described the old `prelude.tr` auto-import and `.`-based module calls, neither of which exist anymore) was removed rather than corrected — this document is its replacement for anything internals-related; a new user-facing detailed doc for modules/stdlib is deferred to a later, separate pass.

---

## Part 1 — External reference (language semantics)

### 1.1 The core rule: `.` is instance-only, `::` is namespace-only

Two, and only two, kinds of qualified access exist, and they are never interchangeable:

| Operator | Means | Example |
|---|---|---|
| `.` | Access something on an **instance you hold** | `p.x`, `p.sum()` (struct field/method) |
| `::` | Access something **by name through a namespace**, no instance involved | `Color::Red` (enum variant), `io::printline(...)` (module function), `list::List<int>` (module type) |

Writing `io.printline(...)` is rejected — but as a **semantic** error caught by the Resolver (`Resolver::visitMethodCall()`, 2.8.2), not a syntax error: `.` after any identifier parses perfectly fine as an ordinary method-call attempt (the parser has no way to know `io` isn't a variable at parse time), so `io.printline(...)` and `StructName.method(...)` (a bare struct TYPE name, no instance, attempting to reach a static method) both reach Pass 2 as normal-looking method calls. `visitMethodCall()` checks whether the base actually names a known module or struct TYPE (as opposed to a real declared variable) and rejects both immediately with `DotOperatorOnNonInstanceError`, pointing at the `::` form instead.

This is enforcement, not just documentation of intent — until this was checked, BOTH of these cases had their own separate, fully-working legacy resolution paths (`VariableExprAST::ResolvedModuleName` for the module case, `::IsStructName` for the struct case) that silently let `.` compile and run identically to `::`, discovered only when directly tested rather than assumed from the fact that every test in this suite happened to already use `::`. See 2.8.2.

### 1.2 Import forms

Exactly four, all starting with `import <module-name>`:

| Form | Syntax | Effect |
|---|---|---|
| Bare | `import io;` | Module reachable **only** via `io::` qualification. Nothing pasted unqualified. |
| Single symbol | `import io::printline;` | `printline` also becomes bare-callable, in addition to `io::printline` still working. |
| Symbol list | `import io::[print, printline];` | Same as above, for every listed name. |
| Wildcard | `import io::[*];` | Every function the module exports becomes bare-callable. |

A module is **always fully parsed** regardless of form — even a single-symbol import reads the whole target file, it just doesn't flatten more than what was asked for into the importing file's own namespace.

### 1.3 Visibility rules

**Bare calls to functions** resolve only to:
1. Something declared directly in the current file, or
2. If the call is textually inside a module's own body, another function declared in that **same** module (self-reference — e.g. `math.tr`'s `power()` calling its own `pow()` bare, no import needed, since this is same-file visibility, not a module feature), or
3. A name explicitly brought in via an on-demand import (forms 2–4 above), scoped to whichever file/module's own `import` statement requested it.

There is **no always-auto-imported module** — `io` must be imported like anything else. (This used to be different and was a real bug: `lib/std/prelude.tr` used to silently `import io;` for every compile, and the bare-call lookup searched every module ever loaded anywhere in the program with no check that the *current* file asked for that visibility — see Part 2.5 and Part 3 for the fix.)

**Struct/enum type names** are a separate, more lenient case, **on purpose, for now**: `List`, `Stack`, `Queue`, and any struct/enum you declare, are visible unqualified as soon as their module is imported in **any** form, including bare — `import list;` alone is enough for `mut l: List<int> = ...`. This is a deliberately-kept, not-yet-closed gap; see Part 5.1 for exactly why and what closing it would require.

**Qualifying is always allowed and always means the same thing as the unqualified spelling** — `list::List<int>` and bare `List<int>` construct/name the exact same type. Qualifying a *function* call, by contrast, is sometimes the *only* way to reach it (anything not on-demand-imported).

### 1.4 Module-qualified types and construction — and that qualification is *real*

Both of these are valid and mean the same thing:

```turf
import list;

mut a: List<int>      = List { data: null, len: 0, cap: 0 };
mut b: list::List<int> = list::List { data: null, len: 0, cap: 0 };
```

Qualifying is **validated**, not decorative. Two independent checks run whenever a `Module::Name` appears, in type position or construction position:
1. Is `Module` a real, imported module at all? (`UnknownScopeQualifierError` if not.)
2. Does `Module` actually declare `Name`? (`UnknownModuleSymbolError` if some *other* real module owns it instead — e.g. `mem::List` is rejected even though `mem` is real, because `List` lives in `list`.)

```turf
import list;
import mem;

mut x: mem::List<int> = ...;   // error: Module 'mem' has nothing named 'List'
```

### 1.5 `::` for enum variants (unchanged, predates the module use)

```turf
enum Color { Red, Green, Blue }
immut c: Color = Color::Red;
Color::COUNT    // int: number of variants
Color::VALUES   // T*: pointer to array of every variant's underlying int value
Color::NAMES    // string*: pointer to array of every variant's name
```

The parser treats `Left::Right` **uniformly** — it never knows in advance whether `Left` is an enum or a module; that's decided after parsing, once `Left`'s real identity is known (see Part 2.3).

### 1.6 Collisions and circular imports

Two on-demand imports flattening two *different* symbols under the same bare name is a compile error, not a silent shadow:

```turf
import moduleA::shared;
import moduleB::shared;   // ImportSymbolCollisionError
```

Importing a name that doesn't exist in the named module errors at the `import` statement itself:

```turf
import io::nosuchfunction;   // UnknownModuleSymbolError, right here
```

A module importing another that (directly or transitively) imports it back is a compile error with the full chain rendered:

```
Circular import detected -- these modules are waiting on each other.

    a  --imports-->  b  --imports-->  a  <- back to the start!
```

A **diamond** (two modules both importing the same third module) is not a cycle and compiles fine — only a genuine loop back to a module already being loaded triggers this (Part 2.8).

### 1.7 Error reference

| Error | Raised when |
|---|---|
| `ModuleNotFoundError` | `import xyz;` and no `xyz.tr` exists anywhere on the search path |
| `CircularImportError` | A module import chain loops back on itself |
| `UnknownScopeQualifierError` | `Left::Right` where `Left` is neither a known enum, struct, nor module |
| `UnknownModuleSymbolError` | `import module::symbol;` (or a qualified type/construction/static-method-call qualifier) names a symbol/owner the module doesn't actually have |
| `ImportSymbolCollisionError` | Two on-demand imports flatten two different symbols under one bare name |
| `UnknownFunctionError` | A bare call that isn't a local declaration, self-reference, or on-demand import |
| `ModuleMethodNotFoundError` | `Module::func(...)` where `Module` is real but has no function named `func` |
| `StructMethodNotFoundError` | `StructName::method(...)` where `StructName` is real but has no method (static or instance) by that name |
| `DotOperatorOnNonInstanceError` | `.` used with a base that names a module (`io.printline(...)`) or a bare struct type (`StructName.method(...)`, no instance) instead of `::` |
| `UnqualifiedTypeNotVisibleError` | A real struct/enum type referenced bare, but not visible from this file/module (2.7.1) |
| `UnknownTypeError` | A type name (bare or generic base name) that doesn't exist as a struct/enum/primitive anywhere |
| `TypeError` (several shapes) | Calling an enum variant as a function; using a module or static-method reference as a bare value; a module name nonsensically qualified by another module name (`Module::Module2::func(...)`) |

---

## Part 2 — Internal architecture (compiler implementation)

### 2.1 Pipeline recap

```
source.tr
   │
   ▼
 Lexer  ──tokens──▶  Parser  ──AST──▶  Resolver (2 passes)  ──annotated AST──▶  TypeChecker  ──▶  Codegen (LLVM IR)
```

One wrinkle relevant here: `main.cpp` runs a **pre-pass** (its own Resolver + TypeChecker instances, sub-passes 2a/2b/3a/3b) before the *mandatory* Resolver/TypeChecker/Codegen pass, purely to hoist function prototypes so forward calls work. The pre-pass's own diagnostics are informational only — the mandatory pass is authoritative and Codegen only ever runs after *it* succeeds. This matters for one specific historical bug (Part 3) but is otherwise invisible to the module system.

### 2.2 Lexer: `::` is `TOK_SCOPE`

`include/Lexer.h:212`, `src/Lexer.cpp:545` — a plain two-character lookahead (`:` then `:`), no special-casing at the lexer level for what follows. `::` has never had any other meaning in this grammar; there is exactly one place upstream of the Resolver that ever consumes it (Part 2.3).

### 2.3 AST representations

**`ScopeAccessExprAST`** (`include/AST.h:840-908`, renamed from `EnumAccessExprAST` when generalized) — the node for *any* `Left::Right`:
```cpp
class ScopeAccessExprAST : public ExprAST {
  std::string    LeftName, RightName;
  SourceLocation LeftLoc, RightLoc, Loc;
  std::string    ModuleQualifier; // "" unless this is a 3-part Module::EnumName::Variant chain
public:
  ScopeAccessKind ResolvedKind = ScopeAccessKind::Unresolved; // set by the Resolver
  ...
};
enum class ScopeAccessKind { Unresolved, EnumVariant, ModuleFunctionRef, ModuleTypeRef /*, StaticMethodRef: reserved, not implemented */ };
```
The parser only ever fills in `LeftName`/`RightName`/locations — it has **no opinion** on what kind of `::` this is. `ResolvedKind` is set later, by the Resolver, once `LeftName`'s real identity (enum? module? neither?) is known. `getEnumName()`/`getVariant()` remain as aliases to `LeftName`/`RightName` so the pre-existing enum-only call sites (TypeChecker's `compare`/`matches` exhaustiveness checking, Codegen's enum codegen) needed zero changes.

**3-part chains** (`Module::EnumName::Variant`, e.g. `colors::Shade::Dark`): the parser only ever builds a 2-part node from `Identifier '::' Identifier`, so a 3-part chain is detected by peeking for a *second* `TOK_SCOPE` right after building the first `ScopeAccessExprAST` (`src/Parser.cpp`, `ParsePrimaryBase()`'s `TOK_SCOPE` branch) — on a hit, the node is rebuilt with the original left identifier reinterpreted as `ModuleQualifier`, the middle identifier as `LeftName` (the enum), and the final identifier as `RightName` (the variant). The Resolver's `visitScopeAccess()` runs `validateModuleQualifier()` (2.7) against this field exactly like a qualified type annotation does, so a 3-part chain gets the same "is this really the right module for this enum" check as `Module::EnumName` used in type position — not a separate, less-checked path.

**`ModuleImportExprAST`** (`include/AST.h:328-390`):
```cpp
class ModuleImportExprAST : public ExprAST {
  SourceLocation Loc;          // the `import` statement itself
  std::string    ModuleName;
  std::string    ResolvedPath; // canonical path actually loaded (see 2.4)
  std::unique_ptr<ExprAST> Body; // the ENTIRE parsed module file
  ImportMode Mode = ImportMode::Bare;             // Bare | Symbol | SymbolList | Wildcard
  std::vector<ImportedSymbolRef> RequestedSymbols; // name + own SourceLocation each
};
```
`Body` is always the full recursively-parsed module — even a single-symbol import parses everything, per 1.2.

**`StructInitExprAST`** (`include/AST.h:1019-1054`) gained a `ModuleQualifier` field (empty for the ordinary bare form), validated the same way as a type annotation's qualifier (2.7).

**`NamedType` / `GenericType`** (`include/Types.h:226-241`, `341-395`) each gained a `Module` field. Deliberately **not** part of `getName()`/type identity — struct/enum names are still one flat, globally-unique namespace (2.6), so two `NamedType`s for the same bare name must compare equal for type-compatibility purposes regardless of whether one spelling happened to be qualified. The field exists solely so the Resolver can validate the qualifier once, at first reference.

### 2.4 Parser layer

**`ParseImport()`** (`src/Parser.cpp:2483`) parses the module name, then optionally `::` followed by one of `Symbol` / `[list]` / `[*]`, building the `Mode`/`RequestedSymbols` above. It then searches, in order: `<compiler-binary-dir>/../lib/std/<module>.tr`, the importing source file's own directory, the current working directory — the first hit becomes `ResolvedPath`, threaded into the `ModuleImportExprAST` alongside `Loc`. `ResolvedPath` is the shared identity key both circular-import detection (2.8) and per-file on-demand-import bookkeeping (2.5) key off of, so neither has to re-derive "what file is this" independently.

**`ParseType()`** (`src/Parser.cpp:206-...`) — after consuming the base identifier, checks for `TOK_SCOPE` **before** its existing `<` generic-args check. If present, the identifier just consumed was actually the module qualifier; it's captured and threaded into the `NamedType`/`GenericType` constructed below, rather than (as in the very first version of this feature) being silently discarded. **Not validated here** — module registration happens during hoisting, which runs after the *entire* program is parsed, so nothing is known yet about which modules exist at this point in `ParseType()`'s own execution. Validation is deferred to the Resolver (2.7).

**`ParsePrimaryBase()`**'s `TOK_IDENTIFIER` case (`src/Parser.cpp:648` onward) handles three things in this order, because each check's absence is what falls through to the next:
1. `Identifier '{' ... '}'` → bare struct-literal construction.
2. `Identifier '::' Identifier` → build a `ScopeAccessExprAST`, *then* check if `'{'` follows — if so, it's qualified construction (`Module::StructName { ... }`), reusing the same field-parsing loop as (1) via the shared helper `ParseStructInitFieldsBody()` (`src/Parser.cpp:613`) so the two forms don't duplicate ~30 lines of parsing logic. Otherwise, the plain `ScopeAccessExprAST` is returned, and whatever postfix syntax follows (a `(` for a call, nothing at all) is handled by the **already-existing, unmodified** postfix-expression machinery — `Module::func(args)` was already round-tripping through the ordinary `(`-call handler with zero new parsing logic, well before any of the qualified-construction work.
3. Otherwise, falls through to ordinary identifier/keyword-typo handling.

**Circular import detection** lives entirely in `ParseFile()` (`src/Parser.cpp`, the function `ParseImport()` calls): a second, disjoint stack from the pre-existing `ImportedFiles` cache set —
```cpp
struct ImportStackFrame { std::string ModuleName, ResolvedPath; SourceLocation ImportLoc; };
static std::vector<ImportStackFrame> ImportStack;
```
Checked **before** the `ImportedFiles` dedup check (a module still on `ImportStack` is an in-progress cycle; a module already fully parsed and *not* on the stack is the ordinary "reuse it" case). An RAII guard pushes/pops the frame around the body-parsing loop so no early-return path can leak a stale entry. On a hit, `CircularImportError` renders the chain by walking `ImportStack` from the re-entered frame through to the top (`BuildCircularImportChain()`, same file).

### 2.5 Resolver layer — data structures

`ResolverSymbolTable` (`include/ResolverSymbolTable.h`) holds five structures relevant here:

```
Modules_               : unordered_map<ModuleName, ResolverModule>
                          Every function/extern declared in EVERY transitively-imported
                          module, namespaced per module. Backs qualified calls
                          (Module::func(...)) and findInModules() suggestions.

CurrentFileFunctions_  : unordered_map<Name, vector<Overload>>
                          The MAIN FILE's own top-level fn/extern declarations ONLY --
                          confirmed by tracing every call site that this map is never
                          written to for anything declared inside a module (those go to
                          Modules_ instead, via registerModuleFunction, never
                          registerCurrentFileFunction).

FlatImportedFunctions_ : unordered_map<ContextLabel, unordered_map<Name, FlatImportedSymbol>>
                          On-demand imports (Symbol/SymbolList/Wildcard), keyed by WHICH
                          FILE/MODULE requested them ("" = main file, else a module name).
                          Stores only {SourceModule, ImportLoc} -- a name-to-module
                          ROUTING TABLE, not a copy of the function's own data; actual
                          overload resolution still goes back through findInModules(),
                          filtered to SourceModule, exactly like self-reference does.

RegisteredStructs_,     : FLAT, module-blind. One name -> one declaration site, globally,
RegisteredEnums_          across the whole program (see 2.6 for why this is intentional).

TypeOwningModule_       : unordered_map<TypeName, ModuleName>
                          WHICH module actually declared each struct/enum ("" = main
                          file) -- distinct from the flat registries above, which only
                          ever answer "does this name exist ANYWHERE". Populated at the
                          exact point registerStruct()/registerEnumType() run. Backs
                          validateModuleQualifier() (2.7): "is Module a real module" is
                          a hasModule() check against Modules_; "does Module actually own
                          this type" is a TypeOwningModule_ lookup.
```

### 2.6 Why structs/enums stay flat (and functions don't)

This is a **deliberate, load-bearing design choice**, not an oversight: LLVM type identity (`LLVMTypeCache[Name]` in `SymbolTable.h`, keyed by bare name with no module prefix) is not a source-visibility concern, and two modules declaring the same struct name is a genuine, permanent conflict regardless of import mode — `DuplicateTypeNameError` already rejects it unconditionally. `isKnownStruct()`/`isEnumName()` answer "does this name exist anywhere in the whole program" — that's an EXISTENCE question and stays flat/global. *Visibility* (can THIS file/module reference the name unqualified) is a separate question, answered by `isTypeVisibleUnqualified()` — see 2.7/2.8.

### 2.7 Resolver layer — Pass 1 (hoisting)

`hoistTopLevel()` (main file) and `hoistModuleBody()` (recursive, one call per `ModuleImportExprAST`, `src/Resolver.cpp:233-360`) walk top-level declarations only (no function bodies) and register:
- `FuncDefExprAST`/`ExternFuncDeclExprAST` → `Modules_[ModuleName]` (module context) or `CurrentFileFunctions_` (main file, `ModuleName.empty()`), via `registerFunctionSignature()`.
- `StructDeclExprAST`/`EnumDeclExprAST` → `RegisteredStructs_`/`RegisteredEnums_` **and** `TypeOwningModule_[Name] = ModuleName`.
- Nested `ModuleImportExprAST` → recurse.

`hoistModuleBody()` also unconditionally calls `SymTable_.registerModule(ModuleName)` up front, before iterating the body at all. This matters for a module that declares **only** structs/enums and no functions at all: `Modules_[ModuleName]` used to only ever get created lazily, inside `registerModuleFunction()` — a function-less module therefore never got an entry, and `hasModule()` (which, before this fix, additionally required `!Functions.empty()`) reported it as unknown even though it had imported successfully. Every `::`-qualified reference into such a module — `Module::Type` in type position, `Module::Type { ... }` construction, `Module::EnumName::Variant` — routed through `validateModuleQualifier()`'s `hasModule()` check and was rejected with "isn't a known enum or module", even though the qualifier was completely correct. `registerModule()` is now idempotent and called unconditionally so module *existence* is decoupled from module *function count*; `hasModule()` is now a pure `Modules_.count(...)` check.

Immediately after each `ModuleImportExprAST`'s body is hoisted, **`registerOnDemandImports()`** (`src/Resolver.cpp:365-...`) runs, using the now-fully-hoisted target module's registries:
- `Wildcard` → every name `getModuleFunctionNames(SourceModule)` returns is flat-registered as a function; every name `getModuleTypeNames(SourceModule)` returns is flat-registered as a type (via `registerFlatTypeImport()`, into the separate `FlatImportedTypes_` map — see 2.7.1).
- `Symbol`/`SymbolList` → for each requested name, first tried as a function (`findInModules()` filtered to `SourceModule`); if not found there, tried as a type via `getTypeOwningModule(Name) == SourceModule` (**ownership**, not mere existence — see 2.7.1 for why this distinction was itself a bug fix); neither → `UnknownModuleSymbolError`.

#### 2.7.1 Bare struct/enum type visibility gate

Struct/enum type **visibility** (as opposed to existence, 2.6) now follows the identical rule bare function-call visibility already did: a plain `import Module;` grants QUALIFIED access to its types (`Module::Type`) but not bare access; bare access requires either declaring the type in the SAME file/module, or an explicit on-demand import (`import Module::Type;` / `Module::[Type,...]` / `Module::[*]`).

`ResolverSymbolTable::isTypeVisibleUnqualified(ContextLabel, Name)` answers this: true if `getTypeOwningModule(Name) == ContextLabel` (own declaration — the same rule the function self-reference check already used), or if `FlatImportedTypes_[ContextLabel]` contains `Name` (on-demand import). `FlatImportedTypes_` is a second, parallel map to `FlatImportedFunctions_` (same shape, same `registerFlatFunctionImport`-style collision handling) rather than folded into it, since a struct/enum name and a function name legally occupy different syntactic positions (type position vs. call position) and can coexist under one bare spelling without colliding.

Enforced at the same three call sites `validateModuleQualifier()` already covered: `validateNamedType()`'s `NamedType` branch, its `GenericType` branch (base name), and `visitStructInit()`. Each follows the same shape: if a qualifier is present, validate it via `validateModuleQualifier()` as before (qualified access is **never** gated by this — only bare/unqualified references are); if no qualifier and the name is a real, known struct/enum, check `isTypeVisibleUnqualified(currentModuleName(), Name)` and raise `UnqualifiedTypeNotVisibleError` if false. Type params (`T` in `fn<T> f(x: T)`) are exempted the same way they already were from the existence check.

A related bug was found and fixed alongside this: `registerOnDemandImports()`'s `Symbol`/`SymbolList` branch used to accept ANY struct/enum name that existed ANYWHERE in the program as satisfying `import Module::TypeName;`, never checking that `Module` actually owns it (only `isKnownStruct`/`isEnumName`, the flat existence check — see 2.6). `import mem::List;` (List actually lives in `list`) incorrectly succeeded as long as `list` had been imported by *something* in the program. Fixed by checking `getTypeOwningModule(Name) == SourceModule` (ownership) instead, mirroring `validateModuleQualifier()`'s own ownership check for qualified type annotations.
- Flat-registration collisions (`registerFlatFunctionImport()` returning a previous, *different*-module entry) raise `ImportSymbolCollisionError`.
- The `ContextLabel` passed in is `""` for a main-file import, or the *enclosing* module's own name for an on-demand import written inside another module's file (the outer `hoistModuleBody()` call's own `ModuleName` parameter — not the module being imported).

### 2.8 Resolver layer — Pass 2 (the full AST walk)

**`visitFuncCall()`** (`src/Resolver.cpp:706-...`) branches on the callee's AST shape:
- `VariableExprAST` (bare call) → `CurrentFileMatches` from `CurrentFileFunctions_`; `ModuleMatches` from **two** sources ORed together: (a) `findInModules()` filtered to `currentModuleName()` (self-reference, empty string = main file = never matches, correctly), and (b) `lookupFlatFunctionImport(currentModuleName(), CalleeName)` (on-demand imports for this exact context).
- `ScopeAccessExprAST` (`Left::Right(...)`) → dispatches on what `LeftName` actually is, in order:
  1. Known enum → `TypeError` (variants aren't callable).
  2. Known struct → **`StaticMethodRef`** (2.8.1 below) — a true static method call, no instance.
  3. Known module → `ModuleFunctionRef` — `ModuleMatches` from `findInModules()` filtered to the **explicit** `LeftName` (no self-reference concept needed — the module is stated outright). If the module is real but has no function named `func` at all, `ModuleMethodNotFoundError` is raised directly, with suggestions ranked from `getModuleFunctionNames(LeftName)` — this specific branch previously existed as dead code (the error class was defined but nothing constructed/raised it), so a call to a nonexistent module function fell through to a generic, less-specific diagnostic instead.
  4. Neither → `UnknownScopeQualifierError`.

Both the struct and module branches then share the exact same downstream arity-check/`ResolvedCandidates`-population code (`src/Resolver.cpp` continues past line ~760) — the callee-shape branching only decides *where `ModuleMatches`/`CurrentFileMatches` come from*, not how they're used afterward.

**`visitScopeAccess()`** (`src/Resolver.cpp:1136-...`) handles a `ScopeAccessExprAST` reached as an ordinary expression (i.e. **not** as a call's direct callee — that's the branch above, which never even calls this): enum-variant validation (unchanged from the enum-only era) if `LeftName` is a known enum; `TypeError` ("no first-class functions") if it's a known module or a known struct instead (the struct case: `StructName::method` referenced without being called — same "no first-class functions" reasoning as the module case); `UnknownScopeQualifierError` if none of the three.

**`validateNamedType()`** (`src/Resolver.cpp:1167-1263`) — called for every type annotation (params, return types, fields, var decls). For a `GenericType`, now validates the base name itself (previously only recursed into type *arguments* — `NonExistentGeneric<int>` used to silently pass) *and* calls `validateModuleQualifier()` on it; for a `NamedType`, the existing `isKnownStruct`/`isEnumName` check is unchanged, with `validateModuleQualifier()` called afterward if the name resolved.

**`validateModuleQualifier()`** (`src/Resolver.cpp:1265-1290`) — the shared helper both `validateNamedType()` and `visitStructInit()` call:
```cpp
bool Resolver::validateModuleQualifier(const std::string &Qualifier,
                                       const std::string &TypeName,
                                       SourceLocation Loc) const {
  if (Qualifier.empty()) return true;                       // unqualified, nothing to check
  if (!SymTable_.hasModule(Qualifier)) { UnknownScopeQualifierError(...).raise(); return false; }
  if (SymTable_.getTypeOwningModule(TypeName) != Qualifier) { UnknownModuleSymbolError(...).raise(); return false; }
  return true;
}
```

**`visitStructInit()`** (`src/Resolver.cpp:1379-...`) calls `validateModuleQualifier()` for the qualified form, or (since 2.7.1) `isTypeVisibleUnqualified()` for the bare form when `Node.getName()` is a known struct. Note this function still does **not** validate that `Node.getName()` is a real struct at all for the *unqualified* form when it ISN'T a known struct — that's a separate, pre-existing gap (5.2), not addressed by this work (a totally nonexistent bare struct name currently reaches an unrelated error path). The qualifier check happens to still produce a sensible error even then, since an unknown type name has no owning module and therefore can never equal a real `Qualifier`.

**`visitModuleImport()`** (`src/Resolver.cpp:1384-...`) pushes/pops `ModuleStack_`, the backing store for `currentModuleName()` (`include/Resolver.h:199-201`, `ModuleStack_.empty() ? "" : ModuleStack_.back()`) — the single source of truth for "which module's body is Pass 2 currently walking through", consulted by both the self-reference check above and every `registerOnDemandImports()` call's `ContextLabel`.

### 2.8.1 True static struct methods

`fn StructName::method(...)` — note `::`, not `.` — declares a genuine static method: no implicit `self` parameter at all, callable only as `StructName::method(args)`, never through an instance.

**Declaration** (`ParseFuncDef()`, `src/Parser.cpp:2091-...`): the existing `.`-branch (instance method: `StructName = FuncName; FuncName = StructName + "." + method`, then unconditionally injects `self: StructName*` as parameter 0) gets a parallel `TOK_SCOPE` branch — same `FuncName = StructName + "." + method` internal registration (methods of one struct, static or instance, share ONE flat name — the `.`-joined internal name is an implementation detail, never user-visible), but the `self`-injection block is skipped entirely (`!StructName.empty() && !IsStaticMethodDecl`). Reusing the exact same internal name means overload resolution, mangling, and forward-reference tolerance all fall out for free — a static and instance method of the same name on the same struct simply become different-arity overloads of one registered name (the presence/absence of `self` as parameter 0 is what actually distinguishes them at the call site, not any separate "is static" flag).

**Call resolution** (`visitFuncCall()`, 2.8): when `LeftName` is a known struct, `Node.ResolvedKind = StaticMethodRef` and the lookup is byte-identical to the `ModuleFunctionRef` case — `getTypeOwningModule(LeftName)` gives the owning context ("" = main file, else a real module), then either `lookupCurrentFileFunctions("StructName.method")` or `findInModules("StructName.method")` filtered to that owning module. Not-found reuses `StructMethodNotFoundError` — the SAME class the ordinary `instance.method()` not-found path already raised (via `TypeChecker::visitMethodCall`) — for one consistent message regardless of which call form was used; its hint now explains both the `.` and `::` declaration forms since it's shared between them.

**Qualified access is existence-only, not visibility-gated** — `StructName::method(...)` only requires `isKnownStruct(LeftName)`, exactly like a qualified type annotation (2.7.1) bypasses the bare-type-visibility gate. A plain `import Module;` (bare form, no symbol) is enough to call `Module::StructName::method(...)` or (once the struct itself is bare-visible, e.g. via `import Module::StructName;`) the shorter `StructName::method(...)`.

**Codegen needs zero changes.** `FuncCallExprAST::codegen()` for a `ScopeAccessExprAST` callee (2.10) never distinguishes `ModuleFunctionRef` from `StaticMethodRef` at all — it evaluates `Args` exactly as written (no implicit prepending) and calls whatever `ResolvedCallTarget->mangledName()` resolves to. Static methods "just work" once the Resolver sets `ResolvedCallTarget` correctly, the same way `ModuleFunctionRef` already did.

**A 3-part chain's leading `ModuleQualifier` is validated, not ignored** — `Module::StructName::method(...)` re-validates `ModuleQualifier` via `validateModuleQualifier()` (does `Module` exist, does it actually own `StructName`) before proceeding. Two related bugs were found and fixed while wiring this up, both from the SAME root cause (the parser's 3-part-chain reinterpretation, 2.3, lands the leading name in `ModuleQualifier` — but until now, NOTHING in `visitFuncCall` ever read that field for a call callee, for either the struct or the plain-module case): `mem::Vec2::zero()` used to compile cleanly even when `Vec2` actually lives in a different module (the wrong qualifier was silently ignored, only `LeftName`/`RightName` mattered); and `mem::io::printline(...)` — a module name nonsensically qualified by ANOTHER module name, which has no defined meaning at all — also used to compile identically to plain `io::printline(...)`. Both now raise a clean, specific error instead of silently discarding the (wrong or meaningless) qualifier.

### 2.8.2 `.` on a module or a bare struct type name is rejected (fixed, not just documented)

`Resolver::visitMethodCall()` (`src/Resolver.cpp:1064-...`) handles every `Left.method(...)` call — the ENTIRE `.`-call path, separate from `visitFuncCall`'s handling of bare calls and `Left::method(...)`. Resolves `Node`'s arguments first (mirroring `visitFuncCall`'s own ordering, so a broken argument still gets its own diagnostic even when the base is about to be rejected), then dispatches on what the base actually is, in priority order:

1. A real declared variable (`ExistingVar`) — resolve it as an instance, `return`. Variable shadowing always wins here: a local variable named the same as a module or struct type takes priority, exactly like ordinary lexical scoping (`mut io: Thing = ...; io.show();` calls `Thing.show()`, not anything module-related — even though `io` is also a real module name).
2. A known **module** name with no shadowing variable — `DotOperatorOnNonInstanceError("module")`, pointing at `Module::func(...)` instead.
3. A known **struct** type name with no shadowing variable — `DotOperatorOnNonInstanceError("struct type")`, pointing at `StructName::method(...)` instead.
4. Neither — resolve normally so `visitVariable()` raises the ordinary `ReferenceError` for a genuinely undeclared name.

**This used to be two separate, fully-functional legacy resolution paths instead of a rejection**, both pre-dating this session's `::` work entirely:

- `VariableExprAST::ResolvedModuleName` (module case) — set instead of erroring, then read all the way through `TypeChecker.cpp` (`Node.ResolvedModuleName`) and `Codegen.cpp` (`Var->ResolvedModuleName`) to actually resolve and emit the call. `io.printline(...)` compiled and ran identically to `io::printline(...)`.
- `VariableExprAST::IsStructName` (struct case) — originally added specifically to fix the static-struct-method-call CRASH (a bare struct type name used to make `getExprType()` return null, crashing Codegen with `report_fatal_error`; see 5.1's history). The fix made `TypeChecker::visitMethodCall()` compute `ExpectedArity = IsStaticMethod ? ArgCount : (ArgCount + 1)` — i.e. "if the base is a bare type name, don't require a `self` argument's worth of extra arity." That was meant only to turn the crash into a clean, always-failing arity error (every method, at the time, was `.`-declared and therefore always had a `self` parameter, so `ArgCount` could never equal a `.`-declared method's real arity). Once true static methods (2.8.1, `fn StructName::method(...)`, genuinely no `self` at all) existed, their arity IS exactly `ArgCount` — so this old check started **succeeding** for them. `MathUtil.square(5)` (bare type name, `.` syntax) compiled, ran, and computed the correct answer, identically to `MathUtil::square(5)`.

Both were found by direct testing (not inferred from the fact that every test file in the suite happened to already use `::`) after a direct request to verify the `.`/`::` split is actually enforced. Neither field was deleted — `ResolvedModuleName`/`IsStructName` and their `TypeChecker.cpp`/`Codegen.cpp` readers are left in place as defense-in-depth (harmless, and now permanently unreachable in a real compile, since `visitMethodCall` always raises and returns before ever setting either field again).

### 2.9 TypeChecker layer

`TypeChecker::visitScopeAccess()` (renamed from `visitEnumAccess`) only assigns a type when `Node.ResolvedKind == EnumVariant` — a module-function-ref callee gets visited (every callee is, so it receives *some* traversal) but intentionally receives no standalone type, since a qualified call's own type comes from `ResolvedCandidates`/`ResolvedCallTarget` (already computed by `visitFuncCall`, independent of the callee node's own shape).

### 2.10 Codegen layer

**Name mangling**: a module function's real LLVM name is always `turf_<module>_<name>_<paramtype>_<paramtype>...`, regardless of how many files import it or by which mode — Codegen only ever compiles it once, under its true owning module. Only Resolver-side *source lookup* changes with import mode; Codegen is essentially untouched by the whole on-demand-import feature.

**Struct codegen idempotency**: `StructDeclExprAST::codegen()` (`src/Codegen.cpp:3769-...`) can genuinely be invoked more than once for the SAME struct declared inside a module — `main.cpp`'s pre-pass calls `ModuleImportExprAST::codegen()` twice (once during its prototype-only sub-pass, again during its body sub-pass), and that function unconditionally re-walks the entire module body, including any nested `StructDeclExprAST` nodes, on every call. A second call now detects the cached `llvm::StructType` is already non-opaque (fully built) and returns immediately, rather than falling through to `StructType::create()` again — which used to silently produce a second, LLVM-renamed `Name.0` type and corrupt `LLVMTypeCache` for every reference already built against the original (see 5.4 for the crash this caused before the guard existed).

**`FuncCallExprAST::codegen()`** (`src/Codegen.cpp:2958-...`) derives `CalleeName` from **either** a `VariableExprAST` **or** a `ScopeAccessExprAST` callee (previously only the former — a qualified call used to hit the function's `report_fatal_error("First-class functions are not supported yet.")` fallback, since nothing else could interpret the callee). Everything downstream (`ResolvedCallTarget->mangledName()` lookup, argument marshaling) is unchanged and callee-shape-agnostic.

**`ScopeAccessExprAST::codegen()`** (`src/Codegen.cpp:3718-...`, renamed from `EnumAccessExprAST::codegen()`) is now reached **only** for the enum-variant meaning — a module-function-ref in call position is fully handled by `FuncCallExprAST::codegen()` without ever calling `Callee->codegen()` (a function reference, qualified or bare, has no "value" to compute, only a name lookup), and the Resolver already rejects any non-call use of a module reference before Codegen ever runs.

**`getExprType()`** (`src/Codegen.cpp:242-...`, Codegen's own independent type-inference — a *different* system from TypeChecker's, and a recurring source of drift bugs in this codebase's history) needed two fixes discovered while migrating call sites from `.` to `::`:
1. Its `FuncCallExprAST` branch, like Codegen proper, only recognized a `VariableExprAST` callee — extended to also accept `ScopeAccessExprAST`.
2. The mangled-name reconstruction (`"turf_" + CalleeName + argTypes...`) never accounted for a module prefix at all. This was invisible for int-returning functions (an int compare doesn't care about a null inferred type) but a real, silent **wrong-answer** bug for a string-returning one used inline in an expression: `strings::trim(s) == "hello"` evaluated to `false` even when `trim()` genuinely returned `"hello"`, because the `==` codegen needed an accurate type to choose `strcmp` over a raw pointer comparison and silently got neither. Fixed by recovering the module prefix from `ResolvedCallTarget->SourceModule` while **preserving** the existing call-site-argument-type mangling scheme (needed for generic-function type-erasure substitution) — an earlier, more aggressive fix that used `ResolvedCallTarget->mangledName()` wholesale broke `generic_free_function_return_type.tr`, since that reflects the function's *declared* signature, not the call site's actual instantiation.

### 2.11 Diagnostics / SLM routing

New error classes (`include/Errors.h`) all reuse the existing `SLM::ErrorCategory::DECLARATION` value — none were judged to warrant a new category. `include/ErrorCategory.h`'s `extractContextKeyword()` gained a `"module"`/`"import"` substring check, routing to `docs/slm/modules.txt` via the same `constructDocs` mechanism `structs.txt`/`enums.txt` already use. Ordering matters: this check runs **after** the struct/enum check, not before — `DuplicateTypeNameError`'s own message (for a plain, non-module-related struct-name collision) contains the substring "every module you import" in its explanatory note, which would have misrouted it ahead of `structs.txt` had the module check been checked first. (The exact same class of trap the struct/enum-before-`controlKeywords` ordering already solved once, for `"for enum"` vs. the control-flow keyword `"for"`.)

---

## Part 3 — End-to-end walkthrough: `import io::printline;` then `printline("hi");`

1. **Parse** `import io::printline;`: `ParseImport()` reads `io`, sees `::`, sees a bare identifier (not `[`) → `Mode = Symbol`, `RequestedSymbols = [{"printline", loc}]`. Searches and finds `lib/std/io.tr`, fully parses it into `Body`, records `ResolvedPath`. Returns a `ModuleImportExprAST`.
2. **Parse** `printline("hi");`: an ordinary `FuncCallExprAST` with a `VariableExprAST("printline")` callee — nothing module-specific happens at parse time here at all.
3. **Hoist (Pass 1)**: `hoistModuleBody(io.tr's Body, "io")` registers `print`/`printline`/`input`/`printf`/`scanf`/`getchar`/`calloc` into `Modules_["io"]`. Immediately after, `registerOnDemandImports()` runs with `Mode = Symbol`, looks up `"printline"` via `findInModules()` filtered to `"io"`, finds all 4 overloads, and calls `registerFlatFunctionImport("", "printline", {SourceModule: "io", ...})` — context `""` because this `import` statement is in the main file.
4. **Resolve (Pass 2)**, visiting the `printline("hi")` call: `visitFuncCall()` takes the `VariableExprAST` branch. `CurrentFileMatches` is empty (not declared locally). Self-reference check: `currentModuleName()` is `""` (not inside any module) → contributes nothing. On-demand check: `lookupFlatFunctionImport("", "printline")` → finds the entry from step 3 → pulls all matching overloads via `findInModules()` filtered to `"io"` → `ModuleMatches` now has 4 candidates. Arity check narrows to the 1-argument overloads (all 4 are). `ResolvedCandidates` populated; `TypeChecker` will later narrow to the `string` overload by argument type.
5. **Codegen**: `FuncCallExprAST::codegen()` reads `ResolvedCallTarget->mangledName()` → `"turf_io_printline_string"`, finds the real LLVM function (already compiled when `io.tr`'s own `ModuleImportExprAST::codegen()` ran), calls it directly.

Contrast with `input()` (never on-demand-imported in this file): step 4's on-demand check misses (`lookupFlatFunctionImport("", "input")` finds nothing), self-reference misses (still not inside a module), `CurrentFileMatches` and `ModuleMatches` are both empty → falls into the "not found" branch, which (after confirming it's not one of the 4 true bare intrinsics) builds `RankedSuggestions` — including, since this specific gap was fixed alongside the bare-call scoping work, an **exact-name match search across all modules purely for suggestion purposes** (`findInModules("input")`, unrestricted, unlike the restricted lookup used for actual *resolution*) — surfacing `io::input() -> string` as a "could be one of these" hint before raising `UnknownFunctionError`.

---

## Part 4 — File map

| File | Role |
|---|---|
| `include/Lexer.h`, `src/Lexer.cpp` | `TOK_SCOPE` token |
| `include/AST.h` | `ScopeAccessExprAST`, `ModuleImportExprAST`, `StructInitExprAST` (+ `ImportMode`, `ImportedSymbolRef`, `ScopeAccessKind`) |
| `include/Types.h` | `NamedType`/`GenericType`'s `Module` field |
| `src/Parser.cpp` | `ParseImport()`, `ParseType()`'s qualifier handling, `ParsePrimaryBase()`'s `TOK_SCOPE`/struct-init branches, `ParseFile()`'s `ImportStack`, `ParseStructInitFieldsBody()` |
| `include/Resolver.h`, `src/Resolver.cpp` | `hoistTopLevel`/`hoistModuleBody`, `registerOnDemandImports`, `visitFuncCall`, `visitScopeAccess`, `validateNamedType`, `validateModuleQualifier`, `visitStructInit`, `visitModuleImport`, `currentModuleName`/`ModuleStack_` |
| `include/ResolverSymbolTable.h`, `src/ResolverSymbolTable.cpp` | `Modules_`, `CurrentFileFunctions_`, `FlatImportedFunctions_`, `RegisteredStructs_`/`RegisteredEnums_`, `TypeOwningModule_`, `registerFlatFunctionImport`/`lookupFlatFunctionImport`, `getTypeOwningModule` |
| `include/TypeChecker.h`, `src/TypeChecker.cpp` | `visitScopeAccess` |
| `src/Codegen.cpp` | `FuncCallExprAST::codegen()`, `ScopeAccessExprAST::codegen()`, `getExprType()` |
| `include/Errors.h` | `CircularImportError`, `UnknownScopeQualifierError`, `UnknownModuleSymbolError`, `ImportSymbolCollisionError` |
| `include/ErrorCategory.h` | `"module"`/`"import"` keyword routing to `docs/slm/modules.txt` |
| `src/Builtins.cpp`, `lib/std/io.tr` | `@intrinsic` removal; `print`/`printline`/`input` now plain Turf + `extern fn` FFI |
| `lib/std/*.tr` | The 7 real stdlib modules (`io`, `mem`, `math`, `list`, `stack`, `queue`, `strings`), each an ordinary `.tr` file, no compiler special-casing |

---

## Part 5 — Known limitations, deliberately deferred

### 5.1 (Resolved) No true static struct methods

Previously documented here as a known limitation — fixed; see 2.8.1. `fn StructName::method(...)` (note `::`) now declares a genuine static method, callable as `StructName::method(args)` with no instance. Left as a pointer rather than deleted outright, same rationale as 5.3 below.

### 5.2 Unqualified struct-literal construction of a nonexistent struct

`visitStructInit()` (2.8) validates a *qualifier* if present, and (since 2.7.1) validates *visibility* for a name that's a known struct — but still does not validate that the *struct name itself* is real at all for the ordinary bare form. A totally nonexistent bare struct name in `StructName { ... }` construction still falls through to an unrelated downstream error rather than a clean `UnknownTypeError` — pre-existing, not introduced or fixed by this work.

### 5.3 (Resolved) Struct/enum bare-type visibility gate

Previously documented here as a known limitation (`List<int>` working unqualified after just `import list;`) — fixed; see 2.7.1. Left as a pointer rather than deleted outright so anyone who read this doc before the fix landed can find where the behavior changed.

### 5.4 A general note on cross-pass struct codegen safety

While hunting the visibility-gate work above, a real compiler CRASH was found and fixed: a struct declared inside a module, used BY VALUE (not just via pointer/`self`) by a free function in that same module, crashed the whole compiler process with an LLVM IR verifier failure (`ret %Point.0 %structval` against a function still declaring `%Point`). Root cause: `main.cpp`'s pre-pass invokes `ModuleImportExprAST::codegen()` twice (once per prototype/body sub-pass), and it unconditionally re-walks the entire module body — including any `StructDeclExprAST` nodes — on every call, with no guard against re-running struct codegen. `StructDeclExprAST::codegen()` (2.10) now tolerates being called more than once for the same struct (returns immediately if the cached LLVM type is already fully built, instead of creating a second, LLVM-renamed `Name.0` type and corrupting the cache for every already-built reference). Never caught before because every prior struct-in-module test only ever used instance methods (pointer receiver, unaffected by which concrete `StructType` backs it) or constructed the struct in the same file it was declared in — a free function passing/returning a module struct BY VALUE across the import boundary was untested territory until this bug hunt.
