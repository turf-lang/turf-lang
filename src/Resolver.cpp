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
#include "Errors.h"          // ReferenceError, DuplicateDeclarationError
#include "ResolverSymbolTable.h"
#include "SuggestionIndex.h" // SuggestionIndex::suggestVisible
#include "Types.h"           // RankedSuggestions

// ─── Public entry points ──────────────────────────────────────────────────────

void Resolver::resolve(ExprAST *Node) {
  if (Node)
    Node->accept(*this);
}

void Resolver::resolveAll(
    const std::vector<std::unique_ptr<ExprAST>> &Nodes) {
  for (const auto &Node : Nodes)
    resolve(Node.get());
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
  try {
    ResolverSourceLocation Loc{Node.getLoc().Line, Node.getLoc().Col};
    SymTable_.declareVariable(Node.getName(), Node.getTypePtr(), Loc);
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
  if (SymTable_.lookupVariable(Node.getName()))
    return; // Found — all good.

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
  try {
    ResolverSourceLocation Loc{Node.getLoc().Line, Node.getLoc().Col};
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

void Resolver::visitRaise(RaiseExprAST &Node) {
  visitChild(Node.getPayload());
}

void Resolver::visitTry(TryExprAST &Node) {
  // Try block
  visitChild(Node.getTryBlock());

  // The error-payload variable is scoped to the catch block.
  SymTable_.enterScope();
  if (!Node.getPayloadName().empty()) {
    try {
      ResolverSourceLocation Loc{Node.getLoc().Line, Node.getLoc().Col};
      SymTable_.declareVariable(Node.getPayloadName(), nullptr, Loc);
    } catch (const std::runtime_error &) {
      // Ignore duplicate (e.g. nested try with the same payload name is rare
      // but permitted by shadowing rules).
    }
  }
  visitChild(Node.getCatchBlock());
  SymTable_.exitScope();
}

void Resolver::visitFuncDef(FuncDefExprAST &Node) {
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

  visitChild(Node.getBody());

  SymTable_.exitScope();
}

void Resolver::visitFuncCall(FuncCallExprAST &Node) {
  // The callee of a direct function call is a VariableExprAST whose name is
  // the function name — it lives in the function namespace, NOT the variable
  // scope.  Resolving function names against the variable table would produce
  // false "Unknown Variable" errors for every call like add(x, y).
  // Function-name resolution is deferred to Issue #6 (module/function resolver).
  //
  // We do still recurse into all argument expressions, because those ARE
  // variable references that need to be in scope.
  for (const auto &Arg : Node.getArgs())
    visitChild(Arg.get());
}

void Resolver::visitMethodCall(MethodCallExprAST &Node) {
  visitChild(Node.getBase());
  for (const auto &Arg : Node.getArgs())
    visitChild(Arg.get());
}

void Resolver::visitFieldAccess(FieldAccessExprAST &Node) {
  visitChild(Node.getBase());
}

void Resolver::visitStructInit(StructInitExprAST &Node) {
  for (const auto &Arg : Node.getArgs())
    visitChild(Arg.get());
}

void Resolver::visitArrayDecl(ArrayDeclExprAST &Node) {
  // Resolve the initializer list expressions.
  for (const auto &Init : Node.getInitList())
    visitChild(Init.get());

  // Register the array variable in the current scope.
  try {
    ResolverSourceLocation Loc{Node.getLoc().Line, Node.getLoc().Col};
    SymTable_.declareVariable(Node.getName(), Node.getElementTypePtr(), Loc);
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
  if (!SymTable_.lookupVariable(Node.getName())) {
    std::vector<std::string> VisibleNames = SymTable_.getAllVisibleNames();
    RankedSuggestions Suggestions;
    Suggestions.Local =
        SuggestionIndex::suggestVisible(Node.getName(), VisibleNames);
    ReferenceError(Node.getLoc(), Node.getName(), Suggestions).raise();
  }
  visitChild(Node.getIndex());
  visitChild(Node.getRHS());
}

void Resolver::visitModuleImport(ModuleImportExprAST &Node) {
  // Recurse into the body (the parsed module namespace contents).
  visitChild(Node.getBody());
}
