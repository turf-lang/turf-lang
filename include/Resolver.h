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
   *        CompileBufferBestEffort(), CompileBuffer.h) and will never feed
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

  // ── All other node types — just recurse into children ─────────────────────

  void visitNumber(NumberExprAST &Node) override { (void)Node; }
  void visitBool(BoolExprAST &Node) override { (void)Node; }
  void visitChar(CharExprAST &Node) override { (void)Node; }
  void visitString(StringExprAST &Node) override { (void)Node; }
  void visitNull(NullExprAST &Node) override { (void)Node; }
  void visitDefaultInit(DefaultInitExprAST &Node) override { (void)Node; }

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
  void visitSizeOf(SizeOfExprAST &Node) override { (void)Node; }

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
   * into a position -> names index themselves (see
   * GetVisibleNamesAtPosition() in CompileBuffer.h for a ready-made
   * implementation) -- the Resolver itself stays storage-agnostic and has
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
  // The enclosing function's own `fn<...>` type parameter names, set for the
  // duration of visitFuncDef()'s body traversal and cleared on exit (a plain
  // member, not a stack, is safe since FunctionNestingDepth_ already forbids
  // nested function definitions -- see NestedFunctionError). Consulted by
  // visitVarDecl()'s validateNamedType() call so a bare generic type
  // parameter used as a local variable's declared type (`fn<T> f() { mut x:
  // T = ...; }`) isn't wrongly flagged as an unknown type.
  std::vector<std::string> CurrentFuncTypeParams_;

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
                         const std::vector<std::string> &TypeParams = {}) const;

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
  bool validateModuleQualifier(const std::string &Qualifier,
                               const std::string &TypeName,
                               SourceLocation Loc) const;

  // ── Pass 1: Hoisting ───────────────────────────────────────────────────────
  // Walk only the top-level list, registering struct names and function
  // signatures.  No bodies are visited in this pass.

  /// Hoist a single top-level node (struct name or function signature).
  void hoistTopLevel(const ExprAST *Node);

  /// Hoist an entire list of top-level nodes (convenience overload for
  /// resolveAll).
  void hoistTopLevel(const std::vector<std::unique_ptr<ExprAST>> &Nodes);

  /// Register all function signatures found inside an imported module body.
  /// Called by hoistTopLevel when it encounters a ModuleImportExprAST.
  void hoistModuleBody(const ExprAST *Body, const std::string &ModuleName);

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

#endif // RESOLVER_H
