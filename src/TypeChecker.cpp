/**
 * @file TypeChecker.cpp
 * @brief Implementation of the TypeChecker pass — Issue #7.
 *
 * Walks the AST bottom-up and infers TurfType for every expression node.
 * Raises TypeError on illegal operations (e.g. string + int).
 *
 * This file includes AST.h (which in turn includes llvm/IR/Value.h) because
 * the AST nodes are defined there.  The TypeChecker logic itself, however,
 * never calls any LLVM API — it only reads node metadata.
 *
 * @author a2ys
 * @copyright Copyright (c) 2026 TurfLang contributors.  Apache License.
 */

#include "TypeChecker.h"

// AST.h is needed for the full definition of every node type.
// It drags in LLVM headers, but TypeChecker itself does not use any LLVM API.
#include "AST.h"

// Errors.h is conditionally included only in production builds (not in the
// standalone unit-test binary).  The TYPECHECKER_NO_ERRORS macro lets the
// test target suppress this dependency so it can link without LLVM.
#ifndef TYPECHECKER_NO_ERRORS
#include "Errors.h"
#endif

#include <cassert>
#include <stdexcept>
#include <string>

// ─── Default production error handler ────────────────────────────────────────

namespace {

#ifndef TYPECHECKER_NO_ERRORS
/// Default error handler: uses the real TypeError class from Errors.h.
void productionErrorHandler(SourceLocation Loc, const std::string &Msg) {
  TypeError(Loc, Msg).raise();
}
#else
/// Stub used when TYPECHECKER_NO_ERRORS is defined (unit-test builds).
/// Throws std::runtime_error so tests can catch it with EXPECT_THROWS.
void productionErrorHandler(SourceLocation Loc, const std::string &Msg) {
  (void)Loc;
  throw std::runtime_error("TypeChecker error: " + Msg);
}
#endif

} // namespace

// ─── Constructors ─────────────────────────────────────────────────────────────

TypeChecker::TypeChecker(const ResolverSymbolTable &RST)
    : RST_(RST), ErrorHandler_(productionErrorHandler) {}

TypeChecker::TypeChecker(const ResolverSymbolTable &RST, ErrorHandler Handler)
    : RST_(RST), ErrorHandler_(std::move(Handler)) {}

// ─── Type query helpers ───────────────────────────────────────────────────────

TurfTypePtr TypeChecker::getType(const ExprAST *Node) const {
  auto It = NodeTypes_.find(Node);
  return It != NodeTypes_.end() ? It->second : nullptr;
}

bool TypeChecker::hasType(const ExprAST *Node) const {
  return NodeTypes_.count(Node) > 0;
}

void TypeChecker::setType(const ExprAST *Node, TurfTypePtr Ty) {
  assert(Node && "setType: null node");
  NodeTypes_[Node] = std::move(Ty);
}

void TypeChecker::reportError(SourceLocation Loc, const std::string &Msg) {
  HadError_ = true;
  if (ErrorHandler_)
    ErrorHandler_(Loc, Msg);
}

// ─── Static type predicate helpers ───────────────────────────────────────────

bool TypeChecker::isNumericType(const TurfType *T) {
  return T && T->isNumeric();
}

bool TypeChecker::areArithCompatible(const TurfType *A, const TurfType *B) {
  if (!A || !B)
    return true; // unknown type — be permissive, error already fired elsewhere
  if (A->isString() && B->isString())
    return true;
  if (A->isNumeric() && B->isNumeric())
    return true;
  return false;
}

// ─── Binary type inference ────────────────────────────────────────────────────

bool TypeChecker::inferArithmeticType(SourceLocation Loc, int Op,
                                      const TurfType *LhsTy,
                                      const TurfType *RhsTy,
                                      TurfTypePtr &ResultOut) {
  // If either side is unknown (nullptr), we can't check — be permissive.
  if (!LhsTy || !RhsTy) {
    ResultOut = nullptr;
    return true;
  }

  // ── string concatenation (only '+' is valid) ─────────────────────────────
  if (LhsTy->isString() || RhsTy->isString()) {
    if (LhsTy->isString() && RhsTy->isString() && Op == '+') {
      ResultOut = TurfString();
      return true;
    }
    // Any other combination of string with non-string, or string OP non-'+':
    reportError(Loc,
                "Cannot apply operator '" + std::string(1, static_cast<char>(Op)) +
                    "' to types '" + LhsTy->getName() + "' and '" +
                    RhsTy->getName() +
                    "'. String concatenation only supports '+'.");
    ResultOut = nullptr;
    return false;
  }

  // ── numeric arithmetic ───────────────────────────────────────────────────
  if (LhsTy->isNumeric() && RhsTy->isNumeric()) {
    // Promote to double if either operand is double
    if (LhsTy->isDouble() || RhsTy->isDouble()) {
      ResultOut = TurfDouble();
    } else {
      // int/bool arithmetic → int
      ResultOut = TurfInt();
    }
    return true;
  }

  // ── anything else is a mismatch ──────────────────────────────────────────
  reportError(Loc,
              "Type mismatch: cannot apply '" +
                  std::string(1, static_cast<char>(Op)) + "' to '" +
                  LhsTy->getName() + "' and '" + RhsTy->getName() + "'.");
  ResultOut = nullptr;
  return false;
}

TurfTypePtr TypeChecker::inferRelationalType(SourceLocation Loc, int Op,
                                             const TurfType *LhsTy,
                                             const TurfType *RhsTy) {
  // Unknown types — let it pass (permissive)
  if (!LhsTy || !RhsTy)
    return TurfBool();

  // Logical operators: allow any types (they coerce to bool)
  if (Op == static_cast<int>('&') || Op == static_cast<int>('|'))
    return TurfBool();

  // Equality (== / !=): same type, or both numeric
  if (Op == static_cast<int>('e') || Op == static_cast<int>('n') ||
      Op == static_cast<int>('=') || Op == static_cast<int>('!')) {
    // Same type or both numeric is fine
    if (LhsTy->getName() == RhsTy->getName())
      return TurfBool();
    if (LhsTy->isNumeric() && RhsTy->isNumeric())
      return TurfBool();
    reportError(Loc, "Cannot compare '" + LhsTy->getName() + "' with '" +
                         RhsTy->getName() + "'.");
    return TurfBool(); // continue with bool to limit cascade errors
  }

  // Ordering operators (< > <= >=): both must be numeric
  if (LhsTy->isNumeric() && RhsTy->isNumeric())
    return TurfBool();

  reportError(Loc, "Cannot compare '" + LhsTy->getName() + "' with '" +
                       RhsTy->getName() + "' using '" +
                       std::string(1, static_cast<char>(Op)) + "'.");
  return TurfBool(); // continue with bool
}

// ─── Literal visitors ─────────────────────────────────────────────────────────

void TypeChecker::visitNumber(NumberExprAST &Node) {
  setType(&Node, Node.isInteger() ? TurfInt() : TurfDouble());
}

void TypeChecker::visitBool(BoolExprAST &Node) {
  setType(&Node, TurfBool());
}

void TypeChecker::visitString(StringExprAST &Node) {
  setType(&Node, TurfString());
}

void TypeChecker::visitNull(NullExprAST &Node) {
  setType(&Node, TurfNull());
}

// ─── Variables & declarations ─────────────────────────────────────────────────

void TypeChecker::visitVariable(VariableExprAST &Node) {
  const std::string &Name = Node.getName();

  // 1. Check loop variable environment first (for-loop induction variables)
  auto LoopIt = LoopVarTypes_.find(Name);
  if (LoopIt != LoopVarTypes_.end()) {
    setType(&Node, LoopIt->second);
    return;
  }

  // 2. Look up in the Resolver's symbol table
  auto Sym = RST_.lookupVariable(Name);
  if (Sym && Sym->Type) {
    setType(&Node, Sym->Type);
    return;
  }

  // 3. Unknown variable — Resolver should have caught this, but be safe
  setType(&Node, nullptr); // unknown type; no new error (Resolver handles it)
}

void TypeChecker::visitVarDecl(VarDeclExprAST &Node) {
  // Visit the initializer first (bottom-up)
  if (Node.getInitializer()) {
    const_cast<ExprAST *>(Node.getInitializer())->accept(*this);
  }

  // The declared type is authoritative; just record it.
  TurfTypePtr DeclTy = Node.getTypePtr();
  setType(&Node, DeclTy);

  // Optional: validate that the initializer type is compatible
  if (Node.getInitializer() && DeclTy) {
    TurfTypePtr InitTy = getType(Node.getInitializer());
    if (InitTy) {
      // Mismatch check: allow numeric promotion (int→double), but not
      // string↔numeric
      bool ok = DeclTy->isEquivalent(InitTy.get()) ||
                (DeclTy->isNumeric() && InitTy->isNumeric()) ||
                DeclTy->isNull() || InitTy->isNull();
      if (!ok) {
        reportError(Node.getLoc(),
                    "Cannot initialise '" + DeclTy->getName() +
                        "' variable '" + Node.getName() + "' with value of type '" +
                        InitTy->getName() + "'.");
      }
    }
  }
}

// ─── Expression visitors ──────────────────────────────────────────────────────

void TypeChecker::visitBinary(BinaryExprAST &Node) {
  // Visit children first (bottom-up)
  if (Node.getLHS())
    const_cast<ExprAST *>(Node.getLHS())->accept(*this);
  if (Node.getRHS())
    const_cast<ExprAST *>(Node.getRHS())->accept(*this);

  const TurfType *LhsTy = getType(Node.getLHS()).get();
  const TurfType *RhsTy = getType(Node.getRHS()).get();
  int Op = Node.getOp();

  // Determine operator category
  // Relational / logical: < > <= >= == != && ||
  // We encode multi-char tokens via the token enum (negative values) —
  // the parser uses the raw ASCII for single-char ops and negative token IDs
  // for compound ones.  We map the common ones here.
  //
  // Single-char relational: '<' (60) '>' (62)
  // Compound (token enum values, negative integers):
  //   TOK_EQ (-9), TOK_NEQ (-10), TOK_LEQ (-11), TOK_GEQ (-12)
  //   TOK_AND (-20), TOK_OR (-21)

  auto isRelational = [](int op) -> bool {
    return op == '<' || op == '>' ||
           op == -9  /*EQ*/  || op == -10 /*NEQ*/ ||
           op == -11 /*LEQ*/ || op == -12 /*GEQ*/ ||
           op == -20 /*AND*/ || op == -21 /*OR*/;
  };

  if (isRelational(Op)) {
    TurfTypePtr Result = inferRelationalType(Node.getLoc(), Op, LhsTy, RhsTy);
    setType(&Node, Result ? Result : TurfBool());
    return;
  }

  // Arithmetic / string: + - * / %
  TurfTypePtr Result;
  if (!inferArithmeticType(Node.getLoc(), Op, LhsTy, RhsTy, Result)) {
    setType(&Node, nullptr); // error already reported
    return;
  }
  setType(&Node, Result ? Result : TurfInt()); // fallback to int
}

void TypeChecker::visitUnary(UnaryExprAST &Node) {
  if (Node.getOperand())
    const_cast<ExprAST *>(Node.getOperand())->accept(*this);

  TurfTypePtr OperandTy = getType(Node.getOperand());
  int Op = Node.getOpcode();

  if (Op == '-') {
    // Negation: operand must be numeric
    if (OperandTy && !OperandTy->isNumeric()) {
      reportError({0, 0},
                  "Unary '-' requires a numeric operand, got '" +
                      OperandTy->getName() + "'.");
      setType(&Node, nullptr);
      return;
    }
    setType(&Node, OperandTy ? OperandTy : TurfInt());
    return;
  }

  if (Op == '!') {
    // Logical not: always produces bool
    setType(&Node, TurfBool());
    return;
  }

  // Unknown unary op: propagate operand type
  setType(&Node, OperandTy);
}

void TypeChecker::visitAssignment(AssignmentExprAST &Node) {
  // Visit both sides
  if (Node.getLHS())
    const_cast<ExprAST *>(Node.getLHS())->accept(*this);
  if (Node.getRHS())
    const_cast<ExprAST *>(Node.getRHS())->accept(*this);

  // The assignment expression type is the RHS type
  TurfTypePtr RhsTy = getType(Node.getRHS());
  setType(&Node, RhsTy);
}

void TypeChecker::visitCast(CastExprAST &Node) {
  // Visit the operand
  if (Node.getOperand())
    const_cast<ExprAST *>(Node.getOperand())->accept(*this);

  // Explicit cast always succeeds at the TypeChecker level; result is DestType.
  setType(&Node, Node.getDestTypePtr());
}

// ─── Control flow visitors ────────────────────────────────────────────────────

void TypeChecker::visitBlock(BlockExprAST &Node) {
  TurfTypePtr LastTy;
  for (const auto &Expr : Node.getExpressions()) {
    Expr->accept(*this);
    LastTy = getType(Expr.get());
  }
  // The block's type is its last expression's type (or void if empty)
  setType(&Node, LastTy ? LastTy : TurfVoid());
}

void TypeChecker::visitIf(IfExprAST &Node) {
  for (const auto &Branch : Node.getBranches()) {
    if (Branch.Cond)
      Branch.Cond->accept(*this);
    if (Branch.Body)
      Branch.Body->accept(*this);
  }
  if (Node.getElseBody())
    Node.getElseBody()->accept(*this);
  setType(&Node, TurfVoid());
}

void TypeChecker::visitWhile(WhileExprAST &Node) {
  if (Node.getCond())
    const_cast<ExprAST *>(Node.getCond())->accept(*this);
  if (Node.getBody())
    Node.getBody()->accept(*this);
  setType(&Node, TurfVoid());
}

void TypeChecker::visitFor(ForExprAST &Node) {
  // Visit start / end / step expressions first so we know their types
  if (Node.getStart())
    const_cast<ExprAST *>(Node.getStart())->accept(*this);
  if (Node.getEnd())
    const_cast<ExprAST *>(Node.getEnd())->accept(*this);
  if (Node.getStep())
    const_cast<ExprAST *>(Node.getStep())->accept(*this);

  // Infer loop variable type from the range bounds.
  // If both start and end are integers → int; if either is double → double.
  TurfTypePtr StartTy = getType(Node.getStart());
  TurfTypePtr EndTy   = getType(Node.getEnd());

  TurfTypePtr LoopVarTy = TurfInt(); // default: int for integer ranges
  if ((StartTy && StartTy->isDouble()) || (EndTy && EndTy->isDouble()))
    LoopVarTy = TurfDouble();

  // Register the loop variable so visitVariable() finds it during body walk
  LoopVarTypes_[Node.getVarName()] = LoopVarTy;

  // Visit body
  if (Node.getBody())
    Node.getBody()->accept(*this);

  // Remove loop variable from scope
  LoopVarTypes_.erase(Node.getVarName());

  setType(&Node, TurfVoid());
}

void TypeChecker::visitBreak(BreakExprAST &Node) {
  setType(&Node, TurfVoid());
}

void TypeChecker::visitContinue(ContinueExprAST &Node) {
  setType(&Node, TurfVoid());
}

void TypeChecker::visitReturn(ReturnExprAST &Node) {
  if (Node.getVal())
    const_cast<ExprAST *>(Node.getVal())->accept(*this);

  TurfTypePtr RetTy = getType(Node.getVal());

  // Check against the enclosing function's declared return type
  if (!ReturnTypeStack_.empty() && ReturnTypeStack_.back()) {
    const TurfType *Declared = ReturnTypeStack_.back().get();
    if (RetTy) {
      bool ok = Declared->isEquivalent(RetTy.get()) ||
                (Declared->isNumeric() && RetTy->isNumeric()) ||
                Declared->isVoid();
      if (!ok) {
        reportError(Node.getLoc(),
                    "Return type mismatch: function declares '" +
                        Declared->getName() + "' but returns '" +
                        RetTy->getName() + "'.");
      }
    }
  }

  setType(&Node, RetTy ? RetTy : TurfVoid());
}

void TypeChecker::visitRaise(RaiseExprAST &Node) {
  if (Node.getPayload())
    const_cast<ExprAST *>(Node.getPayload())->accept(*this);
  setType(&Node, TurfVoid());
}

void TypeChecker::visitTry(TryExprAST &Node) {
  if (Node.getTryBlock())
    const_cast<ExprAST *>(Node.getTryBlock())->accept(*this);
  if (Node.getCatchBlock())
    const_cast<ExprAST *>(Node.getCatchBlock())->accept(*this);
  setType(&Node, TurfVoid());
}

// ─── Functions & calls ────────────────────────────────────────────────────────

void TypeChecker::visitFuncDef(FuncDefExprAST &Node) {
  // Push the declared return type so visitReturn() can validate it
  ReturnTypeStack_.push_back(Node.getReturnTypePtr());

  if (Node.getBody())
    const_cast<ExprAST *>(Node.getBody())->accept(*this);

  ReturnTypeStack_.pop_back();
  setType(&Node, TurfVoid());
}

void TypeChecker::visitFuncCall(FuncCallExprAST &Node) {
  // Visit callee and args
  if (Node.getCallee())
    const_cast<ExprAST *>(Node.getCallee())->accept(*this);
  for (const auto &Arg : Node.getArgs())
    Arg->accept(*this);

  // Full arity/overload checking is Issue #8; for now mark result as unknown.
  setType(&Node, nullptr);
}

void TypeChecker::visitMethodCall(MethodCallExprAST &Node) {
  if (Node.getBase())
    const_cast<ExprAST *>(Node.getBase())->accept(*this);
  for (const auto &Arg : Node.getArgs())
    Arg->accept(*this);

  // Full overload checking deferred to Issue #8.
  setType(&Node, nullptr);
}

// ─── Structs & field access ───────────────────────────────────────────────────

void TypeChecker::visitFieldAccess(FieldAccessExprAST &Node) {
  if (Node.getBase())
    const_cast<ExprAST *>(Node.getBase())->accept(*this);
  // Field type resolution requires a struct registry (future issue).
  setType(&Node, nullptr);
}

void TypeChecker::visitStructDecl(StructDeclExprAST &Node) {
  setType(&Node, TurfVoid());
}

void TypeChecker::visitStructInit(StructInitExprAST &Node) {
  for (const auto &Arg : Node.getArgs())
    Arg->accept(*this);
  // Result type is the struct name; for now mark as NamedType.
  setType(&Node, std::make_shared<NamedType>(Node.getName()));
}

// ─── Arrays ───────────────────────────────────────────────────────────────────

void TypeChecker::visitArrayDecl(ArrayDeclExprAST &Node) {
  for (const auto &Init : Node.getInitList())
    Init->accept(*this);
  setType(&Node, makeArrayType(Node.getElementTypePtr(), Node.getSize()));
}

void TypeChecker::visitArrayAccess(ArrayAccessExprAST &Node) {
  if (Node.getBase())
    const_cast<ExprAST *>(Node.getBase())->accept(*this);
  if (Node.getIndex())
    const_cast<ExprAST *>(Node.getIndex())->accept(*this);

  // Result is the element type of the base array
  TurfTypePtr BaseTy = getType(Node.getBase());
  if (BaseTy && BaseTy->isArray()) {
    const auto *ArrTy = dynamic_cast<const ArrayType *>(BaseTy.get());
    setType(&Node, ArrTy ? ArrTy->getElementTypePtr() : nullptr);
  } else {
    setType(&Node, nullptr);
  }
}

void TypeChecker::visitArrayAssign(ArrayAssignExprAST &Node) {
  if (Node.getIndex())
    const_cast<ExprAST *>(Node.getIndex())->accept(*this);
  if (Node.getRHS())
    const_cast<ExprAST *>(Node.getRHS())->accept(*this);
  setType(&Node, TurfVoid());
}

void TypeChecker::visitArrayLength(ArrayLengthExprAST &Node) {
  setType(&Node, TurfInt());
}

// ─── Modules & misc ───────────────────────────────────────────────────────────

void TypeChecker::visitModuleImport(ModuleImportExprAST &Node) {
  if (Node.getBody())
    const_cast<ExprAST *>(Node.getBody())->accept(*this);
  setType(&Node, TurfVoid());
}

void TypeChecker::visitSizeOf(SizeOfExprAST &Node) {
  // sizeof(T) always produces int
  setType(&Node, TurfInt());
}
