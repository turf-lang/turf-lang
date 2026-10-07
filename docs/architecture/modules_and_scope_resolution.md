# The Module System and `::` Namespace Resolution

Complete internal + external reference for Turflang's module/import system and the `::` scope-resolution operator, as they exist after the 2026-07 rewrite. This document describes both **what a Turflang programmer observes** (Part 1) and **how the compiler actually implements it, pass by pass** (Part 2), with file:line references throughout so the two halves can be cross-checked against the real source.

Related, narrower docs:
- `docs/slm/modules.txt` — the user-facing quick-reference used by the SLM diagnostic assistant (shorter, example-driven, no internals).

`docs/detailed/modules_and_stdlib.md` (which described the old `prelude.tr` auto-import and `.`-based module calls, neither of which exist anymore) was removed rather than corrected — this document is its replacement for anything internals-related. The user-facing counterpart deferred at the time has since landed: `docs/reference/functions_and_modules.md`.

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
| Single symbol | `import io::printline;` | `printline` becomes bare-callable. It does **not** make `io::printline` legal: that needs `import io;` as well. |
| Symbol list | `import io::[print, printline];` | Same as above, for every listed name. |
| Wildcard | `import io::[*];` | Every function the module exports becomes bare-callable (and, again, no `io::`). |

A module is **always fully parsed** regardless of form — even a single-symbol import reads the whole target file, it just doesn't flatten more than what was asked for into the importing file's own namespace.

**No transitive imports (LEC-008, `FREEZE.md` §6.24, F-IMP-1/2).** A file may write `m::` — a call, a constant, a type, `m::T::new()` — only if it wrote a plain `import m;` itself. The prelude's modules, and the module's own body, are the only exceptions. A module that some *other* import happened to load is not visible: after `import json;` (which imports `list`), `list::List<int>` is `E145`, with a hint naming the line to add. The symbol forms import names, not the qualifier, so a file that wants both `List` and `list::List` writes both `import list::List;` and `import list;`. What is deliberately *not* gated (F-IMP-4) is anything that follows the **type** rather than a name: methods, UFCS calls (`x.f()`), operators, and holding or passing a value whose type came from a module the file never imported — only *spelling* `io::T` needs the import (F-IMP-3).

### 1.3 Visibility rules

> **LEC-002 (landed 2026-09-29).** A module's top-level **functions, types, methods and struct fields are private unless written `pub`/`public`** (synonyms), and another module can only reach the `pub` ones — see "`pub` visibility" below. A `pub` signature or field may not mention a private type of its own module (`E089`). **Migrating a 1.x module:** `turf --migrate=pub --write <module.tr>...` puts `pub` on every top-level declaration and struct field (the exact 1.x surface, as a starting point — then remove it from what should be internal); without `--write` it only reports what it would change. Full status: `docs/development/changes/LEC-002.md`; the rules are frozen in `FREEZE.md` §6.15. `pub` where it means nothing is `E087`; reaching a private name is `E088`.

**`pub` visibility (LEC-002).** Every cross-module reference to a function or type is checked against the declaration's `pub` mark — by qualified name (`m::f()`, `m::T`, `m::T::default()`, `m::T::Variant`) and by `import m::name;`. A module always sees its own declarations. `import m::[*]` brings in only the `pub` names, so a private name of `m` can never collide with one of yours; an explicit `import m::helper;` of a private name is `E088` (not "unknown symbol" — the module has it, it just did not export it). If a name has both `pub` and private overloads, only the `pub` ones take part in overload resolution from outside. **Members:** a struct's fields and methods follow the same rule *even in a `pub` struct* — `p.field`, `p.method()`, `T::method()`, `T::new()` and a UFCS call `p.f()` (a private free function is not a candidate outside its module) are all checked; `T::default()` is exempt because it only zero-fills. The Resolver checks what it can name without a receiver's type (`T::new`, `T::method`); the TypeChecker checks the rest against the module the code was *written* in (`TypeChecker::CurrentModule_`) — for a monomorphized clone that is its template's module, so `pub` generics keep access to their own private fields (`docs/development/changes/LEC-002.md`, stage 4). `export immut` constants are always public (`export` is their `pub`). Operators are not gated: they are visible wherever their operand types are.

**Bare calls to functions** resolve only to:
1. Something declared directly in the current file — *when that file is the main program*. A module's body never sees the main program's functions (an importer cannot leak into what it imports; this used to be violated, so a module's bare `helper()` could silently call the importer's unrelated `helper()`), or
2. If the call is textually inside a module's own body, another function declared in that **same** module (self-reference — e.g. `math.tr`'s `power()` calling its own `pow()` bare, no import needed, since this is same-file visibility, not a module feature), or
3. A name explicitly brought in via an on-demand import (forms 2–4 above), scoped to whichever file/module's own `import` statement requested it, or
4. **Only if none of 1–3 has any function of that name** — the prelude tier (`FREEZE.md` §6.25): a public free function of `io`, `math` or `sys`. So `printline("x")`, `abs(-3)` and `exit(1)` work with no import, and a declaration of your own (or an explicit import) of the same name hides the prelude's rather than making the call ambiguous.

The six core prelude modules (`int`, `double`, `bool`, `char`, `strings`, `array`) are known to every compile and reached through UFCS, operators and `m::`, but are **not** bare-callable; `io`, `math` and `sys` are the three that are (owner decision 2026-10-07). Before that decision there was no always-auto-imported *flattened* module — a real bug had once been `lib/std/prelude.tr` silently `import io;`-ing for every compile while the bare-call lookup searched every module ever loaded, with no check that the *current* file asked for that visibility (see Part 2.5 and Part 3 for that fix). The prelude tier is not that bug: it is a closed, named list (`PreludeBareModuleNames` in `src/ParserModule.cpp`), consulted last, and shadowed by anything the file says.

**Struct/enum type names** follow the identical rule bare function-call visibility does (2.7.1): a plain bare `import list;` grants **qualified** access only (`list::List`) — bare `List` is visible unqualified only if it's declared in the same file/module, or brought in via an on-demand import (`import list::List;` / `import list::[List, ...];` / `import list::[*];`). This used to be more lenient (any import form, including bare, made the type bare-visible) — that was a deliberately-kept gap, closed by 2.7.1; see Part 5.1 for the history.

**Qualifying is always allowed and always means the same thing as the unqualified spelling** — `list::List<int>` and bare `List<int>` construct/name the exact same type. Qualifying a *function* call, by contrast, is sometimes the *only* way to reach it (anything not on-demand-imported).

### 1.4 Module-qualified types and construction — and that qualification is *real*

Both of these are valid and mean the same thing:

```turf
import list;
import list::List;

mut a: List<int>      = List::default();
mut b: list::List<int> = list::List::default();
```

(Each spelling needs its own import: `import list::List;` is what makes the bare `List` on the first line legal — see 1.3/2.7.1 — and `import list;` is what makes the qualified `list::List` on the second legal, symbol imports granting no qualifier. See the "No transitive imports" paragraph in 1.2.)

(Struct construction is `StructName::new(args...)`/`StructName::default()` only — see the struct-construction-overhaul work; there is no brace-literal form anymore. `List` has no user-defined `.new`, so `::default()` is the only way to construct it.)

Qualifying is **validated**, not decorative. Three independent checks run whenever a `Module::Name` appears, in type position or construction position (`new`/`default` included):
1. Is `Module` a real module at all? (`UnknownScopeQualifierError` if not.)
2. Did *this file* import it? (`ModuleNotImportedError`, E145, if the module exists but only another file's import loaded it — LEC-008.)
3. Does `Module` actually declare `Name`? (`UnknownModuleSymbolError` if some *other* real module owns it instead — e.g. `mem::List` is rejected even though `mem` is real, because `List` lives in `list`.)

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

**Types belong to their module** (FDN-003, `FREEZE.md` §6.11). A struct, enum, machine or `error` type is identified by *(owning module, name)*: two modules may each declare `Node`, and a program may declare its own `struct List` or `struct Parser` whatever it imports — they are distinct types (`a::Node`, `b::Node`, `List`). Only declaring one name twice in the *same* file is an error (`DuplicateTypeNameError`, E034). `typeof()` prints the identity: `"json::JsonValue"` for an imported type, bare `"Node"` for one declared in the main program file (whose module, `main`, is never spelled).

A bare type name that could mean two types is rejected **at the use**, naming both — no "the local one wins":

```turf
import shapes::[*];        // shapes declares `struct Point`
struct Point { x: int }    // ...and so does this file -- both legal

mut p: Point = ...;        // AmbiguousTypeReferenceError (E085): write
                           // `shapes::Point` for theirs, and leave Point
                           // out of the import
```

Two on-demand imports that both bring in a `Point` are still rejected at the second import (`ImportSymbolCollisionError`), exactly as for functions.

### 1.7 Exporting constants: `export immut`

Functions, structs, enums, errors and machines are exported with `pub` (`public` is the same word) — see the `pub` rule in 1.3; without it they are private to their module. Plain variables are different: a bare `mut`/`immut` declaration only ever makes sense *inside* a function (it's destroyed when that scope ends), so there is no existing top-level-variable syntax to piggyback on. `export` fills exactly that gap, and **only** that gap:

```turf
export immut PI: double = 3.14159;
export immut MAX_USERS: int = 100;
export immut GREETING: string = "hello";
export immut DEBUG: bool = true;
```

Rules, deliberately narrow:
- **Top level only.** `export` inside a function body is a compile error (`ExportOutsideTopLevelError`) — grammatically it parses in any statement position (the parser has no reason to reject it there), so this is enforced by the Resolver checking function-nesting depth, the same style of enforcement as 1.1's `.`-rejection.
- **`immut` only, for now.** `export mut PI: double = 3.14159;` is rejected at parse time (`'export mut' isn't supported yet`) — a true mutable shared global needs its own future feature (initialization ordering, real global-variable codegen); `export` today only covers compile-time constants.
- **Initializer must be a literal.** `export immut X: int = someFunction();` is rejected (`ExportNonConstantInitializerError`) — an exported constant has **no real storage at all**. Every reference to it, anywhere, is replaced by the literal value directly at compile time; there is no global variable, no alloca, nothing to load from.
- **Everything else about it is an ordinary variable.** Typo detection, duplicate-name detection, and immutability (you cannot assign to an exported constant, same `ImmutableAssignmentError` as any other `immut`) all apply exactly as they would to a local `immut` — `export` only changes *where* it can be declared and *how far* its visibility reaches.

Visibility and access follow the exact same rules as functions (1.3):

```turf
// consts.tr
export immut VERSION: int = 7;
```

```turf
import consts;
consts::VERSION        // qualified access always works once imported, in any form

import consts::VERSION;
VERSION                 // bare access -- only after an on-demand import (or self-reference
                         // within consts.tr itself)
```

### 1.8 Error reference

| Error | Raised when |
|---|---|
| `ModuleNotFoundError` | `import xyz;` and no `xyz.tr` exists anywhere on the search path |
| `CircularImportError` | A module import chain loops back on itself |
| `UnknownScopeQualifierError` | `Left::Right` where `Left` is neither a known enum, struct, module, nor a declared variable |
| `ModuleNotImportedError` (E145) | `m::name` (call, constant, type, construction) in a file that has no plain `import m;` of its own — the module exists only because another file imported it, or the file only has `import m::name;` (LEC-008); the hint prints the `import m;` line |
| `ScopeOperatorOnInstanceError` | `::` used with a base that's a real declared variable (an instance) instead of a type/module/enum name -- e.g. `p::method(...)` where `p: SomeStruct`; points at `.` instead |
| `UnknownModuleSymbolError` | `import module::symbol;` (or a qualified type/construction/static-method-call qualifier) names a symbol/owner the module doesn't actually have |
| `ImportSymbolCollisionError` | Two on-demand imports flatten two different symbols under one bare name |
| `UnknownFunctionError` | A bare call that isn't a local declaration, self-reference, or on-demand import |
| `ModuleMethodNotFoundError` | `Module::func(...)` where `Module` is real but has no function named `func` |
| `StructMethodNotFoundError` | `StructName::method(...)` where `StructName` is real but has no method (static or instance) by that name |
| `DotOperatorOnNonInstanceError` | `.` used with a base that names a module (`io.printline(...)`) or a bare struct type (`StructName.method(...)`, no instance) instead of `::` |
| `UnqualifiedTypeNotVisibleError` | A real struct/enum type referenced bare, but not visible from this file/module (2.7.1) |
| `UnknownTypeError` | A type name (bare or generic base name) that doesn't exist as a struct/enum/primitive anywhere |
| `AmbiguousTypeReferenceError` (E085) | A bare type name that could mean two visible types — this file's own and an on-demand-imported one (FDN-003) |
| `DuplicateTypeNameError` (E034) | The same type name declared twice in ONE file or module (two different modules may each declare it — FDN-003) |
| `TypeError` (several shapes) | Calling an enum variant as a function; using a module or static-method reference as a bare value; a module name nonsensically qualified by another module name (`Module::Module2::func(...)`); a generic struct named without type arguments in a type annotation (`mut b: Box = ...` — "Missing type arguments!") |
| `PrivateInPublicError` (E089) | A `pub` function, method, extern fn or field whose signature/type mentions — at any depth, e.g. as a generic argument or inside `!T` — a type its own module did not mark `pub`; names both declarations, and the hint offers both fixes (make the type `pub`, or drop the `pub` that leaks it). Not raised in the main file (LEC-002; see `docs/errors/codes/E089.txt`) |
| `PrivateSymbolAccessError` (E088) | A function, type, method or struct field another module declared without `pub`, reached by qualified name, by `import m::name;`, through a receiver (`x.field`, `x.method()`, UFCS) or through `T::new`/`T::method`; the hint names the file and the exact `pub` spelling to write, and lists the module's own public alternatives — for a private field, the type's public methods (LEC-002; see `docs/errors/codes/E088.txt`) |
| `MisplacedVisibilityError` (E087) | `pub`/`public` where visibility means nothing — on an operator/import/variable, on a variant or parameter, inside a function, with `export`, before a decorator, or as a name (LEC-002; each shape's hint is in `docs/errors/codes/E087.txt`) |
| `ExportOutsideTopLevelError` | `export` used inside a function body |
| `ExportMutNotSupportedError` | `export mut` — only `export immut` is supported today |
| `ExportNonConstantInitializerError` | `export immut X = <non-literal>;` — the initializer isn't a literal the compiler can fold at compile time |
| `NoConstructorDefinedError` | `StructName::new(...)` called but `StructName` has no `.new` declared |
| `ReservedConstructorNameError` | `fn StructName.default(...)` declared by hand — `default` is always compiler-synthesized |
| `StaticOutsideStructError` | `static` used on a function that isn't struct-bound (`fn StructName.method(...)`) |
| `ConstructorCannotBeStaticError` | `fn StructName.new(...) static: ...` — `new` always constructs via `self` |
| `DefaultConstructorArityError` | `StructName::default(x, ...)` — `default()` never takes arguments |
| `ConstructorCalledOnInstanceError` | `instance.new(...)` / `instance.default(...)` — reserved for `StructName::new/default(...)` only |
| `IncompleteConstructorError` | `new()` doesn't assign every field of its struct on every path (ConstructorFieldAssignmentPass, a real dataflow check, not "assigned somewhere") |

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

**Struct construction** (`StructName::new(...)`/`StructName::default()`) needed no new AST node or field at all -- it parses as an ordinary `ScopeAccessExprAST` callee (see 2.3 above) with `RightName` "new"/"default", reusing the SAME `ModuleQualifier` field the enum/static-method paths already had. The Resolver sets `ResolvedKind` to the new `ConstructorCall`/`DefaultConstruct` enum values and validates the qualifier via the same `validateModuleQualifier()` a qualified type annotation goes through (2.7) -- see 2.8.1a. (The former `StructInitExprAST` node, which backed the old brace-literal form `StructName { field: val, ... }`, was deleted entirely as part of this -- there is no brace-literal construction anymore.)

**`NamedType` / `GenericType`** (`include/Types.h:226-241`, `341-395`) each gained a `Module` field: the qualifier as SPELLED. Since FDN-003 (2.6) the Resolver rewrites the name itself to the type's identity (`list::List`), so `getName()` — not this field — carries the module for every later pass; the field only records what the source wrote, for validating the qualifier once, at first reference.

### 2.4 Parser layer

**`ParseImport()`** (`src/Parser.cpp:2483`) parses the module name, then optionally `::` followed by one of `Symbol` / `[list]` / `[*]`, building the `Mode`/`RequestedSymbols` above, then hands off to `ParseModuleImportByName()` for the actual file search. The first hit becomes `ResolvedPath`, threaded into the `ModuleImportExprAST` alongside `Loc`. `ResolvedPath` is the shared identity key both circular-import detection (2.8) and per-file on-demand-import bookkeeping (2.5) key off of, so neither has to re-derive "what file is this" independently.

**Module search roots and `TURF_HOME`** (2026-08 rewrite; LEC-001 stage 3 made it all-roots — `TurfInstallRoot()`/`IndexModuleDir()`/`CollectModuleCandidates()`/`ParseModuleImportByName()`, `src/ParserModule.cpp`): four roots, **all examined** (`CollectModuleCandidates()`). Exactly one must hold the module. More than one distinct file (compared by canonical path) is **E090 `AmbiguousModuleError`**, naming every file with its kind and a fix-naming hint. Before LEC-001 stage 3 the roots were tried in order with the first hit winning, so a local `json.tr` was silently shadowed by the stdlib's (audit F-19); `FREEZE.md` §6.16 F-PKG-2 forbids that. The roots, listed in the order the candidates are reported:

1. **Installation stdlib** — `$TURF_HOME/lib/std/**/<module>.tr` if the `TURF_HOME` environment variable is set (and non-empty), else `<compiler-binary-dir>/../lib/std/**/<module>.tr` as a zero-config fallback so a freshly built `build/turfc` still finds the sibling `lib/` directory with nothing exported. `TurfInstallRoot()` decides which of the two applies; everything downstream (both this step and the next) is relative to that one root.
2. **Installed user package** — `$TURF_HOME/lib/usr/**/<module>.tr` (or the binary-relative fallback root, same rule as above). No package manager/installer exists yet — this is the *search* half of that story only, ready for whenever one lands. A future installer's contract: one subfolder per module name under `lib/usr/`, e.g. `lib/usr/http/http.tr`.
3. Same directory as the file that contains the `import` (`SourceFilename` at the time of the import; Issue #19 — lets `turfc path/to/file.tr` find sibling modules without a `cd` first).
4. Current working directory.

Three rules keep the all-roots search from producing false ambiguities:
- **Same file, one candidate.** When the importing file's directory *is* the cwd (the test runner always `cd`s into the test's directory), steps 3 and 4 find the same file. Candidates are deduplicated by canonical path, and the first spelling is kept, because that string becomes `ResolvedPath`, the `ImportedFiles`/`ImportStack` identity key.
- **Imports written inside `lib/std` resolve stdlib-only** (`ImportsResolveInStdlibOnly()`). The stdlib is one package, and its own modules are stdlib modules (F-PKG-2). So `strings.tr`'s `import list;` means lib/std's `list` even when the user's program sits beside a `list.tr`. Without this rule, that harmless local file would become an E090 raised inside a stdlib file the user never wrote.
- **The prelude resolves stdlib-only** (`ParsePreludeImports()` passes `StdlibOnly`). It is not an import statement the user wrote, so there is nothing for them to disambiguate. A local `strings.tr` breaks only a program that explicitly writes `import strings;`.

`ResolveModuleFile()` (tooling) uses the same collection and returns `""` for an ambiguous name, since a real import of it is an error, not a file.

**Package builds** (LEC-001 stage 4, `turfc --package-graph <file>`; `include/PackageGraph.h`, `src/PackageGraph.cpp`, INV-PKG-1/2):
- **Resolution.** From a module of package P, `import m;` may name P's own `src/**/m.tr` (every match, so two are E090), the root module `src/m.tr` of a *direct* dependency named `m`, or the stdlib module `m` (`CollectPackageModuleCandidates()`). lib/usr, the importing file's directory and the cwd are not searched. A module that exists in the graph but is out of reach is **E091**: a transitive dependency, or another package's internal module (`ExplainUnreachableModule()`).
- **Identity.** A module's identity is (package, path). A dependency package's modules are identified as `<package>/<path under src/>`, and everything else by its name. The parser rewrites a file's spelling of an imported module to its identity at every qualifier it reads (`ModuleKeyFor()` over the per-file `CurrentFileModuleAliases`), so every later pass (the `Modules_` table, `TypeIdentity`, flat imports, codegen names) keys on identities with no change of its own. Two packages may therefore each have a `util` with a `Node`, and they are two modules and two types. In E088/E089, `DisplayModuleName()` shows a package's root module `geometry/geometry` as `geometry`.
- **Capability.** In a package build `unsafe module;` is honored per package (the graph's `allow_unsafe` bit, see `HasRawPointerCapability()`), and `--allow-unsafe-modules` is not consulted.

Steps 1–2 don't do a flat `<root>/lib/std/<module>.tr` path join — `lib/std` is organized into logical subfolders on disk (`core/`, `collections/`, `io/`, `mem/`, `math/`, `testing/` — purely for humans browsing the repo; a module NAME is never a path, so `import list;` doesn't care that `list.tr` physically lives under `lib/std/collections/`). `IndexModuleDir(Root, "std")` walks the whole subtree once with `llvm::sys::fs::recursive_directory_iterator`, builds a `module-name -> full-path` map, and caches it (keyed by `Root + subdir`) for the rest of the compiler invocation — every subsequent import (there are usually several, including the 6 always-on prelude modules) is then an O(1) map lookup rather than a repeated filesystem walk. Same function, same cache, indexes `lib/usr` for step 2.

`ParsePreludeImports()`'s self-import check (skipping a prelude name that resolves to the file currently being compiled as the entry point — see its own doc comment and AGENTS.md) uses this exact same `TurfInstallRoot()`/`IndexModuleDir()` pair rather than an independent path guess, so it can never disagree with what a real import would have resolved to. That check alone only ever covered a prelude module BEING the entry file — **OVH-011** closes the more general gap it left: the entry file's own top-level parse (`main.cpp`'s own token-consuming loop, never routed through `ParseFile()` at all) had no presence in `ImportedFiles`/`ImportStack` whatsoever, so a transitive import that happened to resolve BACK to the entry file's own path (the concrete case: the always-injected `strings` prelude module's own `import list::List;`, when `lib/std/collections/list.tr` itself is the entry file) triggered a second, fully independent parse of identical content — every struct/function declared twice, `E034`. `ClaimEntryFileImportSlot()` (`src/ParserModule.cpp`) closes it: called once per parse pass, immediately after that pass's own `ParsePreludeImports()` and before the entry file's own top-level loop starts, it either (a) finds the entry path already in `ImportedFiles` — a transitive import during this pass's own prelude injection already parsed it for real — and tells the caller to skip its own top-level loop entirely, or (b) registers the entry path now, before that loop runs, so a LATER self-reference from one of the entry file's own `import` statements (not just a prelude-injected one) also becomes an ordinary cache hit instead of a second, independent re-parse. See that function's own doc comment for the full two-outcome contract, and `docs/development/changes/OVH-011.md` for the general case's one disclosed limitation (a **bare** back-reference is proven; an on-demand/qualified one reaching into the entry file's own symbols is not).

**Setting `TURF_HOME`**: only for a relocated binary that is not sitting next to its own `lib/` (`TURF_HOME` is the installation root, the directory containing `lib/`). When set, it is authoritative over the binary-relative rule. **Do not export it for a dev checkout.** The binary-relative rule already finds the checkout from `build/turfc`'s real path, including through a symlink (`scripts/dev_install.sh`). An exported `TURF_HOME` pointing at one checkout silently makes every *other* build (a worktree, a scratch build directory) compile against that checkout's `lib/std` and runtime. This happened during LEC-001 stage 2 (`docs/development/changes/LEC-001.md`). An earlier version of this paragraph recommended the export.

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
                          GLOBAL on purpose: it answers "was this module loaded anywhere",
                          which UFCS, operators and suggestions need. It does NOT answer
                          "may THIS file write `Module::`" -- that is BareImports_ below.

BareImports_           : unordered_map<ContextLabel, unordered_set<ModuleName>>
                          (ResolverModuleTable, LEC-008.) Per file/module ("" = main
                          file), the modules that file named with a plain `import m;`.
                          Filled in Pass 1a (hoistTypesIn's import branch -- the first
                          place a file's imports are seen, before Pass 1b resolves
                          qualified spellings). isModuleImportedBy(m, ctx) is true for
                          m == ctx or m in BareImports_[ctx]; every place a `m::` is
                          spelled asks it (Resolver::requireModuleImported -> E145).
                          A symbol import is recorded separately (SymbolImports_) and
                          grants nothing; it only lets the hint say "that is not the
                          qualifier". A monomorphized clone is checked against its
                          TEMPLATE's module (resolveMonomorphizedClone pushes it).

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

RegisteredStructs_,     : Keyed by TYPE IDENTITY (FDN-003, 2.6): `M::Name` for a type
RegisteredEnums_,         declared in module M, bare `Name` for the main file's. Two
RegisteredErrorTypes_     modules' same-named types are two entries. Registration aborts
                          (INV-TYPE-3) unless the key's module part matches the declaring
                          module passed with it.

CurrentFileConstants_, : Same shape/keying as the *Functions_ registries above, but for
Modules_[M].Variables,   `export immut` constants -- a Name -> ResolverConstantSymbol map,
FlatImportedConstants_   never a vector (constants don't overload). ResolverConstantSymbol
                          deliberately stores the constant's actual VALUE (a tagged union of
                          int64/double/bool/string), not a pointer back to the initializer's
                          AST node -- this header has a hard "zero LLVM/AST dependency"
                          constraint, and there is no other way to make a reference site
                          usable from Codegen without re-parsing the initializer. Populated
                          during Pass 1 hoisting (Resolver::hoistExportedConstant), consumed
                          by VariableExprAST::ResolvedConstant / ScopeAccessExprAST::
                          ResolvedConstant (set by visitVariable / visitScopeAccess), which
                          Codegen reads directly (CodegenConstantValue(), src/Codegen.cpp) --
                          an exported constant has no alloca, no GlobalVariable, nothing to
                          load from; VarDeclExprAST::codegen() is an explicit no-op for one
                          (IsExported), since it's reached from multiple blind top-level/
                          module-body codegen loops that assume a valid insertion point.

TypeOwningModule_       : unordered_map<TypeIdentity, ModuleName>
                          WHICH module declared each type ("" = main file) -- always
                          equal to TypeIdentity::owningModule(key). "Does module M own
                          a type spelled N" is isKnownTypeIdentity(M::N); "what types
                          are spelled N anywhere" is identitiesWithBareName(N).
```

### 2.6 Type identity is (module, name) — FDN-003

Until FDN-003 (`docs/development/changes/FDN-003.md`, frozen as `FREEZE.md` §6.11) this section explained why struct/enum names were deliberately one flat, program-wide namespace: `LLVMTypeCache` and `GlobalSymbolTable` were keyed by bare name, so two modules declaring `Point` were a permanent conflict (`DuplicateTypeNameError`). That made every type name in every dependency — public or private — reserved for the whole program: a program with no imports could not declare `struct List` (the prelude's `strings` pulls in `list::List`), and `struct Parser` collided with `json`'s internal helper.

Now every name-keyed type table — the Resolver's registry, `GlobalSymbolTable`, `LLVMTypeCache`, method and constructor keys (`"<identity>.method"`), monomorphized clone names — is keyed by the **type identity**, one string per (module, name) pair, built only by `TypeIdentity::make()` (`include/Types.h`):

| Declared in | Identity | LLVM / descriptor / DWARF name |
|---|---|---|
| module `json` | `json::Parser` | `json.Parser` |
| main program file (module `main`, never spelled) | `Parser` | `Parser` |
| generic clone | `list::List_gen1_int` | `list.List_gen1_int` |

`::` cannot occur in an identifier, so no identity can collide with another. Keeping the identity as a plain string (rather than a (module, name) struct) is what let every existing name-keyed table become module-aware without changing its key type; the price is the rule that makes it safe — **a spelling is never used as a key**. Two things produce identities:

- **Declarations** are named by the Parser from the module being parsed (`CurrentParseModule()` — `ImportStack`'s top frame, `src/ParserModule.cpp`), so a `StructDeclExprAST`'s `getName()` *is* its identity from the moment it exists.
- **References** (`Node`, `m::Node`, `Node::new(...)`, `fn Node.method()`) are spellings until the Resolver resolves them — rewriting the AST in place (`NamedType::resolveTo()`, `GenericType::resolveBaseTo()`, `ScopeAccessExprAST::resolveLeftTypeTo()`, a method's `FuncDefExprAST` name). The TypeChecker and Codegen only ever see identities.

A resolved reference is also *marked* resolved (the `Resolved` flags): a main-file type's identity is its bare spelling, so text alone cannot say "already resolved" — and a monomorphized clone is re-resolved in its template's module, where a substituted main-file `Node` must not be re-read as that module's own `Node` (2.8, `resolveMonomorphizedClone()`).

"Does this name exist ANYWHERE" (`identitiesWithBareName()`) still exists, but only to choose between "unknown type" and "not visible here without qualification" diagnostics — never to pick a type.

### 2.7 Resolver layer — Pass 1 (hoisting)

Pass 1 runs in three steps (`Resolver::resolveAll()`, `src/ResolverCore.cpp`), in this order because each needs the previous one complete (FDN-003):

1. **1a — `hoistTypes()`** (`src/ResolverTypeIdentity.cpp`): every `StructDeclExprAST`/`EnumDeclExprAST`/`MachineDeclExprAST`/`ErrorDeclExprAST` in the main file and every module body → the registry under its identity, plus `registerModule()` for each import and `registerOnDemandTypeImports()` — the type half of on-demand imports — so bare type visibility is complete before anything is resolved.
2. **1b — `resolveDeclarationSpellings()`**: every declaration-level type spelling — struct field types, function/extern parameter and return types, a method's receiver (`fn Recv.m` → `<Recv's identity>.m`, found by visibility, since a file may declare a method on an imported struct) — resolved in place, *silently*: an unresolvable spelling is left for Pass 2 to report (Pass 1 aborts the whole `resolveAll()` on its first error). This must precede 3, because a captured signature compares types by name, and precede `checkStructLayoutAcyclicity()`, whose edges are field types' names.
3. `hoistTopLevel()` (main file) and `hoistModuleBody()` (recursive) walk top-level declarations only (no function bodies) and register:
   - `FuncDefExprAST`/`ExternFuncDeclExprAST` → `Modules_[ModuleName]` (module context) or `CurrentFileFunctions_` (main file, `ModuleName.empty()`), via `registerFunctionSignature()`.
   - Nested `ModuleImportExprAST` → recurse. (Type declarations: already done in 1a.)

`hoistModuleBody()` also unconditionally calls `SymTable_.registerModule(ModuleName)` up front, before iterating the body at all. This matters for a module that declares **only** structs/enums and no functions at all: `Modules_[ModuleName]` used to only ever get created lazily, inside `registerModuleFunction()` — a function-less module therefore never got an entry, and `hasModule()` (which, before this fix, additionally required `!Functions.empty()`) reported it as unknown even though it had imported successfully. Every `::`-qualified reference into such a module — `Module::Type` in type position, `Module::Type { ... }` construction, `Module::EnumName::Variant` — routed through `validateModuleQualifier()`'s `hasModule()` check and was rejected with "isn't a known enum or module", even though the qualifier was completely correct. `registerModule()` is now idempotent and called unconditionally so module *existence* is decoupled from module *function count*; `hasModule()` is now a pure `Modules_.count(...)` check.

Immediately after each `ModuleImportExprAST`'s body is hoisted, **`registerOnDemandImports()`** (`src/Resolver.cpp:365-...`) runs, using the now-fully-hoisted target module's registries:
- `Wildcard` → every name `getModuleFunctionNames(SourceModule)` returns is flat-registered as a function; every name `getModuleTypeNames(SourceModule)` returns is flat-registered as a type (via `registerFlatTypeImport()`, into the separate `FlatImportedTypes_` map — see 2.7.1).
- `Symbol`/`SymbolList` → for each requested name, first tried as a function (`findInModules()` filtered to `SourceModule`); if not found there, tried as a type via `getTypeOwningModule(Name) == SourceModule` (**ownership**, not mere existence — see 2.7.1 for why this distinction was itself a bug fix); neither → `UnknownModuleSymbolError`.

#### 2.7.1 Bare struct/enum type visibility gate

Struct/enum type **visibility** (as opposed to existence, 2.6) now follows the identical rule bare function-call visibility already did: a plain `import Module;` grants QUALIFIED access to its types (`Module::Type`) but not bare access; bare access requires either declaring the type in the SAME file/module, or an explicit on-demand import (`import Module::Type;` / `Module::[Type,...]` / `Module::[*]`).

`ResolverSymbolTable::unqualifiedTypeCandidates(ContextLabel, Name)` answers what a bare spelling can mean (FDN-003): `ContextLabel::Name` if that is a registered identity (own declaration — the same rule the function self-reference check already used), plus `SourceModule::Name` if `FlatImportedTypes_[ContextLabel]` maps `Name` to `SourceModule` (on-demand import). One candidate resolves; two is `AmbiguousTypeReferenceError`; none is "not visible" (if `identitiesWithBareName(Name)` is non-empty) or "unknown". `isTypeVisibleUnqualified(ContextLabel, Identity)` is the same question asked of an already-resolved identity (used by `Name::new(...)`'s visibility gate). `FlatImportedTypes_` is a second, parallel map to `FlatImportedFunctions_` (same shape, same `registerFlatFunctionImport`-style collision handling) rather than folded into it, since a struct/enum name and a function name legally occupy different syntactic positions (type position vs. call position) and can coexist under one bare spelling without colliding.

Enforced at the same three call sites `validateModuleQualifier()` already covered: `validateNamedType()`'s `NamedType` branch, its `GenericType` branch (base name), and `visitStructInit()`. Each follows the same shape: if a qualifier is present, validate it via `validateModuleQualifier()` as before (qualified access is **never** gated by this — only bare/unqualified references are); if no qualifier and the name is a real, known struct/enum, check `isTypeVisibleUnqualified(currentModuleName(), Name)` and raise `UnqualifiedTypeNotVisibleError` if false. Type params (`T` in `fn<T> f(x: T)`) are exempted the same way they already were from the existence check.

A related bug was found and fixed alongside this: `registerOnDemandImports()`'s `Symbol`/`SymbolList` branch used to accept ANY struct/enum name that existed ANYWHERE in the program as satisfying `import Module::TypeName;`, never checking that `Module` actually owns it (only `isKnownStruct`/`isEnumName`, the flat existence check — see 2.6). `import mem::List;` (List actually lives in `list`) incorrectly succeeded as long as `list` had been imported by *something* in the program. Fixed by checking ownership instead — today (FDN-003) literally `isKnownTypeIdentity(SourceModule::Name)` — mirroring `validateModuleQualifier()`'s own ownership check for qualified type annotations.
- Flat-registration collisions (`registerFlatFunctionImport()` returning a previous, *different*-module entry) raise `ImportSymbolCollisionError`.
- The `ContextLabel` passed in is `""` for a main-file import, or the *enclosing* module's own name for an on-demand import written inside another module's file (the outer `hoistModuleBody()` call's own `ModuleName` parameter — not the module being imported).

### 2.8 Resolver layer — Pass 2 (the full AST walk)

**`visitFuncCall()`** (`src/Resolver.cpp:706-...`) branches on the callee's AST shape:
- `VariableExprAST` (bare call) → `CurrentFileMatches` from `CurrentFileFunctions_`; `ModuleMatches` from **two** sources ORed together: (a) `findInModules()` filtered to `currentModuleName()` (self-reference, empty string = main file = never matches, correctly), and (b) `lookupFlatFunctionImport(currentModuleName(), CalleeName)` (on-demand imports for this exact context).
- `ScopeAccessExprAST` (`Left::Right(...)`) → dispatches on what `LeftName` actually is, in order:
  1. Known enum → `TypeError` (variants aren't callable).
  2. Known struct → **`StaticMethodRef`** (2.8.1 below) — a true static method call, no instance.
  3. Known module → `ModuleFunctionRef` — `ModuleMatches` from `findInModules()` filtered to the **explicit** `LeftName` (no self-reference concept needed — the module is stated outright). If the module is real but has no function named `func` at all, `ModuleMethodNotFoundError` is raised directly, with suggestions ranked from `getModuleFunctionNames(LeftName)` — this specific branch previously existed as dead code (the error class was defined but nothing constructed/raised it), so a call to a nonexistent module function fell through to a generic, less-specific diagnostic instead.
  4. Neither, but `LeftName` is a real declared variable (an instance) → `ScopeOperatorOnInstanceError`, a more specific "you meant '.'" diagnosis than the generic fallback just below (a genuinely common mistake to make right after learning static methods exist: `p::method(...)` instead of `p.method(...)`).
  5. Neither, and not a variable either → `UnknownScopeQualifierError`.

Both the struct and module branches then share the exact same downstream arity-check/`ResolvedCandidates`-population code (`src/Resolver.cpp` continues past line ~760) — the callee-shape branching only decides *where `ModuleMatches`/`CurrentFileMatches` come from*, not how they're used afterward.

**`visitScopeAccess()`** (`src/Resolver.cpp:1136-...`) handles a `ScopeAccessExprAST` reached as an ordinary expression (i.e. **not** as a call's direct callee — that's the branch above, which never even calls this): enum-variant validation (unchanged from the enum-only era) if `LeftName` is a known enum; `TypeError` ("no first-class functions") if it's a known module or a known struct instead (the struct case: `StructName::method` referenced without being called — same "no first-class functions" reasoning as the module case); the same `ScopeOperatorOnInstanceError` upgrade as `visitFuncCall()` above if `LeftName` is a declared variable; `UnknownScopeQualifierError` as the final fallback.

**`validateNamedType()`** (`src/Resolver.cpp:1167-1263`) — called for every type annotation (params, return types, fields, var decls). For a `GenericType`, now validates the base name itself (previously only recursed into type *arguments* — `NonExistentGeneric<int>` used to silently pass) *and* calls `validateModuleQualifier()` on it; for a `NamedType`, the existing `isKnownStruct`/`isEnumName` check is unchanged, with `validateModuleQualifier()` called afterward if the name resolved.

**`validateModuleQualifier()`** (`src/Resolver.cpp:1265-1290`) — the shared helper both `validateNamedType()` and `visitStructInit()` call:
```cpp
bool Resolver::validateModuleQualifier(const std::string &Qualifier,
                                       const std::string &TypeName,
                                       SourceLocation Loc) const {
  if (Qualifier.empty()) return true;                       // unqualified, nothing to check
  if (!SymTable_.hasModule(Qualifier)) { UnknownScopeQualifierError(...).raise(); return false; }
  if (!requireModuleImported(Qualifier, currentModuleName(), Loc, ...)) return false;   // E145, LEC-008
  if (SymTable_.getTypeOwningModule(TypeName) != Qualifier) { UnknownModuleSymbolError(...).raise(); return false; }
  return true;
}
```

**`visitStructInit()`** (`src/Resolver.cpp:1379-...`) calls `validateModuleQualifier()` for the qualified form, or (since 2.7.1) `isTypeVisibleUnqualified()` for the bare form when `Node.getName()` is a known struct. Note this function still does **not** validate that `Node.getName()` is a real struct at all for the *unqualified* form when it ISN'T a known struct — that's a separate, pre-existing gap (5.2), not addressed by this work (a totally nonexistent bare struct name currently reaches an unrelated error path). The qualifier check happens to still produce a sensible error even then, since an unknown type name has no owning module and therefore can never equal a real `Qualifier`.

**`visitModuleImport()`** (`src/Resolver.cpp:1384-...`) pushes/pops `ModuleStack_`, the backing store for `currentModuleName()` (`include/Resolver.h:199-201`, `ModuleStack_.empty() ? "" : ModuleStack_.back()`) — the single source of truth for "which module's body is Pass 2 currently walking through", consulted by both the self-reference check above and every `registerOnDemandImports()` call's `ContextLabel`.

### 2.8.1 True static struct methods

`fn StructName.method(...) static: RetType { ... }` declares a genuine static method: no implicit `self` parameter at all, callable only as `StructName::method(args)`, never through an instance.

**Declaration syntax note (post struct-construction-overhaul):** this used to be spelled `fn StructName::method(...)` (note `::`, not `.`) — a separate declaration branch keyed on `TOK_SCOPE` right after the function name. That branch is gone: **every** method declaration, static or not, now goes through the single `.`-branch; a trailing `static` keyword (consumed right after the parameter list, before the return-type `:`) is what skips `self`-injection instead of the callee-name token deciding it. This was a deliberate simplification, not just a respelling — `::` had started meaning two structurally different things at the SAME token position depending on context (declaring a static method vs., after this overhaul, nothing at all in declaration position), which is exactly the kind of "colliding grammar" this project avoids elsewhere (see 1.1's `.`/`::` split itself). `::` is now reserved purely for call/reference syntax (enum variants, module functions, static method calls, and `new`/`default`) — it never appears in a `fn` declaration.

**Declaration** (`ParseFuncDef()`, `src/Parser.cpp`): the `.`-branch always does `StructName = FuncName; FuncName = StructName + "." + method` (methods of one struct, static or instance, share ONE flat name — the `.`-joined internal name is an implementation detail, never user-visible). Self-injection is decided AFTER the parameter list is parsed: `IsStaticMethodDecl = (CurTok == TOK_STATIC)` right after `)`; if true, consume it and skip injecting `self: StructName*` as parameter 0. Reusing the exact same internal name means overload resolution, mangling, and forward-reference tolerance all fall out for free — a static and instance method of the same name on the same struct simply become different-arity overloads of one registered name (the presence/absence of `self` as parameter 0 is what actually distinguishes them at the call site, not the `static` keyword itself — `FuncDefExprAST::IsStaticDecl` exists only so `Resolver::visitFuncDef()` can reject `static` in a position where "self absent" would otherwise be ambiguous, e.g. a free non-struct-bound function).

**Reserved names, enforced in `Resolver::visitFuncDef()`:** `default` can never be declared by hand at all (`ReservedConstructorNameError`, any struct-bound method named `default`, static or not) — it's always compiler-synthesized (2.8.1a). `new` can never be `static` (`ConstructorCannotBeStaticError`) — it always constructs via `self`. `static` on a function whose name has no `StructName.` prefix at all is rejected (`StaticOutsideStructError`) — meaningless, since a free function never had a `self` to omit.

**Call resolution** (`visitFuncCall()`, 2.8): when `LeftName` is a known struct, `Node.ResolvedKind = StaticMethodRef` and the lookup is byte-identical to the `ModuleFunctionRef` case — `getTypeOwningModule(LeftName)` gives the owning context ("" = main file, else a real module), then either `lookupCurrentFileFunctions("StructName.method")` or `findInModules("StructName.method")` filtered to that owning module. Not-found reuses `StructMethodNotFoundError` — the SAME class the ordinary `instance.method()` not-found path already raised (via `TypeChecker::visitMethodCall`) — for one consistent message regardless of which call form was used; its hint now explains both the `.` and `::` declaration forms since it's shared between them.

**Qualified access is existence-only, not visibility-gated** — `StructName::method(...)` only requires `isKnownStruct(LeftName)`, exactly like a qualified type annotation (2.7.1) bypasses the bare-type-visibility gate. A plain `import Module;` (bare form, no symbol) is enough to call `Module::StructName::method(...)` or (once the struct itself is bare-visible, e.g. via `import Module::StructName;`) the shorter `StructName::method(...)`.

**Codegen needs zero changes.** `FuncCallExprAST::codegen()` for a `ScopeAccessExprAST` callee (2.10) never distinguishes `ModuleFunctionRef` from `StaticMethodRef` at all — it evaluates `Args` exactly as written (no implicit prepending) and calls whatever `ResolvedCallTarget->mangledName()` resolves to. Static methods "just work" once the Resolver sets `ResolvedCallTarget` correctly, the same way `ModuleFunctionRef` already did.

**A 3-part chain's leading `ModuleQualifier` is validated, not ignored** — `Module::StructName::method(...)` re-validates `ModuleQualifier` via `validateModuleQualifier()` (does `Module` exist, does it actually own `StructName`) before proceeding. Two related bugs were found and fixed while wiring this up, both from the SAME root cause (the parser's 3-part-chain reinterpretation, 2.3, lands the leading name in `ModuleQualifier` — but until now, NOTHING in `visitFuncCall` ever read that field for a call callee, for either the struct or the plain-module case): `mem::Vec2::zero()` used to compile cleanly even when `Vec2` actually lives in a different module (the wrong qualifier was silently ignored, only `LeftName`/`RightName` mattered); and `mem::io::printline(...)` — a module name nonsensically qualified by ANOTHER module name, which has no defined meaning at all — also used to compile identically to plain `io::printline(...)`. Both now raise a clean, specific error instead of silently discarding the (wrong or meaningless) qualifier.

### 2.8.1a Struct construction: `::new(...)` / `::default()`

Replaces the old brace-literal (`StructName { field: val, ... }`, and the `g_AllowStructInit`/`NoStructInitGuard` flag machinery it needed to avoid colliding with every block-taking construct's own `{`) entirely. Dispatched in `Resolver::visitFuncCall()`'s `isKnownStruct(LeftName)` branch, BEFORE the general static-method lookup (2.8.1) — `new`/`default` are reserved names a struct's own methods can never shadow, enforced at declaration time (2.8.1's reserved-names paragraph).

**`StructName::default()`** — no user function involved at all. Set `ScopeCallee->ResolvedKind = DefaultConstruct`; reject any arguments (`DefaultConstructorArityError`). TypeChecker short-circuits at the top of `visitFuncCall()` (before the ordinary builtin/candidate paths, which would otherwise see empty `ResolvedCandidates` and treat it as unresolved) and sets the node's type to a bare `NamedType(StructName)` directly — the same type a resolved struct construction always produced under the old brace-literal form, which is what makes it compatible with a `GenericType` of the same base name on the assignment's other side (see 25_generic_struct_construction.tr's own doc comment for why that compatibility rule exists and still applies). Codegen allocates the struct type, stores `GetDefaultValue()`'s recursive zero-fill, and loads it back — the exact same fallback path the old brace-literal used when no fields were given.

**`StructName::new(args...)`** is declared as an ordinary `.`-method named `new` (self at `Params[0]`) but CALLED with self omitted. Rather than a parallel type-checking implementation, the Resolver builds a **self-stripped, return-type-overridden copy** of each matching `StructName.new` overload — `Params.erase(begin())`, `ReturnType = NamedType(StructName)` (the declared return type is always `void`; the constructed struct is what the CALL EXPRESSION evaluates to) — and pushes those into `Node.ResolvedCandidates` exactly like an ordinary call. This means TypeChecker's existing arity/type-match/generic-substitution loop in `visitFuncCall()` handles overload resolution completely unmodified, landing on the correct struct type on its own. Errors: zero overloads found at all → `NoConstructorDefinedError`; none arity-match → `WrongArityError` against the stripped signatures.

Codegen reconstructs the REAL (self-included) mangled name from the stripped `ResolvedCallTarget` by manually inserting `"_" + StructName + "_ptr"` right after the base name — mirroring `ResolverFunctionSymbol::mangledName()`'s own algorithm with self's mangled type (`TurfPointerType::getMangledName()` = `"<pointee>_ptr"`) reinserted where `Params[0]` used to be — rather than needing the original (unstripped) symbol carried through some new field. It then allocates the struct as `self`, calls the real function, and loads the result — the same alloca-call-load shape the old brace-literal's "user-defined constructor" block used for its `.init()` mechanism (see 22_static_constructor_init.tr's migration notes for how that old mechanism maps onto this one).

**Bare-visibility IS gated, unlike a static method call** — `StructName::new/default()`'s bare (unqualified) form goes through the same `isTypeVisibleUnqualified()` check a `mut x: StructName = ...` type annotation already does (2.7.1), matching what the old `visitStructInit()` enforced for bare brace-literal construction. This is a deliberate asymmetry with 2.8.1's static-method calls (existence-only, never visibility-gated) — constructing a struct is treated the same as naming its TYPE, not as an arbitrary namespaced function call.

**Definite field assignment** (`new()` must assign every field on every path, not just "somewhere in the body") is a genuine control-flow-sensitive dataflow analysis, `ConstructorFieldAssignmentPass` (`include/Analysis.h`/`src/Analysis.cpp`), mirroring `DefiniteAssignmentPass`'s exact shape (forward dataflow, meet-as-intersection at merge points) but tracking `self.<field>` assignment names instead of local-variable identities, and checking the CFG's exit-block fixed-point set (covers every declared field?) rather than reporting per-use-site. Wired into `FuncDefExprAST::codegen()` (Codegen layer, 2.10) alongside the other CFG passes, gated on the function's internal name ending in `.new`. Raises `IncompleteConstructorError`, naming every field missed on at least one path.

### 2.8.2 `.` on a module or a bare struct type name is rejected (fixed, not just documented)

`Resolver::visitMethodCall()` (`src/Resolver.cpp:1064-...`) handles every `Left.method(...)` call — the ENTIRE `.`-call path, separate from `visitFuncCall`'s handling of bare calls and `Left::method(...)`. Resolves `Node`'s arguments first (mirroring `visitFuncCall`'s own ordering, so a broken argument still gets its own diagnostic even when the base is about to be rejected), then unconditionally rejects `.new(...)`/`.default(...)` on ANY base at all (`ConstructorCalledOnInstanceError`) BEFORE the base-shape dispatch below — a real instance calling `.new(...)` would otherwise resolve completely normally, since `new` is registered as an ordinary `.`-declared instance method internally (2.8.1a); a bare struct type name calling either is already separately caught by the dispatch below, for every method name, not just these two. After that check, dispatches on what the base actually is, in priority order:

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

**Name mangling**: a module function's real LLVM name is always `turf_<module>_<name>_<paramtype>_<paramtype>...`, regardless of how many files import it or by which mode — Codegen only ever compiles it once, under its true owning module. Only Resolver-side *source lookup* changes with import mode; Codegen is essentially untouched by the whole on-demand-import feature. A parameter type's mangled component and a method's name carry the type's identity verbatim (`turf_json_json::Parser.parse_json::Parser_ptr`) — `::` is legal in an LLVM/ELF symbol and cannot occur in a spelling, so it cannot collide.

**Type names (FDN-003)**: an LLVM struct, its GC type descriptor (`__turf_typedesc_<name>`, and the name string the runtime sees) and its DWARF type are all named `TypeIdentity::llvmName(identity)` — `json.Parser`, `list.List_gen1_int`; a main-file type keeps its bare name. `LLVMTypeCache` is keyed by the identity itself, never by the LLVM name.

**Struct codegen idempotency**: `StructDeclExprAST::codegen()` (`src/Codegen.cpp:3769-...`) can genuinely be invoked more than once for the SAME struct declared inside a module — `main.cpp`'s pre-pass calls `ModuleImportExprAST::codegen()` twice (once during its prototype-only sub-pass, again during its body sub-pass), and that function unconditionally re-walks the entire module body, including any nested `StructDeclExprAST` nodes, on every call. A second call now detects the cached `llvm::StructType` is already non-opaque (fully built) and returns immediately, rather than falling through to `StructType::create()` again — which used to silently produce a second, LLVM-renamed `Name.0` type and corrupt `LLVMTypeCache` for every reference already built against the original (see 5.4 for the crash this caused before the guard existed).

**`FuncCallExprAST::codegen()`** (`src/Codegen.cpp:2958-...`) derives `CalleeName` from **either** a `VariableExprAST` **or** a `ScopeAccessExprAST` callee (previously only the former — a qualified call used to hit the function's `report_fatal_error("First-class functions are not supported yet.")` fallback, since nothing else could interpret the callee). Everything downstream (`ResolvedCallTarget->mangledName()` lookup, argument marshaling) is unchanged and callee-shape-agnostic.

**`ScopeAccessExprAST::codegen()`** (`src/Codegen.cpp:3718-...`, renamed from `EnumAccessExprAST::codegen()`) is now reached **only** for the enum-variant meaning — a module-function-ref in call position is fully handled by `FuncCallExprAST::codegen()` without ever calling `Callee->codegen()` (a function reference, qualified or bare, has no "value" to compute, only a name lookup), and the Resolver already rejects any non-call use of a module reference before Codegen ever runs.

**`getExprType()`** (`src/Codegen.cpp:242-...`, Codegen's own independent type-inference — a *different* system from TypeChecker's, and a recurring source of drift bugs in this codebase's history) needed two fixes discovered while migrating call sites from `.` to `::`:
1. Its `FuncCallExprAST` branch, like Codegen proper, only recognized a `VariableExprAST` callee — extended to also accept `ScopeAccessExprAST`.
2. The mangled-name reconstruction (`"turf_" + CalleeName + argTypes...`) never accounted for a module prefix at all. This was invisible for int-returning functions (an int compare doesn't care about a null inferred type) but a real, silent **wrong-answer** bug for a string-returning one used inline in an expression: `strings::trim(s) == "hello"` evaluated to `false` even when `trim()` genuinely returned `"hello"`, because the `==` codegen needed an accurate type to choose `strcmp` over a raw pointer comparison and silently got neither. Fixed by recovering the module prefix from `ResolvedCallTarget->SourceModule` while **preserving** the existing call-site-argument-type mangling scheme (needed for generic-function type-erasure substitution) — an earlier, more aggressive fix that used `ResolvedCallTarget->mangledName()` wholesale broke `generic_free_function_return_type.tr`, since that reflects the function's *declared* signature, not the call site's actual instantiation.

### 2.11 Diagnostics / SLM routing

New error classes (`include/Errors.h`) all reuse the existing `SLM::ErrorCategory::DECLARATION` value — none were judged to warrant a new category. `include/ErrorCategory.h`'s `extractContextKeyword()` gained a `"module"`/`"import"` substring check, routing to `docs/slm/modules.txt` via the same `constructDocs` mechanism `structs.txt`/`enums.txt` already use. Ordering matters: this check runs **after** the struct/enum check, not before — `DuplicateTypeNameError`'s own message (for a plain, non-module-related struct-name collision) contains the substring "module" in its explanatory note ("another file or module may reuse it", since FDN-003), which would have misrouted it ahead of `structs.txt` had the module check been checked first. (The exact same class of trap the struct/enum-before-`controlKeywords` ordering already solved once, for `"for enum"` vs. the control-flow keyword `"for"`.)

---

## Part 3 — End-to-end walkthrough: `import io::printline;` then `printline("hi");`

1. **Parse** `import io::printline;`: `ParseImport()` reads `io`, sees `::`, sees a bare identifier (not `[`) → `Mode = Symbol`, `RequestedSymbols = [{"printline", loc}]`. Searches and finds `lib/std/io/io.tr` (via the recursive `lib/std` index — see 2.4), fully parses it into `Body`, records `ResolvedPath`. Returns a `ModuleImportExprAST`.
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
| `include/AST.h` | `ScopeAccessExprAST` (+ `ImportMode`, `ImportedSymbolRef`, `ScopeAccessKind` incl. `ConstructorCall`/`DefaultConstruct`), `ModuleImportExprAST`, `FuncDefExprAST::IsStaticDecl`. `StructInitExprAST` is GONE (deleted with the brace-literal form). |
| `include/Types.h` | `NamedType`/`GenericType`'s `Module` field |
| `src/Parser.cpp` | `ParseImport()`, `ParseType()`'s qualifier handling, `ParsePrimaryBase()`'s `TOK_SCOPE` branch (struct-init branches removed entirely), `ParseFile()`'s `ImportStack`, `ParseFuncDef()`'s `.`-declaration + `static`-keyword handling |
| `include/Resolver.h`, `src/Resolver.cpp` | `hoistTopLevel`/`hoistModuleBody`, `registerOnDemandImports`, `visitFuncCall` (incl. `new`/`default` dispatch), `visitFuncDef` (reserved-name checks), `visitMethodCall` (incl. `ConstructorCalledOnInstanceError`), `visitScopeAccess`, `validateNamedType`, `validateModuleQualifier`, `visitModuleImport`, `currentModuleName`/`ModuleStack_`. `visitStructInit` is GONE. |
| `include/ResolverSymbolTable.h`, `src/ResolverSymbolTable.cpp` | `Modules_`, `CurrentFileFunctions_`, `FlatImportedFunctions_`, `FlatImportedTypes_`, `RegisteredStructs_`/`RegisteredEnums_`, `TypeOwningModule_`, `registerFlatFunctionImport`/`lookupFlatFunctionImport`, `registerFlatTypeImport`/`isTypeVisibleUnqualified`, `getTypeOwningModule`, `ResolverConstantSymbol`, `CurrentFileConstants_`/`Modules_[M].Variables`/`FlatImportedConstants_` |
| `include/TypeChecker.h`, `src/TypeChecker.cpp` | `visitScopeAccess`, `visitMethodCall`, `visitFuncCall` (broadened `CalleeName` derivation, `DefaultConstruct` branch). `visitStructInit` is GONE. |
| `src/Codegen.cpp` | `FuncCallExprAST::codegen()` (incl. `ConstructorCall`/`DefaultConstruct` branches), `ScopeAccessExprAST::codegen()`, `MethodCallExprAST::codegen()`, `getExprType()`, `CodegenConstantValue()`, `VarDeclExprAST::codegen()`'s `IsExported` no-op and its alias-avoidance construction check. `StructInitExprAST::codegen()` is GONE. |
| `include/Analysis.h`, `src/Analysis.cpp` | `ConstructorFieldAssignmentPass` (definite field assignment for `new()`) |
| `include/Errors.h` | `CircularImportError`, `UnknownScopeQualifierError`, `ScopeOperatorOnInstanceError`, `DotOperatorOnNonInstanceError`, `UnknownModuleSymbolError`, `ImportSymbolCollisionError`, `UnqualifiedTypeNotVisibleError`, `ExportOutsideTopLevelError`, `ExportMutNotSupportedError`, `ExportNonConstantInitializerError`, `NoConstructorDefinedError`, `ReservedConstructorNameError`, `StaticOutsideStructError`, `ConstructorCannotBeStaticError`, `DefaultConstructorArityError`, `ConstructorCalledOnInstanceError`, `IncompleteConstructorError` |
| `include/ErrorCategory.h` | `"module"`/`"import"` keyword routing to `docs/slm/modules.txt` |
| `src/Builtins.cpp`, `lib/std/io/io.tr` | `@intrinsic` removal; `print`/`printline`/`input` now plain Turf + `extern fn` FFI |
| `lib/std/*.tr` | The 7 real stdlib modules (`io`, `mem`, `math`, `list`, `stack`, `queue`, `strings`), each an ordinary `.tr` file, no compiler special-casing |

---

## Part 5 — Known limitations, deliberately deferred

### 5.1 (Resolved) No true static struct methods

Previously documented here as a known limitation — fixed; see 2.8.1. A genuine static method, callable as `StructName::method(args)` with no instance, is fully supported (declared `fn StructName.method(...) static: RetType { ... }` — see 2.8.1's note on the `::`-declaration form this originally shipped with, later respelled to `.` + `static`). Left as a pointer rather than deleted outright, same rationale as 5.3 below.

### 5.2 (Resolved) Unqualified struct-literal construction of a nonexistent struct

Previously documented here as a known limitation of the old brace-literal form (`StructName { ... }`'s `visitStructInit()` validated a qualifier and, since 2.7.1, bare-visibility for a name that WAS a known struct, but never validated that a totally nonexistent bare struct name was real at all — it fell through to an unrelated downstream error instead of a clean `UnknownTypeError`). Moot now: brace-literal construction doesn't exist anymore. Its replacement, `StructName::new(...)`/`StructName::default()` (2.8.1a), goes through the SAME `isKnownStruct(LeftName)` gate every other `::`-on-a-struct form already used, so a nonexistent struct name here falls through to `UnknownScopeQualifierError` (or, if it happens to shadow a real variable, `ScopeOperatorOnInstanceError`) — the same clean, existing diagnostics any other unknown `::` qualifier gets, not a struct-construction-specific gap.

### 5.3 (Resolved) Struct/enum bare-type visibility gate

Previously documented here as a known limitation (`List<int>` working unqualified after just `import list;`) — fixed; see 2.7.1. Left as a pointer rather than deleted outright so anyone who read this doc before the fix landed can find where the behavior changed.

### 5.4 A general note on cross-pass struct codegen safety

While hunting the visibility-gate work above, a real compiler CRASH was found and fixed: a struct declared inside a module, used BY VALUE (not just via pointer/`self`) by a free function in that same module, crashed the whole compiler process with an LLVM IR verifier failure (`ret %Point.0 %structval` against a function still declaring `%Point`). Root cause: `main.cpp`'s pre-pass invokes `ModuleImportExprAST::codegen()` twice (once per prototype/body sub-pass), and it unconditionally re-walks the entire module body — including any `StructDeclExprAST` nodes — on every call, with no guard against re-running struct codegen. `StructDeclExprAST::codegen()` (2.10) now tolerates being called more than once for the same struct (returns immediately if the cached LLVM type is already fully built, instead of creating a second, LLVM-renamed `Name.0` type and corrupting the cache for every already-built reference). Never caught before because every prior struct-in-module test only ever used instance methods (pointer receiver, unaffected by which concrete `StructType` backs it) or constructed the struct in the same file it was declared in — a free function passing/returning a module struct BY VALUE across the import boundary was untested territory until this bug hunt.
