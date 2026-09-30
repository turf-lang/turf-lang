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
  // LEC-004: `err == IOError::NotFound` reads only which variant occurred, so
  // the bare reference may name a variant that has payload fields -- which no
  // other position may (visitScopeAccess()). Marked before the operands are
  // visited, and only for a DIRECT operand: `(f(IOError::NotFound)) == x` is
  // still a bare value.
  if (Node.getOp() == TOK_EQ || Node.getOp() == TOK_NEQ) {
    for (const ExprAST *Operand : {Node.getLHS(), Node.getRHS()})
      if (auto *SA = dynamic_cast<ScopeAccessExprAST *>(const_cast<ExprAST *>(Operand)))
        SA->IsTagOperand = true;
  }
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
  // FDN-003: `x to Node` -- the destination is a type spelling like any
  // annotation. Silent (never validated here before FDN-003; an unknown
  // destination is still the TypeChecker's cast-legality error).
  resolveTypeSpellingsSilently(Node.getDestTypePtr(), currentModuleName(),
                               CurrentFuncTypeParams_);
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

  // D21 (docs/development/changes/LANG-001.md): both `x++`/`x--` and
  // `++x`/`--x` (one shared node, see PostfixExprAST's own doc comment,
  // ASTExpr.h) pick up `operator+`/`operator-`, matched against
  // (OperandType, int) -- gathered here rather than reusing BinaryExprAST's
  // candidates since neither direction is a real BinaryExprAST any more
  // (the old prefix desugaring to `x = x + 1` is gone, LANG-001 follow-up).
  Node.ResolvedOperatorCandidates.clear();
  const std::string Key = operatorFunctionName(Node.getOpcode());
  for (const auto *S : SymTable_.lookupCurrentFileFunctions(Key))
    Node.ResolvedOperatorCandidates.push_back(
        std::make_shared<ResolverFunctionSymbol>(*S));
  for (const auto &M : SymTable_.findInModules(Key))
    Node.ResolvedOperatorCandidates.push_back(
        std::make_shared<ResolverFunctionSymbol>(*M.Symbol));
}


void Resolver::resolveArmPattern(
    ExprAST *Pat, MatchesArm &Arm,
    std::unordered_map<std::string, std::shared_ptr<VariableSymbol>> &ArmNames,
    std::vector<std::string> &HeaderNames) {
  if (!Pat)
    return;

  if (dynamic_cast<WildcardPatternExprAST *>(Pat))
    return;

  if (auto *B = dynamic_cast<BindingPatternExprAST *>(Pat)) {
    if (std::find(HeaderNames.begin(), HeaderNames.end(), B->getName()) !=
        HeaderNames.end()) {
      InvalidPatternError(
          B->getLoc(),
          "'" + B->getName() + "' is bound twice in the same pattern.",
          "A name can bind one value per pattern. Give the second one its "
          "own name, or write '_' to ignore it.")
          .raise();
      return;
    }
    HeaderNames.push_back(B->getName());

    auto Existing = ArmNames.find(B->getName());
    if (Existing != ArmNames.end()) {
      B->ResolvedVar = Existing->second;
      return;
    }
    // Immutable, like `immut`: a binding names a value, and assigning to it
    // would only change a private copy of the field. The type is a
    // placeholder until TypeChecker::visitCompare() writes the field's real
    // one in -- the contract ForEachExprAST's loop variable already uses.
    ResolverSourceLocation Loc{B->getLoc().Line, B->getLoc().Col};
    B->ResolvedVar = SymTable_.declareVariable(B->getName(), TurfInt(), Loc,
                                               /*IsImmutable=*/true);
    ArmNames[B->getName()] = B->ResolvedVar;
    Arm.Bindings.push_back(B->ResolvedVar);
    return;
  }

  if (auto *S = dynamic_cast<StructPatternExprAST *>(Pat)) {
    // The struct is a type SPELLING: validating it also rewrites it to its
    // TypeIdentity (FDN-003), and reports an unknown, ambiguous or private
    // type the way a type annotation would. A generic struct is named bare
    // (`Pair { a, b }`): its arguments come from the subject, not the pattern.
    validateNamedType(S->getTypeSpelling(), S->getLoc(), CurrentFuncTypeParams_,
                      /*AllowBareGeneric=*/true);

    std::vector<std::string> SeenFields;
    for (auto &F : S->getFieldsMut()) {
      if (std::find(SeenFields.begin(), SeenFields.end(), F.Name) !=
          SeenFields.end()) {
        InvalidPatternError(
            F.Loc, "field '" + F.Name + "' is listed twice in this pattern.",
            "Each field can appear once. To test a field and also keep its "
            "value, bind it and add a guard: `matches P { x } if x == 0:`.")
            .raise();
        continue;
      }
      SeenFields.push_back(F.Name);
      resolveArmPattern(F.Sub.get(), Arm, ArmNames, HeaderNames);
    }
    return;
  }

  // LEC-004: `Type::Variant(sub, sub, ..)`. The head is an ordinary scope
  // access (resolving it validates the type, its visibility and that the
  // variant exists, with suggestions), marked so a variant that has payload
  // fields is not rejected as a value -- a pattern head only reads its tag.
  // Each sub-pattern is then resolved exactly as a struct pattern's field is.
  if (auto *EV = dynamic_cast<ErrorVariantPatternExprAST *>(Pat)) {
    if (auto *Head = dynamic_cast<ScopeAccessExprAST *>(EV->getHeadMut())) {
      Head->IsTagOperand = true;
      visitChild(Head);
    }
    for (auto &Sub : EV->getSubsMut())
      resolveArmPattern(Sub.get(), Arm, ArmNames, HeaderNames);
    return;
  }

  // A value pattern: an ordinary expression (a literal, an enum variant, or
  // -- at the top of a header only -- any expression at all).
  visitChild(Pat);
}


void Resolver::normalizeHeaderPattern(std::unique_ptr<ExprAST> &Slot) {
  // `E::V(...)` where E is not an error type: the call it always was. Only a
  // plain expression survives the trip -- a binding turns back into the
  // variable it was spelled as -- and anything pattern-only (`_`, `..`, a
  // struct pattern) has no call to go back to.
  if (auto *EV = dynamic_cast<ErrorVariantPatternExprAST *>(Slot.get())) {
    auto *Head = dynamic_cast<ScopeAccessExprAST *>(EV->getHeadMut());
    if (!Head)
      return;
    resolveScopeLeftType(*Head, Head->getLeftLoc());
    if (SymTable_.isErrorTypeName(Head->getLeftName()))
      return;

    std::vector<std::unique_ptr<ExprAST>> Args;
    for (auto &Sub : EV->getSubsMut()) {
      if (auto *B = dynamic_cast<BindingPatternExprAST *>(Sub.get())) {
        Args.push_back(std::make_unique<VariableExprAST>(B->getLoc(), B->getName()));
      } else if (dynamic_cast<WildcardPatternExprAST *>(Sub.get()) ||
                 dynamic_cast<StructPatternExprAST *>(Sub.get()) ||
                 dynamic_cast<ErrorVariantPatternExprAST *>(Sub.get())) {
        InvalidPatternError(
            EV->getLoc(),
            "'" + Head->getLeftName() + "::" + Head->getRightName() +
                "(...)' is not an error variant, so it can't be destructured.",
            "Only an error type's variant takes a pattern in parentheses. If "
            "'" + Head->getRightName() + "' is a function, pass it plain "
            "arguments; if you meant an error, check the type's name.")
            .raise();
        return;
      } else {
        Args.push_back(std::move(Sub));
      }
    }
    if (EV->hasRest()) {
      InvalidPatternError(
          EV->getRestLoc(), "'..' is only meaningful in a pattern over an error variant.",
          "'" + Head->getLeftName() + "' is not an error type.")
          .raise();
      return;
    }
    SourceLocation Loc = EV->getLoc();
    Slot = std::make_unique<FuncCallExprAST>(Loc, EV->releaseHead(), std::move(Args));
    return;
  }

  // A bare `E::V` over an error type is a variant pattern with no
  // sub-patterns. (Whether it may be one -- a variant with fields must say
  // `E::V(..)` -- is the TypeChecker's rule, which knows the variant.)
  if (auto *SA = dynamic_cast<ScopeAccessExprAST *>(Slot.get())) {
    resolveScopeLeftType(*SA, SA->getLeftLoc());
    if (!SymTable_.isErrorTypeName(SA->getLeftName()))
      return;
    SourceLocation Loc = SA->getLoc();
    Slot = std::make_unique<ErrorVariantPatternExprAST>(
        Loc, std::move(Slot), std::vector<std::unique_ptr<ExprAST>>{},
        /*HasParens=*/false, /*HasRest=*/false, SourceLocation{0, 0});
  }
}


void Resolver::visitCompare(CompareExprAST &Node) {
  // Resolve the compare subject
  visitChild(Node.getSubjectMut());

  // Resolve each arm: patterns, guards, then body, all in the arm's own scope
  // so a binding is visible to its guard and body and to nothing else.
  for (auto &Arm : Node.getArmsMut()) {
    // Each arm body gets its own scope so declarations don't leak between arms
    SymTable_.enterScope();

    std::unordered_map<std::string, std::shared_ptr<VariableSymbol>> ArmNames;
    std::vector<std::string> FirstHeaderNames;
    for (size_t K = 0; K < Arm.Values.size(); ++K) {
      std::vector<std::string> HeaderNames;
      normalizeHeaderPattern(Arm.Values[K]);
      resolveArmPattern(Arm.Values[K].get(), Arm, ArmNames, HeaderNames);

      // Every stacked header feeds the ONE shared body, which reads one
      // symbol per name. A name one header binds and another does not would
      // leave the body reading a value that, on that path, was never bound.
      if (K == 0) {
        FirstHeaderNames = HeaderNames;
      } else {
        auto Sorted = [](std::vector<std::string> V) {
          std::sort(V.begin(), V.end());
          return V;
        };
        if (Sorted(HeaderNames) != Sorted(FirstHeaderNames)) {
          InvalidPatternError(
              Arm.Loc,
              "stacked `matches` headers must bind the same names, but this "
              "arm's headers bind different ones.",
              "The arms share one body, so every header has to bind exactly "
              "the same names (or none): either give each header the same "
              "bindings, or split them into separate arms.")
              .raise();
        }
      }
    }

    for (auto &G : Arm.Guards)
      visitChild(G.get());

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
