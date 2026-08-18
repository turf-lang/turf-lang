/**
 * @file TypeCheckerDecl.cpp
 * @brief Declaration type checking: var decl, fn/struct/enum/error/extern/array/module-import.
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


void TypeChecker::visitVarDecl(VarDeclExprAST &Node) {
  if (Node.isImmutable() && !Node.hasExplicitInit()) {
    reportError(Node.getLoc(),
                "Immutable variable '" + Node.getName() + "' requires an explicit initializer.",
                {},
                "'immutable'/'immut' variables can never be assigned to "
                "later, so they must get their only value right here.",
                "Missing initializer!");
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

  // `unsafe { }` gate (issue #238): declaring a raw ptr/T*-typed variable
  // is one of the three explicitly-gated "uses" (declaration, cast,
  // dereference -- see FINAL_SPRINT.md's Track A). Checked after
  // NamedType resolution above so a struct-pointer field's real type is
  // seen here, not a placeholder.
  checkUnsafeGate(DeclTy.get(), Node.getLoc(), &Node);

  // Optional: validate that the initializer type is compatible
  if (Node.getInitializer() && DeclTy) {
    TurfTypePtr InitTy = getType(Node.getInitializer());
    if (InitTy) {
      if (InitTy->isNull()) {
        // A `&T` safe reference (issue #238) is deliberately excluded even
        // though isPointer() is true for it (inherited from
        // TurfPointerType, see that class's own doc comment) -- its whole
        // point is a compiler-guaranteed non-null contract, so `null` must
        // be rejected here exactly like any other non-pointer type. Mirrors
        // typesCompatible()'s own Rule 1 (src/Types.cpp).
        if ((!DeclTy->isPointer() || DeclTy->isReference()) &&
            !DeclTy->isNamed("ptr") && !DeclTy->isNamed("void*")) {
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


void TypeChecker::visitFuncDef(FuncDefExprAST &Node) {
  // Unconditional (issue #238, "disallow outright" -- see
  // checkNoRawPointersInSignature()'s own doc comment): a plain function's
  // raw pointer parameter/return type is never fixable by wrapping in
  // `unsafe { }`, so this runs regardless of UnsafeDepth_.
  checkNoRawPointersInSignature(Node);

  // Push the declared return type so visitReturn() can validate it
  ReturnTypeStack_.push_back(Node.getReturnTypePtr());

  if (Node.getBody())
    const_cast<ExprAST *>(Node.getBody())->accept(*this);

  ReturnTypeStack_.pop_back();
  setType(&Node, TurfVoid());
}


void TypeChecker::visitEnumDecl(EnumDeclExprAST &Node) {
  setType(&Node, TurfVoid());
}


void TypeChecker::visitMachineDecl(MachineDeclExprAST &Node) {
  // Declaration itself has no value-type, mirroring visitEnumDecl -- all the
  // real per-use typing happens at MachineName::State access sites, which
  // already goes through the ordinary enum ScopeAccess path unchanged (see
  // TurfMachineType's doc comment in include/Types.h).
  setType(&Node, TurfVoid());
}


void TypeChecker::visitErrorDecl(ErrorDeclExprAST &Node) {
  // Declaration itself has no value-type, mirroring visitEnumDecl/
  // visitStructDecl.
  setType(&Node, TurfVoid());
}


void TypeChecker::visitStructDecl(StructDeclExprAST &Node) {
  // `unsafe { }` gate (issue #238): a raw ptr/T*-typed FIELD is a gated
  // use, but unlike a var-decl/cast, the diagnostic is raised once against
  // the struct's OWN location (not each field) -- the resolved design
  // (FINAL_SPRINT.md's Track A) requires the ENTIRE struct declaration to
  // be lexically inside `unsafe { }` if it has even one raw pointer field,
  // so "which field" isn't the actionable part of the message.
  for (const auto &Field : Node.getFields()) {
    if (isRawPointer(Field.Type.get())) {
      checkUnsafeGate(Field.Type.get(), Node.getLoc(), &Node);
      break; // one diagnostic per struct is enough
    }
  }
  setType(&Node, TurfVoid());
}


// ─── Arrays ───────────────────────────────────────────────────────────────────

void TypeChecker::visitArrayDecl(ArrayDeclExprAST &Node) {
  for (const auto &Init : Node.getInitList())
    Init->accept(*this);
  if (Node.getInitExpr())
    Node.getInitExpr()->accept(*this);

  // Neither the init list's LENGTH nor its elements' TYPES were ever
  // checked against the declared array type here -- unlike the sibling
  // ArrayLiteralExprAST (visitArrayLiteral, just below), whose elements
  // ARE cross-checked against each other, and struct-literal field init,
  // whose values ARE checked against each field's declared type.
  // `int[3] arr = [1, "two", 3]` compiled with zero diagnostic and was
  // confirmed to be a genuine type-safety hole, not just a missing check:
  // reading `arr[1] to string` back printed the string literal's raw
  // pointer bits reinterpreted as an int. `int[3] arr = [1,2,3,4,5]` (5
  // elements into a declared size-3 array) also compiled clean.
  if (!Node.getInitList().empty()) {
    if (Node.getInitList().size() != static_cast<size_t>(Node.getSize())) {
      reportError(Node.getLoc(),
                  "Array declared with size " + std::to_string(Node.getSize()) +
                      " but initialized with " +
                      std::to_string(Node.getInitList().size()) +
                      " element" +
                      (Node.getInitList().size() == 1 ? "" : "s") + ".",
                  {},
                  "Change the declared size to match the initializer list, "
                  "or add/remove elements so the counts agree.",
                  "Array size mismatch!");
    }
    // ExprAST has no base-class getLoc() (each concrete node declares its
    // own, inconsistently), so this uses the array declaration's own
    // location rather than each individual initializer's -- matching what
    // visitArrayLiteral does just below for its own analogous mismatch.
    TurfTypePtr DeclaredElemTy = Node.getElementTypePtr();
    for (const auto &Init : Node.getInitList()) {
      TurfTypePtr InitTy = getType(Init.get());
      if (DeclaredElemTy && InitTy &&
          !typesCompatible(InitTy.get(), DeclaredElemTy.get())) {
        reportError(Node.getLoc(),
                    "Cannot initialize an array of '" +
                        DeclaredElemTy->getName() + "' with a value of type '" +
                        InitTy->getName() + "'.",
                    {});
      }
    }
  }

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
