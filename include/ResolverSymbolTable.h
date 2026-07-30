/**
 * @file ResolverSymbolTable.h
 * @brief Isolated, LLVM-free symbol table data structures for the Resolver pass.
 *
 * Issue #2 of the monolith → multi-pass migration.
 *
 * This file intentionally has ZERO dependency on LLVM headers.  It models the
 * same concepts as the codegen-facing SymbolTable (scopes, variables, functions,
 * modules) but stores only the information the Resolver and TypeChecker need:
 *
 *   VariableSymbol   — name + type + declaration source location + scope depth
 *   FunctionParam    — a single parameter (name + type) for diagnostic formatting
 *   FunctionSymbol   — base name, arity, parameter names+types, return type,
 *                      source module, and declaration location.
 *                      NOTE: No mangled name — Codegen derives it on demand from
 *                      SourceModule + BaseName + ParamTypes.  This keeps the
 *                      Resolver's error messages in the user's namespace, never
 *                      the LLVM namespace.
 *   ResolverModule   — namespace-isolated map of function overloads (imported)
 *   ResolverScope    — flat name→VariableSymbol map + parent pointer
 *   ResolverSymbolTable — hierarchical scope manager + loaded modules +
 *                         current-file function registry (two-tier diagnostics)
 *
 * Design constraints:
 *  - No llvm::AllocaInst, no llvm::Type, no LLVM headers of any kind.
 *  - All symbols use std::shared_ptr so the Resolver can safely attach them
 *    to AST nodes (Issue #5 will add AST fields to hold these pointers).
 *  - The SymbolTable can be constructed and used entirely without running the
 *    LLVM back-end; unit tests therefore require no special LLVM initialisation.
 *
 * @author a2ys
 * @copyright Copyright (c) 2026 TurfLang contributors.  Apache License.
 */

#ifndef RESOLVER_SYMBOL_TABLE_H
#define RESOLVER_SYMBOL_TABLE_H

#include "Lexer.h" // TOK_EQ/TOK_NEQ/TOK_LEQ/TOK_GEQ -- operator token constants, lightweight/no LLVM
#include "Types.h" // TurfTypePtr, TurfType — pure C++ header, no LLVM

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ─── Source location (same layout as Lexer.h's SourceLocation) ───────────────
// Duplicated as a plain POD here so this header has no dependency on Lexer.h.
struct ResolverSourceLocation {
  int Line = 0;
  int Col  = 0;
};

// ─── VariableSymbol ──────────────────────────────────────────────────────────

/**
 * @brief Represents a single declared variable visible to the Resolver.
 *
 * Fields deliberately mirror what a future TypeChecker pass will need:
 *  - Name and Type are the primary data.
 *  - DeclLoc enables rich "already declared here" error messages.
 *  - ScopeDepth allows the Resolver to implement correct shadowing rules.
 */
struct VariableSymbol {
  std::string            Name;
  TurfTypePtr            Type;        ///< Frontend type (no LLVM involvement)
  ResolverSourceLocation DeclLoc;
  size_t                 ScopeDepth; ///< 0 = global, 1 = first block, …
  bool                   IsImmutable = false;

  VariableSymbol() = default;
  VariableSymbol(std::string Name, TurfTypePtr Type,
                 ResolverSourceLocation DeclLoc, size_t ScopeDepth, bool IsImmutable = false)
      : Name(std::move(Name)), Type(std::move(Type)), DeclLoc(DeclLoc),
        ScopeDepth(ScopeDepth), IsImmutable(IsImmutable) {}
};

// ─── FunctionParam ───────────────────────────────────────────────────────────

/**
 * @brief A single function parameter, carrying both name and type.
 *
 * Storing the name enables rich diagnostic messages:
 *   print(int x)   — rather than just   print(int)
 *
 * The TypeChecker and Codegen use ParamTypes extracted from this for overload
 * resolution and LLVM type mapping respectively.
 */
struct FunctionParam {
  std::string Name; ///< e.g. "x"
  TurfTypePtr Type; ///< e.g. TurfInt()
};

// ─── operator overload helpers ────────────────────────────────────────────
//
// Shared by Parser (ParseOperatorDecl), Resolver (visitBinary candidate
// gathering, registerFunctionSignature's builtin-pair check), and Codegen
// (mangled-name computation) so the four passes can never disagree on what
// counts as an overloadable operator or what its synthesized function name
// is. See STRING_UFCS_OPERATOR_CHAR_RFC.md §4 for the design this implements.
// Declared here, BEFORE ResolverFunctionSymbol, because its own inline
// mangledName() member calls sanitizeSymbolName() -- an inline member
// function body is looked up as if placed right after its enclosing class's
// closing brace, not deferred to end-of-translation-unit, so the free
// function it calls must already be declared earlier in the file.

/// The fixed, whitelisted set of binary operator tokens that `operator`
/// declarations may overload (RFC §8's assumed whitelist, minus []/[]= and
/// unary -/! -- both explicitly out of scope for this pass; see the RFC's
/// build-order notes). Deliberately excludes `%`, `**`, bitwise (`& | ^ <<
/// >>`), `&&`/`||`, and every assignment form -- none of those go through
/// operator resolution at all, so e.g. `a & b` on a struct with no bitwise
/// meaning stays a plain, unambiguous type error rather than silently
/// searching an overload table that was never meant to apply to it.
inline bool isOverloadableBinaryOp(int Tok) {
  switch (Tok) {
  case '+':
  case '-':
  case '*':
  case '/':
  case TOK_EQ:
  case TOK_NEQ:
  case '<':
  case '>':
  case TOK_LEQ:
  case TOK_GEQ:
    return true;
  default:
    return false;
  }
}

/// Synthesizes the overload-table key / FuncDefExprAST name for an
/// `operator` declaration or use, e.g. '+' -> "operator+", TOK_EQ ->
/// "operator==". This can never collide with a real user identifier --
/// `operator` is a keyword token (TOK_OPERATOR) and no legal TOK_IDENTIFIER
/// spelling can contain '+ - * / = < >' -- so storing operator overloads in
/// the same name-keyed overload map ordinary `fn`s use (ResolverModule::
/// Functions / TurfModule::Functions) is safe and requires no new map.
/// Returns "" for a non-overloadable token (callers should have already
/// checked isOverloadableBinaryOp() first).
inline std::string operatorFunctionName(int Tok) {
  switch (Tok) {
  case '+': return "operator+";
  case '-': return "operator-";
  case '*': return "operator*";
  case '/': return "operator/";
  case TOK_EQ: return "operator==";
  case TOK_NEQ: return "operator!=";
  case '<': return "operator<";
  case '>': return "operator>";
  case TOK_LEQ: return "operator<=";
  case TOK_GEQ: return "operator>=";
  default: return "";
  }
}

/// Maps a synthesized operator function name (e.g. "operator+",
/// "operator==") to a symbol-safe spelling for LLVM mangled names, so the
/// emitted symbol is e.g. "turf_strings_operator_plus_string_string"
/// instead of the raw "turf_strings_operator+_string_string" (which
/// contains characters no linker/assembler is guaranteed to accept in a
/// symbol name). Every non-operator name (the overwhelming majority of
/// calls) passes through completely unchanged via the map-miss fallback.
inline std::string sanitizeSymbolName(const std::string &Name) {
  static const std::unordered_map<std::string, std::string> Map = {
      {"operator+", "operator_plus"},   {"operator-", "operator_minus"},
      {"operator*", "operator_star"},   {"operator/", "operator_slash"},
      {"operator==", "operator_eq"},    {"operator!=", "operator_ne"},
      {"operator<", "operator_lt"},     {"operator>", "operator_gt"},
      {"operator<=", "operator_le"},    {"operator>=", "operator_ge"},
  };
  auto It = Map.find(Name);
  return It != Map.end() ? It->second : Name;
}

// ─── ResolverFunctionSymbol ──────────────────────────────────────────────────

/**
 * @brief Represents one overload of a function visible to the Resolver.
 *
 * Design decisions driven by the two-tier diagnostic system:
 *
 *  - NO MangledName field.  The LLVM symbol name is derived by Codegen on
 *    demand via a deterministic mangling function:
 *        mangle("io", "printline", [int])  → "turf_io_printline_int"
 *        mangle("",   "foo",       [int])  → "foo"
 *    Removing it from the Resolver keeps all error messages in the user's
 *    namespace, never in LLVM's.
 *
 *  - SourceModule: empty = defined in the current file;
 *                  non-empty = name of the imported module ("io", "math", …).
 *    This drives the two-tier error format:
 *
 *        Error: Unknown function 'pritn'
 *          Current file:   print(int x)     — line 12
 *          From io module: io.printline(str) — try 'io.printline(x)'
 *
 *  - DeclLoc: source location of the fn keyword, used for "defined at …" notes.
 */
struct ResolverFunctionSymbol {
  std::string              BaseName;     ///< User-visible name, e.g. "printline"
  std::string              SourceModule; ///< "" = current file, "io" = imported
  std::vector<FunctionParam> Params;     ///< Ordered parameters (name + type)
  TurfTypePtr              ReturnType;
  ResolverSourceLocation   DeclLoc;      ///< Location of the fn definition
  bool                     IsVarArg    = false; ///< true for variadic extern fn (e.g. printf)
  /// True for an `extern fn` declaration (FFI binding to a real C symbol).
  /// Codegen's ExternFuncDeclExprAST::codegen() always registers the LLVM
  /// function under its raw declared name (Name_, e.g. "free") for linking
  /// purposes -- it is NEVER given a Turf-mangled name the way a regular
  /// `fn`/module function is. mangledName() below checks this flag so it
  /// returns that same raw name instead of computing a Turf-mangled one
  /// that was never actually registered anywhere.
  bool                     IsExtern    = false;

  /// True if this is a struct instance method (Params[0].Name == "self")
  /// whose body directly assigns to a `self.<field>` somewhere. Computed
  /// once by Resolver::buildFunctionSymbol() via a conservative, non-
  /// interprocedural scan (see MutatesSelfField() in Resolver.cpp).
  /// TypeChecker::visitMethodCall() uses this to reject calling a mutating
  /// method through an immutable variable -- e.g. `c.bump()` where
  /// `bump()` does `self.val += 1` -- which direct field assignment
  /// (`c.val = 5`) already correctly rejects, but method calls previously
  /// bypassed entirely (ImmutableAssignmentError never even considered
  /// them). Always false for static methods, free functions, and module
  /// functions (none of which have a "self" to mutate).
  bool                     MutatesSelf = false;

  /// Arity of this overload.
  /// For variadic functions, this is the number of *named* (declared) params.
  /// Callers should use isVarArg() to determine if extra args are accepted.
  int arity() const { return static_cast<int>(Params.size()); }
  bool isVarArg() const { return IsVarArg; }

  /// Format a human-readable signature for diagnostics:
  ///   "print(int x, string y) -> void"
  std::string signature() const {
    std::string S = BaseName + "(";
    for (size_t i = 0; i < Params.size(); ++i) {
      if (i) S += ", ";
      S += (Params[i].Type ? Params[i].Type->getName() : "?");
      if (!Params[i].Name.empty()) { S += " "; S += Params[i].Name; }
    }
    S += ") -> ";
    S += (ReturnType ? ReturnType->getName() : "void");
    return S;
  }

  /// Extract just the parameter types — used by overload resolution logic.
  std::vector<TurfTypePtr> paramTypes() const {
    std::vector<TurfTypePtr> T;
    T.reserve(Params.size());
    for (const auto &P : Params) T.push_back(P.Type);
    return T;
  }

  /// Derive the LLVM-level mangled name for this overload.
  /// Mirrors the mangling performed by FuncDefExprAST::codegen():
  ///   module="io",  base="printline", params=[int] → "turf_io_printline_int"
  ///   module="",    base="add",       params=[int,int] → "turf_add_int_int"
  ///   module="",    base="main",      params=[] → "main"
  ///
  /// An extern fn is different: it is never Turf-mangled at all, in any
  /// module or arity/type combination -- see IsExtern's doc comment above.
  /// Before this check existed, a module-qualified call to a re-exported
  /// extern (e.g. `mem.free(buf)`, where lib/std/mem.tr declares
  /// `extern fn free(p: void*): void;` directly, with no Turf-level wrapper)
  /// computed "turf_mem_free_void_ptr" here, which was never registered as
  /// an LLVM function anywhere -- Codegen only ever created "free". A
  /// direct (non-module-qualified) call to the same extern happened to keep
  /// working regardless, purely because FuncCallExprAST::codegen() has a
  /// fallback that retries the plain, unmangled callee name when the
  /// mangled lookup misses; MethodCallExprAST::codegen()'s module-call path
  /// has no equivalent fallback and hit a hard "Internal error: LLVM
  /// function ... not found" instead.
  std::string mangledName() const {
    if (BaseName == "main") return "main";
    if (IsExtern) return BaseName;
    std::string S = "turf_";
    if (!SourceModule.empty()) S += SourceModule + "_";
    S += sanitizeSymbolName(BaseName);
    for (const auto &P : Params)
      if (P.Type) S += "_" + P.Type->getMangledName();
    return S;
  }
};

// ─── ResolverModule ──────────────────────────────────────────────────────────

/**
 * @brief Represents one imported module's function namespace.
 *
 * Lookup is strictly namespace-isolated: searching "io" will never find
 * functions registered under "math".
 */
/**
 * @brief A single `export immut NAME: TYPE = LITERAL;` declaration.
 *
 * Unlike ResolverFunctionSymbol, there is no MangledName-avoidance concern
 * here at all -- an exported constant never becomes a real, addressable
 * LLVM symbol in the first place. It's a pure compile-time value: Codegen
 * emits the literal directly at every reference site (bare or
 * `Module::NAME`-qualified) instead of loading from any storage location.
 * That's why this struct stores the actual VALUE (as a small tagged union
 * over the 4 primitive kinds `export immut` currently supports), not a
 * name to look up later -- there IS no "later", the value IS the symbol.
 *
 * Deliberately excludes any `ExprAST*`/AST-node reference, matching this
 * whole header's "zero LLVM/AST dependency" design constraint (see the
 * file-level doc comment) -- the value is extracted from the literal
 * initializer expression ONCE, during Resolver Pass 1 hoisting, and lives
 * here as plain data from then on.
 */
struct ResolverConstantSymbol {
  std::string Name;
  std::string SourceModule; ///< "" = current file, else the declaring module
  TurfTypePtr Type;
  ResolverSourceLocation DeclLoc;

  // Exactly one of these is meaningful, selected by Type->isInt()/
  // isDouble()/isBool()/isString() -- `export immut` only supports these 4
  // primitive literal kinds today (see ParseExportDecl()'s validation).
  long long IntVal = 0;
  double DoubleVal = 0.0;
  bool BoolVal = false;
  std::string StringVal;
};

class ResolverModule {
public:
  std::string Name; ///< e.g. "io", "math"

  /// base name → list of overloads
  std::unordered_map<std::string, std::vector<ResolverFunctionSymbol>>
      Functions;

  /// base name → exported constant. Unlike Functions, never a vector --
  /// variables don't overload.
  std::unordered_map<std::string, ResolverConstantSymbol> Variables;

  explicit ResolverModule(std::string name) : Name(std::move(name)) {}

  /**
   * @brief Register a function overload.
   * Duplicate registrations (same BaseName + arity + param types) are
   * silently ignored to handle prototype re-entry during the pre-pass.
   */
  void registerFunction(const ResolverFunctionSymbol &Sym);

  // NOTE: this class used to also have a lookupFunction(BaseName, ArgTypes)
  // overload-resolution method (arity + exact-type-match + numeric-coercion
  // fallback, mirroring TurfModule::lookupFunction in SymbolTable.h). It was
  // removed after confirming via repo-wide grep that it had zero callers:
  // the Resolver's actual module-function lookup path is
  // ResolverSymbolTable::findInModules() followed by the arity/type
  // narrowing already inline in Resolver::visitFuncCall/visitMethodCall, not
  // a per-module lookup here.

  /// Return all registered base names (for suggestion / diagnostics).
  std::vector<std::string> getFunctionNames() const;
};

// ─── ResolverScope ───────────────────────────────────────────────────────────

/**
 * @brief A single lexical scope in the Resolver's scope stack.
 *
 * Uses a pointer-to-parent chain so scopes can be queried independently of
 * the full stack (useful for the TypeChecker which may clone a scope chain).
 */
struct ResolverScope {
  size_t Depth;   ///< 0 = global, increments per nested block
  ResolverScope *Parent = nullptr; ///< Outer scope, or nullptr at global level

  /// Variable name → owned VariableSymbol
  std::unordered_map<std::string, std::shared_ptr<VariableSymbol>> Symbols;

  explicit ResolverScope(size_t Depth, ResolverScope *Parent = nullptr)
      : Depth(Depth), Parent(Parent) {}

  /// Look up a name in this scope only (no parent traversal).
  std::shared_ptr<VariableSymbol> lookupLocal(const std::string &Name) const {
    auto It = Symbols.find(Name);
    return It != Symbols.end() ? It->second : nullptr;
  }

  /// True if @p Name is declared directly in this scope.
  bool has(const std::string &Name) const {
    return Symbols.count(Name) > 0;
  }
};

// ─── ResolverSymbolTable ─────────────────────────────────────────────────────

/**
 * @brief Hierarchical symbol table for the Resolver and TypeChecker passes.
 *
 * This class manages:
 *  - A stack of ResolverScope objects for lexical scope handling.
 *  - A registry of ResolverModule objects for module-qualified lookups.
 *
 * It is entirely LLVM-free.  Unit tests can construct and exercise it without
 * initialising the LLVM back-end.
 *
 * Error handling: lookup failures return nullptr (not exceptions) so that the
 * Resolver can accumulate multiple errors per compilation run.  enterScope /
 * exitScope are infallible (no error on over-pop; production code tracks this
 * separately).
 */
class ResolverSymbolTable {
public:
  ResolverSymbolTable();

  // ── Scope management ────────────────────────────────────────────────────────

  /// Push a new nested scope.
  void enterScope();

  /// Pop the current scope.  Variables declared in it become invisible.
  void exitScope();

  /// Reset the scope stack to the specified depth. Used during error recovery.
  void resetToDepth(size_t TargetDepth);

  /// Current nesting depth (0 = global).
  size_t currentDepth() const { return Depth_; }

  // ── Variable declarations ────────────────────────────────────────────────────

  /**
   * @brief Declare a new variable in the current scope.
   *
   * Returns a shared_ptr to the new VariableSymbol so the Resolver can attach
   * it to AST nodes (Issue #5).  Throws std::runtime_error if the name is
   * already declared in the *current* scope (shadowing outer scopes is allowed).
   */
  std::shared_ptr<VariableSymbol> declareVariable(
      const std::string &Name, TurfTypePtr Type,
      ResolverSourceLocation Loc = {}, bool IsImmutable = false);

  /**
   * @brief Look up a variable by name, searching from the innermost scope out.
   * @return Shared pointer to the symbol, or nullptr if not found.
   */
  std::shared_ptr<VariableSymbol> lookupVariable(const std::string &Name) const;

  /**
   * @brief True if @p Name is declared in the current (innermost) scope only.
   */
  bool isDeclaredInCurrentScope(const std::string &Name) const;

  /**
   * @brief Collect every variable name visible from the current scope.
   *
   * Walks the scope stack from innermost to outermost and deduplicates names
   * so that a shadowed outer variable is only listed once.  The result is
   * suitable as the candidate list passed to SuggestionIndex::suggestVisible
   * when emitting an "Unknown Variable" diagnostic.
   */
  std::vector<std::string> getAllVisibleNames() const;

  // ── Module / imported function registry ─────────────────────────────────────

  /**
   * @brief Register a function symbol under the given imported module namespace.
   * The module is created automatically if it doesn't exist yet.
   * Use this for `import io` declarations — not for functions defined in the
   * current file (use registerCurrentFileFunction for those).
   */
  void registerModuleFunction(const std::string &ModuleName,
                               const ResolverFunctionSymbol &Sym);

  /**
   * @brief Register an `export immut` constant under the given module
   * namespace. Mirrors registerModuleFunction() exactly (auto-creates the
   * module entry if needed), except a duplicate name is a real conflict
   * (variables don't overload) -- the caller is expected to have already
   * checked isKnownConstant()-style existence before calling this, the same
   * way registerStruct()/registerEnumType() callers do for DuplicateTypeNameError.
   */
  void registerModuleConstant(const std::string &ModuleName,
                              const ResolverConstantSymbol &Sym);

  /**
   * @brief Look up an exported constant by name, scoped to exactly
   * @p ModuleName. Returns nullptr if that module has no such constant
   * (or isn't a known module at all). Mirrors the "explicit qualification"
   * lookup shape findInModules() + a ModuleName filter already provides for
   * functions, but constants never overload so this returns at most one
   * match directly instead of a candidate list.
   */
  const ResolverConstantSymbol *
  findModuleConstant(const std::string &ModuleName,
                     const std::string &Name) const;

  // NOTE: this class used to also have resolveModuleFunction(ModuleName,
  // FuncName, ArgTypes) and getModule(ModuleName) methods for looking up a
  // single named module directly. Both were removed after confirming via
  // repo-wide grep that they had zero callers anywhere -- module-qualified
  // calls (e.g. `io.printline(42)`) are resolved via findInModules() (which
  // searches ALL modules and lets the caller filter by name, used by
  // Resolver::visitFuncCall/visitMethodCall for their two-tier "current file
  // vs imported module" diagnostics) rather than a direct single-module
  // lookup.

  /**
   * @brief Ensures @p ModuleName is registered as a known, imported module,
   * even if it turns out to declare zero functions (e.g. a module that only
   * declares structs/enums, like `enum Shade {...}` with no functions at
   * all). Called once per hoisted `ModuleImportExprAST`, unconditionally,
   * BEFORE any function/struct/enum inside it is processed -- registerModuleFunction()
   * ALSO lazily creates an entry, but only when a function actually exists,
   * which used to mean a function-less module was completely invisible to
   * hasModule() (found via a real bug: `import colors;` where colors.tr
   * declares only an enum -- `colors::Shade` then failed with "'colors'
   * isn't a known enum or module" despite the import succeeding and the
   * enum being perfectly real and usable bare). Idempotent.
   */
  void registerModule(const std::string &ModuleName);

  /**
   * @brief True if @p ModuleName names a module that was actually imported
   * somewhere in this program -- regardless of whether it happens to
   * declare any functions. NOT the same question as "does findInModules()
   * return anything for some specific name in it".
   */
  bool hasModule(const std::string &ModuleName) const;

  /**
   * @brief Return all base function names registered under @p ModuleName.
   */
  std::vector<std::string> getModuleFunctionNames(
      const std::string &ModuleName) const;

  /**
   * @brief Return all `export immut` constant names registered under
   * @p ModuleName. Mirrors getModuleFunctionNames(); used by
   * registerOnDemandImports()'s Wildcard branch to flat-import every
   * constant a module exports, same as it already does for functions/types.
   */
  std::vector<std::string> getModuleConstantNames(
      const std::string &ModuleName) const;

  // ── Current-file function registry ──────────────────────────────────────────
  // These track `fn` definitions in the file being compiled, separately from
  // imported module functions.  This separation powers the two-tier diagnostic:
  //
  //   Error: Unknown function 'pritn'
  //     Current file:   print(int x)      — line 12
  //     From io module: io.printline(str)  — try 'io.printline(x)'
  //
  // Issue #6 wires visitFuncDef to call registerCurrentFileFunction.

  /**
   * @brief Register a top-level fn definition from the current source file.
   * Duplicate registrations (same signature) are silently ignored.
   * Emits no error on overloads — that is the TypeChecker's job.
   */
  void registerCurrentFileFunction(const ResolverFunctionSymbol &Sym);

  /**
   * @brief Register an `export immut` constant declared at the top level of
   * the current (main) source file. Mirrors registerCurrentFileFunction();
   * see ResolverConstantSymbol's doc comment for why this stores the actual
   * value rather than a name to resolve later.
   */
  void registerCurrentFileConstant(const ResolverConstantSymbol &Sym);

  /**
   * @brief Look up an `export immut` constant declared in the current file
   * by name. Returns nullptr if no such constant exists there (this file's
   * own self-reference case -- mirrors lookupCurrentFileFunctions(), minus
   * the vector-of-overloads shape since constants never overload).
   */
  const ResolverConstantSymbol *
  lookupCurrentFileConstant(const std::string &Name) const;

  /**
   * @brief Return all overloads of @p Name defined in the current file.
   * Returns an empty vector if the name is not defined here.
   */
  std::vector<const ResolverFunctionSymbol *>
  lookupCurrentFileFunctions(const std::string &Name) const;

  /**
   * @brief Return all function names defined in the current file.
   * Used as the Tier-1 candidate list for Damerau-Levenshtein suggestions
   * when an unknown function name is encountered.
   */
  std::vector<std::string> getAllCurrentFileFunctionNames() const;

  // ── On-demand (flat) import registry ─────────────────────────────────────────
  // Backs `import module::symbol;` / `import module::[a, b];` /
  // `import module::[*];` (ImportMode::Symbol/SymbolList/Wildcard) -- each
  // pulls specific function names into a "context" (the main file, if
  // ContextLabel == "", or a module name, if some module itself does an
  // on-demand import of another module) so a BARE call can resolve to them,
  // same as it already can for that context's own self-declared functions.
  // The registry only ever stores WHICH module backs a name, not the
  // function symbol itself -- the actual overload data is already in
  // Modules_ (see findInModules()), so this is purely a name->module
  // routing table, re-queried through findInModules() at lookup time
  // exactly like self-reference already is (Resolver::visitFuncCall).
  //
  // Struct/enum symbol imports get the PARALLEL FlatImportedTypes_ registry
  // below, not this one -- see its doc comment for why bare TYPE visibility
  // is gated the same way as bare CALL visibility now (it deliberately
  // wasn't at first; see isTypeVisibleUnqualified()'s doc comment).

  struct FlatImportedSymbol {
    std::string SourceModule;
    ResolverSourceLocation ImportLoc;
  };

  /**
   * @brief Register that @p Name is on-demand-imported from @p Sym.SourceModule
   * into @p ContextLabel's flat namespace.
   * Returns the PREVIOUS entry if @p Name already has a flat import in this
   * context (from an earlier explicit symbol/list import, or a previous
   * wildcard import that already claimed the name) -- the caller raises a
   * collision error using it. Returns std::nullopt on a fresh registration.
   */
  std::optional<FlatImportedSymbol>
  registerFlatFunctionImport(const std::string &ContextLabel,
                             const std::string &Name,
                             const FlatImportedSymbol &Sym);

  /**
   * @brief Look up whether @p Name is flat-imported into @p ContextLabel.
   * Returns nullptr if not. Used by visitFuncCall's bare-call resolution
   * alongside (not instead of) the self-reference check.
   */
  const FlatImportedSymbol *
  lookupFlatFunctionImport(const std::string &ContextLabel,
                           const std::string &Name) const;

  /**
   * @brief Register that struct/enum @p Name is on-demand-imported from
   * @p Sym.SourceModule into @p ContextLabel's flat namespace. Mirrors
   * registerFlatFunctionImport() exactly, kept as a SEPARATE registry (not
   * folded into FlatImportedFunctions_) because a struct/enum name and a
   * function name can legally coexist under the same bare spelling without
   * colliding -- they occupy different syntactic positions (type position
   * vs. call position) so there is no reason to force them to share one
   * collision-detection namespace.
   * Returns the PREVIOUS entry on a same-name collision from a different
   * module, std::nullopt on a fresh registration -- same contract as the
   * function-import version.
   */
  std::optional<FlatImportedSymbol>
  registerFlatTypeImport(const std::string &ContextLabel,
                         const std::string &Name,
                         const FlatImportedSymbol &Sym);

  /**
   * @brief Register that `export immut` constant @p Name is on-demand-
   * imported from @p Sym.SourceModule into @p ContextLabel's flat namespace.
   * Mirrors registerFlatFunctionImport()/registerFlatTypeImport() exactly,
   * kept as its own registry (FlatImportedConstants_) for the same reason
   * the type registry is separate from the function one -- see this class's
   * FlatImportedConstants_ doc comment.
   * Returns the PREVIOUS entry on a same-name collision from a different
   * module, std::nullopt on a fresh registration.
   */
  std::optional<FlatImportedSymbol>
  registerFlatConstantImport(const std::string &ContextLabel,
                             const std::string &Name,
                             const FlatImportedSymbol &Sym);

  /**
   * @brief Look up whether constant @p Name is flat-imported into
   * @p ContextLabel. Returns nullptr if not.
   */
  const FlatImportedSymbol *
  lookupFlatConstantImport(const std::string &ContextLabel,
                           const std::string &Name) const;

  /**
   * @brief True if struct/enum @p Name is unqualified-visible from
   * @p ContextLabel: either @p ContextLabel is the type's OWN declaring
   * module (per getTypeOwningModule()), or it was brought in via an
   * on-demand symbol/list/wildcard import (registerFlatTypeImport()).
   *
   * Before this gate existed, isKnownStruct()/isEnumName() alone answered
   * "does this name exist ANYWHERE in the whole program" with no per-file
   * dimension at all -- so `List<int>` (or ANY struct/enum from ANY
   * transitively-imported module) was bare-usable after just `import
   * list;`, unlike bare function calls, which already required
   * self-reference or an explicit on-demand import
   * (lookupFlatFunctionImport()). That inconsistency is the literal bug
   * this method fixes: bare type visibility now follows the identical rule
   * bare call visibility already did. Qualified access (`list::List`) is
   * NOT gated by this at all -- it goes through validateModuleQualifier()
   * instead, which only cares whether the qualifier is correct, not
   * whether it was on-demand-imported.
   */
  bool isTypeVisibleUnqualified(const std::string &ContextLabel,
                                const std::string &Name) const;

  /**
   * @brief Every struct/enum name declared directly by @p ModuleName (not
   * transitively -- a nested import inside that module doesn't count).
   * Used by registerOnDemandImports()'s Wildcard branch to flat-import
   * every type a module owns, mirroring getModuleFunctionNames().
   */
  std::vector<std::string> getModuleTypeNames(const std::string &ModuleName) const;

  // ── Two-tier diagnostic helpers ──────────────────────────────────────────────

  /**
   * @brief Search every loaded module for overloads matching @p Name.
   *
   * Returns a list of (module_name, symbol_ptr) pairs so the diagnostic
   * can format:  "From io: io.printline(str)  — try 'io.printline(x)'"
   *
   * This is Tier-2 in the error hierarchy — checked after the current-file
   * registry fails.
   */
  struct ModuleFunctionMatch {
    std::string ModuleName;
    const ResolverFunctionSymbol *Symbol;
  };
  std::vector<ModuleFunctionMatch>
  findInModules(const std::string &Name) const;

  /**
   * @brief Collect every function name visible across ALL modules.
   * Used to compute Damerau-Levenshtein suggestions spanning both tiers:
   * the Resolver can rank and present them grouped by source.
   */
  std::vector<std::string> getAllModuleFunctionNames() const;

  // ── Struct name registry ─────────────────────────────────────────────────
  // Tracks struct names declared in the current file so the Resolver can
  // validate NamedType references in Pass 2 without asking the TypeChecker.
  // Populated during Pass 1 (hoistTopLevel); queried during visitStructDecl
  // and any future pass that needs to distinguish struct names from unknowns.

  /**
   * @brief Register a struct name hoisted from a top-level StructDeclExprAST.
   *
   * Struct (and enum) names live in ONE flat namespace shared by the current
   * file and every transitively-imported module -- unlike functions, which
   * are namespaced per-module (see ResolverModule / registerModuleFunction).
   * That means two unrelated modules declaring a same-named struct silently
   * collide here, and would previously go on to crash Codegen with a
   * confusing "Internal compiler error" (LLVM refuses two distinct opaque
   * struct types sharing one name, renames the second to "Name.1", and the
   * GEP/return-type instructions already emitted against the frontend name
   * end up pointing at the wrong LLVM type). Returns the ORIGINAL
   * declaration's location if @p Name was already registered (so the caller
   * can raise a clean DuplicateTypeNameError instead of reaching Codegen at
   * all), or std::nullopt if this is a fresh registration.
   *
   * @p ModuleName records WHICH module actually declares this type ("" for
   * the main file) -- unlike the flat isKnownStruct()/getAllStructNames()
   * registry (which intentionally answers only "does this name exist
   * ANYWHERE"), this powers getTypeOwningModule(), used to validate an
   * explicit `Module::TypeName` qualifier actually names the RIGHT module,
   * not just A real one.
   */
  std::optional<ResolverSourceLocation>
  registerStruct(const std::string &Name, ResolverSourceLocation Loc,
                 const std::string &ModuleName = "");

  /**
   * @brief True if @p Name was registered as a struct in the current file.
   * Used by the Resolver to validate NamedType field references.
   */
  bool isKnownStruct(const std::string &Name) const;

  /**
   * @brief Return all registered struct names (sorted).
   * Used as the candidate list for Damerau-Levenshtein suggestions when an
   * unknown named type is encountered in a struct field declaration.
   */
  std::vector<std::string> getAllStructNames() const;

  // ── Enum registry ─────────────────────────────────────────────────────────

  // Same flat, module-blind namespace concern as registerStruct() above
  // applies to enums (they share the type namespace with structs at the
  // Codegen/LLVM layer too). Returns the original declaration's location on
  // a duplicate name, std::nullopt on a fresh registration.
  std::optional<ResolverSourceLocation>
  registerEnumType(const std::string& Name, ResolverSourceLocation Loc,
                   std::shared_ptr<TurfEnumType> ET,
                   const std::string &ModuleName = "");
  std::shared_ptr<TurfEnumType> lookupEnumType(const std::string& Name) const;
  bool isEnumName(const std::string& Name) const;
  std::vector<std::string> getAllEnumNames() const;

  // ── User-declared `error` type registry (docs/planning/Errors.md) ────────

  // Same shape as the enum registry above -- a flat, module-blind namespace,
  // own duplicate-name map (not cross-checked against structs/enums, same as
  // struct vs. enum today). Returns the original declaration's location on a
  // duplicate name, std::nullopt on a fresh registration.
  std::optional<ResolverSourceLocation>
  registerErrorType(const std::string& Name, ResolverSourceLocation Loc,
                    std::shared_ptr<TurfUserErrorType> ET,
                    const std::string &ModuleName = "");
  std::shared_ptr<TurfUserErrorType> lookupErrorType(const std::string& Name) const;
  bool isErrorTypeName(const std::string& Name) const;
  std::vector<std::string> getAllErrorTypeNames() const;

  /**
   * @brief Which module declares @p Name ("" for the main file, or if
   * @p Name isn't a known struct/enum at all). Struct/enum names are
   * globally unique once declared (see registerStruct()'s doc comment on
   * the flat-namespace collision rule), so there is always exactly one
   * owner to report.
   */
  std::string getTypeOwningModule(const std::string &Name) const;

  /**
   * @brief Dumps the contents of the symbol table (all scopes and modules) to the output stream.
   */
  void dump(std::ostream &OS) const;

  /**
   * @brief Dumps EVERYTHING this Resolver instance knows about modules,
   * specifically -- unlike dump() (which mixes in variable scopes and is
   * organized around the two-tier call-resolution model), this is a
   * dedicated module-system introspection report: every imported module's
   * full function list, which module owns each struct/enum type, and every
   * on-demand (Symbol/SymbolList/Wildcard) flat import currently in effect,
   * grouped by which file/module requested it. Backs `--dump-modules`.
   */
  void dumpModules(std::ostream &OS) const;

private:
  // Stack of scope objects.  We own them via unique_ptr; the parent pointer
  // inside ResolverScope is a raw (non-owning) back-reference.
  std::vector<std::unique_ptr<ResolverScope>> ScopeStack_;
  size_t Depth_ = 0;

  /// Imported module registries (one per 'import X' statement).
  std::unordered_map<std::string, std::unique_ptr<ResolverModule>> Modules_;

  /// Top-level fn definitions from the current source file.
  /// Keyed by bare function name; value is the list of overloads.
  /// Separate from Modules_ so the two-tier error system can distinguish
  /// "this file" from "imported modules" without string-matching module names.
  std::unordered_map<std::string, std::vector<ResolverFunctionSymbol>>
      CurrentFileFunctions_;

  /// On-demand flat function imports, keyed by context ("" = main file,
  /// else a module name), then by the bare imported name. See
  /// registerFlatFunctionImport()'s doc comment.
  std::unordered_map<std::string, std::unordered_map<std::string, FlatImportedSymbol>>
      FlatImportedFunctions_;

  /// On-demand flat struct/enum TYPE imports -- same shape and keying as
  /// FlatImportedFunctions_ above, kept as a separate map (see
  /// registerFlatTypeImport()'s doc comment for why).
  std::unordered_map<std::string, std::unordered_map<std::string, FlatImportedSymbol>>
      FlatImportedTypes_;

  /// `export immut` constants declared at the top level of the current
  /// (main) source file. Mirrors CurrentFileFunctions_ but never a vector --
  /// variables don't overload.
  std::unordered_map<std::string, ResolverConstantSymbol> CurrentFileConstants_;

  /// On-demand flat CONSTANT imports -- same shape and keying as
  /// FlatImportedFunctions_/FlatImportedTypes_ above, kept as a third
  /// separate map since a constant, a function, and a type can all legally
  /// share one bare spelling without colliding (they occupy different
  /// syntactic positions: value position, call position, type position).
  std::unordered_map<std::string, std::unordered_map<std::string, FlatImportedSymbol>>
      FlatImportedConstants_;

  /// Struct names declared in the current file or any transitively-imported
  /// module (hoisted in Pass 1), mapped to their declaration site so a
  /// same-named second declaration can be reported with a "first declared
  /// here" related location. Queried in Pass 2 to validate NamedType field
  /// references without involving the TypeChecker or LLVM.
  std::unordered_map<std::string, ResolverSourceLocation> RegisteredStructs_;

  /// Enums declared in the current file or any transitively-imported module.
  std::unordered_map<std::string, std::shared_ptr<TurfEnumType>> RegisteredEnums_;

  /// Parallel to RegisteredEnums_, since TurfEnumType itself carries no
  /// declaration location -- see registerEnumType()'s doc comment.
  std::unordered_map<std::string, ResolverSourceLocation> EnumDeclLocs_;

  /// User-declared `error` types (docs/planning/Errors.md), declared in the
  /// current file or any transitively-imported module. Parallel shape to
  /// RegisteredEnums_/EnumDeclLocs_ above.
  std::unordered_map<std::string, std::shared_ptr<TurfUserErrorType>> RegisteredErrorTypes_;
  std::unordered_map<std::string, ResolverSourceLocation> ErrorDeclLocs_;

  /// Which module owns each struct/enum/error name ("" = main file). See
  /// getTypeOwningModule()'s doc comment.
  std::unordered_map<std::string, std::string> TypeOwningModule_;
};

#endif // RESOLVER_SYMBOL_TABLE_H
