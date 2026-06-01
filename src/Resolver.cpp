#include "Resolver.h"
#include "AST.h"
#include "Errors.h"
#include "SuggestionIndex.h"

#include <memory>
#include <algorithm>

Resolver::Resolver(ResolverSymbolTable &SymTable) : SymTable_(SymTable) {}

void Resolver::run(ExprAST *Root) {
  if (!Root)
    return;
  Root->accept(*this);
}

// --- Basic visitors to recurse into child nodes ---
void Resolver::visitBlock(BlockExprAST &Node) {
  for (const auto &E : Node.getExpressions())
    if (E)
      E->accept(*this);
}

void Resolver::visitBinary(BinaryExprAST &Node) {
  if (const ExprAST *L = Node.getLHS())
    const_cast<ExprAST *>(L)->accept(*this);
  if (const ExprAST *R = Node.getRHS())
    const_cast<ExprAST *>(R)->accept(*this);
}

void Resolver::visitAssignment(AssignmentExprAST &Node) {
  if (const ExprAST *L = Node.getLHS())
    const_cast<ExprAST *>(L)->accept(*this);
  if (const ExprAST *R = Node.getRHS())
    const_cast<ExprAST *>(R)->accept(*this);
}

void Resolver::visitVarDecl(VarDeclExprAST &Node) {
  if (const ExprAST *Init = Node.getInitializer())
    const_cast<ExprAST *>(Init)->accept(*this);
  // declare variable in current scope
  SymTable_.declareVariable(Node.getName(), Node.getTypePtr(), {Node.getLoc().Line, Node.getLoc().Col});
}

void Resolver::visitIf(IfExprAST &Node) {
  for (const auto &B : Node.getBranches()) {
    if (B.Cond)
      const_cast<ExprAST *>(B.Cond.get())->accept(*this);
    if (B.Body)
      const_cast<ExprAST *>(B.Body.get())->accept(*this);
  }
  if (Node.getElseBody())
    const_cast<ExprAST *>(Node.getElseBody())->accept(*this);
}

void Resolver::visitWhile(WhileExprAST &Node) {
  if (const ExprAST *C = Node.getCond())
    const_cast<ExprAST *>(C)->accept(*this);
  if (ExprAST *B = const_cast<ExprAST *>(Node.getBody()))
    B->accept(*this);
}

void Resolver::visitFor(ForExprAST &Node) {
  if (const ExprAST *S = Node.getStart())
    const_cast<ExprAST *>(S)->accept(*this);
  if (const ExprAST *E = Node.getEnd())
    const_cast<ExprAST *>(E)->accept(*this);
  if (const ExprAST *St = Node.getStep())
    const_cast<ExprAST *>(St)->accept(*this);
  if (ExprAST *B = const_cast<ExprAST *>(Node.getBody()))
    B->accept(*this);
}

void Resolver::visitReturn(ReturnExprAST &Node) {
  if (const ExprAST *V = Node.getVal())
    const_cast<ExprAST *>(V)->accept(*this);
}

void Resolver::visitTry(TryExprAST &Node) {
  if (const ExprAST *T = Node.getTryBlock())
    const_cast<ExprAST *>(T)->accept(*this);
  if (const ExprAST *C = Node.getCatchBlock())
    const_cast<ExprAST *>(C)->accept(*this);
}

void Resolver::visitCast(CastExprAST &Node) {
  if (const ExprAST *Op = Node.getOperand())
    const_cast<ExprAST *>(Op)->accept(*this);
}

void Resolver::visitUnary(UnaryExprAST &Node) {
  if (const ExprAST *Op = Node.getOperand())
    const_cast<ExprAST *>(Op)->accept(*this);
}

void Resolver::visitNumber(NumberExprAST &Node) {}
void Resolver::visitString(StringExprAST &Node) {}
void Resolver::visitBool(BoolExprAST &Node) {}
void Resolver::visitNull(NullExprAST &Node) {}
void Resolver::visitVariable(VariableExprAST &Node) {}

// --- Function definition visitor ---
void Resolver::visitFuncDef(FuncDefExprAST &Node) {
  if (FunctionNestingDepth_ > 0) {
    // nested functions are not allowed
    LogErrorAt(Node.getLoc(), "Nested functions are not allowed");
    return;
  }

  ++FunctionNestingDepth_;
  SymTable_.enterScope();

  // Register parameters as variables and build ResolverFunctionSymbol
  ResolverFunctionSymbol Sym;
  Sym.BaseName = Node.getName();
  Sym.SourceModule = "";
  Sym.ReturnType = Node.getReturnTypePtr();
  Sym.DeclLoc = {Node.getLoc().Line, Node.getLoc().Col};
  Sym.IsIntrinsic = !Node.getIntrinsicName().empty();

  for (const auto &P : Node.getParams()) {
    // declare parameter variable
    SymTable_.declareVariable(P.Name, P.Type, {Node.getLoc().Line, Node.getLoc().Col});
    FunctionParam FP{P.Name, P.Type};
    Sym.Params.push_back(FP);
    Sym.ParamTypes.push_back(P.Type);
  }

  // Register in symbol table (tier-1)
  SymTable_.registerCurrentFileFunction(Sym);

  // Attach the last-registered overload to the node
  auto Overloads = SymTable_.lookupCurrentFileFunctions(Sym.BaseName);
  if (!Overloads.empty())
    Node.ResolvedDef = Overloads.back();

  // Recurse into body
  if (Node.getBody())
    const_cast<ExprAST *>(Node.getBody())->accept(*this);

  SymTable_.exitScope();
  --FunctionNestingDepth_;
}

// --- Function call visitor ---
void Resolver::visitFuncCall(FuncCallExprAST &Node) {
  // Recurse into callee and arguments
  if (const ExprAST *C = Node.getCallee())
    const_cast<ExprAST *>(C)->accept(*this);
  for (const auto &A : Node.getArgs())
    if (A)
      const_cast<ExprAST *>(A.get())->accept(*this);

  // Only handle simple identifier callees for now
  std::string CalleeName;
  if (auto *V = dynamic_cast<const VariableExprAST *>(Node.getCallee())) {
    CalleeName = V->getName();
  } else {
    return; // Complex callee (field access, call result) — leave to other passes
  }

  auto Tier1 = SymTable_.lookupCurrentFileFunctions(CalleeName);
  auto Tier2 = SymTable_.findInModules(CalleeName);

  if (Tier1.empty() && Tier2.empty()) {
    RankedSuggestions Suggestions;
    auto LocalNames = SymTable_.getAllCurrentFileFunctionNames();
    auto ModuleNames = SymTable_.getAllModuleFunctionNames();

    Suggestions.Local = SuggestionIndex::suggestVisible(CalleeName, LocalNames);

    // Module suggestions: suggest per-module matches
    for (const auto &M : SymTable_.getModuleFunctionNames("main")) {
      // noop placeholder to satisfy loop semantics
      (void)M;
    }

    // Build module suggestion map by asking SuggestionIndex across all module names
    // We will include up to 3 suggestions per module
    for (const auto &ModPair : SymTable_.getAllModuleFunctionNames()) {
      (void)ModPair; // handled below
    }

    // Simpler: global module suggestions list
    auto AllModuleNames = SymTable_.getAllModuleFunctionNames();
    auto ModSuggestions = SuggestionIndex::suggestVisible(CalleeName, AllModuleNames);
    if (!ModSuggestions.empty())
      Suggestions.Module["modules"] = ModSuggestions;

    throw LookupError(Node.getLoc(), CalleeName, Suggestions);
  }

  // Populate candidates
  Node.ResolvedCandidates.clear();
  for (auto &S : Tier1)
    Node.ResolvedCandidates.push_back(S);
  for (auto &S : Tier2)
    Node.ResolvedCandidates.push_back(S);
}

