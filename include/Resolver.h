#ifndef RESOLVER_H
#define RESOLVER_H

#include "ASTVisitor.h"
#include "ResolverSymbolTable.h"

class Resolver : public ASTVisitor {
public:
  explicit Resolver(ResolverSymbolTable &SymTable);

  // Entry: run resolver over an AST root node
  void run(ExprAST *Root);

  // Visitor overrides
  void visitBlock(BlockExprAST &Node) override;
  void visitBinary(BinaryExprAST &Node) override;
  void visitAssignment(AssignmentExprAST &Node) override;
  void visitVarDecl(VarDeclExprAST &Node) override;
  void visitIf(IfExprAST &Node) override;
  void visitWhile(WhileExprAST &Node) override;
  void visitFor(ForExprAST &Node) override;
  void visitReturn(ReturnExprAST &Node) override;
  void visitTry(TryExprAST &Node) override;
  void visitFuncDef(FuncDefExprAST &Node) override;
  void visitFuncCall(FuncCallExprAST &Node) override;
  void visitCast(CastExprAST &Node) override;
  void visitUnary(UnaryExprAST &Node) override;
  void visitNumber(NumberExprAST &Node) override;
  void visitString(StringExprAST &Node) override;
  void visitBool(BoolExprAST &Node) override;
  void visitNull(NullExprAST &Node) override;
  void visitVariable(VariableExprAST &Node) override;

private:
  ResolverSymbolTable &SymTable_;
  int FunctionNestingDepth_ = 0;
};

#endif // RESOLVER_H
