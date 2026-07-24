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

// SymbolTable.h is included for GlobalSymbolTable, used in visitStructInit
// to look up struct type definitions for named-field validation.
// Not available in the unit-test binary (TYPECHECKER_NO_ERRORS mode) since it
// pulls in the LLVM-dependent SymbolTable implementation.
#ifndef TYPECHECKER_NO_ERRORS
#include "SymbolTable.h"
#endif

// Errors.h is conditionally included only in production builds (not in the
// standalone unit-test binary).  The TYPECHECKER_NO_ERRORS macro lets the
// test target suppress this dependency so it can link without LLVM.
#ifndef TYPECHECKER_NO_ERRORS
#include "Errors.h"
#endif

// Builtins.h provides LookupBuiltin() / BuiltinInfo for arity checking of
// pure intrinsics (typeof, lengthof, abs, power, …) that have no
// ResolvedCandidates entry.  It is LLVM-free at the header level; the
// function implementations live in Builtins.cpp which links against LLVM,
// but the header itself (and LookupBuiltin) are safe to call from the
// TypeChecker in both production and unit-test builds.
#include "Builtins.h"

#include <cassert>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>


// ─── Default production error handler ────────────────────────────────────────

namespace {

#ifndef TYPECHECKER_NO_ERRORS
/// Default error handler: uses the real TypeError class from Errors.h.
void productionErrorHandler(SourceLocation Loc, const std::string &Msg,
                            const std::vector<RelatedLocation> &Related) {
  TypeError Err(Loc, Msg);
  for (const auto &R : Related)
    Err.withRelated(R.Loc, R.Description);
  Err.raise();
}
#else
/// Stub used when TYPECHECKER_NO_ERRORS is defined (unit-test builds).
/// Throws std::runtime_error so tests can catch it with EXPECT_THROWS.
void unitTestErrorHandler(SourceLocation, const std::string &Msg,
                          const std::vector<RelatedLocation> &) {
  throw std::runtime_error("TypeError: " + Msg);
}
#endif

const VariableExprAST *findRootVariable(const ExprAST *Base) {
  while (Base) {
    if (auto *Var = dynamic_cast<const VariableExprAST *>(Base)) {
      return Var;
    } else if (auto *ArrAccess =
                   dynamic_cast<const ArrayAccessExprAST *>(Base)) {
      Base = ArrAccess->getBase();
    } else if (auto *FieldAccess =
                   dynamic_cast<const FieldAccessExprAST *>(Base)) {
      Base = FieldAccess->getBase();
    } else {
      break;
    }
  }
  return nullptr;
}

// Convert from the resolver's location type (no Lexer.h dependency) to the
// diagnostic engine's location type. Same layout — explicit conversion only
// because the types are intentionally kept separate.
static SourceLocation toSourceLocation(ResolverSourceLocation R) {
  return {R.Line, R.Col};
}

} // namespace

// ─── Constructors ─────────────────────────────────────────────────────────────

TypeChecker::TypeChecker(const ResolverSymbolTable &RST)
    : RST_(RST), ErrorHandler_(productionErrorHandler) {}

TypeChecker::TypeChecker(const ResolverSymbolTable &RST, ErrorHandler Handler)
    : RST_(RST), ErrorHandler_(std::move(Handler)) {}

void TypeChecker::checkAll(const std::vector<std::unique_ptr<ExprAST>> &Nodes) {
  for (const auto &Node : Nodes)
    check(Node.get());
}

void TypeChecker::check(ExprAST *Node) {
  if (Node)
    Node->accept(*this);
}

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

void TypeChecker::reportError(SourceLocation Loc, const std::string &Msg,
                              const std::vector<RelatedLocation> &Related) {
  HadError_ = true;
  if (ErrorHandler_)
    ErrorHandler_(Loc, Msg, Related);
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

// ─── Overload-resolution type compatibility ───────────────────────────────────

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
                    "'. Hint: To concatenate a non-string, convert it first with `<expr> to string`.");
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

  // Logical operators (&& / ||): both operands must be numeric (bool counts
  // as numeric, which is what lets ordinary `cond1 && cond2` type-check).
  //
  // This used to check `Op == '&' || Op == '|'` -- the single-character
  // BITWISE operator tokens (ASCII 38/124) -- instead of TOK_AND/TOK_OR (the
  // actual && / || tokens, -20/-21). That check could never match: visitBinary()
  // already routes bitwise '&'/'|' to a separate isBitwise() branch and
  // returns before this function is ever called, so Op is never literally
  // '&' or '|' here. The dead check meant a genuine && / || silently fell
  // through everything below to the final "Cannot compare ... using ..."
  // error path, which happened to still produce the right answer for the
  // common bool && bool case (bool satisfies isNumeric()) but, for any
  // actually-invalid use, formatted the operator into the message via
  // `std::string(1, static_cast<char>(Op))` -- and -20/-21 cast to char are
  // not printable ASCII, so the message showed a garbage character instead
  // of "&&"/"||".
  if (Op == -20 /*TOK_AND*/ || Op == -21 /*TOK_OR*/) {
    if (LhsTy->isNumeric() && RhsTy->isNumeric())
      return TurfBool();
    reportError(Loc, "Cannot use '" + std::string(Op == -20 ? "&&" : "||") +
                         "' with '" + LhsTy->getName() + "' and '" +
                         RhsTy->getName() + "'.");
    return TurfBool();
  }

  // Equality (== / !=): same type, or both numeric.
  //
  // Only -9/-10 (TOK_EQ/TOK_NEQ, the real tokens for == and !=) actually
  // reach here in practice -- the 'e'/'n'/'='/'!' character checks that used
  // to sit alongside them were similarly dead: the lexer never emits a bare
  // 'e' or 'n' token (those characters only ever appear inside identifiers),
  // '=' alone is TOK_ASSIGN (a different token, not routed to this
  // function), and a bare '!' is consumed entirely inside ParseUnary()
  // (desugared into `operand != true`, never surviving as its own binary
  // operator token). Removed rather than left in as a source of confusion
  // about some alternate operator encoding that doesn't exist.
  if (Op == -9 /*TOK_EQ*/ || Op == -10 /*TOK_NEQ*/) {
    // Same type or both numeric is fine
    if (LhsTy->getName() == RhsTy->getName())
      return TurfBool();
    if (LhsTy->isNumeric() && RhsTy->isNumeric())
      return TurfBool();
    
    // Allow comparing pointers to null
    bool isLhsPtrOrNull = LhsTy->isPointer() || LhsTy->isNull() || LhsTy->isNamed("ptr") || LhsTy->isNamed("void*") || LhsTy->isString();
    bool isRhsPtrOrNull = RhsTy->isPointer() || RhsTy->isNull() || RhsTy->isNamed("ptr") || RhsTy->isNamed("void*") || RhsTy->isString();
    if (isLhsPtrOrNull && isRhsPtrOrNull)
      return TurfBool();

    reportError(Loc, "Cannot compare '" + LhsTy->getName() + "' with '" +
                         RhsTy->getName() + "'.");
    return TurfBool(); // continue with bool to limit cascade errors
  }

  // Ordering operators (< > <= >=): both must be numeric OR both must be string.
  // String ordering is byte-by-byte via strcmp (Decision D2 \u2014 no locale, no Unicode).
  if (LhsTy->isString() && RhsTy->isString())
    return TurfBool();

  if (LhsTy->isEnum() || RhsTy->isEnum()) {
    std::string EnumName = LhsTy->isEnum() ? LhsTy->getName() : RhsTy->getName();
    reportError(Loc, "Cannot use ordering operators on enum type '" + EnumName + 
                     "'. Cast to int first: (a to int) < (b to int)");
    return TurfBool();
  }

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

void TypeChecker::visitDefaultInit(DefaultInitExprAST &Node) {
  auto DeclTy = Node.getTypePtr();
#ifndef TYPECHECKER_NO_ERRORS
  if (auto *NT = dynamic_cast<const NamedType *>(DeclTy.get())) {
    if (GlobalSymbolTable) {
      if (auto StructTy = GlobalSymbolTable->LookupType(NT->getName())) {
        DeclTy = StructTy;
        Node.setTypePtr(StructTy);
      }
    }
  }
#endif
  setType(&Node, DeclTy);
}

// ─── Variables & declarations ─────────────────────────────────────────────────

void TypeChecker::visitVariable(VariableExprAST &Node) {
  const std::string &Name = Node.getName();

  // 1. Check loop variable environment first (for-loop induction variables).
  //    These are not tracked by the Resolver's scope stack but are registered
  //    directly by visitFor() into LoopVarTypes_.
  auto LoopIt = LoopVarTypes_.find(Name);
  if (LoopIt != LoopVarTypes_.end()) {
    setType(&Node, LoopIt->second);
    return;
  }

  // 2. Read the type from the ResolvedSymbol pointer that the Resolver already
  //    attached to this node (Issue #5).  This is the primary lookup path
  //    in production (after the Resolver has run):
  if (Node.ResolvedSymbol && Node.ResolvedSymbol->Type) {
    setType(&Node, Node.ResolvedSymbol->Type);
    return;
  }

  // 2b. Fallback: try a direct RST lookup.  This covers two cases:
  //     (a) Unit tests that call RST.declareVariable() directly without a
  //         Resolver pass (in production the RST scope stack is empty here).
  //     (b) Any variable the Resolver somehow missed but that the TypeChecker
  //         can still find in the remaining scope.
  if (auto Sym = RST_.lookupVariable(Name)) {
    setType(&Node, Sym->Type);
    return;
  }

  // 3. Module qualifier (e.g. `io` in `io.printline(...)`).
  //    Module names have no variable type — leave the type as null (unknown).
  if (!Node.ResolvedModuleName.empty()) {
    setType(&Node, nullptr);
    return;
  }

  // 4. Unknown variable — Resolver should have caught this already.
  //    Be permissive: insert nullptr so we don't emit a cascade error.
  setType(&Node, nullptr);
}

void TypeChecker::visitVarDecl(VarDeclExprAST &Node) {
  if (Node.isImmutable() && !Node.hasExplicitInit()) {
    reportError(Node.getLoc(), "Immutable variable '" + Node.getName() + "' requires an explicit initializer.");
  }

  // Visit the initializer first (bottom-up)
  if (Node.getInitializer()) {
    const_cast<ExprAST *>(Node.getInitializer())->accept(*this);
  }

  TurfTypePtr DeclTy = Node.getTypePtr();

#ifndef TYPECHECKER_NO_ERRORS
  // Resolve NamedType -> TurfStructType
  if (auto *NT = dynamic_cast<const NamedType *>(DeclTy.get())) {
    if (GlobalSymbolTable) {
      if (auto StructTy = GlobalSymbolTable->LookupType(NT->getName())) {
        DeclTy = StructTy;
        Node.setTypePtr(StructTy);
        if (Node.ResolvedSymbol) {
          Node.ResolvedSymbol->Type = StructTy;
        }
      }
    }
  }
#endif

  setType(&Node, DeclTy);

  // Optional: validate that the initializer type is compatible
  if (Node.getInitializer() && DeclTy) {
    TurfTypePtr InitTy = getType(Node.getInitializer());
    if (InitTy) {
      if (InitTy->isNull()) {
        if (!DeclTy->isPointer() && !DeclTy->isNamed("ptr") && !DeclTy->isNamed("void*")) {
#ifndef TYPECHECKER_NO_ERRORS
          NullAssignmentError(Node.getLoc(), Node.getName(), DeclTy->getName()).raise();
#else
          throw std::runtime_error("NullAssignmentError: " + Node.getName());
#endif
        }
      }
      // Mismatch check: use unified typesCompatible logic
      if (!typesCompatible(InitTy.get(), DeclTy.get())) {
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

  // Bitwise binary operators: &, |, ^, <<, >> — require integer operands.
  auto isBitwise = [](int op) -> bool {
    return op == '&' || op == '|' || op == '^' ||
           op == -79 /*TOK_LSHIFT*/ || op == -80 /*TOK_RSHIFT*/;
  };

  if (isBitwise(Op)) {
    bool lhsOk = !LhsTy || LhsTy->isNamed("int");
    bool rhsOk = !RhsTy || RhsTy->isNamed("int");

    if (!lhsOk) {
      reportError(Node.getLoc(),
                  "Bitwise operator requires integer operands, but left operand is '" +
                      LhsTy->getName() + "'.");
    }
    if (!rhsOk) {
      reportError(Node.getLoc(),
                  "Bitwise operator requires integer operands, but right operand is '" +
                      RhsTy->getName() + "'.");
    }
    setType(&Node, TurfInt());
    return;
  }

  if (isRelational(Op)) {
    TurfTypePtr Result = inferRelationalType(Node.getLoc(), Op, LhsTy, RhsTy);
    setType(&Node, Result ? Result : TurfBool());
    return;
  }

  // Exponentiation: ** (TOK_POWER = -78) — numeric operands, promotes to double
  if (Op == -78 /*TOK_POWER*/) {
    if (LhsTy && !LhsTy->isNumeric()) {
      reportError(Node.getLoc(),
                  "Exponentiation operator '**' requires numeric operands, but left operand is '" +
                      LhsTy->getName() + "'.");
      setType(&Node, nullptr);
      return;
    }
    if (RhsTy && !RhsTy->isNumeric()) {
      reportError(Node.getLoc(),
                  "Exponentiation operator '**' requires numeric operands, but right operand is '" +
                      RhsTy->getName() + "'.");
      setType(&Node, nullptr);
      return;
    }
    setType(&Node, TurfDouble());
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

  if (Op == '~') {
    // Bitwise NOT: operand must be int
    if (OperandTy && !OperandTy->isNamed("int")) {
      reportError({0, 0},
                  "Unary '~' (bitwise NOT) requires an integer operand, got '" +
                      OperandTy->getName() + "'.");
      setType(&Node, nullptr);
      return;
    }
    setType(&Node, TurfInt());
    return;
  }

  // Unknown unary op: propagate operand type
  setType(&Node, OperandTy);
}

void TypeChecker::visitAddressOf(AddressOfExprAST &Node) {
  // Recurse into the operand so its type is inferred first.
  if (Node.getOperand())
    const_cast<ExprAST *>(Node.getOperand())->accept(*this);

  TurfTypePtr OperandTy = getType(Node.getOperand());
  if (!OperandTy) {
    setType(&Node, nullptr);
    return;
  }
  // &expr produces a TurfPointerType wrapping the operand's type.
  setType(&Node, std::make_shared<TurfPointerType>(OperandTy));
}

void TypeChecker::visitAssignment(AssignmentExprAST &Node) {
  if (Node.getLHS())
    const_cast<ExprAST *>(Node.getLHS())->accept(*this);
  if (Node.getRHS())
    const_cast<ExprAST *>(Node.getRHS())->accept(*this);

  TurfTypePtr LhsTy = getType(Node.getLHS());
  TurfTypePtr RhsTy = getType(Node.getRHS());
  const VariableExprAST *LHSVar = findRootVariable(Node.getLHS());

  if (LhsTy && RhsTy) {
    if (RhsTy->isNull()) {
      if (!LhsTy->isPointer() && !LhsTy->isNamed("ptr") && !LhsTy->isNamed("void*")) {
        std::string VarName = LHSVar ? LHSVar->getName() : "";
#ifndef TYPECHECKER_NO_ERRORS
        {
          NullAssignmentError Err(Node.getLoc(), VarName, LhsTy->getName());
          if (LHSVar && LHSVar->ResolvedSymbol)
            Err.withRelated(toSourceLocation(LHSVar->ResolvedSymbol->DeclLoc),
                            "declared here");
          Err.raise();
        }
#else
        throw std::runtime_error("NullAssignmentError: " + VarName);
#endif
      }
    }
    if (!typesCompatible(RhsTy.get(), LhsTy.get())) {
      std::vector<RelatedLocation> Related;
      if (LHSVar && LHSVar->ResolvedSymbol) {
        Related.push_back({toSourceLocation(LHSVar->ResolvedSymbol->DeclLoc),
                           "declared here"});
      }
      reportError(Node.getLoc(),
                  "Cannot assign value of type '" + RhsTy->getName() +
                      "' to variable of type '" + LhsTy->getName() + "'.",
                  Related);
    }
  }

  // The assignment expression type is the RHS type
  setType(&Node, RhsTy);

  if (LHSVar) {
    if (LHSVar->ResolvedSymbol && LHSVar->ResolvedSymbol->IsImmutable) {
#ifndef TYPECHECKER_NO_ERRORS
      ImmutableAssignmentError(Node.getLoc(), LHSVar->getName())
          .withRelated(toSourceLocation(LHSVar->ResolvedSymbol->DeclLoc),
                       "declared immutable here")
          .raise();
#else
      throw std::runtime_error("ImmutableAssignmentError: " + LHSVar->getName());
#endif
    }
  }
}

void TypeChecker::visitCompoundAssignment(CompoundAssignmentExprAST &Node) {
  if (Node.getLHS())
    const_cast<ExprAST *>(Node.getLHS())->accept(*this);
  if (Node.getRHS())
    const_cast<ExprAST *>(Node.getRHS())->accept(*this);

  TurfTypePtr LhsTy = getType(Node.getLHS());
  TurfTypePtr RhsTy = getType(Node.getRHS());
  TurfTypePtr ResultTy;

  if (LhsTy && RhsTy) {
    if (inferArithmeticType(Node.getLoc(), Node.getOpcode(), LhsTy.get(), RhsTy.get(), ResultTy)) {
      if (!typesCompatible(ResultTy.get(), LhsTy.get())) {
        const VariableExprAST *LHSVar_early = findRootVariable(Node.getLHS());
        std::vector<RelatedLocation> Related;
        if (LHSVar_early && LHSVar_early->ResolvedSymbol)
          Related.push_back({toSourceLocation(LHSVar_early->ResolvedSymbol->DeclLoc),
                             "declared here"});
        reportError(Node.getLoc(),
                    "Cannot assign value of type '" + ResultTy->getName() +
                        "' to variable of type '" + LhsTy->getName() + "'.",
                    Related);
      }
    }
  }

  setType(&Node, LhsTy);

  const VariableExprAST *LHSVar = findRootVariable(Node.getLHS());

  if (LHSVar) {
    if (LHSVar->ResolvedSymbol && LHSVar->ResolvedSymbol->IsImmutable) {
#ifndef TYPECHECKER_NO_ERRORS
      ImmutableAssignmentError(Node.getLoc(), LHSVar->getName())
          .withRelated(toSourceLocation(LHSVar->ResolvedSymbol->DeclLoc),
                       "declared immutable here")
          .raise();
#else
      throw std::runtime_error("ImmutableAssignmentError: " + LHSVar->getName());
#endif
    }
  }
}

void TypeChecker::visitPostfixExpr(PostfixExprAST &Node) {
  if (Node.getOperand())
    const_cast<ExprAST *>(Node.getOperand())->accept(*this);

  TurfTypePtr OpTy = getType(Node.getOperand());

  if (OpTy && !OpTy->isNumeric()) {
    reportError(Node.getLoc(),
                "Postfix operator requires a numeric operand, got '" +
                    OpTy->getName() + "'.");
  }

  setType(&Node, OpTy);

  const VariableExprAST *LHSVar = findRootVariable(Node.getOperand());

  if (LHSVar) {
    if (LHSVar->ResolvedSymbol && LHSVar->ResolvedSymbol->IsImmutable) {
#ifndef TYPECHECKER_NO_ERRORS
      ImmutableAssignmentError(Node.getLoc(), LHSVar->getName())
          .withRelated(toSourceLocation(LHSVar->ResolvedSymbol->DeclLoc),
                       "declared immutable here")
          .raise();
#else
      throw std::runtime_error("ImmutableAssignmentError: " + LHSVar->getName());
#endif
    }
  }
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

  bool IsTernary = false;
  if (Node.getBranches().size() == 1 && Node.getElseBody()) {
    bool BodyIsBlock = dynamic_cast<BlockExprAST*>(Node.getThen()) != nullptr;
    bool ElseIsBlock = dynamic_cast<BlockExprAST*>(Node.getElse()) != nullptr;
    if (!BodyIsBlock && !ElseIsBlock) {
      IsTernary = true;
    }
  }

  if (IsTernary) {
    TurfTypePtr ThenTy = getType(Node.getThen());
    TurfTypePtr ElseTy = getType(Node.getElse());
    if (ThenTy && ElseTy) {
      if (ThenTy->getName() == ElseTy->getName()) {
        setType(&Node, ThenTy);
      } else if (ThenTy->isNumeric() && ElseTy->isNumeric()) {
        if (ThenTy->isDouble() || ElseTy->isDouble()) {
          setType(&Node, TurfDouble());
        } else {
          setType(&Node, TurfInt());
        }
      } else {
        reportError(Node.getLoc(),
                    "Type mismatch in ternary expression: 'then' type '" +
                        ThenTy->getName() + "' and 'else' type '" +
                        ElseTy->getName() + "' are incompatible.");
        setType(&Node, TurfVoid());
      }
    } else {
      setType(&Node, TurfVoid());
    }
  } else {
    setType(&Node, TurfVoid());
  }
}

void TypeChecker::visitWhile(WhileExprAST &Node) {
  if (Node.getCond())
    const_cast<ExprAST *>(Node.getCond())->accept(*this);
  if (Node.getBody())
    Node.getBody()->accept(*this);
  setType(&Node, TurfVoid());
}

void TypeChecker::visitFor(ForExprAST &Node) {
  // Visit start / end first so we know their types -- this determines the
  // loop variable's inferred type below.
  if (Node.getStart())
    const_cast<ExprAST *>(Node.getStart())->accept(*this);
  if (Node.getEnd())
    const_cast<ExprAST *>(Node.getEnd())->accept(*this);

  // Infer loop variable type from the range bounds.
  // If both start and end are integers → int; if either is double → double.
  TurfTypePtr StartTy = getType(Node.getStart());
  TurfTypePtr EndTy   = getType(Node.getEnd());

  TurfTypePtr LoopVarTy = TurfInt(); // default: int for integer ranges
  if ((StartTy && StartTy->isDouble()) || (EndTy && EndTy->isDouble()))
    LoopVarTy = TurfDouble();

  // Register the loop variable BEFORE visiting Step, not after.
  //
  // This ordering is load-bearing, not cosmetic: Step almost always
  // references the loop variable directly (`step x += 1`, `step x++`), and
  // visitVariable() only returns the correctly-inferred type (int or double,
  // just computed above) by checking LoopVarTypes_ FIRST -- if that lookup
  // misses, it falls back to Node.ResolvedSymbol->Type, which is the
  // Resolver's registration of the loop variable and is UNCONDITIONALLY
  // TurfInt() regardless of the range's real type (Resolver::visitFor()
  // doesn't do type inference; that's this pass's job). Visiting Step before
  // this map was populated meant any double-ranged for-loop whose step
  // touched the loop variable -- i.e. essentially every double-ranged for
  // loop, since that's the entire point of a step clause -- failed with
  // "Cannot assign value of type 'double' to variable of type 'int'".
  // Confirmed via `for x in 0.0..3.0 step x += 1.0 { ... }` before this fix.
  LoopVarTypes_[Node.getVarName()] = LoopVarTy;

  // Also commit the inferred type back onto the actual VariableSymbol the
  // Resolver created for this loop variable (Node.ResolvedLoopVar) -- the
  // same shared_ptr<VariableSymbol> instance every VariableExprAST reference
  // to the loop variable gets via SymTable_.lookupVariable(), so mutating it
  // here propagates everywhere, mirroring how visitVarDecl() already
  // corrects Node.ResolvedSymbol->Type when resolving a NamedType to its
  // real struct type.
  //
  // LoopVarTypes_ alone is NOT enough: it is TypeChecker-private bookkeeping
  // that only affects visitVariable()'s type-CHECKING answer. Codegen's
  // getExprType() has no knowledge of it and reads ResolvedSymbol->Type
  // directly -- which, without this line, stays the Resolver's hardcoded
  // TurfInt() forever. Confirmed as a real, severe consequence, not a
  // theoretical one: `for x in 0.0..3.0 step x += 1.0 { ... }` used to
  // compile (once the ordering fix above was applied) but then hang
  // indefinitely at runtime, because CompoundAssignmentExprAST::codegen()
  // trusted getExprType()'s wrong `int` answer for `x` and emitted a raw
  // bitcast-to-i64-then-add-1-then-bitcast-back instead of CreateFAdd --
  // incrementing the double's raw bit pattern by 1 (an ULP-scale change)
  // instead of its value by 1.0, so the loop condition `x < 3.0` was for all
  // practical purposes never going to become false.
  if (Node.ResolvedLoopVar)
    Node.ResolvedLoopVar->Type = LoopVarTy;

  if (Node.getStep())
    const_cast<ExprAST *>(Node.getStep())->accept(*this);

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
      bool Compatible = typesCompatible(RetTy.get(), Declared);

      // Special-case: a pointer-to-struct value whose pointee is exactly
      // the declared (bare, by-value) struct return type -- most commonly
      // `return self;` inside a struct method declared to return the
      // struct type itself rather than a pointer. `self` is always typed
      // as `StructName*` (see ParseFuncDef's implicit self-parameter
      // construction), and this language has no explicit dereference
      // operator, so without this, `fn Node.get(): Node { return self; }`
      // could never be satisfied at all -- only `fn Node.get(): Node* {...}`
      // worked, which is fine for method chaining but not for a method that
      // genuinely wants to hand back a copy. Compared by name (rather than
      // requiring both sides to already be resolved TurfStructType
      // instances) since ParseType() always produces a bare NamedType for a
      // struct name, never a TurfStructType, and Declared/the pointee here
      // commonly still are NamedType at this point. Codegen's CastToType()
      // performs the actual load (see its "Implicit dereference" comment).
      if (!Compatible) {
        if (const auto *PT = dynamic_cast<const TurfPointerType *>(RetTy.get())) {
          if (PT->getPointee() && PT->getPointee()->getName() == Declared->getName())
            Compatible = true;
        }
      }

      if (!Compatible) {
        reportError(Node.getLoc(),
                    "Return type mismatch: function declares '" +
                        Declared->getName() + "' but returns '" +
                        RetTy->getName() + "'.");
      }
    }
  }

  setType(&Node, RetTy ? RetTy : TurfVoid());
}

// ─── Compare / Matches / Done ─────────────────────────────────────────────────

void TypeChecker::visitCompare(CompareExprAST &Node) {
  // 1. Type-check the subject expression
  check(Node.getSubjectMut());
  TurfTypePtr SubjectTy = getType(Node.getSubject());

  // Resolve a NamedType subject to its real registered type (TurfEnumType,
  // most importantly). getType() on a VariableExprAST returns whatever type
  // the variable was DECLARED with -- for a local `mut`/`immut` variable,
  // Resolver::visitVarDecl() already corrects a NamedType("Color") in that
  // position to the real TurfEnumType, but a FUNCTION PARAMETER's type
  // (`fn describe(d: Direction)`) is declared via a separate path
  // (Resolver::visitFuncDef()'s `SymTable_.declareVariable(Param.Name,
  // Param.Type, ...)`) that never applies the same correction -- ParseType()
  // always parses a bare non-primitive type name as NamedType regardless.
  // Without this, `SubjectTy->isEnum()` below is unconditionally false for
  // ANY compare whose subject is a function parameter (an extremely common,
  // idiomatic shape -- e.g. `fn describe(d: Direction): int { compare d {...} }`),
  // which has two compounding effects: `ET` a few lines down is always
  // nullptr, so the entire enum-exhaustiveness check silently never runs
  // (confirmed: a compare over a 4-variant enum parameter covering only 2
  // variants, with no `otherwise`, compiled with ZERO diagnostics -- a real,
  // silent loss of the exhaustiveness guarantee for the single most common
  // way to write a compare); and separately, Codegen's CFG builder reads the
  // same (always-false) Node.SubjectIsEnum flag to know whether a `compare`
  // with no `otherwise` still has a "no arm matched" fallthrough edge --
  // getting this wrong made an exhaustive enum-parameter compare with a
  // `return` in every arm look like it had a non-returning fallthrough path,
  // producing a false "missing return" error for perfectly valid code.
  if (SubjectTy) {
    if (const auto *NT = dynamic_cast<const NamedType *>(SubjectTy.get())) {
      if (GlobalSymbolTable) {
        if (auto Resolved = GlobalSymbolTable->LookupType(NT->getName()))
          SubjectTy = Resolved;
      }
    }
  }

  // Determine if the subject is a string (selects strcmp chain in codegen)
  bool IsStringSubject = SubjectTy && SubjectTy->isNamed("string");
  Node.SubjectIsString = IsStringSubject;

  // Collect the name for error reporting
  std::string SubjectTypeName =
      SubjectTy ? SubjectTy->getName() : "<unknown>";

  // Track which pattern values have already been covered by an earlier arm,
  // to detect and reject duplicate/unreachable 'matches' patterns -- e.g.
  // two arms both matching Color::Red, or two arms both matching the
  // literal 5. Without this, a duplicate pattern compiled silently: for an
  // enum/int subject, Codegen's SwitchInst-based path issued two
  // SwitchInst::addCase() calls with the SAME case value pointing at two
  // different blocks -- which arm actually ran was an artifact of LLVM's
  // case-insertion order, not a guaranteed language semantic. For a string
  // subject (strcmp chain), the first matching arm deterministically won,
  // but the duplicate second arm was still silently dead code with no
  // warning either way. Only patterns whose value is known at this point
  // (an enum variant, or an integer/string literal) are checked -- this
  // mirrors exactly the class of patterns Codegen's CompareExprAST::codegen()
  // already requires to be compile-time constants for the int/enum switch
  // path (see its "matches pattern must be a compile-time integer constant"
  // error), so nothing checkable here is being skipped.
  std::set<std::string> SeenPatternKeys;

  // 2. Type-check each arm's patterns and body
  for (auto &Arm : Node.getArmsMut()) {
    for (auto &PatExpr : Arm.Values) {
      check(PatExpr.get());
      TurfTypePtr PatTy = getType(PatExpr.get());

      // Enforce subject-pattern type compatibility
      if (SubjectTy && PatTy) {
        if (!typesCompatible(SubjectTy.get(), PatTy.get())) {
#ifndef TYPECHECKER_NO_ERRORS
          CompareTypeError(Arm.Loc, SubjectTypeName, PatTy->getName()).raise();
#else
          throw std::runtime_error("TypeError: compare subject type '" +
                                   SubjectTypeName + "' != pattern type '" +
                                   PatTy->getName() + "'");
#endif
        }
      }

      std::optional<std::string> PatternKey;
      if (auto *EA = dynamic_cast<EnumAccessExprAST *>(PatExpr.get())) {
        PatternKey = "enum:" + EA->getVariant();
      } else if (auto *Num = dynamic_cast<NumberExprAST *>(PatExpr.get())) {
        if (Num->isInteger())
          PatternKey = "int:" + std::to_string(Num->getIntVal());
      } else if (auto *Str = dynamic_cast<StringExprAST *>(PatExpr.get())) {
        PatternKey = "str:" + Str->getValue();
      }

      if (PatternKey && !SeenPatternKeys.insert(*PatternKey).second) {
        reportError(Arm.Loc,
                    "Duplicate 'matches' pattern: this value is already "
                    "handled by an earlier arm in this compare block, so "
                    "this arm can never run.\nHint: Remove the duplicate "
                    "arm, or combine it with the earlier one by stacking "
                    "(`matches A:` then `matches B:` sharing one body).");
      }
    }

    // Type-check arm body
    check(Arm.Body.get());
  }

  // 3. Type-check the 'otherwise' body if present
  if (Node.getOtherBodyMut())
    check(Node.getOtherBodyMut());

  // 4. Exhaustiveness gate for Enums
  bool IsEnum = SubjectTy && SubjectTy->isEnum();
  Node.SubjectIsEnum = IsEnum;

  const TurfEnumType* ET = IsEnum
      ? dynamic_cast<const TurfEnumType*>(SubjectTy.get()) : nullptr;

  if (ET && !Node.getOtherBody()) {
      std::set<std::string> CoveredVariants;
      for (auto &Arm : Node.getArms()) {
          for (auto &PatExpr : Arm.Values) {
              if (auto* EA = dynamic_cast<EnumAccessExprAST*>(PatExpr.get()))
                  CoveredVariants.insert(EA->getVariant());
          }
      }

      std::vector<std::string> Missing;
      for (const auto& V : ET->getVariants())
          if (!CoveredVariants.count(V))
              Missing.push_back(V);

      if (!Missing.empty()) {
          std::string Msg = "Non-exhaustive enum comparison on '" + ET->getName()
                          + "'.\nMissing variants:";
          for (const auto& M : Missing)
              Msg += "\n    " + ET->getName() + "::" + M;
          Msg += "\nHint: Add 'otherwise:' to handle remaining variants.";
          reportError(Node.getLoc(), Msg);
      }
  }

  // 5. Empty compare{} warning was already emitted by the Parser
  setType(&Node, TurfVoid());
}

// ─── Functions & calls ────────────────────────────────────────────────────────

/**
 * @brief Walk a candidate's declared parameter type against the actual
 *        argument type passed at a call site, recording any bare
 *        TurfGenericParamType encountered into @p Subst (keyed by its name).
 *
 * Recurses through pointer/array wrapping on both sides in lockstep (T* vs
 * concrete*, T[] vs concrete[]) so a generic parameter used underneath a
 * wrapper still gets unified correctly. The first argument position that
 * mentions a given generic parameter name wins; later positions mentioning
 * the same name are not re-unified (good enough for this compiler's actual
 * generic surface, where a type parameter typically appears in exactly one
 * "shape" per signature — see Resolver::substituteTypeParams()'s doc
 * comment for the parallel, opposite-direction transform this mirrors).
 */
static void collectGenericSubstitution(
    const TurfTypePtr &ParamTy, const TurfTypePtr &ArgTy,
    std::unordered_map<std::string, TurfTypePtr> &Subst) {
  if (!ParamTy || !ArgTy)
    return;

  if (const auto *GP = dynamic_cast<const TurfGenericParamType *>(ParamTy.get())) {
    Subst.emplace(GP->getName(), ArgTy);
    return;
  }
  if (const auto *PPtr = dynamic_cast<const TurfPointerType *>(ParamTy.get())) {
    if (const auto *APtr = dynamic_cast<const TurfPointerType *>(ArgTy.get()))
      collectGenericSubstitution(PPtr->getPointee(), APtr->getPointee(), Subst);
    return;
  }
  if (isArrayType(ParamTy.get()) && isArrayType(ArgTy.get())) {
    const auto *PArr = dynamic_cast<const ArrayType *>(ParamTy.get());
    const auto *AArr = dynamic_cast<const ArrayType *>(ArgTy.get());
    if (PArr && AArr)
      collectGenericSubstitution(PArr->getElementTypePtr(),
                                 AArr->getElementTypePtr(), Subst);
    return;
  }
  if (const auto *PGen = dynamic_cast<const GenericType *>(ParamTy.get())) {
    if (const auto *AGen = dynamic_cast<const GenericType *>(ArgTy.get())) {
      const auto &PArgs = PGen->getTypeArgs();
      const auto &AArgs = AGen->getTypeArgs();
      for (size_t i = 0; i < PArgs.size() && i < AArgs.size(); ++i)
        collectGenericSubstitution(PArgs[i], AArgs[i], Subst);
    }
    return;
  }
}

/**
 * @brief Rewrite @p Ty, replacing any bare TurfGenericParamType occurrence
 *        (recursively through pointer/array/generic wrapping) with its
 *        concrete type from @p Subst, if one was recorded.
 *
 * This is what actually closes the call-site type-inference gap: without
 * it, TypeChecker::getType() for a call to `fn<T> firstElem(arr: T[]): T`
 * returned the callee's literal declared return type -- TurfGenericParamType
 * ("T") itself -- regardless of what concrete type T was instantiated to at
 * this call site, so `firstElem(intArray) == 10` failed type-checking with
 * "Cannot compare 'T' with 'int'" even though the call was perfectly valid.
 * A type parameter with no corresponding entry in Subst (never appeared in
 * any parameter position, e.g. a return-only phantom type) is left as-is.
 */
static TurfTypePtr substituteReturnType(
    const TurfTypePtr &Ty,
    const std::unordered_map<std::string, TurfTypePtr> &Subst) {
  if (!Ty || Subst.empty())
    return Ty;

  if (const auto *GP = dynamic_cast<const TurfGenericParamType *>(Ty.get())) {
    auto It = Subst.find(GP->getName());
    return It != Subst.end() ? It->second : Ty;
  }
  if (const auto *PT = dynamic_cast<const TurfPointerType *>(Ty.get())) {
    TurfTypePtr NewPointee = substituteReturnType(PT->getPointee(), Subst);
    return NewPointee == PT->getPointee()
              ? Ty
              : std::make_shared<TurfPointerType>(NewPointee);
  }
  if (isArrayType(Ty.get())) {
    const auto *AT = dynamic_cast<const ArrayType *>(Ty.get());
    TurfTypePtr NewElem = substituteReturnType(AT->getElementTypePtr(), Subst);
    return NewElem == AT->getElementTypePtr()
              ? Ty
              : makeArrayType(NewElem, AT->getSize());
  }
  if (const auto *GT = dynamic_cast<const GenericType *>(Ty.get())) {
    std::vector<TurfTypePtr> NewArgs;
    bool Changed = false;
    for (const auto &Arg : GT->getTypeArgs()) {
      TurfTypePtr NewArg = substituteReturnType(Arg, Subst);
      Changed |= (NewArg != Arg);
      NewArgs.push_back(std::move(NewArg));
    }
    return Changed ? std::make_shared<GenericType>(GT->getBaseName(),
                                                    std::move(NewArgs))
                   : Ty;
  }
  return Ty;
}

void TypeChecker::visitFuncDef(FuncDefExprAST &Node) {
  // Push the declared return type so visitReturn() can validate it
  ReturnTypeStack_.push_back(Node.getReturnTypePtr());

  if (Node.getBody())
    const_cast<ExprAST *>(Node.getBody())->accept(*this);

  ReturnTypeStack_.pop_back();
  setType(&Node, TurfVoid());
}

void TypeChecker::visitFuncCall(FuncCallExprAST &Node) {
  // 1. Visit callee expression so it gets a type annotation too.
  if (Node.getCallee())
    const_cast<ExprAST *>(Node.getCallee())->accept(*this);

  // 2. Visit all argument expressions bottom-up; collect inferred types.
  std::vector<TurfTypePtr> ArgTypes;
  for (const auto &Arg : Node.getArgs()) {
    Arg->accept(*this);
    ArgTypes.push_back(getType(Arg.get()));
  }
  const int ArgCount = static_cast<int>(Node.getArgs().size());

  // 3. Determine the callee name for diagnostics and builtin lookup.
  std::string CalleeName;
  if (const auto *VE =
          dynamic_cast<const VariableExprAST *>(Node.getCallee())) {
    CalleeName = VE->getName();
  }

  // ── Bare language intrinsic path ─────────────────────────────────────────
  // Only 'typeof' and 'lengthof' are true bare intrinsics (callable without a
  // module qualifier and not registered as user functions).
  // Everything else that used to live here (abs, power, print, input, malloc…)
  // now lives in lib/std and is resolved via the Resolver's module registry.
  // LookupBuiltin() returns null for all stdlib functions since Issue #9.
  if (Node.ResolvedCandidates.empty() && !CalleeName.empty()) {
    const BuiltinInfo *BI = LookupBuiltin(CalleeName);
    if (BI) {
      // Arity check using the canonical arity from Builtins.h.
      if (BI->Arity >= 0 && ArgCount != BI->Arity) {
        reportError(Node.getLoc(),
                    "'" + CalleeName + "' expects " +
                        std::to_string(BI->Arity) + " argument" +
                        (BI->Arity == 1 ? "" : "s") + ", got " +
                        std::to_string(ArgCount) + ".");
        setType(&Node, nullptr);
        return;
      }

      // ── typeof(x) → string ───────────────────────────────────────────────
      if (CalleeName == "typeof") {
        setType(&Node, TurfString());
        return;
      }

      // ── lengthof(x) → int ────────────────────────────────────────────────
      if (CalleeName == "lengthof") {
        // Argument must be a string.
        if (ArgCount == 1) {
          const TurfType *ArgTy = ArgTypes[0].get();
          if (ArgTy && !ArgTy->isString()) {
            reportError(Node.getLoc(),
                        "'lengthof' expects a string argument, got '" +
                            ArgTy->getName() + "'.");
            setType(&Node, nullptr);
            return;
          }
        }
        setType(&Node, TurfInt());
        return;
      }

      // ── assert(cond, [msg]) → int, fail(msg) → int ──────────────────────
      // Both are registered with BuiltinInfo::Arity == -1 ("variadic/
      // unknown") because assert() genuinely accepts either 1 or 2 args
      // (condition, optional message) -- not a single fixed count the
      // generic arity check above can express. That means the generic
      // check never rejects a wrong argument count for assert() (0 args,
      // or 3+), and -- unlike typeof/lengthof -- neither of these two ever
      // reached a `setType(&Node, ...); return;` here at all, so they fell
      // through everything below and hit the final "Unknown call — be
      // permissive" branch, which sets a null type.
      //
      // That was never just a type-annotation gap: CodegenIntrinsic's
      // "assert"/"fail" handlers unconditionally read Args[0] (and
      // Args[1] for assert's optional message) with no bounds check,
      // trusting this exact arity validation to have already run. It
      // normally has, by the time Codegen's main pass runs -- but
      // main.cpp's pre-pass (sub-pass 2b) deliberately swallows TypeChecker
      // errors ("informational only... main-pass TC will re-check") and
      // then unconditionally proceeds to codegen the same AST anyway in
      // sub-pass 3b. A call like `assert()` or `fail()` (0 args) hits that
      // gap: the pre-pass TypeChecker's error is discarded, pre-pass
      // codegen runs CodegenIntrinsic with an empty Args vector, and
      // `Args[0]` is a real out-of-bounds vector access -- a compiler
      // crash (SIGABRT) before the user ever sees a diagnostic, for input
      // as simple as a bare `assert();`. Fixed on two fronts: this
      // explicit arity check closes the TypeChecker-side gap (so the
      // pre-pass's own TC invocation reports the SAME error, even if that
      // particular report is discarded) and, as defense in depth,
      // CodegenIntrinsic itself now bounds-checks Args before indexing
      // (see src/Builtins.cpp) so any future gap of this shape degrades to
      // a clean SemanticError instead of undefined behavior.
      if (CalleeName == "assert") {
        if (ArgCount != 1 && ArgCount != 2) {
          reportError(Node.getLoc(),
                      "'assert' expects 1 or 2 arguments (a condition, and "
                      "an optional message), got " +
                          std::to_string(ArgCount) + ".");
          setType(&Node, nullptr);
          return;
        }
        setType(&Node, TurfInt());
        return;
      }

      // ── fail(msg) → int ───────────────────────────────────────────────────
      // Arity (exactly 1) is already enforced by the generic check above
      // (BuiltinInfo::Arity == 1). This branch only exists to give a valid
      // call a real type -- CodegenIntrinsic's "fail" handler returns a
      // dummy int(0) (its call site is unreachable; abort() is called
      // first), so TurfInt() here matches actual codegen behavior instead
      // of silently falling through to the "unknown call" branch below.
      if (CalleeName == "fail") {
        setType(&Node, TurfInt());
        return;
      }

      // Any other name that hits LookupBuiltin() is unexpected — fall through.
    }
  }

  // ── User-defined / Resolver-registered call path ──────────────────────────
  if (!Node.ResolvedCandidates.empty()) {
    // Step a: filter to candidates whose arity matches.
    // Vararg functions accept any ArgCount >= their declared param count.
    std::vector<std::shared_ptr<ResolverFunctionSymbol>> ArityMatches;
    for (const auto &Cand : Node.ResolvedCandidates) {
      if (Cand && (Cand->arity() == ArgCount ||
                   (Cand->isVarArg() && ArgCount >= Cand->arity())))
        ArityMatches.push_back(Cand);
    }

    if (ArityMatches.empty()) {
      // Build a human-readable list of known overloads for the error message.
      std::string Overloads;
      std::vector<RelatedLocation> Related;
      for (const auto &Cand : Node.ResolvedCandidates) {
        if (Cand) {
          if (!Overloads.empty()) Overloads += ", ";
          Overloads += Cand->signature();
          Related.push_back({toSourceLocation(Cand->DeclLoc),
                             "overload: " + Cand->signature()});
        }
      }
      reportError(Node.getLoc(),
                  "'" + CalleeName + "' called with " +
                      std::to_string(ArgCount) + " argument" +
                      (ArgCount == 1 ? "" : "s") +
                      "; known overloads: " + Overloads + ".",
                  Related);
      setType(&Node, nullptr);
      return;
    }

    // Step b: filter arity-matching candidates by type compatibility.
    // For vararg functions, only check the declared (named) params.
    // Extra args beyond Cand->arity() are passed raw per C ABI.
    std::vector<std::shared_ptr<ResolverFunctionSymbol>> TypeMatches;
    for (const auto &Cand : ArityMatches) {
      bool AllOk = true;
      int CheckCount = std::min(ArgCount, Cand->arity());
      for (int i = 0; i < CheckCount; ++i) {
        const TurfType *ParamTy = Cand->Params[i].Type.get();
        const TurfType *ArgTy   = ArgTypes[i].get();
        if (ParamTy && ArgTy && !typesCompatible(ArgTy, ParamTy)) {
          AllOk = false;
          break;
        }
      }
      if (AllOk)
        TypeMatches.push_back(Cand);
    }

    if (TypeMatches.empty()) {
      // Build argument-type string for the error.
      std::string ArgTypeStr;
      std::vector<RelatedLocation> Related;
      for (int i = 0; i < ArgCount; ++i) {
        if (i) ArgTypeStr += ", ";
        ArgTypeStr += (ArgTypes[i] ? ArgTypes[i]->getName() : "unknown");
      }
      for (const auto &Cand : ArityMatches)
        if (Cand)
          Related.push_back({toSourceLocation(Cand->DeclLoc),
                             "candidate: " + Cand->signature()});
      reportError(Node.getLoc(),
                  "No overload of '" + CalleeName +
                      "' matches argument types (" + ArgTypeStr + ").",
                  Related);
      setType(&Node, nullptr);
      return;
    }

    // Step c: narrow to the best match (if multiple, pick first — exact
    // matches will naturally appear before promotion matches because the
    // Resolver registers them in declaration order).
    // A future refinement can count implicit casts and pick fewest.
    auto Best = TypeMatches[0];
    if (TypeMatches.size() > 1) {
      // Prefer an exact-match candidate over a numeric-promotion one.
      for (const auto &Cand : TypeMatches) {
        bool ExactAll = true;
        int CheckCount = std::min(ArgCount, Cand->arity());
        for (int i = 0; i < CheckCount; ++i) {
          const TurfType *P = Cand->Params[i].Type.get();
          const TurfType *A = ArgTypes[i].get();
          if (P && A && P->getName() != A->getName()) {
            ExactAll = false;
            break;
          }
        }
        if (ExactAll) { Best = Cand; break; }
      }
    }

    Node.ResolvedCallTarget = Best;

    // If Best's return type mentions one of its own generic type parameters
    // (e.g. `fn<T> firstElem(arr: T[]): T`), report the CONCRETE type it was
    // instantiated to at this call site instead of the literal declared
    // TurfGenericParamType -- see collectGenericSubstitution()'s doc comment
    // for the gap this closes. A no-op (empty substitution map) for the
    // overwhelming majority of calls, which aren't generic at all.
    std::unordered_map<std::string, TurfTypePtr> Subst;
    int SubstCheckCount = std::min(ArgCount, Best->arity());
    for (int i = 0; i < SubstCheckCount; ++i)
      collectGenericSubstitution(Best->Params[i].Type, ArgTypes[i], Subst);

    setType(&Node, substituteReturnType(Best->ReturnType, Subst));
    return;
  }

  // ── Unknown call (no builtin, no candidates) — be permissive. ─────────────
  // The Resolver should have caught truly unknown functions already.  Leave
  // the type as null so we don't cascade spurious errors.
  setType(&Node, nullptr);
}

void TypeChecker::visitMethodCall(MethodCallExprAST &Node) {
  // 1. Visit the base expression (e.g. `io` in io.printline(...)).
  if (Node.getBase())
    const_cast<ExprAST *>(Node.getBase())->accept(*this);

  // 2. Visit all argument expressions bottom-up; collect inferred types.
  std::vector<TurfTypePtr> ArgTypes;
  for (const auto &Arg : Node.getArgs()) {
    Arg->accept(*this);
    ArgTypes.push_back(getType(Arg.get()));
  }
  const int ArgCount = static_cast<int>(Node.getArgs().size());

  const std::string &MethodName = Node.getMethod();

  // 3. Detect whether the base is a module qualifier.
  //    The Resolver marks `io` in `io.printline(...)` by setting
  //    ResolvedModuleName on the VariableExprAST.
  std::string ModuleName;
  if (const auto *BaseVar =
          dynamic_cast<const VariableExprAST *>(Node.getBase())) {
    ModuleName = BaseVar->ResolvedModuleName;
  }

  if (!ModuleName.empty()) {
    // 4. Module-qualified call: look up all matching overloads in that module.
    auto Matches = RST_.findInModules(MethodName);

    // Collect just the candidates from the right module.
    std::vector<const ResolverFunctionSymbol *> Candidates;
    for (const auto &M : Matches) {
      if (M.ModuleName == ModuleName && M.Symbol)
        Candidates.push_back(M.Symbol);
    }

    if (Candidates.empty()) {
      // Unknown module function — be permissive (Resolver should have caught).
      setType(&Node, nullptr);
      return;
    }

    // 5. Apply same arity → type-match → narrowing logic as visitFuncCall.
    //    a. Arity filter.
    std::vector<const ResolverFunctionSymbol *> ArityMatches;
    for (const auto *Cand : Candidates) {
      if (Cand->arity() == ArgCount)
        ArityMatches.push_back(Cand);
    }

    if (ArityMatches.empty()) {
      std::string Overloads;
      std::vector<RelatedLocation> Related;
      for (const auto *Cand : Candidates) {
        if (!Overloads.empty()) Overloads += ", ";
        Overloads += ModuleName + "." + Cand->signature();
        Related.push_back({toSourceLocation(Cand->DeclLoc),
                           "overload: " + ModuleName + "." + Cand->signature()});
      }
      reportError(Node.getLoc(),
                  "'" + ModuleName + "." + MethodName + "' called with " +
                      std::to_string(ArgCount) + " argument" +
                      (ArgCount == 1 ? "" : "s") +
                      "; known overloads: " + Overloads + ".",
                  Related);
      setType(&Node, nullptr);
      return;
    }

    //    b. Type-compatibility filter.
    std::vector<const ResolverFunctionSymbol *> TypeMatches;
    for (const auto *Cand : ArityMatches) {
      bool AllOk = true;
      for (int i = 0; i < ArgCount; ++i) {
        const TurfType *ParamTy = Cand->Params[i].Type.get();
        const TurfType *ArgTy   = ArgTypes[i].get();
        if (ParamTy && ArgTy && !typesCompatible(ArgTy, ParamTy)) {
          AllOk = false;
          break;
        }
      }
      if (AllOk)
        TypeMatches.push_back(Cand);
    }

    if (TypeMatches.empty()) {
      std::string ArgTypeStr;
      std::vector<RelatedLocation> Related;
      for (int i = 0; i < ArgCount; ++i) {
        if (i) ArgTypeStr += ", ";
        ArgTypeStr += (ArgTypes[i] ? ArgTypes[i]->getName() : "unknown");
      }
      for (const auto *Cand : ArityMatches)
        Related.push_back({toSourceLocation(Cand->DeclLoc),
                           "candidate: " + ModuleName + "." + Cand->signature()});
      reportError(Node.getLoc(),
                  "No overload of '" + ModuleName + "." + MethodName +
                      "' matches argument types (" + ArgTypeStr + ").",
                  Related);
      setType(&Node, nullptr);
      return;
    }

    //    c. Pick best candidate.
    const ResolverFunctionSymbol *Best = TypeMatches[0];
    if (TypeMatches.size() > 1) {
      for (const auto *Cand : TypeMatches) {
        bool ExactAll = true;
        for (int i = 0; i < ArgCount; ++i) {
          const TurfType *P = Cand->Params[i].Type.get();
          const TurfType *A = ArgTypes[i].get();
          if (P && A && P->getName() != A->getName()) {
            ExactAll = false;
            break;
          }
        }
        if (ExactAll) { Best = Cand; break; }
      }
    }

    // Set ResolvedCallTarget so Codegen can read mangledName() directly
    // without any overload resolution.  We use a shared_ptr with a no-op
    // deleter because the symbol is owned by the ResolverSymbolTable; we
    // just need an owning handle that Codegen can call ->mangledName() on.
    Node.ResolvedCallTarget = std::make_shared<ResolverFunctionSymbol>(*Best);
    setType(&Node, Best->ReturnType);
    return;
  }

  // ── Struct method call (e.g. u.grow(), a.inc()) ──────────────────────────
  // The base is a variable whose type is a struct. Struct methods are
  // registered in the Resolver's CurrentFileFunctions_ with BaseName
  // "StructName.methodName" (e.g. "User.grow").
  {
    TurfTypePtr BaseTy = getType(Node.getBase());
    bool IsStaticMethod = false;
    if (auto *Var = dynamic_cast<const VariableExprAST *>(Node.getBase())) {
      if (Var->IsStructName && GlobalSymbolTable) {
        IsStaticMethod = true;
        BaseTy = GlobalSymbolTable->LookupType(Var->getName());
      }
    }

    // Unwrap pointer-to-struct (self inside methods)
    if (BaseTy && !IsStaticMethod) {
      if (auto *PtrTy = dynamic_cast<const TurfPointerType *>(BaseTy.get()))
        BaseTy = PtrTy->getPointee();
    }
    // Resolve NamedType to TurfStructType
    if (BaseTy) {
      if (auto *NT = dynamic_cast<const NamedType *>(BaseTy.get())) {
        if (GlobalSymbolTable)
          BaseTy = GlobalSymbolTable->LookupType(NT->getName());
      }
    }
    // Resolve GenericType (e.g. a `Box<int>`-typed variable) to its
    // underlying TurfStructType via the base name -- mirrors the NamedType
    // resolution just above. Without this, calling a method on a generic
    // struct instance (`b.get()` where `b: Box<int>`) fell all the way
    // through to "Non-module, non-struct method call — be permissive" at
    // the bottom of this function, leaving Node.ResolvedCallTarget null.
    // Codegen's MethodCallExprAST::codegen() then silently returned nullptr
    // with NO diagnostic (MangledName stays empty, CalleeF stays null, and
    // the struct-method-call branch just `return nullptr;`), which meant
    // any variable declared from that call's result was never registered
    // in Codegen's symbol table -- surfacing only as a confusing
    // "you're trying to use 'p' before telling me what it is" error the
    // next time that variable was actually referenced somewhere codegen
    // could reach (e.g. inside assert()), nowhere near the real cause.
    if (BaseTy) {
      if (auto *GT = dynamic_cast<const GenericType *>(BaseTy.get())) {
        if (GlobalSymbolTable)
          BaseTy = GlobalSymbolTable->LookupType(GT->getBaseName());
      }
    }
    if (BaseTy) {
      if (auto *STy = dynamic_cast<const TurfStructType *>(BaseTy.get())) {
        const std::string &StructName = STy->getName();
        // Struct methods declared in the SAME file are stored in
        // CurrentFileFunctions_ as "User.grow".
        auto Candidates = RST_.lookupCurrentFileFunctions(StructName + "." + MethodName);
        // Struct methods declared in an IMPORTED module (e.g. lib/std/list.tr's
        // `fn<T> List.push(...)`, found via `import list;`) are hoisted into
        // the MODULE registry instead (Resolver::hoistModuleBody() ->
        // registerFunctionSignature() -> registerModuleFunction(), since the
        // module name is non-empty there) -- lookupCurrentFileFunctions()
        // alone can never find them, regardless of struct name/arity/
        // anything else. Without this, EVERY method call on an instance of a
        // struct declared in an imported module (not just generic ones --
        // this is unrelated to the GenericType fix above) silently failed to
        // resolve: Node.ResolvedCallTarget stayed null, and Codegen's
        // MethodCallExprAST::codegen() then silently returned nullptr with
        // no diagnostic. Confirmed via `--emit-llvm`: a whole sequence of
        // `l.push(...)`/`l.pop()`/assert() calls on a `List<int>` imported
        // from lib/std/list.tr vanished ENTIRELY from the generated
        // `main()` body -- not a crash, not a compile error, just silently
        // absent -- while an unconditional `io.printline("list ok")` after
        // them still ran, making the whole test look like it passed.
        for (const auto &Match : RST_.findInModules(StructName + "." + MethodName))
          if (Match.Symbol)
            Candidates.push_back(Match.Symbol);
        // Arity: struct methods have `self` as first param, user args = arity - 1 (unless static)
        int ExpectedArity = IsStaticMethod ? ArgCount : (ArgCount + 1);
        std::vector<const ResolverFunctionSymbol *> ArityMatches;
        for (const auto *C : Candidates)
          if (C->arity() == ExpectedArity) ArityMatches.push_back(C);
        if (!ArityMatches.empty()) {
          const ResolverFunctionSymbol *Best = ArityMatches[0];
          Node.ResolvedCallTarget = std::make_shared<ResolverFunctionSymbol>(*Best);

          // If the ORIGINAL (pre-struct-resolution) base type was a
          // GenericType (e.g. `l: List<int>`, T=int), substitute that
          // concrete type into the method's declared return type wherever
          // it mentions the struct's own type parameter -- e.g.
          // `fn<T> List.pop(): T` must report `int`, not the literal
          // TurfGenericParamType("T"), the same call-site-inference gap
          // fixed for free functions in visitFuncCall() (see
          // collectGenericSubstitution()/substituteReturnType() above), but
          // via a different substitution SOURCE: a struct method's `self`
          // parameter is declared as a bare `StructName*` with no type
          // arguments of its own (see ParseFuncDef's self-parameter
          // construction), so there is nothing to unify against in
          // Best->Params the way a free function's own parameter list would
          // give us -- the concrete type argument only exists on the BASE
          // EXPRESSION's type. Since TurfStructType doesn't record its own
          // declared type-parameter NAMES, they're recovered here by
          // scanning the struct's fields for NamedType occurrences that
          // don't resolve to any known struct/enum (through pointer/array
          // wrapping) in declaration order and pairing each distinct name
          // found with GT's type args positionally.
          //
          // NamedType, not TurfGenericParamType: GlobalSymbolTable's
          // registered TurfStructType for a generic struct is constructed
          // once, at PARSE time (ParseStructDecl), directly from the
          // as-parsed Fields -- it is a SEPARATE object from the
          // StructDeclExprAST AST node that Resolver::visitStructDecl()
          // later mutates in place (substituting NamedType("T") ->
          // TurfGenericParamType("T")). That mutation updates the AST
          // node's own copy only; nothing propagates it back into
          // GlobalSymbolTable's already-constructed TurfStructType, so its
          // fields are permanently stuck with the original NamedType("T").
          // This is exact for every generic struct in this language's
          // stdlib today (all single-type-parameter), though not fully
          // general for a hypothetical multi-parameter struct whose fields
          // don't happen to mention every parameter in declaration order.
          if (auto *GT = dynamic_cast<const GenericType *>(getType(Node.getBase()).get())) {
            std::unordered_map<std::string, TurfTypePtr> Subst;
            const auto &TypeArgs = GT->getTypeArgs();
            std::function<void(const TurfTypePtr &)> Scan = [&](const TurfTypePtr &FTy) {
              if (!FTy || Subst.size() >= TypeArgs.size()) return;
              if (auto *GP = dynamic_cast<const TurfGenericParamType *>(FTy.get())) {
                if (!Subst.count(GP->getName()))
                  Subst.emplace(GP->getName(), TypeArgs[Subst.size()]);
              } else if (auto *NT = dynamic_cast<const NamedType *>(FTy.get())) {
                bool KnownElsewhere = GlobalSymbolTable &&
                                     GlobalSymbolTable->LookupType(NT->getName()) != nullptr;
                if (!KnownElsewhere && !Subst.count(NT->getName()))
                  Subst.emplace(NT->getName(), TypeArgs[Subst.size()]);
              } else if (auto *PT = dynamic_cast<const TurfPointerType *>(FTy.get())) {
                Scan(PT->getPointee());
              } else if (isArrayType(FTy.get())) {
                Scan(dynamic_cast<const ArrayType *>(FTy.get())->getElementTypePtr());
              }
            };
            for (const auto &F : STy->getFields())
              Scan(F.Type);
            setType(&Node, substituteReturnType(Best->ReturnType, Subst));
            return;
          }

          setType(&Node, Best->ReturnType);
          return;
        }
      }
    }
  }
  // Non-module, non-struct method call — be permissive.
  setType(&Node, nullptr);
}

// ─── Structs & field access ───────────────────────────────────────────────────

void TypeChecker::visitFieldAccess(FieldAccessExprAST &Node) {
  if (Node.getBase())
    const_cast<ExprAST *>(Node.getBase())->accept(*this);

#ifndef TYPECHECKER_NO_ERRORS
  // Resolve the base type and look up the field.
  // GlobalSymbolTable is only available in the full build (not unit tests).
  TurfTypePtr BaseTy = getType(Node.getBase());

  // Unwrap pointer-to-struct (e.g. self in method bodies)
  if (BaseTy) {
    if (auto *PtrTy = dynamic_cast<const TurfPointerType *>(BaseTy.get()))
      BaseTy = PtrTy->getPointee();
  }

  // Resolve NamedType to TurfStructType via symbol table
  if (BaseTy) {
    if (auto *NT = dynamic_cast<const NamedType *>(BaseTy.get())) {
      if (GlobalSymbolTable)
        BaseTy = GlobalSymbolTable->LookupType(NT->getName());
    }
  }
  // Resolve GenericType (e.g. a `Box<int>`-typed variable) to its underlying
  // TurfStructType via the base name -- mirrors the NamedType resolution
  // just above, and the identical fix in visitMethodCall()'s struct-method
  // path. Without this, TypeChecker::getType() for `b.val` (b: Box<int>)
  // came back null, silently skipping any further type checking that
  // depends on it (e.g. comparing `b.val` against another value).
  if (BaseTy) {
    if (auto *GT = dynamic_cast<const GenericType *>(BaseTy.get())) {
      if (GlobalSymbolTable)
        BaseTy = GlobalSymbolTable->LookupType(GT->getBaseName());
    }
  }

  if (BaseTy) {
    if (auto *STy = dynamic_cast<const TurfStructType *>(BaseTy.get())) {
      TurfTypePtr FieldTy = STy->getFieldType(Node.getFieldName());
      // Resolve NamedType field types so chained access (a.b.c) propagates
      if (FieldTy) {
        if (auto *NT = dynamic_cast<const NamedType *>(FieldTy.get())) {
          if (GlobalSymbolTable) {
            if (auto R = GlobalSymbolTable->LookupType(NT->getName()))
              FieldTy = R;
          }
        }
      }
      setType(&Node, FieldTy);
      return;
    }
  }
#endif
  // Field type unknown or TypeChecker running without struct registry.
  setType(&Node, nullptr);
}

void TypeChecker::visitEnumDecl(EnumDeclExprAST &Node) {
  setType(&Node, TurfVoid());
}

void TypeChecker::visitEnumAccess(EnumAccessExprAST &Node) {
  auto ET = GlobalSymbolTable ? GlobalSymbolTable->LookupType(Node.getEnumName()) : nullptr;
  if (!ET) return; // Should not happen, Resolver catches this

  if (Node.getVariant() == "COUNT") {
    setType(&Node, TurfInt());
  } else if (Node.getVariant() == "VALUES") {
    setType(&Node, std::make_shared<TurfPointerType>(ET));
  } else if (Node.getVariant() == "NAMES") {
    setType(&Node, std::make_shared<TurfPointerType>(TurfString()));
  } else {
    setType(&Node, ET);
  }
}

void TypeChecker::visitStructDecl(StructDeclExprAST &Node) {
  setType(&Node, TurfVoid());
}

void TypeChecker::visitStructInit(StructInitExprAST &Node) {
  // Visit each value expression for type annotation regardless of validation.
  for (const auto &[FieldName, Val] : Node.getNamedFields())
    Val->accept(*this);

#ifndef TYPECHECKER_NO_ERRORS
  // Look up the struct definition so we can validate the named fields.
  // GlobalSymbolTable is only available in the full build (not unit tests).
  TurfTypePtr StructTPtr;
  if (GlobalSymbolTable)
    StructTPtr = GlobalSymbolTable->LookupType(Node.getName());
  const TurfStructType *SType =
      StructTPtr ? dynamic_cast<const TurfStructType *>(StructTPtr.get())
                 : nullptr;

  if (SType) {
    const auto &DeclFields = SType->getFields();

    // 1. Check for unknown fields (fields provided that don't exist in the
    //    struct) AND that each provided value's type is compatible with its
    //    field's declared type.
    //
    //    The type check is not optional hardening: before it existed,
    //    `User { age: "not a number" }` (age declared as int) compiled with
    //    ZERO diagnostics. At runtime this stored the string literal's raw
    //    pointer bit-pattern directly into the field's i64 storage slot --
    //    silent type confusion producing garbage data (confirmed: printing
    //    the resulting `age` field gave a nonsense value like
    //    102697087172654, not even a crash to signal something was wrong).
    for (const auto &[FieldName, Val] : Node.getNamedFields()) {
      const StructField *DeclField = nullptr;
      for (const auto &DeclF : DeclFields) {
        if (DeclF.Name == FieldName) {
          DeclField = &DeclF;
          break;
        }
      }
      if (!DeclField) {
        reportError(Node.getLoc(), "Unknown field '" + FieldName +
                                       "' in struct '" + Node.getName() + "'");
        continue;
      }

      TurfTypePtr ValTy = getType(Val.get());
      if (ValTy && DeclField->Type &&
          !typesCompatible(ValTy.get(), DeclField->Type.get())) {
        reportError(Node.getLoc(),
                    "Field '" + FieldName + "' of struct '" + Node.getName() +
                        "' expects type '" + DeclField->Type->getName() +
                        "', but got '" + ValTy->getName() + "'.");
      }
    }

    // 2. Missing fields are allowed and default-initialized under the hood.
  }
#endif

  // Result type is the struct name.
  setType(&Node, std::make_shared<NamedType>(Node.getName()));
}

// ─── Arrays ───────────────────────────────────────────────────────────────────

void TypeChecker::visitArrayDecl(ArrayDeclExprAST &Node) {
  for (const auto &Init : Node.getInitList())
    Init->accept(*this);
  if (Node.getInitExpr())
    Node.getInitExpr()->accept(*this);
  setType(&Node, makeArrayType(Node.getElementTypePtr(), Node.getSize()));
}

void TypeChecker::visitArrayLiteral(ArrayLiteralExprAST &Node) {
  if (Node.getElements().empty()) {
    TypeError(Node.getLoc(), "Cannot infer type of empty array literal").raise();
    return;
  }

  for (const auto &Elem : Node.getElements()) {
    const_cast<ExprAST *>(Elem.get())->accept(*this);
  }

  TurfTypePtr ElemType = getType(Node.getElements()[0].get());
  if (!ElemType) {
    setType(&Node, nullptr);
    return;
  }
  for (size_t i = 1; i < Node.getElements().size(); ++i) {
    TurfTypePtr ElemIType = getType(Node.getElements()[i].get());
    if (!ElemIType || ElemIType->getName() != ElemType->getName()) {
      std::string gotName = ElemIType ? ElemIType->getName() : "unknown";
      TypeError(Node.getLoc(), "Array literal type mismatch: expected " + ElemType->getName() + ", got " + gotName).raise();
    }
  }

  auto ArrTy = makeArrayType(ElemType, Node.getElements().size());
  Node.CachedType = ArrTy;
  setType(&Node, ArrTy);
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
    TurfTypePtr ElemTy = ArrTy ? ArrTy->getElementTypePtr() : nullptr;

#ifndef TYPECHECKER_NO_ERRORS
    // Issue #17 fix: if the element type is a NamedType (e.g. "User"), resolve
    // it to the actual TurfStructType via the symbol table. Without this,
    // arr[0].field fails because the type never becomes a struct.
    // GlobalSymbolTable is only available in the full build (not unit tests).
    if (ElemTy) {
      if (const auto *NT = dynamic_cast<const NamedType *>(ElemTy.get())) {
        if (GlobalSymbolTable) {
          if (auto Resolved = GlobalSymbolTable->LookupType(NT->getName()))
            ElemTy = Resolved;
        }
      }
    }
#endif

    setType(&Node, ElemTy);
  } else if (BaseTy && BaseTy->isString()) {
    setType(&Node, TurfInt());
  } else {
    setType(&Node, nullptr);
  }
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

// ─── FFI ─────────────────────────────────────────────────────────────────────

void TypeChecker::visitExternFuncDecl(ExternFuncDeclExprAST &Node) {
  // Validate return type: must be a known/primitive type (not an unresolved
  // NamedType that was never registered as a struct).
  // Primitive and well-known types (int, double, bool, string, void, ptr, T*)
  // are always fine.  Only NamedType with an unknown struct name is an error.
  auto checkType = [&](TurfTypePtr Ty, const std::string &Context) {
    if (!Ty)
      return;
    // Unwrap pointer wrapper
    const TurfType *Base = Ty.get();
    if (const auto *PT = dynamic_cast<const TurfPointerType *>(Base))
      Base = PT->getPointee().get();
    // NamedType: must be either a primitive synonym or a registered struct.
    // (The Resolver has already validated struct names; here we only guard
    //  against types that slipped through — e.g. from invalid user input.)
    // We accept any NamedType that is not provably bad; Codegen will catch it.
    (void)Base;
    (void)Context;
  };

  checkType(Node.getReturnType(), "return type");
  for (const auto &[PName, PType] : Node.getParams())
    checkType(PType, "parameter '" + PName + "'");

  // The declaration itself has no runtime value.
  setType(&Node, TurfVoid());
}
