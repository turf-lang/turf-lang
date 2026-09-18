#ifndef ERRORS_MODULE_H
#define ERRORS_MODULE_H

#include "Errors.h"

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
      : TurfError(Loc, Colors::BRIGHT_RED + "Could not find module '" +
                           Colors::CYAN + ModuleName + Colors::BRIGHT_RED +
                           "'." + Colors::RESET,
                  "Make sure the file '" + ModuleName + ".tr' exists in the standard library or your current directory.") {
    Category_ = SLM::ErrorCategory::DECLARATION;
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
  }
};
// Phase 2: Module-scoped argument type mismatch error
// Raised when the function name is found in a module but no overload matches
// the given argument types (e.g. math::abs(true) — no abs(bool) overload).
//
// Currently unreachable: grep confirms no call site anywhere in the
// compiler actually constructs this class (module-qualified arity/type
// mismatches are reported through a different path today). Kept
// registered (E025) rather than deleted since the class/message are
// still correct for the scenario it names and it may get wired up again;
// the '.'-form syntax in its message/hint was stale from before the
// module-call migration to '::' and has been corrected here even though
// it isn't live, so it's right if this class is ever raised again.
class ModuleArityOrTypeError : public TurfError {
public:
  ModuleArityOrTypeError(SourceLocation Loc,
                         const std::string &ModuleName,
                         const std::string &MethodName,
                         const std::string &Detail)
      : TurfError(Loc,
                  Colors::BRIGHT_RED + "No matching overload of '" +
                      Colors::CYAN + ModuleName + "::" + MethodName +
                      Colors::BRIGHT_RED + "' for the arguments you gave.\n" +
                      Colors::RESET + "  " + Detail,
                  "Check the argument types and count. Use the "
                  "correct overload, e.g. 'math::abs(int)' or "
                  "'math::abs(double)'.") {
    Category_ = SLM::ErrorCategory::FUNCTION_SIGNATURE;
  }
};

#endif
