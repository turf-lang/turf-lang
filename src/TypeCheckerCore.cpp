/**
 * @file TypeCheckerCore.cpp
 * @brief Entry points, type-inference helpers (arithmetic/relational), and the default error handler.
 * @details Part of the TypeChecker pass (Issue #7), split from the original
 *          monolithic TypeChecker.cpp by category (mirrors AST.h's
 *          ASTExpr/ASTStmt/ASTDecl/ASTAccess split -- see
 *          docs/planning/COMPILER_MODULE_BREAKDOWN.md).
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

#ifndef TYPECHECKER_NO_ERRORS
/// Default error handler: uses the real TypeError class from Errors.h.
void productionErrorHandler(SourceLocation Loc, const std::string &Msg,
                            const std::vector<RelatedLocation> &Related,
                            const std::string &CustomHint,
                            const std::string &CustomBanner) {
  TypeError Err(Loc, Msg, CustomHint, CustomBanner);
  for (const auto &R : Related)
    Err.withRelated(R.Loc, R.Description);
  Err.raise();
}
#else
/// Stub used when TYPECHECKER_NO_ERRORS is defined (unit-test builds).
/// Throws std::runtime_error so tests can catch it with EXPECT_THROWS.
void unitTestErrorHandler(SourceLocation, const std::string &Msg,
                          const std::vector<RelatedLocation> &,
                          const std::string &, const std::string &) {
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
                              const std::vector<RelatedLocation> &Related,
                              const std::string &CustomHint,
                              const std::string &CustomBanner) {
  HadError_ = true;
  if (ErrorHandler_)
    ErrorHandler_(Loc, Msg, Related, CustomHint, CustomBanner);
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
    // (docs/planning/Errors.md): `err`'s type is AnyErrorType (its real
    // concrete error type isn't knowable at compile time -- `!T` doesn't
    // restrict which declared error type a call can fail with), so the
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
