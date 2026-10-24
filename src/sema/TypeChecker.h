/**
 * @file TypeChecker.h
 * @brief Type-inference and type-checking pass.
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
 * Design constraints
 * ──────────────────
 *  - No Codegen or LLVM IR header is included here; the only LLVM include is
 *    llvm/ADT/APSInt.h, for literal folding.
 *  - Inferred types are stored in NodeTypes_ (read back with getType()); the
 *    pass also writes some results onto nodes (e.g. resolved call targets)
 *    for Codegen.
 *  - Error reporting uses a pluggable ErrorHandler callback.  The default
 *    handler raises a TypeError (Errors.h); a test can install one that
 *    throws instead.
 *
 * For-loop variables
 * ──────────────────
 *  visitFor() types the loop variable (`int`, or `double` when a range bound
 *  is a double) and inserts it into LoopVarTypes_, where visitVariable()
 *  finds it during the body walk.  It is removed when the for-loop node is
 *  fully visited.
 *
 * Global-state dependency
 * ───────────────────────
 *  Struct lookups (visitMethodCall() among them) consult the process-global
 *  `GlobalSymbolTable` (SymbolTable.h, populated by the Parser's
 *  ParseStructDecl()) to decide whether a call's base expression is a struct
 *  instance. It is null until `InitializeCompilerGlobals()`/
 *  `InitializeSymbolTable()` (Parser.h) runs once per process; the read sites
 *  are null-guarded so this will not crash, but every instance method call
 *  silently fails to resolve instead, with a diagnostic ("only struct
 *  instances have methods") that reads exactly like a real type error in
 *  otherwise-valid source.
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

#include "frontend/ASTVisitor.h"
#include "frontend/Lexer.h" // for SourceLocation, RelatedLocation
#include "resolve/ResolverSymbolTable.h"
#include "sema/Types.h"
#include "sema/TypeWalk.h"

#include "llvm/ADT/APSInt.h"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Forward-declared; the full definition is not needed here.
struct SourceLocation;

// ExprAST is the base of all AST nodes.  Only a forward declaration is needed
// here because TypeChecker.h stores ExprAST* as map keys and exposes getType /
// hasType queries.  The full definition lives in AST.h, which only the .cpp
// files include.
class ExprAST;
class ArrayAccessExprAST;

// Forward-declared for the same reason as ExprAST above -- TypeChecker only
// ever stores a pointer to one and calls record(), so the full Monomorphize.h
// definition doesn't need to be pulled in here.
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
  // Signature: (SourceLocation, error code, message, related locations,
  // custom hint, custom banner, diagnostic hints). The production handler
  // raises a TypeError, which unwinds to the innermost recovery point
  // (RecoverUnwind, Lexer.h) or exits the process, so it does not return; a
  // test handler may throw instead. If a handler does return, the
  // TypeChecker carries on so that further errors can still be found.
  //
  // CustomHint: when non-empty, replaces the generic TypeError hint instead
  // of stacking alongside it, so a site can give situation-specific advice
  // without embedding it in the message text.
  //
  // CustomBanner: reportError() is the single production path EVERY
  // TypeChecker diagnostic funnels through, and not all of them are a
  // mismatch between two types (a duplicate `matches` pattern, a
  // non-exhaustive enum, an out-of-range cast, a missing initializer, a call
  // to something uncallable). When non-empty, this replaces the default
  // "Type mismatch!" header the same way CustomHint replaces the default hint.
  //
  // Code: the ErrCode:: (ErrorCodes.h) naming WHICH mistake this is, so a
  // wrong argument count, a missing initializer and a raw pointer outside
  // `unsafe` are not all reported as "type mismatch". It is the second
  // parameter of every report, never defaulted, so a new type error cannot be
  // filed without deciding which mistake it is.
  using ErrorHandler =
      std::function<void(SourceLocation, const char *, const std::string &,
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

  /// Tell this checker which module the code it is about to check
  /// was written in ("" = the main file). Needed only for a monomorphized
  /// clone, which the Monomorphizer type-checks in a FRESH TypeChecker after
  /// clearing the clone's own `SourceModule` (a mangling requirement): the
  /// clone is the template's code, so it must see the template's private
  /// fields and helpers, whichever module asked for the instantiation.
  void setCurrentModule(std::string Module) { CurrentModule_ = std::move(Module); }

  // ── Error handler replacement ─────────────────────────────────────────────

  /// Replace the error handler after construction (useful in tests).
  void setErrorHandler(ErrorHandler Handler) {
    ErrorHandler_ = std::move(Handler);
  }

  /// Opt in to feeding every generic call/declaration this TypeChecker
  /// resolves into an instantiation-request collector. Null (the default) is
  /// a complete no-op -- every record() call site is guarded on this being
  /// non-null, so a checker that is not collecting pays no cost. @p Collector
  /// must outlive this TypeChecker.
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
   * parameter's semantics -- see that doc comment (Resolver.h) for the full
   * rationale. The default (false) arms no recovery here, which is what the
   * real compile pipeline (main.cpp) needs: it must never let a
   * partially-checked AST reach Codegen. Pass true only for tooling that
   * wants best-effort, multi-diagnostic results and will never codegen the
   * result (e.g. turf::analyze()).
   */
  void checkAll(const std::vector<std::unique_ptr<ExprAST>> &Nodes,
               bool RecoverPerDeclaration = false);

  /**
   * @brief Reset internal per-declaration state after a recovered error.
   *
   * Mirrors Resolver::resetForRecovery() (Resolver.h). A TypeError raised
   * mid-body and caught at a recovery point (RecoverUnwind, Lexer.h) skips
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
    // UnsafeDepth_: mirrors ReturnTypeStack_/MachineNarrowStack_ above -- a
    // TypeError raised mid-body inside `unsafe { }` skips
    // visitUnsafeBlock()'s own `--UnsafeDepth_` on the way out, which would
    // otherwise leave every SUBSEQUENT top-level declaration in the same file
    // wrongly gate-exempt for the rest of the compile.
    UnsafeDepth_ = 0;
    // Same reasoning as UnsafeDepth_ immediately above -- a TypeError raised
    // mid-body inside a `@caller_location` function skips visitFuncDef()'s
    // own restore of InCallerLocationFunc_ on the way out.
    InCallerLocationFunc_ = false;
    CurrentFuncName_.clear(); // same reasoning, visitFuncDef()'s restore
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
  /// E081/E082 for a deferred statement that reads through a `&?T` variable
  /// (a field, a method, a cast to `&T`) which a later statement of the same
  /// block may set to null. NullabilityPass checks a deferred statement where
  /// it is written, but it runs when the block exits: in
  /// `while (w != null && ..) { defer use(w.val); w = w.next; }` the check that
  /// covered `w.val` at the `defer` no longer holds when it runs, so a null
  /// could be dereferenced in safe code. Assigning `&x` or a `&T` value is fine; a later
  /// `defer` counts too (it runs first).
  void rejectDeferredNullableUse(const BlockExprAST &Block);
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
  /// Resolution proper; visitFuncCall() applies an LG-1 written type
  /// (`List<int>::default()`) to its result.
  void visitFuncCallImpl(FuncCallExprAST &Node);
  /// LG-1: `Name<Args>::default()` / `Name<Args>::new(...)` is the call with
  /// `Name<Args>` as the type it builds, as if stored into a declaration of
  /// that type.
  void applyWrittenConstructedType(FuncCallExprAST &Node);
  /// E129 for a `guess` declaration whose initializer is a generic struct
  /// with no type arguments; with a fix when exactly one set fits (LG-7).
  void reportNothingToInferGeneric(const VarDeclExprAST &Node);
  void visitMethodCall(MethodCallExprAST &Node) override;
  /// Resolution proper; visitMethodCall() adds the check that needs its result.
  void visitMethodCallImpl(MethodCallExprAST &Node);

  /// Reports E081 when @p Base, of type @p BaseTy, is a `&?T` that is not a
  /// plain variable and @p Member (a field, or a method if @p IsMethod) is
  /// reached through it: `p.next.val`, `get().val`, `list[i].val`.
  /// NullabilityPass narrows variables only, by design, so it cannot cover any
  /// other `&?T` base; without this check a null one is dereferenced
  /// unchecked: it faults, or reads garbage once optimized (FREEZE §6.6: a
  /// `&?T` "must be null-tested before use"). A variable base is left to that
  /// pass. Returns true if it reported.
  bool rejectNullableNonVariableUse(const ExprAST *Base, const TurfType *BaseTy,
                                    SourceLocation Loc, const std::string &Member,
                                    bool IsMethod);

  // ── Structs & field access ────────────────────────────────────────────────
  void visitFieldAccess(FieldAccessExprAST &Node) override;
  void visitEnumDecl(EnumDeclExprAST &Node) override;
  void visitMachineDecl(MachineDeclExprAST &Node) override;
  void visitErrorDecl(ErrorDeclExprAST &Node) override;
  void visitRecoverExpr(RecoverExprAST &Node) override;
  void visitTry(TryExprAST &Node) override;
  void visitScopeAccess(ScopeAccessExprAST &Node) override;
  void visitStructDecl(StructDeclExprAST &Node) override;

  // ── Arrays ────────────────────────────────────────────────────────────────
  void visitArrayDecl(ArrayDeclExprAST &Node) override;
  void visitArrayLiteral(ArrayLiteralExprAST &Node) override;
  void visitArrayAccess(ArrayAccessExprAST &Node) override;
  void visitSlice(SliceExprAST &Node) override;
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
  /// doc comment for why that header isn't always accurate. Hints, when
  /// non-empty, is forwarded to the raised TypeError via
  /// TurfError::withKnownType() for each entry -- see DiagnosticHints's own
  /// doc comment (ErrorCategory.h). Empty for the overwhelming majority of
  /// call sites, which is a complete no-op.
  void reportError(SourceLocation Loc, const char *Code, const std::string &Msg,
                   const std::vector<RelatedLocation> &Related = {},
                   const std::string &CustomHint = "",
                   const std::string &CustomBanner = "",
                   const SLM::DiagnosticHints &Hints = {});

  /// Visits @p Cond and reports E066 at @p Loc unless it is a `bool`.
  /// Conditions have no truthiness: Codegen branches on an `i1` and has no
  /// conversion for any other type. @p Construct names the statement ("if").
  void checkCondition(ExprAST *Cond, SourceLocation Loc, const char *Construct);

  /// A value of type `void` used where a value is needed -- initialising,
  /// assigning, returning, an operand -- is its own mistake (E067), not "type
  /// mismatch": there is no type to convert, there is no value at all. Every
  /// mismatch site that has both types passes them here so a void on either
  /// side picks E067 and the hint that says what to do about it; otherwise
  /// the caller's own code and hint stand.
  static const char *codeForMismatch(const TurfType *A, const TurfType *B,
                                     const char *Default);

  /// The declaration of the struct @p T as the model should see it
  /// (DiagnosticHints::ReceiverDecl) -- `struct Box<int> { v: int, n: int }`,
  /// built from the type's own name and fields, so a generic instantiation
  /// shows the concrete field types. "" when @p T is not a struct.
  static std::string describeStruct(const TurfType *T);

  /// Adds `name: type` to @p Hints.KnownTypes for each
  /// variable under @p E whose type is known and that no existing fact already
  /// describes -- the type the TYPE CHECKER settled on, which for an
  /// identifier declared far from the error (or with `guess`) is not on any
  /// line the model is shown. Call it at a raise site with the expression the
  /// message is about, before reportError(). Bounded (6 facts per diagnostic).
  void addIdentifierTypes(const ExprAST *E, SLM::DiagnosticHints &Hints);
  /// The hint for a value of type @p A where another type is needed. @p Dest,
  /// when given, is that type: a reference to a @p Dest (`&P` where a `P`
  /// belongs) gets the explicit copy, `x to P`.
  static std::string hintForMismatch(const TurfType *A, const TurfType *B,
                                     const std::string &Default,
                                     const TurfType *Dest = nullptr);


  // ── `unsafe { }` gate ───────────────────────────────────────────────────

  /// RangeNeverEndsWarning for a `for` whose constant bounds and step move
  /// the loop variable away from its end (TypeCheckerStmt.cpp).
  void warnIfRangeNeverEnds(const ForExprAST &Node, const TurfType *StartTy,
                            const TurfType *EndTy);

  /// A template's own type parameter, unsubstituted (`T` itself, not
  /// `List<T>` or `&T`). An operation on one is checked per instantiation
  /// (FREEZE §6.1), never against the template.
  static bool isOpenTypeParam(const TurfType *T) {
    return T && TypeWalk::containsOpenParam(T, TypeWalk::ParamScope::Bare);
  }

  /// True for a raw, GATED pointer type -- `ptr` (OpaquePointerType) or a
  /// concrete `T*` (TurfPointerType) -- and false for everything else,
  /// including a `&T` safe reference (TurfReferenceType) and a `&?T`
  /// nullable reference (TurfNullableReferenceType), both of which are
  /// TurfPointerType SUBCLASSES (isPointer() is true for both) but are
  /// deliberately exempt from THIS gate -- see each class's own doc comment
  /// (Types.h) for why the inheritance is structured that way. `&?T`'s own
  /// safety is enforced by a SEPARATE mechanism entirely (NullabilityPass,
  /// Analysis.h) -- it is not "safe like &T", it is "checked instead of
  /// gated".
  static bool isRawPointer(const TurfType *T) {
    return T && T->isPointer() && !T->isReference() && !T->isNullableReference();
  }

  /// True when a raw pointer type's use at @p Loc is authorized: either
  /// lexically inside an `unsafe { }` block (UnsafeDepth_ > 0), spliced in
  /// from a TOP-LEVEL `unsafe { }` block (Parser.h's
  /// WasDeclaredInsideTopLevelUnsafe() -- see AppendFlattenedTopLevelDecl()'s
  /// doc comment for why top-level unsafe-wrapping can't just be
  /// UnsafeDepth_), or located in a file that declared `unsafe module;`
  /// (HasRawPointerCapability(), Parser.h).
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
  /// function's signature unless the file declared `unsafe module;`.
  /// `extern fn` goes through visitExternFuncDecl() instead and
  /// is unaffected by this check.
  void checkNoRawPointersInSignature(const FuncDefExprAST &Node);

  /// E140: a `T[]` is a slice that borrows its caller's array
  /// for the length of one call, so the only place it may appear is as a
  /// function parameter's OWN type (@p AllowAtTop). Anywhere else -- a
  /// variable, a field, a return type, an element type (`int[][]`'s inner
  /// one), behind `&`, as a generic type argument, inside a function type --
  /// it would either outlive the array it points into or need a length
  /// nothing stores. Reports the first such use inside @p T against @p Loc;
  /// @p What names the place for the message ("variable 'x'"). Returns true
  /// if it reported.
  bool checkUnsizedArrayPlacement(const TurfType *T, bool AllowAtTop,
                                  SourceLocation Loc, const std::string &What,
                                  const std::string &Hint);

  /// A slice bound's value when it folds to a constant, else nullopt.
  std::optional<long long> constantSliceBound(const ExprAST *E);

  /// E144: @p Source (an array expression) is about to become a `mut` view
  /// named @p ViewName -- refuse it when the array it is rooted at is
  /// immutable, since writes through the view would change that array.
  /// Returns true if it reported.
  bool checkMutableViewSource(const ExprAST *Source, SourceLocation Loc,
                              const std::string &ViewName);

  /// A generic call whose arguments bind one type parameter to two different
  /// types (`fn<T> pair(a: T, b: T)` called as `pair(1, "a")`) is E056.
  /// unify() keeps the first binding and skips the rest, so without this the
  /// clone takes the first type and the other argument is passed unconverted.
  /// @p Patterns are the declared parameter types, parallel to @p ArgTypes.
  bool checkConsistentTypeArguments(const std::vector<TurfTypePtr> &Patterns,
                                    const std::vector<TurfTypePtr> &ArgTypes,
                                    const std::string &CalleeName,
                                    SourceLocation Loc);

  /// E140 for a generic call: no type parameter may stand for (or contain) a
  /// `T[]`, since a `T`-typed value can be stored or returned. Passing a
  /// slice to `fn<T> f(arr: T[])` binds T to the ELEMENT type, which is fine.
  void checkSliceTypeArguments(
      const std::unordered_map<std::string, TurfTypePtr> &Subst,
      const std::string &CalleeName, SourceLocation Loc);

  /// E141: records every argument a call hands to a `T[]`
  /// parameter of @p Target, so checkSliceWrites() can refuse the call if the
  /// argument is an `immut` array and the callee writes through that slice.
  /// @p Receiver is a UFCS receiver (Params[0]); @p ParamOffset is where
  /// @p Args start in Target.Params.
  void noteSliceArguments(const std::vector<std::unique_ptr<ExprAST>> &Args,
                          const ExprAST *Receiver,
                          const std::shared_ptr<ResolverFunctionSymbol> &Target,
                          size_t ParamOffset, SourceLocation Loc);

  /// Runs the checks noteSliceArguments() recorded. Deferred to the end of
  /// checkAll(): whether a callee writes through a slice depends on the calls
  /// in ITS body (it may hand the slice on), and those are only resolved once
  /// that body has been checked.
  void checkSliceWrites();

public:
  /// Does the body of @p Decl write through its @p ParamIdx-th parameter (a
  /// `T[]`), directly or by passing it to another function that does?
  bool writesThroughSliceParam(const FuncDefExprAST *Decl, size_t ParamIdx);
  /// writesThroughSliceParam() for a call target's @p ParamIdx-th parameter
  /// (Target.Params and the declaration's own list differ for a constructor).
  bool targetWritesThroughSlice(const ResolverFunctionSymbol &Target,
                                size_t ParamIdx);
  bool mutatesSelfTransitively(const FuncDefExprAST *Decl) const;
  /// True when a value of type @p Src is a reference (or `self`'s pointer) to
  /// a @p Dest, so `x to Dest` copies the value out of it.
  static bool isCopyOut(const TurfType *Src, const TurfType *Dest);

private:
  struct PendingSliceWrite {
    SourceLocation Loc;
    const VariableSymbol *Array; ///< the immutable array handed over
    std::string ArrayName;
    std::shared_ptr<ResolverFunctionSymbol> Callee;
    size_t ParamIdx;
  };
  std::vector<PendingSliceWrite> PendingSliceWrites_;
  /// Method calls on an `immut` receiver whose method doesn't mutate self
  /// directly; checkSliceWrites() refuses those that write self through a
  /// slice (ParamIdx is unused: always `self`).
  std::vector<PendingSliceWrite> PendingSelfSliceWrites_;
  /// writesThroughSliceParam() memo: 0 = in progress (a recursive cycle counts
  /// as "no write" unless some other path writes), 1 = no, 2 = yes.
  std::map<std::pair<const FuncDefExprAST *, size_t>, int> SliceWriteMemo_;

  /**
   * @brief Infer the result type of a binary arithmetic operation.
   *
   * Rules:
   *  1. numeric OP numeric (int, int32, double)  →  double if either side is
   *     a double, else int.
   *  2. Anything else (string, bool, char, struct, ...)  →  TypeError.
   *
   * Operator overloads, string '+' included, are resolved in visitBinary()
   * before this is called, so a string operand that reaches here has no
   * matching overload.
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
   * @return Always TurfBool(). A mismatch is reported first; the result stays
   *         bool so a handler that returns does not cascade errors.
   */
  TurfTypePtr inferRelationalType(SourceLocation Loc, int Op,
                                  const TurfType *LhsTy,
                                  const TurfType *RhsTy);

  /**
   * @brief UFCS fallback: try to
   * resolve a dot-call as an ordinary free function taking the receiver as
   * its first argument, when the bound-method tier (struct methods, or "no
   * bound-method mechanism at all" for primitives) has already produced
   * zero matches for this exact call.
   *
   * Receiver matching is EXACT-name-match only (no numeric coercion). The
   * remaining, explicitly-written call arguments use the ordinary
   * typesCompatible() coercion rules, same as any other function call; "no
   * coercion" applies to the implicit receiver binding specifically, not the
   * whole call.
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
  bool tryUFCSFallback(MethodCallExprAST &Node, const TurfTypePtr &BaseTy,
                       const std::vector<TurfTypePtr> &ArgTypes,
                       std::vector<const ResolverFunctionSymbol *> *OutNamedCandidates = nullptr);

  /**
   * tryUFCSFallback() for a receiver that may be a pointer. visitMethodCall()
   * unwraps `T*` to `T` before looking for bound methods (so `self.m()` finds
   * `T`'s methods), but UFCS matches the receiver as WRITTEN first: `p.free()`
   * with `p: void*` is `free(p)`. Only when that fails and the pointer points
   * at a struct is the struct itself tried (`self.f()` inside a method, for a
   * `fn f(t: T)`), with Node.UFCSLoadsReceiver set so Codegen passes the
   * value, not the pointer. A pointer to anything else is never loaded
   * implicitly.
   *
   * @param ReceiverTy  The receiver's type before any unwrapping.
   * @param BaseTy      What visitMethodCall() unwrapped it to.
   */
  bool tryUFCSOnReceiver(MethodCallExprAST &Node, const TurfTypePtr &ReceiverTy,
                         const TurfTypePtr &BaseTy,
                         const std::vector<TurfTypePtr> &ArgTypes,
                         std::vector<const ResolverFunctionSymbol *> *OutNamedCandidates = nullptr);

  /// True if @p T is a numeric primitive (int, int32 or double; not bool or char).
  static bool isNumericType(const TurfType *T);

  /// True if two types are compatible for arithmetic (both numeric or both
  /// string — string + string is valid; mixed is not).
  static bool areArithCompatible(const TurfType *A, const TurfType *B);

  // ── Literal-representability checking ──────────────────────────────────
  // (FREEZE.md §6.2 rows 3/6/9)

  /// Recursive fold of a compile-time integer constant built ONLY from
  /// untyped literals via unary '-' and binary '+'/'-'/'*' (row 6).
  /// Returns std::nullopt for anything else (a variable, a call, '/'/'%'/
  /// '**', a typed constant reference, ...) without raising anything --
  /// those are left to runtime overflow trapping instead (Codegen); a
  /// typed-constant reference is not folded.
  ///
  /// @p IsTopLevel is true only for the ORIGINAL call (from
  /// checkLiteralRepresentable()) and propagates through a unary '-'
  /// (folding "-BIG" is still conceptually the same single literal, just
  /// negated -- FREEZE.md's own `-9223372036854775808` example), but is
  /// always false for a BinaryExprAST's own LHS/RHS: those are independent
  /// leaves that Codegen (NumberExprAST::codegen()) must truncate to 64 bits
  /// BEFORE any arithmetic runs, so a leaf that doesn't independently fit
  /// int64 is rejected immediately, citing 'int' -- whether the whole
  /// expression's final value would fit is irrelevant, since Codegen could
  /// not honor that for the leaf. Only a bare top-level literal (this
  /// function called directly on a NumberExprAST) defers to
  /// checkLiteralRepresentable()'s own destination-aware message.
  ///
  /// Unary '-' written directly on a literal is ONE literal, so the leaf
  /// check judges the NEGATED value (`-9223372036854775808` fits in any
  /// operand position, the magnitude alone does not). Both operands of a
  /// binary node are folded before either's failure returns, so a
  /// runtime-variable operand on one side does not hide an out-of-range
  /// literal on the other.
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
  /// literal wording ("must be exactly representable"): the exact value must
  /// ALSO independently fit int64, because Codegen (NumberExprAST::codegen())
  /// truncates every literal through a 64-bit int before any int-to-double
  /// conversion runs, regardless of the literal's true magnitude -- a literal
  /// that's exactly representable as a double but exceeds int64 (e.g. 2^100)
  /// would codegen a WRONG value if this check let it through.
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

  /// Tells a construction call (`Box::new(..)`, `Box::default()`) used as
  /// @p E which concrete instantiation @p Expected it builds
  /// (noteConstructedType()), then reports E056 for a generic constructor
  /// argument that does not fit the declared instantiation: `mut b:
  /// Box<string> = Box::new(42);` takes T from `Box<string>`, so 42 must be a
  /// string. The call itself only saw its arguments, from which it bound
  /// T = int.
  void noteConstructed(const ExprAST *E, const TurfTypePtr &Expected);

  /// E138 for `Box::default().get()` / `Box::default().v` on a generic
  /// `Box`: a receiver has no declared type for noteConstructed() to take
  /// the instantiation from, so the call would build the bare template.
  void rejectUninstantiatedReceiver(const ExprAST *Base);
  void noteConstructedArgs(const std::vector<std::unique_ptr<ExprAST>> &Args,
                           const std::vector<FunctionParam> &Params,
                           size_t ParamOffset,
                           const std::unordered_map<std::string, TurfTypePtr>
                               *Subst = nullptr);

  /// FREEZE.md §6.2 row 3, F-NUM-1: true when @p Dest is `int32`
  /// and @p Val is exactly the kind of untyped-literal-only expression
  /// tryFoldUntypedIntConstant() folds (a bare literal, or +/-/* of pure
  /// literals). `typesCompatible()` (Types.cpp) sees such a value as plain
  /// `int` -- visitNumber() always types an integer literal `int`, with no
  /// way to record "still untyped" in a TurfTypePtr -- so it correctly
  /// rejects `int -> int32` as the narrowing conversion that IS for a
  /// genuinely `int`-typed value, but wrongly rejects it for a literal that
  /// was never actually typed `int` and should acquire `int32` directly,
  /// exactly like `guess`'s literal-defaulting rule (§6.3) or the 'double'
  /// destination case (checkLiteralRepresentable() above). Every type-fixing
  /// call site (VarDecl init, assignment, argument, return) checks this
  /// explicitly alongside typesCompatible() before treating a mismatch as
  /// real, since typesCompatible() itself has no ExprAST access to tell the
  /// two apart.
  bool untypedLiteralAcquiresInt32(const ExprAST *Val, const TurfType *Dest);

  /// True when @p Val is an array literal whose elements are all `&N` or
  /// `null` and @p Dest is `(&?N)[n]` of the same length: a fresh array may
  /// take the wider element type, as a declaration's initializer list does
  /// (`a = [null, null]`, `f([heap(n), heap(m)])`). Never a view or another
  /// variable: those keep their exact element type, since writing `null`
  /// through a widened alias would break a `&N` array. When it widens, the
  /// literal is retyped to @p Dest.
  bool arrayLiteralWidensToNullableRefs(const ExprAST *Val, const TurfType *Dest);

  /// untypedLiteralAcquiresInt32() or arrayLiteralWidensToNullableRefs():
  /// what every type-fixing call site checks alongside typesCompatible().
  bool literalAcquiresType(const ExprAST *Val, const TurfType *Dest) {
    return untypedLiteralAcquiresInt32(Val, Dest) ||
           arrayLiteralWidensToNullableRefs(Val, Dest);
  }

  /// If writing into @p Place would write into the by-value result
  /// of an `operator[]` read (`bs[0].a`, `outer[0]`, `bs[0].inner.x`), the
  /// ArrayAccessExprAST that produced the temporary; null otherwise. Walks
  /// down field accesses and builtin indexing, and STOPS at a pointer or
  /// reference base: a write through one lands in real storage, so
  /// `nodes[0].next = p` for a `List<Node*>` is fine. Only meaningful after
  /// the operands have been type-checked (it reads their types).
  const ArrayAccessExprAST *operatorIndexTemporaryRoot(const ExprAST *Place);

  /// Reports E098 (@p What is the whole problem sentence) for a write into
  /// an operator-index temporary, if operatorIndexTemporaryRoot(@p Place)
  /// finds one. @p PlaceIsStoreTarget is true when @p Place is the slot being
  /// assigned (its own type is the value stored), false when it is a method
  /// receiver or the container of a `[]=` -- a pointer-valued one of those is
  /// written through, so it is never a temporary.
  bool rejectOperatorTemporaryWrite(const ExprAST *Place, SourceLocation Loc,
                                    const std::string &What,
                                    bool PlaceIsStoreTarget);

  /// The one place an `operator` overload candidate is matched
  /// against concrete operand types -- every operator form (binary, unary,
  /// compound assignment, `++`/`--`, `[]`, `[]=`) calls this instead of
  /// comparing `sameTypeExact()` inline, so a generic candidate gets the
  /// same rules everywhere. See the definition's doc comment
  /// (TypeCheckerCall.cpp) for the rules; @p Site is the operator node, to
  /// be rewritten to the instantiated clone by the monomorphizer.
  bool matchOperatorOverload(const std::shared_ptr<ResolverFunctionSymbol> &C,
                             const std::vector<TurfTypePtr> &Operands,
                             bool RelaxSelf, ExprAST *Site, SourceLocation Loc,
                             TurfTypePtr &ResultTy);

  // ── State ─────────────────────────────────────────────────────────────────

  /// Reference to the symbol table filled by the Resolver.
  const ResolverSymbolTable &RST_;

  /// Map from AST node pointer to its inferred type.
  /// Pointer stability: we only ever store nodes whose lifetime is managed
  /// by the AST owning the unique_ptrs, so raw pointers are safe here.
  std::unordered_map<const ExprAST *, TurfTypePtr> NodeTypes_;

  /// Pluggable error reporter (default = TypeError::raise()).
  ErrorHandler ErrorHandler_;

  /// Null unless setMonomorphizationCollector() was called. See that
  /// method's doc comment.
  Monomorphize::InstantiationCollector *MonoCollector_ = nullptr;

  /// Stack of expected return types for nested function definitions.
  /// Empty when we are at the top level.
  std::vector<TurfTypePtr> ReturnTypeStack_;
  /// The `base[i]` on the left of the assignment being checked, if any.
  /// visitArrayAccess() reports "cannot index a value of type X" for a base
  /// nothing can index, but an assignment target has a more specific diagnostic
  /// of its own (E056, "no operator[]= matches"), so it stays quiet there.
  const ArrayAccessExprAST *IndexWriteTarget_ = nullptr;

  /// The function type expected of ExpectedFnSite_, the one expression being
  /// visited where a value of that type is needed (a declared variable, an
  /// assignment target, a return type, a parameter). A generic function named
  /// there as a value takes its type arguments from it
  /// (typeGenericFunctionValue()); anywhere else it has none to take.
  const ExprAST *ExpectedFnSite_ = nullptr;
  TurfTypePtr ExpectedFnType_;
  /// The same for a call: the type its result is stored into. A generic
  /// function's size parameter that no argument binds takes its value from
  /// it (`mut a: int[8] = fill(0);`).
  const ExprAST *ExpectedCallSite_ = nullptr;
  TurfTypePtr ExpectedCallType_;

  /// Visits @p E with @p Expected as its expected function type, when it is
  /// one and @p E is a bare name.
  void visitWithExpectedType(const ExprAST *E, const TurfTypePtr &Expected);

  /// Types a generic function used as a value (`immut f: fn(int): int = id;`)
  /// from the expected function type, and records that instantiation so the
  /// reference names the concrete clone.
  void typeGenericFunctionValue(VariableExprAST &Node);

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
  /// subject (e.g. a field access) is a scope limit and is left
  /// unnarrowed.
  ///
  /// A proven transition moves StateName to its target when it runs straight
  /// through the arm (Depth == ControlDepth_). One nested in a branch is still
  /// checked against StateName but leaves the state unknown afterwards, and
  /// one in a loop or `defer` body (LoopDepth_ > LoopDepth), which may run
  /// again or later, is left to the runtime guard and also leaves it unknown.
  struct MachineNarrowFrame {
    std::string VarName;
    std::string MachineName;
    std::string StateName;
    int Depth = 0;
    int LoopDepth = 0;
    bool Known = true;
  };
  std::vector<MachineNarrowFrame> MachineNarrowStack_;
  /// Marks every narrowing of @p Place (if it is a plain variable) unknown:
  /// an assignment or a `&` can change its state without a `.transition`.
  void forgetMachineNarrowing(const ExprAST *Place);
  /// Bodies entered conditionally (if/compare arms/recover blocks) or
  /// repeatedly/out of order (loops, `defer`), and the second kind alone.
  int ControlDepth_ = 0;
  int LoopDepth_ = 0;
  struct ControlScope {
    TypeChecker &TC;
    bool Loop;
    ControlScope(TypeChecker &TC, bool Loop) : TC(TC), Loop(Loop) {
      ++TC.ControlDepth_;
      if (Loop)
        ++TC.LoopDepth_;
    }
    ~ControlScope() {
      --TC.ControlDepth_;
      if (Loop)
        --TC.LoopDepth_;
    }
  };

  /// Loop variable types: populated by visitFor() and visitForEach(), consulted
  /// by visitVariable(). Maps loop variable name → its type (`int` or `double`
  /// for a range-for, the element type for a for-each).
  std::unordered_map<std::string, TurfTypePtr> LoopVarTypes_;

  /// Nesting depth of `unsafe { }` blocks -- an int counter,
  /// not a bool, so `unsafe { unsafe { } }` (silly but harmless) stays
  /// authorized all the way through, and so a stray extra pop can never
  /// underflow past zero. Mirrors Resolver::FunctionNestingDepth_
  /// (Resolver.h) -- incremented/decremented bracketing visitUnsafeBlock()'s
  /// body traversal. Consulted by isUnsafeGateExempt().
  int UnsafeDepth_ = 0;

  /// True while visiting the body of a function declared
  /// `@caller_location`. A plain bool, not a counter like UnsafeDepth_:
  /// visitFuncDef() sets it from each function's own declaration and
  /// save/restores it around the body, so a block-nested `fn` is a SEPARATE
  /// visitFuncDef() call with its own value and never inherits the outer
  /// function's. Consulted by visitCallerLocation().
  bool InCallerLocationFunc_ = false;

  /// The name of the function whose body is being checked ("" at the top
  /// level), save/restored around visitFuncDef()'s body traversal. Only
  /// visitTry() reads it, to tell a `try` in `main` -- which can never be made
  /// fallible, so the hint differs -- from one in any other function that
  /// returns a plain type.
  std::string CurrentFuncName_;

  // ── Visibility (FREEZE.md §6.15) ─────────────────────────────────────────────
  //
  // Field and method access can only be checked here: the Resolver does not
  // know a receiver's type. Both are private-to-module unless `pub`, so the
  // checker needs to know which module the code it is looking at was WRITTEN in.

  /// The module whose source is being checked ("" = the main file). Set by
  /// visitFuncDef() from the function's own declaring module -- not from the
  /// enclosing import -- so a monomorphized clone is checked as its TEMPLATE's
  /// module: `List<int>.push`'s body reads the private `self.data` because
  /// `list` wrote it, whichever module asked for the instantiation.
  std::string CurrentModule_;

  /// True if code in CurrentModule_ may use member @p Vis of type
  /// @p OwnerIdentity: it is public, or CurrentModule_ owns the type.
  bool memberVisible(const VisibilityMark &Vis, const std::string &OwnerIdentity) const;

  /// Raises E088 for private field @p FieldName of struct @p STy at @p Loc.
  void raisePrivateField(SourceLocation Loc, const TurfStructType &STy,
                         const StructField &Field) const;

  /// Drops from @p Candidates every method/function CurrentModule_ may not
  /// call (private, declared in another module). If that empties a non-empty
  /// list, raises E088 (as a method of @p OwnerIdentity when non-empty, else
  /// as a free function) and returns false.
  bool filterVisibleCandidates(std::vector<const ResolverFunctionSymbol *> &Candidates,
                               SourceLocation Loc, const std::string &Name,
                               const std::string &OwnerIdentity);

  // ── Destructuring patterns (FREEZE.md §6.17) ─────────────────────────────────

  /// The type a field of @p STy has in ONE instantiation of that struct:
  /// generic parameters substituted from @p GenericTypeArgs, a NamedType
  /// resolved to its registered type. @p FieldTy is the field's declared type.
  /// Shared by `p.field` (visitFieldAccess) and by struct patterns, so a
  /// destructured field can never disagree with the same field read directly.
  TurfTypePtr instantiatedFieldType(const TurfStructType &STy,
                                    const std::vector<TurfTypePtr> &GenericTypeArgs,
                                    TurfTypePtr FieldTy);

  /// Types one pattern against the type @p Ty of the value it is matched
  /// with, writing each binding's type and each struct pattern's field types
  /// into the AST. Returns true if the pattern is IRREFUTABLE -- it matches
  /// every value of @p Ty -- which is what makes an arm a catch-all.
  ///
  /// @p Where is the enclosing header's or field's location, for diagnostics
  /// about a value sub-pattern (an expression has no location of its own).
  /// @p ArmBindingTypes maps each name the arm's earlier headers bound to the
  /// type it was bound at, so a name bound at two different types by two
  /// stacked headers is caught. @p SelfSubject is true only for the top-level
  /// subject `self`, the one raw pointer a pattern may look through (it is
  /// never null, and field access exempts it the same way).
  bool checkPattern(ExprAST *Pat, const TurfTypePtr &Ty, SourceLocation Where,
                    std::unordered_map<std::string, TurfTypePtr> &ArmBindingTypes,
                    bool SelfSubject = false);

  /// Whether we had at least one error during this pass.
  bool HadError_ = false;
};

#endif // TYPE_CHECKER_H
