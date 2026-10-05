#ifndef ERRORS_MODULE_H
#define ERRORS_MODULE_H

#include "Errors.h"
#include <algorithm>

// LEC-001 stage 4: how a module identity reads in a message. A dependency
// package's modules have identities `<package>/<path>` (ParserModule.cpp,
// ModuleCandidate::Key); the package's ROOT module, `geometry/geometry`, is
// what the user imports as `geometry`, so it is shown that way. An internal
// module keeps its `<package>/` prefix -- "util" alone would be ambiguous
// with the importer's own util, which is the very collision identities exist
// to separate. Every other identity has no '/' and is shown as is.
inline std::string DisplayModuleName(const std::string &Identity) {
  size_t Slash = Identity.find('/');
  if (Slash == std::string::npos)
    return Identity;
  if (Identity.compare(Slash + 1, std::string::npos, Identity, 0, Slash) == 0)
    return Identity.substr(0, Slash);
  return Identity;
}

// The file a "mark it `pub` in <file>" hint names: the declaration's own file
// when its location is known. `<module>.tr` was right while a module's name
// was always its file stem, and is wrong for a dependency's `geometry/util`.
inline std::string DeclaringFileName(const SourceLocation &DeclLoc,
                                     const std::string &Module) {
  if (DeclLoc.Filename.empty())
    return DisplayModuleName(Module) + ".tr";
  size_t Sep = DeclLoc.Filename.find_last_of("/\\");
  return Sep == std::string::npos ? DeclLoc.Filename : DeclLoc.Filename.substr(Sep + 1);
}

// `export` used inside a function body -- only legal at the top level of a
// file or module (see VarDeclExprAST::IsExported's doc comment).
class ExportOutsideTopLevelError : public TurfError {
public:
  ExportOutsideTopLevelError(SourceLocation Loc, const std::string &Name)
      : TurfError(Loc, Colors::BRIGHT_RED +
                           "'export' can't be used inside a function: '" +
                           Colors::CYAN + Name + Colors::BRIGHT_RED + "'.",
                  "Move this declaration to the top level of the file "
                  "(outside any function) if it needs to be exported. "
                  "A regular 'mut " + Name + "' or 'immut " + Name +
                  "' declaration (no 'export') is fine right here if it "
                  "doesn't need to be shared with other files.") {
    Category_ = SLM::ErrorCategory::DECLARATION;
  }
};
// `export mut` -- true mutable shared globals aren't supported yet; only
// `export immut` (compile-time constants) are.
class ExportMutNotSupportedError : public TurfError {
public:
  ExportMutNotSupportedError(SourceLocation Loc, const std::string &Name)
      : TurfError(Loc, Colors::BRIGHT_RED +
                           "'export mut' isn't supported yet: '" +
                           Colors::CYAN + Name + Colors::BRIGHT_RED + "'.",
                  "Only 'export immut' (a shared compile-time constant) "
                  "is supported right now. Use 'export immut " + Name +
                  ": <type> = <value>;' instead.") {
    Category_ = SLM::ErrorCategory::DECLARATION;
  }
};
// `export immut` whose initializer isn't a literal the compiler can fold at
// compile time -- exported constants have no real storage (see
// ResolverConstantSymbol's doc comment), so the value must be known now.
class ExportNonConstantInitializerError : public TurfError {
public:
  ExportNonConstantInitializerError(SourceLocation Loc, const std::string &Name)
      : TurfError(Loc, Colors::BRIGHT_RED +
                           "'export immut " + Colors::CYAN + Name +
                           Colors::BRIGHT_RED +
                           "' needs a literal value I can compute right now.",
                  "Exported constants must be initialised with a plain "
                  "literal (a number, string, or bool), like "
                  "'export immut " + Name + ": int = 42;' -- not an "
                  "expression or a call to another function.") {
    Category_ = SLM::ErrorCategory::DECLARATION;
  }
};
// `operator +(int, int)`-shaped declarations -- both operand types are
// already in the compiler's builtin set (int/double/bool/char/Pointer), so
// there's nothing for a user overload to add; allowing it would also make
// builtin arithmetic itself ambiguous with the user's own definition. See
// STRING_UFCS_OPERATOR_CHAR_RFC.md §4.3's "builtin-pair" hard-error rule --
// this is a compile-time error at the DECLARATION site, not merely
// unreachable due to resolution order.
class OperatorBuiltinPairError : public TurfError {
public:
  OperatorBuiltinPairError(SourceLocation Loc, const std::string &OpSpelling,
                           const std::string &LhsType,
                           const std::string &RhsType)
      : TurfError(Loc, Colors::BRIGHT_RED + "'operator " + OpSpelling +
                           "(" + LhsType + ", " + RhsType +
                           ")' is not allowed." + Colors::RESET,
                  "Both operand types are already built in -- overloading "
                  "an operator between two builtin types would make "
                  "ordinary arithmetic ambiguous. Operator overloads only "
                  "make sense when at least one operand is a struct or "
                  "'string'.") {
    Category_ = SLM::ErrorCategory::DECLARATION;
  }
};
// LANG-001's orphan rule (ARCHITECTURE_OVERHAUL.md §13.4): at least one
// operand of a declared operator must be a struct/enum declared in the SAME
// module as the operator itself. Without this, module C could declare
// `operator +(a: ModuleA::Foo, b: ModuleB::Bar)` -- redefining what `+`
// means between two types it doesn't own -- and the result would silently
// depend on which modules happen to be imported, in what order. A builtin/
// `string` operand (never itself "declared" anywhere) doesn't count toward
// satisfying the rule, but also can't violate it by itself -- see
// Resolver::validateOperatorDecl()'s own comment for the exact reasoning.
class OperatorOrphanRuleError : public TurfError {
public:
  OperatorOrphanRuleError(SourceLocation Loc, const std::string &OpSpelling,
                          const std::string &DeclaringModule,
                          const std::string &OperandTypeName,
                          const std::string &OperandOwningModule)
      : TurfError(Loc, Colors::BRIGHT_RED + "'operator " + OpSpelling +
                           "' does not own any of its operand types." +
                           Colors::RESET,
                  "'" + OperandTypeName + "' is declared in " +
                      (OperandOwningModule.empty() ? "the main file"
                                                   : "module '" + OperandOwningModule + "'") +
                      ", not in " +
                      (DeclaringModule.empty() ? "the main file" : "module '" + DeclaringModule + "'") +
                      " -- at least one operand of an operator declaration "
                      "must be a struct or enum declared in the same "
                      "module/file as the operator itself, so an operator's "
                      "meaning never depends on import order.") {
    Category_ = SLM::ErrorCategory::DECLARATION;
  }
};
// LEC-006: a generic `operator<T> ...` declaration that cannot work as
// written -- a type parameter no operand mentions (it could never be
// inferred at a use), or no operand that is a type the declaring module
// owns (a bare `T` operand would match types from every module, so the
// orphan rule cannot be satisfied vacuously for a generic operator the way
// it is for a concrete all-builtin one).
class GenericOperatorError : public TurfError {
public:
  GenericOperatorError(SourceLocation Loc, const std::string &Problem,
                       const std::string &Hint)
      : TurfError(Loc, Colors::BRIGHT_RED + Problem + Colors::RESET, Hint) {
    Category_ = SLM::ErrorCategory::DECLARATION;
  }
};
// LEC-006: a write whose destination is the BY-VALUE result of an
// `operator[]` read -- `bs[0].a = 50`, `outer[0].push(5)`, `mm["a"]["b"] = 1`
// with a by-reference `[]=`. The index operator returns a copy, so the write
// would change the copy and vanish. This is the rule C# calls CS1612 and Go
// applies to a struct held in a map; without it the compiler used to spill
// the copy and mutate that, silently losing the write.
class OperatorTemporaryWriteError : public TurfError {
public:
  OperatorTemporaryWriteError(SourceLocation Loc, const std::string &Problem)
      : TurfError(Loc, Colors::BRIGHT_RED + Problem + Colors::RESET,
                  "'x[i]' returns the element by value, so this would only change "
                  "a copy. Read the element into a variable, change that, and "
                  "store it back: `mut e: T = x[i]; e.field = v; x[i] = e;`. "
                  "Writing THROUGH a pointer or reference element, or into a "
                  "nested list (`grid[i][j] = v`), is fine.") {
    Category_ = SLM::ErrorCategory::TYPE_MISMATCH;
  }
};
// Two `extern fn` declarations for the same raw C name with genuinely
// different signatures (arity, parameter types, return type, or variadic-
// ness). Extern mangled names are just the bare C symbol name (not
// parameter-type-qualified, unlike ordinary Turf functions), so this can't
// be caught the way an ordinary duplicate-function-body collision already
// is (see FuncDefExprAST::codegen()'s "Duplicate function definition"
// check) -- ExternFuncDeclExprAST::codegen() used to silently reuse
// whichever declaration came first, regardless of whether the second one's
// signature actually matched, binding any call resolved against the SECOND
// declaration's (different) signature to the FIRST one's real LLVM type.
// A re-declaration with an IDENTICAL signature (the legitimate case: the
// same libc function externed from two different stdlib modules, e.g.
// strlen in both strings.tr and net.tr) is NOT an error -- only a genuine
// mismatch is.
class ConflictingExternDeclarationError : public TurfError {
public:
  ConflictingExternDeclarationError(SourceLocation Loc, const std::string &Name)
      : TurfError(Loc, Colors::BRIGHT_RED + "'extern fn " + Colors::CYAN +
                           Name + Colors::BRIGHT_RED +
                           "' conflicts with an earlier declaration of the "
                           "same name." + Colors::RESET,
                  "Two 'extern fn " + Name + "' declarations exist with "
                  "different signatures (parameter types, return type, or "
                  "variadic-ness). Only ONE C function can actually be "
                  "called through this name -- make both declarations "
                  "match exactly, or give one a different name.") {
    Category_ = SLM::ErrorCategory::DECLARATION;
  }
};
// Phase 3: Module search error
class ModuleNotFoundError : public TurfError {
public:
  ModuleNotFoundError(SourceLocation Loc, const std::string &ModuleName)
      : ModuleNotFoundError(Loc, ModuleName, "") {}

  // @p ExtraHint is appended to the standard hint. LEC-001 stage 5: a
  // single-file compile of a file that lives inside a turf.toml project fails
  // on an import of one of the project's dependencies, which only a package
  // build can see; the standard hint ("make sure the file exists in the
  // standard library or your current directory") would send the user looking
  // for a file that is not the problem. Keeping the base message unchanged
  // means every existing E027 expectation still holds.
  ModuleNotFoundError(SourceLocation Loc, const std::string &ModuleName,
                      const std::string &ExtraHint)
      : TurfError(Loc, Colors::BRIGHT_RED + "Could not find module '" +
                           Colors::CYAN + ModuleName + Colors::BRIGHT_RED +
                           "'." + Colors::RESET,
                  "Make sure the file '" + ModuleName + ".tr' exists in the standard library or your current directory." +
                      (ExtraHint.empty() ? "" : " " + ExtraHint)) {
    Category_ = SLM::ErrorCategory::DECLARATION;
  }

  /// A package build (LEC-001 stage 4): the only places `import m;` can
  /// resolve are the package's own src/, its direct dependencies and the
  /// stdlib. The single-file hint above names "your current directory", which
  /// a package build never searches, so it pointed the user at the one place
  /// a new file would still not be found. Same message and code; only the
  /// hint changes.
  static ModuleNotFoundError inPackage(SourceLocation Loc, const std::string &ModuleName,
                                       const std::string &Package) {
    ModuleNotFoundError E(Loc, ModuleName);
    E.Hint_ = "In a project, `import " + ModuleName + ";` names one of three "
              "things: a module of this package (a file '" + ModuleName +
              ".tr' anywhere under " + Package + "'s src/), a package listed "
              "under [dependencies] in its turf.toml, or a standard-library "
              "module. Check the spelling, create src/" + ModuleName +
              ".tr, or add the dependency (`turf add " + ModuleName +
              " --path <dir>`).";
    return E;
  }
};
// LEC-001 stage 3 (FREEZE.md 6.16, F-PKG-2): `import m;` names more than one
// file. The search roots used to be tried in order with the first hit winning,
// so a local json.tr beside main.tr was silently never loaded while the stdlib
// had a `json` (audit F-19) -- the program ran code its author did not write,
// and nothing said so. Every root is now examined and more than one distinct
// file is this error: never silent shadowing, never "local wins".
//
// The candidates go in the message body, one per line, rather than as
// withRelated() notes: a related note renders as "(line, col)" with no file
// name, and naming the files IS the diagnostic here.
struct ModuleCandidateView {
  std::string Kind; // "standard library", "next to main.tr", ...
  std::string Path; // for display
  bool IsStdlib = false;
  bool IsDependency = false; // a dependency's root module: its name is fixed by its package
};
class AmbiguousModuleError : public TurfError {
public:
  AmbiguousModuleError(SourceLocation Loc, const std::string &ModuleName,
                       const std::vector<ModuleCandidateView> &Candidates)
      : TurfError(Loc, "") {
    Category_ = SLM::ErrorCategory::DECLARATION;
    Message = Colors::BRIGHT_RED + "Module '" + Colors::CYAN + ModuleName +
              Colors::BRIGHT_RED + "' is ambiguous: " +
              std::to_string(Candidates.size()) +
              " different files answer to that name." + Colors::RESET;
    size_t Width = 0;
    for (const auto &C : Candidates)
      Width = std::max(Width, C.Kind.size());
    for (const auto &C : Candidates)
      Message += "\n    " + C.Kind + std::string(Width - C.Kind.size() + 2, ' ') +
                 C.Path;

    // The fix differs by what collided. Against the standard library the
    // user's file is the one that can move (a stdlib name is not theirs to
    // change); two user files are the user's to tell apart either way.
    std::vector<std::string> Own;
    bool HasStdlib = false, HasDependency = false;
    for (const auto &C : Candidates) {
      if (C.IsStdlib)
        HasStdlib = true;
      else if (C.IsDependency)
        HasDependency = true;
      else
        Own.push_back(C.Path);
    }
    std::string Files;
    for (size_t I = 0; I < Own.size(); ++I)
      Files += (I ? (I + 1 == Own.size() ? " and " : ", ") : "") + Own[I];
    if (HasStdlib && !Own.empty()) {
      Hint_ = "A module may not share its name with a standard-library "
              "module. Rename " + Files + " (for example to 'my_" +
              ModuleName + ".tr') and import it by the new name.";
    } else if (HasDependency && !Own.empty()) {
      // A dependency is imported by its package name, which the user
      // cannot change from here; their own module can move.
      Hint_ = "A module may not share its name with a dependency. Rename " +
              Files + " (for example to 'my_" + ModuleName +
              ".tr') and import it by the new name.";
    } else {
      Hint_ = "These are different files that both answer to '" + ModuleName +
              "'. Rename one of them, or delete the one you did not mean.";
    }
  }
};

// LEC-001 stage 4 (FREEZE.md 6.16, F-PKG-2): in a package build, `import m;`
// names a module that exists in the build but that this package may not
// import. Distinct from E027 "not found" on purpose: the file is there, a rule
// forbids it, and the fix is different (edit the manifest, or import the
// package's root module instead) -- "could not find module" would send the
// user looking for a file that already exists.
class ModuleNotImportableError : public TurfError {
  ModuleNotImportableError(SourceLocation Loc) : TurfError(Loc, "") {
    Category_ = SLM::ErrorCategory::DECLARATION;
  }

public:
  /// @p Package is in the build, but not a direct dependency of @p Importer.
  /// @p Requirers are the packages that do depend on it.
  static ModuleNotImportableError
  notADirectDependency(SourceLocation Loc, const std::string &Importer,
                       const std::string &Package,
                       const std::vector<std::string> &Requirers) {
    ModuleNotImportableError E(Loc);
    std::string By;
    for (size_t I = 0; I < Requirers.size(); ++I)
      By += (I ? ", '" : "'") + Requirers[I] + "'";
    E.Message = Colors::BRIGHT_RED + "Package '" + Colors::CYAN + Package +
                Colors::BRIGHT_RED + "' is not a dependency of '" + Importer +
                "'." + Colors::RESET;
    if (!By.empty())
      E.Message += "\n    It is in this build because " + By +
                   (Requirers.size() == 1 ? " depends" : " depend") + " on it.";
    E.Hint_ = "A package can import only its own modules, its direct "
              "dependencies and the standard library. Add '" + Package +
              "' to [dependencies] in " + Importer +
              "'s turf.toml to import it here.";
    return E;
  }

  /// @p Name is an internal (non-root) module of @p Owner, at @p Path.
  static ModuleNotImportableError
  internalModule(SourceLocation Loc, const std::string &Name,
                 const std::string &Importer, const std::string &Owner,
                 const std::string &Path) {
    ModuleNotImportableError E(Loc);
    E.Message = Colors::BRIGHT_RED + "Module '" + Colors::CYAN + Name +
                Colors::BRIGHT_RED + "' is internal to package '" + Owner +
                "'." + Colors::RESET + "\n    " + Path;
    E.Hint_ = "Only a package's root module can be imported from outside it. "
              "Import '" + Owner + "' and use what its root module makes "
              "`pub`" + (Importer == Owner ? "" : "; if '" + Owner +
              "' should offer this, its root module has to expose it") + ".";
    return E;
  }
};

// A single hop in a detected import cycle: which module, and the location
// of the `import` statement that pulled it onto the in-progress stack.
struct ImportChainEntry {
  std::string ModuleName;
  SourceLocation Loc;
};

// Raised when ParseFile() re-enters a module that is still being parsed
// (present on the Parser's in-progress import stack) -- a genuine cycle
// (A imports B imports ... imports A), not the ordinary "already fully
// parsed, reuse it" case (which returns an empty body silently and is by
// design, see ParseFile()'s ImportedFiles set). Before this class existed, a
// cycle didn't error or hang -- it silently truncated the re-imported side
// to an empty module body, which is a confusing, hard-to-diagnose class of
// bug all on its own (missing symbols with no explanation), not just a
// missing diagnostic.
class CircularImportError : public TurfError {
public:
  CircularImportError(SourceLocation Loc, const std::string &ModuleName,
                      const std::vector<ImportChainEntry> &Chain)
      : TurfError(Loc, Colors::BRIGHT_RED +
                           "Circular import detected -- these modules are "
                           "waiting on each other." + Colors::RESET) {
    Category_ = SLM::ErrorCategory::DECLARATION;

    // Render as a simple linear chain (A --imports--> B --imports--> ...
    // --imports--> A) rather than true box-drawing: a 2-3 module cycle is
    // the overwhelmingly common case and reads perfectly clearly this way,
    // and unlike a box diagram this degrades gracefully to any chain length
    // without needing terminal-width-aware layout logic.
    std::string Diagram = "\n\n    ";
    for (const auto &Entry : Chain) {
      Diagram += Colors::CYAN + Entry.ModuleName + Colors::RESET +
                 Colors::BRIGHT_BLACK + "  --imports-->  " + Colors::RESET;
    }
    Diagram += Colors::CYAN + ModuleName + Colors::RESET + Colors::BRIGHT_RED +
               "  <- back to the start!" + Colors::RESET;
    Message += Diagram;

    Hint_ = "Break the cycle by removing one of these imports, or moving "
            "the code both sides actually need into a third module that "
            "neither one has to import the other to reach.";

    for (const auto &Entry : Chain)
      RelatedLocations_.push_back({Entry.Loc, "imports '" + Entry.ModuleName + "' here"});
  }
};
// Raised by Resolver::visitScopeAccess/visitFuncCall when the left-hand
// name of a `Left::Right` expression isn't a known enum OR a known module
// -- the two things `::` can currently name. Mirrors ModuleNotFoundError's
// category choice (DECLARATION): this is fundamentally the same shape of
// problem (an unresolved name), just for the newer, more general `::`
// operator rather than specifically `import`.
class UnknownScopeQualifierError : public TurfError {
public:
  UnknownScopeQualifierError(SourceLocation Loc, const std::string &LeftName,
                             const RankedSuggestions &Suggestions)
      : TurfError(Loc, Colors::BRIGHT_RED + "'" + Colors::CYAN + LeftName +
                           Colors::BRIGHT_RED +
                           "' isn't a known enum, struct, or module." + Colors::RESET) {
    Category_ = SLM::ErrorCategory::DECLARATION;
    Hint_ = suggestionsOrFallback(
        Suggestions,
        "'::' only works after an enum name (Color::Red), an imported "
        "module name (io::printline), or a struct type name for a static "
        "method (StructName::method). Double-check the spelling, or make "
        "sure you've imported the module.");
    trySetDeterministicRename(LeftName, Suggestions);
  }
};
// Raised when '::' -- reserved for namespace-resolution (enum/module/
// struct-type qualifiers) -- is used with a base that names a real
// declared VARIABLE (an instance) instead. The natural counterpart to
// DotOperatorOnNonInstanceError ('.' misused on a non-instance): this is
// '::' misused ON an instance. Deliberately a separate, more specific
// error than the generic UnknownScopeQualifierError fallback -- "isn't a
// known enum, struct, or module" is technically true but unhelpful here,
// since the real, nameable mistake is "you have an instance; you meant
// '.', not '::'".
class ScopeOperatorOnInstanceError : public TurfError {
public:
  ScopeOperatorOnInstanceError(SourceLocation Loc, const std::string &Name,
                               const std::string &RightName)
      : TurfError(Loc, Colors::BRIGHT_RED + "'" + Colors::CYAN + Name +
                           Colors::BRIGHT_RED +
                           "' is a variable, not a type or module -- '::' "
                           "doesn't apply to it." + Colors::RESET) {
    Category_ = SLM::ErrorCategory::DECLARATION;
    Hint_ = "'::' is for namespace-qualified access (enum variants, module "
            "functions, static methods) -- '" + Name + "' holds an "
            "instance. Did you mean '" + Name + "." + RightName + "(...)'?";
    // `Name` is a declared variable here (both raise sites check that), so
    // `Name::member` can only have meant `Name.member`; the sandbox still has
    // to agree that member exists on it.
    withDeterministicFix(Name + "::" + RightName, Name + "." + RightName,
                         "'" + Name + "' is a variable holding an instance, "
                         "and a member of an instance is reached with '.', "
                         "not '::'.");
  }
};
// Raised when `import module::symbol;` / `module::[a, b]` / `module::[*]`
// names a symbol that doesn't actually exist in that module (as either a
// function or a struct/enum type) -- the on-demand counterpart to
// ModuleNotFoundError (which is about the MODULE not existing; this is
// about the module existing but not having what was asked for).
class UnknownModuleSymbolError : public TurfError {
public:
  UnknownModuleSymbolError(SourceLocation Loc, const std::string &ModuleName,
                           const std::string &SymbolName,
                           const RankedSuggestions &Suggestions)
      : TurfError(Loc, Colors::BRIGHT_RED + "Module '" + Colors::CYAN +
                           ModuleName + Colors::BRIGHT_RED +
                           "' has nothing named '" + Colors::CYAN + SymbolName +
                           Colors::BRIGHT_RED + "'." + Colors::RESET) {
    Category_ = SLM::ErrorCategory::DECLARATION;
    Hint_ = suggestionsOrFallback(
        Suggestions, "Double-check the spelling, or check " + ModuleName +
                         ".tr for its actual exported names.");
    trySetDeterministicRename(SymbolName, Suggestions);
  }
};
// Raised when two on-demand imports (import module::[...];) in the SAME
// file/context bring in two DIFFERENT symbols under the same bare name --
// exactly the ambiguity the on-demand import feature was designed to catch
// explicitly rather than silently letting one shadow the other. Distinct
// from DuplicateDeclarationError/DuplicateTypeNameError (which are about
// two actual DECLARATIONS colliding): here neither symbol is declared
// twice, they're both real, singly-declared symbols that just happen to
// share a bare name once flattened into the same importing file.
class ImportSymbolCollisionError : public TurfError {
public:
  ImportSymbolCollisionError(SourceLocation Loc, const std::string &SymbolName,
                             const std::string &FirstModule,
                             const std::string &SecondModule,
                             SourceLocation FirstImportLoc)
      : TurfError(Loc, Colors::BRIGHT_RED + "'" + Colors::CYAN + SymbolName +
                           Colors::BRIGHT_RED +
                           "' is imported from two different modules -- '" +
                           Colors::CYAN + FirstModule + Colors::BRIGHT_RED +
                           "' and '" + Colors::CYAN + SecondModule +
                           Colors::BRIGHT_RED + "'." + Colors::RESET) {
    Category_ = SLM::ErrorCategory::DECLARATION;
    Hint_ = "Either qualify one of them explicitly (`" + FirstModule +
            "::" + SymbolName + "` or `" + SecondModule + "::" + SymbolName +
            "`), or switch one of the wildcard/list imports to a more "
            "targeted one that leaves '" + SymbolName + "' out.";
    RelatedLocations_.push_back({FirstImportLoc, "'" + SymbolName + "' first imported from '" + FirstModule + "' here"});
  }
};
// Raised when a struct/enum type name is real (declared SOMEWHERE in the
// program) but not visible UNQUALIFIED from the current file/module: it
// isn't declared in this same file/module, and it was never brought in via
// an on-demand import (`import Module::Type;` / `import Module::[Type,
// ...];` / `import Module::[*];`). Bare visibility for types now follows
// the exact same rule bare function-call visibility already did -- a plain
// `import Module;` grants QUALIFIED access (Module::Type) but not bare
// access, matching import.tr's documented on-demand semantics.
class UnqualifiedTypeNotVisibleError : public TurfError {
public:
  UnqualifiedTypeNotVisibleError(SourceLocation Loc, const std::string &TypeName,
                                 const std::string &OwningModule)
      : TurfError(Loc, Colors::BRIGHT_RED + "'" + Colors::CYAN + TypeName +
                           Colors::BRIGHT_RED +
                           "' isn't visible here without qualification." +
                           Colors::RESET) {
    Category_ = SLM::ErrorCategory::DECLARATION;
    Hint_ = "'" + TypeName + "' is declared in module '" + OwningModule +
            "'. Either use it qualified (`" + OwningModule + "::" + TypeName +
            "`), or bring it into scope with `import " + OwningModule +
            "::" + TypeName + ";`.";
  }
};
// FDN-003: a BARE type spelling that could mean two different types from
// here -- this file's/module's own `Name` and an on-demand-imported one, or
// (defensively) two imports. Before FDN-003 this situation could not arise:
// type names were one program-wide namespace, so declaring the second type
// was already an error (DuplicateTypeNameError). Neither declaration is wrong
// on its own now; only the unqualified USE is, so this is raised at the use.
// Deliberately NOT resolved by "the local one wins": a rule that is too
// strict can be relaxed after 2.0 without breaking anyone, the reverse cannot.
class AmbiguousTypeReferenceError : public TurfError {
public:
  static std::string describe(const std::string &Identity) {
    auto P = Identity.find("::");
    return P == std::string::npos
               ? "'" + Identity + "' declared in this program's main file"
               : "'" + Identity + "' from module '" + Identity.substr(0, P) + "'";
  }

  AmbiguousTypeReferenceError(SourceLocation Loc, const std::string &Spelling,
                              const std::vector<std::string> &Identities)
      : TurfError(Loc, Colors::BRIGHT_RED + "'" + Colors::CYAN + Spelling +
                           Colors::BRIGHT_RED +
                           "' is ambiguous here -- it could mean more than "
                           "one type." + Colors::RESET) {
    Category_ = SLM::ErrorCategory::DECLARATION;
    for (const auto &Id : Identities)
      Message += "\n  " + Colors::BRIGHT_BLUE + "Note: could be " +
                 describe(Id) + Colors::RESET;
    std::string Qualified;
    for (const auto &Id : Identities)
      if (Id.find("::") != std::string::npos) {
        Qualified = Id;
        break;
      }
    Hint_ = Qualified.empty()
                ? "Rename one of them, or import only the names you need "
                  "instead of the one that clashes."
                : "Write the imported one qualified (`" + Qualified +
                      "`) and leave '" + Spelling + "' out of the `import` "
                      "(list the names you need instead of `[*]`), so '" +
                      Spelling + "' means just one type.";
  }
};
// Phase 2: Module-scoped method lookup error
// Raised when a user types e.g. io.pritn() — wrong function name in a known
// module. Suggestions are sourced from that module's function registry.
class ModuleMethodNotFoundError : public TurfError {
public:
  ModuleMethodNotFoundError(SourceLocation Loc,
                            const std::string &ModuleName,
                            const std::string &MethodName,
                            const RankedSuggestions &Suggestions)
      : TurfError(Loc,
                  Colors::BRIGHT_RED + "Oops! Module '" + Colors::CYAN +
                      ModuleName + Colors::BRIGHT_RED +
                      "' has no function named '" + Colors::CYAN + MethodName +
                      Colors::BRIGHT_RED + "'." + Colors::RESET) {
    // Left as the default GENERAL (a judgment call, DIAG-001 audit): could
    // arguably be UNDECLARED_VAR, but the ranked suggestions already do the
    // heavy lifting here.
    // No explicit "Did you mean:\n" prefix -- suggestionsOrFallback()'s
    // formatted suggestion text already starts with its own lead-in
    // ("It may be from the current file: ..."), and stripping its leading
    // "\n  " indent (to attach a prefix in front) left this hint's SECOND
    // line with no indentation of its own once printDiag() added its
    // single "Hint: " lead-in, unlike every other Suggestions-based error
    // class in this file, which passes no prefix and reads cleanly.
    Hint_ = suggestionsOrFallback(Suggestions,
      "Double-check your spelling, or maybe you need to define it?");
    trySetDeterministicRename(MethodName, Suggestions);
  }
};
// (ModuleArityOrTypeError, E025, was deleted in issue #263. It was "kept
// registered since it may get wired up again" while nothing constructed it --
// exactly how a documented code ends up describing an error that never
// appears. A module call that no overload accepts, `math::abs(true)`, is now
// reported with E025 by TypeChecker::reportError() and the curated "No
// overload of 'math.abs' matches argument types (...)" message.)

// LEC-002 (docs/development/changes/LEC-002.md, FREEZE.md §6.15): `pub` /
// `public` written where visibility means nothing. Deliberately an error and
// not a silently ignored modifier -- a `pub` that does nothing is how a typo
// becomes a belief ("I exported that") that only fails once another module
// imports it. One class for every misplacement, with the WHAT in the message
// and the FIX in the hint, so each shape tells the writer exactly what to
// write instead rather than merely that they were wrong.
class MisplacedVisibilityError : public TurfError {
public:
  enum class Where {
    NotFollowedByDeclaration, ///< `pub` then something that is not a declaration
    WithExport,               ///< `pub export immut ...`
    OnOperator,               ///< `pub operator ...`
    OnImport,                 ///< `pub import ...`
    OnVariable,               ///< `pub mut x` / `pub immut X`
    OnEnumVariant,            ///< `pub` before an enum variant
    OnErrorVariant,           ///< `pub` before an `error` type's variant
    OnMachineState,           ///< `pub` before a machine state/transition
    OnParameter,              ///< `pub` before a function parameter
    OnUnsafeBlock,            ///< `pub unsafe { ... }`
    InsideFunction,           ///< `pub` on a declaration local to a function body
    BeforeDecorator,          ///< `pub @caller_location fn ...`
    UsedAsName,               ///< `pub`/`public` where a variable/function/type name is expected
  };

  MisplacedVisibilityError(SourceLocation Loc, Where W)
      : TurfError(Loc, "") {
    Category_ = SLM::ErrorCategory::DECLARATION;
    const std::string Pub = Colors::CYAN + "pub" + Colors::BRIGHT_RED;
    std::string What;
    switch (W) {
    case Where::NotFollowedByDeclaration:
      What = "'" + Pub + "' has to be followed by a declaration.";
      Hint_ = "Put `pub` right before `fn`, `struct`, `enum`, `error`, "
              "`machine` or `extern fn`, or before a struct field -- e.g. "
              "`pub fn area(r: int): int { ... }` or `pub struct Point { pub "
              "x: int }`. (`public` is accepted as a synonym.)";
      break;
    case Where::WithExport:
      What = "'" + Pub + "' can't be combined with 'export'.";
      Hint_ = "`export immut NAME: T = value;` is already visible to every "
              "module that imports this one -- `export` is its `pub`. Just "
              "remove `pub`.";
      break;
    case Where::OnOperator:
      What = "'" + Pub + "' can't be applied to an operator.";
      Hint_ = "An operator is visible wherever its operand types are, so "
              "there is nothing to export. Remove `pub` from the `operator` "
              "declaration; to let another module use `a + b` on your type, "
              "make the type itself `pub`.";
      break;
    case Where::OnImport:
      What = "'" + Pub + "' can't be applied to an import.";
      Hint_ = "An `import` only affects the file that writes it -- imports "
              "are never re-exported. Remove `pub`, and have any module that "
              "needs the imported names write its own `import`.";
      break;
    case Where::OnVariable:
      What = "'" + Pub + "' can't be applied to a variable.";
      Hint_ = "A top-level constant is declared `export immut NAME: T = "
              "value;` -- `export` already makes it visible to importers. A "
              "local variable is never visible outside its function, so "
              "just remove `pub`.";
      break;
    case Where::OnEnumVariant:
      What = "'" + Pub + "' can't be applied to an enum variant.";
      Hint_ = "A variant is public exactly when its enum is. Remove `pub` "
              "here and write `pub enum Name { ... }` instead -- every "
              "variant then becomes visible to importers.";
      break;
    case Where::OnErrorVariant:
      What = "'" + Pub + "' can't be applied to an error variant.";
      Hint_ = "A variant is public exactly when its `error` type is. Remove "
              "`pub` here and write `pub error Name { ... }` instead.";
      break;
    case Where::OnMachineState:
      What = "'" + Pub + "' can't be applied to a machine state or "
             "transition.";
      Hint_ = "States and transitions are public exactly when their machine "
              "is. Remove `pub` here and write `pub machine Name { ... }` "
              "instead.";
      break;
    case Where::OnParameter:
      What = "'" + Pub + "' can't appear in a parameter list.";
      Hint_ = "Parameters are local to their function, so there is nothing "
              "for `pub` to export -- and `pub`/`public` are reserved words, "
              "so neither can be a parameter's name. To make the function "
              "itself visible to importers, put `pub` before `fn`: `pub fn "
              "name(x: int): int`.";
      break;
    case Where::OnUnsafeBlock:
      What = "'" + Pub + "' can't be applied to an `unsafe` block.";
      Hint_ = "`pub` belongs on the declarations inside the block -- write "
              "`unsafe { pub fn f(...) ... }`, not `pub unsafe { ... }`.";
      break;
    case Where::InsideFunction:
      What = "'" + Pub + "' can't be used on a declaration inside a "
             "function.";
      Hint_ = "A declaration inside a function is local to it -- no other "
              "module can ever see it, so there is nothing for `pub` to "
              "do. Remove `pub`, or move the declaration to the top level "
              "of the file if it should be shared.";
      break;
    case Where::UsedAsName:
      What = "'" + Pub + "' is a reserved word and can't be used as a name.";
      Hint_ = "`pub` and `public` are keywords -- they mark a declaration as "
              "visible to other modules -- so they can't name a variable, "
              "function or type. Pick another name, e.g. `is_public` or "
              "`published`.";
      break;
    case Where::BeforeDecorator:
      What = "'" + Pub + "' has to come after a decorator, not before it.";
      Hint_ = "Write the decorator first, then `pub`: `@caller_location` on "
              "its own line, followed by `pub fn name(...)`.";
      break;
    }
    Message = Colors::BRIGHT_RED + What + Colors::RESET;

    // Fix-it: delete the `pub` -- but only for the shapes where deleting it IS
    // the whole fix. Every case above that ends "Remove `pub`" with no
    // alternative qualifies; the rest do not: `pub` on a variable could mean
    // `export` at the top level or nothing at all in a function (the site
    // does not know which), inside a function the hint offers "or move the
    // declaration", a `pub` parameter or name needs a new name, `pub unsafe {`
    // means the writer wants `pub` on what is inside, and a missing
    // declaration or a decorator order needs text this site never saw.
    switch (W) {
    case Where::WithExport:
      removePub("'export' already makes the constant visible to every module "
                "that imports this one, so `pub` adds nothing -- removing it "
                "is the whole fix.");
      break;
    case Where::OnOperator:
      removePub("An operator is visible wherever its operand types are, so "
                "`pub` has nothing to export here -- removing it is the fix.");
      break;
    case Where::OnImport:
      removePub("An `import` only affects the file that writes it and is "
                "never re-exported, so `pub` has no meaning here -- removing "
                "it is the fix.");
      break;
    case Where::OnEnumVariant:
    case Where::OnErrorVariant:
    case Where::OnMachineState:
      removePub("A variant (or state) is public exactly when its enclosing "
                "declaration is, so `pub` on it is meaningless -- removing it "
                "is the fix. Mark the enclosing declaration `pub` if importers "
                "should see it.");
      break;
    default:
      break;
    }
  }

  // BeforeDecorator: `pub @caller_location fn f()` -> `@caller_location pub fn
  // f()`. The raising parser site (ParseVisibilityPrefixed) has to read ahead
  // to the `fn` the decorator is attached to -- the one place a column the
  // message did not already point at is needed -- so it calls this with that
  // token's location, and only when the decorator and `fn` really are what it
  // expected. Moves the writer's own spelling (`pub` or `public`).
  MisplacedVisibilityError &withDecoratorOrderFixIt(const SourceLocation &FnLoc) {
    const std::string Spelling = pubSpellingAtLoc();
    const std::string *FnLine = FixItSourceLine(FnLoc);
    if (Spelling.empty() || !FnLine || FnLoc.Col < 1 ||
        FnLine->compare(FnLoc.Col - 1, 2, "fn") != 0)
      return *this;
    withTokenFixIt(Loc, Spelling, "", EatBlanks::After);
    withFixIt(FnLoc.Line, FnLoc.Col, FnLoc.Col, Spelling + " ");
    withFixReason("A decorator comes first, then the visibility: `@caller_location` "
                  "followed by `pub fn ...`. This moves `" + Spelling +
                  "` from before the decorator to just before `fn`.");
    return *this;
  }

private:
  // The `pub`/`public` token at Loc, or "" if the source line does not read
  // that way there (a mis-pointed Loc must never lead to an edit).
  std::string pubSpellingAtLoc() const {
    const std::string *Line = FixItSourceLine(Loc);
    if (!Line || Loc.Col < 1)
      return "";
    for (const char *Word : {"public", "pub"}) {
      const std::string W(Word);
      const size_t End = Loc.Col - 1 + W.size();
      if (Line->compare(Loc.Col - 1, W.size(), W) == 0 &&
          (End >= Line->size() || !isIdentChar((*Line)[End])))
        return W;
    }
    return "";
  }

  // Deletes the `pub`/`public` token at Loc together with the blanks after it
  // (`pub import x;` -> `import x;`), only if the source really has that
  // token there.
  void removePub(const std::string &Reason) {
    const std::string Spelling = pubSpellingAtLoc();
    if (Spelling.empty())
      return;
    withTokenFixIt(Loc, Spelling, "", EatBlanks::After);
    withFixReason(Reason);
  }
};

// LEC-002 (docs/development/changes/LEC-002.md, FREEZE.md §6.15): a reference
// from one module to a declaration another module did not mark `pub`. One
// class for every kind of private thing (function, type, ...) -- the message
// says WHAT is private and where it lives, and the hint says what to do about
// it: either open it up (name the exact file and keyword) or, if the module
// offers a public way to get the same result, name those. A bare "is private"
// would leave the writer to guess which of the two the module author meant.
class PrivateSymbolAccessError : public TurfError {
public:
  /// Field and Method are about a member of a type: @p Owner names the type.
  enum class Kind { Function, Type, Field, Method };

  /// @param Keyword what to write after `pub` -- "fn" for a function or
  ///        method, the declaring keyword ("struct"/"enum"/"error"/"machine")
  ///        for a type; unused for a field (its declaration is `name: T`).
  /// @param PublicAlternatives the `pub` names of the same kind to offer in
  ///        the hint -- the module's public functions or types, or the owner
  ///        type's public methods (also what a private FIELD is reached
  ///        through, so it is what the hint lists for one); may be empty.
  /// @param Owner the bare name of the type a Field/Method belongs to.
  PrivateSymbolAccessError(SourceLocation Loc, Kind K, const std::string &Name,
                           const std::string &Module, SourceLocation DeclLoc,
                           const std::string &Keyword,
                           const std::vector<std::string> &PublicAlternatives,
                           const std::string &Owner = "")
      : TurfError(Loc, "") {
    Category_ = SLM::ErrorCategory::DECLARATION;
    // What the reader wrote and how to spell the declaration that needs `pub`.
    std::string What, Shown, Fix, Noun;
    switch (K) {
    case Kind::Function:
      What = "Function"; Shown = Name;
      Fix = "`pub " + Keyword + " " + Name + "(...)`"; Noun = "functions";
      break;
    case Kind::Type:
      What = "Type"; Shown = Name;
      Fix = "`pub " + Keyword + " " + Name + "`"; Noun = "types";
      break;
    case Kind::Field:
      What = "Field"; Shown = Owner + "." + Name;
      Fix = "`pub " + Name + ": ...` inside `struct " + Owner + "`";
      Noun = "methods";
      break;
    case Kind::Method:
      What = "Method"; Shown = Owner + "." + Name;
      Fix = "`pub fn " + Owner + "." + Name + "(...)`"; Noun = "methods";
      break;
    }
    const std::string ModuleShown = DisplayModuleName(Module);
    const std::string Subject = K == Kind::Field || K == Kind::Method
                                    ? "'" + Owner + "'"
                                    : "'" + ModuleShown + "'";
    Message = Colors::BRIGHT_RED + What + " '" + Colors::CYAN + Shown +
              Colors::BRIGHT_RED + "' is private to module '" + Colors::CYAN +
              ModuleShown + Colors::BRIGHT_RED + "'." + Colors::RESET;
    withRelated(DeclLoc, "'" + Shown + "' is declared in module '" + ModuleShown +
                             "' without `pub`");

    const std::string File = DeclaringFileName(DeclLoc, Module);
    Hint_ = "If '" + Shown + "' is meant to be used from other modules, mark "
            "it `pub` in " + File + ": " + Fix + ".";
    if (PublicAlternatives.empty()) {
      Hint_ += " " + Subject + " has no public " + Noun + " to use instead.";
    } else {
      Hint_ += " Otherwise, " + Subject + " offers these public " + Noun + ": ";
      size_t Shown = 0;
      for (const auto &Alt : PublicAlternatives) {
        if (Shown == 5) {
          Hint_ += ", ...";
          break;
        }
        Hint_ += (Shown++ ? ", " : "") + Alt;
      }
      Hint_ += ".";
    }
  }
};

// LEC-002 stage 5 (FREEZE.md §6.15, "private-in-public"): a `pub` function
// signature or `pub` field that mentions a type its own module did not mark
// `pub`. Legal in a private declaration; illegal here because an importer
// could then hold a value of a type it cannot spell (`guess`, or passing it
// straight on) -- the privacy the type asked for would leak through the
// signature. Names BOTH declarations: the primary location is the public
// item, the related note is the private type; the hint offers the two ways
// out, and says which one is usually the right one to take.
class PrivateInPublicError : public TurfError {
public:
  /// @param ItemKind "function", "method", "extern function" or "field".
  /// @param ItemName the public item (a method as "Owner.name", a field as
  ///        "Owner.name"), shown to the reader without module qualifiers.
  /// @param TypeName the private type's bare name; @p TypeKeyword its
  ///        declaring keyword (struct/enum/error/machine).
  PrivateInPublicError(SourceLocation Loc, const std::string &ItemKind,
                       const std::string &ItemName, const std::string &TypeName,
                       const std::string &TypeKeyword, const std::string &Module,
                       SourceLocation TypeDeclLoc)
      : TurfError(Loc, "") {
    Category_ = SLM::ErrorCategory::DECLARATION;
    const bool IsField = ItemKind == "field";
    Message = Colors::BRIGHT_RED + "Public " + ItemKind + " '" + Colors::CYAN +
              ItemName + Colors::BRIGHT_RED + "' exposes the private type '" +
              Colors::CYAN + TypeName + Colors::BRIGHT_RED + "'." + Colors::RESET;
    withRelated(TypeDeclLoc, "'" + TypeName + "' is declared in module '" +
                                 DisplayModuleName(Module) + "' without `pub`");
    const std::string File = DeclaringFileName(TypeDeclLoc, Module);
    Hint_ = "Anything `pub` is visible to other modules, but '" + TypeName +
            "' is not, so they would receive values of a type they cannot "
            "name. Either make the type public -- `pub " + TypeKeyword + " " +
            TypeName + "` in " + File + " -- or keep " +
            (IsField ? "the field" : "the " + ItemKind) +
            " internal by removing its `pub`.";
  }
};

#endif
