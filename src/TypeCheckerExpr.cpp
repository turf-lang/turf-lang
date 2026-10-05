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
#include "TypeWalk.h" // SMP-002: shared containsOpenParam() walker

#include "llvm/ADT/SmallString.h" // `compare` pattern-key exact-value fingerprinting

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
    } else if (auto *Sl = dynamic_cast<const SliceExprAST *>(Base)) {
      // `nums[1..][0] = v` writes nums itself: a slice is a view.
      Base = Sl->getBase();
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

// LANG-001: true for an ArrayAccessExprAST base type that can ONLY ever be
// indexed through `operator[]`/`operator[]=` -- i.e. everything except the
// three builtin-indexable kinds (real array, pointer, string). Deliberately
// NOT `T->isStruct() || T->isEnum()`: a monomorphized generic instantiation
// like `List<int>`/`Set<int>`/`Map<string, int>` is a GenericType (Types.h),
// a SIBLING of TurfStructType, not a subclass of it -- isStruct() is false
// for one, so that narrower check silently let a struct/enum-shaped
// diagnostic here miss every stdlib collection type entirely (confirmed
// live: fail_stdlib_set_no_index_set.tr crashed with Codegen's "Internal
// error: Base is not an array or pointer" ICE instead of raising the clean
// error this function exists to produce, until this widened to "not a
// builtin-indexable kind" instead of "is one of these two specific kinds").
bool needsOperatorIndex(const TurfType *T) {
  return T && !T->isArray() && !T->isPointer() && !T->isString();
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
  // SEM-002 (FREEZE.md §6.2 row 8): a float literal whose decimal->binary
  // rounding overflowed to +/-inf is a hard error UNCONDITIONALLY --
  // unlike the int-literal-range checks (checkLiteralRepresentable()),
  // this doesn't depend on what destination the literal is acquiring, so
  // it belongs here rather than at each acquires-a-type call site.
  if (!Node.isInteger() && Node.overflowed()) {
#ifndef TYPECHECKER_NO_ERRORS
    FloatLiteralOverflowError(Node.getLoc(), Node.getSpelling()).raise();
#else
    throw std::runtime_error("FloatLiteralOverflowError");
#endif
  }
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

  // ── Unary `operator!` resolution (LANG-001) ──────────────────────────────
  // Checked BEFORE the ordinary `operator!=`/builtin '!=' handling below --
  // if this node is a user-written `!x` (IsDesugaredNot) and the operand's
  // type declares a unary `operator!`, that ALWAYS takes it: `!x` means
  // "apply unary not", never "compare x to the literal true" once a type
  // opts into its own unary-not meaning. See ResolvedNotOperatorCandidates's
  // doc comment (ASTExpr.h) for why this is layered onto the existing
  // desugared shape instead of a real UnaryExprAST('!', ...) node.
  if (Node.IsDesugaredNot && !Node.ResolvedNotOperatorCandidates.empty() && LhsTy) {
    TurfTypePtr NotResultTy;
    for (const auto &C : Node.ResolvedNotOperatorCandidates) {
      TurfTypePtr R;
      if (matchOperatorOverload(C, {getType(Node.getLHS())}, /*RelaxSelf=*/false,
                                &Node, Node.getLoc(), R)) {
        if (Node.ResolvedNotOperatorTarget) {
          reportError(Node.getLoc(), ErrCode::AmbiguousCall,
                      "Ambiguous 'operator!(" + LhsTy->getName() +
                          ")' call: more than one visible overload matches "
                          "at this call site.");
          break;
        }
        Node.ResolvedNotOperatorTarget = C;
        NotResultTy = R;
      }
    }
    if (Node.ResolvedNotOperatorTarget) {
      setType(&Node, NotResultTy);
      return;
    }
  }

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
    TurfTypePtr OpResultTy;
    for (const auto &C : Node.ResolvedOperatorCandidates) {
      TurfTypePtr R;
      if (matchOperatorOverload(C, {getType(Node.getLHS()), getType(Node.getRHS())},
                                /*RelaxSelf=*/false, &Node, Node.getLoc(), R)) {
        if (Node.ResolvedOperatorTarget) {
          // Two visible candidates with an identical operand-type signature
          // -- RFC §4.3's universal conflict rule, enforced here at the call
          // site (declaration-site detection needs RFC step 5's
          // TurfModule::registerFunction origin-tracking fix, out of scope
          // for this pass).
          reportError(Node.getLoc(), ErrCode::AmbiguousCall,
                      "Ambiguous 'operator " + OpToDisplayString(Op) +
                          "(" + LhsTy->getName() + ", " + RhsTy->getName() +
                          ")' call: more than one visible overload matches "
                          "at this call site.");
          break;
        }
        Node.ResolvedOperatorTarget = C;
        OpResultTy = R;
      }
    }
    if (Node.ResolvedOperatorTarget) {
      setType(&Node, OpResultTy);
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
      reportError(Node.getLoc(), ErrCode::ArithmeticOperand,
                  "Bitwise operator requires integer operands, but left operand is '" +
                      LhsTy->getName() + "'.");
    }
    if (!rhsOk) {
      reportError(Node.getLoc(), ErrCode::ArithmeticOperand,
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
      reportError(Node.getLoc(), ErrCode::ArithmeticOperand,
                  "Exponentiation operator '**' requires numeric operands, but left operand is '" +
                      LhsTy->getName() + "'.");
      setType(&Node, nullptr);
      return;
    }
    if (RhsTy && !RhsTy->isNumeric()) {
      reportError(Node.getLoc(), ErrCode::ArithmeticOperand,
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

  // ── Operator overload resolution ─────────────────────────────────────────
  // Mirrors visitBinary()'s identical block above -- checked BEFORE the
  // builtin numeric/bool/bitwise dispatch below, for the same reason:
  // Resolver::validateOperatorDecl() already hard-rejects an all-builtin
  // operand, so a user overload can never shadow a builtin match here.
  // Exact-type-match only (F-OP-1, sameTypeExact, no numeric coercion).
  if (isOverloadableUnaryOp(Op) && !Node.ResolvedOperatorCandidates.empty() &&
      OperandTy) {
    TurfTypePtr OpResultTy;
    for (const auto &C : Node.ResolvedOperatorCandidates) {
      TurfTypePtr R;
      if (matchOperatorOverload(C, {OperandTy}, /*RelaxSelf=*/false, &Node,
                                Node.getLoc(), R)) {
        if (Node.ResolvedOperatorTarget) {
          reportError(Node.getLoc(), ErrCode::AmbiguousCall,
                      "Ambiguous 'operator " + OpToDisplayString(Op) + "(" +
                          OperandTy->getName() +
                          ")' call: more than one visible overload matches "
                          "at this call site.");
          break;
        }
        Node.ResolvedOperatorTarget = C;
        OpResultTy = R;
      }
    }
    if (Node.ResolvedOperatorTarget) {
      setType(&Node, OpResultTy);
      return;
    }
  }

  if (Op == '-' || Op == '+') {
    // Negation / unary plus: operand must be numeric. Unary '+' is a no-op
    // on every numeric type (added purely for symmetry with '-' -- e.g.
    // `+5` next to `-5`); still type-checked the same way so `+"str"` is
    // rejected rather than silently passing the string through.
    if (OperandTy && !OperandTy->isNumeric()) {
      reportError(Node.getLoc(), ErrCode::ArithmeticOperand,
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
      reportError(Node.getLoc(), ErrCode::ArithmeticOperand,
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


const ArrayAccessExprAST *
TypeChecker::operatorIndexTemporaryRoot(const ExprAST *Place) {
  // Is the value at E reached through a pointer/reference? Then whatever is
  // written below E lands in the pointee, not in a copy.
  auto IsPointerValued = [&](const ExprAST *E) {
    TurfTypePtr T = getType(E);
    return T && T->isPointer();
  };

  const ExprAST *E = Place;
  while (E) {
    if (const auto *AA = dynamic_cast<const ArrayAccessExprAST *>(E)) {
      // An `operator[]` READ is a call that returns the element by value.
      if (AA->ResolvedIndexGetTarget)
        return IsPointerValued(AA) ? nullptr : AA;
      // A builtin index: addressable storage, unless it indexes a pointer
      // (then the element is the pointee's), in which case there is nothing
      // to find. Otherwise the array lives inside whatever its base is.
      if (IsPointerValued(AA->getBase()))
        return nullptr;
      E = AA->getBase();
    } else if (const auto *FA = dynamic_cast<const FieldAccessExprAST *>(E)) {
      // `p.x` with `p` a pointer/reference writes through `p`.
      if (IsPointerValued(FA->getBase()))
        return nullptr;
      E = FA->getBase();
    } else {
      return nullptr; // a variable, a call result, ...: not this check's job
    }
  }
  return nullptr;
}


bool TypeChecker::rejectOperatorTemporaryWrite(const ExprAST *Place,
                                               SourceLocation Loc,
                                               const std::string &What,
                                               bool PlaceIsStoreTarget) {
  // A RECEIVER or container that is itself pointer-valued is written through;
  // a store TARGET is the slot being assigned, so its own type says nothing.
  if (!PlaceIsStoreTarget) {
    TurfTypePtr T = getType(Place);
    if (T && T->isPointer())
      return false;
  }
  if (!operatorIndexTemporaryRoot(Place))
    return false;
#ifndef TYPECHECKER_NO_ERRORS
  OperatorTemporaryWriteError(Loc, What).raise();
#else
  throw std::runtime_error("OperatorTemporaryWriteError");
#endif
  return true;
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

  // LEC-006: `bs[0].a = 50` writes into the copy `bs[0]` returned. A target
  // that is itself an `operator[]` access is the `[]=` path below, not this.
  {
    const auto *LAA = dynamic_cast<const ArrayAccessExprAST *>(Node.getLHS());
    TurfTypePtr LBase = LAA ? getType(LAA->getBase()) : nullptr;
    const bool IsOperatorIndex = LAA && LBase && needsOperatorIndex(LBase.get());
    if (!IsOperatorIndex)
      rejectOperatorTemporaryWrite(
          Node.getLHS(), Node.getLoc(),
          "Cannot assign to part of the value an 'operator[]' read returned: "
          "it is a copy, so the write would be lost.",
          /*PlaceIsStoreTarget=*/true);
  }

  TurfTypePtr LhsTy = getType(Node.getLHS());
  TurfTypePtr RhsTy = getType(Node.getRHS());

  // E144: rebinding a `mut` slice variable (`view = other[1..];`) to an
  // immutable array would let later writes through `view` change it.
  if (const auto *LV = dynamic_cast<const VariableExprAST *>(Node.getLHS())) {
    const auto *LArr = dynamic_cast<const ArrayType *>(LhsTy.get());
    if (LArr && LArr->getSize() < 0 && LV->getResolvedSymbol() &&
        !LV->getResolvedSymbol()->IsImmutable &&
        checkMutableViewSource(Node.getRHS(), Node.getLoc(), LV->getName())) {
      setType(&Node, nullptr);
      return;
    }
  }

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

  // ── operator[]= overload resolution (LANG-001) ───────────────────────────
  // If the LHS is an `arr[i]` shape whose base is a struct (LhsTy is null
  // here for a write-only container -- see visitArrayAccess(), which only
  // ever gives ArrayAccessExprAST a real type via a matching `operator[]`
  // READ candidate or a builtin array/pointer/string), look for an exact
  // match against `operator[]=`'s own (self, index, value) signature using
  // the base/index's REAL types (not LhsTy, which doesn't describe this at
  // all) plus the RHS's type as the value operand. A genuine array/pointer/
  // string LHS never has any ResolvedIndexSetCandidates to begin with (no
  // `operator[]=` can ever be declared with a builtin self type -- see
  // validateOperatorDecl()), so this is a pure no-op for every existing
  // (non-operator) assignment shape.
  bool HandledViaOperatorIndexSet = false;
  if (const auto *ArrAcc = dynamic_cast<const ArrayAccessExprAST *>(Node.getLHS())) {
    auto *MutableArrAcc = const_cast<ArrayAccessExprAST *>(ArrAcc);
    TurfTypePtr ArrBaseTy = getType(ArrAcc->getBase());
    TurfTypePtr ArrIndexTy = getType(ArrAcc->getIndex());
    if (!ArrAcc->ResolvedIndexSetCandidates.empty() && ArrBaseTy && ArrIndexTy && RhsTy) {
      for (const auto &C : ArrAcc->ResolvedIndexSetCandidates) {
        TurfTypePtr Unused;
        // The site is the ArrayAccessExprAST, not the assignment: it is the
        // node Codegen reads ResolvedIndexSetTarget from.
        if (matchOperatorOverload(C, {ArrBaseTy, ArrIndexTy, RhsTy},
                                  /*RelaxSelf=*/true, MutableArrAcc,
                                  MutableArrAcc->getLoc(), Unused)) {
          if (MutableArrAcc->ResolvedIndexSetTarget) {
            reportError(Node.getLoc(), ErrCode::AmbiguousCall,
                        "Ambiguous 'operator[]=(" + ArrBaseTy->getName() +
                            ", " + ArrIndexTy->getName() + ", " +
                            RhsTy->getName() +
                            ")' call: more than one visible overload "
                            "matches at this call site.");
            break;
          }
          MutableArrAcc->ResolvedIndexSetTarget = C;
        }
      }
      HandledViaOperatorIndexSet = MutableArrAcc->ResolvedIndexSetTarget != nullptr;
      // LEC-006: a `[]=` that takes its container by reference exists to
      // MUTATE it (Map: put() changes len/cap). If the container is itself
      // the by-value result of another `[]`, that mutation hits a copy.
      // A by-value self (List) writes through its shared buffer and is fine.
      if (HandledViaOperatorIndexSet &&
          !MutableArrAcc->ResolvedIndexSetTarget->Params.empty() &&
          MutableArrAcc->ResolvedIndexSetTarget->Params[0].Type &&
          MutableArrAcc->ResolvedIndexSetTarget->Params[0].Type->isReference())
        rejectOperatorTemporaryWrite(
            ArrAcc->getBase(), Node.getLoc(),
            "Cannot assign through 'operator[]=' on the value another "
            "'operator[]' read returned: the container is a copy, so the "
            "write would be lost.",
            /*PlaceIsStoreTarget=*/false);
    }
    // A struct/enum base with no matching `operator[]=` (either none
    // declared, or none with this exact (base, index, value) signature) is
    // a clean type error here, not the "Internal error: Base is not an
    // array or pointer" ICE Codegen's codegenLValue() would otherwise hit --
    // same "clean error, not an ICE" bar fail_operator_not_visible.tr holds
    // ordinary binary operators to (tests/18_operator_overload/).
    if (!HandledViaOperatorIndexSet && ArrBaseTy &&
        needsOperatorIndex(ArrBaseTy.get())) {
      reportError(Node.getLoc(), ErrCode::ArgumentType,
                  "No 'operator[]=' overload matches '" + ArrBaseTy->getName() +
                      "[" + (ArrIndexTy ? ArrIndexTy->getName() : "?") +
                      "] = " + (RhsTy ? RhsTy->getName() : "?") + "'.");
    }
  }

  if (LhsTy && RhsTy && !HandledViaOperatorIndexSet) {
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
    // OVH-005: an untyped literal (`n = 5;` for an 'int32' n) acquires
    // 'int32' directly -- see untypedLiteralAcquiresInt32()'s doc comment.
    if (!typesCompatible(RhsTy.get(), LhsTy.get()) &&
        !untypedLiteralAcquiresInt32(Node.getRHS(), LhsTy.get())) {
      std::vector<RelatedLocation> Related;
      if (LHSIsBareVariable && LHSVar && LHSVar->ResolvedSymbol) {
        // SEM-003 (§11.6's worked example): a `guess`-inferred variable's
        // declaration doesn't show its type in the source the way an
        // explicit annotation does, so the generic "declared here" note
        // would leave a reader to go re-read the initializer themselves --
        // name the inferred type directly instead.
        std::string Note = LHSVar->ResolvedSymbol->WasGuessInferred
            ? "'" + LHSVar->getName() + "' was inferred as '" +
                  LhsTy->getName() + "' here"
            : "declared here";
        Related.push_back({toSourceLocation(LHSVar->ResolvedSymbol->DeclLoc),
                           Note});
      }
      // SLM-001 §16.3 item 3: surface a `guess`-inferred operand's resolved
      // type as a structured fact for the SLM prompt. Deliberately narrower
      // than "every operand's type" -- an EXPLICITLY annotated declaration's
      // type is already spelled out on its own source line (which the
      // pinned-context mechanism above already shows), so restating it here
      // would just be the same fact twice. A `guess`-inferred local is
      // different: the concrete type WasGuessInferred names is never written
      // anywhere in the source at all, so it is otherwise invisible to the
      // model no matter how much surrounding context it is shown.
      SLM::DiagnosticHints Hints;
      const auto *RHSVar = dynamic_cast<const VariableExprAST *>(Node.getRHS());
      if (LHSIsBareVariable && LHSVar && LHSVar->ResolvedSymbol &&
          LHSVar->ResolvedSymbol->WasGuessInferred)
        Hints.KnownTypes.push_back(
            "'" + LHSVar->getName() + "' (assignment target) was declared "
            "with 'guess' and inferred as '" + LhsTy->getName() + "'.");
      if (RHSVar && RHSVar->ResolvedSymbol &&
          RHSVar->ResolvedSymbol->WasGuessInferred)
        Hints.KnownTypes.push_back(
            "'" + RHSVar->getName() + "' (assigned value) was declared "
            "with 'guess' and inferred as '" + RhsTy->getName() + "'.");
      reportError(Node.getLoc(),
                  codeForMismatch(RhsTy.get(), nullptr, ErrCode::TypeMismatch),
                  "Cannot assign value of type '" + RhsTy->getName() +
                      "' to variable of type '" + LhsTy->getName() + "'.",
                  Related, hintForMismatch(RhsTy.get(), nullptr, ""), "", Hints);
    } else {
      // SEM-002 -- see visitVarDecl()'s identical comment (TypeCheckerDecl.cpp).
      checkLiteralRepresentable(Node.getRHS(), LhsTy.get(), Node.getLoc());
    }
  }

  // TYPE-002 (S14a): `h.items = List::default();` -- an ASSIGNMENT, not a
  // VarDeclExprAST initializer -- needs the identical ConstructedType
  // hand-off TypeChecker::visitVarDecl() already does (TypeCheckerDecl.cpp),
  // for the same reason: `List::default()`'s own static type is always the
  // bare NamedType("List") (U-5), so FuncCallExprAST::codegen()'s
  // construction branch has no other way to know it's building `List<int>`
  // specifically. Found live: this exact assignment shape (S12b/S13a's own
  // `Holder.items = List::default();` test pattern) crashed with "Cannot
  // allocate unsized type %List" once the shared template struct's own
  // codegen was skipped (S14a, this session) -- the construction fell back
  // to the now-permanently-opaque bare "List" struct instead of the real,
  // per-instantiation one.
  if (LhsTy) {
    if (dynamic_cast<const GenericType *>(LhsTy.get())) {
      if (auto *FC = dynamic_cast<FuncCallExprAST *>(
              const_cast<ExprAST *>(Node.getRHS()))) {
        if (auto *SC = dynamic_cast<const ScopeAccessExprAST *>(FC->getCallee())) {
          if (SC->ResolvedKind == ScopeAccessKind::DefaultConstruct ||
              SC->ResolvedKind == ScopeAccessKind::ConstructorCall)
            FC->ConstructedType = LhsTy;
        }
      }
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

  // LEC-006: `bs[0].n += 1` updates the copy `bs[0]` returned. (A target that
  // is itself an `operator[]` access is rejected by its own check below.)
  {
    const auto *LAA = dynamic_cast<const ArrayAccessExprAST *>(Node.getLHS());
    TurfTypePtr LBase = LAA ? getType(LAA->getBase()) : nullptr;
    if (!(LAA && LBase && needsOperatorIndex(LBase.get())))
      rejectOperatorTemporaryWrite(
          Node.getLHS(), Node.getLoc(),
          "Cannot update part of the value an 'operator[]' read returned: "
          "it is a copy, so the update would be lost.",
          /*PlaceIsStoreTarget=*/true);
  }

  TurfTypePtr LhsTy = getType(Node.getLHS());
  TurfTypePtr RhsTy = getType(Node.getRHS());
  TurfTypePtr ResultTy;
  int Op = Node.getOpcode();

  // ── Operator overload resolution (F-OP-2's desugaring) ───────────────────
  // `a op= b` = `a = a op b` -- checked BEFORE the bitwise/`**`/arithmetic
  // builtin dispatch below for the identical reason visitBinary() checks
  // first: validateOperatorDecl() already hard-rejects an all-builtin
  // operand pair, so a user overload can never shadow a builtin match.
  // Exact-type-match only (F-OP-1). The LHS location was evaluated exactly
  // ONCE, above (`Node.getLHS())->accept(*this)`), same as every other
  // branch of this function -- there is no second evaluation to worry
  // about, unlike a literal `a = a op b` AST rewrite would have caused
  // (§13.5's explicit concern) if this had chosen that route instead.
  if (isOverloadableBinaryOp(Op) && !Node.ResolvedOperatorCandidates.empty() &&
      LhsTy && RhsTy) {
    TurfTypePtr OpResultTy;
    for (const auto &C : Node.ResolvedOperatorCandidates) {
      TurfTypePtr R;
      if (matchOperatorOverload(C, {LhsTy, RhsTy}, /*RelaxSelf=*/false, &Node,
                                Node.getLoc(), R)) {
        if (Node.ResolvedOperatorTarget) {
          reportError(Node.getLoc(), ErrCode::AmbiguousCall,
                      "Ambiguous 'operator " + OpToDisplayString(Op) + "=(" +
                          LhsTy->getName() + ", " + RhsTy->getName() +
                          ")' call: more than one visible overload matches "
                          "at this call site.");
          break;
        }
        Node.ResolvedOperatorTarget = C;
        OpResultTy = R;
      }
    }
    if (Node.ResolvedOperatorTarget) {
      // F-OP-2: `a op= b` desugars to `a = a op b` -- the ASSIGNED type is
      // therefore the operator's OWN return type, not necessarily LhsTy
      // (mirrors ordinary compound assignment's own typesCompatible(
      // ResultTy, LhsTy) check further below, except an operator overload's
      // result must match the LHS's declared type exactly, the same "no
      // coercion" rule every other operator-overload call site enforces).
      if (OpResultTy && !sameTypeExact(OpResultTy.get(), LhsTy.get())) {
        reportError(Node.getLoc(), ErrCode::TypeMismatch,
                    "'operator " + OpToDisplayString(Op) + "' returns '" +
                        OpResultTy->getName() + "', which cannot be "
                        "assigned back to '" + LhsTy->getName() +
                        "' without an explicit cast.");
      }
      setType(&Node, LhsTy);
      return;
    }
  }

  // LANG-001 scope boundary: compound assignment THROUGH `operator[]`
  // (`myMap[k] += 1`) is deliberately not supported by this pass -- it would
  // need a read-modify-write through both `operator[]` and `operator[]=`
  // together, a genuinely separate feature from plain index read/write.
  // Caught here as a clean diagnostic rather than reaching Codegen's
  // CompoundAssignmentExprAST::codegen(), which calls LHS->codegenLValue()
  // unconditionally and would hit ArrayAccessExprAST::codegenLValue()'s
  // "Internal error: Base is not an array or pointer" ICE for a struct base
  // with no PHYSICAL address (see docs/development/changes/LANG-001.md's
  // Known pitfalls).
  if (const auto *ArrAcc = dynamic_cast<const ArrayAccessExprAST *>(Node.getLHS())) {
    TurfTypePtr ArrBaseTy = getType(ArrAcc->getBase());
    // Struct/enum base means this access is ALWAYS operator[]-mediated
    // (never a real array/pointer/string) -- checked directly on the
    // base's type, not on whether LhsTy came back null, because a
    // container that declares BOTH operator[] and operator[]= (the
    // common, symmetric case -- see 10_operator_index_get_set.tr) gives
    // LhsTy a perfectly real type (operator[]'s return type) here, which
    // would otherwise defeat a "LhsTy is null" check entirely.
    if (ArrBaseTy && needsOperatorIndex(ArrBaseTy.get())) {
      reportError(Node.getLoc(), ErrCode::UnsupportedOperation,
                  "Compound assignment through 'operator[]' is not "
                  "supported -- write 'x[i] = x[i] " + OpToDisplayString(Op) +
                      " " + (RhsTy ? RhsTy->getName() : "value") +
                      "' explicitly instead.");
      setType(&Node, RhsTy);
      return;
    }
  }

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
      reportError(Node.getLoc(), ErrCode::ArithmeticOperand,
                  "Bitwise compound assignment requires an integer operand, "
                  "but the left operand is '" + LhsTy->getName() + "'.");
    }
    if (RhsTy && !RhsTy->isNamed("int")) {
      reportError(Node.getLoc(), ErrCode::ArithmeticOperand,
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
      reportError(Node.getLoc(), ErrCode::ArithmeticOperand,
                  "Exponentiation compound assignment requires a numeric "
                  "operand, but the left operand is '" + LhsTy->getName() + "'.");
    }
    if (RhsTy && !RhsTy->isNumeric()) {
      reportError(Node.getLoc(), ErrCode::ArithmeticOperand,
                  "Exponentiation compound assignment requires a numeric "
                  "operand, but the right operand is '" + RhsTy->getName() + "'.");
    }
    // LhsTy is set on Node below (unconditionally, same as every other
    // branch) -- nothing further to do here.
  } else if (LhsTy && RhsTy && LhsTy->isInt32()) {
    // OVH-005 (docs/reference/variables_and_types.md §int32): int32 is
    // never itself an arithmetic result type -- inferArithmeticType()
    // always promotes an int32 operand to plain 'int' (the same
    // "int/bool arithmetic -> int" branch every other integer compound
    // assignment takes), so falling through to the generic path below
    // would report "Cannot assign value of type 'int' to variable of type
    // 'int32'" -- technically true, but it reads like an ordinary type
    // mismatch a wider RHS type could fix, when none ever could. Caught
    // here, before inferArithmeticType() runs at all, with a message that
    // says why and a concrete rewrite.
    const VariableExprAST *LHSVar_i32 = findRootVariable(Node.getLHS());
    std::string VarSpelling = LHSVar_i32 ? LHSVar_i32->getName() : "x";
    reportError(Node.getLoc(), ErrCode::UnsupportedOperation,
                "Compound assignment '" + OpToDisplayString(Op) +
                    "=' is not supported on 'int32' -- int32 is never "
                    "itself an arithmetic result type.",
                {},
                "Write '" + VarSpelling + " = (" + VarSpelling + " " +
                    OpToDisplayString(Op) + " " +
                    (RhsTy ? RhsTy->getName() : "value") +
                    ") to int32;' explicitly instead.");
  } else if (LhsTy && RhsTy) {
    if (inferArithmeticType(Node.getLoc(), Op, LhsTy.get(), RhsTy.get(), ResultTy)) {
      if (!typesCompatible(ResultTy.get(), LhsTy.get())) {
        const VariableExprAST *LHSVar_early = findRootVariable(Node.getLHS());
        std::vector<RelatedLocation> Related;
        if (LHSVar_early && LHSVar_early->ResolvedSymbol)
          Related.push_back({toSourceLocation(LHSVar_early->ResolvedSymbol->DeclLoc),
                             "declared here"});
        reportError(Node.getLoc(), ErrCode::TypeMismatch,
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

  const char *Dir = Node.IsPrefix ? "prefix" : "postfix";

  // LANG-001 follow-up: both directions now parse ANY assignable lvalue
  // (see PostfixExprAST's own doc comment, ASTExpr.h, and ParserCore.cpp's
  // prefix handling for why) instead of prefix being restricted to a bare
  // identifier at parse time -- so a non-lvalue operand (`++(a + b)`,
  // `(f())++`) is no longer caught by the parser and needs the same check
  // assignment/compound assignment already run, here instead.
  if (!isAssignableLValue(Node.getOperand())) {
#ifndef TYPECHECKER_NO_ERRORS
    InvalidAssignmentTargetError(Node.getLoc(),
                                 describeNonLValue(Node.getOperand()))
        .raise();
#else
    throw std::runtime_error("InvalidAssignmentTargetError");
#endif
    setType(&Node, nullptr);
    return;
  }

  // LEC-006: `bs[0].n++` updates the copy `bs[0]` returned.
  {
    const auto *OAA = dynamic_cast<const ArrayAccessExprAST *>(Node.getOperand());
    TurfTypePtr OBase = OAA ? getType(OAA->getBase()) : nullptr;
    if (!(OAA && OBase && needsOperatorIndex(OBase.get())))
      rejectOperatorTemporaryWrite(
          Node.getOperand(), Node.getLoc(),
          "Cannot update part of the value an 'operator[]' read returned: "
          "it is a copy, so the update would be lost.",
          /*PlaceIsStoreTarget=*/true);
  }

  TurfTypePtr OpTy = getType(Node.getOperand());
  std::string OpSpelling(1, static_cast<char>(Node.getOpcode()));

  // LANG-001 scope boundary: `++`/`--` THROUGH `operator[]` (`box[i]++`,
  // `++box[i]`) is deliberately not supported, same reasoning as compound
  // assignment through `operator[]` just above visitCompoundAssignment()'s
  // identical guard -- it would need operator[]/operator[]= chained
  // together, and PostfixExprAST::codegen() calls Operand->codegenLValue()
  // unconditionally, which would otherwise hit the same "Internal error:
  // Base is not an array or pointer" ICE for a struct base with no
  // physical address. A BUILTIN array/pointer/string base (needsOperator
  // Index() is false for those) is completely unaffected -- `++arr[0]` on
  // a plain `int[N]` works exactly like `arr[0]++` always did.
  if (const auto *ArrAcc = dynamic_cast<const ArrayAccessExprAST *>(Node.getOperand())) {
    TurfTypePtr ArrBaseTy = getType(ArrAcc->getBase());
    if (ArrBaseTy && needsOperatorIndex(ArrBaseTy.get())) {
      reportError(Node.getLoc(), ErrCode::UnsupportedOperation,
                  std::string(Dir) + " '" + OpSpelling + OpSpelling +
                      "' through 'operator[]' is not supported -- write "
                      "'x[i] = x[i] " + OpSpelling + " 1' explicitly instead.");
      setType(&Node, OpTy);
      return;
    }
  }

  // ── Operator overload resolution (D21) ───────────────────────────────────
  // Exact match against (OperandType, int), for EITHER direction -- the
  // implicit "+1"/"-1" is always a plain `int`. The expression's OWN type
  // stays OpTy either way (only WHICH value -- old or new -- Codegen
  // returns differs by direction), but the overload's return type must
  // match OpTy exactly for the implicit store-back to be sound.
  bool HandledViaOperator = false;
  if (!Node.ResolvedOperatorCandidates.empty() && OpTy) {
    TurfTypePtr IntTy = TurfInt();
    TurfTypePtr OpResultTy;
    for (const auto &C : Node.ResolvedOperatorCandidates) {
      TurfTypePtr R;
      if (matchOperatorOverload(C, {OpTy, IntTy}, /*RelaxSelf=*/false, &Node,
                                Node.getLoc(), R)) {
        OpResultTy = R;
        if (Node.ResolvedOperatorTarget) {
          reportError(Node.getLoc(), ErrCode::AmbiguousCall,
                      "Ambiguous " + std::string(Dir) + " 'operator " + OpSpelling + "(" +
                          OpTy->getName() +
                          ", int)' call: more than one visible overload "
                          "matches at this call site.");
          break;
        }
        Node.ResolvedOperatorTarget = C;
      }
    }
    if (Node.ResolvedOperatorTarget) {
      if (OpResultTy && !sameTypeExact(OpResultTy.get(), OpTy.get())) {
        reportError(Node.getLoc(), ErrCode::TypeMismatch,
                    "'operator " + OpSpelling + "' returns '" +
                        OpResultTy->getName() +
                        "', which cannot be stored back into '" +
                        OpTy->getName() + "' for " + Dir + " '" + OpSpelling +
                        OpSpelling + "'.");
      }
      HandledViaOperator = true;
    }
  }

  // OVH-005 (docs/reference/variables_and_types.md §int32): int32 is never
  // itself an arithmetic result type, so `++`/`--` (exactly `x = x +/- 1`)
  // has no legal lowering for it -- checked BEFORE the generic "requires a
  // numeric operand" rejection just below (int32 IS numeric, so that check
  // would silently pass it through) and, critically, BEFORE any Codegen is
  // reached at all: PostfixExprAST::codegen() (CodegenVar.cpp) unconditionally
  // builds its overflow-checked-add intrinsic call assuming an i64 operand,
  // and a genuinely i32-typed OldVal there produced invalid LLVM IR -- a
  // real internal compiler error, not a diagnosable one -- before this
  // check existed.
  if (!HandledViaOperator && OpTy && OpTy->isInt32()) {
    const VariableExprAST *OperandVar = findRootVariable(Node.getOperand());
    std::string VarSpelling = OperandVar ? OperandVar->getName() : "x";
    reportError(Node.getLoc(), ErrCode::UnsupportedOperation,
                std::string(Dir) + " '" + OpSpelling + OpSpelling +
                    "' is not supported on 'int32' -- int32 is never itself "
                    "an arithmetic result type.",
                {},
                "Write '" + VarSpelling + " = (" + VarSpelling + " " +
                    OpSpelling + " 1) to int32;' explicitly instead.");
    setType(&Node, OpTy);
    return;
  }

  if (!HandledViaOperator && OpTy && !OpTy->isNumeric()) {
    reportError(Node.getLoc(), ErrCode::ArithmeticOperand,
                (std::string(Dir) + " operator requires a numeric operand, got '") +
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
  // E140 (issue #260): nothing converts TO a slice -- a `T[N]` becomes one
  // only by being passed to a `T[]` parameter.
  if (checkUnsizedArrayPlacement(
          DestTy.get(), /*AllowAtTop=*/false, Node.getLoc(),
          "A conversion",
          "There is no conversion to an unsized array. Pass the array "
          "straight to the 'T[]' parameter; it is accepted as it is.")) {
    setType(&Node, nullptr);
    return;
  }
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
          reportError(Node.getLoc(), ErrCode::EnumValueOutOfRange,
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

  // SEM-002 (docs/planning/ARCHITECTURE_OVERHAUL.md §8): 'to'-cast
  // legality, Rule B1 -- moved here from Codegen (CastExprAST::codegen(),
  // CodegenExpr.cpp) so an illegal cast is caught at the TypeChecker
  // layer, like every other type rule, instead of Codegen being the only
  // thing standing between it and a diagnostic (docs/architecture/
  // compiler_pipeline.md §0's own stated pipeline invariant: Codegen only
  // ever runs on an AST that has ALREADY passed the TypeChecker cleanly).
  // Mirrors CodegenExpr.cpp's own pairwise dispatch pair-for-pair --
  // Codegen's own final "Everything else is unsupported" TypeError stays
  // in place as a defensive, should-be-unreachable fallback (see this
  // group's change record's Known pitfalls on why it isn't deleted).
  if (OperandTy) {
    // SMP-002: TypeWalk::containsOpenParam(..., ParamScope::Bare) is exactly
    // the bare `dynamic_cast<const TurfGenericParamType *>` this used to
    // spell out inline -- no unwrapping, no NamedType fallback, mirroring
    // typesCompatible()'s own Rule 1 top-level check (Types.cpp) which this
    // bypass was always meant to shadow (STD-003's Fix 3).
    bool EitherSideIsGenericParam =
        TypeWalk::containsOpenParam(OperandTy.get(), TypeWalk::ParamScope::Bare) ||
        TypeWalk::containsOpenParam(DestTy.get(), TypeWalk::ParamScope::Bare);

    if (!EitherSideIsGenericParam) {
      const std::string SrcName = OperandTy->getName();
      const std::string DstName = DestTy->getName();

      // Resolve OperandTy through GlobalSymbolTable too, mirroring DestTy's
      // own resolution above -- an enum-typed operand is otherwise still a
      // bare NamedType at this point (same root cause as the enum-literal
      // check above).
      TurfTypePtr ResolvedOperandTy = OperandTy;
      if (auto *NT2 = dynamic_cast<const NamedType *>(OperandTy.get())) {
        if (GlobalSymbolTable) {
          if (auto Resolved = GlobalSymbolTable->LookupType(NT2->getName()))
            ResolvedOperandTy = Resolved;
        }
      }
      bool SrcIsEnum = dynamic_cast<const TurfEnumType *>(ResolvedOperandTy.get()) != nullptr;
      bool DstIsEnum = dynamic_cast<const TurfEnumType *>(DestTy.get()) != nullptr;
      bool SrcIsPtrLike = dynamic_cast<const TurfPointerType *>(OperandTy.get()) != nullptr ||
                          SrcName == "ptr" || SrcName == "string";
      bool DstIsPtrLike = dynamic_cast<const TurfPointerType *>(DestTy.get()) != nullptr ||
                          DstName == "ptr" || DstName == "string";

      bool Legal =
          SrcName == DstName ||
          (SrcName == "int" && DstName == "double") ||
          (SrcName == "double" && DstName == "int") ||
          (SrcName == "int32" && DstName == "int") ||
          (SrcName == "int" && DstName == "int32") ||
          (SrcName == "int32" && DstName == "double") ||
          (SrcName == "double" && DstName == "int32") ||
          (SrcName == "char" && DstName == "int") ||
          (SrcName == "int" && DstName == "char") ||
          (SrcIsEnum && DstName == "int") ||
          (SrcName == "int" && DstIsEnum) ||
          (SrcName == "string" && DstName == "int") ||
          (SrcName == "string" && DstName == "double") ||
          (SrcName == "int" && DstName == "string") ||
          (SrcName == "double" && DstName == "string") ||
          (SrcName == "bool" && DstName == "string") ||
          (SrcIsEnum && DstName == "string") ||
          (SrcName == "string" && DstIsEnum) ||
          (SrcName == "int" && dynamic_cast<const TurfPointerType *>(DestTy.get()) != nullptr) ||
          (SrcIsPtrLike && DstIsPtrLike);

      if (!Legal) {
#ifndef TYPECHECKER_NO_ERRORS
        // `x to string` of something with no text form is E065, whose page
        // lists what DOES convert; E065 was raised only from a Codegen backstop
        // this check always pre-empts, so it was documented and unreachable.
        TypeError(Node.getLoc(),
                  DstName == "string" ? ErrCode::StringConversion
                                      : ErrCode::InvalidCast,
                  "Cannot explicitly cast '" + SrcName +
                                      "' to '" + DstName + "'")
            .raise();
#else
        throw std::runtime_error("TypeError: Cannot explicitly cast");
#endif
      }
    }
  }

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

  // LEC-003 (FREEZE.md §6.17). Arms are tried in source order and the first
  // header that matches wins, so one UNGUARDED header that matches every value
  // (`_`, or a struct pattern whose fields all bind or ignore) makes every
  // later arm, and `otherwise`, dead code. Remembered as the location of the
  // first such header, to point the warning at.
  std::optional<SourceLocation> CatchAllLoc;

  // What the UNGUARDED headers cover, for the exhaustiveness check below:
  // enum/machine variant names, or "true"/"false" for a bool subject. A
  // guarded header is left out on purpose -- its guard may fail. For a subject
  // of a declared error type (LEC-004) it also holds the variants covered by
  // a variant pattern whose every field pattern is irrefutable.
  std::set<std::string> CoveredValues;

  // The one raw pointer a pattern may look through is `self` (never null;
  // field access exempts it the same way).
  const bool SelfSubject = [&] {
    const auto *V = dynamic_cast<const VariableExprAST *>(Node.getSubject());
    return V && V->getName() == "self";
  }();

  // 2. Type-check each arm's patterns and body
  for (auto &Arm : Node.getArmsMut()) {
    if (CatchAllLoc && FirstWarningAt("unreachable-arm", Arm.Loc))
      UnreachableArmWarning(Arm.Loc, "This arm", *CatchAllLoc).emit();

    // A binding shadows anything of the same name for the whole arm. A `for`
    // loop variable lives in LoopVarTypes_ and visitVariable() consults it
    // BEFORE the resolved symbol, so it would otherwise win over the binding.
    std::vector<std::pair<std::string, std::optional<TurfTypePtr>>> ShadowedLoopVars;
    for (const auto &B : Arm.Bindings) {
      auto It = LoopVarTypes_.find(B->Name);
      ShadowedLoopVars.push_back(
          {B->Name, It == LoopVarTypes_.end()
                        ? std::nullopt
                        : std::optional<TurfTypePtr>(It->second)});
      if (It != LoopVarTypes_.end())
        LoopVarTypes_.erase(It);
    }

    std::unordered_map<std::string, TurfTypePtr> ArmBindingTypes;
    std::optional<SourceLocation> ArmCatchAllLoc;

    for (size_t K = 0; K < Arm.Values.size(); ++K) {
      auto &PatExpr = Arm.Values[K];
      ExprAST *Guard = K < Arm.Guards.size() ? Arm.Guards[K].get() : nullptr;

      // `_` and `Name { ... }` are the destructuring forms; everything else is
      // the original equality pattern, checked below exactly as before.
      const bool IsDestructuring =
          dynamic_cast<WildcardPatternExprAST *>(PatExpr.get()) ||
          dynamic_cast<StructPatternExprAST *>(PatExpr.get()) ||
          dynamic_cast<ErrorVariantPatternExprAST *>(PatExpr.get());

      if (IsDestructuring) {
        const bool Irrefutable =
            checkPattern(PatExpr.get(), SubjectTy, Arm.Loc, ArmBindingTypes, SelfSubject);
        if (Irrefutable && !Guard && !ArmCatchAllLoc) {
          if (auto *W = dynamic_cast<WildcardPatternExprAST *>(PatExpr.get()))
            ArmCatchAllLoc = W->getLoc();
          else
            ArmCatchAllLoc = static_cast<StructPatternExprAST *>(PatExpr.get())->getLoc();
        }
        // An error-variant pattern is never a catch-all (checkPattern() says
        // so by returning false), but an unguarded one whose fields all match
        // anything covers its whole variant.
        if (auto *EV = dynamic_cast<ErrorVariantPatternExprAST *>(PatExpr.get());
            EV && !Guard && EV->FieldsIrrefutable)
          if (const auto *Head = dynamic_cast<const ScopeAccessExprAST *>(EV->getHead()))
            CoveredValues.insert(Head->getRightName());
      } else {
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
        if (Num->isInteger()) {
          // Exact accessor, not getIntVal() -- this key is what makes two
          // `compare` arms with the same literal a real
          // DuplicateOtherwiseArmError-style diagnostic (SEM-001,
          // docs/planning/ARCHITECTURE_OVERHAUL.md §8's own Risks note).
          // getIntVal()'s 64-bit truncation could make two DIFFERENT wide
          // literals collide into a false "duplicate arm".
          llvm::SmallString<32> Digits;
          Num->getIntValExact().toString(Digits, 10);
          PatternKey = "int:" + Digits.str().str();
        }
      } else if (auto *Str = dynamic_cast<StringExprAST *>(PatExpr.get())) {
        PatternKey = "str:" + Str->getValue();
      }

      // A GUARDED header can legitimately repeat a value its guard makes
      // conditional (`matches 1 if a:` then `matches 1 if b:`), so only an
      // unguarded one is a duplicate -- and only an unguarded one is
      // remembered for the next header to collide with.
      if (PatternKey && !Guard && !SeenPatternKeys.insert(*PatternKey).second) {
        reportError(Arm.Loc, ErrCode::DuplicateMatchArm,
                    "Duplicate 'matches' pattern: this value is already "
                    "handled by an earlier arm in this compare block, so "
                    "this arm can never run.",
                    {},
                    "Remove the duplicate arm, or combine it with the "
                    "earlier one by stacking (`matches A:` then "
                    "`matches B:` sharing one body).",
                    "Duplicate pattern!");
      }

      if (!Guard) {
        if (auto *EA = dynamic_cast<ScopeAccessExprAST *>(PatExpr.get()))
          CoveredValues.insert(EA->getVariant());
        else if (auto *BL = dynamic_cast<BoolExprAST *>(PatExpr.get()))
          CoveredValues.insert(BL->getVal() ? "true" : "false");
      }
      } // original equality pattern

      // A guard is a plain condition, evaluated with the header's bindings in
      // scope. Checked after the pattern so those bindings have their types.
      if (Guard) {
        check(Guard);
        TurfTypePtr GuardTy = getType(Guard);
        if (GuardTy && !GuardTy->isNamed("bool")) {
          InvalidPatternError(
              Arm.Loc,
              "a guard must be a 'bool' condition, but this one has type '" +
                  GuardTy->getName() + "'.",
              "Write a comparison, e.g. `matches Point { x, y } if x == y:`.")
              .raise();
        }
      }
    }

    // Only once every header of THIS arm is read: the arm's own later headers
    // are not shadowed by its first.
    if (ArmCatchAllLoc && !CatchAllLoc)
      CatchAllLoc = ArmCatchAllLoc;

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

    // Hand the shadowed loop variables back.
    for (auto &[Name, Saved] : ShadowedLoopVars)
      if (Saved)
        LoopVarTypes_[Name] = *Saved;
  }

  // 3. Type-check the 'otherwise' body if present
  if (Node.getOtherBodyMut()) {
    if (CatchAllLoc && FirstWarningAt("unreachable-arm", Node.getOtherLoc()))
      UnreachableArmWarning(Node.getOtherLoc(), "The 'otherwise' arm", *CatchAllLoc).emit();
    check(Node.getOtherBodyMut());
  }

  // 4. Exhaustiveness (FREEZE.md §6.17). With no `otherwise` and no header
  //    that matches everything, an enum/machine or bool subject must have
  //    every value covered by an UNGUARDED header, and an open error union
  //    can never be -- it always needs an arm that takes the rest. An int or
  //    string subject, or a struct with a refutable pattern, may still fall
  //    through to "no arm ran" (existing behaviour; rejecting it would add
  //    no soundness, only breakage).
  bool IsEnum = SubjectTy && SubjectTy->isEnum();
  Node.SubjectIsEnum = IsEnum;

  const TurfEnumType* ET = IsEnum
      ? dynamic_cast<const TurfEnumType*>(SubjectTy.get()) : nullptr;

  bool Covered = false;
  if (!Node.getOtherBody()) {
    if (CatchAllLoc) {
      Covered = true;
    } else if (ET) {
      std::vector<std::string> Missing;
      for (const auto &V : ET->getVariants())
        if (!CoveredValues.count(V))
          Missing.push_back(ET->getName() + "::" + V);
      if (Missing.empty())
        Covered = true;
      else
        NonExhaustiveCompareError(
            Node.getLoc(), "enum comparison", ET->getName(), "Missing variants:",
            Missing,
            "Add an arm for each missing variant, or 'otherwise:' (or "
            "`matches _:`) to handle the rest. A guarded arm doesn't count: "
            "its guard may be false.")
            .raise();
    } else if (SubjectTy && SubjectTy->isNamed("bool")) {
      std::vector<std::string> Missing;
      for (const char *V : {"true", "false"})
        if (!CoveredValues.count(V))
          Missing.push_back(V);
      if (Missing.empty())
        Covered = true;
      else
        NonExhaustiveCompareError(
            Node.getLoc(), "bool comparison", "bool", "Missing values:", Missing,
            "Add `matches true:` and `matches false:` arms, or 'otherwise:' "
            "to handle the rest. A guarded arm doesn't count: its guard may "
            "be false.")
            .raise();
    } else if (const auto *UET =
                   SubjectTy ? dynamic_cast<const TurfUserErrorType *>(SubjectTy.get())
                             : nullptr) {
      // A DECLARED error type has a closed, listable set of variants (unlike
      // the open union), so covering them all is exhaustive -- each by an
      // unguarded variant pattern that matches every payload it can carry.
      std::vector<std::string> Missing;
      for (const auto &V : UET->getVariants())
        if (!CoveredValues.count(V))
          Missing.push_back(TypeIdentity::bareName(UET->getName()) + "::" + V);
      if (Missing.empty())
        Covered = true;
      else
        NonExhaustiveCompareError(
            Node.getLoc(), "error comparison", UET->getName(), "Missing variants:",
            Missing,
            "Add an arm for each missing variant, or 'otherwise:' (or `matches "
            "_:`) to handle the rest. A guarded arm doesn't count, and neither "
            "does one whose payload pattern can fail to match (`E::V(0)`): use "
            "`E::V(..)` or `E::V(name)` to cover the variant.")
            .raise();
    } else if (SubjectTy && SubjectTy->isAnyError()) {
      NonExhaustiveCompareError(
          Node.getLoc(), "comparison", SubjectTy->getName(), "", {},
          "An error value can be any error a callee might return, which "
          "can't be listed. End the compare with 'otherwise:' (or `matches "
          "_:`) to handle the ones you don't name.")
          .raise();
    }
  }
  Node.Exhaustive = Covered;

  // 5. Empty compare{} warning was already emitted by the Parser
  setType(&Node, TurfVoid());
}
