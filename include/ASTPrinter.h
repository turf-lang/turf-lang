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

#include "llvm/ADT/SmallString.h" // visitNumber()'s exact-value printing

#include <cctype>
#include <iostream>
#include <string>

// OpToDisplayString() now lives in Lexer.h (promoted there once
// TypeChecker.cpp needed the identical fix for its own diagnostics) --
// transitively available here via AST.h's include of Lexer.h.

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
    // Exact accessors, not getIntVal()/getDoubleVal() -- this is SEM-001's
    // own completion-criteria proof (docs/planning/ARCHITECTURE_OVERHAUL.md
    // §8: "a 48-digit literal still compiles ... now carries its exact
    // value, provable via --dump-ast"). getIntVal() truncates to 64 bits,
    // which would silently defeat that proof for exactly the literals it
    // matters most for.
    if (N.isInteger()) {
      llvm::SmallString<32> Digits;
      N.getIntValExact().toString(Digits, 10);
      std::cout << "NumberExpr(int:" << Digits.str().str() << ")";
    } else {
      std::cout << "NumberExpr(double:" << N.getDoubleVal();
      if (N.overflowed())
        std::cout << ", OVERFLOWED-TO-INF";
      std::cout << ")";
    }
    printTypeBadge(&N);
    std::cout << "\n";
  }

  void visitBool(BoolExprAST &N) override {
    printIndent(Depth_);
    std::cout << "BoolExpr(" << (N.getVal() ? "true" : "false") << ")";
    printTypeBadge(&N);
    std::cout << "\n";
  }

  void visitChar(CharExprAST &N) override {
    printIndent(Depth_);
    std::cout << "CharExpr('" << N.getVal() << "')";
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
    std::cout << "BinaryExpr(op='" << OpToDisplayString(N.getOp()) << "')";
    printTypeBadge(&N);
    std::cout << "\n";
    descend();
    print(const_cast<ExprAST *>(N.getLHS()), Depth_);
    print(const_cast<ExprAST *>(N.getRHS()), Depth_);
    ascend();
  }

  void visitUnary(UnaryExprAST &N) override {
    printIndent(Depth_);
    std::cout << "UnaryExpr(op='" << OpToDisplayString(N.getOpcode()) << "')\n";
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

  void visitCompoundAssignment(CompoundAssignmentExprAST &N) override {
    printIndent(Depth_);
    // OpToDisplayString(), not a raw (char) cast: the opcode can now be a
    // multi-char operator (&=, <<=, **=, ...) encoded as a negative token
    // constant, which a bare char cast renders as garbage (see its doc
    // comment in Lexer.h for the identical bug this already caused for
    // BinaryExprAST's own dump output).
    std::cout << "CompoundAssignmentExpr(op:'" << OpToDisplayString(N.getOpcode()) << "=')\n";
    descend();
    print(const_cast<ExprAST *>(N.getLHS()), Depth_);
    print(const_cast<ExprAST *>(N.getRHS()), Depth_);
    ascend();
  }

  void visitPostfixExpr(PostfixExprAST &N) override {
    printIndent(Depth_);
    std::cout << (N.IsPrefix ? "PrefixExpr(op:'" : "PostfixExpr(op:'")
              << (char)N.getOpcode() << "')\n";
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

  void visitDoWhile(DoWhileExprAST &N) override {
    printIndent(Depth_);
    std::cout << "DoWhileExpr\n";
    descend();
    print(N.getBody(), Depth_);
    print(const_cast<ExprAST *>(N.getCond()), Depth_);
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

  void visitForEach(ForEachExprAST &N) override {
    printIndent(Depth_);
    std::cout << "ForEachExpr(value=" << N.getValueVarName();
    if (N.hasIndexVar())
      std::cout << ", index=" << N.getIndexVarName();
    std::cout << ")";
    printResolvedBadge(N.ResolvedValueVar.get());
    std::cout << "\n";
    descend();
    print(const_cast<ExprAST *>(N.getIterable()), Depth_);
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

  void visitCompare(CompareExprAST &N) override {
    printIndent(Depth_);
    std::cout << "CompareExpr(" << N.getArms().size() << " arms"
              << (N.getOtherBody() ? ", otherwise" : "") << ")\n";
    descend();
    printIndent(Depth_);
    std::cout << "Subject:\n";
    descend();
    print(const_cast<ExprAST *>(N.getSubject()), Depth_);
    ascend();
    for (size_t i = 0; i < N.getArms().size(); ++i) {
      const auto &Arm = N.getArms()[i];
      printIndent(Depth_);
      std::cout << "Arm[" << i << "] (" << Arm.Values.size() << " pattern(s)):\n";
      descend();
      for (size_t k = 0; k < Arm.Values.size(); ++k) {
        print(const_cast<ExprAST *>(Arm.Values[k].get()), Depth_);
        if (k < Arm.Guards.size() && Arm.Guards[k]) {
          printIndent(Depth_);
          std::cout << "Guard:\n";
          descend();
          print(Arm.Guards[k].get(), Depth_);
          ascend();
        }
      }
      printIndent(Depth_);
      std::cout << "Body:\n";
      descend();
      print(Arm.Body.get(), Depth_);
      ascend();
      ascend();
    }
    if (N.getOtherBody()) {
      printIndent(Depth_);
      std::cout << "Otherwise:\n";
      descend();
      print(const_cast<ExprAST *>(N.getOtherBody()), Depth_);
      ascend();
    }
    ascend();
  }

  // ── Destructuring patterns (LEC-003) ────────────────────────────────────────

  void visitWildcardPattern(WildcardPatternExprAST &) override {
    printIndent(Depth_);
    std::cout << "WildcardPattern\n";
  }

  void visitBindingPattern(BindingPatternExprAST &N) override {
    printIndent(Depth_);
    std::cout << "BindingPattern(" << N.getName() << ")\n";
  }

  void visitStructPattern(StructPatternExprAST &N) override {
    printIndent(Depth_);
    std::cout << "StructPattern(" << N.getTypeSpelling()->getName()
              << (N.hasRest() ? ", ..)" : ")") << "\n";
    descend();
    for (const auto &F : N.getFields()) {
      printIndent(Depth_);
      std::cout << "Field(" << F.Name << (F.IsShorthand ? ", shorthand" : "")
                << "):\n";
      descend();
      print(F.Sub.get(), Depth_);
      ascend();
    }
    ascend();
  }

  // ── Functions ───────────────────────────────────────────────────────────────
  void visitFuncDef(FuncDefExprAST &N) override {
    printIndent(Depth_);
    std::cout << "FuncDef(" << N.getName() << " -> "
              << (N.getReturnType() ? N.getReturnType()->getName() : "?")
              << ", params=" << N.getParams().size()
              << (N.isPublic() ? ", pub" : "") << ")";
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
              << ", fields=" << N.getFields().size()
              << (N.isPublic() ? ", pub" : "") << ")\n";
    descend();
    for (const auto &F : N.getFields()) {
      printIndent(Depth_);
      std::cout << "Field(" << F.Name << " : "
                << (F.Type ? F.Type->getName() : "?")
                << (F.Vis.IsPublic ? ", pub" : "") << ")\n";
    }
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
    } else if (N.getInitExpr()) {
      descend();
      print(N.getInitExpr().get(), Depth_);
      ascend();
    }
  }

  void visitArrayLiteral(ArrayLiteralExprAST &N) override {
    printIndent(Depth_);
    std::cout << "ArrayLiteral(size=" << N.getElements().size() << ")";
    printTypeBadge(&N);
    std::cout << "\n";
    descend();
    for (const auto &E : N.getElements()) {
      print(const_cast<ExprAST *>(E.get()), Depth_);
    }
    ascend();
  }

  void visitArrayAccess(ArrayAccessExprAST &N) override {
    printIndent(Depth_);
    std::cout << "ArrayAccess\n";
    descend();
    print(const_cast<ExprAST *>(N.getBase()), Depth_);
    print(const_cast<ExprAST *>(N.getIndex()), Depth_);
    ascend();
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
