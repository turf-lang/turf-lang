/**
 * @file ResolverCore.cpp
 * @brief Entry points, hoisting pass (forward-declaration registration), and shared cross-category helpers.
 * @details Part of the Resolver name-resolution pass (Issue #18), split from
 *          the original monolithic Resolver.cpp by category (mirrors
 *          AST.h's ASTExpr/ASTStmt/ASTDecl/ASTAccess split -- see
 *          docs/planning/COMPILER_MODULE_BREAKDOWN.md).
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
#include "ResolverSymbolTable.h"
#include "ResolverInternal.h"
#include "SuggestionIndex.h" // SuggestionIndex::suggestVisible
#include "Types.h"           // RankedSuggestions

#include <memory>
#include <unordered_set>


// ─── Public entry points ──────────────────────────────────────────────────────

void Resolver::resolve(ExprAST *Node) {
  if (Node)
    Node->accept(*this);
}


void Resolver::resolveAll(
    const std::vector<std::unique_ptr<ExprAST>> &Nodes) {
  // ─────────────────────────────────────────────────────────────────────────
  // Pass 1 — Hoisting
  // Walk only top-level nodes (no body recursion).
  // Register every struct name and function signature so that Pass 2
  // can resolve forward references (foo calling bar defined below, or
  // a struct field using a type declared later in the file).
  // ─────────────────────────────────────────────────────────────────────────
  hoistTopLevel(Nodes);

  // ─────────────────────────────────────────────────────────────────────────
  // Pass 2 — Full Resolution
  // Walk the entire AST (all bodies). Forward references resolve correctly
  // because Pass 1 has already registered all struct names and signatures.
  // ─────────────────────────────────────────────────────────────────────────
  for (const auto &Node : Nodes)
    resolve(Node.get());
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
 * statements (docs/planning/Errors.md), for ResolverFunctionSymbol::
 * PossibleErrorTypeNames -- see that field's doc comment for why this is
 * intentionally unfiltered (not yet checked against isErrorTypeName()) and
 * why the scan is conservative/non-interprocedural, same rationale as
 * MutatesSelfField() just above (mirrors its exact recursion shape).
 *
 * Recognizes `return ErrorType::Variant;` (a bare ScopeAccessExprAST -- error
 * variants are always bare, non-callable references, exactly like `enum`
 * variants; there's no `ErrorType::Variant(...)` call shape to also check
 * for), collecting `Left` (e.g. "MathError").
 */
static void CollectReturnedErrorTypeNames(const ExprAST *E,
                                          std::vector<std::string> &Out) {
  if (!E)
    return;

  if (auto *Ret = dynamic_cast<const ReturnExprAST *>(E)) {
    if (auto *SA = dynamic_cast<const ScopeAccessExprAST *>(Ret->getVal()))
      Out.push_back(SA->getLeftName());
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
  ResolverSourceLocation DeclLoc;
  DeclLoc.Line = Node.getLoc().Line;
  DeclLoc.Col = Node.getLoc().Col;
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

  if (Sym.Params.size() != 2)
    return; // arity mismatch is caught elsewhere (parser); nothing to check here

  const TurfType *LhsTy = Sym.Params[0].Type.get();
  const TurfType *RhsTy = Sym.Params[1].Type.get();
  if (!IsBuiltinOperandType(LhsTy) || !IsBuiltinOperandType(RhsTy))
    return;

  std::string OpSpelling = Sym.BaseName.substr(8); // strip "operator" prefix
  OperatorBuiltinPairError(toSourceLocation(Sym.DeclLoc), OpSpelling,
                           LhsTy->getName(), RhsTy->getName())
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
    if (Num->isInteger())
      Sym.IntVal = Num->getIntVal();
    else
      Sym.DoubleVal = Num->getDoubleVal();
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

  // Hoist `error` type declarations (docs/planning/Errors.md): register the
  // name and variants so NamedType/ScopeAccess references resolve in Pass 2.
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
      auto Sugg = SuggestionIndex::suggestVisible(
          Ref.Name, SymTable_.getModuleFunctionNames(SourceModule));
      if (!Sugg.empty())
        Suggestions.Module[SourceModule] = Sugg;
      UnknownModuleSymbolError(Ref.Loc, SourceModule, Ref.Name, Suggestions).raise();
    }
  }
}
