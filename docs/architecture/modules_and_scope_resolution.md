# The Module System and `::` Namespace Resolution

Reference for the module/import system, module-scoped type identity,
visibility (`pub`), package-aware module resolution, and the `::` operator.
Part 1 states what a TurfLang programmer observes. Part 2 states how the
compiler implements it, pass by pass. The user-facing counterpart of Part 1 is
`docs/reference/functions_and_modules.md`; the pipeline these passes sit in is
`docs/architecture/compiler_pipeline.md`.

Related:

- `docs/slm/modules.txt`: the short, example-driven quick reference used by the
  SLM diagnostic assistant.
- `docs/development/FREEZE.md`, sections 6.11 (type identity), 6.15 (`pub`),
  6.16 (packages), 6.24 (imports) and 6.25 (the prelude): the frozen rules.

---

## Part 1: External reference (language semantics)

### 1.1 `.` is instance-only, `::` is namespace-only

Two kinds of qualified access exist and they are never interchangeable:

| Operator | Means | Example |
|---|---|---|
| `.` | something on an **instance you hold** | `p.x`, `p.sum()` |
| `::` | something **by name through a namespace**, no instance | `Color::Red` (enum variant), `io::printline(...)` (module function), `list::List<int>` (module type), `Point::new(...)` (static method or constructor) |

`io.printline(...)` and `StructName.method(...)` (a bare type name with no
instance) are rejected as a **semantic** error (`DotOperatorOnNonInstanceError`,
E048), not a syntax error. The parser cannot know that `io` is not a variable,
so both parse as ordinary method calls. `Resolver::visitMethodCall()` finds that
the base names a module or a struct type and not a declared variable, and
points at the `::` form. A local variable with the same spelling as a module or
type always wins, as in any lexical scoping: `mut io: Thing = ...; io.show();`
calls `Thing.show()`.

### 1.2 Import forms

Four forms, all starting with `import <module-name>`:

| Form | Syntax | Effect |
|---|---|---|
| Bare | `import io;` | The module is reachable **only** through `io::`. Nothing is unqualified. |
| Single symbol | `import io::printline;` | `printline` is bare-callable. This does **not** make `io::printline` legal: that needs `import io;` too. |
| Symbol list | `import io::[print, printline];` | The same, for every listed name. |
| Wildcard | `import io::[*];` | Every `pub` name the module exports becomes bare, and still no `io::`. |

A module is always parsed in full, whatever the form: even a single-symbol
import reads the whole target file. The form decides only what is added to the
importing file's own namespace.

**No transitive imports (`FREEZE.md` 6.24, F-IMP-1/2).** A file may write `m::`
(a call, a constant, a type, `m::T::new()`) only if it wrote a plain
`import m;` itself. The prelude's modules and the module's own body are the only
exceptions. A module that some other import happened to load is not visible:
after `import json;` (which imports `list`), `list::List<int>` is E145, with a
hint naming the line to add. The symbol forms import names, not the qualifier,
so a file that wants both `List` and `list::List` writes `import list::List;`
and `import list;`. What is deliberately not gated (F-IMP-4) is anything that
follows the **type** and not a spelled name: methods, UFCS calls (`x.f()`),
operators, and holding or passing a value whose type came from a module the
file never imported. Only *spelling* `io::T` needs the import (F-IMP-3).

### 1.3 Visibility

**`pub` (`FREEZE.md` 6.15).** A module's top-level functions, types, methods
and struct fields are private unless written `pub` (or `public`, a synonym).
Another module reaches only the `pub` ones.

- Every cross-module reference to a function or type is checked against the
  declaration's `pub` mark: by qualified name (`m::f()`, `m::T`,
  `m::T::default()`, `m::T::Variant`) and by `import m::name;`.
- A module always sees its own declarations.
- `import m::[*]` brings in only the `pub` names, so a private name of `m` can
  never collide with one of yours. An explicit `import m::helper;` of a private
  name is `E088`, not "unknown symbol": the module has it, it did not export it.
- If a name has both `pub` and private overloads, only the `pub` ones take part
  in overload resolution from outside.
- A struct's fields and methods follow the same rule even in a `pub` struct:
  `p.field`, `p.method()`, `T::method()`, `T::new()` and a UFCS call `p.f()` are
  all checked (a private free function is not a UFCS candidate outside its
  module). `T::default()` is exempt because it only zero-fills.
- A `pub` signature or field may not mention a private type of its own module
  (E089).
- `export immut` constants are always public (`export` is their `pub`).
  Operators are not gated: they are visible wherever their operand types are.
- `pub` where it means nothing (on an operator, import, variable, variant,
  parameter, inside a function, with `export`, before a decorator, or as a name)
  is `E087`.
- `turf --migrate=pub [--write] <module.tr>...` puts `pub` on every top-level
  declaration and field of a 1.x module as a starting point; without `--write`
  it only reports.

**Bare calls to functions** resolve, in this order, to:

1. Something declared directly in the current file, *when that file is the main
   program*. A module's body never sees the main program's functions: an
   importer cannot leak into what it imports.
2. If the call is inside a module's own body, another function declared in that
   **same** module (self-reference).
3. A name brought in by an on-demand import (forms 2 to 4 above), scoped to the
   file or module whose `import` statement requested it.
4. Only if none of 1 to 3 has any function of that name, the prelude tier
   (`FREEZE.md` 6.25): a public free function of `io`, `math` or `sys`. So
   `printline("x")`, `abs(-3)` and `exit(1)` work with no import, and a
   declaration of your own (or an explicit import) of the same name hides the
   prelude's instead of making the call ambiguous.

A local variable of function type that shares the callee's name is called as a
function value; a local of any other type that shares it is an error, not a
silent call to the same-named function.

The nine prelude modules (`int`, `double`, `bool`, `char`, `strings`, `array`,
`io`, `math`, `sys`) are known to every compile and may be spelled `m::` in every
file. Six of them are reached only through UFCS, operators and `m::`; `io`,
`math` and `sys` are the three whose public free functions are also
bare-callable (`PreludeBareModuleNames`, `src/frontend/ParserModule.cpp`; the
tool-facing list is `turf::preludeBareModules()` in `include/turf/Analysis.h`).
The prelude tier is a closed named list, consulted last and shadowed by
anything the file says; it is not a flattened auto-import.

**Struct and enum type names** follow the same rule bare function names do. A
plain `import list;` grants **qualified** access only (`list::List`); a bare
`List` is visible only if the same file or module declares it, or brought it in
by an on-demand import (`import list::List;`, `import list::[List, ...];`,
`import list::[*];`).

Qualifying always means the same thing as the unqualified spelling:
`list::List<int>` and `List<int>` name the same type. Qualifying a *function*
call is sometimes the only way to reach it (anything not on-demand imported).

### 1.4 Module-qualified types and construction

Both spellings are valid and name the same type:

```turf
import list;
import list::List;

mut a: List<int>       = List::default();
mut b: list::List<int> = list::List::default();
```

Each spelling needs its own import: `import list::List;` makes the bare `List`
legal and `import list;` makes the qualified `list::List` legal, because symbol
imports grant no qualifier.

Struct construction is `StructName::new(args...)` or `StructName::default()`
only; there is no brace-literal form (the parser reports `RemovedSyntax` with a
hint at `::new`/`::default`). `List` has no user-defined `new`, so
`::default()` is how to construct it.

Qualifying is **validated**, not decorative. Whenever `Module::Name` appears, in
type position or construction position, three checks run in order:

1. Is `Module` a real module? (`UnknownScopeQualifierError`, E031.)
2. Did *this file* import it with a plain `import Module;`? (`ModuleNotImportedError`,
   E145, if the module exists only because another file loaded it.)
3. Does `Module` itself declare `Name`? (`UnknownModuleSymbolError`, E030, if
   some other module owns it: `mem::List` is rejected even though `mem` is real,
   because `List` lives in `list`.)

### 1.5 `::` for enum variants

```turf
enum Color { Red, Green, Blue }
immut c: Color = Color::Red;
Color::COUNT    // int: number of variants
Color::VALUES   // T*: pointer to array of every variant's underlying int value
Color::NAMES    // string*: pointer to array of every variant's name
```

The parser treats `Left::Right` uniformly. It never knows whether `Left` is an
enum, an error type, a struct or a module; that is decided after parsing
(Part 2.3).

### 1.6 Collisions and circular imports

Two on-demand imports that flatten two *different* symbols under one bare name
are an error, not a silent shadow (`ImportSymbolCollisionError`, E024):

```turf
import moduleA::shared;
import moduleB::shared;   // error
```

Importing a name the module does not have errors at the `import` itself
(`import io::nosuchfunction;` is E030).

A module that imports another which imports it back, directly or transitively,
is a compile error with the chain rendered (`CircularImportError`, E019):

```text
Circular import detected -- these modules are waiting on each other.

    ca  --imports-->  cb  --imports-->  ca  <- back to the start!
```

A **diamond** (two modules importing the same third) is not a cycle and
compiles; only a loop back to a module still being loaded triggers it (Part 2.4).

**Types belong to their module** (`FREEZE.md` 6.11). A struct, enum, machine or
`error` type is identified by *(owning module, name)*. Two modules may each
declare `Node`, and a program may declare its own `struct List` or
`struct Parser` whatever it imports; they are distinct types (`a::Node`,
`b::Node`, `List`). Declaring one name twice in the *same* file is the error
(`DuplicateTypeNameError`, E034). `typeof()` prints the identity:
`"json::JsonValue"` for an imported type, bare `"Node"` for one declared in the
main program file (whose module, `main`, is never spelled).

A bare type name that could mean two types is rejected **at the use**, naming
both. There is no "the local one wins":

```turf
import shapes::[*];        // shapes declares `struct Point`
struct Point { x: int }    // ...and so does this file: both legal

mut p: Point = ...;        // AmbiguousTypeReferenceError (E085): write
                           // `shapes::Point` for theirs, and leave Point
                           // out of the import
```

Two on-demand imports that both bring in a `Point` are rejected at the second
import (`ImportSymbolCollisionError`), as for functions.

### 1.7 Exporting constants: `export immut`

Functions, structs, enums, errors and machines are exported with `pub`. A plain
variable only makes sense inside a function, so top-level constants have their
own keyword, which covers exactly that gap:

```turf
export immut PI: double = 3.14159;
export immut MAX_USERS: int = 100;
export immut GREETING: string = "hello";
export immut DEBUG: bool = true;
```

- **Top level only.** `export` inside a function body is
  `ExportOutsideTopLevelError` (E023). It parses in any statement position, so
  the Resolver enforces this by checking function-nesting depth.
- **`immut` only.** `export mut` is rejected at parse time
  (`ExportMutNotSupportedError`, E021): a mutable shared global would need
  initialization ordering and real global-variable codegen.
- **The initializer must be a literal** (`ExportNonConstantInitializerError`,
  E022). An exported constant has no storage at all: every reference is
  replaced by the literal value at compile time. There is no global, no alloca,
  nothing to load.
- **Otherwise it is an ordinary variable.** Typo detection, duplicate-name
  detection, and immutability all apply; `export` changes only where it can be
  declared and how far it is visible.

Access follows the rules of functions (1.3):

```turf
// consts.tr
export immut VERSION: int = 7;
```

```turf
import consts;
consts::VERSION        // qualified access works once imported

import consts::VERSION;
VERSION                // bare access: only after an on-demand import, or inside consts.tr
```

### 1.8 Error reference

| Error | Code | Raised when |
|---|---|---|
| `ModuleNotFoundError` | E027 | `import xyz;` and no `xyz.tr` exists on any search root |
| `AmbiguousModuleError` | E090 | the name is found in more than one place (2.4) |
| `ModuleNotImportableError` | E091 | a package build: the module exists but is not reachable from this package (2.4) |
| `CircularImportError` | E019 | a module import chain loops back on itself |
| `UnknownScopeQualifierError` | E031 | `Left::Right` where `Left` is not a known enum, error type, struct, module, or declared variable |
| `ModuleNotImportedError` | E145 | `m::name` (call, constant, type, construction) in a file with no plain `import m;` of its own; the hint prints the line to add |
| `ScopeOperatorOnInstanceError` | E029 | `::` with a base that is a declared variable, e.g. `p::method(...)` for `p: SomeStruct`; points at `.` |
| `DotOperatorOnNonInstanceError` | E048 | `.` with a base that names a module or a bare struct type; points at `::` |
| `UnknownModuleSymbolError` | E030 | `import module::symbol;` or a qualified type/construction/static-method qualifier names something the module does not have |
| `ImportSymbolCollisionError` | E024 | two on-demand imports flatten two different symbols under one bare name |
| `UnknownFunctionError` | E041 | a bare call that is not a local declaration, self-reference, on-demand import, or prelude function |
| `ModuleMethodNotFoundError` | E026 | `Module::func(...)` where `Module` is real but has no such function |
| `StructMethodNotFoundError` | E054 | `StructName::method(...)` where the struct has no method by that name |
| `UnqualifiedTypeNotVisibleError` | E032 | a real struct/enum referenced bare but not visible from this file or module |
| `UnknownTypeError` | E042 | a type name that is not a struct, enum, or primitive anywhere |
| `AmbiguousTypeReferenceError` | E085 | a bare type name that could mean two visible types |
| `DuplicateTypeNameError` | E034 | the same type name declared twice in one file or module |
| `PrivateSymbolAccessError` | E088 | a function, type, method or field another module did not mark `pub`, reached by qualified name, by `import m::name;`, through a receiver, or through `T::new`/`T::method`; the hint names the file and the exact `pub` spelling, and lists the public alternatives |
| `PrivateInPublicError` | E089 | a `pub` function, method, extern fn or field whose signature mentions, at any depth (generic argument, `!T`, ...), a type its module did not mark `pub`; the main file is exempt |
| `MisplacedVisibilityError` | E087 | `pub` where visibility means nothing (1.3) |
| `ExportOutsideTopLevelError` | E023 | `export` inside a function body |
| `ExportMutNotSupportedError` | E021 | `export mut` |
| `ExportNonConstantInitializerError` | E022 | `export immut X = <non-literal>;` |
| `NoConstructorDefinedError` | E050 | `StructName::new(...)` for a struct with no `new` |
| `ReservedConstructorNameError` | E051 | `fn StructName.default(...)` declared by hand; `default` is always synthesized |
| `StaticOutsideStructError` | E053 | `static` on a function that is not struct-bound |
| `ConstructorCannotBeStaticError` | E046 | `fn StructName.new(...) static: ...` |
| `DefaultConstructorArityError` | E047 | `StructName::default(x, ...)` |
| `ConstructorCalledOnInstanceError` | E045 | `instance.new(...)` or `instance.default(...)` |
| `IncompleteConstructorError` | E049 | `new()` does not assign every field on every path |

`TypeError` also covers calling an enum variant as a function, using a module or
static-method reference as a bare value, a module name qualified by another
module name (`Module::Module2::func(...)`), and a generic struct named without
type arguments in a type annotation.

---

## Part 2: Internal architecture

### 2.1 Pipeline recap

```
source.tr -> Lexer -> Parser -> Resolver (pass 1, pass 2) -> TypeChecker -> Monomorphizer -> Codegen
```

`runCompilerMain()` runs a pre-pass (its own Resolver and TypeChecker) before the
authoritative pass, so that prototypes and struct layouts exist before bodies.
Two consequences matter here and are described below: the entry file is parsed
twice, so import bookkeeping is reset between the passes (2.4), and module
bodies are codegenned twice (2.10). See `compiler_pipeline.md` section 0.

### 2.2 Lexer: `::` is `TOK_SCOPE`

`gettokScan()` (`src/frontend/Lexer.cpp`) peeks after a `:` for a second `:` and
returns `TOK_SCOPE`; there is no special-casing of what follows. `::` has no
other meaning in the grammar, and exactly one place upstream of the Resolver
consumes it (2.3).

### 2.3 AST representations

**`ScopeAccessExprAST`** (`src/frontend/ASTAccess.h`) is the node for any
`Left::Right`:

```cpp
class ScopeAccessExprAST : public ExprAST {
  std::string    LeftName, RightName;
  SourceLocation LeftLoc, RightLoc, Loc;
  std::string    ModuleQualifier; // "" unless this is a 3-part Module::Name::Right chain
public:
  ScopeAccessKind ResolvedKind = ScopeAccessKind::Unresolved; // set by the Resolver
  /* ... */
};
```

`ScopeAccessKind` is `Unresolved`, `EnumVariant`, `ModuleFunctionRef`,
`ModuleTypeRef`, `StaticMethodRef`, `ModuleConstantRef`, `ConstructorCall`,
`DefaultConstruct`, plus the error-type kinds (`ErrorVariantRef` and
`ErrorVariantConstruct`). The parser fills in only names and locations. It has no
opinion on what kind of `::` this is; `ResolvedKind` is set by the Resolver once
`LeftName`'s identity is known. `getEnumName()` and `getVariant()` alias
`LeftName` and `RightName` for the enum-specific consumers.

**3-part chains** (`Module::Name::Right`, for example `colors::Shade::Dark` or
`mem::Vec2::zero()`): the primary-expression parser (`ParsePrimaryBase()`'s
identifier case, `ParserCore.cpp`) builds a 2-part node from
`Identifier '::' Identifier`, then peeks for a second `TOK_SCOPE`. On a hit the
node is rebuilt with the first identifier reinterpreted as `ModuleQualifier`, the
middle one as `LeftName`, and the last as `RightName`. The first identifier is
first mapped through `ModuleKeyFor()` (2.4), so a package alias becomes the
module's identity. The Resolver validates `ModuleQualifier` with
`validateModuleQualifier()` (2.8) exactly as a qualified type annotation, so a
3-part chain gets the same "is this the right module for that name" check as
`Module::Name` in type position.

**`ModuleImportExprAST`** (`src/frontend/ASTDecl.h`):

```cpp
class ModuleImportExprAST : public ExprAST {
  SourceLocation Loc;          // the `import` statement
  std::string    ModuleName;
  std::string    ResolvedPath; // canonical path actually loaded (2.4)
  std::unique_ptr<ExprAST> Body; // the ENTIRE parsed module file
  ImportMode Mode;               // Bare | Symbol | SymbolList | Wildcard
  std::vector<ImportedSymbolRef> RequestedSymbols; // name + its own SourceLocation
};
```

`Body` is always the full, recursively parsed module, per 1.2.

**Struct construction** (`StructName::new(...)`, `StructName::default()`) has no
node of its own. It parses as an ordinary `ScopeAccessExprAST` callee with
`RightName` `new` or `default`; the Resolver sets `ResolvedKind` to
`ConstructorCall` or `DefaultConstruct` (2.8.2).

**`NamedType` and `GenericType`** (`src/sema/Types.h`) each have a `Module`
field: the qualifier **as spelled**. The Resolver rewrites the type's name itself
to its identity (`list::List`, 2.6), so `getName()` carries the module for every
later pass; the field only records what the source wrote, so the qualifier can
be validated once, at first reference.

### 2.4 Parser layer: finding and loading modules

**`ParseImport()`** (`src/frontend/ParserModule.cpp`) reads the module name
(the primitive-type keywords `int`, `double`, `bool`, `char` are accepted as
module names, since they lex as type tokens), then optionally `::` followed by
`Symbol`, `[a, b]` or `[*]`, building `Mode` and `RequestedSymbols`. It then
calls `ParseModuleImportByName()`, which searches for the file and parses it.
The file that is found becomes `ResolvedPath`, the identity key that both
circular-import detection and per-file import bookkeeping share.

**Search roots and `TURF_HOME`.** `CollectModuleCandidates()` examines up to four
roots and **all** of them: exactly one distinct file (compared by canonical path)
may hold the module. More than one is `E090 AmbiguousModuleError`, naming every
file with its kind and a hint. The roots, in the order candidates are reported:

1. **Installation stdlib**: `$TURF_HOME/lib/std/**/<module>.tr` if `TURF_HOME` is
   set and non-empty, else `<compiler-binary-dir>/../lib/std/**/<module>.tr`, so
   a freshly built `build/turfc` finds its sibling `lib/` with nothing exported.
   `TurfInstallRoot()` decides which applies; the next root uses the same one.
2. **Installed user package**: `$TURF_HOME/lib/usr/**/<module>.tr` (or the
   binary-relative root). The convention for an installer is one subfolder per
   module under `lib/usr/`, such as `lib/usr/http/http.tr`.
3. **Beside the importing file**: the directory of `SourceFilename` at the time of
   the import, so `turfc path/to/file.tr` finds sibling modules without a `cd`.
4. **The current working directory.**

Three rules keep an all-roots search from raising false ambiguities:

- *Same file, one candidate.* When the importing file's directory is the working
  directory (the test runner always `cd`s into the test's directory), roots 3 and
  4 find the same file. Candidates are deduplicated by canonical path, and the
  first spelling is kept because that string becomes `ResolvedPath`, the key of
  `ImportedFiles` and `ImportStack`.
- *Imports written inside `lib/std` resolve stdlib-only*
  (`ImportsResolveInStdlibOnly()`). The stdlib is one package, so `strings.tr`'s
  `import list;` means lib/std's `list` even when the user's program sits beside
  a `list.tr`. Without this, a harmless local file would become an E090 raised
  inside a stdlib file the user never wrote.
- *The prelude resolves stdlib-only.* `ParsePreludeImports()` passes
  `StdlibOnly`, because the prelude is not an import the user wrote, so there is
  nothing for them to disambiguate. A local `strings.tr` breaks only a program
  that explicitly writes `import strings;`.

`ResolveModuleFile()` uses the same collection and returns `""` for an ambiguous
name, since a real import of it is an error and not a file. The query facade
exposes it as `turf::resolveModuleFile()`.

**The stdlib directory layout is not part of the language.** `lib/std` is split
into subfolders for people (`core/`, `collections/`, `io/`, `mem/`, ...). A
module name is never a path, so `import list;` does not care that `list.tr` is
under `lib/std/collections/`. `IndexModuleDir(Root, "std")` walks the tree once
with `llvm::sys::fs::recursive_directory_iterator`, builds a
`module name -> path` map, and caches it (keyed by root and subdirectory) for the
rest of the compiler invocation, so every later import is a map lookup. The same
function and cache index `lib/usr`.

**The prelude and the entry file.** `ParsePreludeImports()` injects
`import int; import double; ...` (the nine modules of 1.3) as if they were the
first lines of the entry file. When the file being compiled *is* one of those
modules, it skips that name, using the same `TurfInstallRoot()`/`IndexModuleDir()`
pair as a real import so the two cannot disagree. The more general gap is a
transitive import that resolves back to the entry file itself (the prelude's
`strings` imports `list::List`, so compiling `lib/std/collections/list.tr`
directly would reach itself): the entry file's own top-level parse never went
through `ParseFile()`, so without help the same declarations would be registered
twice (`E034`). `ClaimEntryFileImportSlot()` closes it. It runs once per parse
pass, right after that pass's `ParsePreludeImports()` and before the entry file's
top-level loop. Either the entry path is already in `ImportedFiles` (a transitive
import already parsed it), and the caller skips its own loop; or the path is
registered now, so a later self-reference from the entry file's own imports is an
ordinary cache hit. It also registers the file's stdlib alias, because a
transitive import reaches the file through the search path and not through the
command-line spelling. The limitation: a bare back-reference is covered; an
on-demand or qualified one reaching into the entry file's own symbols is not.

**Setting `TURF_HOME`** is for a relocated binary that is not next to its own
`lib/` (it is the installation root, the directory containing `lib/`). When set
it is authoritative over the binary-relative rule. **Do not export it for a dev
checkout.** The binary-relative rule already finds the checkout from `build/turfc`'s
real path, including through a symlink (`scripts/install/dev_install.sh`), and an
exported `TURF_HOME` pointing at one checkout silently makes every other build
(a worktree, a scratch build directory) compile against that checkout's
`lib/std` and runtime. A stale value makes every compile fail with E027.

**Package builds** (`turfc --package-graph <file>`; `src/tooling/PackageGraph.h`,
`src/tooling/PackageGraph.cpp`; `FREEZE.md` 6.16):

- *Resolution.* From a module of package P, `import m;` may name P's own
  `src/**/m.tr` (every match, so two is E090), the root module `src/m.tr` of a
  *direct* dependency named `m`, or the stdlib module `m`
  (`CollectPackageModuleCandidates()`). `lib/usr`, the importing file's directory
  and the working directory are not searched. A module that exists in the graph
  but is out of reach is **E091** (`ModuleNotImportableError`): a transitive
  dependency, or another package's internal module (`ExplainUnreachableModule()`).
- *Identity.* A module's identity is (package, path). A dependency package's
  modules are identified as `<package>/<path under src/>`, and everything else by
  its name. The parser rewrites a file's spelling of an imported module to its
  identity at every qualifier it reads (`ModuleKeyFor()`, over the per-file
  `CurrentFileModuleAliases`), so every later pass (the `Modules_` table,
  `TypeIdentity`, flat imports, codegen names) keys on identities without change.
  Two packages may each have a `util` with a `Node`, and they are two modules and
  two types. In E088/E089, `DisplayModuleName()` shows a package's root module
  `geometry/geometry` as `geometry`.
- *Capability.* In a package build `unsafe module;` is honored per package (the
  graph's `allow_unsafe` bit, see `HasRawPointerCapability()`), and
  `--allow-unsafe-modules` is not consulted.

**`ParseType()`** (`ParserCore.cpp`), after the base identifier, checks for
`TOK_SCOPE` before its `<` generic-argument check. If present, the identifier
just read was the module qualifier: it is mapped through `ModuleKeyFor()` and
carried into the `NamedType`/`GenericType`. It is **not validated here**: module
registration happens during hoisting, after the entire program is parsed, so
nothing is known yet about which modules exist. Validation is deferred to the
Resolver (2.8).

**Circular import detection** is in `ParseFile()`, the function
`ParseModuleImportByName()` calls: a stack, disjoint from the `ImportedFiles`
cache,

```cpp
struct ImportStackFrame { std::string ModuleName, ResolvedPath; SourceLocation ImportLoc; };
static std::vector<ImportStackFrame> ImportStack;
```

is checked **before** the `ImportedFiles` dedup check: a module still on
`ImportStack` is an in-progress cycle, whereas a module already fully parsed and
not on the stack is the ordinary "reuse it" case (a diamond). An RAII guard
pushes and pops the frame around the body-parsing loop so no early return can
leak a stale entry. On a hit, `CircularImportError` renders the chain by walking
`ImportStack` from the re-entered frame to the top (`BuildCircularImportChain()`).
`ImportStack`'s top frame is also what names the module being parsed:
`CurrentParseModule()` gives a declaration its type identity (2.6).

### 2.5 Resolver layer: data structures

`ResolverSymbolTable` (`src/resolve/ResolverSymbolTable.h`) is a thin composite
over three sub-tables, each LLVM-free: `ResolverScopeTable` (lexical scopes),
`ResolverModuleTable` (module and import registries) and `ResolverTypeRegistry`
(types). The module-related state:

```
Modules_               unordered_map<ModuleName, ResolverModule>
                       Every function/extern and `export immut` constant declared in
                       EVERY loaded module, namespaced per module. Backs qualified calls
                       (Module::func(...)) and findInModules() suggestions. GLOBAL on
                       purpose: it answers "was this module loaded anywhere", which UFCS,
                       operators and suggestions need. It does NOT answer "may THIS file
                       write `Module::`": that is BareImports_.

BareImports_           unordered_map<ContextLabel, unordered_set<ModuleName>>
                       Per file or module ("" = main file), the modules that file named
                       with a plain `import m;`. Filled in pass 1a (hoistTypesIn's import
                       branch, the first place a file's imports are seen).
                       isModuleImportedBy(m, ctx) is true for m == ctx or m in
                       BareImports_[ctx]; every place a `m::` is spelled asks it, through
                       Resolver::requireModuleImported() (E145). A symbol import is
                       recorded separately (SymbolImports_) and grants nothing; it only
                       lets the hint say "that is not the qualifier". A monomorphized
                       clone is checked against its TEMPLATE's module.

CurrentFileFunctions_  unordered_map<Name, vector<Overload>>
                       The MAIN FILE's own top-level fn/extern declarations ONLY. Nothing
                       declared inside a module goes here (those go to Modules_ through
                       registerModuleFunction).

FlatImportedFunctions_ unordered_map<ContextLabel, unordered_map<Name, FlatImportedSymbol>>
FlatImportedTypes_     On-demand imports (Symbol/SymbolList/Wildcard), keyed by WHICH
FlatImportedConstants_ file or module requested them ("" = main, else a module name). Each
                       stores only {SourceModule, ImportLoc}: a name-to-module ROUTING
                       table, not a copy. Overload resolution still goes back through
                       findInModules(), filtered to SourceModule. Three maps because a
                       function, a type and a constant can legally share one bare
                       spelling (they occupy different syntactic positions).

RegisteredStructs_,    (ResolverTypeRegistry) keyed by TYPE IDENTITY: `M::Name` for a type
RegisteredEnums_,      declared in module M, bare `Name` for the main file's. Two modules'
RegisteredErrorTypes_  same-named types are two entries. Registration checks that the
                       key's module part matches the declaring module passed with it.

TypeOwningModule_      unordered_map<TypeIdentity, ModuleName>
                       WHICH module declared each type ("" = main file); always
                       TypeIdentity::owningModule(key). "Does module M own a type
                       spelled N" is isKnownTypeIdentity(M::N); "what types are spelled N
                       anywhere" is identitiesWithBareName(N).

TypeVisibility_        identity -> {IsPublic, DeclLoc, keyword}, marked in pass 1a, so a
                       diagnostic about a private type can point at its declaration.

CurrentFileConstants_, `export immut` constants: a Name -> ResolverConstantSymbol map,
Modules_[M].Variables  never a vector (constants do not overload). The symbol stores the
                       constant's VALUE (a tagged union of int64/double/bool/string, the
                       integer kept as an exact decimal string), not a pointer to the
                       initializer's AST node: the symbol-table headers have a hard "no
                       LLVM, no AST dependency" rule.
```

`export immut` constants are populated during pass 1 by
`Resolver::hoistExportedConstant()` and consumed through
`VariableExprAST::ResolvedConstant` and `ScopeAccessExprAST::ResolvedConstant`
(set by `visitVariable()` and `visitScopeAccess()`). Codegen reads them directly
(`CodegenConstantValue()`, `CodegenCore.cpp`): an exported constant has no alloca
and no `GlobalVariable`. `VarDeclExprAST::codegen()` is an explicit no-op for one
(`IsExported`), because it is reached from top-level and module-body loops that
assume a valid insertion point.

### 2.6 Type identity is (module, name)

Every name-keyed type table is keyed by the **type identity**, one string per
(module, name) pair, built only by `TypeIdentity::make()` (`src/sema/Types.h`):
the Resolver's registry, `GlobalSymbolTable`, `LLVMTypeCache`, method and
constructor keys (`"<identity>.method"`), and monomorphized clone names.

| Declared in | Identity | LLVM / descriptor / DWARF name |
|---|---|---|
| module `json` | `json::Parser` | `json.Parser` |
| main program file (module `main`, never spelled) | `Parser` | `Parser` |
| generic clone | `list::List_gen1_int` | `list.List_gen1_int` |

`::` cannot occur in an identifier, so no identity can collide with another.
The identity is a plain string and not a (module, name) struct, which is what
lets every name-keyed table be module-aware without changing its key type. The
rule that makes this safe: **a spelling is never used as a key.** Two things
produce identities:

- **Declarations** are named by the Parser from the module being parsed
  (`CurrentParseModule()`, the top frame of `ImportStack`,
  `ParserModule.cpp`), so a `StructDeclExprAST`'s `getName()` *is* its identity
  from the moment it exists.
- **References** (`Node`, `m::Node`, `Node::new(...)`, `fn Node.method()`) are
  spellings until the Resolver resolves them, rewriting the AST in place
  (`NamedType::resolveTo()`, `GenericType::resolveBaseTo()`,
  `ScopeAccessExprAST::resolveLeftTypeTo()`, and a method's `FuncDefExprAST`
  name). The TypeChecker and Codegen only ever see identities.

A resolved reference is also *marked* resolved (`isResolvedIdentity()`,
`isBaseResolved()`, `isLeftTypeIdentity()`): a main-file type's identity is its
bare spelling, so text alone cannot say "already resolved", and a monomorphized
clone is re-resolved in its template's module, where a substituted main-file
`Node` must not be re-read as that module's own `Node` (2.8,
`resolveMonomorphizedClone()`).

`identitiesWithBareName()` answers "does this name exist anywhere". It is used
only to choose between the "unknown type" and "not visible here without
qualification" diagnostics, never to pick a type.

### 2.7 Resolver layer: pass 1 (hoisting)

Pass 1 runs in three steps (`Resolver::resolveAll()`, `ResolverCore.cpp`), in
this order because each needs the previous one complete:

1. **1a: `hoistTypes()`** (`ResolverTypeIdentity.cpp`): every
   `StructDeclExprAST`, `EnumDeclExprAST`, `MachineDeclExprAST` and
   `ErrorDeclExprAST` in the main file and in every module body is registered
   under its identity, with its visibility (`markTypeVisibility()`).
   `hoistTypesIn()` also calls `registerModule()` for each import, records
   `noteBareImport()` or `noteSymbolImport()` for it, and calls
   `registerOnDemandTypeImports()`, the type half of on-demand imports, so bare
   type visibility is complete before anything is resolved. A duplicate
   registration is E034.
2. **1b: `resolveDeclarationSpellings()`**: every declaration-level type spelling
   (struct field types, function and extern parameter and return types, an error
   variant's payload types, and a method's receiver, `fn Recv.m` becoming
   `<Recv's identity>.m`, found by visibility because a file may declare a method
   on an imported struct) is resolved in place through
   `resolveTypeSpellingsSilently()`. It is *silent*: an unresolvable spelling is
   left for pass 2 to report, because a pass 1 error aborts the whole
   `resolveAll()`. It must precede 1c, because a captured signature compares
   types by name, and precede `checkStructLayoutAcyclicity()`, whose edges are
   field types' names. `tryResolveTypeSpelling()` is the shared spelling-to-identity
   step, and it deliberately leaves a private type or an un-imported module
   unresolved so pass 2 can report it with a location and hints.
3. **1c: `hoistTopLevel()`** (main file) and **`hoistModuleBody()`** (recursive)
   walk top-level declarations only (no function bodies) and register
   `FuncDefExprAST` and `ExternFuncDeclExprAST` in `Modules_[ModuleName]` (module
   context) or `CurrentFileFunctions_` (main file, `ModuleName.empty()`), through
   `registerFunctionSignature()`; `export immut` constants through
   `hoistExportedConstant()`; and a nested `ModuleImportExprAST` by recursing.

`hoistModuleBody()` calls `SymTable_.registerModule(ModuleName)` before iterating
the body. This makes a module's *existence* independent of how many functions it
has: a module that declares only structs and enums must still make `hasModule()`
true, or every `Module::Type` reference into it would be rejected. `hasModule()`
is a plain `Modules_.count(...)`.

Right after each `ModuleImportExprAST`'s body is hoisted,
**`registerOnDemandImports()`** (`ResolverCore.cpp`) runs against the
now-hoisted target module. For `Wildcard`, every `pub` function, type and
constant name the module has is flat-registered. For `Symbol` and `SymbolList`,
each requested name must resolve to something `SourceModule` owns: a function
(`findInModules()` filtered to `SourceModule`), a type (`SourceModule::Name` is a
registered identity, which is **ownership**, not mere existence anywhere), or an
`export immut` constant. A private function or type is skipped by `[*]`; when
requested by name it is `E088` (`raisePrivateFunction()`, `raisePrivateType()`).
A name found nowhere is `UnknownModuleSymbolError` with suggestions drawn from
all three name kinds. Flat registration colliding with a different module's
entry is `ImportSymbolCollisionError`. The `ContextLabel` is `""` for a main-file
import, or the **enclosing** module's name for an on-demand import written inside
another module's file (the `ModuleName` parameter of the outer
`hoistModuleBody()`, not the module being imported).

#### 2.7.1 Bare type visibility

A bare type spelling can mean what `ResolverSymbolTable::unqualifiedTypeCandidates(ContextLabel, Name)`
returns: `ContextLabel::Name` if that is a registered identity (the context's
own declaration), plus `SourceModule::Name` if `FlatImportedTypes_[ContextLabel]`
maps `Name` to `SourceModule` (an on-demand import). One candidate resolves; two
is `AmbiguousTypeReferenceError`; none is "not visible" (if
`identitiesWithBareName(Name)` is non-empty) or "unknown".
`isTypeVisibleUnqualified(ContextLabel, Identity)` asks the same question of an
already-resolved identity (used by the `Name::new(...)` visibility gate).
`isTypeVisibleFrom(Identity, FromModule)` is the separate `pub` question: the
module owns the type, or the type is `pub`.

Qualified access is never gated by bare visibility: it goes through
`validateModuleQualifier()` (2.8), which asks only whether the qualifier is right
(and imported, and the type `pub`). Type parameters (`T` in `fn<T> f(x: T)`) are
exempt from the existence check.

### 2.8 Resolver layer: pass 2 (the full AST walk)

**`visitFuncCall()`** (`ResolverCall.cpp`) resolves the arguments first, then
branches on the callee's shape.

*Bare callee* (`VariableExprAST`). A variable of function type with that name is a
call through a function value (resolved as an ordinary variable reference;
Codegen emits an indirect call); a variable of any other type with that name is
an error. Otherwise the candidates are collected from, in order:

- the main file's own functions (`CurrentFileFunctions_`), which a module body
  never sees (a module cannot see its importer);
- self-reference: `findInModules()` filtered to `currentModuleName()` (empty
  string, the main file, never matches);
- on-demand imports: `lookupFlatFunctionImport(currentModuleName(), CalleeName)`,
  passed through `filterToVisibleOverloads()` so only `pub` overloads take part;
- only if all of those found nothing, the prelude tier: a public function of a
  module for which `IsPreludeBareModule()` is true.

A call to `main` is rejected (its signature is the C entry point's).

*`ScopeAccessExprAST` callee* (`Left::Right(args)`). First the left name is turned
into a type identity if it names a type (`resolveScopeLeftType()`), then it
dispatches on what `LeftName` is, in order:

1. A known **enum**: `TypeError` (variants are not callable).
2. A known **error type**: `ErrorType::Variant(args)` constructs an error value
   (`ErrorVariantConstruct`; arity is checked here, argument types by the
   TypeChecker); a nonexistent variant is `NoSuchVariantError`.
3. A known **struct**: `new` and `default` are reserved and dispatched first
   (2.8.2); everything else is `StaticMethodRef` (2.8.1).
4. A known **module**: `ModuleFunctionRef`. `ModuleMatches` comes from
   `findInModules()` filtered to the explicit `LeftName`, after
   `requireModuleImported()` (E145) and the visibility filter. A module that is
   real but has no such function is `ModuleMethodNotFoundError`, with suggestions
   ranked from `getModuleFunctionNames(LeftName)`. A module name qualified by
   another module name (`mem::io::printline(...)`) is a `TypeError`.
5. A declared **variable** (an instance): `ScopeOperatorOnInstanceError`, the more
   specific "you meant `.`" diagnosis.
6. Otherwise `UnknownScopeQualifierError`.

The branches share the downstream arity-check and `ResolvedCandidates`
population. The shape-specific branching decides only where
`ModuleMatches`/`CurrentFileMatches` come from.

**`visitScopeAccess()`** handles a `ScopeAccessExprAST` reached as an ordinary
expression, not as a call's callee. It resolves the left type, then: an enum
variant (validating the variant, `COUNT`, `VALUES`, `NAMES`, and the
`ModuleQualifier` of a 3-part chain); a bare error-type variant
(`ErrorVariantRef`; a variant that declares payload fields cannot be a value
without them); a module constant (`ModuleConstantRef`, after
`requireModuleImported()`); a module or struct member used without a call, which
is a `TypeError` ("no first-class functions"); `ScopeOperatorOnInstanceError` if
the left name is a declared variable; and `UnknownScopeQualifierError` as the
final fallback.

**`validateNamedType()`** (`ResolverDecl.cpp`) runs for every type annotation
(parameters, return types, fields, variable declarations). It unwraps one pointer
level and array dimensions, recurses into function types and generic arguments,
and validates a `GenericType`'s base name as well as its arguments. For each
named type it turns the spelling into an identity or reports why it cannot:

- *qualified*: the module exists and owns the identity, `requireModuleImported()`
  (E145), then the `pub` check (E088); otherwise `UnknownTypeError` if the name
  exists nowhere, or `validateModuleQualifier()`'s diagnosis if it exists
  elsewhere;
- *bare*: `unqualifiedTypeCandidates()` gives one (resolved), several
  (`AmbiguousTypeReferenceError`), or none (`UnqualifiedTypeNotVisibleError` if
  the name exists in another module, else `UnknownTypeError`).

Whatever it checks it also rewrites to the identity, and a trailing step resolves
the positions it does not descend into, so no spelling survives pass 2
unresolved because its position was never validated. A bare generic struct name
without type arguments is `TypeError` ("Missing type arguments!"), except for
`self`.

**`validateModuleQualifier()`** is the shared helper for the qualified forms:

```cpp
bool Resolver::validateModuleQualifier(const std::string &Qualifier,
                                       const std::string &TypeName,
                                       SourceLocation Loc) const {
  if (Qualifier.empty()) return true;                       // nothing to check
  if (!SymTable_.hasModule(Qualifier)) { /* E031 */ return false; }
  if (!requireModuleImported(Qualifier, currentModuleName(), Loc, ...)) return false;   // E145
  if (!SymTable_.isKnownTypeIdentity(TypeIdentity::make(Qualifier, Bare))) { /* E030 */ return false; }
  if (!SymTable_.isTypeVisibleFrom(Id, currentModuleName())) { raisePrivateType(...); return false; }  // E088
  return true;
}
```

**`visitModuleImport()`** pushes and pops `ModuleStack_`, the backing store for
`currentModuleName()`: the single source of truth for "which module's body is
pass 2 walking". Both the self-reference check and every
`registerOnDemandImports()` call's `ContextLabel` use it.
`resolveMonomorphizedClone()` pushes the template's module for the duration of a
clone, with `SuppressUnqualifiedVisibilityCheck_` set. A clone's substituted type
arguments can name a type from any module the original call site could see (a
`List<JsonValue>` clone declared in `list.tr` references `JsonValue`, which
`list.tr` never imports), so those already-resolved values are trusted and not
re-validated in the wrong scope.

**The `pub` helpers** live in `ResolverVisibility.cpp`: `mayQualifyWith()` and
`requireModuleImported()` (the E145 gate; prelude modules always pass),
`raisePrivateFunction()`, `raisePrivateType()`, `raisePrivateMethod()` (one place
so the E088 wording and hints are identical however the private name was
reached), `checkNoPrivateInPublic()` (E089, for a `pub` function, method, extern
or field of a module body), and `filterToVisibleOverloads()`. The Resolver checks
what it can name without a receiver's type (`T::new`, `T::method`, qualified
names, imports); the TypeChecker checks the rest (`p.field`, `p.method()`, UFCS)
in `TypeCheckerVisibility.cpp` against the module the code was *written* in
(`TypeChecker::CurrentModule_`). For a monomorphized clone that is its template's
module, so `pub` generics keep access to their own private fields.

#### 2.8.1 True static struct methods

`fn StructName.method(...) static: RetType { ... }` declares a method with no
implicit `self`, callable only as `StructName::method(args)`.

*Declaration.* `ParseFuncDef()` (`ParserDecl.cpp`) handles every method
declaration, static or not, in the single `.` branch: the function name becomes
`StructName.method` (the same flat name for static and instance methods of one
struct). A trailing `static`, consumed right after the parameter list and before
the return-type `:`, is what skips injecting `self: StructName*` as parameter 0.
There is no `::` in a `fn` declaration: `::` is reserved for call and reference
syntax. Because the name is the same, overload resolution, mangling and
forward-reference tolerance work unchanged; a static and an instance method of one
name are just different-arity overloads. `FuncDefExprAST::IsStaticDecl` exists
only so `Resolver::visitFuncDef()` can reject `static` where "self absent" would
be ambiguous.

*Reserved names, enforced in `visitFuncDef()`.* `default` can never be declared by
hand, static or not (`ReservedConstructorNameError`): it is always synthesized.
`new` can never be `static` (`ConstructorCannotBeStaticError`): it constructs via
`self`. `static` on a function whose name has no `StructName.` prefix is
`StaticOutsideStructError`.

*Call resolution.* When `LeftName` is a known struct, `ResolvedKind` is
`StaticMethodRef`, and the lookup mirrors `ModuleFunctionRef`:
`getTypeOwningModule(LeftName)` gives the owning context (`""` is the main file),
then either `lookupCurrentFileFunctions("StructName.method")` or
`findInModules("StructName.method")` filtered to that module, passed through the
`pub` filter. A miss is `StructMethodNotFoundError`, the same class the instance
`x.method()` path raises. The call itself needs only that the type exists; it is
not gated by bare type visibility, unlike construction (2.8.2). A leading
`ModuleQualifier` in `Module::StructName::method(...)` is validated by
`validateModuleQualifier()`.

*Codegen needs nothing special.* `FuncCallExprAST::codegen()` does not
distinguish `ModuleFunctionRef` from `StaticMethodRef`: it evaluates `Args`
exactly as written and calls `ResolvedCallTarget->mangledName()`.

#### 2.8.2 Struct construction: `::new(...)` and `::default()`

Dispatched in `visitFuncCall()`'s known-struct branch, before the static-method
lookup (the reserved names cannot be shadowed, 2.8.1).

**`StructName::default()`** involves no user function. The Resolver sets
`ResolvedKind = DefaultConstruct` and rejects any argument
(`DefaultConstructorArityError`). The TypeChecker's `visitFuncCall()` handles it
at the top and types the node as the struct's `NamedType` directly, which keeps
it compatible with a `GenericType` of the same base name on the other side of an
assignment. Codegen allocates the struct, stores `GetDefaultValue()`'s recursive
zero-fill, and loads it back.

**`StructName::new(args...)`** is declared as an ordinary `.`-method named `new`
(self at `Params[0]`) but called with `self` omitted. The Resolver builds a
**self-stripped, return-type-overridden copy** of each matching `StructName.new`
overload (`Params.erase(begin())`, `ReturnType = NamedType(StructName)`, because
the declared return type is `void` and the call evaluates to the constructed
struct) and pushes the copies into `Node.ResolvedCandidates` like an ordinary
call. The TypeChecker's existing arity, type-match and generic-substitution loop
then handles overload resolution unmodified. No overload at all is
`NoConstructorDefinedError`; none matching the arity is `WrongArityError`.
Codegen reconstructs the real (self-included) mangled name from the stripped
target by inserting `"_" + StructName + "_ptr"` after the base name, mirroring
`ResolverFunctionSymbol::mangledName()` with self's type reinserted. It then
allocates the struct as `self`, calls the function, and loads the result.

**Bare visibility is gated** here, unlike a static method call: unqualified
`StructName::new/default()` goes through `isTypeVisibleUnqualified()`, so
constructing a struct is treated like naming its type.

**Definite field assignment.** `new()` must assign every field on every path.
That is `ConstructorFieldAssignmentPass` (`Analysis.cpp`), a forward dataflow
analysis with the shape of `DefiniteAssignmentPass`, run by
`runFunctionFlowPasses()` when the function's name ends in `.new`
(`compiler_pipeline.md` section 6). It raises `IncompleteConstructorError`.

#### 2.8.3 `.` on a module or a bare type name is rejected

`Resolver::visitMethodCall()` handles the whole `Left.method(...)` path. It
resolves the arguments first, so a broken argument is still reported when the
base is about to be rejected. It records a `self.m()` call as an edge for the
transitive "mutates self" closure. Then it rejects `.new(...)` and `.default(...)`
on **any** base (`ConstructorCalledOnInstanceError`), before the base dispatch,
because `new` is registered as an ordinary `.`-declared instance method and a real
instance calling it would otherwise resolve normally. Then, in priority order:

1. A declared variable: resolve it as an instance. Shadowing wins.
2. A known **module** with no shadowing variable:
   `DotOperatorOnNonInstanceError("module")`.
3. A name that is a **struct** type in any module (`identitiesWithBareName()`)
   with no shadowing variable: `DotOperatorOnNonInstanceError("struct type")`.
4. Neither: resolve normally, so `visitVariable()` reports the ordinary
   undeclared-name error.

`VariableExprAST::ResolvedModuleName` and `IsStructName` still exist, with
readers in the TypeChecker and Codegen, but nothing sets them in a real compile:
`visitMethodCall()` always raises first. They are leftover defense in depth, not
a live path.

### 2.9 TypeChecker layer

`TypeChecker::visitScopeAccess()` assigns a type when `ResolvedKind` is an enum
variant, an error-type variant, or a module constant. A module-function-ref
callee is visited but gets no standalone type: a qualified call's own type comes
from `ResolvedCandidates`/`ResolvedCallTarget`, computed by `visitFuncCall()`
independently of the callee node's shape. The TypeChecker also owns the
visibility checks that need a receiver's type (2.8) in `TypeCheckerVisibility.cpp`,
and `TypeChecker::CurrentModule_` is the module a piece of code was written in.

### 2.10 Codegen layer

**Name mangling.** A module function's LLVM name is
`turf_<module>_<name>_<paramtype>_...`, whatever the import mode or number of
importers: Codegen compiles it once, under its owning module. Only the
Resolver's source lookup depends on how the module was imported. A parameter
type's mangled component and a method's name carry the type's identity verbatim
(`turf_json_json::Parser.at_json::Parser_ptr`); `::` is legal in an
LLVM/ELF symbol and cannot occur in a spelling, so it cannot collide.

**Type names.** An LLVM struct, its GC type descriptor
(`__turf_typedesc_<name>`, and the name string the runtime sees) and its DWARF
type are all named `TypeIdentity::llvmName(identity)`: `json.Parser`,
`list.List_gen1_int`. A main-file type keeps its bare name. `LLVMTypeCache` is
keyed by the identity, never by the LLVM name.

**Struct codegen is idempotent.** `StructDeclExprAST::codegen()` can be reached
more than once for a struct declared inside a module, because the pre-pass calls
`ModuleImportExprAST::codegen()` twice (prototype pass, body pass) and that
re-walks the whole module body. A call that finds the cached `llvm::StructType`
already non-opaque returns at once, instead of creating a second, renamed
`Name.0` type and corrupting `LLVMTypeCache` for every reference already built.

**`FuncCallExprAST::codegen()`** derives `CalleeName` from a `VariableExprAST` or
a `ScopeAccessExprAST` callee. Everything downstream (the `ResolvedCallTarget`
lookup, argument marshaling) is callee-shape-agnostic. **`ScopeAccessExprAST::codegen()`**
is reached only for an enum variant (and an error-variant or module constant
reference): a module function reference in call position never calls
`Callee->codegen()`, since it has no value, only a name lookup, and the Resolver
rejects any non-call use of a module reference before Codegen.

**`getExprType()`** (`CodegenCore.cpp`) is Codegen's own type inference,
independent of the TypeChecker's (`compiler_pipeline.md` section 4), and has to
understand module-qualified calls too: it accepts a `ScopeAccessExprAST` callee
and recovers the module prefix from `ResolvedCallTarget->SourceModule` while
keeping the call-site-argument mangling that generic instantiation needs.
Using `ResolvedCallTarget->mangledName()` wholesale does not work, because that
reflects the function's declared signature and not the call site's instantiation.

### 2.11 Diagnostics and SLM routing

The module and import error classes (`src/diagnostics/Errors*.h`, notably
`ErrorsModule.h` and `ErrorsScope.h`) use the existing
`SLM::ErrorCategory::DECLARATION` category. `extractContextKeyword()`
(`src/diagnostics/ErrorCategory.h`) has a `"module"`/`"import"` substring check
that routes to `docs/slm/modules.txt` through the `constructDocs` map. It runs
**after** the struct/enum check: `DuplicateTypeNameError`'s note contains the
word "module" ("another file or module may reuse it"), and checking `"module"`
first would route it away from `structs.txt`.

---

## Part 3: End-to-end walkthrough, `import io::printline;` then `printline("hi");`

1. **Parse** `import io::printline;`. `ParseImport()` reads `io`, sees `::`, sees a
   bare identifier (not `[`): `Mode = Symbol`, `RequestedSymbols =
   [{"printline", loc}]`. `ParseModuleImportByName()` finds `lib/std/core/io.tr`
   through the stdlib index (2.4), parses all of it into `Body`, and records
   `ResolvedPath`. It returns a `ModuleImportExprAST`.
2. **Parse** `printline("hi");`: an ordinary `FuncCallExprAST` with a
   `VariableExprAST("printline")` callee. Nothing module-specific happens.
3. **Pass 1a**: `hoistTypesIn()` registers `io`, records the symbol import (which
   grants no `io::`), and registers `io`'s types.
4. **Pass 1c**: `hoistModuleBody(io.tr's Body, "io")` registers its functions into
   `Modules_["io"]`. Immediately after, `registerOnDemandImports()` runs with
   `Mode = Symbol`, looks `"printline"` up with `findInModules()` filtered to
   `"io"`, finds the `pub` overloads, and calls
   `registerFlatFunctionImport("", "printline", {SourceModule: "io", ...})`. The
   context is `""` because this `import` is in the main file.
5. **Pass 2**, visiting the call: `visitFuncCall()` takes the bare-callee branch.
   `CurrentFileMatches` is empty. Self-reference contributes nothing
   (`currentModuleName()` is `""`). The on-demand lookup
   `lookupFlatFunctionImport("", "printline")` finds the entry from step 4 and
   pulls the overloads through `filterToVisibleOverloads()`. `ResolvedCandidates`
   is populated, and the TypeChecker later narrows it to the `string` overload by
   argument type.
6. **Codegen**: `FuncCallExprAST::codegen()` reads
   `ResolvedCallTarget->mangledName()` (`turf_io_printline_string`) and calls the
   function, already compiled when `io.tr`'s own `ModuleImportExprAST::codegen()`
   ran.

Contrast with `parse_number("1")` in a file that only has `import json;`. The
on-demand lookup misses (`json` was imported bare, which grants only `json::`),
self-reference misses (not inside a module), and `json` is not in the prelude
tier, so the candidates stay empty. The call falls to the not-found branch, which
builds `RankedSuggestions` including an exact-name search across all modules *for
suggestions only* (`findInModules(name)`, unrestricted, unlike the restricted
lookup used for resolution) and raises `UnknownFunctionError` (E041) with the
`json::` spelling offered as a hint.

---

## Part 4: File map

| File | Role |
|---|---|
| `src/frontend/Lexer.h`, `src/frontend/Lexer.cpp` | the `TOK_SCOPE` token |
| `src/frontend/ASTAccess.h` | `ScopeAccessExprAST`, `ScopeAccessKind` |
| `src/frontend/ASTDecl.h` | `ModuleImportExprAST`, `ImportMode`, `ImportedSymbolRef`, `FuncDefExprAST::IsStaticDecl` |
| `src/sema/Types.h` | `NamedType`/`GenericType`'s `Module` field, `TypeIdentity` |
| `src/frontend/ParserModule.cpp` | `ParseImport()`, `ParseModuleImportByName()`, `ParseFile()` and `ImportStack`, `CollectModuleCandidates()`, `ParsePreludeImports()`, `ClaimEntryFileImportSlot()`, `ModuleKeyFor()`, the prelude lists |
| `src/frontend/ParserCore.cpp` | `ParseType()`'s qualifier handling, the `TOK_SCOPE` branch of primary-expression parsing |
| `src/frontend/ParserDecl.cpp` | `ParseFuncDef()`'s `.` declaration and `static` handling, `ParseExportDecl()` |
| `src/tooling/PackageGraph.h`, `src/tooling/PackageGraph.cpp` | package graph, package-aware candidates |
| `src/resolve/Resolver.h`, `ResolverCore.cpp` | `resolveAll()`, `hoistTopLevel()`, `hoistModuleBody()`, `registerOnDemandImports()`, `hoistExportedConstant()`, `buildFunctionSymbol()` |
| `src/resolve/ResolverTypeIdentity.cpp` | `hoistTypes()`, `resolveDeclarationSpellings()`, `tryResolveTypeSpelling()`, `resolveScopeLeftType()` |
| `src/resolve/ResolverCall.cpp` | `visitFuncCall()` (incl. `new`/`default`), `visitMethodCall()`, `visitScopeAccess()` |
| `src/resolve/ResolverDecl.cpp` | `visitFuncDef()` (reserved names), `validateNamedType()`, `validateModuleQualifier()`, `visitModuleImport()` |
| `src/resolve/ResolverVisibility.cpp` | the `pub` and E145 helpers |
| `src/resolve/ResolverSymbolTable.h`, `ResolverModuleTable.h`, `ResolverTypeRegistry.h` | `Modules_`, `BareImports_`, `CurrentFileFunctions_`, the flat-import maps, `RegisteredStructs_`/`RegisteredEnums_`/`RegisteredErrorTypes_`, `TypeOwningModule_`, `isTypeVisibleUnqualified()`, `isTypeVisibleFrom()`, `ResolverConstantSymbol` |
| `src/sema/TypeChecker.h`, `TypeCheckerCall.cpp`, `TypeCheckerVisibility.cpp` | `visitScopeAccess()`, `visitMethodCall()`, `visitFuncCall()` (`DefaultConstruct` branch), member visibility |
| `src/codegen/CodegenCore.cpp`, `CodegenFunc.cpp`, `CodegenStruct.cpp`, `CodegenExpr.cpp`, `CodegenVar.cpp` | `FuncCallExprAST::codegen()`, `ScopeAccessExprAST::codegen()`, `getExprType()`, `CodegenConstantValue()`, `ModuleImportExprAST::codegen()`, `StructDeclExprAST::codegen()`, `VarDeclExprAST::codegen()`'s `IsExported` no-op |
| `src/analysis/Analysis.h`, `Analysis.cpp` | `ConstructorFieldAssignmentPass` |
| `src/diagnostics/ErrorsModule.h`, `ErrorsScope.h`, `ErrorsStruct.h`, `Errors.h`, `ErrorCodes.h` | the error classes and codes of 1.8 |
| `src/diagnostics/ErrorCategory.h` | `"module"`/`"import"` routing to `docs/slm/modules.txt` |
| `lib/std/**` | the standard library modules, ordinary `.tr` files with no compiler special-casing |
