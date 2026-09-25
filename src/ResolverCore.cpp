/**
 * @file ResolverCore.cpp
 * @brief Entry points, hoisting pass (forward-declaration registration), and shared cross-category helpers.
 * @details Part of the Resolver name-resolution pass (Issue #18), split from
 *          the original monolithic Resolver.cpp by category (mirrors
 *          AST.h's ASTExpr/ASTStmt/ASTDecl/ASTAccess split -- see
 *          docs/architecture/compiler_pipeline.md).
 *
 * @author a2ys
 * @copyright Copyright (c) 2026 TurfLang contributors.  Apache License.
 */

#include "Resolver.h"
#include "SymbolTable.h"

#include <algorithm> // std::find

#include "AST.h"
#include "Builtins.h"        // LookupBuiltin — for skipping bare intrinsics in visitFuncCall
#include "Errors.h"          // ReferenceError, DuplicateDeclarationError
#include "ErrorsStruct.h"    // StructLayoutCycleError
#include "ResolverSymbolTable.h"
#include "ResolverInternal.h"
#include "SuggestionIndex.h" // SuggestionIndex::suggestVisible
#include "Types.h"           // RankedSuggestions

#include "llvm/ADT/SmallString.h" // Num->getIntValExact().toString() below

#include <cstring>
#include <functional> // std::function -- checkStructLayoutAcyclicity()'s recursive DFS lambda
#include <memory>
#include <unordered_map> // checkStructLayoutAcyclicity()'s name -> AST / colour maps
#include <unordered_set>


// ─── Public entry points ──────────────────────────────────────────────────────

void Resolver::resolve(ExprAST *Node) {
  if (!Node)
    return;
  // Top-level nodes are dispatched directly here rather than through
  // visitChild() (there is no "parent" child-dispatch call for them to go
  // through) -- fire the same scope-snapshot hook here too, so it covers
  // top-level declarations as well as everything nested under them. See
  // ScopeSnapshotCallback's doc comment (Resolver.h) and issue #244.
  if (ScopeSnapshotCB_)
    ScopeSnapshotCB_(*Node, SymTable_.getAllVisibleNames());
  Node->accept(*this);
}


void Resolver::resolveAll(
    const std::vector<std::unique_ptr<ExprAST>> &Nodes,
    bool RecoverPerDeclaration) {
  // ─────────────────────────────────────────────────────────────────────────
  // Pass 1 — Hoisting
  // Walk only top-level nodes (no body recursion).
  // Register every struct name and function signature so that Pass 2
  // can resolve forward references (foo calling bar defined below, or
  // a struct field using a type declared later in the file).
  //
  // Always one uninterrupted call regardless of RecoverPerDeclaration --
  // see this parameter's own doc comment (Resolver.h) for why a Pass 1
  // failure must still abort the whole resolveAll() call.
  // ─────────────────────────────────────────────────────────────────────────
  hoistTopLevel(Nodes);

  // Every struct name this call just hoisted (plus every one hoisted by an
  // earlier resolveAll() call on this same Resolver instance, see
  // HoistedStructDecls_'s own doc comment) now has a computable-or-not
  // by-value layout to check. Must run here -- after EVERY struct name in
  // this call's node list is registered, strictly before Pass 2 below (or
  // anything else) ever computes a layout, a default value, or a GC
  // descriptor for one. See checkStructLayoutAcyclicity()'s own doc
  // comment (Resolver.h) for the full graph definition.
  checkStructLayoutAcyclicity();

  // ─────────────────────────────────────────────────────────────────────────
  // Pass 2 — Full Resolution
  // Walk the entire AST (all bodies). Forward references resolve correctly
  // because Pass 1 has already registered all struct names and signatures.
  // ─────────────────────────────────────────────────────────────────────────
  if (!RecoverPerDeclaration) {
    for (const auto &Node : Nodes)
      resolve(Node.get());
    return;
  }

  // Per-top-level-node recovery (issue #246, opt-in -- see
  // RecoverPerDeclaration's doc comment). Mirrors #228's identical fix for
  // the PARSER's own per-statement loop: each node gets its own setjmp()
  // instead of one guard around the whole Pass-2 loop, so one
  // declaration's error doesn't prevent every other declaration from being
  // resolved and having its own errors surfaced too.
  //
  // g_recoverJmp/g_recoverActive are saved and restored around each node's
  // own setjmp() so this doesn't corrupt whatever OUTER recovery scope the
  // caller already has active around this whole resolveAll() call.
  for (const auto &Node : Nodes) {
    const bool SavedRecoverActive = g_recoverActive;
    jmp_buf SavedJmpBuf;
    std::memcpy(SavedJmpBuf, g_recoverJmp, sizeof(jmp_buf));

    g_recoverActive = true;
    if (setjmp(g_recoverJmp) == 0) {
      resolve(Node.get());
    } else {
      // See resetForRecovery()'s own doc comment: scope depth and
      // FunctionNestingDepth_ can be left non-zero by a longjmp that
      // skipped whatever enterScope()/exitScope() pairs or nesting-depth
      // increments were mid-flight when the error fired.
      resetForRecovery();
    }
    g_recoverActive = SavedRecoverActive;
    std::memcpy(g_recoverJmp, SavedJmpBuf, sizeof(jmp_buf));
  }
}


/**
 * @brief Rewrite a parsed type so any NamedType naming one of @p TypeParams
 *        becomes a real TurfGenericParamType, recursively through pointer,
 *        array, and generic-argument wrapping.
 *
 * ParseType() has no way to know at parse time that a bare identifier like
 * `T` in `x: T` names the enclosing function's own generic type parameter
 * rather than a struct -- it always emits a plain NamedType("T"). That
 * ambiguity is harmless for name *validation* once validateNamedType() is
 * told about the enclosing TypeParams list, but it is NOT what
 * typesCompatible()'s type-erasure fast path (Types.cpp) checks for: that
 * path dynamic_casts specifically for TurfGenericParamType. Left
 * unsubstituted, a signature like `fn<T> identity(x: T): T` type-checked
 * its own body fine, but calling it (e.g. `identity(42)`) failed overload
 * resolution with "No overload of 'identity' matches argument types (int)",
 * because typesCompatible(int, NamedType("T")) has no idea NamedType("T")
 * is supposed to mean "any type". This substitution is applied once, when
 * the signature is captured into ResolverFunctionSymbol (buildFunctionSymbol
 * below), so every downstream consumer -- overload resolution here, and
 * later TypeChecker/Codegen, which both trust ResolvedCandidates/
 * ResolvedCallTarget rather than re-deriving the signature -- sees a real
 * TurfGenericParamType and the existing type-erasure handling just works.
 *
 * Non-generic functions (empty TypeParams) and non-matching names return
 * @p Ty unchanged (structurally, the same shared_ptr).
 */
TurfTypePtr substituteTypeParams(const TurfTypePtr &Ty,
                                 const std::vector<std::string> &TypeParams) {
  if (!Ty || TypeParams.empty())
    return Ty;

  if (const auto *NT = dynamic_cast<const NamedType *>(Ty.get())) {
    if (std::find(TypeParams.begin(), TypeParams.end(), NT->getName()) !=
        TypeParams.end())
      return std::make_shared<TurfGenericParamType>(NT->getName());
    return Ty;
  }
  // Checked BEFORE the plain TurfPointerType branch below: TurfReferenceType
  // (&T, issue #238) is a SUBCLASS of TurfPointerType (see its own doc
  // comment, Types.h), so `dynamic_cast<const TurfPointerType *>` matches
  // it too -- without this earlier, more specific check, substituting a
  // generic function's own `&T` parameter/return/field type silently
  // reconstructed a plain `TurfPointerType`, turning every `&T` into `T*`
  // (confirmed live: `fn<T> Box.set(v: &T)` reported its own parameter as
  // `T*` in the "raw pointer in function signature" diagnostic).
  if (const auto *RT = dynamic_cast<const TurfReferenceType *>(Ty.get())) {
    TurfTypePtr NewPointee = substituteTypeParams(RT->getPointee(), TypeParams);
    return NewPointee == RT->getPointee()
              ? Ty
              : std::make_shared<TurfReferenceType>(NewPointee);
  }
  // Same bug class, same fix, for the OTHER safe-reference kind (LANG-002).
  // TurfNullableReferenceType is a SIBLING of TurfReferenceType (both direct
  // TurfPointerType subclasses -- see its own doc comment, Types.h), so the
  // check just above does NOT catch it; without this it would fall through
  // to the generic TurfPointerType branch below and silently turn every
  // substituted generic `&?T` into `T*`, the identical failure this whole
  // function's doc comment already documents for `&T`.
  if (const auto *NRT = dynamic_cast<const TurfNullableReferenceType *>(Ty.get())) {
    TurfTypePtr NewPointee = substituteTypeParams(NRT->getPointee(), TypeParams);
    return NewPointee == NRT->getPointee()
              ? Ty
              : std::make_shared<TurfNullableReferenceType>(NewPointee);
  }
  if (const auto *PT = dynamic_cast<const TurfPointerType *>(Ty.get())) {
    TurfTypePtr NewPointee = substituteTypeParams(PT->getPointee(), TypeParams);
    return NewPointee == PT->getPointee()
              ? Ty
              : std::make_shared<TurfPointerType>(NewPointee);
  }
  if (const auto *AT = dynamic_cast<const ArrayType *>(Ty.get())) {
    TurfTypePtr NewElem =
        substituteTypeParams(AT->getElementTypePtr(), TypeParams);
    return NewElem == AT->getElementTypePtr()
              ? Ty
              : makeArrayType(NewElem, AT->getSize());
  }
  if (const auto *GT = dynamic_cast<const GenericType *>(Ty.get())) {
    std::vector<TurfTypePtr> NewArgs;
    bool Changed = false;
    for (const auto &Arg : GT->getTypeArgs()) {
      TurfTypePtr NewArg = substituteTypeParams(Arg, TypeParams);
      Changed |= (NewArg != Arg);
      NewArgs.push_back(std::move(NewArg));
    }
    return Changed ? std::make_shared<GenericType>(GT->getBaseName(),
                                                    std::move(NewArgs))
                   : Ty;
  }
  // STD-003: a callback parameter's own signature (`cmp: fn(T, T): int`)
  // never had its inner `T`s substituted here -- FuncType simply had no
  // case, so `T` inside `fn(...)` stayed the bare, unresolved NamedType("T")
  // forever, invisible to every downstream consumer that dynamic_casts
  // specifically for TurfGenericParamType (typesCompatible(),
  // collectGenericSubstitution(), Monomorphize::substituteConcreteTypes() --
  // all fixed alongside this one, same root cause). This is the first of
  // those fixes to run, at declaration time, so every one downstream sees a
  // real TurfGenericParamType the same way a bare `x: T` parameter already
  // does.
  if (const auto *FT = dynamic_cast<const FuncType *>(Ty.get())) {
    std::vector<TurfTypePtr> NewParams;
    bool Changed = false;
    for (const auto &P : FT->getParamTypes()) {
      TurfTypePtr NewP = substituteTypeParams(P, TypeParams);
      Changed |= (NewP != P);
      NewParams.push_back(std::move(NewP));
    }
    TurfTypePtr NewRet = substituteTypeParams(FT->getReturnType(), TypeParams);
    Changed |= (NewRet != FT->getReturnType());
    return Changed ? std::make_shared<FuncType>(std::move(NewRet),
                                                std::move(NewParams))
                   : Ty;
  }
  return Ty;
}


/**
 * @brief Conservative, direct-only scan: does @p E ever assign to a
 *        `self.<field>` anywhere in its subtree?
 *
 * Used to compute ResolverFunctionSymbol::MutatesSelf for a struct instance
 * method, so TypeChecker::visitMethodCall() can reject calling it through an
 * immutable variable -- see that field's doc comment in
 * ResolverSymbolTable.h for the motivating bug (`c.bump()` silently mutating
 * an `immut c` when `bump()` internally does `self.val += 1`).
 *
 * Deliberately NOT interprocedural (never looks inside a nested function/
 * method call the body makes) and NOT alias-aware (passing `self` or
 * `&self.field` to another function is not itself treated as a mutation).
 * This is the opposite conservatism from Lint.cpp's MayModifyVariable(),
 * which backs a WARNING and so errs toward over-flagging; this result gates
 * a hard compile ERROR, so it errs toward under-flagging instead -- only
 * provable, direct, in-body mutation counts, so a genuinely read-only
 * method (e.g. a getter that happens to call a private helper) can never be
 * wrongly rejected on an immutable variable. The cost is symmetrical to
 * MayModifyVariable's: some real mutations that only happen through a
 * nested call go undetected, same as before this check existed at all.
 */
static bool MutatesSelfField(const ExprAST *E) {
  if (!E)
    return false;

  auto IsSelfFieldAccess = [](const ExprAST *Candidate) {
    auto *FA = dynamic_cast<const FieldAccessExprAST *>(Candidate);
    if (!FA)
      return false;
    auto *Base = dynamic_cast<const VariableExprAST *>(FA->getBase());
    return Base && Base->getName() == "self";
  };

  if (auto *Assign = dynamic_cast<const AssignmentExprAST *>(E))
    return IsSelfFieldAccess(Assign->getLHS());
  if (auto *CAssign = dynamic_cast<const CompoundAssignmentExprAST *>(E))
    return IsSelfFieldAccess(CAssign->getLHS());
  if (auto *Post = dynamic_cast<const PostfixExprAST *>(E))
    return IsSelfFieldAccess(Post->getOperand());
  if (auto *Blk = dynamic_cast<const BlockExprAST *>(E)) {
    for (const auto &Child : Blk->getExpressions())
      if (MutatesSelfField(Child.get()))
        return true;
    return false;
  }
  if (auto *If = dynamic_cast<const IfExprAST *>(E)) {
    for (const auto &B : If->getBranches())
      if (MutatesSelfField(B.Body.get()))
        return true;
    return MutatesSelfField(If->getElseBody());
  }
  if (auto *W = dynamic_cast<const WhileExprAST *>(E))
    return MutatesSelfField(W->getBody());
  if (auto *F = dynamic_cast<const ForExprAST *>(E))
    return MutatesSelfField(F->getBody());
  if (auto *FE = dynamic_cast<const ForEachExprAST *>(E))
    return MutatesSelfField(FE->getBody());
  return false;
}


/**
 * @brief Collects candidate error-type names from a function body's `return`
 * statements (docs/architecture/errors_and_recover.md), for
 * ResolverFunctionSymbol:: PossibleErrorTypeNames -- see that field's doc
 * comment for why this is intentionally unfiltered (not yet checked against
 * isErrorTypeName()) and why the scan is conservative/non-interprocedural, same
 * rationale as MutatesSelfField() just above (mirrors its exact recursion
 * shape).
 *
 * Recognizes `return ErrorType::Variant;` (a bare ScopeAccessExprAST) AND,
 * since STD-002 (ARCHITECTURE_OVERHAUL.md §8/§15.6(c)), `return
 * ErrorType::Variant(messageExpr);` (a FuncCallExprAST whose Callee is a
 * ScopeAccessExprAST -- the exact same generic `Left::Right(args)` call
 * shape `StructName::new(...)`/`Module::func(...)` also parse as), either
 * way collecting `Left` (e.g. "MathError"). The call-shape branch is
 * UNFILTERED the identical way the bare-reference branch already is --
 * `ResolvedKind` isn't set yet at this point (Pass 1 hoisting, before Pass
 * 2's visitFuncCall runs), so this can't yet tell an error-variant call
 * apart from a struct constructor or module-function call by anything
 * other than shape; every consumer already re-filters through
 * isErrorTypeName() before drawing any conclusion (see this function's own
 * doc comment above).
 */
static void CollectReturnedErrorTypeNames(const ExprAST *E,
                                          std::vector<std::string> &Out) {
  if (!E)
    return;

  if (auto *Ret = dynamic_cast<const ReturnExprAST *>(E)) {
    if (auto *SA = dynamic_cast<const ScopeAccessExprAST *>(Ret->getVal())) {
      Out.push_back(SA->getLeftName());
    } else if (auto *FC = dynamic_cast<const FuncCallExprAST *>(Ret->getVal())) {
      if (auto *SA = dynamic_cast<const ScopeAccessExprAST *>(FC->getCallee()))
        Out.push_back(SA->getLeftName());
    }
    return;
  }
  if (auto *Blk = dynamic_cast<const BlockExprAST *>(E)) {
    for (const auto &Child : Blk->getExpressions())
      CollectReturnedErrorTypeNames(Child.get(), Out);
    return;
  }
  if (auto *If = dynamic_cast<const IfExprAST *>(E)) {
    for (const auto &B : If->getBranches())
      CollectReturnedErrorTypeNames(B.Body.get(), Out);
    CollectReturnedErrorTypeNames(If->getElseBody(), Out);
    return;
  }
  if (auto *W = dynamic_cast<const WhileExprAST *>(E)) {
    CollectReturnedErrorTypeNames(W->getBody(), Out);
    return;
  }
  if (auto *F = dynamic_cast<const ForExprAST *>(E)) {
    CollectReturnedErrorTypeNames(F->getBody(), Out);
    return;
  }
  if (auto *FE = dynamic_cast<const ForEachExprAST *>(E)) {
    CollectReturnedErrorTypeNames(FE->getBody(), Out);
    return;
  }
}


ResolverFunctionSymbol
Resolver::buildFunctionSymbol(const FuncDefExprAST &Node,
                              const std::string &ModuleName) const {
  ResolverFunctionSymbol Sym;
  Sym.BaseName = Node.getName();
  Sym.SourceModule = ModuleName;
  Sym.Params.reserve(Node.getParams().size());
  const auto &TypeParams = Node.getTypeParams();
  for (const auto &Param : Node.getParams()) {
    Sym.Params.push_back(
        {Param.Name, substituteTypeParams(Param.Type, TypeParams)});
  }
  Sym.ReturnType = substituteTypeParams(Node.getReturnTypePtr(), TypeParams);
  Sym.TypeParams = TypeParams;
  Sym.HasCallerLocation = Node.hasCallerLocation();
  ResolverSourceLocation DeclLoc;
  // getNameLoc(), not getLoc() -- getLoc() is the 'fn'/'operator' keyword's
  // own position (see FuncDefExprAST's doc comment in ASTDecl.h), which
  // used to make every "go to definition" land one token early, on 'f' of
  // 'fn' rather than the function's actual name (confirmed live via
  // turf-lsp: textDocument/definition on `io::print` returned a 1-character
  // range at column 0 of `fn print(...)`, not on "print"). getNameLoc()
  // already exists for exactly this (issue #230) and for a struct method
  // (`fn Point.new(...)`) correctly points at "new", not "Point" or "fn".
  DeclLoc.Line = Node.getNameLoc().Line;
  DeclLoc.Col = Node.getNameLoc().Col;
  Sym.DeclLoc = DeclLoc;
  // Only instance methods (Params[0] is the implicit "self" -- see
  // ParseFuncDef's self-parameter construction) can mutate self at all;
  // static methods and free/module functions leave MutatesSelf at its
  // default false.
  if (!Sym.Params.empty() && Sym.Params[0].Name == "self")
    Sym.MutatesSelf = MutatesSelfField(Node.getBody());
  CollectReturnedErrorTypeNames(Node.getBody(), Sym.PossibleErrorTypeNames);
  return Sym;
}


void Resolver::registerFunctionSignature(const FuncDefExprAST &Node,
                                         const std::string &ModuleName) {
  auto Sym = buildFunctionSymbol(Node, ModuleName);
  validateOperatorDecl(Sym);
  if (ModuleName.empty()) {
    SymTable_.registerCurrentFileFunction(Sym);
  } else {
    SymTable_.registerModuleFunction(ModuleName, Sym);
  }
}


void Resolver::validateOperatorDecl(const ResolverFunctionSymbol &Sym) const {
  if (Sym.BaseName.rfind("operator", 0) != 0)
    return;

  auto IsBuiltinOperandType = [](const TurfType *T) {
    return T && (T->isInt() || T->isInt32() || T->isDouble() || T->isBool() ||
                 T->isChar() || T->isPointer());
  };

  const bool IsIndexGet = Sym.BaseName == OperatorIndexGetName();
  const bool IsIndexSet = Sym.BaseName == OperatorIndexSetName();
  const std::string OpSpelling = Sym.BaseName.substr(8); // strip "operator" prefix

  // Collect the operand(s) the two checks below apply to. `operator[]`/
  // `operator[]=` only ever check the SELF operand (Params[0]) -- indexing
  // is inherently asymmetric (the index/value types don't define what is
  // being indexed), unlike a symmetric binary operator where either operand
  // could be the "foreign" one being redefined.
  std::vector<const TurfType *> Operands;
  if (IsIndexGet || IsIndexSet) {
    if (Sym.Params.empty())
      return; // arity mismatch is caught elsewhere (parser)
    Operands.push_back(Sym.Params[0].Type.get());
  } else if (Sym.Params.size() == 1 || Sym.Params.size() == 2) {
    for (const auto &P : Sym.Params)
      Operands.push_back(P.Type.get());
  } else {
    return; // arity mismatch is caught elsewhere (parser)
  }

  // All-primitive rejection (existing behaviour, §13.4) -- skipped for
  // operator[]/[]=: there is no second "pairing" operand for this rule to
  // compare against, so a nonsensical `operator[](a: int, i: int): int` is
  // left to the orphan rule below instead (it has no struct/enum operand at
  // all, so it fails that check).
  if (!IsIndexGet && !IsIndexSet) {
    bool AllBuiltin = true;
    for (const auto *T : Operands)
      if (!IsBuiltinOperandType(T)) { AllBuiltin = false; break; }
    if (AllBuiltin) {
      OperatorBuiltinPairError(
          toSourceLocation(Sym.DeclLoc), OpSpelling, Operands[0]->getName(),
          (Operands.size() > 1 ? Operands[1] : Operands[0])->getName())
          .raise();
      return;
    }
  }

  // Orphan rule (LANG-001, §13.4): at least one operand that IS a declared
  // struct/enum must be owned by the SAME module this operator is declared
  // in -- otherwise a third module could redefine the operator's meaning on
  // two types it doesn't own, and the result would depend on import order.
  // A builtin/`string` operand is never "declared" anywhere (no
  // registerStruct()/registerEnumType() call ever runs for it), so it can
  // neither satisfy nor violate this rule by itself: if NONE of the
  // operands is a struct/enum at all (e.g. `strings.tr`'s existing
  // `operator +(string, string)`), the rule is vacuously satisfied rather
  // than flagged -- there is no "foreign type" for any module to be
  // redefining behaviour on. Checked against `lib/std` before landing (see
  // the change record) -- confirmed to introduce zero regressions there.
  // NOTE: an operand's TurfType is very likely still a NamedType("A") here,
  // not yet resolved to the real TurfStructType/TurfEnumType -- Pass 1
  // hoisting (which calls registerFunctionSignature(), this function's only
  // caller) runs before the type-resolution pass that would replace it.
  // isStruct()/isEnum() only return true on the ALREADY-RESOLVED classes
  // (confirmed live: checking those two predicates directly here let a
  // cross-module orphan declaration through silently, since every operand
  // was still a NamedType at this point and NamedType overrides neither
  // predicate) -- so a struct/enum operand is identified by NAME against
  // the type registry instead, which is populated by hoisting itself and
  // therefore already has both this file's own structs/enums AND every
  // directly-imported module's (imports are hoisted before this file's own
  // body, per the existing hoisting order) by the time this runs.
  std::vector<const TurfType *> NamedOperands;
  for (const auto *T : Operands)
    if (T && (T->isStruct() || T->isEnum() ||
              SymTable_.isKnownStruct(T->getName()) ||
              SymTable_.isEnumName(T->getName())))
      NamedOperands.push_back(T);

  if (NamedOperands.empty())
    return;

  for (const auto *T : NamedOperands)
    if (SymTable_.getTypeOwningModule(T->getName()) == Sym.SourceModule)
      return; // at least one operand IS owned by this module -- satisfied

  const TurfType *Culprit = NamedOperands[0];
  OperatorOrphanRuleError(toSourceLocation(Sym.DeclLoc), OpSpelling,
                          Sym.SourceModule, Culprit->getName(),
                          SymTable_.getTypeOwningModule(Culprit->getName()))
      .raise();
}


void Resolver::hoistExportedConstant(const VarDeclExprAST &Var,
                                     const std::string &ModuleName) {
  SourceLocation Loc = Var.getLoc();

  if (!Var.isImmutable()) {
    ExportMutNotSupportedError(Loc, Var.getName()).raise();
    return;
  }

  // Duplicate check -- exported constants never go through
  // SymTable_.declareVariable() (they have no scope/storage at all, see
  // visitVarDecl), so this hoisting step is the only place a same-name
  // collision within one file/module would ever be caught.
  if (ModuleName.empty()) {
    if (const auto *Prev = SymTable_.lookupCurrentFileConstant(Var.getName())) {
      DuplicateDeclarationError(Loc, Var.getName(),
                                toSourceLocation(Prev->DeclLoc))
          .raise();
      return;
    }
  } else if (const auto *Prev =
                 SymTable_.findModuleConstant(ModuleName, Var.getName())) {
    DuplicateDeclarationError(Loc, Var.getName(), toSourceLocation(Prev->DeclLoc))
        .raise();
    return;
  }

  const ExprAST *Init = Var.getInitializer();
  ResolverConstantSymbol Sym;
  Sym.Name = Var.getName();
  Sym.SourceModule = ModuleName;
  Sym.Type = Var.getTypePtr();
  Sym.DeclLoc = {Loc.Line, Loc.Col};

  if (const auto *Num = dynamic_cast<const NumberExprAST *>(Init)) {
    if (Num->isInteger()) {
      // Exact decimal string, not the narrow getIntVal() -- see
      // ResolverConstantSymbol::IntVal's own doc comment (ResolverSymbolBase.h)
      // for why this field is a string at all.
      llvm::SmallString<32> Digits;
      Num->getIntValExact().toString(Digits, /*Radix=*/10);
      Sym.IntVal = std::string(Digits.str());
    } else {
      Sym.DoubleVal = Num->getDoubleVal();
    }
  } else if (const auto *B = dynamic_cast<const BoolExprAST *>(Init)) {
    Sym.BoolVal = B->getVal();
  } else if (const auto *S = dynamic_cast<const StringExprAST *>(Init)) {
    Sym.StringVal = S->getValue();
  } else {
    ExportNonConstantInitializerError(Loc, Var.getName()).raise();
    return;
  }

  if (ModuleName.empty())
    SymTable_.registerCurrentFileConstant(Sym);
  else
    SymTable_.registerModuleConstant(ModuleName, Sym);
}


// ─── Pass 1: Hoisting ─────────────────────────────────────────────────────────────────

void Resolver::hoistTopLevel(
    const std::vector<std::unique_ptr<ExprAST>> &Nodes) {
  for (const auto &Node : Nodes)
    hoistTopLevel(Node.get());
}


void Resolver::hoistTopLevel(const ExprAST *Node) {
  if (!Node)
    return;

  // Hoist struct declarations: register the name so NamedType("MyStruct")
  // references in other struct fields or function signatures resolve in Pass 2.
  if (const auto *Struct = dynamic_cast<const StructDeclExprAST *>(Node)) {
    SourceLocation Loc = Struct->getLoc();
    if (auto PrevLoc = SymTable_.registerStruct(
            Struct->getName(), {Loc.Line, Loc.Col})) {
      DuplicateTypeNameError(Loc, Struct->getName(), "struct",
                             {PrevLoc->Line, PrevLoc->Col})
          .raise();
    }
    // Keep the AST node itself, not just its name -- checkStructLayoutAcyclicity()
    // needs the actual field types, which ResolverTypeRegistry never stores.
    HoistedStructDecls_.push_back(Struct);
    return;
  }

  // Hoist enum declarations: register the name and variants.
  if (const auto *Enum = dynamic_cast<const EnumDeclExprAST *>(Node)) {
    auto ET = std::make_shared<TurfEnumType>(Enum->getName(), Enum->getVariants(), Enum->getUnderlyingType());
    SourceLocation Loc = Enum->getLoc();
    if (auto PrevLoc = SymTable_.registerEnumType(
            Enum->getName(), {Loc.Line, Loc.Col}, ET)) {
      DuplicateTypeNameError(Loc, Enum->getName(), "enum",
                             {PrevLoc->Line, PrevLoc->Col})
          .raise();
    }
    return;
  }

  // Hoist `machine` declarations: register the name/states/edges through the
  // SAME registerEnumType() enums use (a TurfMachineType IS-A TurfEnumType,
  // see its doc comment in include/Types.h) -- this one decision is what
  // lets MachineName::State access, `compare`/`matches` exhaustiveness, and
  // Codegen's constant-inlining all work on a machine subject with zero
  // further code anywhere in those paths.
  if (const auto *Machine = dynamic_cast<const MachineDeclExprAST *>(Node)) {
    std::map<std::string, std::set<std::string>> Edges;
    for (const auto &E : Machine->getEdges())
      Edges[E.From].insert(E.To);
    auto MT = std::make_shared<TurfMachineType>(Machine->getName(),
                                                Machine->getStates(), Edges);
    SourceLocation Loc = Machine->getLoc();
    if (auto PrevLoc = SymTable_.registerEnumType(
            Machine->getName(), {Loc.Line, Loc.Col}, MT)) {
      DuplicateTypeNameError(Loc, Machine->getName(), "state machine",
                             {PrevLoc->Line, PrevLoc->Col})
          .raise();
    }
    return;
  }

  // Hoist `error` type declarations (docs/architecture/errors_and_recover.md):
  // register the name and variants so NamedType/ScopeAccess references resolve
  // in Pass 2.
  if (const auto *ErrDecl = dynamic_cast<const ErrorDeclExprAST *>(Node)) {
    auto ET = std::make_shared<TurfUserErrorType>(
        ErrDecl->getName(), ErrDecl->getVariants(), ErrDecl->isBareForm());
    SourceLocation Loc = ErrDecl->getLoc();
    if (auto PrevLoc = SymTable_.registerErrorType(
            ErrDecl->getName(), {Loc.Line, Loc.Col}, ET)) {
      DuplicateTypeNameError(Loc, ErrDecl->getName(), "error",
                             {PrevLoc->Line, PrevLoc->Col})
          .raise();
    }
    return;
  }

  // Hoist function definitions: register the signature (NOT the body).
  if (const auto *Func = dynamic_cast<const FuncDefExprAST *>(Node)) {
    registerFunctionSignature(*Func, currentModuleName());
    return;
  }

  // Hoist extern function declarations: register the signature in the
  // current-file function registry with an IsExtern flag so forward-calling
  // code can resolve the name.  No body exists, so no body recursion.
  if (const auto *Extern = dynamic_cast<const ExternFuncDeclExprAST *>(Node)) {
    ResolverFunctionSymbol Sym;
    Sym.BaseName = Extern->getName();
    Sym.SourceModule = ""; // extern fns are in the global (file) namespace
    Sym.ReturnType = Extern->getReturnType();
    Sym.IsExtern    = true; // see IsExtern's doc comment in ResolverSymbolTable.h
    Sym.IsVarArg    = Extern->isVarArg(); // propagate '...' to arity check
    for (const auto &[PName, PType] : Extern->getParams())
      Sym.Params.push_back({PName, PType});
    SymTable_.registerCurrentFileFunction(Sym);
    return;
  }

  // Hoist imported modules: recurse into the module body to find its fns.
  if (const auto *Import = dynamic_cast<const ModuleImportExprAST *>(Node)) {
    hoistModuleBody(Import->getBody(), Import->getModuleName());
    registerOnDemandImports(*Import, /*ContextLabel=*/"");
    return;
  }

  // Hoist `export immut` constants declared at the top level of the main
  // file. A plain (non-exported) VarDeclExprAST can't reach hoistTopLevel
  // at all -- ParsePrimaryBase() has no case for bare TOK_MUTABLE/
  // TOK_IMMUTABLE, so main.cpp's top-level dispatch never produces one; the
  // only way a VarDeclExprAST shows up here is via TOK_EXPORT.
  if (const auto *Var = dynamic_cast<const VarDeclExprAST *>(Node)) {
    if (Var->isExported())
      hoistExportedConstant(*Var, /*ModuleName=*/"");
    return;
  }
  // All other top-level nodes (e.g. standalone expressions) are not
  // declarations and have nothing to hoist.
}


void Resolver::hoistModuleBody(const ExprAST *Body,
                               const std::string &ModuleName) {
  const auto *Block = dynamic_cast<const BlockExprAST *>(Body);
  if (!Block)
    return;

  // Register this module as known EVEN IF it turns out to declare zero
  // functions (e.g. a module that only declares structs/enums) -- see
  // registerModule()'s doc comment for the real bug this fixes.
  SymTable_.registerModule(ModuleName);

  for (const auto &Expr : Block->getExpressions()) {
    if (const auto *Func = dynamic_cast<const FuncDefExprAST *>(Expr.get())) {
      registerFunctionSignature(*Func, ModuleName);
    } else if (const auto *Extern =
                   dynamic_cast<const ExternFuncDeclExprAST *>(Expr.get())) {
      ResolverFunctionSymbol Sym;
      Sym.BaseName = Extern->getName();
      Sym.SourceModule = ModuleName;
      Sym.ReturnType = Extern->getReturnType();
      Sym.IsExtern = true; // see IsExtern's doc comment in ResolverSymbolTable.h
      Sym.IsVarArg = Extern->isVarArg();
      for (const auto &[PName, PType] : Extern->getParams())
        Sym.Params.push_back({PName, PType});
      SymTable_.registerModuleFunction(ModuleName, Sym);
    } else if (const auto *Struct =
                   dynamic_cast<const StructDeclExprAST *>(Expr.get())) {
      SourceLocation Loc = Struct->getLoc();
      if (auto PrevLoc = SymTable_.registerStruct(
              Struct->getName(), {Loc.Line, Loc.Col}, ModuleName)) {
        DuplicateTypeNameError(Loc, Struct->getName(), "struct",
                               {PrevLoc->Line, PrevLoc->Col})
            .raise();
      }
      // See the identical comment on the hoistTopLevel() struct branch above.
      HoistedStructDecls_.push_back(Struct);
    } else if (const auto *Enum =
                   dynamic_cast<const EnumDeclExprAST *>(Expr.get())) {
      // Previously MISSING entirely: an enum declared inside an imported
      // module's body was never hoisted at all (only structs/functions/
      // externs were handled here), so it would silently fail to resolve
      // anywhere it was referenced -- e.g. "I can't find anything named
      // 'X'" for a perfectly real enum, just because it happened to live
      // inside a module instead of the top-level file. No stdlib module
      // declares an enum yet, so this had never been exercised.
      auto ET = std::make_shared<TurfEnumType>(
          Enum->getName(), Enum->getVariants(), Enum->getUnderlyingType());
      SourceLocation Loc = Enum->getLoc();
      if (auto PrevLoc = SymTable_.registerEnumType(
              Enum->getName(), {Loc.Line, Loc.Col}, ET, ModuleName)) {
        DuplicateTypeNameError(Loc, Enum->getName(), "enum",
                               {PrevLoc->Line, PrevLoc->Col})
            .raise();
      }
    } else if (const auto *Machine =
                   dynamic_cast<const MachineDeclExprAST *>(Expr.get())) {
      // Hoisted here too, not just in hoistTopLevel() -- the enum branch
      // just above was ONCE missing from this function entirely (see its own
      // comment), silently breaking any enum declared inside an imported
      // module's body. Don't repeat that gap for `machine` declarations.
      std::map<std::string, std::set<std::string>> Edges;
      for (const auto &E : Machine->getEdges())
        Edges[E.From].insert(E.To);
      auto MT = std::make_shared<TurfMachineType>(
          Machine->getName(), Machine->getStates(), Edges);
      SourceLocation Loc = Machine->getLoc();
      if (auto PrevLoc = SymTable_.registerEnumType(
              Machine->getName(), {Loc.Line, Loc.Col}, MT, ModuleName)) {
        DuplicateTypeNameError(Loc, Machine->getName(), "state machine",
                               {PrevLoc->Line, PrevLoc->Col})
            .raise();
      }
    } else if (const auto *ErrDecl =
                   dynamic_cast<const ErrorDeclExprAST *>(Expr.get())) {
      // Hoisted here too, not just in hoistTopLevel() -- the enum branch
      // just above was ONCE missing from this function entirely (see its own
      // comment), silently breaking any enum declared inside an imported
      // module's body. Don't repeat that gap for `error` types.
      auto ET = std::make_shared<TurfUserErrorType>(
          ErrDecl->getName(), ErrDecl->getVariants(), ErrDecl->isBareForm());
      SourceLocation Loc = ErrDecl->getLoc();
      if (auto PrevLoc = SymTable_.registerErrorType(
              ErrDecl->getName(), {Loc.Line, Loc.Col}, ET, ModuleName)) {
        DuplicateTypeNameError(Loc, ErrDecl->getName(), "error",
                               {PrevLoc->Line, PrevLoc->Col})
            .raise();
      }
    } else if (const auto *Import =
                   dynamic_cast<const ModuleImportExprAST *>(Expr.get())) {
      // Nested module imports (unusual but possible)
      hoistModuleBody(Import->getBody(), Import->getModuleName());
      // ContextLabel is the OUTER module currently being hoisted (this
      // function's own ModuleName parameter) -- an on-demand import
      // written INSIDE module M's own file makes its requested symbols
      // bare-visible within M, not within whatever imported M.
      registerOnDemandImports(*Import, ModuleName);
    } else if (const auto *Var =
                   dynamic_cast<const VarDeclExprAST *>(Expr.get())) {
      if (Var->isExported())
        hoistExportedConstant(*Var, ModuleName);
    }
  }
}


// ─── Pass 1 (end): struct layout acyclicity ────────────────────────────────
//
// See the doc comment on the declaration (Resolver.h) for the full graph
// definition and why this must run where resolveAll() calls it (end of
// hoisting, before Pass 2). What follows is the "how", kept out of the
// header because it's implementation, not contract.

namespace {

// Three-colour DFS state, one entry per struct name that has been reached
// (a name never reached at all -- e.g. one HoistedStructDecls_ has no entry
// for, which should not happen but is handled defensively below -- has no
// map entry and is implicitly White).
enum class DFSColor { Grey, Black };

/**
 * @brief The struct name @p T's type contributes to a by-value containment
 *        edge FROM, or "" if it contributes no edge at all.
 *
 * Unwraps array dimensions (an array of N structs still contains each by
 * value) and looks inside a GenericType only as far as its OWN base name --
 * `Box<T>`'s type ARGUMENTS are deliberately not recursed into here. This
 * is a template-level check: at this point in the pipeline `T` in `Box<T>`
 * is still an uninstantiated generic parameter, not a concrete type, so
 * there is nothing concrete to check yet -- `struct Box<T> { inner: Box<T>
 * }` is correctly caught (its OWN base name, "Box", is a registered struct,
 * self-edge found regardless of T), but `struct List<T> { data: T* }`
 * instantiated as `List<Container>` is correctly NOT flagged here even
 * though a naive "recurse into type args" version would wrongly treat `T`
 * (i.e. `Container` after substitution) as an edge. Once generics are
 * monomorphized (TYPE-002), the exact same function is called again per
 * concrete instantiation, where a real substituted field type CAN turn a
 * template-safe generic struct into a genuinely cyclic concrete one -- see
 * TYPE-001's "Generic interactions" note in
 * docs/planning/ARCHITECTURE_OVERHAUL.md §8.
 *
 * A pointer-shaped field (isPointer() -- covers TurfPointerType, its
 * TurfReferenceType subclass, and OpaquePointerType, see
 * docs/architecture/pointers_and_references.md §2.1) is never an edge and
 * is deliberately NOT unwrapped further -- what it points to is irrelevant
 * to ITS OWN size.
 *
 * @param KnownStructs Every struct name reached so far (HoistedStructDecls_
 *                     keyed by name) -- an edge target that isn't in this
 *                     set is either a primitive/generic-param placeholder
 *                     or a genuinely unknown name (Pass 2's validateNamedType()
 *                     raises UnknownTypeError for the latter; this check
 *                     runs before that and simply finds no edge either way).
 */
std::string byValueEdgeTarget(
    const TurfType *T,
    const std::unordered_map<std::string, const StructDeclExprAST *> &KnownStructs) {
  if (!T || T->isPointer())
    return "";

  if (isArrayType(T))
    return byValueEdgeTarget(getArrayElementType(T), KnownStructs);

  if (const auto *GT = dynamic_cast<const GenericType *>(T)) {
    auto It = KnownStructs.find(GT->getBaseName());
    return It != KnownStructs.end() ? GT->getBaseName() : "";
  }

  if (const auto *NT = dynamic_cast<const NamedType *>(T)) {
    auto It = KnownStructs.find(NT->getName());
    return It != KnownStructs.end() ? NT->getName() : "";
  }

  // Defensive: a field type is a NamedType placeholder everywhere in the
  // pipeline this check actually runs (Pass 1, before anything resolves a
  // struct reference to a real TurfStructType) -- but handle the resolved
  // form too rather than silently miss an edge if that ever changes.
  if (const auto *ST = dynamic_cast<const TurfStructType *>(T))
    return KnownStructs.count(ST->getName()) ? ST->getName() : "";

  return "";
}

/// Finds @p FieldName inside @p Decl's own field list. Only ever called
/// with a name that byValueEdgeTarget() just derived from one of Decl's own
/// fields, so the lookup cannot fail in practice; returns nullptr instead
/// of asserting so a future caller mistake degrades to a missing note
/// rather than a crash in a diagnostic path.
const StructField *findField(const StructDeclExprAST *Decl,
                             const std::string &FieldName) {
  for (const auto &F : Decl->getFields())
    if (F.Name == FieldName)
      return &F;
  return nullptr;
}

/// Shared DFS core of both checkStructLayoutAcyclicity() (Pass-1, every
/// hoisted TEMPLATE) and checkStructLayoutAcyclicityForClone() (TYPE-002
/// S13a, one freshly-substituted instantiation) -- extracted verbatim, no
/// behaviour change, so both callers stay correct by construction rather
/// than by keeping two copies in sync. @p ByName is the full universe of
/// structs a field edge may name; @p StartNames is which of them to run a
/// fresh DFS from (every hoisted struct for the Pass-1 sweep; just the one
/// new clone for the per-instantiation check -- see checkStructLayoutAcyclicityForClone()'s
/// own doc comment, Resolver.h, for why the rest of the graph doesn't need
/// re-walking).
void detectStructLayoutCycles(
    const std::unordered_map<std::string, const StructDeclExprAST *> &ByName,
    const std::vector<std::string> &StartNames) {
  std::unordered_map<std::string, DFSColor> Color;
  // The current DFS path: PathNames[i] is a struct name on the path;
  // PathFields[i] is the name of the FIELD in PathNames[i] whose type is
  // the edge to PathNames[i+1] (so PathFields.size() == PathNames.size() - 1
  // at all times -- pushed immediately before recursing into the target,
  // popped immediately after it returns).
  std::vector<std::string> PathNames;
  std::vector<std::string> PathFields;

  // A cycle is reported once per strongly-connected component: the moment
  // one is found, every struct still on PathNames is unwound normally
  // (colored Black on the way out, like any other completed DFS node), so
  // a second starting point that would re-enter the same SCC finds every
  // member of it already Black and does no further work. In a normal
  // (non-recovering) compile this is moot -- StructLayoutCycleError::raise()
  // calls std::exit(1) or longjmp()s out of the whole pass on the first hit
  // -- but the loop below is written to be correct even if raise() is ever
  // changed to collect and continue.
  std::function<void(const std::string &)> Visit = [&](const std::string &Name) {
    Color[Name] = DFSColor::Grey;
    PathNames.push_back(Name);

    const StructDeclExprAST *Decl = ByName.at(Name);
    for (const auto &Field : Decl->getFields()) {
      std::string Target = byValueEdgeTarget(Field.Type.get(), ByName);
      if (Target.empty())
        continue;

      auto CIt = Color.find(Target);
      if (CIt != Color.end() && CIt->second == DFSColor::Black)
        continue; // fully explored elsewhere already -- no cycle through here

      if (CIt != Color.end() && CIt->second == DFSColor::Grey) {
        // Found it: Target is somewhere earlier on PathNames. Reconstruct
        // the cycle starting there, closing back through this field.
        auto StartIt = std::find(PathNames.begin(), PathNames.end(), Target);
        size_t Start = static_cast<size_t>(StartIt - PathNames.begin());

        std::vector<std::string> CyclePath(PathNames.begin() + Start,
                                           PathNames.end());
        CyclePath.push_back(Target); // close the loop in the printed path

        StructLayoutCycleError Err(ByName.at(Target)->getLoc(), Target, CyclePath);
        // One note per edge in the cycle: PathFields[i] is the field inside
        // PathNames[i] that points at PathNames[i+1], for every edge already
        // on the path, plus the closing edge (Field, in the struct we're
        // CURRENTLY visiting -- PathNames.back() -- back to Target).
        for (size_t I = Start; I + 1 < PathNames.size(); ++I) {
          const StructField *F = findField(ByName.at(PathNames[I]), PathFields[I]);
          if (F)
            Err.withRelated(F->NameLoc, "field '" + PathNames[I] + "::" +
                                            F->Name + "' has type '" +
                                            F->Type->getName() + "'");
        }
        Err.withRelated(Field.NameLoc, "field '" + Name + "::" + Field.Name +
                                          "' has type '" +
                                          Field.Type->getName() + "'");
        Err.raise();
        continue;
      }

      // White (unseen): record which field led here, then recurse.
      PathFields.push_back(Field.Name);
      Visit(Target);
      PathFields.pop_back();
    }

    PathNames.pop_back();
    Color[Name] = DFSColor::Black;
  };

  for (const auto &Name : StartNames) {
    if (Color.find(Name) == Color.end())
      Visit(Name);
  }
}

} // namespace

void Resolver::checkStructLayoutAcyclicity() {
  std::unordered_map<std::string, const StructDeclExprAST *> ByName;
  std::vector<std::string> StartNames;
  for (const auto *Decl : HoistedStructDecls_) {
    ByName[Decl->getName()] = Decl;
    StartNames.push_back(Decl->getName());
  }
  detectStructLayoutCycles(ByName, StartNames);
}

void Resolver::checkStructLayoutAcyclicityForClone(const StructDeclExprAST *Clone) {
  std::unordered_map<std::string, const StructDeclExprAST *> ByName;
  for (const auto *Decl : HoistedStructDecls_)
    ByName[Decl->getName()] = Decl;
  // Register the clone itself under its own (already-mangled) name so a
  // field edge naming it (a nested self-reference through this specific
  // instantiation) resolves correctly -- see this function's own doc
  // comment (Resolver.h) for why only THIS one node needs to be a DFS root.
  ByName[Clone->getName()] = Clone;
  detectStructLayoutCycles(ByName, {Clone->getName()});
}


void Resolver::registerOnDemandImports(const ModuleImportExprAST &Import,
                                       const std::string &ContextLabel) {
  if (Import.getMode() == ImportMode::Bare)
    return;

  const std::string &SourceModule = Import.getModuleName();

  // Registers every overload of Name found in SourceModule as flat-imported
  // into ContextLabel. Returns true if Name was found there at all (as a
  // function) -- the caller uses this to decide whether an explicit
  // Symbol/SymbolList request also needs to fall back to checking the
  // (already-flat, module-blind) struct/enum registry before giving up.
  auto RegisterFunctionIfPresent = [&](const std::string &Name,
                                       SourceLocation Loc) -> bool {
    bool Found = false;
    for (const auto &Match : SymTable_.findInModules(Name)) {
      if (Match.ModuleName != SourceModule)
        continue;
      Found = true;
      ResolverSymbolTable::FlatImportedSymbol Sym{SourceModule, {Loc.Line, Loc.Col}};
      if (auto Prev = SymTable_.registerFlatFunctionImport(ContextLabel, Name, Sym)) {
        if (Prev->SourceModule != SourceModule) {
          ImportSymbolCollisionError(Loc, Name, Prev->SourceModule, SourceModule,
                                     toSourceLocation(Prev->ImportLoc))
              .raise();
        }
        // Same module claiming the same name twice (e.g. re-listed in the
        // same import, or two overloads both already handled by the loop
        // above finding multiple Matches) is not a real collision -- just
        // don't re-raise.
      }
    }
    return Found;
  };

  // Registers Name as flat-type-imported into ContextLabel IF SourceModule
  // actually owns it (per TypeOwningModule_, not the flat/module-blind
  // isKnownStruct()/isEnumName() existence check -- a name that merely
  // exists SOMEWHERE in the program, declared by a totally different
  // module, must not satisfy `import SourceModule::Name;`). Returns
  // whether Name is a real struct/enum owned by SourceModule at all.
  auto RegisterTypeIfOwned = [&](const std::string &Name,
                                 SourceLocation Loc) -> bool {
    if (SymTable_.getTypeOwningModule(Name) != SourceModule)
      return false;
    ResolverSymbolTable::FlatImportedSymbol Sym{SourceModule, {Loc.Line, Loc.Col}};
    if (auto Prev = SymTable_.registerFlatTypeImport(ContextLabel, Name, Sym)) {
      if (Prev->SourceModule != SourceModule) {
        ImportSymbolCollisionError(Loc, Name, Prev->SourceModule, SourceModule,
                                   toSourceLocation(Prev->ImportLoc))
            .raise();
      }
    }
    return true;
  };

  // Registers Name as flat-CONSTANT-imported into ContextLabel IF
  // SourceModule actually owns an `export immut` constant by that name.
  // Kept as its own registry (FlatImportedConstants_) for the same reason
  // types are separate from functions -- see that map's doc comment.
  auto RegisterConstantIfPresent = [&](const std::string &Name,
                                       SourceLocation Loc) -> bool {
    if (!SymTable_.findModuleConstant(SourceModule, Name))
      return false;
    ResolverSymbolTable::FlatImportedSymbol Sym{SourceModule, {Loc.Line, Loc.Col}};
    if (auto Prev = SymTable_.registerFlatConstantImport(ContextLabel, Name, Sym)) {
      if (Prev->SourceModule != SourceModule) {
        ImportSymbolCollisionError(Loc, Name, Prev->SourceModule, SourceModule,
                                   toSourceLocation(Prev->ImportLoc))
            .raise();
      }
    }
    return true;
  };

  if (Import.getMode() == ImportMode::Wildcard) {
    // Every function, struct/enum type, AND `export immut` constant
    // SourceModule declares becomes flat-visible.
    for (const auto &Name : SymTable_.getModuleFunctionNames(SourceModule))
      RegisterFunctionIfPresent(Name, Import.getLoc());
    for (const auto &Name : SymTable_.getModuleTypeNames(SourceModule))
      RegisterTypeIfOwned(Name, Import.getLoc());
    for (const auto &Name : SymTable_.getModuleConstantNames(SourceModule))
      RegisterConstantIfPresent(Name, Import.getLoc());
    return;
  }

  // Symbol / SymbolList: each requested name must resolve to SOMETHING
  // SourceModule actually owns -- a function, a struct/enum, or a constant.
  for (const auto &Ref : Import.getRequestedSymbols()) {
    bool FoundFn = RegisterFunctionIfPresent(Ref.Name, Ref.Loc);
    bool FoundType = RegisterTypeIfOwned(Ref.Name, Ref.Loc);
    bool FoundConst = RegisterConstantIfPresent(Ref.Name, Ref.Loc);
    if (!FoundFn && !FoundType && !FoundConst) {
      RankedSuggestions Suggestions;
      // Candidates must cover every name-kind the gate above actually
      // accepts (function, type, OR constant) -- this used to search
      // function names alone, so `import math::[Pi];` (meaning the real
      // constant `PI`) got no suggestion at all even though
      // RegisterConstantIfPresent() right above already considered and
      // rejected that exact possibility.
      std::vector<std::string> Candidates =
          SymTable_.getModuleFunctionNames(SourceModule);
      std::vector<std::string> TypeNames =
          SymTable_.getModuleTypeNames(SourceModule);
      Candidates.insert(Candidates.end(), TypeNames.begin(), TypeNames.end());
      std::vector<std::string> ConstNames =
          SymTable_.getModuleConstantNames(SourceModule);
      Candidates.insert(Candidates.end(), ConstNames.begin(), ConstNames.end());
      auto Sugg = SuggestionIndex::suggestVisible(Ref.Name, Candidates);
      if (!Sugg.empty())
        Suggestions.Module[SourceModule] = Sugg;
      UnknownModuleSymbolError(Ref.Loc, SourceModule, Ref.Name, Suggestions).raise();
    }
  }
}
