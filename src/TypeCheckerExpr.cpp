/**
 * @file TypeCheckerExpr.cpp
 * @brief Literal and operator-expression type checking: numbers, binary/unary/cast/assignment/compare.
 * @details Part of the TypeChecker pass (Issue #7), split from the original
 *          monolithic TypeChecker.cpp by category (mirrors AST.h's
 *          ASTExpr/ASTStmt/ASTDecl/ASTAccess split -- see
 *          docs/architecture/compiler_pipeline.md).
 *
 * @author a2ys
 * @copyright Copyright (c) 2026 TurfLang contributors.  Apache License.
 */

#include "TypeChecker.h"

// AST.h is needed for the full definition of every node type.
// It drags in LLVM headers, but TypeChecker itself does not use any LLVM API.
#include "AST.h"

#include "SymbolTable.h"

#include "Errors.h"
#include "SuggestionIndex.h"

#include "Builtins.h"

#include "TypeCheckerInternal.h"

#include <cassert>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

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

// The only expression kinds Codegen knows how to produce a store address
// for (see each class's codegenLValue() override in AST.h/ASTAccess.h --
// everything else inherits the base class's `return nullptr`). Checked here,
// at TypeCheck time, so an invalid target (`true = 5;`, `break = 5;`) gets
// one clear, specific diagnostic instead of either a nonsensical generic
// type-mismatch message (see InvalidAssignmentTargetError's doc comment) or
// silently falling through to a null Value* deep in Codegen.
bool isAssignableLValue(const ExprAST *E) {
  return dynamic_cast<const VariableExprAST *>(E) ||
         dynamic_cast<const ArrayAccessExprAST *>(E) ||
         dynamic_cast<const FieldAccessExprAST *>(E);
}

// A short, human-readable description of what non-lvalue expression kind
// was found on the LHS of '=', for InvalidAssignmentTargetError's message.
std::string describeNonLValue(const ExprAST *E) {
  if (dynamic_cast<const BoolExprAST *>(E)) return "a boolean literal";
  if (dynamic_cast<const NumberExprAST *>(E)) return "a numeric literal";
  if (dynamic_cast<const StringExprAST *>(E)) return "a string literal";
  if (dynamic_cast<const CharExprAST *>(E)) return "a character literal";
  if (dynamic_cast<const NullExprAST *>(E)) return "'null'";
  if (dynamic_cast<const BreakExprAST *>(E)) return "the 'break' keyword";
  if (dynamic_cast<const ContinueExprAST *>(E)) return "the 'continue' keyword";
  if (dynamic_cast<const FuncCallExprAST *>(E) ||
      dynamic_cast<const MethodCallExprAST *>(E))
    return "a function call's result";
  return "this expression";
}

} // namespace


// ─── Literal visitors ─────────────────────────────────────────────────────────

void TypeChecker::visitNumber(NumberExprAST &Node) {
  setType(&Node, Node.isInteger() ? TurfInt() : TurfDouble());
}


void TypeChecker::visitBool(BoolExprAST &Node) {
  setType(&Node, TurfBool());
}


void TypeChecker::visitChar(CharExprAST &Node) {
  setType(&Node, TurfChar());
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

  // 2a. An `export immut` constant (no ResolvedSymbol -- see its field's
  //     doc comment in AST.h; the Resolver sets ResolvedConstant instead).
  if (Node.ResolvedConstant && Node.ResolvedConstant->Type) {
    setType(&Node, Node.ResolvedConstant->Type);
    return;
  }

  // 2b. A first-class function reference -- a bare name resolved by the
  //     Resolver to a real `fn` used as a VALUE (see ResolvedFunctionRef's
  //     doc comment in AST.h). Its type is the FuncType matching that
  //     function's own signature.
  if (Node.ResolvedFunctionRef) {
    setType(&Node, std::make_shared<FuncType>(Node.ResolvedFunctionRef->ReturnType,
                                              Node.ResolvedFunctionRef->paramTypes()));
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


void TypeChecker::visitBinary(BinaryExprAST &Node) {
  // Visit children first (bottom-up)
  if (Node.getLHS())
    const_cast<ExprAST *>(Node.getLHS())->accept(*this);
  if (Node.getRHS())
    const_cast<ExprAST *>(Node.getRHS())->accept(*this);

  const TurfType *LhsTy = getType(Node.getLHS()).get();
  const TurfType *RhsTy = getType(Node.getRHS()).get();
  int Op = Node.getOp();

  // ── Operator overload resolution ─────────────────────────────────────────
  // Checked BEFORE the builtin bitwise/relational/exponent/arithmetic
  // dispatch below, for every whitelisted operator token. Safe to check
  // first rather than "builtin fast path first" (RFC §4.2's stated order):
  // Resolver::validateOperatorDecl() already hard-rejects any `operator`
  // declaration where BOTH operand types are builtin, so a user overload can
  // never shadow a builtin match here -- there is no ordering ambiguity to
  // resolve. Exact-type-match only (sameTypeExact, no numeric coercion).
  if (isOverloadableBinaryOp(Op) && !Node.ResolvedOperatorCandidates.empty() &&
      LhsTy && RhsTy) {
    for (const auto &C : Node.ResolvedOperatorCandidates) {
      if (!C || C->arity() != 2)
        continue;
      if (sameTypeExact(C->Params[0].Type.get(), LhsTy) &&
          sameTypeExact(C->Params[1].Type.get(), RhsTy)) {
        if (Node.ResolvedOperatorTarget) {
          // Two visible candidates with an identical operand-type signature
          // -- RFC §4.3's universal conflict rule, enforced here at the call
          // site (declaration-site detection needs RFC step 5's
          // TurfModule::registerFunction origin-tracking fix, out of scope
          // for this pass).
          reportError(Node.getLoc(),
                      "Ambiguous 'operator " + OpToDisplayString(Op) +
                          "(" + LhsTy->getName() + ", " + RhsTy->getName() +
                          ")' call: more than one visible overload matches "
                          "at this call site.");
          break;
        }
        Node.ResolvedOperatorTarget = C;
      }
    }
    if (Node.ResolvedOperatorTarget) {
      setType(&Node, Node.ResolvedOperatorTarget->ReturnType);
      return;
    }
  }

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

  if (Op == '-' || Op == '+') {
    // Negation / unary plus: operand must be numeric. Unary '+' is a no-op
    // on every numeric type (added purely for symmetry with '-' -- e.g.
    // `+5` next to `-5`); still type-checked the same way so `+"str"` is
    // rejected rather than silently passing the string through.
    if (OperandTy && !OperandTy->isNumeric()) {
      reportError(Node.getLoc(),
                  "Unary '" + std::string(1, static_cast<char>(Op)) +
                      "' requires a numeric operand, got '" +
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
      reportError(Node.getLoc(),
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
  // &expr produces a TurfReferenceType wrapping the operand's type (issue
  // #238 -- a breaking retype: this used to produce a raw TurfPointerType).
  // Codegen is unaffected -- AddressOfExprAST::codegen() just returns the
  // operand's lvalue address either way, and &T's LLVM representation is
  // identical to T*'s (see TurfReferenceType's own doc comment, Types.h).
  // Always sound: the operand went through codegenLValue() (an existing
  // named variable/field/array element's real storage), so the resulting
  // address is never null.
  setType(&Node, std::make_shared<TurfReferenceType>(OperandTy));
}


void TypeChecker::visitAssignment(AssignmentExprAST &Node) {
  if (Node.getLHS())
    const_cast<ExprAST *>(Node.getLHS())->accept(*this);
  if (Node.getRHS())
    const_cast<ExprAST *>(Node.getRHS())->accept(*this);

  if (Node.getLHS() && !isAssignableLValue(Node.getLHS())) {
#ifndef TYPECHECKER_NO_ERRORS
    InvalidAssignmentTargetError(Node.getLoc(),
                                 describeNonLValue(Node.getLHS()))
        .raise();
#else
    throw std::runtime_error("InvalidAssignmentTargetError");
#endif
    return;
  }

  TurfTypePtr LhsTy = getType(Node.getLHS());
  TurfTypePtr RhsTy = getType(Node.getRHS());
  const VariableExprAST *LHSVar = findRootVariable(Node.getLHS());
  // findRootVariable() unwraps FieldAccessExprAST/ArrayAccessExprAST down to
  // the base variable (e.g. 'u' in 'u.age = ...'), which is exactly what's
  // needed to find the ResolvedSymbol for the immutability check below --
  // but that base variable's OWN declaration location is only meaningful as
  // a "declared here" pointer for the "declared type" mismatch when the
  // LHS actually IS that variable directly. For 'u.age = "x"', 'u's
  // declaration line shows User's type, not 'age's declared type -- pointing
  // "declared here" at it implied the wrong thing entirely. StructField
  // carries no location of its own to point at instead (would need a new
  // field threaded through struct parsing), so the safest fix is to simply
  // not attach a location note when the LHS isn't the bare variable itself.
  const bool LHSIsBareVariable =
      dynamic_cast<const VariableExprAST *>(Node.getLHS()) != nullptr;

  if (LhsTy && RhsTy) {
    if (RhsTy->isNull()) {
      if (!LhsTy->isPointer() && !LhsTy->isNamed("ptr") && !LhsTy->isNamed("void*")) {
        std::string VarName = LHSVar ? LHSVar->getName() : "";
#ifndef TYPECHECKER_NO_ERRORS
        {
          NullAssignmentError Err(Node.getLoc(), VarName, LhsTy->getName());
          if (LHSIsBareVariable && LHSVar && LHSVar->ResolvedSymbol)
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
      if (LHSIsBareVariable && LHSVar && LHSVar->ResolvedSymbol) {
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
    } else if (LHSVar->ResolvedConstant) {
      // `export immut` constants are always immutable (export mut isn't
      // supported yet -- see ExportMutNotSupportedError) and have no
      // ResolvedSymbol to hang the check above off of.
#ifndef TYPECHECKER_NO_ERRORS
      ImmutableAssignmentError(Node.getLoc(), LHSVar->getName())
          .withRelated(toSourceLocation(LHSVar->ResolvedConstant->DeclLoc),
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

  if (Node.getLHS() && !isAssignableLValue(Node.getLHS())) {
#ifndef TYPECHECKER_NO_ERRORS
    InvalidAssignmentTargetError(Node.getLoc(),
                                 describeNonLValue(Node.getLHS()))
        .raise();
#else
    throw std::runtime_error("InvalidAssignmentTargetError");
#endif
    return;
  }

  TurfTypePtr LhsTy = getType(Node.getLHS());
  TurfTypePtr RhsTy = getType(Node.getRHS());
  TurfTypePtr ResultTy;
  int Op = Node.getOpcode();

  // Bitwise compound-assign (&=, |=, ^=, <<=, >>=) -- same "int operands
  // only" rule as the plain binary form (visitBinary()'s isBitwise branch).
  // Skipping this and falling into the generic inferArithmeticType() call
  // below would silently accept e.g. `d &= 3.0;` for a double `d`: that
  // function only ever checks "both numeric" and bool/double promote to
  // int/double like ordinary arithmetic, so it would never reject the
  // double operand the way bitwise ops must.
  auto isBitwiseOp = [](int op) -> bool {
    return op == '&' || op == '|' || op == '^' ||
           op == TOK_LSHIFT || op == TOK_RSHIFT;
  };
  if (isBitwiseOp(Op)) {
    if (LhsTy && !LhsTy->isNamed("int")) {
      reportError(Node.getLoc(),
                  "Bitwise compound assignment requires an integer operand, "
                  "but the left operand is '" + LhsTy->getName() + "'.");
    }
    if (RhsTy && !RhsTy->isNamed("int")) {
      reportError(Node.getLoc(),
                  "Bitwise compound assignment requires an integer operand, "
                  "but the right operand is '" + RhsTy->getName() + "'.");
    }
    // LhsTy is set on Node below (unconditionally, same as every other
    // branch) -- nothing further to do here.
  } else if (Op == TOK_POWER) {
    // `**=` -- same numeric-operand rule as the plain `**` binary form.
    // Deliberately does NOT run the generic typesCompatible(ResultTy, LhsTy)
    // check below: `**`'s result is always double (see visitBinary()'s
    // TOK_POWER branch and Codegen's CompoundAssignmentExprAST::codegen(),
    // which lowers this through the same llvm.pow-on-doubles path and casts
    // the result back down to LhsTy for the store), so an int LHS is
    // perfectly valid here (`x **= 2;` narrows the pow result back to int)
    // even though ResultTy itself is double -- the generic check would
    // reject that as "cannot assign double to int", which is wrong for this
    // one operator specifically.
    if (LhsTy && !LhsTy->isNumeric()) {
      reportError(Node.getLoc(),
                  "Exponentiation compound assignment requires a numeric "
                  "operand, but the left operand is '" + LhsTy->getName() + "'.");
    }
    if (RhsTy && !RhsTy->isNumeric()) {
      reportError(Node.getLoc(),
                  "Exponentiation compound assignment requires a numeric "
                  "operand, but the right operand is '" + RhsTy->getName() + "'.");
    }
    // LhsTy is set on Node below (unconditionally, same as every other
    // branch) -- nothing further to do here.
  } else if (LhsTy && RhsTy) {
    if (inferArithmeticType(Node.getLoc(), Op, LhsTy.get(), RhsTy.get(), ResultTy)) {
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
    } else if (LHSVar->ResolvedConstant) {
#ifndef TYPECHECKER_NO_ERRORS
      ImmutableAssignmentError(Node.getLoc(), LHSVar->getName())
          .withRelated(toSourceLocation(LHSVar->ResolvedConstant->DeclLoc),
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
    } else if (LHSVar->ResolvedConstant) {
#ifndef TYPECHECKER_NO_ERRORS
      ImmutableAssignmentError(Node.getLoc(), LHSVar->getName())
          .withRelated(toSourceLocation(LHSVar->ResolvedConstant->DeclLoc),
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

  // A literal integer cast to an enum type was never checked against that
  // enum's actual variant range -- `99 to Color` (Color only has 3
  // variants) compiled and ran fine, producing a Color value with no
  // corresponding variant name at all (`c to int` came back 99). This is
  // deliberately narrow: only a compile-time-constant literal operand is
  // checked, since that's the only case where "is this value valid" is
  // even decidable here without runtime bounds checking (a genuinely
  // variable-valued cast, e.g. `x to Color` where x is computed, can't be
  // validated at compile time and is left alone, same as before).
  // ParseType() always parses a bare identifier as NamedType, never
  // resolving it to the real registered TurfEnumType (that resolution
  // normally happens via GlobalSymbolTable elsewhere in the pipeline) --
  // and Resolver::visitCast() doesn't touch DestType at all, so
  // Node.getDestTypePtr() for `... to Color` is still NamedType("Color")
  // by the time this runs, not TurfEnumType. Without resolving it here
  // first, the dynamic_cast below could never match, silently defeating
  // this whole check.
  TurfTypePtr DestTy = Node.getDestTypePtr();
  if (auto *NT = dynamic_cast<const NamedType *>(DestTy.get())) {
    if (GlobalSymbolTable) {
      if (auto Resolved = GlobalSymbolTable->LookupType(NT->getName()))
        DestTy = Resolved;
    }
  }
  if (auto *ET = dynamic_cast<const TurfEnumType *>(DestTy.get())) {
    if (auto *Lit = dynamic_cast<const NumberExprAST *>(Node.getOperand())) {
      if (Lit->isInteger()) {
        long long V = Lit->getIntVal();
        if (V < 0 || V >= ET->numVariants()) {
          reportError(Node.getLoc(),
                      "Value " + std::to_string(V) + " is out of range for enum '" +
                          ET->getName() + "' (valid range: 0 to " +
                          std::to_string(ET->numVariants() - 1) + ").",
                      {},
                      "Use one of '" + ET->getName() +
                          "'s declared variants instead of an arbitrary "
                          "integer literal.",
                      "Value out of range!");
        }
      }
    }
  }

  // `unsafe { }` gate (issue #238): a cast whose SOURCE or DESTINATION is a
  // raw ptr/T* type is one of the three explicitly-gated "uses"
  // (declaration, cast, dereference -- see FINAL_SPRINT.md's Track A) --
  // this is also the mechanism that lets a caller explicitly convert a raw
  // pointer into a `&T` (`somePtr to &T`) or vice versa, always inside
  // `unsafe { }`. Checked against the ORIGINAL (possibly-NamedType)
  // DestTypePtr, not the enum-resolved DestTy local above -- a struct/enum
  // cast target is never pointer-shaped either way, so this doesn't matter
  // for correctness, only for using the accurate as-written type.
  TurfTypePtr OperandTy = Node.getOperand() ? getType(Node.getOperand()) : nullptr;
  if (!checkUnsafeGate(Node.getDestTypePtr().get(), Node.getLoc(), &Node))
    checkUnsafeGate(OperandTy.get(), Node.getLoc(), &Node);

  // Explicit cast always succeeds at the TypeChecker level; result is DestType.
  setType(&Node, Node.getDestTypePtr());
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
      if (auto *EA = dynamic_cast<ScopeAccessExprAST *>(PatExpr.get())) {
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
                    "this arm can never run.",
                    {},
                    "Remove the duplicate arm, or combine it with the "
                    "earlier one by stacking (`matches A:` then "
                    "`matches B:` sharing one body).",
                    "Duplicate pattern!");
      }
    }

    // Narrow a TurfMachineType-typed, plain-variable subject to one specific
    // state for the duration of this arm's body -- see MachineNarrowStack_'s
    // doc comment (include/TypeChecker.h). Only a single-pattern arm on a
    // bare variable subject qualifies; a stacked-pattern arm or a non-
    // variable subject (e.g. a field access) is a v1 scope limit and stays
    // unnarrowed, so any `.transition()` inside it falls back to Codegen's
    // runtime guard instead of being compile-time-proven.
    bool PushedNarrow = false;
    if (Arm.Values.size() == 1) {
      if (const auto *MT = dynamic_cast<const TurfMachineType *>(SubjectTy.get())) {
        if (const auto *SubjVar =
                dynamic_cast<const VariableExprAST *>(Node.getSubject())) {
          if (const auto *EA =
                  dynamic_cast<const ScopeAccessExprAST *>(Arm.Values[0].get())) {
            MachineNarrowStack_.push_back(
                {SubjVar->getName(), MT->getName(), EA->getVariant()});
            PushedNarrow = true;
          }
        }
      }
    }

    // Type-check arm body
    check(Arm.Body.get());

    if (PushedNarrow)
      MachineNarrowStack_.pop_back();
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
              if (auto* EA = dynamic_cast<ScopeAccessExprAST*>(PatExpr.get()))
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
          reportError(Node.getLoc(), Msg, {},
                      "Add 'otherwise:' to handle remaining variants.",
                      "Non-exhaustive match!");
      }
  }

  // 5. Empty compare{} warning was already emitted by the Parser
  setType(&Node, TurfVoid());
}
