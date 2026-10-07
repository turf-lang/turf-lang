/**
 * @file ResolverSymbolBase.h
 * @brief Shared, LLVM-free symbol-table data types for the Resolver pass --
 *        the foundation ResolverScopeTable/ResolverModuleTable/
 *        ResolverTypeRegistry/ResolverSymbolTable (split out of the
 *        original monolithic ResolverSymbolTable.h -- see
 *        docs/architecture/compiler_pipeline.md) all build on.
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

class FuncDefExprAST; // ResolverFunctionSymbol::Decl -- a pointer only

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
  /// True if this symbol was declared as a function parameter rather than a
  /// local/global `mut`/`immut` variable. Both go through the same
  /// declareVariable() call (a parameter is just a pre-bound local visible
  /// for the duration of the function body), so nothing else on this struct
  /// distinguishes them -- consumers that need to (e.g. the LSP's semantic
  /// tokens, which color a parameter differently from an ordinary local)
  /// previously had no way to ask. Defaults false so every existing
  /// declareVariable() call site (ordinary locals, loop vars, catch-bound
  /// error names, …) is unaffected.
  bool                   IsParameter = false;
  /// True once TypeChecker::visitVarDecl has replaced this symbol's `guess`
  /// placeholder Type with its inferred concrete type (SEM-003, §11.8).
  /// Declared here rather than derived from Type itself because by the
  /// time anything reads this flag, Type is already the concrete inferred
  /// type -- indistinguishable from an explicitly-annotated variable of the
  /// same type. Lets a later assignment type-mismatch note (visitAssignment,
  /// TypeCheckerExpr.cpp) say "'x' was inferred as 'int' here" instead of
  /// the generic "declared here" specifically for a `guess`-inferred
  /// variable, per §11.6's own worked example.
  bool                   WasGuessInferred = false;
  /// OVH-014 (docs/development/changes/OVH-014.md), FREEZE.md §6.2 row 6
  /// ("Compile-time on typed constants: error"). Set by
  /// TypeChecker::visitVarDecl, only for an `immut` local of declared type
  /// exactly `int` whose initializer TypeChecker::tryFoldUntypedIntConstant()
  /// already proved is a pure literal-arithmetic tree -- its EXACT decimal
  /// value, so a LATER reference to this local inside another
  /// literal-arithmetic expression (`immut a: int = 9223372036854775807;
  /// immut b = a + 1;`) can be folded too and its overflow caught at compile
  /// time, instead of silently deferring to the runtime-checked-arithmetic
  /// trap SEM-002 already built for the non-foldable case. A plain decimal
  /// string, not an llvm::APSInt, for the same LLVM-free reason
  /// ResolverConstantSymbol::IntVal (this file, below) is a string -- see
  /// that field's own doc comment for the exact parsing pattern
  /// (TypeChecker::tryFoldUntypedIntConstant()'s VariableExprAST case uses
  /// the identical one). Empty/false for every other symbol (mut, a
  /// non-`int` type, or an initializer that isn't pure literal arithmetic).
  bool                   HasFoldedIntConstant = false;
  std::string            FoldedIntConstant;

  VariableSymbol() = default;
  VariableSymbol(std::string Name, TurfTypePtr Type,
                 ResolverSourceLocation DeclLoc, size_t ScopeDepth,
                 bool IsImmutable = false, bool IsParameter = false)
      : Name(std::move(Name)), Type(std::move(Type)), DeclLoc(DeclLoc),
        ScopeDepth(ScopeDepth), IsImmutable(IsImmutable), IsParameter(IsParameter) {}
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
/// declarations may overload (LANG-001, ARCHITECTURE_OVERHAUL.md §13.1 --
/// supersedes the original STRING_UFCS_OPERATOR_CHAR_RFC.md §8 whitelist,
/// which additionally deferred these). Deliberately still excludes `&&`/`||`
/// (F-OP-4 -- short-circuit evaluation cannot survive a function call) and
/// every assignment form (compound assignment DESUGARS to the underlying
/// binary op instead of having its own `operator+=`, see §13.5) -- those
/// never go through operator resolution at all, so e.g. `a && b` on a struct
/// stays a plain, unambiguous type error rather than silently searching an
/// overload table that was never meant to apply to it.
inline bool isOverloadableBinaryOp(int Tok) {
  switch (Tok) {
  case '+':
  case '-':
  case '*':
  case '/':
  case '%':
  case TOK_POWER:
  case '&':
  case '|':
  case '^':
  case TOK_LSHIFT:
  case TOK_RSHIFT:
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

/// The fixed set of unary operator tokens that `operator` declarations may
/// overload (LANG-001, §13.1): `-` (negation), `!` (logical not), `~`
/// (bitwise not). Deliberately excludes unary `+` -- it has no dedicated
/// spelling distinct from binary `+` at the declaration site in a way that
/// would carry independent meaning (unary `+` is already a numeric-only
/// no-op, see TypeChecker::visitUnary()), and the frozen decision this
/// session confirmed with the user scopes LANG-001's unary set to exactly
/// these three, matching §13.1's table.
inline bool isOverloadableUnaryOp(int Tok) {
  switch (Tok) {
  case '-':
  case '!':
  case '~':
    return true;
  default:
    return false;
  }
}

/// Synthesizes the overload-table key / FuncDefExprAST name for an
/// `operator` declaration or use, e.g. '+' -> "operator+", TOK_EQ ->
/// "operator==". This can never collide with a real user identifier --
/// `operator` is a keyword token (TOK_OPERATOR) and no legal TOK_IDENTIFIER
/// spelling can contain '+ - * / = < > % * & | ^'  -- so storing operator
/// overloads in the same name-keyed overload map ordinary `fn`s use
/// (ResolverModule::Functions / TurfModule::Functions) is safe and requires
/// no new map.
///
/// Unary and binary forms of the SAME spelling (`-`) share this one name
/// deliberately -- §13.2's "key by (spelling, arity), not by inventing a
/// second name" decision. The ordinary overload map is already arity-aware
/// (ResolverFunctionSymbol::arity()), so `operator-(a: Vec2): Vec2` (unary
/// negation) and `operator-(a: Vec2, b: Vec2): Vec2` (subtraction) coexist
/// as two differently-aritied overloads of the identical name "operator-",
/// exactly the way a free function's own overloads do.
///
/// Returns "" for a token that is neither an overloadable binary nor unary
/// operator (callers should have already checked isOverloadableBinaryOp()/
/// isOverloadableUnaryOp() first).
inline std::string operatorFunctionName(int Tok) {
  switch (Tok) {
  case '+': return "operator+";
  case '-': return "operator-";
  case '*': return "operator*";
  case '/': return "operator/";
  case '%': return "operator%";
  case TOK_POWER: return "operator**";
  case '&': return "operator&";
  case '|': return "operator|";
  case '^': return "operator^";
  case TOK_LSHIFT: return "operator<<";
  case TOK_RSHIFT: return "operator>>";
  case TOK_EQ: return "operator==";
  case TOK_NEQ: return "operator!=";
  case '<': return "operator<";
  case '>': return "operator>";
  case TOK_LEQ: return "operator<=";
  case TOK_GEQ: return "operator>=";
  case '!': return "operator!";
  case '~': return "operator~";
  default: return "";
  }
}

/// Fixed key for `operator[]` (read) and `operator[]=` (write) -- these have
/// no single Lexer token to switch on (the declaration syntax is
/// `operator[](self: T, index: I): E { ... }` / `operator[]=(self: T, index:
/// I, value: E): void { ... }`, parsed as a dedicated ParseOperatorDecl()
/// branch rather than going through isOverloadableBinaryOp()'s token
/// dispatch), so these two names are synthesized directly by the Parser
/// instead of via operatorFunctionName(). Declared here so every consumer
/// (Resolver's ArrayAccessExprAST candidate gathering, TypeChecker,
/// sanitizeSymbolName()) shares the exact same literal spelling.
inline const char *OperatorIndexGetName() { return "operator[]"; }
inline const char *OperatorIndexSetName() { return "operator[]="; }

/// Maps a synthesized operator function name (e.g. "operator+",
/// "operator==") to a symbol-safe spelling for LLVM mangled names, so the
/// emitted symbol is e.g. "turf_strings_operator_plus_string_string"
/// instead of the raw "turf_strings_operator+_string_string" (which
/// contains characters no linker/assembler is guaranteed to accept in a
/// symbol name). Every non-operator name (the overwhelming majority of
/// calls) passes through completely unchanged via the map-miss fallback.
///
/// Unary vs. binary `-` (and any other shared spelling) need no separate
/// entry here: mangledName() appends every PARAMETER's own mangled type
/// after this sanitized base, so "operator_minus_Vec2" (unary) and
/// "operator_minus_Vec2_Vec2" (binary) already differ by arity alone.
inline std::string sanitizeSymbolName(const std::string &Name) {
  static const std::unordered_map<std::string, std::string> Map = {
      {"operator+", "operator_plus"},   {"operator-", "operator_minus"},
      {"operator*", "operator_star"},   {"operator/", "operator_slash"},
      {"operator%", "operator_mod"},    {"operator**", "operator_pow"},
      {"operator&", "operator_band"},   {"operator|", "operator_bor"},
      {"operator^", "operator_bxor"},   {"operator<<", "operator_shl"},
      {"operator>>", "operator_shr"},
      {"operator==", "operator_eq"},    {"operator!=", "operator_ne"},
      {"operator<", "operator_lt"},     {"operator>", "operator_gt"},
      {"operator<=", "operator_le"},    {"operator>=", "operator_ge"},
      {"operator!", "operator_not"},    {"operator~", "operator_bnot"},
      {"operator[]", "operator_index"}, {"operator[]=", "operator_index_set"},
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

  /// LEC-002 (FREEZE.md §6.15): declared `pub`/`public`. Private by default --
  /// a symbol built anywhere that forgets to set this is invisible to other
  /// modules, so a missed construction site fails loudly in the suite rather
  /// than silently leaking. Only consulted for a reference that crosses a
  /// module boundary (a same-module call is always allowed), and never for an
  /// operator: those are visible wherever their operand types are.
  bool                     IsPublic    = false;

  /// STD-002: true when this declaration was written `@caller_location
  /// fn ...` (FuncDefExprAST::hasCallerLocation()). Read by Codegen
  /// (FuncCallExprAST::codegen(), CodegenFunc.cpp) via ResolvedCallTarget to
  /// decide whether a call to this function needs one extra, hidden
  /// argument appended -- never affects arity/overload matching, since this
  /// flag intentionally has no corresponding entry in Params above (see
  /// FuncDefExprAST's own doc comment, ASTDecl.h, for why).
  bool                     HasCallerLocation = false;

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
  ///
  /// DIRECT only: it says nothing about a method that mutates by calling
  /// `self.other()`. Consumers must ask `ResolverSymbolTable::
  /// mutatesSelfTransitively(Decl)` as well -- the call graph is only known
  /// after Pass 2, by which time candidate symbols have already been copied,
  /// so the answer cannot be written back onto this flag.
  bool                     MutatesSelf = false;

  /// Candidate scope-qualifier names (`Left` in every `Left::Right` or
  /// `Left::Right(...)` shape) found among this function's own `return`
  /// statements (docs/architecture/errors_and_recover.md), computed once by
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

  /// OVH-012: true if some `return <name>;` in this function's body
  /// re-throws the bound error variable of an ENCLOSING `recover as err run
  /// { ... }` (e.g. `json::parse`'s `p.parse_value() recover as err run {
  /// return err; };`). This is unconditionally "returns a real error" --
  /// `err`'s static type is `AnyErrorType` (type-erased, §2.4), so there is
  /// no type NAME to add to PossibleErrorTypeNames above, but propagating
  /// whatever error the wrapped call produced is exactly as much "this
  /// function can fail" as naming a concrete variant would be. Computed
  /// alongside PossibleErrorTypeNames by the same
  /// CollectReturnedErrorTypeNames() scan (Resolver.cpp), which threads the
  /// innermost enclosing AsRun's BoundName down through the recursion to
  /// recognize exactly this shape (and no other bare-variable return, which
  /// stays unclassified -- consistent with this whole scan's conservative,
  /// non-interprocedural nature).
  ///
  /// LEC-004: also true when the function contains a `try` (FuncDefExprAST::
  /// UsesTry) -- the same "passes another call's error on" meaning, and the
  /// scan above cannot find one since a `try` is an expression that can sit
  /// anywhere, so the Parser records it. The bare-error check on
  /// `recover as err` (Resolver::visitRecoverExpr()) skips a callee with this
  /// set, because its PossibleErrorTypeNames is then not the whole set of
  /// errors it can fail with.
  bool ReturnsReboundError = false;

  /// TYPE-002 (S15/D6): this declaration's OWN `fn<...>` type-parameter
  /// list, in declaration order (e.g. ["T", "U"] for `fn<T, U> Wrap.pick`).
  /// Empty for a non-generic declaration. Populated by
  /// Resolver::buildFunctionSymbol() straight from
  /// FuncDefExprAST::getTypeParams() -- this symbol has no other way to see
  /// it, and TypeCheckerCall.cpp's visitMethodCall() needs it to build a
  /// Method-kind InstantiationRequest's TypeArgs in the SAME order
  /// Monomorphize.cpp's drainMethodRequests() pairs them against at drain
  /// time (`Template->getTypeParams()`, positionally). Before this field
  /// existed, a struct-generic method's own request only ever carried the
  /// RECEIVER's type arguments (e.g. just [int] for Wrap<int>), silently
  /// dropping any type parameter the METHOD declares in its own right
  /// alongside the struct's (`fn<T, U> Wrap.pick(other: U): U` -- T
  /// redeclares Wrap's own, U is pick's own) -- see D6 in the change record.
  std::vector<std::string> TypeParams;

  /// LEC-006: the declaration this symbol was built from, set by
  /// Resolver::buildFunctionSymbol(). Lets a monomorphizer request name the
  /// EXACT generic template it instantiates. `(SourceModule, BaseName)` is
  /// not enough: overloads share both, and operators overload one name
  /// constantly -- unary `-` and binary `-` are both "operator-", as are
  /// `operator+(Cell<T>, int)` and `operator+(Cell<T>, Cell<T>)`. Keyed by
  /// name alone the two collapsed into ONE instantiation whose clone served
  /// every call (found live: a call to the unary clone passed two arguments,
  /// "Incorrect number of arguments", at codegen). An ordinary
  /// `fn<T> f(x: T)` / `fn<T> f(x: T, y: T)` pair had the same bug.
  ///
  /// Never owned and never dereferenced except by the monomorphizer: the
  /// declaration lives in the pre-pass AST list for the whole compile, and a
  /// copy of this symbol (candidate lists copy it) names the same one. Null
  /// for a symbol not built from a source declaration.
  const FuncDefExprAST *Decl = nullptr;

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

  /// TYPE-002 (docs/development/changes/TYPE-002.md, S11b): the mangled
  /// name for a MONOMORPHIZED instantiation of this (generic) symbol --
  /// additive, not yet called anywhere (S12a wires it in when it first
  /// registers a real specialization). Not simply mangledName() called
  /// after substituting Params: once a generic parameter is substituted
  /// with a concrete type, Params alone is indistinguishable from a
  /// hand-written, genuinely non-generic overload with the same name and
  /// the same concrete parameter types -- e.g. `fn<T> identity(x: T): T`
  /// instantiated with T=int has Params=[int], mangling to the exact same
  /// "turf_identity_int" a hand-written `fn identity(x: int): int` would
  /// also produce. §9.5's own requirement ("a pure function of (module,
  /// base name, type arguments, parameter types)") is read literally here:
  /// the explicit type-argument tuple is embedded via a "_gen<N>_" marker
  /// no ordinary mangledName() call ever produces, so a monomorphized
  /// instantiation's symbol can never collide with an ordinary
  /// declaration's, regardless of what the substituted parameter types
  /// happen to be.
  std::string
  mangledNameForInstantiation(const std::vector<TurfTypePtr> &TypeArgs) const {
    std::string S = "turf_";
    if (!SourceModule.empty()) S += SourceModule + "_";
    S += sanitizeSymbolName(BaseName);
    S += "_gen" + std::to_string(TypeArgs.size());
    for (const auto &A : TypeArgs)
      if (A) S += "_" + A->getMangledName();
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
  //
  // IntVal is the literal's EXACT value as a decimal string (SEM-001,
  // docs/planning/ARCHITECTURE_OVERHAUL.md §8), not a `long long` -- this
  // header is explicitly, deliberately LLVM/AST-free (see the struct's own
  // doc comment just above), so it cannot hold an llvm::APSInt directly the
  // way NumberExprAST now does. A plain decimal string preserves the same
  // exactness (NumberExprAST::getIntValExact().toString(10, true), written
  // by hoistExportedConstant, ResolverCore.cpp) without pulling in LLVM --
  // CodegenConstantValue (CodegenCore.cpp), which already needs LLVM for
  // everything else it does, is what actually reconstructs an APInt from
  // this string, the same getBitsNeeded()-then-extOrTrunc(64) pattern
  // ParseNumberExpr (ParserLiteral.cpp) and NumberExprAST::codegen()
  // (CodegenExpr.cpp) both already use.
  std::string IntVal = "0";
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

/// Where a candidate overload is declared, as a location a diagnostic can show.
/// A symbol's DeclLoc is a bare line and column; for a function imported from
/// a module (`SourceModule` non-empty) that line is in the MODULE's file, and a
/// location with no file reads as a line of the file being compiled -- the
/// candidate was shown to the --smart model as an unrelated line of the program
/// (or dropped when the number was past its end). Naming the module's file makes
/// the note say where it really is (`lib/std/core/math.tr, line 24`) and lets
/// SLM::collectPinnedLines() fetch that file's text. If the module cannot be
/// resolved to a file the location still names the module, so it is never
/// mistaken for the program's own. Defined in ResolverCore.cpp.
SourceLocation candidateLocation(const ResolverFunctionSymbol &Cand);

#endif // RESOLVER_SYMBOL_BASE_H
