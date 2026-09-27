/**
 * @file TypeChecker.h
 * @brief Turflang TypeChecker pass — Issue #7 (monolith → multi-pass migration).
 *
 * Implements `class TypeChecker : public ASTVisitor`.
 *
 * Pipeline position
 * ─────────────────
 *   Parser  →  Resolver  →  **TypeChecker**  →  Codegen
 *
 * The TypeChecker runs *after* the Resolver.  It walks the AST bottom-up,
 * infers a TurfTypePtr for every expression node, and raises Type Mismatch
 * errors for illegal operations (e.g. `string + int`).
 *
 * Design constraints (Shadow Mode — Phase 2 of migration plan)
 * ─────────────────────────────────────────────────────────────
 *  - LLVM-free at the header level.  No LLVM headers are included here.
 *  - The TypeChecker does NOT replace Codegen yet; it runs in parallel and its
 *    output (inferred types stored in NodeTypes_) will gradually replace the
 *    equivalent monolithic type logic in Codegen.
 *  - Error reporting uses a pluggable callback so the class can be unit-tested
 *    without linking against the full LLVM error machinery (Errors.h pulls in
 *    LLVM via Codegen.h).  Production code installs a real TypeError callback;
 *    unit tests install a std::runtime_error thrower.
 *
 * ForExprAST prerequisite (issue #135 note)
 * ─────────────────────────────────────────
 *  The issue requires that `for` loop variables are properly typed before the
 *  TypeChecker body traversal.  This is handled entirely inside visitFor():
 *  the loop variable is inserted into LoopVarTypes_ as `int` (inferred from
 *  the integer range bounds), visible to visitVariable() during the body walk,
 *  and removed when the for-loop node is fully visited.
 *
 * Global-state dependency (despite the "LLVM-free header" constraint above)
 * ─────────────────────────────────────────────────────────────────────────
 *  visitMethodCall() consults the process-global `GlobalSymbolTable`
 *  (SymbolTable.h, populated by the Parser's ParseStructDecl()) to decide
 *  whether a call's base expression is a struct instance. It is null until
 *  `InitializeCompilerGlobals()`/`InitializeSymbolTable()` (Parser.h) runs
 *  once per process; the read site is null-guarded so this will not crash,
 *  but every instance method call silently fails to resolve instead, with a
 *  diagnostic ("only struct instances have methods") that reads exactly
 *  like a real type error in otherwise-valid source. See issue #231.
 *
 * Usage
 * ─────
 * @code
 *   InitializeCompilerGlobals();     // see Parser.h -- required once per process, before the below
 *   // 1. Build the symbol table (Resolver pass fills it).
 *   ResolverSymbolTable rst;
 *   // ... Resolver runs here ...
 *
 *   // 2. Create the TypeChecker (production — real errors).
 *   TypeChecker tc(rst);
 *
 *   // 3. Walk the AST.
 *   rootNode->accept(tc);
 *
 *   // 4. Query any node's inferred type.
 *   TurfTypePtr ty = tc.getType(someNode);
 * @endcode
 *
 * @author a2ys
 * @copyright Copyright (c) 2026 TurfLang contributors.  Apache License.
 */

#ifndef TYPE_CHECKER_H
#define TYPE_CHECKER_H

#include "ASTVisitor.h"
#include "Lexer.h" // for SourceLocation, RelatedLocation
#include "ResolverSymbolTable.h"
#include "Types.h"

#include "llvm/ADT/APSInt.h"

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Forward-declared; full definition not required here (LLVM-free header).
struct SourceLocation;

// ExprAST is the base of all AST nodes.  We only need a forward declaration
// here because TypeChecker.h stores ExprAST* as map keys and exposes getType /
// hasType queries.  The full definition lives in AST.h (which is LLVM-dependent)
// and is included only in TypeChecker.cpp.
class ExprAST;

// TYPE-002 (docs/development/changes/TYPE-002.md, S11b): forward-declared
// for the same reason as ExprAST above -- TypeChecker only ever stores a
// pointer to one and calls record(), so the full Monomorphize.h definition
// (LLVM-free, but a separate concern) doesn't need to be pulled in here.
namespace Monomorphize {
class InstantiationCollector;
}

// ─── TypeChecker ─────────────────────────────────────────────────────────────

/**
 * @brief Type-inference and type-checking pass for the Turflang compiler.
 *
 * Concrete ASTVisitor that walks the AST and annotates every expression node
 * with its inferred TurfType.  Raises a type-mismatch error if an illegal
 * binary or unary operation is encountered.
 *
 * Thread safety: NOT thread-safe.  One TypeChecker instance per compilation.
 */
class TypeChecker : public ASTVisitor {
public:
  // ── Error callback type ───────────────────────────────────────────────────
  //
  // Signature: (SourceLocation, error message string, related locations,
  // custom hint). The callback is expected to terminate the current
  // compilation path (either via longjmp / std::exit in production, or by
  // throwing in unit tests). After the callback returns the TypeChecker
  // inserts an 'unknown' type for the offending node and continues so that
  // further errors can still be found.
  //
  // CustomHint: every reportError() call used to get the exact same
  // generic TypeError hint ("Check that you're not mixing incompatible
  // types...") no matter what the message actually said -- including
  // call sites that already had a much more specific, situation-tailored
  // hint EMBEDDED IN THE MESSAGE TEXT ITSELF (the only way to say anything
  // more specific than the generic hint, before this parameter existed),
  // producing a confusing, redundant double hint. When non-empty, this
  // overrides the generic default instead of stacking alongside it.
  //
  // CustomBanner: reportError() is the single production path EVERY
  // TypeChecker diagnostic funnels through -- a duplicate `matches`
  // pattern, a non-exhaustive enum, an out-of-range cast, a missing
  // initializer, a call to something uncallable, and a genuine "int vs
  // string" mismatch all used to render under the identical hardcoded
  // "Type mismatch!" header, regardless of whether anything was actually
  // being compared/assigned between two types at all. When non-empty,
  // this overrides that header the same way CustomHint overrides the
  // default hint.
  using ErrorHandler =
      std::function<void(SourceLocation, const std::string &,
                         const std::vector<RelatedLocation> &,
                         const std::string &, const std::string &,
                         const SLM::DiagnosticHints &)>;

  // ── Constructors ──────────────────────────────────────────────────────────

  /**
   * @brief Construct with a pre-populated ResolverSymbolTable.
   *
   * Installs the *production* error handler by default, which calls
   * TypeError::raise() from Errors.h.  Override with setErrorHandler() for
   * unit tests.
   *
   * @param RST  Symbol table populated by the Resolver pass.  Not owned; must
   *             outlive this TypeChecker.
   */
  explicit TypeChecker(const ResolverSymbolTable &RST);

  /**
   * @brief Construct with a custom error handler (for unit tests / fuzz).
   *
   * Installs @p Handler instead of the default TypeError callback.
   */
  TypeChecker(const ResolverSymbolTable &RST, ErrorHandler Handler);

  // ── Error handler replacement ─────────────────────────────────────────────

  /// Replace the error handler after construction (useful in tests).
  void setErrorHandler(ErrorHandler Handler) {
    ErrorHandler_ = std::move(Handler);
  }

  /// TYPE-002 (S11b): opt in to feeding every generic call/declaration
  /// this TypeChecker resolves into an instantiation-request collector.
  /// Null (the default) is a complete no-op -- every record() call site is
  /// guarded on this being non-null, so an ordinary compile that never
  /// passes --dump-instantiations pays no cost and sees no behaviour
  /// change. @p Collector must outlive this TypeChecker.
  void setMonomorphizationCollector(Monomorphize::InstantiationCollector *Collector) {
    MonoCollector_ = Collector;
  }

  // ── Entry points ──────────────────────────────────────────────────────────

  /**
   * @brief Type-check a single top-level expression / statement.
   *
   * Accepts any ExprAST node and dispatches via the visitor pattern.
   * Mirrors Resolver::resolve() so main.cpp can call both uniformly.
   */
  void check(ExprAST *Node);

  /**
   * @brief Type-check an entire program (list of top-level nodes).
   *
   * Convenience wrapper that iterates over a vector of unique_ptr<ExprAST>
   * and calls check() on each one in order.
   * Mirrors Resolver::resolveAll(), including the RecoverPerDeclaration
   * parameter's semantics -- see that doc comment (Resolver.h) for the
   * full rationale (issue #246). Default false preserves today's exact
   * behavior for every existing caller (main.cpp's real compile pipeline,
   * which must never let a partially-checked AST reach Codegen); pass true
   * only for tooling that wants best-effort, multi-diagnostic results and
   * will never codegen the result (e.g. CompileBufferBestEffort()).
   */
  void checkAll(const std::vector<std::unique_ptr<ExprAST>> &Nodes,
               bool RecoverPerDeclaration = false);

  /**
   * @brief Reset internal per-declaration state after a longjmp recovery.
   *
   * Mirrors Resolver::resetForRecovery() (Resolver.h) -- see issue #246.
   * A TypeError raised mid-body and caught via setjmp/longjmp skips
   * whatever push/pop pairs were mid-flight (visitFuncDef()'s
   * ReturnTypeStack_ push, visitCompare()'s MachineNarrowStack_ push,
   * visitFor()'s LoopVarTypes_ insert), leaving them non-empty/stale for
   * the NEXT top-level declaration's type-checking. Does NOT clear
   * NodeTypes_ or HadError_ -- those are meant to accumulate across the
   * whole checkAll() call, not reset per-declaration.
   */
  void resetForRecovery() {
    ReturnTypeStack_.clear();
    MachineNarrowStack_.clear();
    LoopVarTypes_.clear();
    // UnsafeDepth_ (issue #238): mirrors ReturnTypeStack_/MachineNarrowStack_
    // above -- a TypeError raised mid-body inside `unsafe { }` skips
    // visitUnsafeBlock()'s own `--UnsafeDepth_` on the way out via longjmp,
    // which would otherwise leave every SUBSEQUENT top-level declaration in
    // the same file wrongly gate-exempt (or, if the error happened while
    // NOT inside unsafe but a prior recovery already left this non-zero,
    // wrongly gated) for the rest of the compile.
    UnsafeDepth_ = 0;
    // STD-002: same reasoning as UnsafeDepth_ immediately above -- a
    // TypeError raised mid-body inside a `@caller_location` function skips
    // visitFuncDef()'s own restore of InCallerLocationFunc_ on the way out
    // via longjmp.
    InCallerLocationFunc_ = false;
  }

  // ── Type query ────────────────────────────────────────────────────────────

  /**
   * @brief Return the inferred type for a node visited earlier.
   *
   * Returns nullptr if @p Node has not been visited or if inference failed.
   */
  TurfTypePtr getType(const ExprAST *Node) const;

  /**
   * @brief Return true if the given node has a recorded type.
   */
  bool hasType(const ExprAST *Node) const;

  // ── ASTVisitor overrides ──────────────────────────────────────────────────

  // ── Literals ─────────────────────────────────────────────────────────────
  void visitNumber(NumberExprAST &Node) override;
  void visitBool(BoolExprAST &Node) override;
  void visitChar(CharExprAST &Node) override;
  void visitString(StringExprAST &Node) override;
  void visitNull(NullExprAST &Node) override;

  // ── Variables & declarations ──────────────────────────────────────────────
  void visitVariable(VariableExprAST &Node) override;
  void visitVarDecl(VarDeclExprAST &Node) override;

  // ── Expressions ───────────────────────────────────────────────────────────
  void visitBinary(BinaryExprAST &Node) override;
  void visitUnary(UnaryExprAST &Node) override;
  void visitAddressOf(AddressOfExprAST &Node) override;
  void visitAssignment(AssignmentExprAST &Node) override;
  void visitCompoundAssignment(CompoundAssignmentExprAST &Node) override;
  void visitPostfixExpr(PostfixExprAST &Node) override;
  void visitCast(CastExprAST &Node) override;

  // ── Control flow ──────────────────────────────────────────────────────────
  void visitBlock(BlockExprAST &Node) override;
  void visitUnsafeBlock(UnsafeBlockExprAST &Node) override;
  void visitIf(IfExprAST &Node) override;
  void visitWhile(WhileExprAST &Node) override;
  void visitDoWhile(DoWhileExprAST &Node) override;
  void visitFor(ForExprAST &Node) override;
  void visitForEach(ForEachExprAST &Node) override;
  void visitBreak(BreakExprAST &Node) override;
  void visitContinue(ContinueExprAST &Node) override;
  void visitReturn(ReturnExprAST &Node) override;
  void visitCompare(CompareExprAST &Node) override;
  void visitDefer(DeferExprAST &Node) override;

  // ── Functions & calls ─────────────────────────────────────────────────────
  void visitFuncDef(FuncDefExprAST &Node) override;
  void visitFuncCall(FuncCallExprAST &Node) override;
  void visitMethodCall(MethodCallExprAST &Node) override;

  // ── Structs & field access ────────────────────────────────────────────────
  void visitFieldAccess(FieldAccessExprAST &Node) override;
  void visitEnumDecl(EnumDeclExprAST &Node) override;
  void visitMachineDecl(MachineDeclExprAST &Node) override;
  void visitErrorDecl(ErrorDeclExprAST &Node) override;
  void visitRecoverExpr(RecoverExprAST &Node) override;
  void visitScopeAccess(ScopeAccessExprAST &Node) override;
  void visitStructDecl(StructDeclExprAST &Node) override;

  // ── Arrays ────────────────────────────────────────────────────────────────
  void visitArrayDecl(ArrayDeclExprAST &Node) override;
  void visitArrayLiteral(ArrayLiteralExprAST &Node) override;
  void visitArrayAccess(ArrayAccessExprAST &Node) override;
  // ── Modules & misc ────────────────────────────────────────────────────────
  void visitModuleImport(ModuleImportExprAST &Node) override;
  void visitSizeOf(SizeOfExprAST &Node) override;
  void visitCallerLocation(CallerLocationExprAST &Node) override;

  // ── FFI ───────────────────────────────────────────────────────────────────
  void visitExternFuncDecl(ExternFuncDeclExprAST &Node) override;

private:
  // ── Internal helpers ──────────────────────────────────────────────────────

  /// Record the inferred type for @p Node.
  void setType(const ExprAST *Node, TurfTypePtr Ty);

  /// Fire the error handler with the given location and message.
  /// CustomHint, when non-empty, overrides the default TypeError hint
  /// instead of stacking alongside it; CustomBanner, when non-empty,
  /// overrides the default "Type mismatch!" header -- see ErrorHandler's
  /// doc comment for why that header isn't always accurate. Hints (SLM-001
  /// §16.3 item 3), when non-empty, is forwarded to the raised TypeError via
  /// TurfError::withKnownType() for each entry -- see DiagnosticHints's own
  /// doc comment (ErrorCategory.h). Empty for the overwhelming majority of
  /// call sites, which is a complete no-op, identical to before this param
  /// existed.
  void reportError(SourceLocation Loc, const std::string &Msg,
                   const std::vector<RelatedLocation> &Related = {},
                   const std::string &CustomHint = "",
                   const std::string &CustomBanner = "",
                   const SLM::DiagnosticHints &Hints = {});

  // ── `unsafe { }` gate (issue #238) ──────────────────────────────────────

  /// True for a raw, GATED pointer type -- `ptr` (OpaquePointerType) or a
  /// concrete `T*` (TurfPointerType) -- and false for everything else,
  /// including a `&T` safe reference (TurfReferenceType) and a `&?T`
  /// nullable reference (TurfNullableReferenceType, LANG-002), both of
  /// which are TurfPointerType SUBCLASSES (isPointer() is true for both)
  /// but are deliberately exempt from THIS gate -- see each class's own doc
  /// comment (Types.h) for why the inheritance is structured that way.
  /// `&?T`'s own safety is enforced by a SEPARATE mechanism entirely
  /// (NullabilityPass, Analysis.h) -- it is not "safe like &T", it is
  /// "checked instead of gated".
  static bool isRawPointer(const TurfType *T) {
    return T && T->isPointer() && !T->isReference() && !T->isNullableReference();
  }

  /// True when a raw pointer type's use at @p Loc is authorized: either
  /// lexically inside an `unsafe { }` block (UnsafeDepth_ > 0), spliced in
  /// from a TOP-LEVEL `unsafe { }` block (Parser.h's
  /// WasDeclaredInsideTopLevelUnsafe() -- see AppendFlattenedTopLevelDecl()'s
  /// doc comment for why top-level unsafe-wrapping can't just be
  /// UnsafeDepth_), or declared inside `lib/std` (the whole tree is exempt
  /// from this gate -- see FINAL_SPRINT.md's Track A "Resolved" section).
  /// @p Node is optional -- pass it whenever available so the
  /// WasDeclaredInsideTopLevelUnsafe() check can run; omit it (nullptr) for
  /// call sites checking a SUB-part of a node (e.g. a cast operand) that
  /// isn't itself the flattened top-level node.
  bool isUnsafeGateExempt(const SourceLocation &Loc,
                          const ExprAST *Node = nullptr) const;

  /// Raises a "raw pointer used outside unsafe" diagnostic if @p T is a raw
  /// pointer type and its use at @p Loc isn't gate-exempt. Returns true if
  /// it raised (so callers that want to bail out early can check).
  bool checkUnsafeGate(const TurfType *T, SourceLocation Loc,
                       const ExprAST *Node = nullptr);

  /// Unconditional (never fixable by `unsafe { }`) check for an ordinary
  /// (non-extern) function's parameter/return types, applied in
  /// visitFuncDef(): raw pointer types are disallowed outright in a plain
  /// function's signature outside `lib/std` -- see FINAL_SPRINT.md's Track A
  /// addendum. `extern fn` goes through visitExternFuncDecl() instead and
  /// is unaffected by this check.
  void checkNoRawPointersInSignature(const FuncDefExprAST &Node);

  /**
   * @brief Infer the result type of a binary arithmetic/string operation.
   *
   * Rules (in priority order):
   *  1. string OP string  →  string  (only for '+')
   *  2. string OP numeric →  TypeError
   *  3. numeric OP string →  TypeError
   *  4. double OP double  →  double
   *  5. int    OP int     →  int
   *  6. int    OP double  →  double  (numeric promotion)
   *  7. double OP int     →  double  (numeric promotion)
   *  8. bool   OP bool    →  int     (bool promotes to int for arithmetic)
   *
   * @param Loc        Source location for error reporting.
   * @param Op         Operator character ('+', '-', '*', '/', '%').
   * @param LhsTy      Inferred LHS type (may be nullptr → treated as unknown).
   * @param RhsTy      Inferred RHS type (may be nullptr → treated as unknown).
   * @param ResultOut  Populated with the result type on success.
   * @return true if inference succeeded without errors.
   */
  bool inferArithmeticType(SourceLocation Loc, int Op,
                           const TurfType *LhsTy, const TurfType *RhsTy,
                           TurfTypePtr &ResultOut);

  /**
   * @brief Infer the result type of a comparison or logical operator.
   *
   * Comparison operators (< > <= >= == !=) and logical operators (&& ||)
   * always produce bool, provided both operands are of compatible types.
   *
   * @return Always TurfBool() on success; fires an error and returns nullptr
   *         on mismatch.
   */
  TurfTypePtr inferRelationalType(SourceLocation Loc, int Op,
                                  const TurfType *LhsTy,
                                  const TurfType *RhsTy);

  /**
   * @brief UFCS fallback (STRING_UFCS_OPERATOR_CHAR_RFC.md §5): try to
   * resolve a dot-call as an ordinary free function taking the receiver as
   * its first argument, when the bound-method tier (struct methods, or "no
   * bound-method mechanism at all" for primitives) has already produced
   * zero matches for this exact call.
   *
   * Receiver matching is EXACT-name-match only (no numeric coercion) --
   * RFC §5.4. The remaining, explicitly-written call arguments use the
   * ordinary typesCompatible() coercion rules, same as any other function
   * call -- §5.4's "no coercion" language is scoped to the implicit
   * receiver binding specifically, not the whole call.
   *
   * On success: sets Node.ResolvedCallTarget + Node.ResolvedViaUFCS and the
   * node's type, then returns true. On failure (no UFCS candidate matches
   * either): returns false and reports NOTHING -- the caller is expected to
   * raise whatever tier-1 diagnostic it already would have, so UFCS never
   * changes an error message when it doesn't apply.
   *
   * @param BaseTy  The receiver's type, resolved exactly as far as the
   *                caller had already unwrapped it (pointer/NamedType/
   *                GenericType for a struct receiver; the primitive type
   *                itself otherwise).
   * @param OutNamedCandidates  When non-null, always populated (even on
   *                failure) with every visible function sharing the call's
   *                method name, regardless of arity/receiver-type match --
   *                lets a caller whose only remaining fallback is "no
   *                bound-method mechanism at all" (the primitive-receiver
   *                path) distinguish "no such function exists anywhere" from
   *                "it exists, but no overload's receiver/arity matches this
   *                call" and report the latter with an accurate, specific
   *                message instead of a generic "no such method" one.
   */
  bool tryUFCSFallback(MethodCallExprAST &Node, const TurfType *BaseTy,
                       const std::vector<TurfTypePtr> &ArgTypes,
                       std::vector<const ResolverFunctionSymbol *> *OutNamedCandidates = nullptr);

  /// True if @p T is a numeric primitive (int, double, or bool).
  static bool isNumericType(const TurfType *T);

  /// True if two types are compatible for arithmetic (both numeric or both
  /// string — string + string is valid; mixed is not).
  static bool areArithCompatible(const TurfType *A, const TurfType *B);

  // ── SEM-002: literal-representability checking ─────────────────────────
  // (docs/planning/ARCHITECTURE_OVERHAUL.md §8, FREEZE.md §6.2 rows 3/6/9)

  /// Recursive fold of a compile-time integer constant built ONLY from
  /// untyped literals via unary '-' and binary '+'/'-'/'*' (row 6).
  /// Returns std::nullopt for anything else (a variable, a call, '/'/'%'/
  /// '**', a typed constant reference, ...) without raising anything --
  /// those are left to runtime overflow trapping instead (Codegen), or,
  /// for a typed-constant reference specifically, are a disclosed,
  /// deferred gap (see this group's change record).
  ///
  /// @p IsTopLevel is true only for the ORIGINAL call (from
  /// checkLiteralRepresentable()) and propagates through a unary '-'
  /// (folding "-BIG" is still conceptually the same single literal, just
  /// negated -- FREEZE.md's own `-9223372036854775808` example), but is
  /// always false for a BinaryExprAST's own LHS/RHS: those become genuine
  /// independent leaves that Codegen (NumberExprAST::codegen(),
  /// unconditionally since SEM-001) must truncate to 64 bits BEFORE any
  /// arithmetic runs, so a leaf that doesn't independently fit int64 is
  /// rejected immediately, from here, citing 'int' -- deferring to "maybe
  /// the whole expression's final value still fits" is a check Codegen
  /// could not actually honor for that leaf regardless of what the
  /// surrounding expression's value turns out to be. Only a bare
  /// top-level literal (this function called directly on a NumberExprAST)
  /// defers instead to checkLiteralRepresentable()'s own
  /// destination-aware message.
  std::optional<llvm::APSInt> tryFoldUntypedIntConstant(const ExprAST *E,
                                                         bool IsTopLevel);

  /// Checks @p ValueExpr (an untyped-literal-only expression, per
  /// tryFoldUntypedIntConstant() above) against @p DestType's
  /// representable range, when @p DestType is 'int', 'int32', or 'double'
  /// -- a no-op for every other destination (bool/char/string/struct/
  /// enum/pointer either can't receive a NumberExprAST-shaped initializer
  /// at all, or has no numeric range to violate) and a no-op when
  /// @p ValueExpr isn't a pure literal-arithmetic tree.
  ///
  /// The 'double' destination case is narrower than FREEZE.md §6.2 row 9's
  /// literal wording ("must be exactly representable"): this requires the
  /// exact value to ALSO independently fit int64 first, because Codegen
  /// (NumberExprAST::codegen()) truncates every literal through a 64-bit
  /// int before any int-to-double conversion runs, regardless of the
  /// literal's true magnitude -- a literal that's exactly representable
  /// as a double but exceeds int64 (e.g. 2^100) would silently codegen a
  /// WRONG value today if this check let it through. A disclosed, narrow
  /// implementation constraint, not a reinterpretation of the frozen rule
  /// -- see this group's change record's Known pitfalls.
  void checkLiteralRepresentable(const ExprAST *ValueExpr, const TurfType *DestType,
                                 SourceLocation Loc);

  /// Applies checkLiteralRepresentable() to each argument against its
  /// matching resolved parameter's type, once a call's final candidate is
  /// chosen. @p Loc is the call's own location -- reused for every argument
  /// since ExprAST has no virtual getLoc() (see docs/architecture/
  /// compiler_pipeline.md §6's identical note for CFG diagnostics), the
  /// same convention visitArrayDecl() already uses for its own per-element
  /// mismatch diagnostic. @p ParamOffset skips a UFCS candidate's own
  /// implicit receiver slot (Params[0]), whose type never corresponds to
  /// any entry in @p Args at all (tryUFCSFallback()'s own call site).
  void checkCallArgsRepresentable(const std::vector<std::unique_ptr<ExprAST>> &Args,
                                  const std::vector<FunctionParam> &Params,
                                  SourceLocation Loc, size_t ParamOffset = 0);

  /// OVH-005 (FREEZE.md §6.2 row 3, F-NUM-1): true when @p Dest is `int32`
  /// and @p Val is exactly the kind of untyped-literal-only expression
  /// tryFoldUntypedIntConstant() folds (a bare literal, or +/-/* of pure
  /// literals). `typesCompatible()` (Types.cpp) sees such a value as plain
  /// `int` -- visitNumber() always types an integer literal `int`, with no
  /// way to record "still untyped" in a TurfTypePtr -- so it correctly
  /// rejects `int -> int32` as the narrowing conversion that IS for a
  /// genuinely `int`-typed value, but wrongly rejects it for a literal that
  /// was never actually typed `int` to begin with and should acquire
  /// `int32` directly instead, exactly like `guess`'s own literal-
  /// defaulting rule (§6.3) or the existing 'double' destination case
  /// (checkLiteralRepresentable() above). Every type-fixing call site
  /// (VarDecl init, assignment, argument, return) checks this explicitly
  /// alongside typesCompatible() before treating a mismatch as real, since
  /// typesCompatible() itself has no ExprAST access to tell the two apart.
  bool untypedLiteralAcquiresInt32(const ExprAST *Val, const TurfType *Dest);

  // ── State ─────────────────────────────────────────────────────────────────

  /// Reference to the symbol table filled by the Resolver.
  const ResolverSymbolTable &RST_;

  /// Map from AST node pointer to its inferred type.
  /// Pointer stability: we only ever store nodes whose lifetime is managed
  /// by the AST owning the unique_ptrs, so raw pointers are safe here.
  std::unordered_map<const ExprAST *, TurfTypePtr> NodeTypes_;

  /// Pluggable error reporter (default = TypeError::raise()).
  ErrorHandler ErrorHandler_;

  /// TYPE-002 (S11b): null unless setMonomorphizationCollector() was
  /// called. See that method's doc comment.
  Monomorphize::InstantiationCollector *MonoCollector_ = nullptr;

  /// Stack of expected return types for nested function definitions.
  /// Empty when we are at the top level.
  std::vector<TurfTypePtr> ReturnTypeStack_;

  /// One frame per enclosing `compare` arm that narrows a TurfMachineType-
  /// typed, plain-variable subject to exactly one specific state -- pushed
  /// by visitCompare() right before checking that arm's body, popped right
  /// after. visitMethodCall()'s machine-transition branch searches this
  /// (innermost first) by receiver variable name to decide whether a
  /// `.transition()` call can be proven safe at compile time
  /// (TransitionCompileTimeProven) instead of falling back to Codegen's
  /// runtime switch+panic guard. Only ever holds an entry for a
  /// single-pattern arm (`matches State:`) on a bare variable subject --
  /// a stacked-pattern arm (`matches A, matches B:`) or a non-variable
  /// subject (e.g. a field access) is a v1 scope limit and is left
  /// unnarrowed, per polymorphic-seeking-lark.md.
  struct MachineNarrowFrame {
    std::string VarName;
    std::string MachineName;
    std::string StateName;
  };
  std::vector<MachineNarrowFrame> MachineNarrowStack_;

  /// Loop variable types: populated by visitFor(), consulted by visitVariable().
  /// Maps loop variable name → inferred type (always int for range-for).
  std::unordered_map<std::string, TurfTypePtr> LoopVarTypes_;

  /// Nesting depth of `unsafe { }` blocks (issue #238) -- an int counter,
  /// not a bool, so `unsafe { unsafe { } }` (silly but harmless) stays
  /// authorized all the way through, and so a stray extra pop can never
  /// underflow past zero. Mirrors Resolver::FunctionNestingDepth_
  /// (Resolver.h) -- incremented/decremented bracketing visitUnsafeBlock()'s
  /// body traversal. Consulted by isUnsafeGateExempt().
  int UnsafeDepth_ = 0;

  /// STD-002: true while visiting the body of a function declared
  /// `@caller_location`. A plain bool, not a counter like UnsafeDepth_ --
  /// this language has no nested `fn` bodies visited WHILE another one is
  /// still open the way `unsafe { unsafe { } }` nests (a block-nested `fn`,
  /// see docs/architecture/pointers_and_references.md §2.2's "top-level
  /// unsafe struct" note for the general shape, is still a SEPARATE
  /// visitFuncDef() call with its own save/restore bracket, never entered
  /// while the outer one's bracket is still on the call stack). Consulted
  /// by visitCallerLocation(); save/restored around visitFuncDef()'s body
  /// traversal exactly like UnsafeDepth_ is for visitUnsafeBlock().
  bool InCallerLocationFunc_ = false;

  /// Whether we had at least one error during this pass.
  bool HadError_ = false;
};

#endif // TYPE_CHECKER_H
