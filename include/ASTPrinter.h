/**
 * @file ASTPrinter.h
 * @brief A demo ASTVisitor that pretty-prints the AST.
 *
 * This is the acceptance-criteria deliverable for Issue #1: a concrete
 * ASTVisitor subclass that traverses the entire AST and prints a human-readable
 * tree structure without crashing.
 *
 * The printer uses child-dispatch helpers (visitChildren*) to recurse into
 * sub-nodes so that every override is kept short and readable.
 *
 * Usage:
 * @code
 *   ASTPrinter printer;
 *   printer.print(rootNode);   // prints to stdout
 * @endcode
 *
 * @author a2ys
 * @copyright Copyright (c) 2026 TurfLang contributors.  Apache License.
 */

#ifndef AST_PRINTER_H
#define AST_PRINTER_H

#include "AST.h"
#include "ASTVisitor.h"
#include "TypeChecker.h"

#include <iostream>
#include <string>

class ASTPrinter : public ASTVisitor {
public:
  // ── Public entry point ──────────────────────────────────────────────────────
  /// Print the entire subtree rooted at @p Node to stdout.
  void print(ExprAST *Node, int Depth = 0) {
    if (!Node) {
      printIndent(Depth);
      std::cout << "<null>\n";
      return;
    }
    Depth_ = Depth;
    Node->accept(*this);
  }

  /// Attach a TypeChecker instance.  When set, every node that has an inferred
  /// type will have a magenta [: type] annotation appended after its badge.
  /// The TypeChecker must outlive the ASTPrinter.
  void setTypeChecker(const TypeChecker *TC) { TC_ = TC; }

  // ── Literals ────────────────────────────────────────────────────────────────
  void visitNumber(NumberExprAST &N) override {
    printIndent(Depth_);
    if (N.isInteger())
      std::cout << "NumberExpr(int:" << N.getIntVal() << ")";
    else
      std::cout << "NumberExpr(double:" << N.getDoubleVal() << ")";
    printTypeBadge(&N);
    std::cout << "\n";
  }

  void visitBool(BoolExprAST &N) override {
    printIndent(Depth_);
    std::cout << "BoolExpr(" << (N.getVal() ? "true" : "false") << ")";
    printTypeBadge(&N);
    std::cout << "\n";
  }

  void visitString(StringExprAST &N) override {
    printIndent(Depth_);
    std::cout << "StringExpr(\"" << N.getValue() << "\")";
    printTypeBadge(&N);
    std::cout << "\n";
  }

  void visitNull(NullExprAST &N) override {
    printIndent(Depth_);
    std::cout << "NullExpr";
    printTypeBadge(&N);
    std::cout << "\n";
  }

  // ── Variables ───────────────────────────────────────────────────────────────
  void visitVariable(VariableExprAST &N) override {
    printIndent(Depth_);
    std::cout << "VariableExpr(" << N.getName() << ")";
    if (!N.ResolvedModuleName.empty())
      printModuleBadge(N.ResolvedModuleName);
    else
      printResolvedBadge(N.ResolvedSymbol.get());
    printTypeBadge(&N);
    std::cout << "\n";
  }

  void visitVarDecl(VarDeclExprAST &N) override {
    printIndent(Depth_);
    std::cout << "VarDecl(" << N.getName() << " : "
              << (N.getType() ? N.getType()->getName() : "?") << ")";
    printResolvedBadge(N.ResolvedSymbol.get());
    printTypeBadge(&N);
    std::cout << "\n";
    if (N.getInitializer()) {
      descend();
      print(const_cast<ExprAST *>(N.getInitializer()), Depth_);
      ascend();
    }
  }

  // ── Expressions ─────────────────────────────────────────────────────────────
  void visitBinary(BinaryExprAST &N) override {
    printIndent(Depth_);
    std::cout << "BinaryExpr(op='" << static_cast<char>(N.getOp()) << "')";
    printTypeBadge(&N);
    std::cout << "\n";
    descend();
    print(const_cast<ExprAST *>(N.getLHS()), Depth_);
    print(const_cast<ExprAST *>(N.getRHS()), Depth_);
    ascend();
  }

  void visitUnary(UnaryExprAST &N) override {
    printIndent(Depth_);
    std::cout << "UnaryExpr(op='" << static_cast<char>(N.getOpcode()) << "')\n";
    descend();
    print(const_cast<ExprAST *>(N.getOperand()), Depth_);
    ascend();
  }

  void visitAssignment(AssignmentExprAST &N) override {
    printIndent(Depth_);
    std::cout << "AssignmentExpr\n";
    descend();
    print(const_cast<ExprAST *>(N.getLHS()), Depth_);
    print(const_cast<ExprAST *>(N.getRHS()), Depth_);
    ascend();
  }

  void visitCast(CastExprAST &N) override {
    printIndent(Depth_);
    std::cout << "CastExpr(to:"
              << (N.getDestType() ? N.getDestType()->getName() : "?") << ")\n";
    descend();
    print(const_cast<ExprAST *>(N.getOperand()), Depth_);
    ascend();
  }

  // ── Control flow ────────────────────────────────────────────────────────────
  void visitBlock(BlockExprAST &N) override {
    printIndent(Depth_);
    std::cout << "BlockExpr(" << N.getExpressions().size() << " stmts)\n";
    descend();
    for (const auto &Expr : N.getExpressions())
      print(Expr.get(), Depth_);
    ascend();
  }

  void visitIf(IfExprAST &N) override {
    printIndent(Depth_);
    std::cout << "IfExpr(" << N.getBranches().size() << " branches)\n";
    descend();
    for (const auto &Branch : N.getBranches()) {
      printIndent(Depth_);
      std::cout << "Branch:\n";
      descend();
      print(Branch.Cond.get(), Depth_);
      print(Branch.Body.get(), Depth_);
      ascend();
    }
    if (N.getElseBody()) {
      printIndent(Depth_);
      std::cout << "Else:\n";
      descend();
      print(N.getElseBody(), Depth_);
      ascend();
    }
    ascend();
  }

  void visitWhile(WhileExprAST &N) override {
    printIndent(Depth_);
    std::cout << "WhileExpr\n";
    descend();
    print(const_cast<ExprAST *>(N.getCond()), Depth_);
    print(N.getBody(), Depth_);
    ascend();
  }

  void visitFor(ForExprAST &N) override {
    printIndent(Depth_);
    std::cout << "ForExpr(var=" << N.getVarName() << ")";
    printResolvedBadge(N.ResolvedLoopVar.get());
    std::cout << "\n";
    descend();
    print(const_cast<ExprAST *>(N.getStart()), Depth_);
    print(const_cast<ExprAST *>(N.getEnd()), Depth_);
    if (N.getStep())
      print(const_cast<ExprAST *>(N.getStep()), Depth_);
    print(N.getBody(), Depth_);
    ascend();
  }

  void visitBreak(BreakExprAST &N) override {
    printIndent(Depth_);
    std::cout << "BreakExpr\n";
  }

  void visitContinue(ContinueExprAST &N) override {
    printIndent(Depth_);
    std::cout << "ContinueExpr\n";
  }

  void visitReturn(ReturnExprAST &N) override {
    printIndent(Depth_);
    std::cout << "ReturnExpr";
    printTypeBadge(&N);
    std::cout << "\n";
    if (N.getVal()) {
      descend();
      print(const_cast<ExprAST *>(N.getVal()), Depth_);
      ascend();
    }
  }


  // ── Functions ───────────────────────────────────────────────────────────────
  void visitFuncDef(FuncDefExprAST &N) override {
    printIndent(Depth_);
    std::cout << "FuncDef(" << N.getName() << " -> "
              << (N.getReturnType() ? N.getReturnType()->getName() : "?")
              << ", params=" << N.getParams().size() << ")";
    printFuncDefBadge(N.ResolvedDef.get());
    std::cout << "\n";
    descend();
    for (const auto &P : N.getParams()) {
      printIndent(Depth_);
      std::cout << "Param(" << P.Name << " : "
                << (P.Type ? P.Type->getName() : "?") << ")\n";
    }
    print(const_cast<ExprAST *>(N.getBody()), Depth_);
    ascend();
  }

  void visitFuncCall(FuncCallExprAST &N) override {
    printIndent(Depth_);
    std::cout << "FuncCall(args=" << N.getArgs().size() << ")";
    printFuncCandidatesBadge(N.ResolvedCandidates);
    printCallTargetBadge(N.ResolvedCallTarget.get());
    printTypeBadge(&N);
    std::cout << "\n";
    descend();
    print(const_cast<ExprAST *>(N.getCallee()), Depth_);
    for (const auto &Arg : N.getArgs())
      print(Arg.get(), Depth_);
    ascend();
  }

  void visitMethodCall(MethodCallExprAST &N) override {
    printIndent(Depth_);
    std::cout << "MethodCall(." << N.getMethod()
              << ", args=" << N.getArgs().size() << ")";
    printCallTargetBadge(N.ResolvedCallTarget.get());
    printTypeBadge(&N);
    std::cout << "\n";
    descend();
    print(const_cast<ExprAST *>(N.getBase()), Depth_);
    for (const auto &Arg : N.getArgs())
      print(Arg.get(), Depth_);
    ascend();
  }

  // ── Structs ─────────────────────────────────────────────────────────────────
  void visitStructDecl(StructDeclExprAST &N) override {
    printIndent(Depth_);
    std::cout << "StructDecl(" << N.getName()
              << ", fields=" << N.getFields().size() << ")\n";
    descend();
    for (const auto &F : N.getFields()) {
      printIndent(Depth_);
      std::cout << "Field(" << F.Name << " : "
                << (F.Type ? F.Type->getName() : "?") << ")\n";
    }
    ascend();
  }

  void visitStructInit(StructInitExprAST &N) override {
    printIndent(Depth_);
    std::cout << "StructInit(" << N.getName()
              << ", args=" << N.getArgs().size() << ")\n";
    descend();
    for (const auto &Arg : N.getArgs())
      print(Arg.get(), Depth_);
    ascend();
  }

  void visitFieldAccess(FieldAccessExprAST &N) override {
    printIndent(Depth_);
    std::cout << "FieldAccess(." << N.getFieldName() << ")\n";
    descend();
    print(const_cast<ExprAST *>(N.getBase()), Depth_);
    ascend();
  }

  // ── Arrays ───────────────────────────────────────────────────────────────────
  void visitArrayDecl(ArrayDeclExprAST &N) override {
    printIndent(Depth_);
    std::cout << "ArrayDecl(" << N.getName() << "["
              << (N.getElementType() ? N.getElementType()->getName() : "?")
              << "], size=" << N.getSize() << ")";
    printResolvedBadge(N.ResolvedSymbol.get());
    std::cout << "\n";
    if (!N.getInitList().empty()) {
      descend();
      for (const auto &Init : N.getInitList())
        print(Init.get(), Depth_);
      ascend();
    }
  }

  void visitArrayAccess(ArrayAccessExprAST &N) override {
    printIndent(Depth_);
    std::cout << "ArrayAccess\n";
    descend();
    print(const_cast<ExprAST *>(N.getBase()), Depth_);
    print(const_cast<ExprAST *>(N.getIndex()), Depth_);
    ascend();
  }

  void visitArrayAssign(ArrayAssignExprAST &N) override {
    printIndent(Depth_);
    std::cout << "ArrayAssign(" << N.getName() << ")\n";
    descend();
    print(const_cast<ExprAST *>(N.getIndex()), Depth_);
    print(const_cast<ExprAST *>(N.getRHS()), Depth_);
    ascend();
  }

  void visitArrayLength(ArrayLengthExprAST &N) override {
    printIndent(Depth_);
    std::cout << "ArrayLength(" << N.getName() << ")\n";
  }

  // ── Modules & misc ───────────────────────────────────────────────────────────
  void visitModuleImport(ModuleImportExprAST &N) override {
    printIndent(Depth_);
    std::cout << "ModuleImport(" << N.getModuleName() << ")\n";
    if (N.getBody()) {
      descend();
      print(const_cast<ExprAST *>(N.getBody()), Depth_);
      ascend();
    }
  }

  void visitSizeOf(SizeOfExprAST &N) override {
    printIndent(Depth_);
    std::cout << "SizeOf("
              << (N.getTypeArg() ? N.getTypeArg()->getName() : "?") << ")\n";
  }

private:
  int Depth_ = 0;
  const TypeChecker *TC_ = nullptr; ///< Optional: set by setTypeChecker() for type annotations.

  void printIndent(int Depth) const {
    for (int i = 0; i < Depth; ++i)
      std::cout << "  ";
  }

  void descend() { ++Depth_; }
  void ascend() { --Depth_; }

  /// Print a coloured variable-resolution badge (no trailing newline).
  /// Green  [resolved ✓ name @ line:col]  — Resolver attached a VariableSymbol.
  /// Red    [NOT resolved ✗]              — pointer is null (pre-resolver or cloned).
  static void printResolvedBadge(const VariableSymbol *Sym) {
    if (Sym) {
      std::cout << "  \033[32m[resolved \u2713  '" << Sym->Name
                << "' @ " << Sym->DeclLoc.Line << ":" << Sym->DeclLoc.Col
                << ", type=" << (Sym->Type ? Sym->Type->getName() : "?")
                << ", depth=" << Sym->ScopeDepth << "]\033[0m";
    } else {
      std::cout << "  \033[31m[NOT resolved \u2717]\033[0m";
    }
  }

  /// Print a cyan module-resolution badge (no trailing newline).
  /// Cyan   [module: io ✓]  — Resolver identified this node as a module qualifier.
  static void printModuleBadge(const std::string &ModuleName) {
    std::cout << "  \033[36m[module: " << ModuleName << " \u2713]\033[0m";
  }

  /// Print a green badge showing ResolvedDef is populated on a FuncDef node.
  /// Green  [fn resolved ✓ @ line:col]  — Resolver registered this function.
  /// Yellow [fn NOT resolved ✗]         — Resolver hasn't run yet (--dump-ast).
  static void printFuncDefBadge(const ResolverFunctionSymbol *Sym) {
    if (Sym) {
      std::cout << "  \033[32m[fn resolved \u2713 @ "
                << Sym->DeclLoc.Line << ":" << Sym->DeclLoc.Col
                << "]\033[0m";
    } else {
      std::cout << "  \033[33m[fn NOT resolved \u2717]\033[0m";
    }
  }

  /// Print a badge showing how many overload candidates the Resolver found
  /// for a FuncCall node.
  /// Green  [N candidates]  — at least one match found.
  /// Yellow [unresolved]    — Resolver hasn't run or found nothing yet.
  static void printFuncCandidatesBadge(
      const std::vector<std::shared_ptr<ResolverFunctionSymbol>> &Candidates) {
    if (!Candidates.empty()) {
      std::cout << "  \033[32m[" << Candidates.size()
                << (Candidates.size() == 1 ? " candidate" : " candidates")
                << " \u2713]\033[0m";
    } else {
      std::cout << "  \033[33m[unresolved]\033[0m";
    }
  }

  /// Print a badge showing the TypeChecker-resolved call target (return type).
  /// Green  [→ returnType ✓]  — TypeChecker narrowed to one overload.
  /// (no badge)               — TypeChecker hasn't run yet or target is null.
  static void printCallTargetBadge(const ResolverFunctionSymbol *Target) {
    if (Target) {
      const std::string RetName =
          Target->ReturnType ? Target->ReturnType->getName() : "void";
      std::cout << "  \033[32m[\u2192 " << RetName << " \u2713]\033[0m";
    }
    // No badge if null — the candidates badge already shows [unresolved].
  }

  /// Print a magenta [: type] badge when the TypeChecker has an inferred type
  /// for this node.  No-op when TC_ is null (--dump-ast / --dump-resolved).
  void printTypeBadge(const ExprAST *Node) const {
    if (!TC_ || !Node)
      return;
    TurfTypePtr Ty = TC_->getType(Node);
    if (Ty)
      std::cout << "  \033[35m[: " << Ty->getName() << "]\033[0m";
  }

};

#endif // AST_PRINTER_H
