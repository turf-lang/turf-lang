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
#include "Monomorphize.h"    // Monomorphize::instantiatedDeclName() -- byValueEdgeTarget()'s clone-name resolution, OVH-004
#include "Parser.h"          // ResolveModuleFile() -- candidateLocation()
#include "ResolverSymbolTable.h"
#include "ResolverInternal.h"
#include "SuggestionIndex.h" // SuggestionIndex::suggestVisible
#include "Types.h"           // RankedSuggestions
#include "TypeWalk.h"         // SMP-002: shared substitute() walker

#include "llvm/ADT/SmallString.h" // Num->getIntValExact().toString() below

#include <cstring>
#include <functional> // std::function -- checkStructLayoutAcyclicity()'s recursive DFS lambda
#include <memory>
#include <unordered_map> // checkStructLayoutAcyclicity()'s name -> AST / colour maps
#include <unordered_set>


SourceLocation candidateLocation(const ResolverFunctionSymbol &Cand) {
  SourceLocation L = toSourceLocation(Cand.DeclLoc);
  if (!Cand.SourceModule.empty()) {
    L.Filename = ResolveModuleFile(Cand.SourceModule, SourceFilename);
    if (L.Filename.empty())
      L.Filename = "<module " + Cand.SourceModule + ">";
  }
  return L;
}


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
  // FDN-003: types first (1a), then every declaration-level type spelling
  // turned into its identity (1b), THEN functions (below) -- a function
  // signature captured before 1b would carry spellings, not identities, and
  // compare unequal to every argument type later. See Resolver.h.
  hoistTypes(Nodes);
  resolveDeclarationSpellings(Nodes);
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
 *
 * SMP-002: thin wrapper over the shared TypeWalk::substitute() walker
 * (TypeWalk.h) -- this function's own recursive body, duplicated by hand in
 * at least six other places across the compiler, is now the single one that
 * exists. Building `Subst` fresh on every call (rather than caching it per
 * declaration) costs one small map per generic declaration, once, at
 * signature-capture time -- not a hot path. Picking up TypeWalk::substitute()'s
 * TurfErrorUnionType case for free (this function never had one) closes a
 * latent gap of the exact kind SMP-002 exists to close: a generic
 * declaration's own `!T` return/parameter type now gets its `T` marked as an
 * open parameter at declaration time too, not just at each instantiation
 * site (see the change record's *Removed assumptions*).
 */
TurfTypePtr substituteTypeParams(const TurfTypePtr &Ty,
                                 const std::vector<std::string> &TypeParams) {
  if (!Ty || TypeParams.empty())
    return Ty;
  TypeWalk::SubstMap Subst;
  for (const auto &Name : TypeParams)
    Subst.emplace(Name, std::make_shared<TurfGenericParamType>(Name));
  return TypeWalk::substitute(Ty, Subst);
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
 * method call the body makes -- a method that only reaches a mutation through
 * `self.m()` is found by ResolverSymbolTable::mutatesSelfTransitively()
 * instead, which builds the call graph during Pass 2) and NOT alias-aware (passing `self` or
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
///
/// LEC-006 follow-up: the scan used to recognise exactly `self.<field> = …`,
/// and to recurse only through blocks, `if`, `while`, `for` and `for each`. So
/// `self.arr[i] = v`, `self.inner.f = v`, and an assignment inside a `compare`
/// arm, a `defer`, a `do/while` or an `unsafe` block all went unseen, and
/// `immut s; s.viaArray();` compiled. It now (a) walks every statement kind that
/// holds a body, and (b) treats an assignment target as a mutation of self when
/// its place chain is ROOTED at `self` and stays in self's own value storage.
///
/// Staying in value storage needs the declared field types, which is why this
/// takes the struct declaration: a field that is a pointer or reference ends the
/// walk (`self.data[i] = v` on a `T*` writes the pointee, so List.set is NOT a
/// mutation of the List value -- flagging it would reject `grid[i].set(j, v)`,
/// which works). A fixed array or a value struct continues it. A type it cannot
/// resolve (a bare type parameter, an unknown name) also ends it, in the
/// under-flagging direction, like the rest of this function.
using StructFinder = std::function<const StructDeclExprAST *(const TurfType *)>;

static bool WritesSelfValueStorage(const ExprAST *Target,
                                   const StructDeclExprAST *SelfDecl,
                                   const StructFinder &FindStruct) {
  // The place chain, outermost (the assigned slot) first, `self` last.
  std::vector<const ExprAST *> Chain;
  for (const ExprAST *E = Target; E;) {
    Chain.push_back(E);
    if (const auto *FA = dynamic_cast<const FieldAccessExprAST *>(E))
      E = FA->getBase();
    else if (const auto *AA = dynamic_cast<const ArrayAccessExprAST *>(E))
      E = AA->getBase();
    else if (const auto *Sl = dynamic_cast<const SliceExprAST *>(E))
      E = Sl->getBase(); // `self.data[1..][0] = v` writes self.data
    else
      break;
  }
  const auto *Root = dynamic_cast<const VariableExprAST *>(Chain.back());
  if (Chain.size() < 2 || !Root || Root->getName() != "self")
    return false;

  // First step out of `self` must be a field.
  const auto *First = dynamic_cast<const FieldAccessExprAST *>(Chain[Chain.size() - 2]);
  if (!First)
    return false;
  const TurfType *Slot = nullptr;
  if (SelfDecl)
    for (const StructField &F : SelfDecl->getFields())
      if (F.Name == First->getFieldName()) { Slot = F.Type.get(); break; }
  if (Chain.size() == 2)
    return true; // `self.<field> = …` -- always, even if the field type is unresolved

  for (size_t I = Chain.size() - 2; I > 0; --I) {
    if (!Slot || Slot->isPointer())
      return false; // unknown type, or a write THROUGH a pointer/reference
    const ExprAST *Next = Chain[I - 1];
    if (dynamic_cast<const ArrayAccessExprAST *>(Next)) {
      if (!Slot->isArray())
        return false; // indexing a struct/string goes through an operator call
      const auto *Arr = dynamic_cast<const ::ArrayType *>(Slot);
      Slot = Arr ? Arr->getElementTypePtr().get() : nullptr;
    } else if (const auto *FA = dynamic_cast<const FieldAccessExprAST *>(Next)) {
      const StructDeclExprAST *Inner = FindStruct(Slot);
      const TurfType *FieldTy = nullptr;
      if (Inner)
        for (const StructField &F : Inner->getFields())
          if (F.Name == FA->getFieldName()) { FieldTy = F.Type.get(); break; }
      Slot = FieldTy;
    } else if (dynamic_cast<const SliceExprAST *>(Next)) {
      if (!Slot->isArray())
        return false;
      // A view of the same elements: Slot stays an array of the same type.
    } else {
      return false;
    }
  }
  return true; // the assigned slot is inside self's own storage
}

static bool MutatesSelfField(const ExprAST *E, const StructDeclExprAST *SelfDecl,
                             const StructFinder &FindStruct) {
  if (!E)
    return false;
  auto Recurse = [&](const ExprAST *Child) {
    return MutatesSelfField(Child, SelfDecl, FindStruct);
  };

  if (auto *Assign = dynamic_cast<const AssignmentExprAST *>(E))
    return WritesSelfValueStorage(Assign->getLHS(), SelfDecl, FindStruct);
  if (auto *CAssign = dynamic_cast<const CompoundAssignmentExprAST *>(E))
    return WritesSelfValueStorage(CAssign->getLHS(), SelfDecl, FindStruct);
  if (auto *Post = dynamic_cast<const PostfixExprAST *>(E))
    return WritesSelfValueStorage(Post->getOperand(), SelfDecl, FindStruct);
  if (auto *Blk = dynamic_cast<const BlockExprAST *>(E)) {
    for (const auto &Child : Blk->getExpressions())
      if (Recurse(Child.get()))
        return true;
    return false;
  }
  if (auto *If = dynamic_cast<const IfExprAST *>(E)) {
    for (const auto &B : If->getBranches())
      if (Recurse(B.Body.get()))
        return true;
    return Recurse(If->getElseBody());
  }
  if (auto *W = dynamic_cast<const WhileExprAST *>(E))
    return Recurse(W->getBody());
  if (auto *DW = dynamic_cast<const DoWhileExprAST *>(E))
    return Recurse(DW->getBody());
  if (auto *F = dynamic_cast<const ForExprAST *>(E))
    return Recurse(F->getBody());
  if (auto *FE = dynamic_cast<const ForEachExprAST *>(E))
    return Recurse(FE->getBody());
  if (auto *U = dynamic_cast<const UnsafeBlockExprAST *>(E))
    return Recurse(U->getBody());
  if (auto *D = dynamic_cast<const DeferExprAST *>(E))
    return Recurse(D->getStmt());
  if (auto *C = dynamic_cast<const CompareExprAST *>(E)) {
    for (const auto &Arm : C->getArms())
      if (Recurse(Arm.Body.get()))
        return true;
    return Recurse(C->getOtherBody());
  }
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
 *
 * OVH-012: also descends into a `recover run { ... }` / `recover as err run
 * { ... }` block's `return`s -- `immut x: T = call() recover run { return
 * ErrorType::Variant; };` is the idiomatic way a `!T` function surfaces a
 * DIFFERENT error than whatever `call()` itself failed with (every stdlib
 * example: `io::read_lines`, `json::parse_number`/`parse_array`/
 * `parse_object`), and this scan previously had no case for `RecoverExprAST`
 * (nor for the `VarDeclExprAST`/`AssignmentExprAST` that typically wrap one
 * as an initializer/RHS) -- confirmed live: `NeverReturnsErrorWarning` fired
 * on every one of those functions even though each genuinely returns an
 * error on some path. `recover produce`'s `ProduceExpr` is scanned too, for
 * the same structural reason (it's an ordinary sub-expression, just never
 * containing a `return` in the shapes seen so far); `Wrapped` is scanned
 * for symmetry, though a `return` inside the wrapped fallible call itself
 * would belong to a DIFFERENT function, not this one, and so can never
 * actually contribute here.
 *
 * `BoundErrNames` is the stack of `recover as err run` bound names currently
 * enclosing `E` (innermost last; usually at most one, but nothing stops a
 * nested `recover as err run` naming its own binding). When a `return` names
 * a bare variable matching one of these, it is `json::parse`'s
 * `p.parse_value() recover as err run { return err; };` shape -- re-throwing
 * whatever error the wrapped call actually produced. There is no type NAME
 * to collect (`err`'s static type is the type-erased `AnyErrorType`), so
 * this sets `FoundReboundError` instead of pushing into `Out` --
 * ResolverFunctionSymbol::ReturnsReboundError's own doc comment (
 * ResolverSymbolBase.h) has the consumer side. A bare-variable `return` that
 * DOESN'T match any bound name is left unclassified, same as before this
 * case existed (it may be the function's own ordinary success value).
 */
static void CollectReturnedErrorTypeNames(
    const ExprAST *E, std::vector<std::string> &Out, bool &FoundReboundError,
    const std::vector<std::string> &BoundErrNames = {}) {
  if (!E)
    return;

  // FDN-003: a `Q::Err::Variant` spelling is recorded as `Q::Err` -- which is
  // exactly Err's TypeIdentity when Q really owns it -- so buildFunctionSymbol()
  // can map every entry to an identity without the qualifier being lost here.
  auto Spelling = [](const ScopeAccessExprAST *SA) {
    return SA->getModuleQualifier().empty() || SA->isLeftTypeIdentity()
               ? SA->getLeftName()
               : TypeIdentity::make(SA->getModuleQualifier(), SA->getLeftName());
  };
  if (auto *Ret = dynamic_cast<const ReturnExprAST *>(E)) {
    if (auto *SA = dynamic_cast<const ScopeAccessExprAST *>(Ret->getVal())) {
      Out.push_back(Spelling(SA));
    } else if (auto *FC = dynamic_cast<const FuncCallExprAST *>(Ret->getVal())) {
      if (auto *SA = dynamic_cast<const ScopeAccessExprAST *>(FC->getCallee()))
        Out.push_back(Spelling(SA));
    } else if (auto *Var = dynamic_cast<const VariableExprAST *>(Ret->getVal())) {
      if (std::find(BoundErrNames.begin(), BoundErrNames.end(),
                    Var->getName()) != BoundErrNames.end())
        FoundReboundError = true;
    }
    return;
  }
  if (auto *Blk = dynamic_cast<const BlockExprAST *>(E)) {
    for (const auto &Child : Blk->getExpressions())
      CollectReturnedErrorTypeNames(Child.get(), Out, FoundReboundError, BoundErrNames);
    return;
  }
  if (auto *If = dynamic_cast<const IfExprAST *>(E)) {
    for (const auto &B : If->getBranches())
      CollectReturnedErrorTypeNames(B.Body.get(), Out, FoundReboundError, BoundErrNames);
    CollectReturnedErrorTypeNames(If->getElseBody(), Out, FoundReboundError, BoundErrNames);
    return;
  }
  if (auto *W = dynamic_cast<const WhileExprAST *>(E)) {
    CollectReturnedErrorTypeNames(W->getBody(), Out, FoundReboundError, BoundErrNames);
    return;
  }
  if (auto *F = dynamic_cast<const ForExprAST *>(E)) {
    CollectReturnedErrorTypeNames(F->getBody(), Out, FoundReboundError, BoundErrNames);
    return;
  }
  if (auto *FE = dynamic_cast<const ForEachExprAST *>(E)) {
    CollectReturnedErrorTypeNames(FE->getBody(), Out, FoundReboundError, BoundErrNames);
    return;
  }
  if (auto *VD = dynamic_cast<const VarDeclExprAST *>(E)) {
    CollectReturnedErrorTypeNames(VD->getInitializer(), Out, FoundReboundError, BoundErrNames);
    return;
  }
  if (auto *Assign = dynamic_cast<const AssignmentExprAST *>(E)) {
    CollectReturnedErrorTypeNames(Assign->getRHS(), Out, FoundReboundError, BoundErrNames);
    return;
  }
  if (auto *Rec = dynamic_cast<const RecoverExprAST *>(E)) {
    CollectReturnedErrorTypeNames(Rec->getWrapped(), Out, FoundReboundError, BoundErrNames);
    if (Rec->getKind() == RecoverKind::Produce) {
      CollectReturnedErrorTypeNames(Rec->getProduceExpr(), Out, FoundReboundError, BoundErrNames);
    } else {
      std::vector<std::string> InnerBound = BoundErrNames;
      if (Rec->getKind() == RecoverKind::AsRun)
        InnerBound.push_back(Rec->getBoundName());
      CollectReturnedErrorTypeNames(Rec->getBlock(), Out, FoundReboundError, InnerBound);
    }
    return;
  }
}


ResolverFunctionSymbol
Resolver::buildExternSymbol(const ExternFuncDeclExprAST &Node,
                            const std::string &ModuleName) const {
  ResolverFunctionSymbol Sym;
  Sym.BaseName = Node.getName();
  Sym.SourceModule = ModuleName;
  Sym.ReturnType = Node.getReturnType();
  Sym.IsExtern = true; // see IsExtern's doc comment in ResolverSymbolTable.h
  Sym.IsVarArg = Node.isVarArg(); // propagate '...' to the arity check
  Sym.IsPublic = Node.isPublic();
  Sym.Annotations = Node.getAnnotations();
  Sym.DeclLoc = {Node.getNameLoc().Line, Node.getNameLoc().Col};
  for (const auto &[PName, PType] : Node.getParams())
    Sym.Params.push_back({PName, PType});
  return Sym;
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
  Sym.Decl = &Node; // see ResolverFunctionSymbol::Decl
  Sym.HasCallerLocation = Node.hasCallerLocation();
  Sym.Annotations = Node.getAnnotations();
  Sym.IsPublic = Node.isPublic();
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
  if (!Sym.Params.empty() && Sym.Params[0].Name == "self") {
    // A struct declaration by name: exact first (a main-file struct is bare,
    // and a spelling the Resolver already rewrote is an identity), then
    // qualified by this method's own module. No match -> null, which the scan
    // treats as "type unknown" and answers conservatively.
    auto FindStructDecl = [&](const std::string &Name) -> const StructDeclExprAST * {
      for (const auto *D : HoistedStructDecls_)
        if (D->getName() == Name)
          return D;
      if (!ModuleName.empty())
        for (const auto *D : HoistedStructDecls_)
          if (D->getName() == ModuleName + "::" + Name)
            return D;
      return nullptr;
    };
    StructFinder FindStruct = [&](const TurfType *T) -> const StructDeclExprAST * {
      if (!T)
        return nullptr;
      if (const auto *G = dynamic_cast<const GenericType *>(T))
        return FindStructDecl(G->getBaseName());
      return FindStructDecl(T->getName());
    };
    const std::string &FnName = Node.getName();
    const size_t Dot = FnName.find('.');
    const StructDeclExprAST *SelfDecl =
        Dot == std::string::npos ? nullptr : FindStructDecl(FnName.substr(0, Dot));
    Sym.MutatesSelf = MutatesSelfField(Node.getBody(), SelfDecl, FindStruct);
  }
  CollectReturnedErrorTypeNames(Node.getBody(), Sym.PossibleErrorTypeNames,
                                Sym.ReturnsReboundError);
  // LEC-004: a `try` returns another call's error, unchanged -- no type name
  // for the scan above to collect, exactly like `return err;` inside a
  // `recover as err run` block. The Parser recorded whether one exists.
  if (Node.UsesTry)
    Sym.ReturnsReboundError = true;
  // FDN-003: in Pass 1 the body's `Err::Variant` nodes are still spellings;
  // consumers (isErrorTypeName()/lookupErrorType(), visitRecoverExpr and
  // visitFuncDef) key on identities. Same resolution rule as the `Left` of
  // any `Left::Right` (resolveScopeLeftType()): visible bare first, else the
  // one type anywhere spelled that way. A name that resolves to nothing is
  // kept as-is -- the list is documented as unfiltered.
  for (auto &Name : Sym.PossibleErrorTypeNames) {
    if (auto Id = tryResolveTypeSpelling("", Name, ModuleName)) {
      Name = *Id;
    } else if (!TypeIdentity::isModuleQualified(Name)) {
      auto All = SymTable_.identitiesWithBareName(Name);
      if (All.size() == 1)
        Name = All.front();
    }
  }
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

  // LEC-006: a generic operator (`operator<T> [](l: List<T>, i: int): T`)
  // gets two extra rules, both because it is matched by unification rather
  // than by an exact operand name, so a sloppy declaration would not just be
  // wrong, it would apply to far more than its author meant.
  if (!Sym.TypeParams.empty()) {
    // (1) Every type parameter must be bound by an operand. Operator calls
    // have no explicit type-argument syntax, and a parameter that appears
    // only in the return type (`operator<T> [](l: List<int>, i: int): T`)
    // could never be inferred -- it would be rejected at every use, with an
    // error that names the call and not the declaration that caused it.
    std::vector<std::string> Bound;
    for (const auto &P : Sym.Params)
      TypeWalk::collectParamOrder(P.Type, Bound);
    for (const auto &TP : Sym.TypeParams) {
      if (std::find(Bound.begin(), Bound.end(), TP) == Bound.end()) {
        GenericOperatorError(
            toSourceLocation(Sym.DeclLoc),
            "'operator " + OpSpelling + "' declares type parameter '" + TP +
                "', but no operand mentions it.",
            "A generic operator's type arguments are inferred from its "
            "operands (there is no way to write them at a use). Use '" + TP +
                "' in an operand type, or drop it from the '<...>' list.")
            .raise();
        return;
      }
    }

    // (2) The orphan rule below must be satisfied, not vacuously passed.
    // For a concrete operator, "no operand is a declared struct/enum" means
    // the operands are all builtins/`string`, so there is nobody else's type
    // to redefine and the rule has nothing to say. A generic operator is
    // different: `operator<T> +(a: T, b: T): T` has no declared type in it
    // and yet matches EVERY type, including `Vec2`s that belong to other
    // modules -- the exact import-order dependence the orphan rule exists to
    // prevent. So an owned, declared type must appear in the operands, and
    // it is looked for through `&`/`*` and generic arguments' base
    // (`List<T>` is owned by whoever declares `List`).
    auto OwnedBase = [&](const TurfType *T) -> const TurfType * {
      while (T && T->isPointer()) {
        const auto *P = dynamic_cast<const TurfPointerType *>(T);
        T = P ? P->getPointee().get() : nullptr;
      }
      return T;
    };
    auto BaseNameOf = [](const TurfType *T) -> std::string {
      if (const auto *G = dynamic_cast<const GenericType *>(T))
        return G->getBaseName();
      return T->getName();
    };
    bool OwnsOne = false;
    std::string ForeignName, ForeignModule;
    for (const auto *Op : Operands) {
      const TurfType *T = OwnedBase(Op);
      if (!T)
        continue;
      const std::string Base = BaseNameOf(T);
      if (!SymTable_.isKnownStruct(Base) && !SymTable_.isEnumName(Base))
        continue;
      if (SymTable_.getTypeOwningModule(Base) == Sym.SourceModule) {
        OwnsOne = true;
        break;
      }
      if (ForeignName.empty()) {
        ForeignName = Base;
        ForeignModule = SymTable_.getTypeOwningModule(Base);
      }
    }
    if (!OwnsOne) {
      if (!ForeignName.empty()) {
        OperatorOrphanRuleError(toSourceLocation(Sym.DeclLoc), OpSpelling,
                                Sym.SourceModule, ForeignName, ForeignModule)
            .raise();
      } else {
        GenericOperatorError(
            toSourceLocation(Sym.DeclLoc),
            "'operator " + OpSpelling + "' is generic but none of its "
            "operands is a type declared in this " +
                (Sym.SourceModule.empty() ? "file" : "module") + ".",
            "A bare type parameter matches every type, so the operator would "
            "apply to types this module does not own. Make an operand a "
            "struct or enum declared here, e.g. 'List<T>' in the module that "
            "declares List.")
            .raise();
      }
      return;
    }
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

  // Struct/enum/machine/error declarations were registered by hoistTypes()
  // (Pass 1a, FDN-003) before this runs -- nothing left to do for them here.
  if (dynamic_cast<const StructDeclExprAST *>(Node) ||
      dynamic_cast<const EnumDeclExprAST *>(Node) ||
      dynamic_cast<const MachineDeclExprAST *>(Node) ||
      dynamic_cast<const ErrorDeclExprAST *>(Node))
    return;

  // Hoist function definitions: register the signature (NOT the body).
  if (const auto *Func = dynamic_cast<const FuncDefExprAST *>(Node)) {
    registerFunctionSignature(*Func, currentModuleName());
    return;
  }

  // Hoist extern function declarations: register the signature in the
  // current-file function registry with an IsExtern flag so forward-calling
  // code can resolve the name.  No body exists, so no body recursion.
  if (const auto *Extern = dynamic_cast<const ExternFuncDeclExprAST *>(Node)) {
    // "" = the global (file) namespace
    SymTable_.registerCurrentFileFunction(buildExternSymbol(*Extern, ""));
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

  // Struct/enum/machine/error declarations are not handled here: hoistTypes()
  // (Pass 1a, FDN-003) already registered every one, in every module body.
  for (const auto &Expr : Block->getExpressions()) {
    if (const auto *Func = dynamic_cast<const FuncDefExprAST *>(Expr.get())) {
      registerFunctionSignature(*Func, ModuleName);
    } else if (const auto *Extern =
                   dynamic_cast<const ExternFuncDeclExprAST *>(Expr.get())) {
      SymTable_.registerModuleFunction(ModuleName,
                                       buildExternSymbol(*Extern, ModuleName));
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
 * OVH-004: a `GenericType` field first tries its CONCRETE instantiation --
 * `Monomorphize::instantiatedDeclName(BaseName, TypeArgs)`, the exact same
 * mangled name `drainStructRequests()` gives the clone it registers into
 * @p KnownStructs under `Clone->getName()` before calling
 * `checkStructLayoutAcyclicityForClone()` -- before falling back to the bare
 * base name. Without this, `struct A<T> { val: T }` + `struct B { back:
 * A<B> }` silently escaped detection: the base-name fallback always points
 * at the shared TEMPLATE decl for "A", whose own field is still the
 * unsubstituted `TurfGenericParamType` placeholder `T` (no further edges),
 * so the walk into `A<B>` from `B` never reached back to the concrete clone
 * `A_gen1_B` that is actually on the DFS path -- the cycle closed only in
 * reality, never in this graph. The mangled lookup is a pure "is this exact
 * instantiation already a known node" query: it costs nothing when it
 * misses (every Pass-1 template-only call, where no clone has ever been
 * registered) and cannot fabricate a false edge, since @p KnownStructs only
 * ever contains real, already-hoisted-or-cloned struct decls.
 *
 * @param KnownStructs Every struct name reached so far (HoistedStructDecls_
 *                     keyed by name, plus -- for the per-clone call -- the
 *                     one new clone under its own mangled name) -- an edge
 *                     target that isn't in this set is either a
 *                     primitive/generic-param placeholder, a not-yet-drained
 *                     instantiation, or a genuinely unknown name (Pass 2's
 *                     validateNamedType() raises UnknownTypeError for the
 *                     last case; this check runs before that and simply
 *                     finds no edge either way).
 */
std::string byValueEdgeTarget(
    const TurfType *T,
    const std::unordered_map<std::string, const StructDeclExprAST *> &KnownStructs) {
  if (!T || T->isPointer())
    return "";

  if (isArrayType(T))
    return byValueEdgeTarget(getArrayElementType(T), KnownStructs);

  if (const auto *GT = dynamic_cast<const GenericType *>(T)) {
    std::string Mangled =
        Monomorphize::instantiatedDeclName(GT->getBaseName(), GT->getTypeArgs());
    if (KnownStructs.count(Mangled))
      return Mangled;
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
  // type registry (RegisterTypeIfOwned below) before giving up.
  //
  // LEC-002: only `pub` overloads are imported. A private overload is skipped
  // silently for a `[*]` import; for an explicit request the caller checks
  // @p SawPrivate to tell "not there" from "there, but private".
  auto RegisterFunctionIfPresent = [&](const std::string &Name,
                                       SourceLocation Loc,
                                       bool &SawPrivate) -> bool {
    bool Found = false;
    for (const auto &Match : SymTable_.findInModules(Name)) {
      if (Match.ModuleName != SourceModule)
        continue;
      if (!Match.Symbol->IsPublic) {
        SawPrivate = true;
        continue;
      }
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
  // actually owns it -- a type spelled Name that merely exists in some OTHER
  // module must not satisfy `import SourceModule::Name;`. FDN-003: "does
  // SourceModule own a type spelled Name" is exactly "is SourceModule::Name
  // a registered identity". Already registered once by
  // registerOnDemandTypeImports() in Pass 1a; re-registering the same
  // (context, name, module) triple below is a no-op. Returns whether Name is
  // a real type owned by SourceModule at all.
  auto RegisterTypeIfOwned = [&](const std::string &Name, SourceLocation Loc,
                                 bool &SawPrivate) -> bool {
    const std::string Id = TypeIdentity::make(SourceModule, Name);
    if (!SymTable_.isKnownTypeIdentity(Id))
      return false;
    // LEC-002: a private type is not importable (see registerOnDemandType-
    // Imports(), which skips it for the same reason).
    if (!SymTable_.isTypeVisibleFrom(Id, ContextLabel)) {
      SawPrivate = true;
      return false;
    }
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
    bool IgnoredPrivate = false; // a `[*]` import just skips private names
    for (const auto &Name : SymTable_.getModuleFunctionNames(SourceModule))
      RegisterFunctionIfPresent(Name, Import.getLoc(), IgnoredPrivate);
    for (const auto &Name : SymTable_.getModuleTypeNames(SourceModule))
      RegisterTypeIfOwned(Name, Import.getLoc(), IgnoredPrivate);
    for (const auto &Name : SymTable_.getModuleConstantNames(SourceModule))
      RegisterConstantIfPresent(Name, Import.getLoc());
    return;
  }

  // Symbol / SymbolList: each requested name must resolve to SOMETHING
  // SourceModule actually owns -- a function, a struct/enum, or a constant.
  for (const auto &Ref : Import.getRequestedSymbols()) {
    bool PrivateFn = false, PrivateType = false;
    bool FoundFn = RegisterFunctionIfPresent(Ref.Name, Ref.Loc, PrivateFn);
    bool FoundType = RegisterTypeIfOwned(Ref.Name, Ref.Loc, PrivateType);
    bool FoundConst = RegisterConstantIfPresent(Ref.Name, Ref.Loc);
    if (!FoundFn && !FoundType && !FoundConst && (PrivateFn || PrivateType)) {
      // LEC-002: the module HAS this name -- it just isn't `pub`. That is a
      // different problem from a misspelling and needs a different hint.
      if (PrivateFn)
        raisePrivateFunction(Ref.Loc, SourceModule, Ref.Name);
      else
        raisePrivateType(Ref.Loc, TypeIdentity::make(SourceModule, Ref.Name));
      continue;
    }
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
