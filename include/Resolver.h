/**
 * @file Resolver.h
 * @brief The Resolver pass: name resolution before type checking or codegen.
 *
 * Issue #18 — Two-Pass Resolver for Forward Declarations.
 *
 * Implements `class Resolver : public ASTVisitor`. A single call to
 * `Resolver::resolveAll(nodes)` walks the AST in two distinct passes:
 *
 *  **Pass 1 — Hoisting** (`hoistTopLevel`):
 *    Walk only top-level AST nodes (no recursion into bodies).
 *    For each `StructDeclExprAST`: call `SymTable_.registerStruct(Name)`.
 *    For each `FuncDefExprAST`: call `SymTable_.registerCurrentFileFunction(sym)`
 *      — only the signature (name, params, returnType), NOT the body.
 *    For each `ModuleImportExprAST`: register all functions in the module.
 *
 *  **Pass 2 — Full Resolution** (existing visitor behavior):
 *    Walk the entire AST, resolve all variable references, function calls,
 *    and type annotations. Because Pass 1 already registered all structs and
 *    functions, forward references resolve correctly.
 *
 * This two-pass design is why the compiler no longer needs
 * `GlobalSymbolTable->LookupType()` in `ParseType()`: the parser creates a
 * `NamedType("MyStruct")` placeholder, and the Resolver's Pass 2 validates
 * it by looking up the name that was registered in Pass 1.
 *
 * Error handling strategy:
 *  - `visitVarDecl` — raises DuplicateDeclarationError if the same name is
 *    re-declared in the *same* scope. Shadowing an outer scope is allowed.
 *  - `visitVariable` — raises ReferenceError (with Damerau-Levenshtein
 *    suggestions) if the variable name is not found in any enclosing scope.
 *  - `visitStructDecl` — validates that all NamedType field references
 *    (including pointer-to-named-type fields) refer to structs registered in
 *    Pass 1. Primitive types and void are always allowed. Type-level
 *    validation (field types, initialization) is the TypeChecker's job.
 *
 * Design notes:
 *  - The Resolver owns its ResolverSymbolTable instance.
 *  - All child dispatch is done via `child->accept(*this)` so that every
 *    node type is recursed automatically without duplicating traversal code.
 *
 * @author a2ys
 * @copyright Copyright (c) 2026 TurfLang contributors.  Apache License.
 */

#ifndef RESOLVER_H
#define RESOLVER_H

#include "AST.h"
#include "ASTVisitor.h"
#include "ResolverSymbolTable.h"

#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/**
 * @brief Name-resolution pass for the Turflang multi-pass compiler pipeline.
 *
 * Usage:
 * @code
 *   InitializeCompilerGlobals();     // see Parser.h -- required once per process, before the below
 *   Resolver resolver;
 *   resolver.resolve(programRoot);   // throws on the first error found
 * @endcode
 *
 * The Resolver walks the whole AST once before Codegen runs.  In the current
 * "shadow mode" (Phase 2 of the migration plan) its output is not yet fed into
 * Codegen; the pass is merely run for early error detection.
 *
 * Despite this header being LLVM-free/self-contained (see file doc comment
 * above), the Resolver is NOT fully decoupled from process-global state:
 * ParseStructDecl() (Parser, not this class) registers every parsed struct
 * into the global `GlobalSymbolTable` (SymbolTable.h), which TypeChecker's
 * visitMethodCall() later reads back from to decide whether a call's base
 * expression is a struct instance. `GlobalSymbolTable` is null until
 * `InitializeCompilerGlobals()`/`InitializeSymbolTable()` runs -- every read
 * site is null-guarded so skipping it will not crash, but every struct
 * registration and lookup silently becomes a no-op instead, which surfaces
 * later as misleading unrelated diagnostics (see issue #231). An embedder
 * constructing a bare `Resolver` outside main.cpp's own pipeline (e.g. a
 * unit test exercising just resolution) still needs this call first if the
 * source under test declares or calls into any struct.
 */
class Resolver : public ASTVisitor {
public:
  Resolver() = default;

  // ── Public entry points ────────────────────────────────────────────────────

  /**
   * @brief Resolve a single top-level expression / statement.
   *
   * Accepts any ExprAST node and dispatches via the visitor pattern.
   * Typically called once per top-level node produced by the parser.
   */
  void resolve(ExprAST *Node);

  /**
   * @brief TYPE-002 (S12b): resolve() a monomorphized clone -- as if it
   *        were declared inside module @p Module (ModuleStack_ carries
   *        that name for the duration, exactly as a real declaration
   *        hoisted from inside that module's own ModuleImportExprAST body
   *        would see), AND with validateNamedType()'s module-qualified-
   *        visibility check (isTypeVisibleUnqualified()) suppressed.
   *
   * Two independent fixes for two real bugs found live, both needed
   * together:
   *
   * 1. currentModuleName() -- used by validateNamedType() to decide
   *    whether a bare struct name like "List" is "local" and needs no
   *    `module::` qualification -- reads ModuleStack_, which is empty for
   *    an ordinary resolve() call issued from OUTSIDE any import's own
   *    body traversal (exactly the monomorphizer's position, sub-pass
   *    2b.5, main.cpp). Without pushing @p Module first, a clone of e.g.
   *    lib/std/collections/list.tr's `fn<T> List.push` fails re-resolving
   *    its own `self: List*` parameter with "'List' isn't visible here
   *    without qualification".
   *
   * 2. Once (1) is fixed, a DIFFERENT failure appears: a clone's
   *    SUBSTITUTED type arguments can name a struct from ANY module the
   *    ORIGINAL call site could see -- not just what the template's own
   *    declaring module imports. `List<JsonValue>.push`'s clone (declared
   *    in list.tr) needs to reference JsonValue (json.tr) -- something
   *    list.tr itself never imports, correctly, since an ordinary,
   *    non-generic List never needs to. The substituted VALUE is trusted
   *    instead of re-validated: it came from an already-resolved,
   *    already-successfully-typechecked expression at the ORIGINAL call
   *    site (TypeCheckerCall.cpp's collectGenericSubstitution() output),
   *    which only ever succeeds against a type that already passed this
   *    exact check once, in ITS OWN correct scope -- re-deriving that
   *    scope here, from a bare TurfTypePtr with no back-pointer to where
   *    it was written, is not possible, so the check is suppressed for
   *    the substituted parts instead of attempted incorrectly.
   */
  void resolveMonomorphizedClone(ExprAST *Node, const std::string &Module) {
    if (!Module.empty()) ModuleStack_.push_back(Module);
    bool SavedSuppress = SuppressUnqualifiedVisibilityCheck_;
    SuppressUnqualifiedVisibilityCheck_ = true;
    resolve(Node);
    SuppressUnqualifiedVisibilityCheck_ = SavedSuppress;
    if (!Module.empty()) ModuleStack_.pop_back();
  }

  /// See resolveMonomorphizedClone()'s doc comment, fix (2). Read by
  /// validateNamedType() (ResolverDecl.cpp) at both isTypeVisibleUnqualified()
  /// call sites. False for every ordinary resolve()/resolveAll() call --
  /// only resolveMonomorphizedClone() ever sets this, scoped to its own call.
  bool suppressUnqualifiedVisibilityCheck() const {
    return SuppressUnqualifiedVisibilityCheck_;
  }

  /**
   * @brief TYPE-002 (S13a): per-instantiation acyclicity check for one
   *        freshly-cloned, freshly-substituted generic struct.
   *
   * checkStructLayoutAcyclicity() (private, below) is the Pass-1, TEMPLATE-
   * level check: it walks every hoisted struct's fields as-declared, where a
   * generic struct's own type-parameter-typed field (`val: T`) carries no
   * edge at all (byValueEdgeTarget() finds no NamedType/GenericType/
   * TurfStructType to match). That check is already sufficient for every
   * generic struct in this language's stdlib today, because a direct BY-
   * VALUE self-reference (`struct Node<T> { next: Node<T> }`) is flagged
   * there directly, AS a self-edge, regardless of what T ends up being. What
   * the template-level check CANNOT see is a cycle that only exists once a
   * SPECIFIC type argument is substituted in -- e.g. `struct A<T> { val: T }`
   * instantiated as `A<B>` where `struct B { val: A<B> }`: at the template
   * level, A's own field is `val: T` (no edge), so the cycle through B is
   * invisible; only after substituting T -> B does `val: T` become
   * `val: B` and the DFS below correctly finds A_B -> B -> A_B.
   *
   * Reuses the exact same DFS/byValueEdgeTarget() machinery
   * (ResolverCore.cpp, anonymous namespace) as checkStructLayoutAcyclicity()
   * -- not a second checker -- scoped to a SINGLE new node (@p Clone, whose
   * OWN Name is already its unique mangled instantiation name and whose
   * Fields are already fully substituted to concrete types by the caller):
   * every OTHER struct already known to this Resolver (HoistedStructDecls_)
   * is trusted to already be acyclic (Pass 1 already proved that), so only
   * @p Clone itself needs to be walked as a DFS root.
   *
   * Raises StructLayoutCycleError directly (like the Pass-1 check already
   * does) if @p Clone's OWN fields close a cycle -- not re-wrapped in a
   * monomorphizer "in this instantiation" note, unlike a TypeChecker
   * failure (finishInstantiation(), Monomorphize.cpp): the diagnostic this
   * raises already names the concrete offending field chain directly, which
   * a generic re-wrapping would only make less precise.
   */
  void checkStructLayoutAcyclicityForClone(const StructDeclExprAST *Clone);

  /**
   * @brief Resolve an entire program (list of top-level nodes).
   *
   * Convenience wrapper that iterates over a vector of unique_ptr<ExprAST>
   * and calls resolve() on each one in order.
   *
   * @param RecoverPerDeclaration  See issue #246. When false (the default,
   *        unchanged behavior for every existing caller -- main.cpp's real
   *        compile pipeline relies on this): Pass 2 is one uninterrupted
   *        walk, so the FIRST TurfError raised anywhere longjmps straight
   *        back out of this whole call, matching the caller's own single
   *        outer setjmp() guard -- the correct, deliberate behavior for a
   *        pipeline that must never let Codegen run on a partially-
   *        resolved AST (main.cpp's own "Codegen must never run on an
   *        unresolved AST" invariant). When true, each top-level node
   *        instead gets its OWN setjmp() guard (mirroring #228's identical
   *        fix for the parser's per-statement loop), so one declaration's
   *        semantic error doesn't prevent every OTHER declaration in the
   *        buffer from being resolved and having its own errors surfaced
   *        too -- errors are still recorded via the normal
   *        DiagnosticEngine::add() path either way; only whether a single
   *        error aborts the WHOLE call changes. Intended for tooling that
   *        wants best-effort, multi-diagnostic results (e.g.
   *        turf::analyze(), include/turf/Analysis.h) and will never feed
   *        the result to Codegen, not for the CLI's own real compile.
   *
   * Applies only to Pass 2. Pass 1 (hoistTopLevel) always aborts the whole
   * call on its first error regardless of this flag -- a hoisting failure
   * (e.g. two structs registered under the same name) leaves the symbol
   * table in a state Pass 2 cannot safely reason about for ANY
   * declaration, not just the one that caused it, unlike a Pass 2 failure,
   * which genuinely doesn't affect other declarations' correctness.
   */
  void resolveAll(const std::vector<std::unique_ptr<ExprAST>> &Nodes,
                  bool RecoverPerDeclaration = false);

  /**
   * @brief Reset internal scope tracking after a longjmp recovery.
   *
   * Must be called before resuming resolution on subsequent top-level nodes
   * if a previous node raised an error caught by `setjmp`.
   * Warning: Do not loop-with-recovery over multiple top-level nodes without
   * resetting scope depth, FunctionNestingDepth_, ModuleStack_, and
   * CurrentFuncTypeParams_ between iterations.
   *
   * ModuleStack_/CurrentFuncTypeParams_ (found via code review, added after
   * this method's first use in resolveAll()'s RecoverPerDeclaration loop):
   * both are set-before/cleared-after patterns around visitModuleImport()'s
   * and visitFuncDef()'s own body traversal (ResolverDecl.cpp), with no
   * RAII guard -- a longjmp mid-body skips the clear the same way it used
   * to skip FunctionNestingDepth_/scope depth's reset before this method
   * existed. Left stale, a failing `import`'s ModuleStack_ entry makes
   * currentModuleName() return the wrong module for every UNRELATED
   * top-level declaration that follows in the same buffer; a failing
   * generic function's CurrentFuncTypeParams_ makes validateNamedType()
   * wrongly accept that function's own type parameter name (e.g. `T`) as
   * in-scope everywhere else too -- both silent mis-resolutions, not just
   * missing diagnostics. Both are simple "current value, no nesting across
   * top-level declarations" state (never legitimately non-empty at the
   * start of a fresh top-level node), so clearing unconditionally is
   * correct, mirroring FunctionNestingDepth_/scope depth's own reset here.
   */
  void resetForRecovery() {
    FunctionNestingDepth_ = 0;
    SymTable_.resetToDepth(0);
    ModuleStack_.clear();
    CurrentFuncTypeParams_.clear();
  }

  // ── ASTVisitor overrides ───────────────────────────────────────────────────

  // ── Variables & Declarations ──────────────────────────────────────────────

  /**
   * @brief Register the declared variable in the current scope.
   *
   * Also recurses into the initializer expression (if present) so that
   * uses of variables inside the initializer are validated.
   *
   * Raises DuplicateDeclarationError if the name is already declared in the
   * *current* (innermost) scope.
   */
  void visitVarDecl(VarDeclExprAST &Node) override;

  /**
   * @brief Check that the referenced variable exists in an enclosing scope.
   *
   * Raises ReferenceError with Damerau-Levenshtein suggestions if the name
   * cannot be found in any scope from innermost to outermost.
   */
  void visitVariable(VariableExprAST &Node) override;

  // ── Control flow ──────────────────────────────────────────────────────────

  /**
   * @brief Walk a block, wrapping it in enterScope() / exitScope().
   *
   * Every statement inside the block is recursed via accept(*this).
   */
  void visitBlock(BlockExprAST &Node) override;
  void visitUnsafeBlock(UnsafeBlockExprAST &Node) override;

  // ── All other node types — just recurse into children ─────────────────────

  void visitNumber(NumberExprAST &Node) override { (void)Node; }
  void visitBool(BoolExprAST &Node) override { (void)Node; }
  void visitChar(CharExprAST &Node) override { (void)Node; }
  void visitString(StringExprAST &Node) override { (void)Node; }
  void visitNull(NullExprAST &Node) override { (void)Node; }

  void visitBinary(BinaryExprAST &Node) override;
  void visitUnary(UnaryExprAST &Node) override;
  void visitAddressOf(AddressOfExprAST &Node) override;
  void visitAssignment(AssignmentExprAST &Node) override;
  void visitCompoundAssignment(CompoundAssignmentExprAST &Node) override;
  void visitPostfixExpr(PostfixExprAST &Node) override;
  void visitCast(CastExprAST &Node) override;

  void visitIf(IfExprAST &Node) override;
  void visitWhile(WhileExprAST &Node) override;
  void visitDoWhile(DoWhileExprAST &Node) override;
  void visitFor(ForExprAST &Node) override;
  void visitForEach(ForEachExprAST &Node) override;
  void visitBreak(BreakExprAST &Node) override { (void)Node; }
  void visitContinue(ContinueExprAST &Node) override { (void)Node; }
  void visitReturn(ReturnExprAST &Node) override;
  void visitCompare(CompareExprAST &Node) override;
  void visitDefer(DeferExprAST &Node) override;

  void visitFuncDef(FuncDefExprAST &Node) override;
  void visitFuncCall(FuncCallExprAST &Node) override;
  void visitMethodCall(MethodCallExprAST &Node) override;

  void visitFieldAccess(FieldAccessExprAST &Node) override;
  void visitStructDecl(StructDeclExprAST &Node) override;
  void visitEnumDecl(EnumDeclExprAST &Node) override;
  void visitMachineDecl(MachineDeclExprAST &Node) override;
  void visitScopeAccess(ScopeAccessExprAST &Node) override;
  void visitErrorDecl(ErrorDeclExprAST &Node) override;
  void visitRecoverExpr(RecoverExprAST &Node) override;

  void visitArrayDecl(ArrayDeclExprAST &Node) override;
  void visitArrayLiteral(ArrayLiteralExprAST &Node) override;
  void visitArrayAccess(ArrayAccessExprAST &Node) override;
  void visitModuleImport(ModuleImportExprAST &Node) override;
  void visitSizeOf(SizeOfExprAST &Node) override {
    // FDN-003: `sizeof(Node)` names a type -- same silent resolution as a
    // cast's destination (visitCast).
    resolveTypeSpellingsSilently(Node.getTypeArg(), currentModuleName(),
                                 CurrentFuncTypeParams_);
  }

  // ── FFI ────────────────────────────────────────────────────────────────────
  void visitExternFuncDecl(ExternFuncDeclExprAST &Node) override;

  // ── Accessors ──────────────────────────────────────────────────────────────

  /// Read-only access to the symbol table (e.g. for unit tests).
  const ResolverSymbolTable &symbolTable() const { return SymTable_; }

  /**
   * @brief Optional per-node scope-visibility snapshot hook -- see issue
   *        #244 (no position-indexed scope-query API existed for
   *        completions/autocomplete tooling).
   *
   * When set, fired once for every node the Pass-2 walk is ABOUT to visit
   * (before that node's own declarations, e.g. its own VarDeclExprAST
   * name, become visible), passing the node itself and every name visible
   * in scope at that exact point (SymTable_.getAllVisibleNames() --
   * unqualified names only, no kind/type metadata; see that method's own
   * doc comment for what it already covers).
   *
   * Fires from the Resolver's single existing child-dispatch chokepoints
   * (visitChild() below, and resolve() for top-level nodes) rather than
   * needing a callback added to every individual visitXXX() method, so
   * this covers the ENTIRE Pass-2 walk (every node, not just a hand-picked
   * subset) with no change to the traversal logic itself. Not fired during
   * Pass 1 (hoistTopLevel) -- hoisting doesn't visit function/block
   * bodies, so there's nothing position-relevant to snapshot there.
   *
   * A caller wanting "what's visible at position (line, col)" builds this
   * into a position -> names index themselves (the query facade's
   * AnalysisResult::symbolsInScope(), src/AnalysisFacade.cpp, is the
   * in-tree one) -- the Resolver itself stays storage-agnostic and has
   * zero overhead when this isn't set.
   */
  using ScopeSnapshotCallback =
      std::function<void(const ExprAST &Node,
                         const std::vector<std::string> &VisibleNames)>;
  void setScopeSnapshotCallback(ScopeSnapshotCallback CB) {
    ScopeSnapshotCB_ = std::move(CB);
  }

private:
  ScopeSnapshotCallback ScopeSnapshotCB_;
  ResolverSymbolTable SymTable_;
  int FunctionNestingDepth_ = 0;
  std::vector<std::string> ModuleStack_;
  // TYPE-002 (S12b): see resolveMonomorphizedClone()'s doc comment. Always
  // false outside that one method's own scoped set/restore.
  bool SuppressUnqualifiedVisibilityCheck_ = false;
  // The enclosing function's own `fn<...>` type parameter names, set for the
  // duration of visitFuncDef()'s body traversal and cleared on exit (a plain
  // member, not a stack, is safe since FunctionNestingDepth_ already forbids
  // nested function definitions -- see NestedFunctionError). Consulted by
  // visitVarDecl()'s validateNamedType() call so a bare generic type
  // parameter used as a local variable's declared type (`fn<T> f() { mut x:
  // T = ...; }`) isn't wrongly flagged as an unknown type.
  std::vector<std::string> CurrentFuncTypeParams_;

  // Every struct hoisted so far by hoistTopLevel()/hoistModuleBody() (main
  // file plus every transitively-imported module), in hoist order. Neither
  // of those functions keeps the AST node around once the NAME is
  // registered in SymTable_ (ResolverTypeRegistry only stores name ->
  // declaration location, never the fields -- see its own doc comment), so
  // checkStructLayoutAcyclicity() needs its own record of "which
  // StructDeclExprAST does this struct name mean" to walk field types at
  // all. Never cleared: a Resolver instance may run resolveAll() more than
  // once (main.cpp calls it once for FnASTs, once for TopLevelASTs, on the
  // SAME Resolver -- structs only ever appear in the former, but nothing
  // stops a future caller from splitting differently), and accumulating
  // across calls is what makes a struct declared in an earlier call still
  // participate in a cycle check that runs after a later one.
  std::vector<const StructDeclExprAST *> HoistedStructDecls_;

  // FDN-003: every generic struct TEMPLATE hoisted so far (`struct Box<T>`),
  // identity -> its own type-parameter names. Two rules read it:
  // validateNamedType() rejects one written without type arguments (`mut b:
  // Box = ...`) -- which otherwise reached Codegen as the unsized template
  // and failed as "generated LLVM IR is invalid" (the Parser used to catch
  // this for exactly one spelling, a hardcoded bare `List`, which also made
  // a user's own non-generic `struct List` unusable); and visitFuncDef()
  // rejects an instance method on one that declares no type parameters of
  // its own (see there).
  std::unordered_map<std::string, std::vector<std::string>> GenericStructIdentities_;

  // ── Internal helpers ───────────────────────────────────────────────────────

  std::string currentModuleName() const {
    return ModuleStack_.empty() ? std::string() : ModuleStack_.back();
  }

  ResolverFunctionSymbol buildFunctionSymbol(const FuncDefExprAST &Node,
                                             const std::string &ModuleName) const;

  /**
   * @brief Reject an `operator` declaration whose BOTH operand types are
   *        already in the compiler's builtin set (int/double/bool/char/
   *        Pointer) -- RFC §4.3's "builtin-pair" hard error, checked at the
   *        declaration site. `String` is deliberately excluded from that
   *        set, which is what makes `operator +(string, string)` legal --
   *        that's the entire point of this restructuring.
   *
   * Called from registerFunctionSignature() (the single choke point reached
   * by main-file hoisting, module hoisting, AND visitFuncDef -- so an
   * operator declared anywhere is always checked, not just top-level ones).
   * A no-op for any Sym whose BaseName doesn't start with "operator" (the
   * overwhelming majority of calls), so ordinary functions pay one prefix
   * check and nothing else.
   */
  void validateOperatorDecl(const ResolverFunctionSymbol &Sym) const;

  /**
   * @brief Validate that a type reference (field, parameter, or return type)
   *        names either a known primitive, a struct/enum registered in
   *        Pass 1, or one of the enclosing declaration's own generic type
   *        parameters, raising UnknownTypeError otherwise.
   *
   * Shared by visitStructDecl (field types), visitFuncDef (parameter and
   * return types), and visitExternFuncDecl (parameter and return types) so
   * all four positions get identical, complete validation instead of only
   * struct fields being checked — see the doc comment on the .cpp definition
   * for the concrete gap this closes (an unknown struct name in a function
   * signature previously compiled with zero diagnostics).
   *
   * @param Ty         The parsed type to validate. No-op if null.
   * @param Loc        Source location to attach to any raised diagnostic.
   * @param TypeParams Names declared in the enclosing `fn<...>`/`struct<...>`
   *                   parameter list (e.g. {"T"} for `fn<T> identity(x: T)`).
   *                   ParseType() has no way to know at parse time that a
   *                   bare identifier like `T` names a generic parameter
   *                   rather than a struct, so it always produces a plain
   *                   NamedType("T") -- never a TurfGenericParamType -- for
   *                   type ANNOTATIONS (as opposed to the GlobalSymbolTable
   *                   registration ParseFuncDef/ParseStructDecl perform for
   *                   Codegen's benefit, which is a separate, LLVM-aware
   *                   table this LLVM-free pass does not consult). Without
   *                   this exemption list, every generic function or struct
   *                   using its own type parameter as a field/parameter/
   *                   return type (e.g. `fn<T> identity(x: T): T`) is
   *                   incorrectly rejected as "Unknown type 'T'". Defaults to
   *                   empty for call sites with no type parameters of their
   *                   own (currently visitExternFuncDecl).
   */
  void validateNamedType(const TurfTypePtr &Ty, SourceLocation Loc,
                         const std::vector<std::string> &TypeParams = {},
                         bool AllowBareGeneric = false) const;

  /// Validates an explicit `Module::TypeName` qualifier (from NamedType::
  /// getModule()/GenericType::getModule(), or a qualified struct-literal's
  /// ModuleQualifier) once @p TypeName is already confirmed to be a real,
  /// known struct/enum. A no-op (returns true) if @p Qualifier is empty --
  /// an unqualified reference has nothing to validate. Raises
  /// UnknownScopeQualifierError if @p Qualifier isn't a known module at
  /// all, or UnknownModuleSymbolError if it IS a real module but doesn't
  /// own @p TypeName (e.g. `mem::List` when List actually lives in
  /// `list`). Returns false in either error case (already raised), true
  /// if the qualifier is empty or correct.
  // ── Visibility (LEC-002, FREEZE.md §6.15) ──────────────────────────────────
  //
  // Every cross-module reference to a function or type is checked against the
  // declaration's `pub` mark. A reference from inside the declaring module is
  // never checked. All the diagnostics go through the two raise helpers so the
  // wording and the hints (which name the file, the keyword and the module's
  // public alternatives) cannot drift between the ways a private name can be
  // reached: `m::f()`, `m::T`, and `import m::name;`.

  /// Raises PrivateSymbolAccessError (E088) for private function @p Name of
  /// module @p Module, pointing at its declaration.
  void raisePrivateFunction(SourceLocation Loc, const std::string &Module,
                            const std::string &Name) const;
  /// Raises PrivateSymbolAccessError (E088) for private type @p Identity.
  void raisePrivateType(SourceLocation Loc, const std::string &Identity) const;
  /// Drops, from @p Matches, every overload the current module may not call
  /// (private, declared in another module). If that leaves nothing but at
  /// least one overload was dropped, raises E088 at @p Loc and returns false.
  /// Overloads of one name can differ in visibility: a caller who can only
  /// see the `pub` ones must not have the private one join overload
  /// resolution -- it would make the call's meaning depend on a hidden name.
  bool filterToVisibleOverloads(
      std::vector<ResolverSymbolTable::ModuleFunctionMatch> &Matches,
      SourceLocation Loc, const std::string &Name) const;

  bool validateModuleQualifier(const std::string &Qualifier,
                               const std::string &TypeName,
                               SourceLocation Loc) const;

  /// Suggestion candidates for an unresolved `Left::Right` qualifier once
  /// @p Left has already been confirmed to be none of: a known enum, a
  /// known error type, a known struct, a known module, or a declared
  /// variable (every one of those has its own earlier, more specific
  /// check/error with an early return -- see visitFuncCall's and
  /// visitScopeAccess's own dispatch chains). At that point @p Left can
  /// only be a typo of one of those four global name kinds, so the
  /// candidate pool spans all four -- searching only enum names (the
  /// original, narrower behavior) missed a struct/error-type/module typo
  /// entirely, no matter how close the spelling.
  std::vector<std::string> unknownQualifierSuggestionCandidates() const;

  // ── Pass 1: Hoisting ───────────────────────────────────────────────────────
  // Walk only the top-level list, registering struct names and function
  // signatures.  No bodies are visited in this pass.

  // ── Pass 1a/1b: type identity (FDN-003) ─────────────────────────────────
  //
  // A type's identity is (owning module, name) -- TypeIdentity, Types.h.
  // Every struct/enum/machine/error declaration in every module is
  // registered FIRST (1a, hoistTypes), so that 1b can turn every
  // declaration-level type SPELLING (struct fields, function and extern
  // signatures, a method's receiver) into an identity before anything --
  // function-signature registration, the struct-layout cycle check --
  // captures or compares one. Pass 2 (validateNamedType() and the `Left::`
  // handling in visitFuncCall/visitScopeAccess) does the same for every
  // spelling inside a body, with diagnostics.

  /// 1a: register every type declared in @p Nodes (recursing into module
  /// bodies), plus every on-demand import's TYPE names.
  void hoistTypes(const std::vector<std::unique_ptr<ExprAST>> &Nodes);
  void hoistTypesIn(const ExprAST *Node, const std::string &ModuleName);
  /// The type half of registerOnDemandImports(), run in 1a so on-demand-
  /// imported types are already visible to 1b.
  void registerOnDemandTypeImports(const ModuleImportExprAST &Import,
                                   const std::string &ContextLabel);

  /// Pass 2: register an enum/machine/error type declared inside a function
  /// body (Pass 1a only sees top-level ones), reporting a second
  /// declaration of the same name as E034. See visitEnumDecl().
  void registerLocalTypeDecl(const ExprAST &Node, SourceLocation Loc);

  /// 1b: resolve declaration-level type spellings in place, silently -- an
  /// unresolvable spelling is left alone for Pass 2 to report with a real
  /// diagnostic (Pass 1 aborts the whole resolveAll() on its first error).
  void resolveDeclarationSpellings(const std::vector<std::unique_ptr<ExprAST>> &Nodes);
  void resolveDeclarationSpellingsIn(ExprAST *Node, const std::string &Module);

  /// The TypeIdentity a spelling (`Qualifier::Name`, or bare `Name` when
  /// @p Qualifier is empty) means from @p Context, or nullopt if it is
  /// unknown, not visible bare, ambiguous, or wrongly qualified. Silent;
  /// the diagnosing counterparts are validateNamedType() and
  /// resolveScopeLeftType().
  std::optional<std::string> tryResolveTypeSpelling(const std::string &Qualifier,
                                                    const std::string &Name,
                                                    const std::string &Context) const;

  /// Walks every shape @p Ty can take (pointer, reference, array, generic
  /// arguments, error union, function type) and resolves each still-
  /// unresolved NamedType/GenericType base in place via
  /// tryResolveTypeSpelling(). Names in @p TypeParams are never touched.
  void resolveTypeSpellingsSilently(const TurfTypePtr &Ty, const std::string &Context,
                                    const std::vector<std::string> &TypeParams) const;

  /// `Left` of a `[Q::]Left::Right` expression, as a TypeIdentity, if it
  /// names a type -- rewriting the node's LeftName to that identity in
  /// place so every later reader (the rest of this pass, the TypeChecker,
  /// Codegen) sees the identity. Returns "" when Left names no type (a
  /// module, a variable, or nothing). Raises AmbiguousTypeReferenceError
  /// when a bare Left could mean two types.
  ///
  /// A bare Left that is not visible unqualified still resolves when
  /// exactly one type anywhere is spelled that way: `Vec2::zero()` never
  /// required `Vec2` to be bare-imported (static calls and enum variants
  /// are existence-checked, not visibility-gated -- 2.8.1 in
  /// modules_and_scope_resolution.md); construction keeps its own
  /// isTypeVisibleUnqualified() gate on top of this.
  std::string resolveScopeLeftType(ScopeAccessExprAST &Node, SourceLocation Loc);

  /// Hoist a single top-level node (struct name or function signature).
  void hoistTopLevel(const ExprAST *Node);

  /// Hoist an entire list of top-level nodes (convenience overload for
  /// resolveAll).
  void hoistTopLevel(const std::vector<std::unique_ptr<ExprAST>> &Nodes);

  /// Register all function signatures found inside an imported module body.
  /// Called by hoistTopLevel when it encounters a ModuleImportExprAST.
  void hoistModuleBody(const ExprAST *Body, const std::string &ModuleName);

  /**
   * @brief Reject a struct that contains itself by value, directly or
   *        through a chain of other structs (TYPE-001,
   *        docs/planning/ARCHITECTURE_OVERHAUL.md §8, INV-LAYOUT-1).
   *
   * Runs a grey/black-coloured DFS over the by-value containment graph
   * built from HoistedStructDecls_ (every struct hoisted so far, from the
   * main file and every transitively-imported module) and raises
   * StructLayoutCycleError on the first cycle found, naming the full path.
   * Reports one diagnostic per strongly-connected component, not per node
   * in it (a black-coloured node is never re-explored), so a cycle
   * touching several structs does not produce a diagnostic for each one.
   *
   * Called from resolveAll() right after hoistTopLevel(Nodes) -- i.e. at
   * the end of Pass 1, once every struct name in this call's node list is
   * registered, and strictly before Pass 2 (or anything else) ever
   * computes a layout, a default value, or a GC descriptor for one. Must
   * NOT run any earlier: a struct declared later in the same file/module
   * still needs to be in HoistedStructDecls_ for a forward-referencing
   * cycle (`struct A { b: B } struct B { a: A }`, B declared after A) to
   * be found at all.
   *
   * Edges in the graph: a field whose type is a struct (by name, via the
   * registry -- a field's parsed type is a NamedType placeholder, never
   * yet a resolved TurfStructType, at this point in the pipeline), an
   * array of a struct (`B[4]` contains 4 copies of `B`), or a GenericType
   * whose OWN base name is a registered struct (`Box<T>`'s `inner: Box<T>`
   * field is an edge to `Box` regardless of what `T` is -- see this
   * function's .cpp doc comment for why type arguments themselves are not
   * separately recursed into here). Deliberately NOT an edge: any pointer-
   * shaped field (`T*`, `&T`, `&?T`, `ptr` -- TurfType::isPointer() covers
   * all four via TurfReferenceType's inheritance from TurfPointerType, see
   * docs/architecture/pointers_and_references.md §2.1) -- a pointer field
   * is a fixed one word regardless of its pointee, which is exactly why it
   * is the correct, intended way to write a real recursive data structure
   * and must never be treated as containment.
   */
  void checkStructLayoutAcyclicity();

  /// Processes an on-demand import's requested symbols (ImportMode::Symbol/
  /// SymbolList/Wildcard) against the already-hoisted target module,
  /// registering each into ContextLabel's flat namespace ("" = main file,
  /// else the enclosing module doing the importing). No-op for
  /// ImportMode::Bare. Called right after hoistModuleBody() has fully
  /// populated the target module's own registrations, from both
  /// hoistTopLevel (ContextLabel="") and hoistModuleBody's own nested-
  /// import case (ContextLabel=the enclosing module's name).
  void registerOnDemandImports(const ModuleImportExprAST &Import,
                               const std::string &ContextLabel);

  /// Register one function's signature (name, params, return type) in
  /// SymTable_ under the given module namespace ("\'" = current file).
  void registerFunctionSignature(const FuncDefExprAST &Node,
                                 const std::string &ModuleName);

  /**
   * @brief Hoist a top-level `export immut NAME: TYPE = LITERAL;` into
   * SymTable_'s current-file or module constant registry (mirrors
   * registerFunctionSignature()'s empty-vs-named ModuleName split). Called
   * from both hoistTopLevel (ModuleName="") and hoistModuleBody
   * (ModuleName=the module being hoisted) so an exported constant is
   * visible to forward references regardless of which file it's declared
   * in, exactly like functions already are.
   *
   * Validates and raises here (not in visitVarDecl) because hoisting is
   * unconditional -- an invalid export (non-literal initializer, `export
   * mut`) must be caught even if the declaration is never subsequently
   * visited by Pass 2 due to an earlier unrelated parse error aborting
   * that statement's recovery.
   *
   * Does NOT check FunctionNestingDepth_ -- hoisting only ever walks
   * top-level nodes (see hoistTopLevel's own doc comment), so a `export`
   * written inside a function body never reaches this helper at all; that
   * case is instead caught by visitVarDecl in Pass 2 (the only pass that
   * actually descends into function bodies).
   */
  void hoistExportedConstant(const VarDeclExprAST &Var,
                             const std::string &ModuleName);

  /// Dispatch to @p Node if non-null (no-op otherwise). The single shared
  /// chokepoint EVERY Pass-2 child dispatch goes through -- see
  /// ScopeSnapshotCallback's doc comment above for why that makes this the
  /// right (and only) place to fire it.
  void visitChild(ExprAST *Node) {
    if (!Node)
      return;
    if (ScopeSnapshotCB_)
      ScopeSnapshotCB_(*Node, SymTable_.getAllVisibleNames());
    Node->accept(*this);
  }

  /// Convenience: dispatch a const child.
  void visitChild(const ExprAST *Node) {
    if (Node)
      visitChild(const_cast<ExprAST *>(Node));
  }
};

// Save (push) / restore (pop) the Resolver's per-compile state that
// outlives any one Resolver instance -- today only the set of functions
// already warned about in visitFuncDef(). Push leaves it empty. Called only
// by the query facade (src/AnalysisFacade.cpp), alongside
// PushParserCompileState() (Parser.h).
void PushResolverCompileState();
void PopResolverCompileState();

#endif // RESOLVER_H
