#include "Monomorphize.h"

#include "AST.h"
#include "ASTDiff.h"
#include "Errors.h"
#include "ErrorsMonomorphize.h"
#include "Resolver.h"
#include "TypeChecker.h"

#include <cstring>

namespace Monomorphize {

static const char *kindName(InstantiationKind K) {
  switch (K) {
  case InstantiationKind::FreeFunction: return "fn";
  case InstantiationKind::Method:       return "method";
  case InstantiationKind::Struct:       return "struct";
  }
  return "?";
}

void InstantiationCollector::report(std::ostream &OS) const {
  const auto &All = Cache_.all();
  OS << "-- monomorphization instantiation census (" << All.size()
     << " distinct request" << (All.size() == 1 ? "" : "s") << ") --\n";
  for (const auto &Entry : All) {
    const auto &Req = Entry.Request;
    OS << "  [" << kindName(Req.Kind) << "] "
       << (Req.SourceModule.empty() ? "" : Req.SourceModule + "::")
       << Req.DeclKey << "<";
    for (size_t i = 0; i < Req.TypeArgs.size(); ++i) {
      if (i) OS << ", ";
      OS << (Req.TypeArgs[i] ? Req.TypeArgs[i]->getName() : "?");
    }
    OS << ">  (" << Req.Site.Filename << ":" << Req.Site.Line << ", "
       << Entry.Sites.size() << " call site"
       << (Entry.Sites.size() == 1 ? "" : "s") << ")\n";
  }
}

// ─── Concrete-map type substitution ─────────────────────────────────────────
// Relocated verbatim from TypeCheckerCall.cpp's file-local
// substituteReturnType() -- see Monomorphize.h's doc comment for why.

TurfTypePtr substituteConcreteTypes(
    const TurfTypePtr &Ty,
    const std::unordered_map<std::string, TurfTypePtr> &Subst) {
  if (!Ty || Subst.empty())
    return Ty;

  if (const auto *GP = dynamic_cast<const TurfGenericParamType *>(Ty.get())) {
    auto It = Subst.find(GP->getName());
    return It != Subst.end() ? It->second : Ty;
  }
  // A generic STRUCT's own field types are also matched here as NamedType,
  // not TurfGenericParamType -- see this function's declaration comment
  // (Monomorphize.h) and TypeCheckerCall.cpp's visitMethodCall for the full
  // story on why both representations must be handled.
  if (const auto *NT = dynamic_cast<const NamedType *>(Ty.get())) {
    auto It = Subst.find(NT->getName());
    return It != Subst.end() ? It->second : Ty;
  }
  // Checked BEFORE the plain TurfPointerType branch below -- TurfReferenceType
  // (&T) IS-A TurfPointerType, so the more general dynamic_cast below matches
  // it too; without this earlier check a substituted `&T` would silently
  // rebuild as a plain `T*`.
  if (const auto *RT = dynamic_cast<const TurfReferenceType *>(Ty.get())) {
    TurfTypePtr NewPointee = substituteConcreteTypes(RT->getPointee(), Subst);
    return NewPointee == RT->getPointee()
              ? Ty
              : std::make_shared<TurfReferenceType>(NewPointee);
  }
  if (const auto *PT = dynamic_cast<const TurfPointerType *>(Ty.get())) {
    TurfTypePtr NewPointee = substituteConcreteTypes(PT->getPointee(), Subst);
    return NewPointee == PT->getPointee()
              ? Ty
              : std::make_shared<TurfPointerType>(NewPointee);
  }
  if (isArrayType(Ty.get())) {
    const auto *AT = dynamic_cast<const ArrayType *>(Ty.get());
    TurfTypePtr NewElem = substituteConcreteTypes(AT->getElementTypePtr(), Subst);
    return NewElem == AT->getElementTypePtr()
              ? Ty
              : makeArrayType(NewElem, AT->getSize());
  }
  if (const auto *GT = dynamic_cast<const GenericType *>(Ty.get())) {
    std::vector<TurfTypePtr> NewArgs;
    bool Changed = false;
    for (const auto &Arg : GT->getTypeArgs()) {
      TurfTypePtr NewArg = substituteConcreteTypes(Arg, Subst);
      Changed |= (NewArg != Arg);
      NewArgs.push_back(std::move(NewArg));
    }
    return Changed ? std::make_shared<GenericType>(GT->getBaseName(),
                                                    std::move(NewArgs))
                   : Ty;
  }
  if (const auto *EU = dynamic_cast<const TurfErrorUnionType *>(Ty.get())) {
    TurfTypePtr NewSuccess = substituteConcreteTypes(EU->getSuccessTypePtr(), Subst);
    return NewSuccess == EU->getSuccessTypePtr()
              ? Ty
              : std::make_shared<TurfErrorUnionType>(NewSuccess);
  }
  return Ty;
}

std::string instantiatedDeclName(const std::string &BaseName,
                                 const std::vector<TurfTypePtr> &TypeArgs) {
  std::string S = BaseName + "_gen" + std::to_string(TypeArgs.size());
  for (const auto &A : TypeArgs)
    if (A) S += "_" + A->getMangledName();
  return S;
}

// ─── Body type-substitution walker ──────────────────────────────────────────

namespace {

/**
 * @brief Walk every node in a freshly-cloned function body, replacing any
 *        TurfGenericParamType/NamedType occurrence found in @p Subst with
 *        its concrete substitution, on the handful of node kinds that carry
 *        an explicit, parse-time TurfTypePtr (VarDeclExprAST's declared
 *        type, CastExprAST's destination type, SizeOfExprAST's type
 *        argument, ArrayDeclExprAST's element type).
 *
 * Why this is needed at all, and why it is NOT redundant with the
 * Resolver's own CurrentFuncTypeParams_ substitution
 * (ResolverStmt.cpp:visitVarDecl): that mechanism only ever produces
 * TurfGenericParamType("T") -- an OPEN parameter, not a concrete type --
 * because it exists to make the erasure-era, type-checked-once-generically
 * body work at all, not to specialize it. By the time the monomorphizer
 * runs, the TEMPLATE this clone came from has already been through that
 * exact substitution once (as part of the whole program's ordinary
 * resolveAll()/checkAll() pass, before sub-pass 2b.5 ever starts) -- so
 * clone() copies a body that ALREADY has TurfGenericParamType("T")
 * uniformly everywhere the Resolver's own pass put it. This walker's job
 * is the second, DIFFERENT substitution: TurfGenericParamType("T") -> a
 * REAL concrete type (e.g. IntType), which nothing else in the compiler
 * does today because nothing else needs to.
 *
 * Every other node kind is walked purely for recursion (reaching a nested
 * occurrence), never mutated. Coverage mirrors ASTDiff.h's
 * CollectAllNodeIDsInternal(), the codebase's other "walk an entire
 * function body" traversal, plus the few node kinds it doesn't enumerate
 * (Compare/Recover/UnsafeBlock/AddressOf/ScopeAccess) that this compiler's
 * own AST has grown since. See INV-LINT-1 (INVARIANTS.md) for the general
 * risk class this kind of manual per-node-kind dispatch carries: a future
 * node kind that can appear inside a function body and carries its own
 * parse-time TurfTypePtr will silently NOT get substituted here unless
 * this function is updated too.
 */
void substituteTypesInBody(const ExprAST *Node,
                           const std::unordered_map<std::string, TurfTypePtr> &Subst) {
  if (!Node)
    return;

  if (auto *VD = const_cast<VarDeclExprAST *>(dynamic_cast<const VarDeclExprAST *>(Node))) {
    VD->setTypePtr(substituteConcreteTypes(VD->getTypePtr(), Subst));
    substituteTypesInBody(VD->getInitializer(), Subst);
    return;
  }
  if (auto *Cast = const_cast<CastExprAST *>(dynamic_cast<const CastExprAST *>(Node))) {
    Cast->setDestType(substituteConcreteTypes(Cast->getDestTypePtr(), Subst));
    substituteTypesInBody(Cast->getOperand(), Subst);
    return;
  }
  if (auto *SO = const_cast<SizeOfExprAST *>(dynamic_cast<const SizeOfExprAST *>(Node))) {
    SO->setTypeArg(substituteConcreteTypes(SO->getTypeArg(), Subst));
    return;
  }
  if (auto *AD = const_cast<ArrayDeclExprAST *>(dynamic_cast<const ArrayDeclExprAST *>(Node))) {
    AD->setElementTypePtr(substituteConcreteTypes(AD->getElementTypePtr(), Subst));
    for (const auto &Init : AD->getInitList())
      substituteTypesInBody(Init.get(), Subst);
    substituteTypesInBody(AD->getInitExpr().get(), Subst);
    return;
  }

  if (auto *Bin = dynamic_cast<const BinaryExprAST *>(Node)) {
    substituteTypesInBody(Bin->getLHS(), Subst);
    substituteTypesInBody(Bin->getRHS(), Subst);
    return;
  }
  if (auto *Un = dynamic_cast<const UnaryExprAST *>(Node)) {
    substituteTypesInBody(Un->getOperand(), Subst);
    return;
  }
  if (auto *AO = dynamic_cast<const AddressOfExprAST *>(Node)) {
    substituteTypesInBody(AO->getOperand(), Subst);
    return;
  }
  if (auto *Assign = dynamic_cast<const AssignmentExprAST *>(Node)) {
    substituteTypesInBody(Assign->getLHS(), Subst);
    substituteTypesInBody(Assign->getRHS(), Subst);
    return;
  }
  if (auto *CA = dynamic_cast<const CompoundAssignmentExprAST *>(Node)) {
    substituteTypesInBody(CA->getLHS(), Subst);
    substituteTypesInBody(CA->getRHS(), Subst);
    return;
  }
  if (auto *PF = dynamic_cast<const PostfixExprAST *>(Node)) {
    substituteTypesInBody(PF->getOperand(), Subst);
    return;
  }
  if (auto *Block = dynamic_cast<const BlockExprAST *>(Node)) {
    for (const auto &E : Block->getExpressions())
      substituteTypesInBody(E.get(), Subst);
    return;
  }
  if (auto *UB = dynamic_cast<const UnsafeBlockExprAST *>(Node)) {
    substituteTypesInBody(UB->getBody(), Subst);
    return;
  }
  if (auto *If = dynamic_cast<const IfExprAST *>(Node)) {
    for (const auto &Branch : If->getBranches()) {
      substituteTypesInBody(Branch.Cond.get(), Subst);
      substituteTypesInBody(Branch.Body.get(), Subst);
    }
    substituteTypesInBody(If->getElseBody(), Subst);
    return;
  }
  if (auto *While = dynamic_cast<const WhileExprAST *>(Node)) {
    substituteTypesInBody(While->getCond(), Subst);
    substituteTypesInBody(While->getBody(), Subst);
    return;
  }
  if (auto *DoWhile = dynamic_cast<const DoWhileExprAST *>(Node)) {
    substituteTypesInBody(DoWhile->getBody(), Subst);
    substituteTypesInBody(DoWhile->getCond(), Subst);
    return;
  }
  if (auto *For = dynamic_cast<const ForExprAST *>(Node)) {
    substituteTypesInBody(For->getStart(), Subst);
    substituteTypesInBody(For->getEnd(), Subst);
    substituteTypesInBody(For->getStep(), Subst);
    substituteTypesInBody(For->getBody(), Subst);
    return;
  }
  if (auto *ForEach = dynamic_cast<const ForEachExprAST *>(Node)) {
    substituteTypesInBody(ForEach->getIterable(), Subst);
    substituteTypesInBody(ForEach->getBody(), Subst);
    return;
  }
  if (auto *FC = dynamic_cast<const FuncCallExprAST *>(Node)) {
    substituteTypesInBody(FC->getCallee(), Subst);
    for (const auto &Arg : FC->getArgs())
      substituteTypesInBody(Arg.get(), Subst);
    return;
  }
  if (auto *MC = dynamic_cast<const MethodCallExprAST *>(Node)) {
    substituteTypesInBody(MC->getBase(), Subst);
    for (const auto &Arg : MC->getArgs())
      if (Arg) substituteTypesInBody(Arg.get(), Subst);
    return;
  }
  if (auto *Ret = dynamic_cast<const ReturnExprAST *>(Node)) {
    substituteTypesInBody(Ret->getVal(), Subst);
    return;
  }
  if (auto *AA = dynamic_cast<const ArrayAccessExprAST *>(Node)) {
    substituteTypesInBody(AA->getBase(), Subst);
    substituteTypesInBody(AA->getIndex(), Subst);
    return;
  }
  if (auto *AL = dynamic_cast<const ArrayLiteralExprAST *>(Node)) {
    for (const auto &Elem : AL->getElements())
      substituteTypesInBody(Elem.get(), Subst);
    return;
  }
  if (auto *FA = dynamic_cast<const FieldAccessExprAST *>(Node)) {
    substituteTypesInBody(FA->getBase(), Subst);
    return;
  }
  if (auto *Cmp = dynamic_cast<const CompareExprAST *>(Node)) {
    substituteTypesInBody(Cmp->getSubject(), Subst);
    for (const auto &Arm : Cmp->getArms()) {
      for (const auto &V : Arm.Values)
        substituteTypesInBody(V.get(), Subst);
      substituteTypesInBody(Arm.Body.get(), Subst);
    }
    substituteTypesInBody(Cmp->getOtherBody(), Subst);
    return;
  }
  if (auto *Rec = dynamic_cast<const RecoverExprAST *>(Node)) {
    substituteTypesInBody(Rec->getWrapped(), Subst);
    substituteTypesInBody(Rec->getProduceExpr(), Subst);
    substituteTypesInBody(Rec->getBlock(), Subst);
    return;
  }
  // Leaves with no children and no parse-time TurfTypePtr of their own:
  // Break/Continue/Number/Bool/Char/String/Null/Variable/ScopeAccess.
}

} // namespace

// ─── Drain: free functions ───────────────────────────────────────────────────

void drainFreeFunctionRequests(
    std::vector<std::unique_ptr<ExprAST>> &PrePassASTs,
    InstantiationCollector &Collector, Resolver &PreR) {
  // Registry: (SourceModule, Name) -> template declaration. Built once,
  // lazily, from PrePassASTs -- both current-file top-level declarations
  // AND every imported module's own body (ModuleImportExprAST::getBody(),
  // a BlockExprAST holding that module's own top-level declarations --
  // confirmed live: imports are parsed into real AST subtrees, not just
  // registered as symbol-table signatures with no body). Only functions
  // with a non-empty TypeParams() are registered -- an ordinary
  // (non-generic) declaration is never a clone target.
  std::unordered_map<std::string, FuncDefExprAST *> Registry;
  auto RegistryKey = [](const std::string &Module, const std::string &Name) {
    return Module + "::" + Name;
  };
  std::function<void(const std::vector<std::unique_ptr<ExprAST>> &, const std::string &)>
      BuildRegistry = [&](const std::vector<std::unique_ptr<ExprAST>> &Nodes,
                          const std::string &Module) {
    for (const auto &N : Nodes) {
      if (auto *FD = dynamic_cast<FuncDefExprAST *>(N.get())) {
        if (!FD->getTypeParams().empty())
          Registry[RegistryKey(Module, FD->getName())] = FD;
      } else if (auto *MI = dynamic_cast<ModuleImportExprAST *>(N.get())) {
        if (auto *Body = dynamic_cast<const BlockExprAST *>(MI->getBody()))
          BuildRegistry(Body->getExpressions(), MI->getModuleName());
      }
    }
  };
  BuildRegistry(PrePassASTs, "");

  int TotalDrained = 0;

  while (!Collector.worklistEmpty()) {
    size_t Idx = Collector.popWorklist();
    // entryAt() indexes into the SAME growing cache draining itself keeps
    // appending to (a nested generic call discovered while typechecking a
    // fresh clone calls Collector.record() again) -- Idx stays valid since
    // InstantiationCache only ever appends, never reorders or erases.
    InstantiationEntry &Entry = Collector.entryAt(Idx);

    if (Entry.Request.Kind != InstantiationKind::FreeFunction)
      continue; // Method/Struct: S12b/S13a's job. Stays recorded, undrained.

    if (Entry.Sites.empty())
      continue; // Defensive -- record() always pushes at least one site.

    auto RegIt = Registry.find(
        RegistryKey(Entry.Request.SourceModule, Entry.Request.DeclKey));
    if (RegIt == Registry.end()) {
      // Should be unreachable: every FreeFunction request's DeclKey/
      // SourceModule came from a real, already-resolved ResolvedCallTarget
      // at TypeCheck time (TypeCheckerCall.cpp), which means a real
      // FuncDefExprAST declared it. If this fires, it is a compiler bug,
      // not a user error -- mirrors CodegenFunc.cpp's identical "should be
      // unreachable if the Resolver ran correctly" defensive diagnostic.
      SemanticError(Entry.Request.Site,
                    "Internal error: no generic declaration found for '" +
                        Entry.Request.DeclKey +
                        "' during monomorphization. This is a compiler bug.")
          .raise();
      return;
    }
    FuncDefExprAST *Template = RegIt->second;

    // Struct-bound declarations routed through the free-function call path
    // -- `Box::new(...)`/`Box::default()` (a "self-stripped candidate",
    // TypeCheckerCall.cpp's own comment) and, in principle, a static
    // method called the same way -- are NOT a valid S12a target even
    // though they arrive as FreeFunction-kind requests: every one of them
    // takes an implicit `self: StructName*` as Params[0], and `self`'s
    // static type carries NO type argument at all (a struct method's self
    // is declared as a bare pointer with no <...>, TypeCheckerCall.cpp's
    // visitMethodCall doc comment). A body referencing `self.<field>`
    // (which a constructor always does) resolves that field's type from
    // GlobalSymbolTable's SHARED, never-per-instantiation-substituted
    // struct registration -- substituting this clone's OWN Params/body
    // cannot fix that; only S13a's real per-instantiation struct layout
    // can. Found live: `Box::new(42)` re-typechecked as "Cannot assign
    // value of type 'int' to variable of type 'T'" before this exclusion
    // (self.val's type stayed literally NamedType("T"), un-substituted, no
    // matter what v's own type became). Left recorded, undrained -- same
    // treatment as Method/Struct kinds; S12b/S13a act on it.
    if (!Template->getParams().empty() && Template->getParams()[0].Name == "self")
      continue;

    if (++TotalDrained > kMaxInstantiationDepth) {
      std::vector<std::string> Chain;
      for (const auto &E : Collector.cache().all())
        Chain.push_back(E.Request.DeclKey);
      GenericInstantiationDepthError(Entry.Request.Site, Chain).raise();
      return;
    }

    // Build the name->concrete substitution map from TypeParams (declared
    // order) paired positionally with the request's own ordered TypeArgs
    // (TypeCheckerCall.cpp's collectGenericParamNameOrder() already
    // guaranteed these two lists agree in order at record()-time).
    std::unordered_map<std::string, TurfTypePtr> Subst;
    const auto &TypeParams = Template->getTypeParams();
    for (size_t i = 0; i < TypeParams.size() && i < Entry.Request.TypeArgs.size(); ++i)
      Subst[TypeParams[i]] = Entry.Request.TypeArgs[i];

    // Clone, then substitute concrete types throughout: signature first
    // (Params/ReturnType -- direct field access, no walker needed), then
    // the body (the walker above). The template's OWN Params/ReturnType/
    // body already carry TurfGenericParamType uniformly (the whole
    // program's ordinary resolveAll()/checkAll() already ran over it
    // before sub-pass 2b.5 starts) -- clone() copies that state verbatim,
    // so there is no NamedType("T") left anywhere in the clone to worry
    // about; only TurfGenericParamType("T").
    auto ClonedUP = CloneExprAST(Template);
    auto *Clone = static_cast<FuncDefExprAST *>(ClonedUP.get());
    for (auto &Param : Clone->getParamsMut())
      Param.Type = substituteConcreteTypes(Param.Type, Subst);
    Clone->setReturnType(substituteConcreteTypes(Clone->getReturnTypePtr(), Subst));
    substituteTypesInBody(Clone->getBody(), Subst);

    // Rename to a unique, collision-free internal name (§9.5's mangling
    // requirement) BEFORE resolving -- Resolver::visitFuncDef() (called
    // below) registers whatever name the node currently has into the
    // ordinary function-signature table; leaving it as e.g. "identity"
    // would either silently add a second, indistinguishable overload
    // candidate for every ordinary, unrelated call to "identity" elsewhere
    // in the program, or collide outright. instantiatedDeclName() is a
    // pure function of (base name, type arguments) (§9.5), so every call
    // site rewritten to the SAME instantiation independently agrees on
    // this name -- not that anything else needs to independently compute
    // it: every rewrite below reads it directly off Clone->ResolvedDef.
    Clone->setName(instantiatedDeclName(Entry.Request.DeclKey,
                                        Entry.Request.TypeArgs));
    // No longer generic: every TurfGenericParamType in Params/ReturnType/
    // body has already been replaced with a concrete type above. Clearing
    // TypeParams makes that true structurally too (visitFuncDef's own
    // substituteTypeParams() call becomes a documented no-op, and
    // sub-pass 2c/3a/3b's template-skip check, below and in main.cpp,
    // stops treating this clone as an uncompiled template).
    Clone->clearTypeParams();

    // Re-resolve the clone against the REAL, already-hoisted program
    // symbol table. PreR is the SAME Resolver instance whose resolveAll()
    // already ran Pass 1 (hoisting) over the whole program -- calling its
    // single-node resolve() entry point (Pass 2 behaviour only) on this
    // one clone is safe for exactly that reason: hoisting doesn't need to
    // run again (nothing new needs to be forward-referenceable), and
    // resolve()'s own scope enter/exit is self-contained per call, the
    // same guarantee visitBlock/visitFuncDef already give any nested
    // scope. This was flagged as this group's #1 named risk (S11a plan) --
    // resolved by reusing the existing single-node entry point rather than
    // inventing a new one; clone()'s own contract ("resolved fields
    // deliberately not copied; cloned ASTs must be re-resolved") is
    // exactly what this satisfies.
    bool ResolveFailed = false;
    {
      const bool SavedRecoverActive = g_recoverActive;
      jmp_buf SavedJmpBuf;
      std::memcpy(SavedJmpBuf, g_recoverJmp, sizeof(jmp_buf));
      g_recoverActive = true;
      if (setjmp(g_recoverJmp) == 0) {
        PreR.resolve(Clone);
      } else {
        ResolveFailed = true;
      }
      g_recoverActive = SavedRecoverActive;
      std::memcpy(g_recoverJmp, SavedJmpBuf, sizeof(jmp_buf));
    }
    if (ResolveFailed) {
      SemanticError(Entry.Request.Site,
                    "Internal error: monomorphized instantiation of '" +
                        Entry.Request.DeclKey +
                        "' failed to re-resolve. This is a compiler bug.")
          .raise();
      return;
    }

    // Re-typecheck the clone -- THIS is where an instantiation-site error
    // (§9.5, "where errors are reported") is discovered: a concrete T that
    // doesn't support an operation the body needs fails HERE, generically,
    // for the first time (the template itself, checked once generically
    // under Rule 1's "matches anything," could never catch this). A custom
    // error handler captures the inner failure instead of letting it
    // longjmp past this whole function, so it can be re-raised anchored at
    // the INSTANTIATION SITE (every requesting call, via withRelated()),
    // not at the clone's own copied-from-the-template location, which
    // would read as an error in code the user never wrote at that line.
    bool CheckFailed = false;
    SourceLocation InnerLoc;
    std::string InnerMsg, InnerHint;
    TypeChecker CloneTC(PreR.symbolTable(),
                        [&](SourceLocation Loc, const std::string &Msg,
                            const std::vector<RelatedLocation> &,
                            const std::string &Hint, const std::string &) {
                          if (!CheckFailed) {
                            CheckFailed = true;
                            InnerLoc = Loc;
                            InnerMsg = Msg;
                            InnerHint = Hint;
                          }
                          longjmp(g_recoverJmp, 1);
                        });
    // S12a's own instantiation collector must keep collecting through this
    // second, nested TypeChecker instance -- a generic call INSIDE this
    // clone's body (e.g. `wrap<T>` calling `wrap(Box{val:x})`) needs to
    // reach the SAME Collector.record() so its own request lands on the
    // SAME worklist this very loop is draining (§9.5 step 3, "push any new
    // requests its body produced" -- the fixed point this while loop
    // already provides, for free, once this line wires it through).
    CloneTC.setMonomorphizationCollector(&Collector);
    {
      const bool SavedRecoverActive = g_recoverActive;
      jmp_buf SavedJmpBuf;
      std::memcpy(SavedJmpBuf, g_recoverJmp, sizeof(jmp_buf));
      g_recoverActive = true;
      if (setjmp(g_recoverJmp) == 0) {
        CloneTC.check(Clone);
      }
      g_recoverActive = SavedRecoverActive;
      std::memcpy(g_recoverJmp, SavedJmpBuf, sizeof(jmp_buf));
    }
    if (CheckFailed) {
      std::string FullMsg = "In this instantiation: " + InnerMsg;
      if (!InnerHint.empty())
        FullMsg += " (" + InnerHint + ")";
      SemanticError Err(Entry.Request.Site, FullMsg);
      Err.withRelated(Template->getNameLoc(),
                      "in generic declaration '" + Entry.Request.DeclKey + "'");
      for (size_t i = 1; i < Entry.Sites.size(); ++i)
        if (auto *FC = dynamic_cast<FuncCallExprAST *>(Entry.Sites[i]))
          Err.withRelated(FC->getLoc(), "also instantiated here");
      Err.raise();
      return;
    }

    // Register: append to PrePassASTs so main.cpp's ORDINARY sub-pass
    // 2c/3a/3b codegen loops pick this up exactly like any other top-level
    // declaration -- no Codegen change needed at all, since the clone is,
    // by this point, structurally indistinguishable from a hand-written
    // concrete function. Safe to append while this same loop keeps running
    // (nothing else iterates PrePassASTs until 2c starts, strictly after
    // sub-pass 2b.5 -- this whole function -- returns).
    auto ClonedSymbol = Clone->ResolvedDef; // set by PreR.resolve() above
    PrePassASTs.push_back(std::move(ClonedUP));

    // Rewrite every requesting call site to the clone. Every site in
    // Entry.Sites is a FuncCallExprAST (FreeFunction kind is only ever
    // recorded from TypeCheckerCall.cpp's visitFuncCall).
    for (ExprAST *Site : Entry.Sites) {
      if (auto *FC = dynamic_cast<FuncCallExprAST *>(Site))
        FC->ResolvedCallTarget = ClonedSymbol;
    }
  }
}

} // namespace Monomorphize
