/**
 * @file ResolverExpr.cpp
 * @brief Expression resolution: variable lookup, binary/unary/cast/assignment/compare operators.
 * @details Part of the Resolver name-resolution pass (Issue #18), split from
 *          the original monolithic Resolver.cpp by category (mirrors
 *          AST.h's ASTExpr/ASTStmt/ASTDecl/ASTAccess split -- see
 *          docs/architecture/compiler_pipeline.md).
 *
 * @author a2ys
 * @copyright Copyright (c) 2026 TurfLang contributors.  Apache License.
 */

#include "Resolver.h"
#include "SymbolTable.h"

#include <algorithm> // std::find

#include "AST.h"
#include "Builtins.h"        // LookupBuiltin — for skipping bare intrinsics in visitFuncCall
#include "Errors.h"          // ReferenceError, DuplicateDeclarationError
#include "ResolverSymbolTable.h"
#include "ResolverInternal.h"
#include "SuggestionIndex.h" // SuggestionIndex::suggestVisible
#include "Types.h"           // RankedSuggestions

#include <memory>
#include <unordered_set>


// Walk Variable (VarExpr) nodes: look up; emit ReferenceError on miss.
void Resolver::visitVariable(VariableExprAST &Node) {
  auto Sym = SymTable_.lookupVariable(Node.getName());
  if (Sym) {
    // Issue #5: attach the resolved symbol to the AST node so future passes
    // (TypeChecker, Codegen) can read it directly without a second lookup.
    Node.ResolvedSymbol = Sym;
    return;
  }

  // Bare reference to an `export immut` constant -- checked AFTER ordinary
  // scoped variables (a local shadowing an exported name is not an error,
  // same shadowing rule as everywhere else) and BEFORE the ReferenceError
  // fallback. Three sources, mirroring bare function-call resolution
  // exactly: this file's own exported constants (self-reference, always
  // visible, no import needed), a MODULE's own exported constants
  // referenced bare from within that same module's own body (e.g. net.tr's
  // buildSockAddr() using its own AF_INET bare -- the same self-reference
  // rule visitFuncCall already gives functions, via currentModuleName()),
  // then on-demand-imported ones (registerFlatConstantImport, via
  // `import module::CONST;`).
  if (const auto *Const = SymTable_.lookupCurrentFileConstant(Node.getName())) {
    Node.ResolvedConstant = Const;
    return;
  }
  const std::string &SelfModule = currentModuleName();
  if (!SelfModule.empty()) {
    if (const auto *Const = SymTable_.findModuleConstant(SelfModule, Node.getName())) {
      Node.ResolvedConstant = Const;
      return;
    }
  }
  if (const auto *Flat = SymTable_.lookupFlatConstantImport(SelfModule, Node.getName())) {
    if (const auto *Const = SymTable_.findModuleConstant(Flat->SourceModule, Node.getName())) {
      Node.ResolvedConstant = Const;
      return;
    }
  }

  // First-class function reference: a bare name that isn't a variable or
  // constant, but IS a real `fn` -- e.g. `double_it` in `mutable f: fn(int):
  // int = double_it;` or `apply(double_it, 5)`. Same self-file/self-module/
  // on-demand-import visibility rule an ordinary BARE CALL gets (mirrors the
  // three sources above and visitFuncCall's identical VarCallee lookup) --
  // NOT every function visible anywhere in the program. A name used in CALL
  // position never reaches this function at all (see ResolvedFunctionRef's
  // doc comment in AST.h), so this only ever fires for a function used AS A
  // VALUE.
  {
    std::vector<const ResolverFunctionSymbol *> Candidates =
        SymTable_.lookupCurrentFileFunctions(Node.getName());
    if (!SelfModule.empty()) {
      for (const auto &Match : SymTable_.findInModules(Node.getName()))
        if (Match.ModuleName == SelfModule)
          Candidates.push_back(Match.Symbol);
    }
    if (const auto *Flat = SymTable_.lookupFlatFunctionImport(SelfModule, Node.getName())) {
      for (const auto &Match : SymTable_.findInModules(Node.getName()))
        if (Match.ModuleName == Flat->SourceModule)
          Candidates.push_back(Match.Symbol);
    }
    if (Candidates.size() == 1) {
      Node.ResolvedFunctionRef =
          std::make_shared<ResolverFunctionSymbol>(*Candidates[0]);
      return;
    }
    if (Candidates.size() > 1) {
      TypeError(Node.getLoc(), "'" + Node.getName() +
                                   "' is overloaded -- it can't be used as a "
                                   "first-class function value without an "
                                   "unambiguous single signature.",
                "Give the variable a more specific function type so only "
                "one overload matches, or rename one of the overloads.",
                "Ambiguous reference!")
          .raise();
      return;
    }
  }

  // Build Damerau-Levenshtein suggestions from every name currently in scope.
  std::vector<std::string> VisibleNames = SymTable_.getAllVisibleNames();
  RankedSuggestions Suggestions;
  Suggestions.Local =
      SuggestionIndex::suggestVisible(Node.getName(), VisibleNames);

  ReferenceError(Node.getLoc(), Node.getName(), Suggestions).raise();
}


// ─── Structural visitors (recurse into children) ──────────────────────────────

void Resolver::visitBinary(BinaryExprAST &Node) {
  visitChild(Node.getLHS());
  visitChild(Node.getRHS());

  Node.ResolvedOperatorCandidates.clear();
  if (!isOverloadableBinaryOp(Node.getOp()))
    return; // bitwise/&&/||/** etc never go through operator resolution

  const std::string Key = operatorFunctionName(Node.getOp());

  // Self-module: lets a program declare `operator +(a: Vec2, b: Vec2)` in
  // its own file and use it there via bare `a + b`, mirroring
  // visitFuncCall's identical self-reference allowance for ordinary `fn`s.
  for (const auto *S : SymTable_.lookupCurrentFileFunctions(Key))
    Node.ResolvedOperatorCandidates.push_back(
        std::make_shared<ResolverFunctionSymbol>(*S));

  // Every other module: visible if the module is known/hoisted ANYWHERE in
  // this compile (Option A visibility -- confirmed with the user; see
  // STRING_UFCS_OPERATOR_CHAR_RFC.md §4.5's discussion). Operators have no
  // qualifier syntax (`a + b`, never `strings::+(a, b)`), so the strict
  // per-file on-demand-import gating ordinary bare `fn` calls get doesn't
  // apply here -- this instead matches how qualified `Module::func()` calls
  // already work (validateModuleQualifier() only checks the module exists
  // globally, not per-file).
  for (const auto &M : SymTable_.findInModules(Key))
    Node.ResolvedOperatorCandidates.push_back(
        std::make_shared<ResolverFunctionSymbol>(*M.Symbol));

  // No candidates found is NOT an error here -- e.g. `int + int` legitimately
  // has none and must fall through to the builtin arithmetic path. The
  // TypeChecker decides whether the absence of a candidate is actually an
  // error (no builtin match either).

  // LANG-001: this node's LHS is the REAL operand of a user-written `!x`
  // (see ResolvedNotOperatorCandidates's own doc comment, ASTExpr.h) --
  // gather unary `operator!` candidates against it too, same two-source
  // visibility as above. Harmless/empty for an ordinary literal `a != b`
  // (IsDesugaredNot is false there).
  Node.ResolvedNotOperatorCandidates.clear();
  if (Node.IsDesugaredNot) {
    const std::string NotKey = operatorFunctionName('!');
    for (const auto *S : SymTable_.lookupCurrentFileFunctions(NotKey))
      Node.ResolvedNotOperatorCandidates.push_back(
          std::make_shared<ResolverFunctionSymbol>(*S));
    for (const auto &M : SymTable_.findInModules(NotKey))
      Node.ResolvedNotOperatorCandidates.push_back(
          std::make_shared<ResolverFunctionSymbol>(*M.Symbol));
  }
}


void Resolver::visitUnary(UnaryExprAST &Node) {
  visitChild(Node.getOperand());

  Node.ResolvedOperatorCandidates.clear();
  if (!isOverloadableUnaryOp(Node.getOpcode()))
    return; // unary '+' (and anything else) never goes through operator resolution

  const std::string Key = operatorFunctionName(Node.getOpcode());

  // Same two-source visibility as visitBinary() above (self-file, then
  // every other known module) -- see its comment for the reasoning.
  for (const auto *S : SymTable_.lookupCurrentFileFunctions(Key))
    Node.ResolvedOperatorCandidates.push_back(
        std::make_shared<ResolverFunctionSymbol>(*S));
  for (const auto &M : SymTable_.findInModules(Key))
    Node.ResolvedOperatorCandidates.push_back(
        std::make_shared<ResolverFunctionSymbol>(*M.Symbol));
}


void Resolver::visitAddressOf(AddressOfExprAST &Node) {
  // Recurse so the operand variable is resolved before codegen reads it.
  visitChild(Node.getOperand());
}


void Resolver::visitAssignment(AssignmentExprAST &Node) {
  // Check RHS first, then LHS (which may be a variable that must exist).
  visitChild(Node.getRHS());
  visitChild(Node.getLHS());
}


void Resolver::visitCast(CastExprAST &Node) {
  visitChild(Node.getOperand());
}


void Resolver::visitCompoundAssignment(CompoundAssignmentExprAST &Node) {
  visitChild(Node.getRHS());
  visitChild(Node.getLHS());

  Node.ResolvedOperatorCandidates.clear();
  if (!isOverloadableBinaryOp(Node.getOpcode()))
    return; // bitwise/`**` compound forms have their own builtin-only rules

  const std::string Key = operatorFunctionName(Node.getOpcode());
  for (const auto *S : SymTable_.lookupCurrentFileFunctions(Key))
    Node.ResolvedOperatorCandidates.push_back(
        std::make_shared<ResolverFunctionSymbol>(*S));
  for (const auto &M : SymTable_.findInModules(Key))
    Node.ResolvedOperatorCandidates.push_back(
        std::make_shared<ResolverFunctionSymbol>(*M.Symbol));
}


void Resolver::visitPostfixExpr(PostfixExprAST &Node) {
  visitChild(Node.getOperand());

  // D21 (docs/development/changes/LANG-001.md): postfix `++`/`--` picks up
  // `operator+`/`operator-` the same way prefix's desugaring does, matched
  // against (OperandType, int) -- see the field's own doc comment (ASTExpr.h)
  // for why this needs its own gathering rather than reusing BinaryExprAST's.
  Node.ResolvedOperatorCandidates.clear();
  const std::string Key = operatorFunctionName(Node.getOpcode());
  for (const auto *S : SymTable_.lookupCurrentFileFunctions(Key))
    Node.ResolvedOperatorCandidates.push_back(
        std::make_shared<ResolverFunctionSymbol>(*S));
  for (const auto &M : SymTable_.findInModules(Key))
    Node.ResolvedOperatorCandidates.push_back(
        std::make_shared<ResolverFunctionSymbol>(*M.Symbol));
}


void Resolver::visitCompare(CompareExprAST &Node) {
  // Resolve the compare subject
  visitChild(Node.getSubjectMut());

  // Resolve each arm: patterns then body (in its own scope)
  for (auto &Arm : Node.getArmsMut()) {
    for (auto &Val : Arm.Values)
      visitChild(Val.get());
    // Each arm body gets its own scope so declarations don't leak between arms
    SymTable_.enterScope();
    visitChild(Arm.Body.get());
    SymTable_.exitScope();
  }

  // Resolve the otherwise body if present
  if (Node.getOtherBodyMut()) {
    SymTable_.enterScope();
    visitChild(Node.getOtherBodyMut());
    SymTable_.exitScope();
  }
}
