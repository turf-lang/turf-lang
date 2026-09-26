/**
 * @file TypeCheckerCore.cpp
 * @brief Entry points, type-inference helpers (arithmetic/relational), and the default error handler.
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
#include "Parser.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/SmallString.h"

#include <algorithm>

#include <cassert>
#include <cstring>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

#ifndef TYPECHECKER_NO_ERRORS
/// Default error handler: uses the real TypeError class from Errors.h.
void productionErrorHandler(SourceLocation Loc, const std::string &Msg,
                            const std::vector<RelatedLocation> &Related,
                            const std::string &CustomHint,
                            const std::string &CustomBanner,
                            const SLM::DiagnosticHints &Hints) {
  TypeError Err(Loc, Msg, CustomHint, CustomBanner);
  for (const auto &R : Related)
    Err.withRelated(R.Loc, R.Description);
  for (const auto &Fact : Hints.KnownTypes)
    Err.withKnownType(Fact);
  Err.raise();
}
#else
/// Stub used when TYPECHECKER_NO_ERRORS is defined (unit-test builds).
/// Throws std::runtime_error so tests can catch it with EXPECT_THROWS.
void unitTestErrorHandler(SourceLocation, const std::string &Msg,
                          const std::vector<RelatedLocation> &,
                          const std::string &, const std::string &,
                          const SLM::DiagnosticHints &) {
  throw std::runtime_error("TypeError: " + Msg);
}
#endif

} // namespace

// toSourceLocation() is defined once, shared with the Resolver split, in
// ResolverSymbolBase.h -- see its doc comment.

// ─── Constructors ─────────────────────────────────────────────────────────────

TypeChecker::TypeChecker(const ResolverSymbolTable &RST)
    : RST_(RST), ErrorHandler_(productionErrorHandler) {}

TypeChecker::TypeChecker(const ResolverSymbolTable &RST, ErrorHandler Handler)
    : RST_(RST), ErrorHandler_(std::move(Handler)) {}


void TypeChecker::checkAll(const std::vector<std::unique_ptr<ExprAST>> &Nodes,
                           bool RecoverPerDeclaration) {
  if (!RecoverPerDeclaration) {
    for (const auto &Node : Nodes)
      check(Node.get());
    return;
  }

  // Per-top-level-node recovery (issue #246, opt-in) -- mirrors the
  // identical fix in Resolver::resolveAll() (ResolverCore.cpp); see that
  // function's own doc comment for the full save/restore rationale.
  for (const auto &Node : Nodes) {
    const bool SavedRecoverActive = g_recoverActive;
    jmp_buf SavedJmpBuf;
    std::memcpy(SavedJmpBuf, g_recoverJmp, sizeof(jmp_buf));

    g_recoverActive = true;
    if (setjmp(g_recoverJmp) == 0) {
      check(Node.get());
    } else {
      resetForRecovery();
    }
    g_recoverActive = SavedRecoverActive;
    std::memcpy(g_recoverJmp, SavedJmpBuf, sizeof(jmp_buf));
  }
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
                              const std::vector<RelatedLocation> &Related,
                              const std::string &CustomHint,
                              const std::string &CustomBanner,
                              const SLM::DiagnosticHints &Hints) {
  HadError_ = true;
  if (ErrorHandler_)
    ErrorHandler_(Loc, Msg, Related, CustomHint, CustomBanner, Hints);
}


// ─── `unsafe { }` gate (issue #238) ────────────────────────────────────────

bool TypeChecker::isUnsafeGateExempt(const SourceLocation &Loc,
                                     const ExprAST *Node) const {
  if (UnsafeDepth_ > 0)
    return true;
  if (Node && WasDeclaredInsideTopLevelUnsafe(Node))
    return true;
  return HasRawPointerCapability(Loc.Filename);
}


bool TypeChecker::checkUnsafeGate(const TurfType *T, SourceLocation Loc,
                                  const ExprAST *Node) {
  if (!isRawPointer(T))
    return false;
  if (isUnsafeGateExempt(Loc, Node))
    return false;
  // isRawPointer(T) already established T->isPointer(), so this always
  // succeeds -- OpaquePointerType ("ptr") is the only isPointer() type NOT
  // a TurfPointerType, and it has no pointee to suggest a "&T" spelling
  // for anyway, hence the "&T" fallback.
  const auto *PtrTy = dynamic_cast<const TurfPointerType *>(T);
  std::string SuggestedRef = PtrTy ? "&" + PtrTy->getPointee()->getName() : "&T";
  reportError(
      Loc,
      "Raw pointer type '" + T->getName() +
          "' cannot be used outside an 'unsafe' block.",
      {},
      "Wrap this in 'unsafe { }', or use a safe reference ('" + SuggestedRef +
          "') instead if you don't actually need a possibly-null raw pointer.",
      "Unsafe pointer use!");
  return true;
}


void TypeChecker::checkNoRawPointersInSignature(const FuncDefExprAST &Node) {
  if (HasRawPointerCapability(Node.getLoc().Filename))
    return;
  for (const auto &Param : Node.getParams()) {
    // The implicit `self` parameter (ParserDecl.cpp's ParseFuncDef()) is
    // always synthesized as a raw TurfPointerType(StructName), the same
    // "pass-by-reference" idiom this whole issue is narrowing everywhere
    // ELSE -- but self is always non-null by construction and every
    // struct-bound method signature would otherwise fail this check,
    // which would ban ordinary struct methods entirely. Mirrors the
    // identical self-exemption in visitFieldAccess() (TypeCheckerCall.cpp).
    if (Param.Name == "self")
      continue;
    if (isRawPointer(Param.Type.get())) {
      reportError(
          Node.getLoc(),
          "Function '" + Node.getName() + "' cannot use raw pointer type '" +
              Param.Type->getName() + "' for parameter '" + Param.Name + "'.",
          {},
          "Raw pointer types are never allowed in an ordinary function's "
          "signature, even inside 'unsafe { }' -- use '&T' instead. Only "
          "'extern fn' bindings and lib/std may use raw pointers here.",
          "Raw pointer in function signature!");
    }
  }
  if (isRawPointer(Node.getReturnType())) {
    reportError(
        Node.getLoc(),
        "Function '" + Node.getName() + "' cannot return raw pointer type '" +
            Node.getReturnType()->getName() + "'.",
        {},
        "Raw pointer types are never allowed in an ordinary function's "
        "signature, even inside 'unsafe { }' -- use '&T' instead. Only "
        "'extern fn' bindings and lib/std may use raw pointers here.",
        "Raw pointer in function signature!");
  }
}


void TypeChecker::visitUnsafeBlock(UnsafeBlockExprAST &Node) {
  ++UnsafeDepth_;
  check(Node.getBodyMut());
  --UnsafeDepth_;
  // The unsafe block's type is its Body's type -- mirrors visitBlock()
  // (TypeCheckerStmt.cpp)'s own "last expression's type, or void if empty"
  // rule, since Body is always a BlockExprAST (see ParseUnsafeExpr()) and
  // this needs to stay transparent for `unsafe { }` used as a
  // value-producing expression (e.g. `x = unsafe { someRawDeref; };`), not
  // just as a bare statement.
  TurfTypePtr BodyTy = getType(Node.getBody());
  setType(&Node, BodyTy ? BodyTy : TurfVoid());
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


// ─── SEM-002: literal-representability checking ──────────────────────────────
// (docs/planning/ARCHITECTURE_OVERHAUL.md §8, FREEZE.md §6.2 rows 3/6/9) --
// see each method's own doc comment (TypeChecker.h) for the full contract.

std::optional<llvm::APSInt>
TypeChecker::tryFoldUntypedIntConstant(const ExprAST *E, bool IsTopLevel) {
  if (const auto *NE = dynamic_cast<const NumberExprAST *>(E)) {
    if (!NE->isInteger() || !NE->isUntyped())
      return std::nullopt;
    if (!IsTopLevel && !NE->getIntValExact().isSignedIntN(64)) {
      llvm::SmallString<48> S;
      NE->getIntValExact().toString(S, 10, /*Signed=*/true);
#ifndef TYPECHECKER_NO_ERRORS
      IntegerLiteralOutOfRangeError(NE->getLoc(), S.str().str(), "int",
                                    "-9223372036854775808 .. 9223372036854775807")
          .raise();
#else
      throw std::runtime_error("IntegerLiteralOutOfRangeError");
#endif
      return std::nullopt;
    }
    return NE->getIntValExact();
  }

  if (const auto *UE = dynamic_cast<const UnaryExprAST *>(E)) {
    if (UE->getOpcode() != '-')
      return std::nullopt;
    // Propagates IsTopLevel: folding "-BIG" is still the same single
    // literal (just negated), per FREEZE.md's own `-9223372036854775808`
    // example -- see this method's own doc comment.
    auto Operand = tryFoldUntypedIntConstant(UE->getOperand(), IsTopLevel);
    if (!Operand)
      return std::nullopt;
    llvm::APInt Widened = llvm::APInt(*Operand).sext(Operand->getBitWidth() + 1);
    llvm::APInt Negated = -Widened;
    return llvm::APSInt(std::move(Negated), /*isUnsigned=*/false);
  }

  if (const auto *BE = dynamic_cast<const BinaryExprAST *>(E)) {
    int Op = BE->getOp();
    if (Op != '+' && Op != '-' && Op != '*')
      return std::nullopt;
    // Always false: a binary operator's own operands are independent
    // leaves, not "the same literal" -- see this method's own doc comment.
    auto L = tryFoldUntypedIntConstant(BE->getLHS(), false);
    if (!L)
      return std::nullopt;
    auto R = tryFoldUntypedIntConstant(BE->getRHS(), false);
    if (!R)
      return std::nullopt;
    // Generous headroom (never itself a source of overflow -- only the
    // FINAL destination check, in checkLiteralRepresentable(), is allowed
    // to reject a value) so the fold itself is exact arbitrary-precision
    // arithmetic, per FREEZE.md §6.2 row 6.
    unsigned FoldWidth = std::max(L->getBitWidth(), R->getBitWidth()) * 2 + 8;
    llvm::APInt LW = llvm::APInt(*L).sext(FoldWidth);
    llvm::APInt RW = llvm::APInt(*R).sext(FoldWidth);
    llvm::APInt Result = Op == '+' ? (LW + RW) : Op == '-' ? (LW - RW) : (LW * RW);
    return llvm::APSInt(std::move(Result), /*isUnsigned=*/false);
  }

  return std::nullopt;
}


void TypeChecker::checkLiteralRepresentable(const ExprAST *ValueExpr,
                                            const TurfType *DestType,
                                            SourceLocation Loc) {
  if (!ValueExpr || !DestType)
    return;

  bool IsInt = DestType->isNamed("int");
  bool IsInt32 = DestType->isNamed("int32");
  bool IsDouble = DestType->isNamed("double");
  if (!IsInt && !IsInt32 && !IsDouble)
    return;

  auto Folded = tryFoldUntypedIntConstant(ValueExpr, /*IsTopLevel=*/true);
  if (!Folded)
    return; // not a pure literal-arithmetic tree -- nothing to check here

  const llvm::APSInt &V = *Folded;
  llvm::SmallString<48> S;
  V.toString(S, 10, /*Signed=*/true);
  std::string ExactStr = S.str().str();

#ifndef TYPECHECKER_NO_ERRORS
  if (IsInt) {
    if (!V.isSignedIntN(64))
      IntegerLiteralOutOfRangeError(Loc, ExactStr, "int",
                                    "-9223372036854775808 .. 9223372036854775807")
          .raise();
  } else if (IsInt32) {
    if (!V.isSignedIntN(32))
      IntegerLiteralOutOfRangeError(Loc, ExactStr, "int32",
                                    "-2147483648 .. 2147483647")
          .raise();
  } else if (IsDouble) {
    // See TypeChecker.h's own doc comment on why this requires fitting
    // int64 FIRST, rather than checking double-exactness against the
    // literal's true (possibly wider) value directly.
    if (!V.isSignedIntN(64)) {
      IntegerLiteralOutOfRangeError(Loc, ExactStr, "int",
                                    "-9223372036854775808 .. 9223372036854775807")
          .raise();
      return;
    }
    llvm::APFloat AsDouble(0.0);
    llvm::APFloat::opStatus St =
        AsDouble.convertFromAPInt(V, /*IsSigned=*/true, llvm::APFloat::rmNearestTiesToEven);
    if (St & llvm::APFloat::opInexact)
      IntegerLiteralNotExactAsDoubleError(Loc, ExactStr).raise();
  }
#else
  (void)ExactStr;
  if ((IsInt && !V.isSignedIntN(64)) || (IsInt32 && !V.isSignedIntN(32)))
    throw std::runtime_error("IntegerLiteralOutOfRangeError");
#endif
}


void TypeChecker::checkCallArgsRepresentable(
    const std::vector<std::unique_ptr<ExprAST>> &Args,
    const std::vector<FunctionParam> &Params, SourceLocation Loc,
    size_t ParamOffset) {
  if (ParamOffset >= Params.size())
    return;
  size_t Count = std::min(Args.size(), Params.size() - ParamOffset);
  for (size_t i = 0; i < Count; ++i) {
    const TurfType *ParamTy = Params[i + ParamOffset].Type.get();
    if (ParamTy)
      checkLiteralRepresentable(Args[i].get(), ParamTy, Loc);
  }
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

  // String arithmetic ('+' concatenation) is no longer hardcoded here -- it
  // goes through operator overload resolution in visitBinary() (see
  // strings.tr's `operator +(string, string)`), which runs BEFORE this
  // function is ever called and returns early on a match. Reaching this
  // function with a string-typed operand therefore means no operator
  // matched (either `strings` isn't imported, or the combination genuinely
  // has no overload, e.g. `s - 1`) -- fall through to the generic mismatch
  // error below, which gets a string-specific hint.

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
  std::string Hint;
  if (LhsTy->isString() || RhsTy->isString()) {
    Hint = "String '" + OpToDisplayString(Op) + "' is provided by an "
           "'operator " + OpToDisplayString(Op) + "' overload in "
           "lib/std/strings.tr -- make sure 'strings' is imported. To "
           "concatenate a non-string, convert it first with `<expr> to "
           "string`.";
  } else if (LhsTy->isStruct() || RhsTy->isStruct()) {
    // Distinguishes two different reasons this can happen: no overload was
    // ever declared for this operator on these types at all, OR one WAS
    // declared but didn't match because operator resolution requires an
    // EXACT type match on both operands (no int->double widening the way
    // ordinary function-call arguments get, see visitBinary()'s operator-
    // resolution gate) -- e.g. only `operator +(Vec2, double)` exists and
    // this call passed an int. Naming both possibilities here avoids the
    // false impression that no '+' overload exists at all when one
    // actually does, just not for this exact pairing.
    Hint = "'" + OpToDisplayString(Op) + "' isn't a builtin operation on a "
           "struct -- declare 'operator " + OpToDisplayString(Op) + "(" +
           LhsTy->getName() + ", " + RhsTy->getName() +
           "): ReturnType { ... }', or check that an existing overload's "
           "parameter types match EXACTLY (operator resolution never "
           "widens int to double the way an ordinary function call does).";
  }
  reportError(Loc,
              "Type mismatch: cannot apply '" + OpToDisplayString(Op) +
                  "' to '" + LhsTy->getName() + "' and '" +
                  RhsTy->getName() + "'.",
              {}, Hint);
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
    // Struct instances specifically -- reject BEFORE the "same name" check
    // below, which would otherwise accept two same-struct-typed operands
    // (both getName() == "Point") and hand back TurfBool() as if this were
    // a real, meaningful comparison. Codegen has no struct-equality
    // lowering at all -- it emits a raw FCmp/ICmp on the whole aggregate
    // LLVM struct value, which is invalid IR for anything but a true
    // scalar. Confirmed via two `Point{x:1}` instances compiled with `==`:
    // "Internal compiler error: generated LLVM IR is invalid... Invalid
    // operand types for FCmp instruction" -- a confusing ICE for what's
    // actually just an unsupported (for now) language feature, not a
    // compiler bug.
    if (LhsTy->isStruct() || RhsTy->isStruct()) {
      reportError(Loc,
                  "Cannot compare struct values with '" +
                      OpToDisplayString(Op) + "'.",
                  {},
                  "Structs don't support direct comparison yet -- compare "
                  "the fields you care about individually instead (e.g. "
                  "`a.x == b.x`).");
      return TurfBool();
    }
    // `err == MathError::DivisionByZero` inside `recover as err run { ... }`
    // (docs/architecture/errors_and_recover.md): `err`'s type is AnyErrorType
    // (its real concrete error type isn't knowable at compile time -- `!T`
    // doesn't restrict which declared error type a call can fail with), so the
    // ordinary "same getName()" check below can never match it against a
    // real TurfUserErrorType. Allowed specifically (and only) when one side
    // is AnyErrorType and the other is a genuine declared error-type value.
    if ((LhsTy->isAnyError() && RhsTy->isUserError()) ||
        (RhsTy->isAnyError() && LhsTy->isUserError()))
      return TurfBool();

    // Same type or both numeric is fine -- EXCLUDING string. Two strings
    // must resolve via an `operator ==`/`operator !=` overload instead (see
    // STRING_UFCS_OPERATOR_CHAR_RFC.md's build notes, "Blocker 2"): leaving
    // string in this same-name shortcut would let `s1 == s2` compile to a
    // raw pointer-identity ICmp on the underlying i8* whenever no operator
    // overload is visible -- silently comparing addresses while LOOKING
    // like a real string comparison, which is far worse than a compile
    // error. visitBinary()'s operator-resolution gate (above, before this
    // function is ever called) already handles the case where an overload
    // IS visible; reaching here with two strings means none was.
    if (LhsTy->getName() == RhsTy->getName() && !LhsTy->isString())
      return TurfBool();
    if (LhsTy->isNumeric() && RhsTy->isNumeric())
      return TurfBool();

    // Allow comparing pointers to null -- but not two strings against each
    // other (same rationale as directly above). A string still legitimately
    // compares against null/an actual pointer type here.
    if (!(LhsTy->isString() && RhsTy->isString())) {
      bool isLhsPtrOrNull = LhsTy->isPointer() || LhsTy->isNull() || LhsTy->isNamed("ptr") || LhsTy->isNamed("void*") || LhsTy->isString();
      bool isRhsPtrOrNull = RhsTy->isPointer() || RhsTy->isNull() || RhsTy->isNamed("ptr") || RhsTy->isNamed("void*") || RhsTy->isString();
      if (isLhsPtrOrNull && isRhsPtrOrNull)
        return TurfBool();
    }

    reportError(Loc, "Cannot compare '" + LhsTy->getName() + "' with '" +
                         RhsTy->getName() + "'.",
                {},
                (LhsTy->isNull() || RhsTy->isNull())
                    ? "'null' is only valid for pointer types -- a plain "
                      "int/double/bool/string value can never be null, so "
                      "this comparison can never be true."
                    : "");
    return TurfBool(); // continue with bool to limit cascade errors
  }

  // Ordering operators (< > <= >=): both must be numeric. String ordering
  // used to be hardcoded here (byte-by-byte via strcmp) but now goes through
  // operator overload resolution instead (strings.tr's `operator </>/<=/>=`),
  // handled by visitBinary()'s operator-resolution gate before this function
  // is ever called -- reaching here with two strings means no overload was
  // visible, so it correctly falls through to the generic error below.

  // char ordering is built in (byte-value comparison) but, per
  // STRING_UFCS_OPERATOR_CHAR_RFC.md \u00a73, char is deliberately NOT isNumeric()
  // -- so without this explicit branch it would fall through to the
  // isNumeric()&&isNumeric() check below and be rejected. This branch is the
  // one place char's built-in ordering actually gets granted.
  if (LhsTy->isChar() && RhsTy->isChar())
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
                       OpToDisplayString(Op) + "'.");
  return TurfBool(); // continue with bool
}
