/**
 * @file Resolver.cpp
 * @brief Implementation of the Resolver name-resolution pass (Issue #4).
 *
 * Each visitXxx override below either:
 *   (a) performs semantic work specific to Issue #4 (VarDecl, Variable, Block),
 *       or
 *   (b) recurses into child nodes so that the three semantic handlers are
 *       reachable regardless of where they sit in the AST.
 *
 * This file *does* depend on Errors.h (and therefore on LLVM headers via the
 * transitive include chain), because it raises ReferenceError /
 * DuplicateDeclarationError which use the project's diagnostic engine.
 * The Resolver.h header itself is LLVM-free.
 *
 * @author a2ys
 * @copyright Copyright (c) 2026 TurfLang contributors.  Apache License.
 */

#include "Resolver.h"

#include "AST.h"
#include "Builtins.h"        // LookupBuiltin — for skipping bare intrinsics in visitFuncCall
#include "Errors.h"          // ReferenceError, DuplicateDeclarationError
#include "ResolverSymbolTable.h"
#include "SuggestionIndex.h" // SuggestionIndex::suggestVisible
#include "Types.h"           // RankedSuggestions

#include <memory>
#include <unordered_set>

// ─── Public entry points ──────────────────────────────────────────────────────

void Resolver::resolve(ExprAST *Node) {
  if (Node)
    Node->accept(*this);
}

void Resolver::resolveAll(
    const std::vector<std::unique_ptr<ExprAST>> &Nodes) {
  predeclareFunctions(Nodes);
  for (const auto &Node : Nodes)
    resolve(Node.get());
}

ResolverFunctionSymbol
Resolver::buildFunctionSymbol(const FuncDefExprAST &Node,
                              const std::string &ModuleName) const {
  ResolverFunctionSymbol Sym;
  Sym.BaseName = Node.getName();
  Sym.SourceModule = ModuleName;
  Sym.Params.reserve(Node.getParams().size());
  for (const auto &Param : Node.getParams()) {
    Sym.Params.push_back({Param.Name, Param.Type});
  }
  Sym.ReturnType = Node.getReturnTypePtr();
  ResolverSourceLocation DeclLoc;
  DeclLoc.Line = Node.getLoc().Line;
  DeclLoc.Col = Node.getLoc().Col;
  Sym.DeclLoc = DeclLoc;
  Sym.IsIntrinsic = !Node.getIntrinsicName().empty();
  return Sym;
}

void Resolver::registerFunctionSignature(const FuncDefExprAST &Node,
                                         const std::string &ModuleName) {
  auto Sym = buildFunctionSymbol(Node, ModuleName);
  if (ModuleName.empty()) {
    SymTable_.registerCurrentFileFunction(Sym);
  } else {
    SymTable_.registerModuleFunction(ModuleName, Sym);
  }
}

void Resolver::predeclareFunctions(
    const std::vector<std::unique_ptr<ExprAST>> &Nodes) {
  for (const auto &Node : Nodes) {
    predeclareFunctions(Node.get());
  }
}

void Resolver::predeclareModuleBody(const ExprAST *Body,
                                    const std::string &ModuleName) {
  const auto *Block = dynamic_cast<const BlockExprAST *>(Body);
  if (!Block)
    return;

  for (const auto &Expr : Block->getExpressions()) {
    if (const auto *Func = dynamic_cast<const FuncDefExprAST *>(Expr.get())) {
      registerFunctionSignature(*Func, ModuleName);
    } else if (const auto *Import = dynamic_cast<const ModuleImportExprAST *>(Expr.get())) {
      predeclareModuleBody(Import->getBody(), Import->getModuleName());
    }
  }
}

void Resolver::predeclareFunctions(const ExprAST *Node) {
  if (!Node)
    return;

  if (auto *Func = dynamic_cast<const FuncDefExprAST *>(Node)) {
    registerFunctionSignature(*Func, currentModuleName());
    return;
  }

  if (auto *Import = dynamic_cast<const ModuleImportExprAST *>(Node)) {
    predeclareModuleBody(Import->getBody(), Import->getModuleName());
  }
}

// ─── Core Issue #4 visitors ───────────────────────────────────────────────────

// Walk VarDecl nodes: recurse into the initializer first (so the RHS can't
// see the variable being declared, matching Turf's semantics), then add the
// name to the current scope.
void Resolver::visitVarDecl(VarDeclExprAST &Node) {
  // 1. Resolve the initializer expression before registering the new name.
  //    This prevents `int x = x;` from passing the resolver.
  visitChild(Node.getInitializer());

  // 2. Register in the current scope.
  //    declareVariable() throws std::runtime_error on re-declaration in the
  //    same scope; we catch it and re-raise as a proper compiler diagnostic.
  //    On success it returns the shared_ptr we attach to the AST node (#5).
  try {
    ResolverSourceLocation Loc{Node.getLoc().Line, Node.getLoc().Col};
    Node.ResolvedSymbol =
        SymTable_.declareVariable(Node.getName(), Node.getTypePtr(), Loc, Node.isImmutable());
  } catch (const std::runtime_error &) {
    // The name is already in the current scope — emit a rich diagnostic.
    // Look up the previous declaration for the "declared at line N" note.
    auto Prev = SymTable_.lookupVariable(Node.getName());
    if (Prev) {
      SourceLocation PrevLoc{Prev->DeclLoc.Line, Prev->DeclLoc.Col};
      DuplicateDeclarationError(Node.getLoc(), Node.getName(), PrevLoc).raise();
    } else {
      // Fallback (should not happen, but be safe).
      SyntaxError(Node.getLoc(), "Variable '" + Node.getName() +
                                     "' is already declared in this scope.")
          .raise();
    }
  }
}

// Walk Block nodes: bracket child traversal with enterScope/exitScope.
void Resolver::visitBlock(BlockExprAST &Node) {
  SymTable_.enterScope();
  for (const auto &Expr : Node.getExpressions())
    visitChild(Expr.get());
  SymTable_.exitScope();
}

// Walk Variable (VarExpr) nodes: look up; emit ReferenceError on miss.
void Resolver::visitVariable(VariableExprAST &Node) {
  auto Sym = SymTable_.lookupVariable(Node.getName());
  if (Sym) {
    // Issue #5: attach the resolved symbol to the AST node so future passes
    // (TypeChecker, Codegen) can read it directly without a second lookup.
    Node.ResolvedSymbol = Sym;
    return;
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
}

void Resolver::visitUnary(UnaryExprAST &Node) {
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

void Resolver::visitIf(IfExprAST &Node) {
  for (const auto &Branch : Node.getBranches()) {
    visitChild(Branch.Cond.get());
    visitChild(Branch.Body.get());
  }
  visitChild(Node.getElseBody());
}

void Resolver::visitWhile(WhileExprAST &Node) {
  visitChild(Node.getCond());
  visitChild(Node.getBody());
}

void Resolver::visitFor(ForExprAST &Node) {
  // ForExprAST desugars to:  { <var> = start;  while <var> < end { body; step } }
  // The Resolver mirrors that structure:
  //   1. Resolve start and end expressions in the *outer* scope (they can't
  //      reference the loop variable itself — it doesn't exist yet).
  //   2. Push a new scope for the loop variable + body + step.
  //   3. Register the induction variable as int (Codegen infers int from the
  //      start value, which is the same rule we apply here).
  //   4. Resolve the body and step inside that scope.
  //   5. Exit the scope so the loop variable doesn't leak.

  visitChild(Node.getStart());
  visitChild(Node.getEnd());

  SymTable_.enterScope();

  // Register the induction variable as int — mirrors Codegen's type inference.
  // If the start expression is a double literal the TypeChecker (Issue #7)
  // will refine this, but int is the right default for range-based loops.
  // Capture the returned symbol and attach it to the AST node (Issue #5).
  try {
    ResolverSourceLocation Loc{Node.getLoc().Line, Node.getLoc().Col};
    Node.ResolvedLoopVar =
        SymTable_.declareVariable(Node.getVarName(), TurfInt(), Loc);
  } catch (const std::runtime_error &) {
    auto Prev = SymTable_.lookupVariable(Node.getVarName());
    SourceLocation ErrLoc = Node.getLoc();
    if (Prev) {
      SourceLocation PrevLoc{Prev->DeclLoc.Line, Prev->DeclLoc.Col};
      DuplicateDeclarationError(ErrLoc, Node.getVarName(), PrevLoc).raise();
    } else {
      SyntaxError(ErrLoc, "Loop variable '" + Node.getVarName() +
                               "' conflicts with an existing declaration.")
          .raise();
    }
  }

  // Step must be resolved inside the scope so it can reference the loop var
  // (e.g. `step i += 1` uses i).
  visitChild(Node.getStep());
  visitChild(Node.getBody());

  SymTable_.exitScope();
}

void Resolver::visitReturn(ReturnExprAST &Node) {
  visitChild(Node.getVal());
}

void Resolver::visitFuncDef(FuncDefExprAST &Node) {
  if (FunctionNestingDepth_ > 0) {
    NestedFunctionError(Node.getLoc(), Node.getName()).raise();
    return;
  }

  ++FunctionNestingDepth_;

  // Each function body gets its own scope; parameters are visible inside it.
  SymTable_.enterScope();

  for (const auto &Param : Node.getParams()) {
    try {
      // Parameters have no source location in the AST right now; use {0,0}.
      SymTable_.declareVariable(Param.Name, Param.Type, {});
    } catch (const std::runtime_error &) {
      // Duplicate parameter name — let it through; Parser should have caught
      // this already, but be defensive.
    }
  }

  auto Sym = std::make_shared<ResolverFunctionSymbol>(
      buildFunctionSymbol(Node, currentModuleName()));
  Node.ResolvedDef = Sym;

  registerFunctionSignature(Node, currentModuleName());

  visitChild(Node.getBody());

  SymTable_.exitScope();
  --FunctionNestingDepth_;
}

void Resolver::visitFuncCall(FuncCallExprAST &Node) {
  // Resolve argument expressions first so any nested variable lookups happen
  // before we inspect the callee.
  for (const auto &Arg : Node.getArgs())
    visitChild(Arg.get());

  Node.ResolvedCandidates.clear();

  const auto *VarCallee = dynamic_cast<const VariableExprAST *>(Node.getCallee());
  if (!VarCallee) {
    visitChild(Node.getCallee());
    return;
  }

  const std::string &CalleeName = VarCallee->getName();

  auto CurrentFileMatches = SymTable_.lookupCurrentFileFunctions(CalleeName);
  auto ModuleMatches = SymTable_.findInModules(CalleeName);

  if (CurrentFileMatches.empty() && ModuleMatches.empty()) {
    // Before emitting an error, check whether the callee is a true language-level
    // bare intrinsic — one that is callable WITHOUT a module qualifier AND has no
    // module home in the standard library.
    //
    // As of now there are exactly two such intrinsics:
    //   - typeof(x)   — compile-time type name, no module
    //   - lengthof(x) — string length, no module
    //
    // Everything else comes from lib/std:
    //   - print / printline / input  → io.tr   (call as io.printline(...))
    //   - malloc / realloc / free    → mem.tr   (call as mem.malloc(...))
    //   - abs / power / sqrt / …     → math.tr  (call as math.abs(...))
    //
    // Those stdlib functions are found by findInModules() above (when their
    // module is imported), so they never reach this branch.  Do NOT add them
    // here — they should produce a proper "did you mean io.printline?" error
    // when called without a qualifier.
    static const std::unordered_set<std::string> BareIntrinsics = {
        "typeof",
        "lengthof",
        "assert",
        "fail",
    };
    if (BareIntrinsics.count(CalleeName)) {
      // True bare intrinsic — skip error, TypeChecker validates arity/types.
      return;
    }

    RankedSuggestions Suggestions;

    auto CurrentNames = SuggestionIndex::suggestVisible(
        CalleeName, SymTable_.getAllCurrentFileFunctionNames());
    for (const auto &SuggestedName : CurrentNames) {
      for (const auto *Sym : SymTable_.lookupCurrentFileFunctions(SuggestedName)) {
        Suggestions.Local.push_back(Sym->signature());
      }
    }

    auto ModuleNames = SuggestionIndex::suggestVisible(
        CalleeName, SymTable_.getAllModuleFunctionNames());
    for (const auto &SuggestedName : ModuleNames) {
      for (const auto &Match : SymTable_.findInModules(SuggestedName)) {
        Suggestions.Module[Match.ModuleName].push_back(Match.Symbol->signature());
      }
    }

    UnknownFunctionError(Node.getLoc(), CalleeName, Suggestions).raise();
    return;
  }

  // ── Arity check ───────────────────────────────────────────────────────────
  // The symbol table already knows every overload's arity.  Before handing
  // the candidates to the TypeChecker, verify that at least one overload
  // accepts the number of arguments supplied at this call site.
  //
  // This is the authoritative arity check — no hardcoded maps anywhere.
  // WrongArityError fires here; Codegen never sees an arity mismatch.
  const int ArgCount = static_cast<int>(Node.getArgs().size());
  std::vector<std::string> AllSignatures;

  for (const auto *Sym : CurrentFileMatches)
    AllSignatures.push_back(Sym->signature());
  for (const auto &Match : ModuleMatches)
    AllSignatures.push_back(Match.Symbol->signature());

  bool AnyArityMatch = false;
  for (const auto *Sym : CurrentFileMatches) {
    if (Sym->arity() == ArgCount) { AnyArityMatch = true; break; }
  }
  if (!AnyArityMatch) {
    for (const auto &Match : ModuleMatches) {
      if (Match.Symbol->arity() == ArgCount) { AnyArityMatch = true; break; }
    }
  }

  if (!AnyArityMatch) {
    WrongArityError(Node.getLoc(), CalleeName, ArgCount, AllSignatures).raise();
    return;
  }

  // Populate ResolvedCandidates with only the arity-matching overloads.
  // TypeChecker::visitFuncCall then narrows further by type compatibility.
  for (const auto *Sym : CurrentFileMatches) {
    if (Sym->arity() == ArgCount)
      Node.ResolvedCandidates.push_back(
          std::make_shared<ResolverFunctionSymbol>(*Sym));
  }
  for (const auto &Match : ModuleMatches) {
    if (Match.Symbol->arity() == ArgCount)
      Node.ResolvedCandidates.push_back(
          std::make_shared<ResolverFunctionSymbol>(*Match.Symbol));
  }

  // ── Set ResolvedCallTarget when unambiguous ───────────────────────────────
  // If exactly one overload matches the arity, the Resolver can commit to it
  // immediately.  Codegen reads ResolvedCallTarget blindly (Issue #9) — this
  // ensures it's always set for the common single-overload case, even when
  // the TypeChecker hasn't run.
  //
  // Multiple arity-matching candidates means the function has type-overloads
  // (e.g. add(int,int) and add(double,double)).  In that case TypeChecker
  // narrows to one via type compatibility and sets ResolvedCallTarget itself.
  if (Node.ResolvedCandidates.size() == 1) {
    Node.ResolvedCallTarget = Node.ResolvedCandidates[0];
  }
}

void Resolver::visitMethodCall(MethodCallExprAST &Node) {
  // If the base is a plain identifier that names a known imported module
  // (e.g. `io` in `io.printline(...)`), skip variable-resolution on it.
  // Module names live in the module registry, not the variable scope stack,
  // so calling visitVariable("io") would always produce a spurious
  // ReferenceError.  The module import visitor already populated the Tier-2
  // registry; nothing more needs to be done for the base in this case.
  //
  // We do, however, mark the base node with its resolved module name so
  // that the ASTPrinter can show a proper badge (Issue #5 completeness).
  auto *BaseVar = dynamic_cast<VariableExprAST *>(
      const_cast<ExprAST *>(Node.getBase()));
  if (BaseVar && SymTable_.hasModule(BaseVar->getName())) {
    // Base is a module qualifier — annotate it and skip variable resolution.
    BaseVar->ResolvedModuleName = BaseVar->getName();
  } else {
    // Base is either a complex expression or a plain variable — resolve normally.
    visitChild(Node.getBase());
  }
  // Always resolve argument expressions regardless.
  for (const auto &Arg : Node.getArgs())
    visitChild(Arg.get());

  // ── Module method arity check ─────────────────────────────────────────────
  // If the base is a known module, look up the method in the module registry
  // and verify that at least one overload accepts the supplied argument count.
  // This mirrors the arity check in visitFuncCall — all done from symbol table
  // data, no hardcoded maps.
  if (BaseVar && SymTable_.hasModule(BaseVar->getName())) {
    const std::string &ModuleName = BaseVar->getName();
    const std::string &MethodName = Node.getMethod();
    const int ArgCount = static_cast<int>(Node.getArgs().size());

    // Look up all overloads of this method in the module.
    auto ModuleMatches = SymTable_.findInModules(MethodName);
    // Filter to those belonging to this specific module.
    std::vector<const ResolverFunctionSymbol *> MethodOverloads;
    for (const auto &Match : ModuleMatches) {
      if (Match.ModuleName == ModuleName)
        MethodOverloads.push_back(Match.Symbol);
    }

    if (!MethodOverloads.empty()) {
      // Method name exists in this module — check arity.
      bool AnyArityMatch = false;
      std::vector<std::string> AllSigs;
      std::vector<const ResolverFunctionSymbol *> ArityMatches;
      for (const auto *Sym : MethodOverloads) {
        AllSigs.push_back(ModuleName + "." + Sym->signature());
        if (Sym->arity() == ArgCount) {
          AnyArityMatch = true;
          ArityMatches.push_back(Sym);
        }
      }
      if (!AnyArityMatch) {
        WrongArityError(Node.getLoc(),
                        ModuleName + "." + MethodName,
                        ArgCount, AllSigs).raise();
        return;
      }

      // ── Set ResolvedCallTarget for the unambiguous case ──────────────────
      // When exactly one overload matches the arity, set it directly so that
      // Codegen can read mangledName() in the pre-pass before TypeChecker runs.
      // TypeChecker handles the multi-overload narrowing via type compatibility.
      if (ArityMatches.size() == 1) {
        Node.ResolvedCallTarget = std::shared_ptr<ResolverFunctionSymbol>(
            const_cast<ResolverFunctionSymbol *>(ArityMatches[0]),
            [](ResolverFunctionSymbol *) { /* RST owns this */ });
      }
    } else {
      // Method does not exist in this module.
      RankedSuggestions Suggestions;
      std::vector<std::string> ModuleNames = SuggestionIndex::suggestVisible(
          MethodName, SymTable_.getModuleFunctionNames(ModuleName));
      
      for (const auto &SuggestedName : ModuleNames) {
        auto Matches = SymTable_.findInModules(SuggestedName);
        for (const auto &Match : Matches) {
          if (Match.ModuleName == ModuleName) {
            Suggestions.Module[ModuleName].push_back(Match.Symbol->signature());
          }
        }
      }
      
      UnknownFunctionError(Node.getLoc(), ModuleName + "." + MethodName, Suggestions).raise();
      return;
    }
  }
}

void Resolver::visitFieldAccess(FieldAccessExprAST &Node) {
  visitChild(Node.getBase());
}

void Resolver::visitStructInit(StructInitExprAST &Node) {
  for (const auto &[FieldName, Val] : Node.getNamedFields())
    visitChild(Val.get());
}

void Resolver::visitArrayDecl(ArrayDeclExprAST &Node) {
  // Resolve the initializer list expressions.
  for (const auto &Init : Node.getInitList())
    visitChild(Init.get());

  // Register the array variable in the current scope.
  // Capture the returned symbol and attach it to the AST node (Issue #5).
  try {
    ResolverSourceLocation Loc{Node.getLoc().Line, Node.getLoc().Col};
    Node.ResolvedSymbol =
        SymTable_.declareVariable(Node.getName(), Node.getElementTypePtr(), Loc, Node.isImmutable());
  } catch (const std::runtime_error &) {
    auto Prev = SymTable_.lookupVariable(Node.getName());
    if (Prev) {
      SourceLocation PrevLoc{Prev->DeclLoc.Line, Prev->DeclLoc.Col};
      DuplicateDeclarationError(Node.getLoc(), Node.getName(), PrevLoc).raise();
    }
  }
}

void Resolver::visitArrayAccess(ArrayAccessExprAST &Node) {
  visitChild(Node.getBase());
  visitChild(Node.getIndex());
}

void Resolver::visitArrayAssign(ArrayAssignExprAST &Node) {
  // The array being assigned to must already be in scope.
  auto Sym = SymTable_.lookupVariable(Node.getName());
  if (!Sym) {
    std::vector<std::string> VisibleNames = SymTable_.getAllVisibleNames();
    RankedSuggestions Suggestions;
    Suggestions.Local =
        SuggestionIndex::suggestVisible(Node.getName(), VisibleNames);
    ReferenceError(Node.getLoc(), Node.getName(), Suggestions).raise();
  }
  Node.ResolvedSymbol = Sym;
  visitChild(Node.getIndex());
  visitChild(Node.getRHS());
}

void Resolver::visitModuleImport(ModuleImportExprAST &Node) {
  ModuleStack_.push_back(Node.getModuleName());
  visitChild(Node.getBody());
  ModuleStack_.pop_back();
}
