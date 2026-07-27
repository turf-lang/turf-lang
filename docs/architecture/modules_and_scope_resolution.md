# The Module System and `::` Namespace Resolution

Complete internal + external reference for Turflang's module/import system and the `::` scope-resolution operator, as they exist after the 2026-07 rewrite. This document describes both **what a Turflang programmer observes** (Part 1) and **how the compiler actually implements it, pass by pass** (Part 2), with file:line references throughout so the two halves can be cross-checked against the real source.

Related, narrower docs:
- `docs/slm/modules.txt` — the user-facing quick-reference used by the SLM diagnostic assistant (shorter, example-driven, no internals).
- `docs/detailed/modules_and_stdlib.md` — **stale as of this rewrite** (still describes the old `prelude.tr` auto-import and `.`-based module calls, neither of which exist anymore). Not corrected here since it wasn't in scope for this document; flagged for whoever picks that file up next.

---

## Part 1 — External reference (language semantics)

### 1.1 The core rule: `.` is instance-only, `::` is namespace-only

Two, and only two, kinds of qualified access exist, and they are never interchangeable:

| Operator | Means | Example |
|---|---|---|
| `.` | Access something on an **instance you hold** | `p.x`, `p.sum()` (struct field/method) |
| `::` | Access something **by name through a namespace**, no instance involved | `Color::Red` (enum variant), `io::printline(...)` (module function), `list::List<int>` (module type) |

Writing `io.printline(...)` is a **syntax error**, not a semantic one — the parser doesn't get far enough to say "io isn't an instance"; `io` followed by `.printline` simply isn't valid postfix syntax for a bare module name at all (dot-postfix parsing expects a value to dereference, and a module name never produces one — see 2.5).

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
| `UnknownScopeQualifierError` | `Left::Right` where `Left` is neither a known enum nor a known module |
| `UnknownModuleSymbolError` | `import module::symbol;` (or a qualified type/construction) names a symbol the module doesn't actually have |
| `ImportSymbolCollisionError` | Two on-demand imports flatten two different symbols under one bare name |
| `UnknownFunctionError` | A bare call that isn't a local declaration, self-reference, or on-demand import |
| `ModuleMethodNotFoundError` | `Module::func(...)` where `Module` is real but has no function named `func` |
| `UnknownTypeError` | A type name (bare or generic base name) that doesn't exist as a struct/enum/primitive anywhere |
| `TypeError` (two shapes) | Calling an enum variant as a function; using a module reference as a bare value |

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

This is a **deliberate, load-bearing design choice**, not an oversight: LLVM type identity (`LLVMTypeCache[Name]` in `SymbolTable.h`, keyed by bare name with no module prefix) is not a source-visibility concern, and two modules declaring the same struct name is a genuine, permanent conflict regardless of import mode — `DuplicateTypeNameError` already rejects it unconditionally. What *could* still be tightened (and deliberately hasn't been, see Part 5.1) is *lookup*: today, `isKnownStruct()`/`isEnumName()` answer "does this name exist anywhere in the whole program", with no per-file gate the way `CurrentFileFunctions_`/`FlatImportedFunctions_` gate bare function calls.

### 2.7 Resolver layer — Pass 1 (hoisting)

`hoistTopLevel()` (main file) and `hoistModuleBody()` (recursive, one call per `ModuleImportExprAST`, `src/Resolver.cpp:233-360`) walk top-level declarations only (no function bodies) and register:
- `FuncDefExprAST`/`ExternFuncDeclExprAST` → `Modules_[ModuleName]` (module context) or `CurrentFileFunctions_` (main file, `ModuleName.empty()`), via `registerFunctionSignature()`.
- `StructDeclExprAST`/`EnumDeclExprAST` → `RegisteredStructs_`/`RegisteredEnums_` **and** `TypeOwningModule_[Name] = ModuleName`.
- Nested `ModuleImportExprAST` → recurse.

`hoistModuleBody()` also unconditionally calls `SymTable_.registerModule(ModuleName)` up front, before iterating the body at all. This matters for a module that declares **only** structs/enums and no functions at all: `Modules_[ModuleName]` used to only ever get created lazily, inside `registerModuleFunction()` — a function-less module therefore never got an entry, and `hasModule()` (which, before this fix, additionally required `!Functions.empty()`) reported it as unknown even though it had imported successfully. Every `::`-qualified reference into such a module — `Module::Type` in type position, `Module::Type { ... }` construction, `Module::EnumName::Variant` — routed through `validateModuleQualifier()`'s `hasModule()` check and was rejected with "isn't a known enum or module", even though the qualifier was completely correct. `registerModule()` is now idempotent and called unconditionally so module *existence* is decoupled from module *function count*; `hasModule()` is now a pure `Modules_.count(...)` check.

Immediately after each `ModuleImportExprAST`'s body is hoisted, **`registerOnDemandImports()`** (`src/Resolver.cpp:360-419`) runs, using the now-fully-hoisted target module's registries:
- `Wildcard` → iterate every name `getModuleFunctionNames(SourceModule)` returns, flat-register each.
- `Symbol`/`SymbolList` → for each requested name, look it up via `findInModules()` filtered to `SourceModule`; if found, flat-register; else check `isKnownStruct`/`isEnumName` (types need no registration, just existence — 2.6); else `UnknownModuleSymbolError`.
- Flat-registration collisions (`registerFlatFunctionImport()` returning a previous, *different*-module entry) raise `ImportSymbolCollisionError`.
- The `ContextLabel` passed in is `""` for a main-file import, or the *enclosing* module's own name for an on-demand import written inside another module's file (the outer `hoistModuleBody()` call's own `ModuleName` parameter — not the module being imported).

### 2.8 Resolver layer — Pass 2 (the full AST walk)

**`visitFuncCall()`** (`src/Resolver.cpp:706-...`) branches on the callee's AST shape:
- `VariableExprAST` (bare call) → `CurrentFileMatches` from `CurrentFileFunctions_`; `ModuleMatches` from **two** sources ORed together: (a) `findInModules()` filtered to `currentModuleName()` (self-reference, empty string = main file = never matches, correctly), and (b) `lookupFlatFunctionImport(currentModuleName(), CalleeName)` (on-demand imports for this exact context).
- `ScopeAccessExprAST` (`Module::func(...)`) → if `LeftName` is a known enum, `TypeError` (variants aren't callable); if `LeftName` isn't a known module either, `UnknownScopeQualifierError`; otherwise `ModuleMatches` comes from `findInModules()` filtered to the **explicit** `LeftName` (no self-reference concept needed — the module is stated outright), and `Node.ResolvedKind = ModuleFunctionRef`. If the module is real but has no function named `func` at all (`CurrentFileMatches`/`ModuleMatches` both empty and `ResolvedKind == ModuleFunctionRef`), `ModuleMethodNotFoundError` is raised directly, with suggestions ranked from `getModuleFunctionNames(LeftName)` — this specific branch previously existed as dead code (the error class was defined but nothing constructed/raised it), so a call to a nonexistent module function fell through to a generic, less-specific diagnostic instead.

Both paths then share the exact same downstream arity-check/`ResolvedCandidates`-population code (`src/Resolver.cpp` continues past line ~760) — the callee-shape branching only decides *where `ModuleMatches` comes from*, not how it's used afterward.

**`visitScopeAccess()`** (`src/Resolver.cpp:1094-...`) handles a `ScopeAccessExprAST` reached as an ordinary expression (i.e. **not** as a call's direct callee — that's the branch above, which never even calls this): enum-variant validation (unchanged from the enum-only era) if `LeftName` is a known enum; `TypeError` ("no first-class functions") if it's a known module instead; `UnknownScopeQualifierError` if neither.

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

**`visitStructInit()`** (`src/Resolver.cpp:1331-...`) calls `validateModuleQualifier(Node.getModuleQualifier(), Node.getName(), Node.getLoc())` unconditionally (a no-op for the bare form) before recursing into field values. Note this function still does **not** validate that `Node.getName()` is a real struct at all for the *unqualified* form — that's a separate, pre-existing gap, not addressed by this work (a totally nonexistent bare struct name currently reaches an unrelated error path). The qualifier check happens to still produce a sensible error even then, since an unknown type name has no owning module and therefore can never equal a real `Qualifier`.

**`visitModuleImport()`** (`src/Resolver.cpp:1384-...`) pushes/pops `ModuleStack_`, the backing store for `currentModuleName()` (`include/Resolver.h:199-201`, `ModuleStack_.empty() ? "" : ModuleStack_.back()`) — the single source of truth for "which module's body is Pass 2 currently walking through", consulted by both the self-reference check above and every `registerOnDemandImports()` call's `ContextLabel`.

### 2.9 TypeChecker layer

`TypeChecker::visitScopeAccess()` (renamed from `visitEnumAccess`) only assigns a type when `Node.ResolvedKind == EnumVariant` — a module-function-ref callee gets visited (every callee is, so it receives *some* traversal) but intentionally receives no standalone type, since a qualified call's own type comes from `ResolvedCandidates`/`ResolvedCallTarget` (already computed by `visitFuncCall`, independent of the callee node's own shape).

### 2.10 Codegen layer

**Name mangling**: a module function's real LLVM name is always `turf_<module>_<name>_<paramtype>_<paramtype>...`, regardless of how many files import it or by which mode — Codegen only ever compiles it once, under its true owning module. Only Resolver-side *source lookup* changes with import mode; Codegen is essentially untouched by the whole on-demand-import feature.

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

### 5.1 Struct/enum bare-type visibility is not gated

Per 1.3/2.6: `List<int>` works unqualified after just `import list;` (the bare form), even though the on-demand import machinery (2.5, 2.7) fully exists and would support gating this the same way bare function calls now are. Closing this requires more than flipping `isKnownStruct()`/`isEnumName()`'s check in `validateNamedType()` — struct-literal **construction** (`visitStructInit()`) is a separate code path from a type annotation's `NamedType` reference and would need the identical gate, and every stdlib collection test (`List`/`Stack`/`Queue`) would need its imports rewritten to an on-demand form first. Left as its own follow-on rather than a partial flip that leaves some bare struct/enum references gated and others not.

### 5.2 No true static struct methods

`ScopeAccessKind` reserves a `StaticMethodRef` case (commented out, not implemented) for `StructName::method(...)` with no instance — a separate, older, independently-tracked finding: `ParseFuncDef()` unconditionally injects a `self` parameter into every `fn StructName.method(...)` declaration with no opt-out, so even once `::` can *parse* a static-style call, it will still fail arity-checking. Implementing real static methods needs that parser-level change first; this work only reserved the namespace-resolution slot for it.

### 5.3 Unqualified struct-literal construction of a nonexistent struct

`visitStructInit()` (2.8) validates a *qualifier* if present, but still does not validate that the *struct name itself* is real for the ordinary bare form — pre-existing, not introduced or fixed by this work.
