/**
 * @file ResolverSymbolBase.h
 * @brief Shared, LLVM-free symbol-table data types for the Resolver pass --
 *        the foundation ResolverScopeTable/ResolverModuleTable/
 *        ResolverTypeRegistry/ResolverSymbolTable (split out of the
 *        original monolithic ResolverSymbolTable.h -- see
 *        docs/planning/COMPILER_MODULE_BREAKDOWN.md) all build on.
 *
 * Issue #2 of the monolith → multi-pass migration.
 *
 * Deliberately its OWN header, separate from ResolverSymbolTable.h itself:
 * the 3 sub-tables above each need these types (VariableSymbol,
 * ResolverFunctionSymbol, ...), and ResolverSymbolTable.h needs all 3
 * sub-tables -- if the base types lived in ResolverSymbolTable.h directly,
 * that would be a genuine circular #include (sub-table headers including
 * the umbrella that includes them back), which #include guards cannot
 * resolve correctly (whichever header is entered first ends up missing the
 * other's content). Splitting the base types out here breaks the cycle.
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

#ifndef RESOLVER_SYMBOL_BASE_H
#define RESOLVER_SYMBOL_BASE_H

#include "Lexer.h" // TOK_EQ/TOK_NEQ/TOK_LEQ/TOK_GEQ -- operator token constants, lightweight/no LLVM
#include "Types.h" // TurfTypePtr, TurfType — pure C++ header, no LLVM

#include <memory>
#include <string>
#include <unordered_map>
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

  /// Candidate scope-qualifier names (`Left` in every `Left::Right` or
  /// `Left::Right(...)` shape) found among this function's own `return`
  /// statements (docs/planning/Errors.md), computed once by
  /// Resolver::buildFunctionSymbol() via a conservative, non-interprocedural
  /// scan (CollectReturnedErrorTypeNames() in Resolver.cpp) -- same
  /// conservatism/limitations as MutatesSelf above (no nested-call
  /// awareness).
  ///
  /// NOT pre-filtered to "genuinely a declared error type": this is computed
  /// at Pass 1 hoisting time (buildFunctionSymbol() runs from
  /// registerFunctionSignature(), called from hoistTopLevel()), when a
  /// later-declared error type might not be registered yet, and a
  /// `Left::Right` return could just as easily name an enum/struct/module
  /// call that has nothing to do with errors at all. Consumers (the
  /// "never returns an error" warning and the `recover as err` bare-error
  /// restriction, both in Resolver::visitFuncDef/visitRecoverExpr) must
  /// filter this list through ResolverSymbolTable::isErrorTypeName() /
  /// lookupErrorType() themselves, once ALL error types are guaranteed
  /// registered (Pass 2).
  std::vector<std::string> PossibleErrorTypeNames;

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

// Convert from the resolver's location type (no Lexer.h dependency at the
// TYPE level, even though this conversion function itself needs Lexer.h for
// SourceLocation) to the diagnostic engine's location type. Same layout --
// explicit conversion only because the types are intentionally kept
// separate. A single shared definition: both Resolver*.cpp and
// TypeChecker*.cpp used to each carry their own byte-identical `static`
// copy of this (harmless while each was one file with internal linkage,
// but became a real duplicate-symbol link error once both passes' splits
// needed it to have external linkage for their own cross-file use).
inline SourceLocation toSourceLocation(ResolverSourceLocation R) {
  return {R.Line, R.Col};
}

#endif // RESOLVER_SYMBOL_BASE_H
