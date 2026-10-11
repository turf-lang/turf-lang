/**
 * @file TypeCheckerStmt.cpp
 * @brief Control-flow statement type checking: block, if/while/do-while/for/for-each, return.
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
        reportError(Node.getLoc(), ErrCode::TypeMismatch,
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


void TypeChecker::visitDoWhile(DoWhileExprAST &Node) {
  if (Node.getBody())
    Node.getBody()->accept(*this);
  if (Node.getCond())
    const_cast<ExprAST *>(Node.getCond())->accept(*this);
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


void TypeChecker::visitForEach(ForEachExprAST &Node) {
  if (Node.getIterable())
    const_cast<ExprAST *>(Node.getIterable())->accept(*this);

  TurfTypePtr IterableTy = getType(Node.getIterable());

  if (!IterableTy || !isArrayType(IterableTy.get())) {
    ForEachNonArrayError(Node.getLoc(),
                         IterableTy ? IterableTy->getName() : "unknown")
        .raise();
    setType(&Node, TurfVoid());
    return;
  }

  // A `T[]` parameter is accepted like a `T[N]`: since issue #260 it is a
  // slice that carries its length at run time (E006, which refused it, is
  // retired).
  const auto *ArrTy = dynamic_cast<const ArrayType *>(IterableTy.get());
  TurfTypePtr ElemTy = ArrTy->getElementTypePtr();

  // If the element type is a NamedType (e.g. "User"), resolve it to the
  // actual TurfStructType/enum via the symbol table -- same "Issue #17 fix"
  // visitArrayAccess() already applies. Without this, `for user in users {
  // user.name }` would set the value var's type to a bare NamedType("User")
  // and the field access inside the body would fail to type-check.
  if (ElemTy) {
    if (const auto *NT = dynamic_cast<const NamedType *>(ElemTy.get())) {
      if (GlobalSymbolTable) {
        if (auto Resolved = GlobalSymbolTable->LookupType(NT->getName()))
          ElemTy = Resolved;
      }
    }
  }

  // Same reasoning as visitFor(): LoopVarTypes_ is TypeChecker-private
  // bookkeeping that only affects visitVariable()'s answer, but Codegen's
  // independent getExprType() reads ResolvedSymbol->Type directly -- so
  // both need to be updated, not just one.
  LoopVarTypes_[Node.getValueVarName()] = ElemTy;
  if (Node.ResolvedValueVar)
    Node.ResolvedValueVar->Type = ElemTy;
  if (Node.hasIndexVar() && Node.ResolvedIndexVar)
    Node.ResolvedIndexVar->Type = TurfInt();

  if (Node.getBody())
    Node.getBody()->accept(*this);

  LoopVarTypes_.erase(Node.getValueVarName());

  setType(&Node, TurfVoid());
}


void TypeChecker::visitBreak(BreakExprAST &Node) {
  setType(&Node, TurfVoid());
}


void TypeChecker::visitContinue(ContinueExprAST &Node) {
  setType(&Node, TurfVoid());
}


// True if `T` is (or, if an unresolved NamedType, names) a declared `error`
// type (docs/architecture/errors_and_recover.md). A bare `error` variant
// reference (TypeChecker::visitScopeAccess) already resolves directly to the
// real TurfUserErrorType, but a CONSTRUCTED variant
// (`IOError::FileNotFound(...)`) gets its type from the Resolver's synthetic
// candidate, which -- like StructName::new(...) -- deliberately returns a bare
// NamedType(LeftName) (see Resolver::visitFuncCall's struct-constructor branch
// for why: the exact-getName()-match fallback in typesCompatible() already
// makes NamedType("MathError") interchangeable with the real TurfUserErrorType
// everywhere else, so nothing but THIS specific check -- "is this actually
// an error type at all," which name-equality alone can't answer -- needs the
// real lookup).
static bool isDeclaredErrorTypeValue(const TurfType *T) {
  if (!T)
    return false;
  if (T->isUserError())
    return true;
  // AnyErrorType (docs/architecture/errors_and_recover.md) -- the type `err` is
  // bound to inside `recover as err run { ... }` (Resolver::visitRecoverExpr,
  // `SymTable_.declareVariable(Node.getBoundName(), TurfAnyError(), ...)`).
  // Without this, propagating a caught error straight out of an outer
  // `!T`-returning function (`inner() recover as err run { return err; }`)
  // was flatly rejected: `err`'s erased AnyErrorType never satisfied
  // `T->isUserError()` above (a DIFFERENT flag -- see TurfType::isAnyError()'s
  // own doc comment in Types.h for why the two are deliberately distinct),
  // so visitReturn's Compatible check failed no matter what the outer
  // function's success type was, always reporting a confusing "function
  // declares '!T' but returns '<error>'" (AnyErrorType's own getName()).
  // Confirmed live with the smallest possible repro (`fn inner(): !int {
  // return MyError::Bad; } fn outer(): !int { immut x: int = inner()
  // recover as err run { return err; }; return x; }`) before this fix --
  // this is a general gap in the core `!T` propagation idiom, not specific
  // to any one caller. Codegen already treats AnyErrorType and a concrete
  // user-error type identically (both are just the tag string, see
  // getLLVMType()'s `T->isUserError() || T->isAnyError()` branch), so
  // accepting it here doesn't newly permit anything Codegen can't already
  // handle correctly.
  if (T->isAnyError())
    return true;
  if (const auto *NT = dynamic_cast<const NamedType *>(T)) {
    if (GlobalSymbolTable) {
      if (auto Registered = GlobalSymbolTable->LookupType(NT->getName()))
        return Registered->isUserError();
    }
  }
  return false;
}


void TypeChecker::visitReturn(ReturnExprAST &Node) {
  if (Node.getVal())
    const_cast<ExprAST *>(Node.getVal())->accept(*this);

  TurfTypePtr RetTy = getType(Node.getVal());

  // Check against the enclosing function's declared return type
  if (!ReturnTypeStack_.empty() && ReturnTypeStack_.back()) {
    const TurfType *Declared = ReturnTypeStack_.back().get();
    if (RetTy) {
      bool Compatible;
      // `!T` (docs/architecture/errors_and_recover.md): a function declared to
      // return `!T` may return either something compatible with T, OR a value
      // of ANY declared error type -- `!T` is an open/unenumerated union,
      // unlike e.g. Rust's `Result<T, E>`, so there's no specific error type to
      // check RetTy against here; only "is this some declared error value
      // at all" matters.
      // OVH-005: an untyped literal (`return 5;` for a declared 'int32'
      // success type) acquires 'int32' directly rather than being rejected
      // as an 'int -> int32' narrowing -- see
      // untypedLiteralAcquiresInt32()'s own doc comment.
      if (Declared->isErrorUnion()) {
        const auto *EU = static_cast<const TurfErrorUnionType *>(Declared);
        Compatible = typesCompatible(RetTy.get(), EU->getSuccessType()) ||
                     isDeclaredErrorTypeValue(RetTy.get()) ||
                     untypedLiteralAcquiresInt32(Node.getVal(), EU->getSuccessType());
        // LEC-004 (FREEZE.md §6.18): `return f();` where `f` returns exactly
        // the `!T` this function declares hands the fallible value back as it
        // is -- success or error, never unwrapped. Before, it was rejected
        // with "declares '!Rec' but returns '!Rec'" (audit F-21): the success
        // type was compared with the operand's WHOLE union. Recorded on the
        // node because Codegen cannot tell this from `return g(x);` by shape.
        if (!Compatible && RetTy->isErrorUnion() &&
            typesCompatible(RetTy.get(), Declared)) {
          Compatible = true;
          Node.PassesThroughFallible = true;
        }
      } else {
        Compatible = typesCompatible(RetTy.get(), Declared) ||
                     untypedLiteralAcquiresInt32(Node.getVal(), Declared);
      }

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
        // A fallible value returned where it cannot be passed through is the
        // commonest way to reach this, and the plain message gives no way out
        // of it (audit F-21), so name the two that exist.
        if (RetTy->isErrorUnion()) {
          // Already fallible, but of a different success type: the value can
          // only be passed through as exactly the `!T` this function returns,
          // so the way out is to take the success value out first.
          const std::string Hint =
              Declared->isErrorUnion()
                  ? "A `return f();` passes a fallible value through only when it "
                    "is exactly this function's '" + Declared->getName() +
                        "'; nothing converts a '" + RetTy->getName() + "' into it. "
                        "Take the value out first with `try f()` (an error still "
                        "leaves this function) or `recover`, then return a '" +
                        static_cast<const TurfErrorUnionType *>(Declared)
                            ->getSuccessType()->getName() + "'."
                  : "That value can fail. Handle the failure here with `recover`, "
                    "or declare this function '" + RetTy->getName() +
                        "' so it can pass the error on -- then `return f();` or "
                        "`try f()` both work.";
          reportError(Node.getLoc(), ErrCode::ReturnTypeMismatch,
                      "Return type mismatch: function declares '" +
                          Declared->getName() + "' but returns '" +
                          RetTy->getName() + "'.",
                      {}, Hint);
        } else {
          // The types of the identifiers in the returned expression, for the
          // SLM (see visitVarDecl()).
          SLM::DiagnosticHints Hints;
          addIdentifierTypes(Node.getVal(), Hints);
          reportError(Node.getLoc(),
                      codeForMismatch(RetTy.get(), nullptr,
                                      ErrCode::ReturnTypeMismatch),
                      "Return type mismatch: function declares '" +
                          Declared->getName() + "' but returns '" +
                          RetTy->getName() + "'.",
                      {}, hintForMismatch(RetTy.get(), nullptr, ""), "", Hints);
        }
      } else {
        // SEM-002 -- see visitVarDecl()'s identical comment
        // (TypeCheckerDecl.cpp). For a '!T'-declared function, the
        // relevant destination is the success type T, not the error union
        // itself (an error-tag value never has a NumberExprAST shape
        // anyway, so this is a no-op on that path regardless).
        const TurfType *EffectiveDest =
            Declared->isErrorUnion()
                ? static_cast<const TurfErrorUnionType *>(Declared)->getSuccessType()
                : Declared;
        checkLiteralRepresentable(Node.getVal(), EffectiveDest, Node.getLoc());
      }
    }
  }

  // A bare `return;` hands back nothing, so it is only for a function that
  // returns nothing (`void`, or `!void`). Checked above only when there was a
  // value, so `fn main(): int { return; }` reached Codegen, which emitted
  // `ret void` in an i64 function -- "Internal compiler error: generated LLVM
  // IR is invalid" from one missing expression (REL-004 fuzzer).
  if (!Node.getVal() && !ReturnTypeStack_.empty() && ReturnTypeStack_.back()) {
    const TurfType *Declared = ReturnTypeStack_.back().get();
    const TurfType *Effective =
        Declared->isErrorUnion()
            ? static_cast<const TurfErrorUnionType *>(Declared)->getSuccessType()
            : Declared;
    if (Effective && !Effective->isVoid()) {
      reportError(Node.getLoc(), ErrCode::ReturnTypeMismatch,
                  "Return type mismatch: function declares '" + Declared->getName() +
                      "' but this 'return' gives back no value.",
                  {}, "Write the value to return, e.g. `return 0;`, or declare the function as returning void.");
    }
  }

  setType(&Node, RetTy ? RetTy : TurfVoid());
}

void TypeChecker::visitDefer(DeferExprAST &Node) {
  // Type-check the wrapped statement exactly as if it appeared directly in
  // the enclosing block -- a `defer` is a lowering/scheduling detail, not a
  // new typing context (LANG-003, ARCHITECTURE_OVERHAUL.md §8). Without
  // this override, ASTVisitor's default no-op visitDefer() would mean a
  // deferred statement's own expressions never went through the TypeChecker
  // at all (every visit*() call is a DIRECT `Node->accept(*this)` from the
  // parent, not an implicit recursive walk -- see visitBlock() just above).
  if (Node.getStmt())
    const_cast<ExprAST *>(Node.getStmt())->accept(*this);

  // A `defer` statement is always used in statement position (parsed via
  // the same generic ParseExpression()-then-';' path as any other
  // statement, ParserStmt.cpp's ParseDeferStmt()) -- its own "value" is
  // never read by anything, same as return/break/continue alongside it.
  setType(&Node, TurfVoid());
}
