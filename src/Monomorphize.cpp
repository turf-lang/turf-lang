#include "Monomorphize.h"

#include "AST.h"
#include "ASTDiff.h"
#include "Errors.h"
#include "ErrorsMonomorphize.h"
#include "Resolver.h"
#include "SymbolTable.h" // GlobalSymbolTable -- RegisterType()'s target, distinct from Resolver's own SymTable_
#include "TypeChecker.h"
#include "TypeWalk.h" // SMP-002: shared substitute() walker

#include <algorithm>
#include <cassert>
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

// SMP-002: thin wrapper over the shared TypeWalk::substitute() walker
// (TypeWalk.h) -- this function's own recursive body is now the single one
// that exists, shared verbatim with Resolver::substituteTypeParams's
// declaration-direction call (ResolverCore.cpp), which builds its own Subst
// as `{name -> TurfGenericParamType(name)}` instead of a concrete map.
TurfTypePtr substituteConcreteTypes(
    const TurfTypePtr &Ty,
    const std::unordered_map<std::string, TurfTypePtr> &Subst) {
  return TypeWalk::substitute(Ty, Subst);
}

std::string instantiatedDeclName(const std::string &BaseName,
                                 const std::vector<TurfTypePtr> &TypeArgs,
                                 const std::string &Module) {
  std::string S = Module.empty() ? "" : Module + "_";
  S += BaseName + "_gen" + std::to_string(TypeArgs.size());
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
      // LEC-003: a guard is an arbitrary expression (a cast to `T`, a call to
      // a generic function) and needs the same substitution as the body.
      for (const auto &G : Arm.Guards)
        substituteTypesInBody(G.get(), Subst);
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
  // LEC-004: `try f(x as T)` -- the operand is an ordinary expression that can
  // name `T`.
  if (auto *Try = dynamic_cast<const TryExprAST *>(Node)) {
    substituteTypesInBody(Try->getOperand(), Subst);
    return;
  }
  // LEC-004: an error-variant pattern names its error type (never a type
  // PARAMETER -- an error type can't be generic), so, like a struct pattern,
  // only its sub-patterns are walked, so that a future sub-pattern that does
  // carry a type cannot be silently skipped.
  if (auto *EV = dynamic_cast<const ErrorVariantPatternExprAST *>(Node)) {
    for (const auto &S : EV->getSubs())
      substituteTypesInBody(S.get(), Subst);
    return;
  }
  // LEC-003: a struct pattern names its struct (`Pair { a, b }`), never a type
  // PARAMETER -- the arguments come from the subject's own type -- so its
  // TypeSpelling needs no substitution. Its sub-patterns are walked only so a
  // future sub-pattern that does carry a type cannot be silently skipped
  // (INV-LINT-1's risk class, above).
  if (auto *SP = dynamic_cast<const StructPatternExprAST *>(Node)) {
    for (const auto &F : SP->getFields())
      substituteTypesInBody(F.Sub.get(), Subst);
    return;
  }
  // Leaves with no children and no parse-time TurfTypePtr of their own:
  // Break/Continue/Number/Bool/Char/String/Null/Variable/ScopeAccess, and the
  // wildcard/binding patterns.
}

} // namespace

// ─── Drain: free functions ───────────────────────────────────────────────────

namespace {

std::string registryKey(const std::string &Module, const std::string &Name) {
  return Module + "::" + Name;
}

/// Shared by both drain paths (S12a free functions, S12b methods): builds
/// the (SourceModule, Name) -> template declaration registry by walking
/// PrePassASTs and recursing into every ModuleImportExprAST's own body
/// (confirmed live: an import's body is a real, walkable BlockExprAST
/// subtree, not just a symbol-table registration). Only declarations with
/// a non-empty TypeParams() are registered -- keyed by
/// FuncDefExprAST::getName(), which is already "StructName.MethodName" for
/// a struct-bound declaration (ParserDecl.cpp), so this one registry
/// serves free functions AND methods with no special-casing.
std::unordered_map<std::string, FuncDefExprAST *>
buildDeclRegistry(const std::vector<std::unique_ptr<ExprAST>> &PrePassASTs) {
  std::unordered_map<std::string, FuncDefExprAST *> Registry;
  std::function<void(const std::vector<std::unique_ptr<ExprAST>> &, const std::string &)>
      Walk = [&](const std::vector<std::unique_ptr<ExprAST>> &Nodes,
                const std::string &Module) {
    for (const auto &N : Nodes) {
      if (auto *FD = dynamic_cast<FuncDefExprAST *>(N.get())) {
        if (!FD->getTypeParams().empty())
          Registry[registryKey(Module, FD->getName())] = FD;
      } else if (auto *MI = dynamic_cast<ModuleImportExprAST *>(N.get())) {
        if (auto *Body = dynamic_cast<const BlockExprAST *>(MI->getBody()))
          Walk(Body->getExpressions(), MI->getModuleName());
      }
    }
  };
  Walk(PrePassASTs, "");
  return Registry;
}

/// TYPE-002 (S14a, extended S15/D6): the number of type parameters struct
/// @p Name declares in its OWN `<...>` list, found anywhere in
/// @p PrePassASTs (top level or inside an imported module's body); 0 if
/// @p Name isn't declared generic (or isn't found at all) -- used to decide
/// whether a self-bound clone's `self` parameter needs GenericType wrapping
/// (cloneAndSubstituteSelfBound()'s own doc comment: `> 0` is the old
/// isKnownGenericStructName() boolean) AND, when it does, exactly how many
/// of the REQUEST's own TypeArgs belong to the struct itself, as opposed to
/// the METHOD's own additional type parameters (D6: `fn<T, U> Wrap.pick`
/// declares TWO, but Wrap<T> itself only owns one -- self must wrap as
/// `Wrap<T>*`, never `Wrap<T,U>*`, or its per-instantiation struct lookup
/// (a name built from ONLY the struct's own args, drainStructRequests())
/// misses entirely). Needed at all because, as of S14a, a Method-kind
/// InstantiationRequest's TypeArgs can come from TWO structurally different
/// sources that share no other distinguishing field: the RECEIVER's own
/// GenericType (`l: List<int>` -> self must become `List<int>*`) or the
/// METHOD's own type parameters unified against its call arguments
/// (`b.echo(42)` where `b: Box`, Box NOT generic -- self must stay a bare
/// `Box*`, since wrapping it as `GenericType("Box", [int])*` would be
/// wrapping a non-generic struct's name with an unrelated type argument
/// that has nothing to do with Box itself).
size_t genericStructOwnTypeParamCount(
    const std::string &Name,
    const std::vector<std::unique_ptr<ExprAST>> &PrePassASTs) {
  size_t Count = 0;
  bool Found = false;
  std::function<void(const std::vector<std::unique_ptr<ExprAST>> &)> Walk =
      [&](const std::vector<std::unique_ptr<ExprAST>> &Nodes) {
    if (Found) return;
    for (const auto &N : Nodes) {
      if (auto *SD = dynamic_cast<StructDeclExprAST *>(N.get())) {
        if (SD->getName() == Name) {
          Count = SD->getTypeParams().size();
          Found = true;
          return;
        }
      } else if (auto *MI = dynamic_cast<ModuleImportExprAST *>(N.get())) {
        if (auto *Body = dynamic_cast<const BlockExprAST *>(MI->getBody()))
          Walk(Body->getExpressions());
        if (Found) return;
      }
    }
  };
  Walk(PrePassASTs);
  return Count;
}

/// Shared tail of both drain paths, once the caller has produced a clone
/// whose Params/ReturnType/body are ALREADY fully substituted (and, for a
/// method, self's own type already rewritten -- see drainMethodRequests()):
/// rename to a unique internal name, clear TypeParams, re-resolve,
/// re-typecheck with instantiation-site error capture, and append to
/// PrePassASTs. Returns the registered symbol on success, or nullptr if a
/// diagnostic was already raised (caller must stop, not retry).
std::shared_ptr<ResolverFunctionSymbol> finishInstantiation(
    FuncDefExprAST *Template, FuncDefExprAST *Clone,
    std::unique_ptr<ExprAST> ClonedUP, const InstantiationEntry &Entry,
    std::vector<std::unique_ptr<ExprAST>> &PrePassASTs,
    InstantiationCollector &Collector, Resolver &PreR) {
  // Rename to a unique, collision-free internal name (§9.5's mangling
  // requirement) BEFORE resolving -- Resolver::visitFuncDef() (called
  // below) registers whatever name the node currently has into the
  // ordinary function-signature table; leaving it as e.g. "identity"
  // would either silently add a second, indistinguishable overload
  // candidate for every ordinary, unrelated call to "identity" elsewhere
  // in the program, or collide outright. instantiatedDeclName() is a
  // pure function of (base name, type arguments, source module) (§9.5),
  // so every call site rewritten to the SAME instantiation independently
  // agrees on this name -- not that anything else needs to independently
  // compute it: every rewrite below reads it directly off Clone->ResolvedDef.
  //
  // TYPE-002 (S15/D5): Entry.Request.SourceModule is passed here -- two
  // DIFFERENT modules declaring the same-named generic free function
  // (`fn<T> tag(x: T): int` in both amod.tr and bmod.tr) are correctly
  // distinct InstantiationCache entries (sameInstantiationAs() already
  // keys on SourceModule) but used to produce ONE shared, colliding
  // declared name ("tag_gen1_int") once cloned -- "Duplicate function
  // definition" on a program the pre-TYPE-002 compiler accepted (it
  // mangled each module's own "tag" independently). Struct instantiation
  // naming (drainStructRequests(), below) deliberately does NOT do this --
  // struct names are already globally unique by language rule (E034).
  Clone->setName(instantiatedDeclName(Entry.Request.DeclKey, Entry.Request.TypeArgs,
                                      Entry.Request.SourceModule));
  // No longer generic: every TurfGenericParamType in Params/ReturnType/
  // body has already been replaced with a concrete type by the caller.
  // Clearing TypeParams makes that true structurally too (visitFuncDef's
  // own substituteTypeParams() call becomes a documented no-op, and
  // sub-pass 2c/3a/3b's template-skip check, main.cpp, stops treating this
  // clone as an uncompiled template).
  Clone->clearTypeParams();

  // Re-resolve the clone against the REAL, already-hoisted program symbol
  // table. PreR is the SAME Resolver instance whose resolveAll() already
  // ran Pass 1 (hoisting) over the whole program -- calling its
  // single-node resolve() entry point (Pass 2 behaviour only) on this one
  // clone is safe for exactly that reason: hoisting doesn't need to run
  // again, and resolve()'s own scope enter/exit is self-contained per
  // call. This was flagged as this group's #1 named risk (S11a plan) --
  // resolved by reusing the existing single-node entry point rather than
  // inventing a new one.
  bool ResolveFailed = false;
  {
    const bool SavedRecoverActive = g_recoverActive;
    jmp_buf SavedJmpBuf;
    std::memcpy(SavedJmpBuf, g_recoverJmp, sizeof(jmp_buf));
    g_recoverActive = true;
    if (setjmp(g_recoverJmp) == 0) {
      // resolveMonomorphizedClone(), not resolve(): the clone must resolve
      // its own struct-name references (self's type) the same way the
      // ORIGINAL declaration did, AND accept substituted type arguments
      // from modules the template's own declaring module never imports --
      // see that method's own doc comment (Resolver.h) for the two real
      // bugs this fixes.
      PreR.resolveMonomorphizedClone(Clone, Entry.Request.SourceModule);
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
    return nullptr;
  }

  // Clear the just-registered symbol's SourceModule -- resolveMonomorphizedClone()
  // needed "list" (say) as the MODULE CONTEXT for VALIDATION (self's type,
  // above), but the clone must NOT carry that module forward into its own
  // registered symbol. Two independent mangling computations exist in this
  // compiler and normally agree only because both are derived from "which
  // module is this declaration actually inside," computed at each pass's own
  // natural traversal point:
  //   - ResolverFunctionSymbol::mangledName() (ResolverSymbolBase.h) prepends
  //     Sym.SourceModule.
  //   - FuncDefExprAST::codegen() (CodegenFunc.cpp) prepends
  //     GlobalSymbolTable::GetCurrentModule() instead -- a CODEGEN-TIME
  //     GLOBAL, normally set/cleared around a ModuleImportExprAST's own
  //     codegen() traversal of that module's body.
  // The clone is appended directly to PrePassASTs (finishInstantiation,
  // below) and codegenned from THAT flat loop, never from inside any
  // module-import traversal -- GlobalSymbolTable::IsInModule() is false the
  // whole time. Leaving Sym.SourceModule as "list" would make the CALL
  // SITE's mangledName() (which reads Sym.SourceModule) disagree with what
  // the clone's OWN codegen() actually registers the LLVM function under
  // (no module prefix at all) -- `TheModule->getFunction(MangledName)`
  // silently misses, MethodCallExprAST::codegen() returns nullptr with no
  // diagnostic, and a `||`/`&&` short-circuit whose RHS was that call emits
  // an orphaned merge block with no terminator ("does not have terminator!
  // label %or.merge") -- found live, not a hypothetical. instantiatedDeclName()
  // already makes the clone's OWN name globally unique (Monomorphize.h), so
  // it needs no module qualifier at all once renamed.
  if (Clone->ResolvedDef)
    Clone->ResolvedDef->SourceModule.clear();

  // Re-typecheck the clone -- THIS is where an instantiation-site error
  // (§9.5, "where errors are reported") is discovered. A custom error
  // handler captures the inner failure instead of letting it longjmp past
  // this whole function, so it can be re-raised anchored at the
  // INSTANTIATION SITE (every requesting call, via withRelated()), not at
  // the clone's own copied-from-the-template location.
  bool CheckFailed = false;
  SourceLocation InnerLoc;
  std::string InnerMsg, InnerHint;
  TypeChecker CloneTC(PreR.symbolTable(),
                      [&](SourceLocation Loc, const std::string &Msg,
                          const std::vector<RelatedLocation> &,
                          const std::string &Hint, const std::string &,
                          const SLM::DiagnosticHints &) {
                        if (!CheckFailed) {
                          CheckFailed = true;
                          InnerLoc = Loc;
                          InnerMsg = Msg;
                          InnerHint = Hint;
                        }
                        longjmp(g_recoverJmp, 1);
                      });
  // The collector must keep collecting through this second, nested
  // TypeChecker instance -- a generic call INSIDE this clone's body needs
  // to reach the SAME Collector.record() so its own request lands on the
  // SAME worklist this loop is draining (§9.5 step 3, "push any new
  // requests its body produced" -- the fixed point the caller's while loop
  // already provides, for free, once this line wires it through).
  CloneTC.setMonomorphizationCollector(&Collector);
  // LEC-002: the clone is the template's code -- it may touch the template
  // module's private fields and helpers. Its own SourceModule was cleared
  // above, so the checker must be told the module directly.
  CloneTC.setCurrentModule(Entry.Request.SourceModule);
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
    for (size_t i = 1; i < Entry.Sites.size(); ++i) {
      if (auto *FC = dynamic_cast<FuncCallExprAST *>(Entry.Sites[i]))
        Err.withRelated(FC->getLoc(), "also instantiated here");
      else if (auto *MC = dynamic_cast<MethodCallExprAST *>(Entry.Sites[i]))
        Err.withRelated(MC->getLoc(), "also instantiated here");
    }
    Err.raise();
    return nullptr;
  }

  // TYPE-002 (S14a): INV-GEN-1, asserted HERE too (not just in Codegen's
  // getLLVMType()/CastToType()) -- completion criterion 1's own "at the
  // monomorphizer's final step" half. Checks the clone's own Params/
  // ReturnType (the body is covered independently and exhaustively by the
  // Codegen-side asserts, which fire at actual use rather than needing a
  // second full-body walk here) -- if substituteConcreteTypes() somehow
  // missed a TurfGenericParamType, this fails immediately, at the exact
  // clone that produced it, rather than later and further away at whatever
  // Codegen call site first happens to touch it.
  for (const auto &P : Clone->getParams())
    assert(!dynamic_cast<const TurfGenericParamType *>(P.Type.get()) &&
          "INV-GEN-1 violated: a monomorphized clone's own parameter is "
          "still a TurfGenericParamType after substitution. This is a "
          "compiler bug.");
  assert(!dynamic_cast<const TurfGenericParamType *>(Clone->getReturnType()) &&
        "INV-GEN-1 violated: a monomorphized clone's own return type is "
        "still a TurfGenericParamType after substitution. This is a "
        "compiler bug.");

  // Register: append to PrePassASTs so main.cpp's ORDINARY sub-pass
  // 2c/3a/3b codegen loops pick this up exactly like any other top-level
  // declaration -- no Codegen change needed at all, since the clone is, by
  // this point, structurally indistinguishable from a hand-written
  // concrete function/method. Safe to append while the caller's loop keeps
  // running (nothing else iterates PrePassASTs until 2c starts, strictly
  // after sub-pass 2b.5 returns).
  auto ClonedSymbol = Clone->ResolvedDef; // set by PreR.resolve() above
  PrePassASTs.push_back(std::move(ClonedUP));
  return ClonedSymbol;
}

/// TYPE-002 (S15/D1): rewrite Entry.Sites[Entry.SitesRewritten..) to
/// Entry.ClonedSymbol, for a FuncCallExprAST-keyed entry (FreeFunction or
/// constructor kind). Called both right after an entry is first drained
/// (SitesRewritten starts at 0, so this rewrites everything known so far)
/// and again on every later sweep for an ALREADY-drained entry that has
/// gained new Sites since -- record()'s existing-entry branch keeps
/// appending to Sites regardless of Drained, so a call site discovered
/// while re-typechecking a LATER clone in the same fixed-point sweep must
/// still be rewritten once this entry's own ClonedSymbol exists. See D1 in
/// the change record: before this, Drained alone gated BOTH "don't clone
/// again" and "don't rewrite again," so a late-discovered site was recorded
/// but its ResolvedCallTarget silently stayed null/stale forever.
void rewriteNewFuncCallSites(InstantiationEntry &Entry) {
  for (size_t i = Entry.SitesRewritten; i < Entry.Sites.size(); ++i) {
    if (auto *FC = dynamic_cast<FuncCallExprAST *>(Entry.Sites[i]))
      FC->ResolvedCallTarget = Entry.ClonedSymbol;
  }
  Entry.SitesRewritten = Entry.Sites.size();
}

/// Method-call counterpart of rewriteNewFuncCallSites() -- see its doc
/// comment. Used by drainMethodRequests() (Entry.Sites are
/// MethodCallExprAST there, never FuncCallExprAST).
void rewriteNewMethodCallSites(InstantiationEntry &Entry) {
  for (size_t i = Entry.SitesRewritten; i < Entry.Sites.size(); ++i) {
    if (auto *MC = dynamic_cast<MethodCallExprAST *>(Entry.Sites[i]))
      MC->ResolvedCallTarget = Entry.ClonedSymbol;
  }
  Entry.SitesRewritten = Entry.Sites.size();
}

} // namespace

bool drainFreeFunctionRequests(
    std::vector<std::unique_ptr<ExprAST>> &PrePassASTs,
    InstantiationCollector &Collector, Resolver &PreR) {
  std::unordered_map<std::string, FuncDefExprAST *> Registry =
      buildDeclRegistry(PrePassASTs);

  int TotalDrained = 0;
  bool DidWork = false;

  // Index-based full-cache sweep, not a destructive worklist pop (S12b
  // correction -- InstantiationCollector's own doc comment) --
  // Collector.cache().size() is RE-READ every iteration, so a nested
  // discovery appended mid-sweep (re-typechecking one clone finds another
  // FreeFunction-kind call) is picked up in the SAME sweep without a
  // separate outer loop.
  for (size_t Idx = 0; Idx < Collector.cache().size(); ++Idx) {
    InstantiationEntry &Entry = Collector.entryAt(Idx);

    if (Entry.Request.Kind != InstantiationKind::FreeFunction)
      continue; // Method/Struct: drainMethodRequests()/S13a's job.

    if (Entry.Drained) {
      // TYPE-002 (S15/D1): already cloned -- but a call site discovered
      // AFTER this entry drained (e.g. while draining a LATER entry in this
      // same sweep) still needs rewriting to the clone we already have.
      // Constructor-kind (self-bound) entries never get a ClonedSymbol here
      // -- drainConstructorRequests() owns those -- so guard on it being set.
      if (Entry.ClonedSymbol)
        rewriteNewFuncCallSites(Entry);
      continue;
    }

    if (Entry.Sites.empty())
      continue; // Defensive -- record() always pushes at least one site.

    auto RegIt = Registry.find(
        registryKey(Entry.Request.SourceModule, Entry.Request.DeclKey));
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
      return DidWork;
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
    // matter what v's own type became). Left recorded, undrained here --
    // drainConstructorRequests() (S14a, once S13a's per-instantiation
    // struct layout existed to make it correct) drains it instead, sharing
    // this function's own registry/Sites-rewrite shape but the METHOD-style
    // self-substitution technique (cloneAndSubstituteSelfBound()).
    if (!Template->getParams().empty() && Template->getParams()[0].Name == "self")
      continue;

    if (++TotalDrained > kMaxInstantiationDepth) {
      std::vector<std::string> Chain;
      for (const auto &E : Collector.cache().all())
        Chain.push_back(E.Request.DeclKey);
      GenericInstantiationDepthError(Entry.Request.Site, Chain).raise();
      return DidWork;
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

    auto ClonedSymbol = finishInstantiation(Template, Clone, std::move(ClonedUP),
                                            Entry, PrePassASTs, Collector, PreR);
    if (!ClonedSymbol)
      return DidWork; // finishInstantiation already raised a diagnostic.

    // Rewrite every requesting call site to the clone. Every site in
    // Entry.Sites is a FuncCallExprAST (FreeFunction kind is only ever
    // recorded from TypeCheckerCall.cpp's visitFuncCall). Stored on the
    // entry itself (not just applied here) so a site discovered LATER, after
    // Drained is set, can still be rewritten -- see D1/rewriteNewFuncCallSites().
    Entry.ClonedSymbol = ClonedSymbol;
    rewriteNewFuncCallSites(Entry);
    Entry.Drained = true;
    DidWork = true;
  }
  return DidWork;
}

// ─── Drain: methods ──────────────────────────────────────────────────────────

namespace {

/// Shared by drainMethodRequests() and drainConstructorRequests() (S14a):
/// clone @p Template, substitute every param EXCEPT self (index 0) plus the
/// return type and body using @p Request's TypeArgs paired positionally
/// with the declaration's OWN TypeParams, and give self a REAL type
/// argument -- `StructName*` becomes `StructName<TypeArgs>*`. Correct as
/// long as a method/constructor's own type-parameter spelling/order
/// matches its struct's -- true of every generic struct in this language's
/// stdlib today (all single-type-parameter); a multi-parameter struct whose
/// method/constructor reorders its own type parameters relative to the
/// struct's own declaration is a known, disclosed gap -- see the change
/// record.
///
/// This is safe DESPITE struct layout staying fully erased/shared until
/// S13a/S13b landed (originally verified for methods, S12b; nothing about
/// the reasoning is method-specific), for two independently verified
/// reasons:
///
/// 1. self's LLVM REPRESENTATION is unaffected by which struct-bound kind
///    this is -- getLLVMType(GenericType(StructName, Args)) resolves to
///    the per-instantiation TurfStructType (S13b) either way.
///    FieldAccessExprAST's own GEP-index computation is field-POSITION-
///    based, not field-KIND-based, so self.<field>'s ADDRESS computation
///    doesn't care whether self's own declaration was a method or a
///    constructor.
///
/// 2. CastToType's erasure/un-erasure branches (CodegenCore.cpp, still
///    live for anything NOT yet monomorphized) are driven by the VALUE's
///    actual LLVM shape and the DECLARED DESTINATION type, never by
///    whether the declaring function is a method or a constructor.
///
/// What this DOES change, and is the actual point of it: giving self a
/// real GenericType lets TypeChecker::visitFieldAccess's ALREADY-EXISTING
/// GenericTypeArgs substitution (TypeCheckerCall.cpp) correctly resolve
/// `self.<field>`'s TYPE for THIS instantiation during re-typecheck --
/// e.g. `self.data: int*`, not the un-substituted `self.data: T*` that
/// made a naive first attempt at this fail TYPE-CHECKING (not codegen) in
/// S12a's own history with `Box.new` (see that session's *Removed
/// assumptions* -- the fix there was to exclude constructors entirely,
/// since S12a predates S13a's per-instantiation struct layout; S14a's job
/// is to stop excluding them now that the layout exists).
/// @param StructOwnParamCount how many of @p Request.TypeArgs's LEADING
///        entries are the STRUCT's own type arguments (self becomes
///        `StructName<Request.TypeArgs[0, StructOwnParamCount)>*`) -- 0
///        means the struct isn't itself generic, so self stays a bare
///        `StructName*` even if @p Request.TypeArgs is non-empty (the
///        METHOD's own type parameters, unified against its call
///        arguments -- see genericStructOwnTypeParamCount()'s own doc
///        comment for why these need to be told apart, and D6/S15 for why
///        a non-zero count can still be SMALLER than Request.TypeArgs.size()
///        -- a method redeclaring the struct's own parameter(s) alongside
///        ones of its own, e.g. `fn<T, U> Wrap.pick(other: U): U`).
std::unique_ptr<ExprAST> cloneAndSubstituteSelfBound(
    FuncDefExprAST *Template, const InstantiationRequest &Request,
    size_t StructOwnParamCount) {
  std::unordered_map<std::string, TurfTypePtr> Subst;
  const auto &TypeParams = Template->getTypeParams();
  for (size_t i = 0; i < TypeParams.size() && i < Request.TypeArgs.size(); ++i)
    Subst[TypeParams[i]] = Request.TypeArgs[i];

  auto ClonedUP = CloneExprAST(Template);
  auto *Clone = static_cast<FuncDefExprAST *>(ClonedUP.get());
  auto &Params = Clone->getParamsMut();

  // Substitute every param EXCEPT self (index 0) -- self's OWN type is
  // handled separately, below, and differently (never through Subst: a
  // bare "StructName*" contains no TurfGenericParamType/NamedType for
  // Subst to find in the first place).
  for (size_t i = 1; i < Params.size(); ++i)
    Params[i].Type = substituteConcreteTypes(Params[i].Type, Subst);
  Clone->setReturnType(substituteConcreteTypes(Clone->getReturnTypePtr(), Subst));
  substituteTypesInBody(Clone->getBody(), Subst);

  if (StructOwnParamCount > 0 && !Params.empty() && Params[0].Name == "self") {
    const std::string &DK = Request.DeclKey; // "StructName.Method"/"StructName.new"
    size_t Dot = DK.find('.');
    std::string StructName = Dot == std::string::npos ? DK : DK.substr(0, Dot);
    // TYPE-002 (S15/D6): self must wrap ONLY the struct's own type
    // arguments -- Request.TypeArgs[0, StructOwnParamCount) -- never the
    // full request, which can carry MORE entries when the method declares
    // type parameters of its own beyond the struct's (`fn<T, U>
    // Wrap.pick(other: U): U`, StructOwnParamCount == 1, Request.TypeArgs
    // == [T's value, U's value]). Wrapping self as `Wrap<T,U>*` would name
    // an instantiation drainStructRequests() never registers (it only ever
    // uses the STRUCT's own args) -- self's per-instantiation struct
    // lookup would miss entirely.
    size_t N = std::min(StructOwnParamCount, Request.TypeArgs.size());
    std::vector<TurfTypePtr> StructOwnArgs(Request.TypeArgs.begin(),
                                           Request.TypeArgs.begin() + N);
    auto SelfInner = std::make_shared<GenericType>(StructName, std::move(StructOwnArgs));
    // DeclKey's struct part is the receiver's TypeIdentity (the Resolver
    // renamed the method in Pass 1b, FDN-003) -- mark it so, since this
    // clone is about to be re-resolved in the template's module.
    SelfInner->resolveBaseTo(StructName);
    Params[0].Type = std::make_shared<TurfPointerType>(std::move(SelfInner));
  }
  return ClonedUP;
}

} // namespace

bool drainMethodRequests(
    std::vector<std::unique_ptr<ExprAST>> &PrePassASTs,
    InstantiationCollector &Collector, Resolver &PreR) {
  std::unordered_map<std::string, FuncDefExprAST *> Registry =
      buildDeclRegistry(PrePassASTs);

  int TotalDrained = 0;
  bool DidWork = false;

  for (size_t Idx = 0; Idx < Collector.cache().size(); ++Idx) {
    InstantiationEntry &Entry = Collector.entryAt(Idx);

    if (Entry.Request.Kind != InstantiationKind::Method)
      continue; // FreeFunction/Struct: the other drain functions.

    if (Entry.Drained) {
      // TYPE-002 (S15/D1): see drainFreeFunctionRequests()'s identical
      // check -- a call site discovered after this entry drained still
      // needs rewriting to the clone this entry already produced.
      if (Entry.ClonedSymbol)
        rewriteNewMethodCallSites(Entry);
      continue;
    }

    if (Entry.Sites.empty())
      continue; // Defensive -- record() always pushes at least one site.

    auto RegIt = Registry.find(
        registryKey(Entry.Request.SourceModule, Entry.Request.DeclKey));
    if (RegIt == Registry.end()) {
      // Should be unreachable: every Method-kind request's DeclKey came
      // from visitMethodCall's own struct-instance branch resolving a
      // real candidate (TypeCheckerCall.cpp) -- see CodegenFunc.cpp's
      // identical "should be unreachable if the Resolver ran correctly"
      // defensive diagnostic.
      SemanticError(Entry.Request.Site,
                    "Internal error: no generic method declaration found "
                    "for '" + Entry.Request.DeclKey +
                        "' during monomorphization. This is a compiler bug.")
          .raise();
      return DidWork;
    }
    FuncDefExprAST *Template = RegIt->second;

    if (Template->getParams().empty() || Template->getParams()[0].Name != "self") {
      // Should be unreachable: a Method-kind request is only ever recorded
      // from visitMethodCall's struct-INSTANCE branch (TypeCheckerCall.cpp),
      // which only ever resolves to a real, non-static struct method --
      // every one of those has an implicit self (ParseFuncDef). A static
      // method called via `StructName::method(...)` is a DIFFERENT syntax
      // routed through visitFuncCall (FreeFunction-kind, self-STRIPPED) --
      // see drainFreeFunctionRequests()'s own struct-bound exclusion.
      SemanticError(Entry.Request.Site,
                    "Internal error: '" + Entry.Request.DeclKey +
                        "' resolved as a Method-kind instantiation request "
                        "but has no 'self' parameter. This is a compiler bug.")
          .raise();
      return DidWork;
    }

    if (++TotalDrained > kMaxInstantiationDepth) {
      std::vector<std::string> Chain;
      for (const auto &E : Collector.cache().all())
        Chain.push_back(E.Request.DeclKey);
      GenericInstantiationDepthError(Entry.Request.Site, Chain).raise();
      return DidWork;
    }

    // TYPE-002 (S14a): a Method-kind request's TypeArgs can come from the
    // RECEIVER's own GenericType (struct is generic -- self needs
    // GenericType wrapping) OR from the METHOD's own type parameters,
    // unified against its call arguments, on a struct that ISN'T generic
    // (self stays bare) -- see genericStructOwnTypeParamCount()'s own doc
    // comment for the full story. DeclKey is "StructName.MethodName"
    // either way.
    {
      const std::string &DK = Entry.Request.DeclKey;
      size_t Dot = DK.find('.');
      std::string StructName = Dot == std::string::npos ? DK : DK.substr(0, Dot);
      size_t StructOwnParamCount = genericStructOwnTypeParamCount(StructName, PrePassASTs);

      auto ClonedUP = cloneAndSubstituteSelfBound(Template, Entry.Request, StructOwnParamCount);
      auto *Clone = static_cast<FuncDefExprAST *>(ClonedUP.get());

      auto ClonedSymbol = finishInstantiation(Template, Clone, std::move(ClonedUP),
                                              Entry, PrePassASTs, Collector, PreR);
      if (!ClonedSymbol)
        return DidWork; // finishInstantiation already raised a diagnostic.

      // Rewrite every requesting call site to the clone. Every site in
      // Entry.Sites is a MethodCallExprAST (Method kind is only ever
      // recorded from TypeCheckerCall.cpp's visitMethodCall). Stored on the
      // entry (not just applied here) so a later-discovered site can still
      // be rewritten after Drained is set -- see D1/rewriteNewMethodCallSites().
      Entry.ClonedSymbol = ClonedSymbol;
      rewriteNewMethodCallSites(Entry);
    }
    Entry.Drained = true;
    DidWork = true;
  }
  return DidWork;
}

// ─── Drain: constructors ─────────────────────────────────────────────────────

/// TYPE-002 (S14a): the constructor counterpart of drainMethodRequests() --
/// `fn<T> StructName.new(...)` called as `StructName::new(...)`. S12a
/// deliberately excluded every struct-bound declaration reached through the
/// free-function call path (`Box::new(...)`'s "self-stripped candidate",
/// ResolverCall.cpp) from drainFreeFunctionRequests() -- at the time, no
/// per-instantiation struct layout existed for a substituted `self.<field>`
/// reference to resolve against (S12a's own *Removed assumptions*: `Box::new(42)`
/// re-typechecked as "Cannot assign value of type 'int' to variable of type
/// 'T'"). S13a built that layout; this function is what actually drains
/// the constructor requests S12a left recorded but untouched, using the
/// exact same self-type-rewrite technique drainMethodRequests() already
/// uses (cloneAndSubstituteSelfBound(), above) -- self carrying a real
/// GenericType now correctly resolves `self.<field>` during re-typecheck
/// for a constructor's body exactly as it already does for an ordinary
/// method's.
///
/// Still a FreeFunction-kind request (S12a's own recording never changed --
/// TypeCheckerCall.cpp's visitFuncCall records `Box::new(...)` as
/// FreeFunction, keyed "Box.new"), so this function shares
/// drainFreeFunctionRequests()'s registry/lookup/Sites-rewrite shape
/// (FuncCallExprAST, not MethodCallExprAST), not drainMethodRequests()'s --
/// only the CLONE-BUILDING step (self-rewrite) is shared with methods.
bool drainConstructorRequests(
    std::vector<std::unique_ptr<ExprAST>> &PrePassASTs,
    InstantiationCollector &Collector, Resolver &PreR) {
  std::unordered_map<std::string, FuncDefExprAST *> Registry =
      buildDeclRegistry(PrePassASTs);

  int TotalDrained = 0;
  bool DidWork = false;

  for (size_t Idx = 0; Idx < Collector.cache().size(); ++Idx) {
    InstantiationEntry &Entry = Collector.entryAt(Idx);

    if (Entry.Request.Kind != InstantiationKind::FreeFunction)
      continue; // Method/Struct: the other two drain functions.

    if (Entry.Drained) {
      // TYPE-002 (S15/D1): see drainFreeFunctionRequests()'s identical
      // check -- a constructor call site discovered after this entry
      // drained still needs rewriting. drainFreeFunctionRequests()'s own
      // sweep also catches this (it doesn't filter Drained entries by
      // self-boundness), but rewriting here too keeps each drain function
      // responsible for the entries it owns rather than relying on a
      // sibling to clean up after it.
      if (Entry.ClonedSymbol)
        rewriteNewFuncCallSites(Entry);
      continue;
    }

    if (Entry.Sites.empty())
      continue; // Defensive -- record() always pushes at least one site.

    auto RegIt = Registry.find(
        registryKey(Entry.Request.SourceModule, Entry.Request.DeclKey));
    if (RegIt == Registry.end()) {
      // A genuinely missing declaration is drainFreeFunctionRequests()'s
      // error to raise (it runs the identical lookup against the SAME
      // registry for every FreeFunction-kind entry, struct-bound or not,
      // before this function ever gets a chance to) -- reaching this means
      // that function already raised and this call is unreachable in
      // practice. Kept for defensive symmetry with the other two drain
      // functions, not because a different outcome is expected here.
      SemanticError(Entry.Request.Site,
                    "Internal error: no generic declaration found for '" +
                        Entry.Request.DeclKey +
                        "' during monomorphization. This is a compiler bug.")
          .raise();
      return DidWork;
    }
    FuncDefExprAST *Template = RegIt->second;

    if (Template->getParams().empty() || Template->getParams()[0].Name != "self")
      continue; // Not struct-bound -- drainFreeFunctionRequests()'s job.

    if (++TotalDrained > kMaxInstantiationDepth) {
      std::vector<std::string> Chain;
      for (const auto &E : Collector.cache().all())
        Chain.push_back(E.Request.DeclKey);
      GenericInstantiationDepthError(Entry.Request.Site, Chain).raise();
      return DidWork;
    }

    // TYPE-002 (S14a): `Box::new(...)`'s TypeArgs come from unifying the
    // constructor's OWN params against its call arguments (visitFuncCall,
    // TypeCheckerCall.cpp -- the SAME free-function-style unification an
    // ordinary generic function call uses), regardless of whether `Box`
    // itself is declared generic. Most of the time the two coincide by
    // NAMING CONVENTION (`struct Box<T> { ... } fn<T> Box.new(v: T)`
    // reuses "T" for both), but self must only be GenericType-wrapped when
    // Box ACTUALLY is one -- see genericStructOwnTypeParamCount()'s own doc
    // comment.
    const std::string &DK = Entry.Request.DeclKey;
    size_t Dot = DK.find('.');
    std::string StructName = Dot == std::string::npos ? DK : DK.substr(0, Dot);
    size_t StructOwnParamCount = genericStructOwnTypeParamCount(StructName, PrePassASTs);

    auto ClonedUP = cloneAndSubstituteSelfBound(Template, Entry.Request, StructOwnParamCount);
    auto *Clone = static_cast<FuncDefExprAST *>(ClonedUP.get());

    auto ClonedSymbol = finishInstantiation(Template, Clone, std::move(ClonedUP),
                                            Entry, PrePassASTs, Collector, PreR);
    if (!ClonedSymbol)
      return DidWork; // finishInstantiation already raised a diagnostic.

    // Rewrite every requesting call site to the clone. Every site in
    // Entry.Sites is a FuncCallExprAST whose callee is a ScopeAccessExprAST
    // with ResolvedKind == ConstructorCall (`StructName::new(...)`) --
    // FreeFunction kind is only ever recorded from TypeCheckerCall.cpp's
    // visitFuncCall. CodegenFunc.cpp's ConstructorCall branch reads
    // ResolvedCallTarget->Params[0].Name == "self" to tell a monomorphized
    // clone (self present, ordinary mangledName()) apart from the
    // still-self-STRIPPED candidate every non-generic constructor's
    // ResolvedCallTarget remains (stripSelf() runs unconditionally for
    // every `::new(...)` call, generic or not).
    Entry.ClonedSymbol = ClonedSymbol;
    rewriteNewFuncCallSites(Entry);
    Entry.Drained = true;
    DidWork = true;
  }
  return DidWork;
}

// ─── Drain: structs ──────────────────────────────────────────────────────────

namespace {

/// Struct counterpart of buildDeclRegistry(): walks PrePassASTs (recursing
/// into every ModuleImportExprAST's own body, same traversal) collecting
/// every StructDeclExprAST with a non-empty TypeParams list.
///
/// Keyed by the struct's TypeIdentity (FDN-003) -- the declaration's own
/// getName(), which the Parser already built from (module, name), and which
/// is exactly what every GenericType's base name holds once the Resolver
/// has resolved it (`list::List`, or a bare main-file `Box`). Two modules'
/// same-named generic structs are therefore two registry entries.
///
/// History: this used to be keyed by BARE name on the argument that struct
/// names were one program-wide namespace (GlobalSymbolTable::RegisterType()
/// took no module), so a bare key lost nothing -- and because a Struct-kind
/// request's SourceModule was then read off the use site's SPELLED
/// qualifier (empty for an unqualified `List<int>` inside list.tr itself),
/// not the declaring module, so a (SourceModule, Name) key would miss. Both
/// premises are gone: requests now record TypeIdentity::owningModule() of
/// the resolved base, and the key needs no module part at all.
///
/// The Module string carried alongside each entry is the struct's own TRUE
/// declaring module (where the Walk actually found it), independent of
/// Entry.Request.SourceModule -- resolveMonomorphizedClone() (below) needs
/// THIS one, not the request's, for the identical reason the lookup itself
/// can't use the request's: a clone must resolve as if declared in the
/// module it actually came from, regardless of how the REQUESTING site
/// happened to spell the reference to it.
struct StructRegistryEntry {
  StructDeclExprAST *Decl;
  std::string Module;
};

std::unordered_map<std::string, StructRegistryEntry>
buildStructDeclRegistry(const std::vector<std::unique_ptr<ExprAST>> &PrePassASTs) {
  std::unordered_map<std::string, StructRegistryEntry> Registry;
  std::function<void(const std::vector<std::unique_ptr<ExprAST>> &, const std::string &)>
      Walk = [&](const std::vector<std::unique_ptr<ExprAST>> &Nodes,
                const std::string &Module) {
    for (const auto &N : Nodes) {
      if (auto *SD = dynamic_cast<StructDeclExprAST *>(N.get())) {
        if (!SD->getTypeParams().empty())
          Registry[SD->getName()] = {SD, Module};
      } else if (auto *MI = dynamic_cast<ModuleImportExprAST *>(N.get())) {
        if (auto *Body = dynamic_cast<const BlockExprAST *>(MI->getBody()))
          Walk(Body->getExpressions(), MI->getModuleName());
      }
    }
  };
  Walk(PrePassASTs, "");
  return Registry;
}

} // namespace

bool drainStructRequests(
    std::vector<std::unique_ptr<ExprAST>> &PrePassASTs,
    InstantiationCollector &Collector, Resolver &PreR) {
  std::unordered_map<std::string, StructRegistryEntry> Registry =
      buildStructDeclRegistry(PrePassASTs);

  int TotalDrained = 0;
  bool DidWork = false;

  for (size_t Idx = 0; Idx < Collector.cache().size(); ++Idx) {
    InstantiationEntry &Entry = Collector.entryAt(Idx);

    if (Entry.Drained)
      continue;
    if (Entry.Request.Kind != InstantiationKind::Struct)
      continue; // FreeFunction/Method: the other two drain functions.

    if (Entry.Sites.empty())
      continue; // Defensive -- record() always pushes at least one site.

    // Bare-name lookup, NOT registryKey(SourceModule, DeclKey) -- see
    // buildStructDeclRegistry()'s own doc comment for why a Struct-kind
    // request's SourceModule can't be trusted as a registry key the way a
    // FreeFunction/Method one can.
    auto RegIt = Registry.find(Entry.Request.DeclKey);
    if (RegIt == Registry.end()) {
      // Should be unreachable: every Struct-kind request's DeclKey came from
      // a real GenericType's own base name at a point (VarDeclExprAST's
      // declared type, or a method call's receiver, TypeCheckerCall.cpp/
      // TypeCheckerDecl.cpp) that only ever produces a GenericType for an
      // already-resolved, real struct declaration.
      SemanticError(Entry.Request.Site,
                    "Internal error: no generic struct declaration found "
                    "for '" + Entry.Request.DeclKey +
                        "' during monomorphization. This is a compiler bug.")
          .raise();
      return DidWork;
    }
    StructDeclExprAST *Template = RegIt->second.Decl;
    const std::string &TemplateModule = RegIt->second.Module;

    if (++TotalDrained > kMaxInstantiationDepth) {
      std::vector<std::string> Chain;
      for (const auto &E : Collector.cache().all())
        Chain.push_back(E.Request.DeclKey);
      GenericInstantiationDepthError(Entry.Request.Site, Chain).raise();
      return DidWork;
    }

    // Build the name->concrete substitution map from the struct's OWN
    // TypeParams (declared order, e.g. `struct Map<K, V>` -> ["K", "V"])
    // paired positionally with the request's own ordered TypeArgs.
    std::unordered_map<std::string, TurfTypePtr> Subst;
    const auto &TypeParams = Template->getTypeParams();
    for (size_t i = 0; i < TypeParams.size() && i < Entry.Request.TypeArgs.size(); ++i)
      Subst[TypeParams[i]] = Entry.Request.TypeArgs[i];

    // Clone, then substitute every field's type concretely. The template's
    // OWN Fields already carry TurfGenericParamType uniformly (the whole
    // program's ordinary resolveAll() already ran Resolver::visitStructDecl()
    // over it before sub-pass 2b.5 starts, the same way a FuncDefExprAST
    // template's Params/body already do -- see drainFreeFunctionRequests()'s
    // own comment), so substituteConcreteTypes() (its first branch already
    // handles TurfGenericParamType directly) is all that's needed -- no
    // separate body walker: a struct has no body, only a flat field list.
    auto ClonedUP = CloneExprAST(Template);
    auto *Clone = static_cast<StructDeclExprAST *>(ClonedUP.get());
    for (auto &Field : Clone->getFieldsMut())
      Field.Type = substituteConcreteTypes(Field.Type, Subst);

    // Rename to the SAME mangling scheme drainFreeFunctionRequests()/
    // drainMethodRequests() already use (§9.5) -- "List" + [int] ->
    // "List_gen1_int" -- BEFORE anything below reads Clone->getName(), and
    // clear TypeParams so a later Resolver::visitStructDecl() call on this
    // clone (below) treats it as fully concrete, same contract as
    // FuncDefExprAST::clearTypeParams() (S12a).
    Clone->setName(instantiatedDeclName(Entry.Request.DeclKey, Entry.Request.TypeArgs));
    Clone->clearTypeParams();

    // Per-instantiation acyclicity -- BEFORE registering the new type, so a
    // genuinely cyclic instantiation never reaches GlobalSymbolTable/Codegen
    // at all. Raises StructLayoutCycleError directly (propagates to
    // main.cpp's own sub-pass-2b.5-level setjmp, the SAME already-verified
    // path GenericInstantiationDepthError above already relies on -- see
    // Monomorphize.h's own doc comment on this function for why no extra
    // nested catch is needed here, unlike finishInstantiation()'s TypeChecker
    // re-raise).
    PreR.checkStructLayoutAcyclicityForClone(Clone);

    // Register the new, distinct TurfStructType -- GlobalSymbolTable (the
    // codegen/typechecker-facing type registry), NOT Resolver's own
    // SymTable_ (a different, name-existence-only registry -- see
    // resolveMonomorphizedClone() below for that one). This is bullet 1 of
    // S13a's own plan (TYPE-002.md): "one real TurfStructType ... per struct
    // instantiation," registered "under extended mangled names."
    if (GlobalSymbolTable)
      GlobalSymbolTable->RegisterType(
          Clone->getName(),
          std::make_shared<TurfStructType>(Clone->getName(), Clone->getFields()));

    // Re-resolve: registers the clone's own name into Resolver's SymTable_
    // (ResolverTypeRegistry) via Resolver::visitStructDecl(), reused
    // completely unchanged -- StructDeclExprAST::accept() dispatches through
    // the exact same visitor entry point resolveMonomorphizedClone() already
    // uses for a FuncDefExprAST clone (S12b) -- and validates every
    // (already-concrete) field type is visible from Entry.Request.SourceModule's
    // scope, fixing the identical two real bugs that method resolved for:
    // module-scope ("List" needs to resolve as local from list.tr's own
    // context) and substituted-type visibility (a field type naming a
    // struct from a module the template's own declaring module never
    // imports, e.g. Map<string, JsonValue>'s V=JsonValue). Wrapped in the
    // same nested setjmp/longjmp harness finishInstantiation() uses for the
    // identical defensive reason: this should never legitimately fail for a
    // field type that already passed this exact check once, at the original
    // call site -- if it does, that is a compiler bug, not a user error.
    bool ResolveFailed = false;
    {
      const bool SavedRecoverActive = g_recoverActive;
      jmp_buf SavedJmpBuf;
      std::memcpy(SavedJmpBuf, g_recoverJmp, sizeof(jmp_buf));
      g_recoverActive = true;
      if (setjmp(g_recoverJmp) == 0) {
        // TemplateModule, NOT Entry.Request.SourceModule -- see
        // buildStructDeclRegistry()'s own doc comment (StructRegistryEntry)
        // for why the request's own SourceModule is unreliable here.
        PreR.resolveMonomorphizedClone(Clone, TemplateModule);
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
      return DidWork;
    }

    // Register: append to PrePassASTs so main.cpp's ORDINARY sub-pass 2c
    // struct-codegen loop picks this up -- unconditionally, exactly like
    // every other StructDeclExprAST it finds, since that loop carries no
    // TypeParams-empty skip guard at all (unlike sub-pass 3a/3b's function
    // guard) -- producing a real, distinct llvm::StructType under this
    // clone's own mangled name, independent of the shared, erasure-era one
    // the TEMPLATE's own (still-present, still-codegenned) struct decl node
    // keeps producing. No call-site rewrite: see this function's own doc
    // comment (Monomorphize.h) for why nothing needs to be pointed at the
    // new type yet this session.
    PrePassASTs.push_back(std::move(ClonedUP));
    Entry.Drained = true;
    DidWork = true;
  }
  return DidWork;
}

} // namespace Monomorphize
