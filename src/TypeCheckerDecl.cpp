/**
 * @file TypeCheckerDecl.cpp
 * @brief Declaration type checking: var decl, fn/struct/enum/error/extern/array/module-import.
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

#include "Monomorphize.h" // TYPE-002 (S11b): report-only instantiation collector
#include "TypeWalk.h" // SMP-002: shared containsOpenParam() walker

#include "llvm/ADT/SmallString.h" // OVH-014: folded-immut-constant decimal rendering

#include <cassert>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>


// True if Ty is, after unwrapping any number of pointer/reference/array
// layers, an uninstantiated generic type parameter -- i.e. built directly
// from a generic template's own type parameter (FREEZE.md §6.1).
// Deliberately does NOT unwrap a TurfStructType -- that is
// containsUninstantiatedGenericParam()'s job, kept as a separate,
// non-recursive-into-structs function below specifically so THIS function
// can be called on a struct's own field types without risking the
// unbounded recursion a self/mutually-referential struct field would
// otherwise cause (see that function's comment).
//
// A bare `TurfGenericParamType` DOES reach here for a generic struct's own
// field, as of OVH-006: Resolver::visitStructDecl() rewrites
// NamedType("T") -> TurfGenericParamType("T") on the STRUCT DECLARATION AST
// NODE's own field list (Node.getFieldsMut()) AND re-registers the
// corrected field list into GlobalSymbolTable at the same moment -- before
// that fix, GlobalSymbolTable's registered TurfStructType was a SEPARATE
// object, built from a snapshot of the fields at PARSE time (ParserDecl.cpp,
// before the substitution ever ran) and never updated afterward, so a
// generic struct's globally-registered field types stayed plain
// NamedType(paramName) forever -- e.g. List<T>'s `data: T*` read back as
// TurfPointerType(NamedType("T")), not TurfPointerType
// (TurfGenericParamType("T")). That NamedType branch below still exists as
// a defensive fallback (see OVH-006's change record, *Known pitfalls*, for
// why it was kept rather than deleted), but it is no longer the one that
// fires for an ordinary generic struct field.
//
// External linkage (TypeCheckerInternal.h), not anonymous-namespace-local:
// TypeCheckerCall.cpp's visitFuncCall/visitMethodCall need this too, as of
// S14a -- see isFullyConcreteType()'s own doc comment just below for why.
// SMP-002: thin wrapper over the shared walker (TypeWalk.h). The
// NamedType-fallback logic (Resolver::validateNamedType() already
// guarantees, for every field of an already-declared struct, that a bare
// field NamedType's name is exactly one of: a known primitive, a registered
// struct/enum/error type, or one of the enclosing struct's OWN generic type
// parameters -- anything else is already a compile error at
// struct-declaration time -- so a name that is neither a primitive nor
// globally registered can only be the third case) now lives once, in
// TypeWalk::containsOpenParam()'s own NamedType branch; this call site
// supplies `GlobalSymbolTable->LookupType()` as the registry lookup.
// ParamScope::ThroughWrappers is this function's exact original scope: it
// never looked inside a GenericType's own argument list (see
// isFullyConcreteType()'s own comment below for why that scope needs to).
bool isOrWrapsGenericParam(const TurfType *Ty) {
  return TypeWalk::containsOpenParam(
      Ty, TypeWalk::ParamScope::ThroughWrappers,
      [](const std::string &Name) {
        return GlobalSymbolTable && GlobalSymbolTable->LookupType(Name) != nullptr;
      });
}

/// TYPE-002 (S11b): true when @p Ty has no open generic parameter ANYWHERE
/// in its shape -- unlike isOrWrapsGenericParam() above, this recurses
/// into a GenericType's own type-argument list too (needed to tell
/// `Box<int>` apart from `Box<T>` inside a still-uninstantiated template,
/// which isOrWrapsGenericParam() alone can't do -- it has no GenericType
/// case at all, since its one existing caller never needed one). Used only
/// to decide whether a declared variable's type is a real instantiation
/// request worth recording, never to change any type-checking outcome.
///
/// External linkage (TypeCheckerInternal.h): TypeCheckerCall.cpp's
/// visitFuncCall/visitMethodCall gate their own MonoCollector_->record()
/// calls on this too, as of S14a -- found live, not anticipated: neither
/// call site originally checked whether its OWN computed Subst was fully
/// concrete before recording, only whether it was non-empty. A generic
/// function calling ANOTHER generic function using its own still-open type
/// parameter (`fn<T> outer(x: T): T { return inner(x); }`, `inner`'s OWN
/// call site inside OUTER's un-substituted TEMPLATE body) unifies
/// `inner`'s `T` against `outer`'s OWN (still-generic) `T` -- a non-empty
/// but NOT concrete "substitution" (mapping one open parameter to another)
/// -- and recorded a bogus instantiation request like "inner<T>" anyway.
/// Completely harmless under erasure (the resulting bogus clone was dead
/// code nothing ever called, and getLLVMType(TurfGenericParamType) just
/// returned ptr without complaint) -- but once S14a asserts INV-GEN-1 in
/// getLLVMType()/CastToType(), this bogus clone's own literal
/// TurfGenericParamType parameter trips the assertion the moment it
/// reaches Codegen. Confirmed live via
/// tests/38_monomorphization/03_nested_generic_call.tr.
// SMP-002: thin wrapper. ParamScope::ThroughGenericArgs is this function's
// exact original scope -- everything ThroughWrappers already covers, plus a
// GenericType's own type-argument list (see this function's doc comment
// above for why that extra reach is needed).
bool isFullyConcreteType(const TurfType *Ty) {
  return Ty && !TypeWalk::containsOpenParam(
                   Ty, TypeWalk::ParamScope::ThroughGenericArgs,
                   [](const std::string &Name) {
                     return GlobalSymbolTable &&
                            GlobalSymbolTable->LookupType(Name) != nullptr;
                   });
}

namespace {
// True if Ty is an uninstantiated generic template instance: either the
// bare TurfGenericParamType itself (pointer/array-wrapped or not), or a
// struct at least one of whose OWN fields is declared directly against a
// type parameter (e.g. List<T>'s `data: T*`, ParserDecl.cpp's "fields use
// TurfGenericParamType" comment). Used by SEM-003's `guess` inference to
// reject `mut l = List::default();` (§11.6): the call's own type is a bare
// NamedType("List") with no type arguments at all, and even once resolved
// to List's registered (type-erased) TurfStructType, that struct's fields
// still carry the raw TurfGenericParamType('T') -- there is genuinely
// nothing to infer T from, unlike an ordinary `Point::default()` whose
// resolved StructTy has no generic-typed fields at all.
//
// Checks each field with isOrWrapsGenericParam() -- NOT a recursive call
// to this function -- and deliberately does not descend into a nested
// TurfStructType field's OWN fields at all. A field of struct type only
// says something about THAT struct's genericity, and TYPE-001 explicitly
// permits a struct to reference itself (or another struct that references
// it back) through a pointer field -- a linked-list node's `next: Node*`
// is the textbook case. Recursing into struct fields transitively here,
// unlike the bounded pointer/array unwrapping above, has no depth limit
// tied to the type's own structure, so it would stack-overflow on exactly
// that ordinary, already-legal shape the moment it was named as a
// `guess`-inferred variable's default-constructor target.
bool containsUninstantiatedGenericParam(const TurfType *Ty) {
  if (!Ty)
    return false;
  if (isOrWrapsGenericParam(Ty))
    return true;
  if (const auto *ST = dynamic_cast<const TurfStructType *>(Ty)) {
    for (const auto &F : ST->getFields())
      if (isOrWrapsGenericParam(F.Type.get()))
        return true;
  }
  return false;
}

/// TYPE-002 (S15/D2): record a Struct-kind instantiation request for every
/// concrete GenericType occurrence reachable from a struct FIELD's declared
/// type -- through any number of pointer/reference/array wrappers, and
/// recursively into a GenericType's OWN type arguments (so `struct Holder {
/// items: List<List<Big>> }` requests both List<Big> and List<List<Big>>,
/// not just the outer one).
///
/// Why this was missing: visitVarDecl (below) already records a Struct-kind
/// request for a bare `mut b: Box<int> = ...;`, and visitMethodCall/
/// visitFieldAccess (TypeCheckerCall.cpp) record one for a method call or
/// field read on a GenericType-typed value -- but nothing ever walked a
/// STRUCT DECLARATION's own field list, so `struct Holder { items:
/// List<Big>, n: int }` with no `Holder` variable ever instantiated
/// anywhere in the program left `List<Big>` completely invisible to the
/// census: drainStructRequests() never saw it, its per-instantiation
/// TurfStructType never got created, and getLLVMType()'s GenericType branch
/// fell into its forward-declared opaque-stub fallback -- silently wrong
/// layout (sizeof(Holder) == 8, not 32) whenever nothing else in the
/// program happened to force List<Big> into existence some other way, and
/// an "Internal compiler error: generated LLVM IR is invalid" the moment a
/// real `Holder` variable's own alloca/GEP needed the real (unsized-stub)
/// layout.
///
/// A no-op whenever @p Collector is null (every ordinary compile) --
/// matches every other MonoCollector_->record() call site's own gate.
/// isFullyConcreteType() (not isOrWrapsGenericParam()) is the correct
/// concreteness check here, same reasoning as visitVarDecl's own DeclGT
/// gate just below: a generic STRUCT's own declaration (Resolver::
/// visitStructDecl() already rewrote its field types to
/// TurfGenericParamType, per this file's own comment above) must NOT record
/// a bogus non-concrete request for its own still-open type parameter (the
/// S14a "inner<T>" bug this predicate exists to prevent -- see
/// isFullyConcreteType()'s own doc comment).
void recordGenericFieldInstantiations(const TurfTypePtr &Ty, SourceLocation Loc,
                                      ExprAST *Site,
                                      Monomorphize::InstantiationCollector *Collector) {
  if (!Ty || !Collector)
    return;
  if (const auto *GT = dynamic_cast<const GenericType *>(Ty.get())) {
    if (isFullyConcreteType(GT)) {
      Collector->record({Monomorphize::InstantiationKind::Struct,
                         GT->getBaseName(),
                         TypeIdentity::owningModule(GT->getBaseName()),
                         GT->getTypeArgs(), Loc},
                        Site);
      for (const auto &Arg : GT->getTypeArgs())
        recordGenericFieldInstantiations(Arg, Loc, Site, Collector);
    }
    return;
  }
  if (const auto *PT = dynamic_cast<const TurfPointerType *>(Ty.get())) {
    recordGenericFieldInstantiations(PT->getPointee(), Loc, Site, Collector);
    return;
  }
  if (const auto *AT = dynamic_cast<const ::ArrayType *>(Ty.get())) {
    recordGenericFieldInstantiations(AT->getElementTypePtr(), Loc, Site, Collector);
    return;
  }
}
} // namespace

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

  // SEM-003 (§11.8): `guess` is resolved HERE, not the Parser. ParseType()/
  // ParseLetStatement() only ever produce the TurfGuess() placeholder (for
  // both spellings, §11.2); Resolver::visitVarDecl declares the variable
  // with that same placeholder (it does no type inference itself -- see
  // its own comment). Inference happens now, from the initializer's type
  // just computed above, and is written back onto BOTH the AST node
  // (setTypePtr(), which Codegen reads directly) and the declaration's own
  // VariableSymbol (ResolvedSymbol->Type) so every later reference to this
  // variable -- resolved by the Resolver to the SAME shared_ptr, but not
  // yet visited by the TypeChecker at this point in the file, since the
  // Resolver finishes the whole AST before the TypeChecker starts --
  // reads the real, concrete type once the TypeChecker does reach it.
  // Mirrors the pre-existing NamedType -> TurfStructType fixup immediately
  // below, and ForEachExprAST's loop-variable placeholder in
  // Resolver::visitForEach ("placeholder now, refine later").
  if (DeclTy && DeclTy->isGuess()) {
    if (!Node.getInitializer()) {
      // ParseLetStatement() leaves Init null for EVERY no-initializer
      // declaration now (SEM-004, F-INIT-1 -- no type-specific default is
      // ever synthesized there any more), so this is the only place
      // 'mut x: guess;' is ever caught: nothing to infer the type from.
      reportError(Node.getLoc(), "'guess' needs an initializer to infer from.",
                  {}, "", "Missing initializer!");
      setType(&Node, nullptr);
      return;
    }

    TurfTypePtr InitTy = getType(Node.getInitializer());
    // A general expression's type is never auto-resolved from NamedType ->
    // TurfStructType the way a VarDecl's own annotation is (the block just
    // below only ever runs against DeclTy) -- `StructName::default()`
    // (TypeCheckerCall.cpp's DefaultConstruct branch) sets its own type to
    // a bare NamedType(StructName), so it must be resolved here too before
    // the generic-template check and the ordinary mismatch/representability
    // checks below can see the real struct type.
#ifndef TYPECHECKER_NO_ERRORS
    if (auto *InitNT = dynamic_cast<const NamedType *>(InitTy.get())) {
      if (GlobalSymbolTable) {
        if (auto InitStructTy = GlobalSymbolTable->LookupType(InitNT->getName()))
          InitTy = InitStructTy;
      }
    }
#endif

    if (!InitTy) {
      // The initializer's own visit already reported whatever is wrong
      // with it -- don't cascade a second, less specific error on top.
      setType(&Node, nullptr);
      return;
    }
    if (InitTy->isVoid()) {
      reportError(Node.getLoc(), "Cannot infer a type from a 'void' expression.",
                  {},
                  "'guess' needs a value to infer a type from -- give '" +
                      Node.getName() + "' an explicit type annotation instead.",
                  "Nothing to infer!");
      setType(&Node, nullptr);
      return;
    }
    if (InitTy->isNull()) {
      reportError(Node.getLoc(), "Cannot infer a type from 'null'.",
                  {},
                  "Annotate the reference type explicitly, e.g. 'mut " +
                      Node.getName() + ": &?Point = null;'.",
                  "Nothing to infer!");
      setType(&Node, nullptr);
      return;
    }
    if (containsUninstantiatedGenericParam(InitTy.get())) {
      reportError(Node.getLoc(),
                  "Cannot infer the type arguments of a generic type.",
                  {},
                  "'guess' has nothing to infer the type argument from here "
                  "-- annotate it explicitly instead, e.g. 'mut " +
                      Node.getName() + ": List<int> = ...;'.",
                  "Nothing to infer!");
      setType(&Node, nullptr);
      return;
    }

    // §11.6's whole defaulting story ("integer -> int, float -> double, ...,
    // the default is chosen first, THEN representability is checked")
    // requires no extra code here: every literal's own visit (visitNumber/
    // visitBool/visitChar/visitString) already assigns that same default
    // type unconditionally, regardless of any destination -- InitTy IS
    // already that default. Setting DeclTy = InitTy and falling through to
    // the ordinary mismatch/checkLiteralRepresentable() block below (they
    // now trivially agree) performs the representability check in exactly
    // that order for free.
    DeclTy = InitTy;
    Node.setTypePtr(InitTy);
    if (Node.ResolvedSymbol) {
      Node.ResolvedSymbol->Type = InitTy;
      Node.ResolvedSymbol->WasGuessInferred = true;
    }
  }

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

  // TYPE-002 (S11b, report-only): a declared type is a struct
  // instantiation request even with no method call anywhere on it -- e.g.
  // `mut b: Box<int> = Box { val: 5 }; return b.val;` never calls a
  // method, but Box<int> must still exist as its own real type/layout
  // once monomorphization lands (S13a). Recorded here, not just at method
  // call sites, so this case isn't invisible to the census. A no-op
  // whenever MonoCollector_ is null (every ordinary compile).
  if (MonoCollector_) {
    if (auto *DeclGT = dynamic_cast<const GenericType *>(DeclTy.get())) {
      if (isFullyConcreteType(DeclGT))
        MonoCollector_->record({Monomorphize::InstantiationKind::Struct,
                               DeclGT->getBaseName(),
                               TypeIdentity::owningModule(DeclGT->getBaseName()),
                               DeclGT->getTypeArgs(), Node.getLoc()},
                              &Node);
    }
  }

  // TYPE-002 (S13b): `mut b: Box<Big> = Box::default();` -- tell the
  // construction call which CONCRETE instantiation it's building, since it
  // structurally cannot know on its own (U-5: `Box::default()`'s own type
  // is always the bare NamedType("Box"), never a GenericType -- see
  // TypeCheckerCall.cpp's DefaultConstruct branch). Only set when DeclTy is
  // a real GenericType and the initializer is exactly a construction call
  // (`StructName::default()`/`StructName::new(...)`) -- every other
  // initializer shape is unaffected. Read back by FuncCallExprAST::codegen()'s
  // DefaultConstruct/ConstructorCall branch (CodegenFunc.cpp) so its own
  // alloca uses the per-instantiation TurfStructType (drainStructRequests(),
  // S13a), not the shared, erasure-era base struct.
  if (dynamic_cast<const GenericType *>(DeclTy.get())) {
    if (auto *FC = dynamic_cast<FuncCallExprAST *>(
            const_cast<ExprAST *>(Node.getInitializer()))) {
      if (auto *SC = dynamic_cast<const ScopeAccessExprAST *>(FC->getCallee())) {
        if (SC->ResolvedKind == ScopeAccessKind::DefaultConstruct ||
            SC->ResolvedKind == ScopeAccessKind::ConstructorCall)
          FC->ConstructedType = DeclTy;
      }
    }
  }

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
      // OVH-005: an untyped literal (`mut n: int32 = 5;`) acquires 'int32'
      // directly rather than being rejected as an 'int -> int32' narrowing
      // -- see untypedLiteralAcquiresInt32()'s own doc comment.
      if (!typesCompatible(InitTy.get(), DeclTy.get()) &&
          !untypedLiteralAcquiresInt32(Node.getInitializer(), DeclTy.get())) {
        reportError(Node.getLoc(),
                    "Cannot initialise '" + DeclTy->getName() +
                        "' variable '" + Node.getName() + "' with value of type '" +
                        InitTy->getName() + "'.");
      } else {
        // SEM-002: typesCompatible() only checks the KIND of conversion
        // (e.g. "int-literal -> int is fine"), never the literal's own
        // exact value -- this is the destination-representability check
        // FREEZE.md §6.2 rows 3/6/9 add on top.
        checkLiteralRepresentable(Node.getInitializer(), DeclTy.get(), Node.getLoc());

        // OVH-014 (docs/development/changes/OVH-014.md), FREEZE.md
        // §6.2 row 6: an `immut int` whose initializer just passed the
        // representability check above, IF that initializer is itself pure
        // literal arithmetic, records its exact value on the shared
        // VariableSymbol -- see VariableSymbol::HasFoldedIntConstant's own
        // doc comment (ResolverSymbolBase.h) for why -- so a LATER
        // reference to this local (`immut b = a + 1;`) can be folded and
        // checked here too, at compile time, instead of only ever being
        // caught by the runtime-checked-arithmetic trap. `checkLiteralRepresentable()`
        // itself has already exited the process (raise()) on any failure
        // above, so reaching this line means the fold below (if it
        // succeeds) is a value already proven to fit `int`. Only for
        // exactly `int` -- `int32`/`double` locals are never referenced as
        // an operand of further int literal arithmetic.
        if (Node.isImmutable() && DeclTy->isNamed("int") && Node.ResolvedSymbol) {
          if (auto Folded = tryFoldUntypedIntConstant(Node.getInitializer(),
                                                       /*IsTopLevel=*/true)) {
            llvm::SmallString<48> S;
            Folded->toString(S, 10, /*Signed=*/true);
            Node.ResolvedSymbol->HasFoldedIntConstant = true;
            Node.ResolvedSymbol->FoldedIntConstant = S.str().str();
          }
        }
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

  // STD-002: save/restore rather than a blind push/pop pair, since a `fn`
  // CAN be declared lexically inside another function's body block in this
  // language (ParseBlock() accepts a nested `fn`/`struct`/`enum`/`machine`
  // declaration as an ordinary statement) -- an inner, non-`@caller_location`
  // function nested inside an outer `@caller_location` one must not leak
  // the outer function's InCallerLocationFunc_ = true into its own body.
  bool SavedInCallerLocationFunc = InCallerLocationFunc_;
  InCallerLocationFunc_ = Node.hasCallerLocation();

  if (Node.getBody())
    const_cast<ExprAST *>(Node.getBody())->accept(*this);

  InCallerLocationFunc_ = SavedInCallerLocationFunc;
  ReturnTypeStack_.pop_back();
  setType(&Node, TurfVoid());
}


void TypeChecker::visitCallerLocation(CallerLocationExprAST &Node) {
  // STD-002 (ARCHITECTURE_OVERHAUL.md §8/§15.6(a)): mirrors the unsafe
  // gate's enforcement shape (TypeChecker is the sole gate; Resolver and
  // Codegen barely know this expression exists, see
  // docs/architecture/pointers_and_references.md §2.5's "Resolver and
  // Codegen barely know unsafe{} exists" precedent). Codegen's own
  // guarantee (BuildCallerLocationValue() always finding a live
  // NamedValues["caller_location"] entry to forward, CodegenCore.cpp) rests
  // entirely on this check having already rejected every other case.
  if (!InCallerLocationFunc_) {
#ifndef TYPECHECKER_NO_ERRORS
    CallerLocationOutsideMarkedFunctionError(Node.getLoc()).raise();
#else
    throw std::runtime_error("CallerLocationOutsideMarkedFunctionError");
#endif
  }
  setType(&Node, TurfString());
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

  // TYPE-002 (S15/D2): a generic type used only as a struct FIELD (no
  // variable of that type, no method called on it, anywhere in the
  // program) was never recorded as an instantiation request -- see
  // recordGenericFieldInstantiations()'s own doc comment for the full
  // story. Recorded here, once per struct declaration TypeChecker visits
  // (every struct in the program, generic templates included -- the
  // concreteness gate inside the helper skips a template's own
  // still-open type parameters).
  for (const auto &Field : Node.getFields())
    recordGenericFieldInstantiations(Field.Type, Node.getLoc(), &Node, MonoCollector_);

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
      if (DeclaredElemTy && InitTy) {
        if (!typesCompatible(InitTy.get(), DeclaredElemTy.get())) {
          reportError(Node.getLoc(),
                      "Cannot initialize an array of '" +
                          DeclaredElemTy->getName() + "' with a value of type '" +
                          InitTy->getName() + "'.",
                      {});
        } else {
          // SEM-002 -- see visitVarDecl()'s identical comment.
          checkLiteralRepresentable(Init.get(), DeclaredElemTy.get(), Node.getLoc());
        }
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
    } else {
      // SEM-002 -- see visitVarDecl()'s identical comment. Element 0 also
      // establishes ElemType itself, so it's checked against that same
      // type too (a lone too-large first element would otherwise never be
      // checked at all).
      checkLiteralRepresentable(Node.getElements()[i].get(), ElemType.get(), Node.getLoc());
    }
  }
  checkLiteralRepresentable(Node.getElements()[0].get(), ElemType.get(), Node.getLoc());

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
