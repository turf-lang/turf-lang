/**
 * @file Resolver.cpp
 * @brief Implementation of the Resolver name-resolution pass (Issue #18).
 *
 * Two-pass architecture:
 *   Pass 1 — hoistTopLevel(): walk top-level nodes, register struct names
 *             and function signatures so forward references are visible.
 *   Pass 2 — full visitor traversal: resolve variables, calls, type names.
 *
 * Each visitXxx override either:
 *   (a) performs semantic work (VarDecl, Variable, Block, FuncDef,
 *       FuncCall, MethodCall, StructDecl — now with name validation), or
 *   (b) recurses into child nodes so the semantic handlers are reachable
 *       regardless of where they sit in the AST.
 *
 * This file *does* depend on Errors.h (and therefore on LLVM headers via the
 * transitive include chain), because it raises ReferenceError /
 * DuplicateDeclarationError which use the project's diagnostic engine.
 * The Resolver.h header itself is LLVM-free.
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

static SourceLocation toSourceLocation(ResolverSourceLocation R) {
  return {R.Line, R.Col};
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
static TurfTypePtr substituteTypeParams(const TurfTypePtr &Ty,
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
  return false;
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
  return Sym;
}

void Resolver::registerFunctionSignature(const FuncDefExprAST &Node,
                                         const std::string &ModuleName) {
  auto Sym = buildFunctionSymbol(Node, ModuleName);
  if (ModuleName.empty()) {
    SymTable_.registerCurrentFileFunction(Sym);
  } else {
    SymTable_.registerModuleFunction(ModuleName, Sym);
  }
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


// ─── Core Issue #4 visitors ───────────────────────────────────────────────────

// Walk VarDecl nodes: recurse into the initializer first (so the RHS can't
// see the variable being declared, matching Turf's semantics), then add the
// name to the current scope.
void Resolver::visitVarDecl(VarDeclExprAST &Node) {
  if (Node.isExported()) {
    // `export` is only legal at the top level (FunctionNestingDepth_ is
    // incremented only for the duration of a function body traversal --
    // see visitFuncDef) -- grammatically this node parses in ANY statement
    // position (see its doc comment in AST.h), so this is the ONLY place
    // that rule is actually enforced.
    if (FunctionNestingDepth_ > 0) {
      ExportOutsideTopLevelError(Node.getLoc(), Node.getName()).raise();
      return;
    }
    // Already fully registered by hoistExportedConstant() in Pass 1 -- an
    // exported constant has no real scope storage (declareVariable() is
    // never called for it), so there is nothing left for Pass 2 to do here.
    return;
  }

  // Validate the declared type name -- struct fields, function params, and
  // return types already go through validateNamedType() (see its doc
  // comment), but a plain variable declaration never did. Without this,
  // `mut x: NoSuchStruct = 5;` compiled with no "unknown type" diagnostic at
  // all -- it silently reached the TypeChecker's ordinary type-MISMATCH path
  // instead ("Cannot initialise 'NoSuchStruct' variable 'x' with value of
  // type 'int'"), which is real but misleading: it implies NoSuchStruct is
  // a genuine type that just doesn't match int, not that the name itself
  // was never declared anywhere.
  validateNamedType(Node.getTypePtr(), Node.getLoc(), CurrentFuncTypeParams_);

  // 1. Resolve the initializer expression before registering the new name.
  //    This prevents `int x = x;` from passing the resolver.
  visitChild(Node.getInitializer());

  // 2. Register in the current scope.
  //    declareVariable() throws std::runtime_error on re-declaration in the
  //    same scope; we catch it and re-raise as a proper compiler diagnostic.
  //    On success it returns the shared_ptr we attach to the AST node (#5).
  try {
    ResolverSourceLocation Loc{Node.getLoc().Line, Node.getLoc().Col};
    Node.ResolvedSymbol =
        SymTable_.declareVariable(Node.getName(), Node.getTypePtr(), Loc, Node.isImmutable());
  } catch (const std::runtime_error &) {
    // The name is already in the current scope — emit a rich diagnostic.
    // Look up the previous declaration for the "declared at line N" note.
    auto Prev = SymTable_.lookupVariable(Node.getName());
    if (Prev) {
      SourceLocation PrevLoc{Prev->DeclLoc.Line, Prev->DeclLoc.Col};
      DuplicateDeclarationError(Node.getLoc(), Node.getName(), PrevLoc).raise();
    } else {
      // Fallback (should not happen, but be safe).
      SyntaxError(Node.getLoc(), "Variable '" + Node.getName() +
                                     "' is already declared in this scope.")
          .raise();
    }
  }
}

// Walk Block nodes: bracket child traversal with enterScope/exitScope.
void Resolver::visitBlock(BlockExprAST &Node) {
  SymTable_.enterScope();
  for (const auto &Expr : Node.getExpressions())
    visitChild(Expr.get());
  SymTable_.exitScope();
}

// Walk Variable (VarExpr) nodes: look up; emit ReferenceError on miss.
void Resolver::visitVariable(VariableExprAST &Node) {
  auto Sym = SymTable_.lookupVariable(Node.getName());
  if (Sym) {
    // Issue #5: attach the resolved symbol to the AST node so future passes
    // (TypeChecker, Codegen) can read it directly without a second lookup.
    Node.ResolvedSymbol = Sym;
    return;
  }

  // Bare reference to an `export immut` constant -- checked AFTER ordinary
  // scoped variables (a local shadowing an exported name is not an error,
  // same shadowing rule as everywhere else) and BEFORE the ReferenceError
  // fallback. Two sources, mirroring bare function-call resolution exactly:
  // this file's own exported constants (self-reference, always visible, no
  // import needed), then on-demand-imported ones (registerFlatConstantImport,
  // via `import module::CONST;`).
  if (const auto *Const = SymTable_.lookupCurrentFileConstant(Node.getName())) {
    Node.ResolvedConstant = Const;
    return;
  }
  if (const auto *Flat = SymTable_.lookupFlatConstantImport(currentModuleName(), Node.getName())) {
    if (const auto *Const = SymTable_.findModuleConstant(Flat->SourceModule, Node.getName())) {
      Node.ResolvedConstant = Const;
      return;
    }
  }

  // Build Damerau-Levenshtein suggestions from every name currently in scope.
  std::vector<std::string> VisibleNames = SymTable_.getAllVisibleNames();
  RankedSuggestions Suggestions;
  Suggestions.Local =
      SuggestionIndex::suggestVisible(Node.getName(), VisibleNames);

  ReferenceError(Node.getLoc(), Node.getName(), Suggestions).raise();
}

// ─── Structural visitors (recurse into children) ──────────────────────────────

void Resolver::visitBinary(BinaryExprAST &Node) {
  visitChild(Node.getLHS());
  visitChild(Node.getRHS());
}

void Resolver::visitUnary(UnaryExprAST &Node) {
  visitChild(Node.getOperand());
}

void Resolver::visitAddressOf(AddressOfExprAST &Node) {
  // Recurse so the operand variable is resolved before codegen reads it.
  visitChild(Node.getOperand());
}

void Resolver::visitAssignment(AssignmentExprAST &Node) {
  // Check RHS first, then LHS (which may be a variable that must exist).
  visitChild(Node.getRHS());
  visitChild(Node.getLHS());
}

void Resolver::visitCast(CastExprAST &Node) {
  visitChild(Node.getOperand());
}

void Resolver::visitCompoundAssignment(CompoundAssignmentExprAST &Node) {
  visitChild(Node.getRHS());
  visitChild(Node.getLHS());
}

void Resolver::visitPostfixExpr(PostfixExprAST &Node) {
  visitChild(Node.getOperand());
}


void Resolver::visitIf(IfExprAST &Node) {
  for (const auto &Branch : Node.getBranches()) {
    visitChild(Branch.Cond.get());
    visitChild(Branch.Body.get());
  }
  visitChild(Node.getElseBody());
}

void Resolver::visitWhile(WhileExprAST &Node) {
  visitChild(Node.getCond());
  visitChild(Node.getBody());
}

void Resolver::visitFor(ForExprAST &Node) {
  // ForExprAST desugars to:  { <var> = start;  while <var> < end { body; step } }
  // The Resolver mirrors that structure:
  //   1. Resolve start and end expressions in the *outer* scope (they can't
  //      reference the loop variable itself — it doesn't exist yet).
  //   2. Push a new scope for the loop variable + body + step.
  //   3. Register the induction variable as int (Codegen infers int from the
  //      start value, which is the same rule we apply here).
  //   4. Resolve the body and step inside that scope.
  //   5. Exit the scope so the loop variable doesn't leak.

  visitChild(Node.getStart());
  visitChild(Node.getEnd());

  SymTable_.enterScope();

  // Register the induction variable as int — mirrors Codegen's type inference.
  // If the start expression is a double literal the TypeChecker (Issue #7)
  // will refine this, but int is the right default for range-based loops.
  // Capture the returned symbol and attach it to the AST node (Issue #5).
  try {
    ResolverSourceLocation Loc{Node.getLoc().Line, Node.getLoc().Col};
    Node.ResolvedLoopVar =
        SymTable_.declareVariable(Node.getVarName(), TurfInt(), Loc);
  } catch (const std::runtime_error &) {
    auto Prev = SymTable_.lookupVariable(Node.getVarName());
    SourceLocation ErrLoc = Node.getLoc();
    if (Prev) {
      SourceLocation PrevLoc{Prev->DeclLoc.Line, Prev->DeclLoc.Col};
      DuplicateDeclarationError(ErrLoc, Node.getVarName(), PrevLoc).raise();
    } else {
      SyntaxError(ErrLoc, "Loop variable '" + Node.getVarName() +
                               "' conflicts with an existing declaration.")
          .raise();
    }
  }

  // Step must be resolved inside the scope so it can reference the loop var
  // (e.g. `step i += 1` uses i).
  visitChild(Node.getStep());
  visitChild(Node.getBody());

  SymTable_.exitScope();
}

void Resolver::visitReturn(ReturnExprAST &Node) {
  visitChild(Node.getVal());
}

void Resolver::visitCompare(CompareExprAST &Node) {
  // Resolve the compare subject
  visitChild(Node.getSubjectMut());

  // Resolve each arm: patterns then body (in its own scope)
  for (auto &Arm : Node.getArmsMut()) {
    for (auto &Val : Arm.Values)
      visitChild(Val.get());
    // Each arm body gets its own scope so declarations don't leak between arms
    SymTable_.enterScope();
    visitChild(Arm.Body.get());
    SymTable_.exitScope();
  }

  // Resolve the otherwise body if present
  if (Node.getOtherBodyMut()) {
    SymTable_.enterScope();
    visitChild(Node.getOtherBodyMut());
    SymTable_.exitScope();
  }
}

void Resolver::visitFuncDef(FuncDefExprAST &Node) {
  if (FunctionNestingDepth_ > 0) {
    NestedFunctionError(Node.getLoc(), Node.getName()).raise();
    return;
  }

  // Rewrite any parameter/return NamedType("T") that names one of this
  // function's own TypeParams into a real TurfGenericParamType("T") --
  // ParseType() always parses a bare identifier as NamedType regardless of
  // whether it is a generic parameter (see substituteTypeParams()'s doc
  // comment above). This mutates the shared AST node in place, BEFORE
  // TypeChecker or Codegen ever look at Params/ReturnType, so both later
  // passes see a real TurfGenericParamType uniformly -- Codegen in
  // particular erases TurfGenericParamType to void* (getLLVMType()), but
  // was previously building a bogus empty opaque LLVM struct named "T" for
  // the un-substituted NamedType("T"), corrupting any function actually
  // exercising a bare generic-typed parameter (confirmed via
  // `fn<T> identity(x: T): T` crashing the LLVM backend with a malformed
  // fptosi during instruction selection).
  const auto &TypeParams = Node.getTypeParams();
  if (!TypeParams.empty()) {
    for (auto &Param : Node.getParamsMut())
      Param.Type = substituteTypeParams(Param.Type, TypeParams);
    Node.setReturnType(substituteTypeParams(Node.getReturnTypePtr(), TypeParams));
  }

  // Validate every parameter type and the return type name a real primitive
  // or a struct/enum registered in Pass 1 -- see validateNamedType()'s doc
  // comment for the gap this closes. This also validates the implicit
  // `self` parameter of a struct method (ParseFuncDef builds it as
  // TurfPointerType(NamedType(StructName))), so a genuinely misspelled
  // struct name in a method declaration (`fn Usre.grow()`) is caught here --
  // that specific check used to live in the Parser as an eager
  // GlobalSymbolTable lookup, but had to be made lazy to support struct
  // methods declared before their struct (see ParseFuncDef's comment); this
  // is where that validation now correctly belongs instead. (By this point
  // the substitution above has already turned any of this function's own
  // TypeParams into TurfGenericParamType, so the TypeParams argument here is
  // now mostly a defense-in-depth no-op for this call site -- but is kept
  // since validateNamedType() is shared with visitStructDecl, where it is
  // not.)
  for (const auto &Param : Node.getParams())
    validateNamedType(Param.Type, Node.getLoc(), Node.getTypeParams());
  validateNamedType(Node.getReturnTypePtr(), Node.getLoc(),
                    Node.getTypeParams());

  ++FunctionNestingDepth_;

  // Each function body gets its own scope; parameters are visible inside it.
  SymTable_.enterScope();

  for (const auto &Param : Node.getParams()) {
    try {
      // ParamDecl::Loc is now populated with the parameter name's real
      // source location (see its doc comment in AST.h) -- previously
      // hardcoded {0,0} here, which made a duplicate-parameter error's
      // "declared here" note always say "line 0, column 0" for the FIRST
      // occurrence, regardless of where it actually was.
      ResolverSourceLocation ParamLoc{Param.Loc.Line, Param.Loc.Col};
      SymTable_.declareVariable(Param.Name, Param.Type, ParamLoc);
    } catch (const std::runtime_error &) {
      auto Prev = SymTable_.lookupVariable(Param.Name);
      // Point the error itself at the DUPLICATE parameter's own occurrence
      // rather than the function's declaration -- was Node.getLoc() (the
      // 'fn' keyword), which put the caret nowhere near either 'x' in
      // `fn add(x: int, x: int)`.
      SourceLocation ErrLoc = Param.Loc;
      if (Prev) {
        SourceLocation PrevLoc{Prev->DeclLoc.Line, Prev->DeclLoc.Col};
        DuplicateDeclarationError(ErrLoc, Param.Name, PrevLoc).raise();
      } else {
        SyntaxError(ErrLoc, "Duplicate declaration of '" + Param.Name + "'").raise();
      }
    }
  }

  auto Sym = std::make_shared<ResolverFunctionSymbol>(
      buildFunctionSymbol(Node, currentModuleName()));
  Node.ResolvedDef = Sym;

  registerFunctionSignature(Node, currentModuleName());

  // See CurrentFuncTypeParams_'s doc comment in Resolver.h.
  CurrentFuncTypeParams_ = Node.getTypeParams();
  visitChild(Node.getBody());
  CurrentFuncTypeParams_.clear();

  SymTable_.exitScope();
  --FunctionNestingDepth_;
}

void Resolver::visitFuncCall(FuncCallExprAST &Node) {
  // Resolve argument expressions first so any nested variable lookups happen
  // before we inspect the callee.
  for (const auto &Arg : Node.getArgs())
    visitChild(Arg.get());

  Node.ResolvedCandidates.clear();

  const auto *VarCallee = dynamic_cast<const VariableExprAST *>(Node.getCallee());
  // `Module::func(args)` -- a ScopeAccessExprAST callee explicitly naming
  // its module, as opposed to VarCallee's bare/self-reference case below.
  // Handled here, specially and narrowly, rather than via the generic
  // visitScopeAccess() dispatch -- exactly mirroring how VarCallee is
  // special-cased instead of going through visitVariable(). A ScopeAccessExprAST
  // that ISN'T in call position (e.g. used as a bare value) never reaches
  // this branch at all; it falls through to visitChild() below and is
  // handled by visitScopeAccess() instead, which reports it as invalid
  // (no first-class functions).
  auto *ScopeCallee = dynamic_cast<ScopeAccessExprAST *>(
      const_cast<ExprAST *>(Node.getCallee()));
  if (!VarCallee && !ScopeCallee) {
    visitChild(Node.getCallee());
    return;
  }

  std::string CalleeName;
  std::vector<const ResolverFunctionSymbol *> CurrentFileMatches;
  std::vector<ResolverSymbolTable::ModuleFunctionMatch> ModuleMatches;

  if (VarCallee) {
    CalleeName = VarCallee->getName();
    CurrentFileMatches = SymTable_.lookupCurrentFileFunctions(CalleeName);

    // A bare (unqualified) call may only resolve to a function declared in
    // the SAME module currently being walked -- i.e. genuine self-
    // reference, like math.tr's abs() calling its own fabs() bare, or
    // io.tr's print() calling its own printf() bare. It must NOT resolve
    // to some OTHER transitively-imported module's function just because
    // that module happens to be loaded somewhere in the program.
    //
    // This used to be an unconditional, unscoped SymTable_.findInModules(...)
    // search across EVERY loaded module, with no check that the CURRENT
    // file itself imported (let alone flat-imported) anything at all --
    // confirmed live: bare `printline("x");` compiled successfully with
    // ZERO import statements anywhere in user code, because
    // lib/std/prelude.tr always `import io;`-ed for every compile and this
    // lookup didn't care who actually asked for that visibility.
    // prelude.tr is gone now (see its removal), but the underlying
    // promiscuous lookup was the real bug, not just prelude.tr's existence
    // -- fixed here by scoping the search to currentModuleName() (empty =
    // main file, which has no "self" module to search) instead of every
    // module ever loaded.
    const std::string &SelfModule = currentModuleName();
    if (!SelfModule.empty()) {
      for (const auto &Match : SymTable_.findInModules(CalleeName))
        if (Match.ModuleName == SelfModule)
          ModuleMatches.push_back(Match);
    }

    // Also allow a bare call to resolve via an explicit on-demand import
    // (`import module::CalleeName;` / `module::[..., CalleeName, ...]` /
    // `module::[*]`) made by THIS same context -- currentModuleName() is
    // exactly the right key (empty = main file), matching whatever
    // registerOnDemandImports() registered things under during hoisting.
    if (const auto *Flat = SymTable_.lookupFlatFunctionImport(SelfModule, CalleeName)) {
      for (const auto &Match : SymTable_.findInModules(CalleeName))
        if (Match.ModuleName == Flat->SourceModule)
          ModuleMatches.push_back(Match);
    }
  } else {
    // ScopeCallee: an explicit `Left::Right(args)` call.
    CalleeName = ScopeCallee->getRightName();
    const std::string &LeftName = ScopeCallee->getLeftName();

    if (SymTable_.isEnumName(LeftName)) {
      // e.g. `Color::Red(...)` -- a real enum, but variants aren't callable.
      ScopeCallee->ResolvedKind = ScopeAccessKind::EnumVariant;
      TypeError(Node.getLoc(), "'" + LeftName + "::" + CalleeName +
                                   "' is an enum variant, not a function -- "
                                   "it can't be called.")
          .raise();
      return;
    }

    if (SymTable_.isKnownStruct(LeftName)) {
      // `StructName::method(args)` -- a true STATIC method call: no
      // instance involved at all (unlike `instance.method(args)`, handled
      // entirely separately by visitMethodCall/MethodCallExprAST). A
      // static method is registered under the exact same "StructName.method"
      // internal name an INSTANCE method would be (see ParseFuncDef()'s
      // `::`-declaration branch), just without a `self` parameter -- so the
      // lookup here is byte-identical to the ModuleFunctionRef branch below,
      // just scoped to the struct's OWNING module (main file "" or a real
      // module name) instead of a qualifier that names a module directly.
      // Qualified access is never gated by bare-type visibility (2.7.1 in
      // the architecture doc) -- isKnownStruct() alone (existence) is the
      // only requirement, exactly like a qualified type annotation.
      ScopeCallee->ResolvedKind = ScopeAccessKind::StaticMethodRef;
      // `Module::StructName::method(...)` -- the parser reinterprets a
      // second consecutive '::' the same way it does for
      // Module::EnumName::Variant (see ParsePrimaryBase()'s TOK_SCOPE
      // branch), landing the leading module name in ModuleQualifier rather
      // than LeftName/RightName. Without this check, a WRONG qualifier
      // (`mem::Vec2::zero()` when Vec2 actually lives in a different
      // module) was silently ignored entirely -- LeftName/RightName alone
      // determine the lookup below, and ModuleQualifier was never
      // consulted, so any qualifier (right or wrong) produced the same
      // result. Confirmed live before this check existed.
      if (!validateModuleQualifier(ScopeCallee->getModuleQualifier(), LeftName,
                                   Node.getLoc()))
        return;
      const std::string OwningModule = SymTable_.getTypeOwningModule(LeftName);
      const std::string MethodKey = LeftName + "." + CalleeName;
      if (OwningModule.empty()) {
        CurrentFileMatches = SymTable_.lookupCurrentFileFunctions(MethodKey);
      } else {
        for (const auto &Match : SymTable_.findInModules(MethodKey))
          if (Match.ModuleName == OwningModule)
            ModuleMatches.push_back(Match);
      }
    } else if (!SymTable_.hasModule(LeftName)) {
      // Not an enum, struct, or module -- but if it's a real, declared
      // variable (an instance), give the specific "you meant '.'" error
      // instead of the generic "unknown qualifier" one. `p::get()` where
      // `p: SomeStruct` is a genuinely common mistake to make right after
      // learning static methods exist, and "isn't a known enum, struct,
      // or module" doesn't actually tell the user what they did wrong.
      if (SymTable_.lookupVariable(LeftName)) {
        ScopeOperatorOnInstanceError(Node.getLoc(), LeftName, CalleeName).raise();
        return;
      }
      RankedSuggestions QualifierSuggestions;
      auto EnumSugg = SuggestionIndex::suggestVisible(LeftName, SymTable_.getAllEnumNames());
      for (const auto &S : EnumSugg)
        QualifierSuggestions.Local.push_back(S + "::" + CalleeName);
      UnknownScopeQualifierError(Node.getLoc(), LeftName, QualifierSuggestions).raise();
      return;
    } else {
      ScopeCallee->ResolvedKind = ScopeAccessKind::ModuleFunctionRef;
      // A 3-part `ModuleQualifier::LeftName::RightName` chain only has a
      // defined meaning when LeftName is an enum or struct (Module owns
      // it -- validated above/below); modules aren't nested inside other
      // modules via '::' at all, so a qualifier here is always meaningless.
      // Without this check it was silently IGNORED entirely -- LeftName
      // alone determined the lookup, so `mem::io::printline(...)` compiled
      // identically to `io::printline(...)` regardless of what the leading
      // qualifier said. Confirmed live before this check existed.
      if (!ScopeCallee->getModuleQualifier().empty()) {
        TypeError(Node.getLoc(), "'" + ScopeCallee->getModuleQualifier() +
                                     "::" + LeftName +
                                     "' -- modules can't be qualified by "
                                     "another module; just write '" +
                                     LeftName + "::" + CalleeName + "(...)'.")
            .raise();
        return;
      }
      // Explicit qualification means the search is scoped to EXACTLY the
      // named module -- no self-reference concept applies here, unlike the
      // bare-call case above.
      for (const auto &Match : SymTable_.findInModules(CalleeName))
        if (Match.ModuleName == LeftName)
          ModuleMatches.push_back(Match);
    }
  }

  if (CurrentFileMatches.empty() && ModuleMatches.empty()) {
    // An explicit `Module::func(...)` call where Module IS real but doesn't
    // have `func` needs its own, much more specific error than the bare-call
    // path below -- that path's hints ("import the module that defines it")
    // are actively confusing once the user has ALREADY written the
    // qualifier. ModuleMethodNotFoundError existed in Errors.h for exactly
    // this shape (written for the old dot-based module-call syntax) but was
    // never actually wired up anywhere -- confirmed by grep, dead code until
    // now.
    if (ScopeCallee && ScopeCallee->ResolvedKind == ScopeAccessKind::ModuleFunctionRef) {
      const std::string &LeftName = ScopeCallee->getLeftName();
      RankedSuggestions Suggestions;
      Suggestions.Local = SuggestionIndex::suggestVisible(
          CalleeName, SymTable_.getModuleFunctionNames(LeftName));
      ModuleMethodNotFoundError(Node.getLoc(), LeftName, CalleeName, Suggestions).raise();
      return;
    }

    // Same idea as ModuleMethodNotFoundError above, for the struct case:
    // `StructName::method(...)` where StructName is real but has no
    // (static OR instance -- either would show up here, since both share
    // the "StructName.method" registration name) method by that name.
    // Reuses StructMethodNotFoundError, the exact class the INSTANCE
    // (`instance.method()`) not-found path already raises, for one
    // consistent message regardless of which call form the user wrote.
    if (ScopeCallee && ScopeCallee->ResolvedKind == ScopeAccessKind::StaticMethodRef) {
      const std::string &LeftName = ScopeCallee->getLeftName();
      const std::string Prefix = LeftName + ".";
      std::vector<std::string> AllNames = SymTable_.getAllCurrentFileFunctionNames();
      std::vector<std::string> ModuleNames = SymTable_.getAllModuleFunctionNames();
      AllNames.insert(AllNames.end(), ModuleNames.begin(), ModuleNames.end());
      std::vector<std::string> BareMethodNames;
      for (const auto &N : AllNames)
        if (N.size() > Prefix.size() && N.compare(0, Prefix.size(), Prefix) == 0)
          BareMethodNames.push_back(N.substr(Prefix.size()));
      RankedSuggestions Suggestions;
      Suggestions.Local = SuggestionIndex::suggestVisible(CalleeName, BareMethodNames);
      StructMethodNotFoundError(Node.getLoc(), LeftName, CalleeName, Suggestions).raise();
      return;
    }

    // Before emitting an error, check whether the callee is a true language-level
    // bare intrinsic — one that is callable WITHOUT a module qualifier AND has no
    // module home in the standard library.
    //
    // As of now there are exactly two such intrinsics:
    //   - typeof(x)   — compile-time type name, no module
    //   - lengthof(x) — string length, no module
    //
    // Everything else comes from lib/std:
    //   - print / printline / input  → io.tr   (call as io.printline(...))
    //   - malloc / realloc / free    → mem.tr   (call as mem.malloc(...))
    //   - abs / power / sqrt / …     → math.tr  (call as math.abs(...))
    //
    // A module-qualified call (io.printline(...)) never reaches this
    // function at all -- it's a MethodCallExprAST, handled by
    // visitMethodCall instead. This function only ever sees bare,
    // unqualified names, so no stdlib function name belongs in the
    // BareIntrinsics set below no matter how "primitive" it feels.
    static const std::unordered_set<std::string> BareIntrinsics = {
        "typeof",
        "lengthof",
        "assert",
        "fail",
    };
    if (BareIntrinsics.count(CalleeName)) {
      // True bare intrinsic — skip error, TypeChecker validates arity/types.
      return;
    }

    RankedSuggestions Suggestions;

    auto CurrentNames = SuggestionIndex::suggestVisible(
        CalleeName, SymTable_.getAllCurrentFileFunctionNames());
    for (const auto &SuggestedName : CurrentNames) {
      for (const auto *Sym : SymTable_.lookupCurrentFileFunctions(SuggestedName)) {
        Suggestions.Local.push_back(Sym->signature());
      }
    }

    // Exact-name matches in OTHER modules (checked before the fuzzy pass
    // below, which deliberately skips exact matches -- see
    // SuggestionIndex::suggestFromCandidates' `Candidate == Query` guard,
    // since it exists for typo-correction, not this case). This is now the
    // single most common way to land in this branch: a real, correctly-
    // spelled function that exists in some OTHER imported module, called
    // bare with no qualifier -- e.g. `printline(x)` when `io.printline(x)`
    // was meant. Surface it as a real suggestion instead of the generic
    // "double-check the spelling" fallback, which is actively wrong here
    // (the spelling IS correct; the qualifier is just missing).
    for (const auto &Match : SymTable_.findInModules(CalleeName))
      Suggestions.Module[Match.ModuleName].push_back(Match.Symbol->signature());

    auto ModuleNames = SuggestionIndex::suggestVisible(
        CalleeName, SymTable_.getAllModuleFunctionNames());
    for (const auto &SuggestedName : ModuleNames) {
      for (const auto &Match : SymTable_.findInModules(SuggestedName)) {
        Suggestions.Module[Match.ModuleName].push_back(Match.Symbol->signature());
      }
    }

    // Issue #8 fix: also search intrinsic/builtin names (assert, fail, typeof,
    // lengthof) so typos like `asert()` produce a proper suggestion.
    auto BuiltinMatches = SuggestionIndex::suggestBuiltins(CalleeName);
    for (const auto &B : BuiltinMatches)
      Suggestions.Local.push_back(B + "(...)");

    UnknownFunctionError(Node.getLoc(), CalleeName, Suggestions).raise();
    return;
  }

  // ── Arity check ───────────────────────────────────────────────────────────
  // The symbol table already knows every overload's arity.  Before handing
  // the candidates to the TypeChecker, verify that at least one overload
  // accepts the number of arguments supplied at this call site.
  //
  // This is the authoritative arity check — no hardcoded maps anywhere.
  // WrongArityError fires here; Codegen never sees an arity mismatch.
  const int ArgCount = static_cast<int>(Node.getArgs().size());
  std::vector<std::string> AllSignatures;

  for (const auto *Sym : CurrentFileMatches)
    AllSignatures.push_back(Sym->signature());
  for (const auto &Match : ModuleMatches)
    AllSignatures.push_back(Match.Symbol->signature());

  bool AnyArityMatch = false;
  for (const auto *Sym : CurrentFileMatches) {
    // Vararg functions accept any number of args >= their declared param count
    if (Sym->arity() == ArgCount || (Sym->isVarArg() && ArgCount >= Sym->arity()))
    { AnyArityMatch = true; break; }
  }
  if (!AnyArityMatch) {
    for (const auto &Match : ModuleMatches) {
      if (Match.Symbol->arity() == ArgCount ||
          (Match.Symbol->isVarArg() && ArgCount >= Match.Symbol->arity()))
      { AnyArityMatch = true; break; }
    }
  }

  if (!AnyArityMatch) {
    auto Err = WrongArityError(Node.getLoc(), CalleeName, ArgCount, AllSignatures);
    for (const auto *Sym : CurrentFileMatches)
      Err.withRelated(toSourceLocation(Sym->DeclLoc), "candidate");
    for (const auto &Match : ModuleMatches)
      Err.withRelated(toSourceLocation(Match.Symbol->DeclLoc), "candidate");
    Err.raise();
    return;
  }

  // Populate ResolvedCandidates with only the arity-matching overloads.
  // TypeChecker::visitFuncCall then narrows further by type compatibility.
  for (const auto *Sym : CurrentFileMatches) {
    if (Sym->arity() == ArgCount || (Sym->isVarArg() && ArgCount >= Sym->arity()))
      Node.ResolvedCandidates.push_back(
          std::make_shared<ResolverFunctionSymbol>(*Sym));
  }
  for (const auto &Match : ModuleMatches) {
    if (Match.Symbol->arity() == ArgCount ||
        (Match.Symbol->isVarArg() && ArgCount >= Match.Symbol->arity()))
      Node.ResolvedCandidates.push_back(
          std::make_shared<ResolverFunctionSymbol>(*Match.Symbol));
  }

  // ── Set ResolvedCallTarget when unambiguous ───────────────────────────────
  // If exactly one overload matches the arity, the Resolver can commit to it
  // immediately.  Codegen reads ResolvedCallTarget blindly (Issue #9) — this
  // ensures it's always set for the common single-overload case, even when
  // the TypeChecker hasn't run.
  //
  // Multiple arity-matching candidates means the function has type-overloads
  // (e.g. add(int,int) and add(double,double)).  In that case TypeChecker
  // narrows to one via type compatibility and sets ResolvedCallTarget itself.
  if (Node.ResolvedCandidates.size() == 1) {
    Node.ResolvedCallTarget = Node.ResolvedCandidates[0];
  }
}

void Resolver::visitMethodCall(MethodCallExprAST &Node) {
  // Resolve argument expressions FIRST (mirrors visitFuncCall's own
  // ordering) so a genuinely broken argument (e.g. an undeclared variable)
  // is still reported even when the base itself turns out to be rejected
  // below -- errors accumulate across the whole Resolver pass rather than
  // stopping at the first one, so there's no reason to skip resolving the
  // arguments just because the call as a whole won't be allowed to proceed.
  for (const auto &Arg : Node.getArgs())
    visitChild(Arg.get());

  // IMPORTANT — variable shadowing: a locally-declared variable of the same
  // spelling as a module or struct type ALWAYS takes priority, exactly like
  // ordinary lexical scoping everywhere else in the language. This is not a
  // hypothetical: `io` used to be auto-imported into EVERY Turf program via
  // lib/std/prelude.tr, so a variable literally named `io` used to always be
  // misread as the io module — `mut io: Thing = ...; io.show();` raised
  // "Unknown function 'io.show'" instead of calling Thing's struct method,
  // because the module check ran before any check for an actual declared
  // variable of that name.
  auto *BaseVar = dynamic_cast<VariableExprAST *>(
      const_cast<ExprAST *>(Node.getBase()));
  if (BaseVar) {
    auto ExistingVar = SymTable_.lookupVariable(BaseVar->getName());
    if (ExistingVar) {
      // Base is a declared variable — resolve it directly (equivalent to
      // what visitVariable() would do, without re-running its lookup).
      BaseVar->ResolvedSymbol = ExistingVar;
      return;
    }
    if (SymTable_.hasModule(BaseVar->getName())) {
      // Base is a module qualifier used with '.' -- not allowed. '.' is
      // instance-member-access only; a module-qualified call must use
      // '::' (io::printline(...), not io.printline(...)). This used to
      // silently succeed via VariableExprAST::ResolvedModuleName, a legacy
      // resolution path threaded all the way through TypeChecker/Codegen
      // that made '.' a working, undocumented alias for '::' here --
      // confirmed live before this check existed. That field (and its
      // downstream TypeChecker.cpp/Codegen.cpp readers) are left in place
      // purely as defense-in-depth; they can no longer be reached in a
      // real compile now that this branch always raises and returns
      // instead of ever setting it.
      DotOperatorOnNonInstanceError(Node.getLoc(), BaseVar->getName(),
                                    "module", Node.getMethod())
          .raise();
      return;
    }
    if (SymTable_.isKnownStruct(BaseVar->getName())) {
      // Base is a bare struct TYPE name used with '.' -- same rule as the
      // module case just above: only '::' can invoke a static method
      // (StructName::method(...)), never '.'. This used to silently
      // succeed for any method whose arity happened to match with no
      // `self` (i.e. any `::`-declared static method, or coincidentally
      // any '.'-declared instance method called with one fewer argument
      // than it actually needs) via VariableExprAST::IsStructName, a
      // legacy path originally added only to turn a compiler CRASH into a
      // clean arity error -- never intended to make '.' a real, working
      // way to call a static method, but it accidentally became exactly
      // that once true static methods (fn StructName::method(...), no
      // self at all) were introduced. Confirmed live before this check
      // existed. IsStructName is left in place (TypeChecker.cpp/
      // Codegen.cpp still read it) as defense-in-depth only, same
      // rationale as ResolvedModuleName above.
      DotOperatorOnNonInstanceError(Node.getLoc(), BaseVar->getName(),
                                    "struct type", Node.getMethod())
          .raise();
      return;
    }
    // Base is neither a declared variable, a module, nor a struct name —
    // resolve normally so visitVariable() raises the proper ReferenceError.
    visitChild(Node.getBase());
    return;
  }
  // Base is a complex expression — resolve normally.
  visitChild(Node.getBase());
}

void Resolver::visitFieldAccess(FieldAccessExprAST &Node) {
  visitChild(Node.getBase());
}

void Resolver::visitEnumDecl(EnumDeclExprAST &Node) {
  // Pass 1 already hoisted the enum to the registry.
}

// Handles a `Left::Right` node reached as an ordinary expression -- i.e.
// NOT as the direct callee of a FuncCallExprAST (that case is handled
// specially and more narrowly by visitFuncCall itself, mirroring how a bare
// VariableExprAST callee is special-cased there too, so this function never
// even sees it). What's left for this function to disambiguate:
//   - EnumName::Variant used as a value (e.g. `mut c: Color = Color::Red;`)
//     -- the original, only-ever-supported meaning of this node.
//   - Module::something referenced but NOT called -- this language has no
//     first-class functions, so there is no valid non-call meaning of a
//     module-qualified name; reported as a clean error instead of reaching
//     Codegen and crashing (FuncCallExprAST::codegen() already has a
//     "First-class functions are not supported yet" internal error for
//     exactly this shape once it gets that far un-rejected).
void Resolver::visitScopeAccess(ScopeAccessExprAST &Node) {
  const std::string &LeftName = Node.getLeftName();
  const std::string &RightName = Node.getRightName();

  if (SymTable_.isEnumName(LeftName)) {
    Node.ResolvedKind = ScopeAccessKind::EnumVariant;
    // Module::EnumName::Variant -- the parser already reinterpreted the
    // module-qualified enum name into ModuleQualifier/LeftName by the time
    // this runs; validate it the same way a qualified type/struct-literal
    // is (Resolver::validateModuleQualifier()).
    if (!validateModuleQualifier(Node.getModuleQualifier(), LeftName, Node.getLoc()))
      return;
    if (RightName != "COUNT" && RightName != "VALUES" && RightName != "NAMES") {
      auto ET = SymTable_.lookupEnumType(LeftName);
      if (ET && !ET->hasVariant(RightName)) {
        std::string Msg = "Enum '" + LeftName + "' has no variant '" + RightName + "'.";
        auto Sugg = SuggestionIndex::suggestVisible(RightName, ET->getVariants());
        if (!Sugg.empty()) {
          Msg += "\nDid you mean:\n";
          for (const auto& s : Sugg) Msg += "    " + s + "\n";
        }
        LogErrorAt(Node.getLoc(), Msg);
      }
    }
    return;
  }

  if (SymTable_.hasModule(LeftName)) {
    // `module::CONST` -- a qualified reference to an `export immut`
    // constant. Checked BEFORE the "functions aren't first-class" fallback
    // just below, since a constant genuinely IS usable as a value (unlike a
    // bare `module::function`).
    if (const auto *Const = SymTable_.findModuleConstant(LeftName, RightName)) {
      Node.ResolvedKind = ScopeAccessKind::ModuleConstantRef;
      Node.ResolvedConstant = Const;
      return;
    }
    // A module name IS real, but referencing `module::function` without
    // calling it has no meaning in this language yet.
    TypeError(Node.getLoc(), "'" + LeftName + "::" + RightName +
                                 "' can't be used as a value -- functions "
                                 "aren't first-class yet, only calls like '" +
                                 LeftName + "::" + RightName + "(...)' work.")
        .raise();
    return;
  }

  if (SymTable_.isKnownStruct(LeftName)) {
    // A struct name IS real, but referencing `StructName::method` without
    // calling it has no meaning either -- same "no first-class functions"
    // rule as the module case just above, worded for the static-method form.
    Node.ResolvedKind = ScopeAccessKind::StaticMethodRef;
    TypeError(Node.getLoc(), "'" + LeftName + "::" + RightName +
                                 "' can't be used as a value -- functions "
                                 "aren't first-class yet, only calls like '" +
                                 LeftName + "::" + RightName + "(...)' work.")
        .raise();
    return;
  }

  // Same "you meant '.'" specific-error upgrade as visitFuncCall's
  // equivalent fallback -- see its comment for why this is worth
  // distinguishing from the generic "unknown qualifier" case below.
  if (SymTable_.lookupVariable(LeftName)) {
    ScopeOperatorOnInstanceError(Node.getLoc(), LeftName, RightName).raise();
    return;
  }

  RankedSuggestions Suggestions;
  auto EnumSugg = SuggestionIndex::suggestVisible(LeftName, SymTable_.getAllEnumNames());
  for (const auto &S : EnumSugg)
    Suggestions.Local.push_back(S + "::" + RightName);
  UnknownScopeQualifierError(Node.getLoc(), LeftName, Suggestions).raise();
}

/**
 * @brief Validate that @p Ty (once pointer- and array-unwrapped) either
 *        names a known primitive or refers to a struct/enum registered in
 *        Pass 1 hoisting.
 *
 * Before this helper existed, this exact validation lived ONLY inline inside
 * visitStructDecl(), checking struct field types alone. That left a real gap:
 * an unknown/misspelled struct name in a function parameter or return type
 * compiled with ZERO diagnostics anywhere in the compiler --
 * `fn f(x: NoSuchStruct): void {}` was confirmed to compile silently and
 * produce a bogus empty opaque LLVM struct type for "NoSuchStruct". This
 * helper is now shared by visitStructDecl, visitFuncDef, and
 * visitExternFuncDecl so every type-reference position gets the same
 * validation and the same Damerau-Levenshtein "did you mean?" suggestions.
 *
 * Unwrapping: one level of pointer (e.g. `Node*` -> check "Node", matching
 * how `self` and other struct-pointer parameters are spelled) and any number
 * of array dimensions (e.g. `User[5]` -> check "User"; the original
 * struct-field-only version did not handle arrays at all).
 *
 * Type-level validation beyond "does this name exist" (field/parameter type
 * correctness, initializer compatibility, etc.) remains the TypeChecker's
 * responsibility -- the Resolver only checks names, consistent with its role
 * as the naming/scoping pass that runs before type inference.
 *
 * @param Ty         The parsed type to validate. No-op if null.
 * @param Loc        Source location to attach to any raised diagnostic.
 * @param TypeParams Names declared by the enclosing `fn<...>`/`struct<...>`
 *                   that are exempt from the "known struct/enum" check --
 *                   see the header doc comment for why this is necessary
 *                   (ParseType() always emits a plain NamedType for a bare
 *                   identifier, even when that identifier is a generic type
 *                   parameter rather than a struct name).
 */
void Resolver::validateNamedType(
    const TurfTypePtr &Ty, SourceLocation Loc,
    const std::vector<std::string> &TypeParams) const {
  if (!Ty)
    return;

  // The set of names that are always valid without being a declared struct.
  static const std::unordered_set<std::string> KnownPrimitives = {
      "int", "double", "bool", "string", "void", "null", "ptr", "void*"};

  const TurfType *BaseType = Ty.get();

  // Unwrap one level of pointer (e.g. Node* -> check "Node").
  if (const auto *PT = dynamic_cast<const TurfPointerType *>(BaseType))
    BaseType = PT->getPointee().get();

  // Unwrap any number of array dimensions (e.g. User[5][2] -> check "User").
  while (isArrayType(BaseType))
    BaseType = getArrayElementType(BaseType);

  // Recurse into a generic type's own type arguments (e.g. List<NoSuchType>)
  // -- this function never used to look inside GenericType at all, so an
  // unknown type argument slipped through validation entirely and only
  // surfaced later as a confusing, misattributed error far from the real
  // cause (e.g. "No overload of 'List.push' matches argument types (int)",
  // blaming the call's argument instead of the undeclared type argument).
  if (const auto *GT = dynamic_cast<const GenericType *>(BaseType)) {
    // The generic's OWN base name (e.g. "List" in List<int>) was never
    // validated here either, only its type arguments -- confirmed live:
    // `mut x: NonExistentGeneric<int> = 5;` produced an unrelated type-
    // mismatch error ("Cannot initialise... with value of type 'int'"),
    // never "unknown type", because nothing checked BaseName existed at
    // all. Found and fixed alongside the module-qualifier validation below
    // since both need the exact same isKnownStruct/isEnumName groundwork.
    const std::string &BaseName = GT->getBaseName();
    bool BaseIsOwnTypeParam =
        std::find(TypeParams.begin(), TypeParams.end(), BaseName) != TypeParams.end();
    if (!BaseIsOwnTypeParam &&
        !SymTable_.isKnownStruct(BaseName) && !SymTable_.isEnumName(BaseName)) {
      RankedSuggestions Suggestions;
      Suggestions.Local =
          SuggestionIndex::suggestVisible(BaseName, SymTable_.getAllStructNames());
      UnknownTypeError(Loc, BaseName, Suggestions).raise();
    } else if (!GT->getModule().empty()) {
      validateModuleQualifier(GT->getModule(), BaseName, Loc);
    } else if (!BaseIsOwnTypeParam &&
               !SymTable_.isTypeVisibleUnqualified(currentModuleName(), BaseName)) {
      UnqualifiedTypeNotVisibleError(Loc, BaseName,
                                     SymTable_.getTypeOwningModule(BaseName))
          .raise();
    }
    for (const auto &Arg : GT->getTypeArgs())
      validateNamedType(Arg, Loc, TypeParams);
    return;
  }

  // Only validate NamedType references — primitives have their own classes
  // (IntType, DoubleType, etc.) and never reach here as NamedType. A true
  // TurfGenericParamType instance is a different class entirely and is
  // correctly never matched here either -- but ParseType() never actually
  // produces one for a type ANNOTATION (only GlobalSymbolTable's separate,
  // Codegen-facing registration does), so the caller-supplied TypeParams
  // list below is what actually exempts `T` in `fn<T> f(x: T)`.
  //
  // isKnownStruct()/isEnumName() below check the GLOBAL, module-blind
  // struct/enum EXISTENCE registry (does this name exist ANYWHERE) --
  // that's a separate question from whether it's VISIBLE UNQUALIFIED from
  // the current file/module, which isTypeVisibleUnqualified() below
  // answers (own declaration, or an on-demand `import Module::Type;` /
  // `Module::[Type,...]` / `Module::[*]`). A plain `import Module;` alone
  // grants qualified access (`Module::Type`) but not bare access, exactly
  // mirroring how bare function-call visibility already worked before this
  // gate existed for types too.
  if (const auto *NT = dynamic_cast<const NamedType *>(BaseType)) {
    const std::string &TypeName = NT->getName();
    bool IsOwnTypeParam =
        std::find(TypeParams.begin(), TypeParams.end(), TypeName) !=
        TypeParams.end();
    if (!IsOwnTypeParam && !KnownPrimitives.count(TypeName) &&
        !SymTable_.isKnownStruct(TypeName) &&
        !SymTable_.isEnumName(TypeName)) {
      RankedSuggestions Suggestions;
      Suggestions.Local =
          SuggestionIndex::suggestVisible(TypeName, SymTable_.getAllStructNames());
      UnknownTypeError(Loc, TypeName, Suggestions).raise();
    } else if (!IsOwnTypeParam && !KnownPrimitives.count(TypeName)) {
      if (!NT->getModule().empty()) {
        validateModuleQualifier(NT->getModule(), TypeName, Loc);
      } else if (!SymTable_.isTypeVisibleUnqualified(currentModuleName(), TypeName)) {
        UnqualifiedTypeNotVisibleError(Loc, TypeName,
                                       SymTable_.getTypeOwningModule(TypeName))
            .raise();
      }
    }
  }
}

// See the doc comment on the declaration in Resolver.h.
bool Resolver::validateModuleQualifier(const std::string &Qualifier,
                                       const std::string &TypeName,
                                       SourceLocation Loc) const {
  if (Qualifier.empty())
    return true;

  if (!SymTable_.hasModule(Qualifier)) {
    RankedSuggestions Suggestions;
    auto EnumSugg = SuggestionIndex::suggestVisible(Qualifier, SymTable_.getAllEnumNames());
    for (const auto &S : EnumSugg)
      Suggestions.Local.push_back(S + "::" + TypeName);
    UnknownScopeQualifierError(Loc, Qualifier, Suggestions).raise();
    return false;
  }

  std::string Owner = SymTable_.getTypeOwningModule(TypeName);
  if (Owner != Qualifier) {
    RankedSuggestions Suggestions;
    if (!Owner.empty())
      Suggestions.Module[Owner].push_back(TypeName);
    UnknownModuleSymbolError(Loc, Qualifier, TypeName, Suggestions).raise();
    return false;
  }

  return true;
}

void Resolver::visitStructDecl(StructDeclExprAST &Node) {
  // Pass 1 (hoistTopLevel) already registered this struct's name -- but ONLY
  // for a struct declared at the TOP LEVEL of a file or an imported module's
  // body. hoistTopLevel() never recurses into a FUNCTION BODY looking for
  // nested declarations, so a struct declared LOCALLY inside a function
  // (`fn f() { struct Point {...}; mut p: Point = ...; }` -- a supported,
  // already-tested pattern, see tests/06_structs/02_struct_local.tr) is
  // never hoisted at all. Pass 2 visits statements in textual order, so by
  // the time THIS node is visited, it's still safe to register it here for
  // the first time -- any later statement in the same or a nested scope
  // that references the name will see it correctly. Guarded on
  // isKnownStruct() so a top-level struct (already registered by Pass 1)
  // is never re-registered here, which would otherwise raise a spurious
  // DuplicateTypeNameError against itself on every single struct in the
  // file.
  if (!SymTable_.isKnownStruct(Node.getName())) {
    SourceLocation Loc = Node.getLoc();
    SymTable_.registerStruct(Node.getName(), {Loc.Line, Loc.Col}, currentModuleName());
  }

  // Rewrite any field NamedType("T") that names one of this struct's own
  // TypeParams into a real TurfGenericParamType("T") -- same rationale and
  // same mutate-the-shared-AST-node-in-place approach as visitFuncDef(); see
  // substituteTypeParams()'s doc comment. Most generic struct fields in
  // practice are pointers (e.g. `data: T*` in lib/std/list.tr), which were
  // already unaffected by this gap because TurfPointerType lowers to an
  // opaque pointer regardless of what it points to -- but a bare `val: T`
  // field would hit the same "bogus opaque struct named T" bug fixed here.
  const auto &TypeParams = Node.getTypeParams();
  if (!TypeParams.empty()) {
    for (auto &Field : Node.getFieldsMut())
      Field.Type = substituteTypeParams(Field.Type, TypeParams);
  }

  // In Pass 2 (here) we validate every field's type via validateNamedType().
  for (const auto &Field : Node.getFields())
    validateNamedType(Field.Type, Node.getLoc(), Node.getTypeParams());
}

void Resolver::visitStructInit(StructInitExprAST &Node) {
  // Validates an explicit `Module::StructName { ... }` qualifier, if any
  // (a no-op for the ordinary bare `StructName { ... }` form). Note this
  // function does NOT otherwise validate that Node.getName() is a real,
  // known struct at all -- that pre-existing gap (a totally nonexistent
  // struct name in a BARE construction currently falls through to an
  // unrelated error, or worse, uncaught) is separate from the module-
  // qualifier work and intentionally not addressed here; the qualifier
  // check below, however, naturally still produces a correct, specific
  // error even when the struct name itself doesn't exist anywhere (see
  // validateModuleQualifier(): an unknown TypeName has no owning module,
  // so it never equals a real Qualifier, correctly raising
  // UnknownModuleSymbolError either way).
  if (!Node.getModuleQualifier().empty()) {
    validateModuleQualifier(Node.getModuleQualifier(), Node.getName(), Node.getLoc());
  } else if (SymTable_.isKnownStruct(Node.getName()) &&
             !SymTable_.isTypeVisibleUnqualified(currentModuleName(), Node.getName())) {
    // Bare construction of a REAL struct that just isn't bare-visible from
    // here (declared in another module, never on-demand imported) -- see
    // isTypeVisibleUnqualified()'s doc comment. A totally unknown struct
    // name still falls through untouched, per the pre-existing gap noted
    // above.
    UnqualifiedTypeNotVisibleError(Node.getLoc(), Node.getName(),
                                   SymTable_.getTypeOwningModule(Node.getName()))
        .raise();
  }

  for (const auto &[FieldName, Val] : Node.getNamedFields())
    visitChild(Val.get());
}

void Resolver::visitArrayDecl(ArrayDeclExprAST &Node) {
  // Resolve the initializer list expressions.
  for (const auto &Init : Node.getInitList())
    visitChild(Init.get());
  if (Node.getInitExpr())
    visitChild(Node.getInitExpr().get());

  // Register the array variable in the current scope.
  // Capture the returned symbol and attach it to the AST node (Issue #5).
  try {
    ResolverSourceLocation Loc{Node.getLoc().Line, Node.getLoc().Col};
    Node.ResolvedSymbol =
        SymTable_.declareVariable(Node.getName(), makeArrayType(Node.getElementTypePtr(), Node.getSize()), Loc, Node.isImmutable());
  } catch (const std::runtime_error &) {
    auto Prev = SymTable_.lookupVariable(Node.getName());
    if (Prev) {
      SourceLocation PrevLoc{Prev->DeclLoc.Line, Prev->DeclLoc.Col};
      DuplicateDeclarationError(Node.getLoc(), Node.getName(), PrevLoc).raise();
    } else {
      SyntaxError(Node.getLoc(), "Duplicate declaration of '" + Node.getName() + "'").raise();
    }
  }
}

void Resolver::visitArrayLiteral(ArrayLiteralExprAST &Node) {
  for (const auto &Elem : Node.getElements()) {
    visitChild(Elem.get());
  }
}
void Resolver::visitArrayAccess(ArrayAccessExprAST &Node) {
  visitChild(Node.getBase());
  visitChild(Node.getIndex());
}

void Resolver::visitModuleImport(ModuleImportExprAST &Node) {
  ModuleStack_.push_back(Node.getModuleName());
  visitChild(Node.getBody());
  ModuleStack_.pop_back();
}

// ExternFuncDeclExprAST: hoistTopLevel already registered the symbol in Pass 1.
// The only work left for Pass 2 is validating that the parameter and return
// type names are real -- see validateNamedType()'s doc comment. This used to
// be a complete no-op, meaning an extern fn parameter or return type
// referencing an unknown struct name was never checked anywhere in the
// compiler; ordinary `fn` definitions had the same gap until visitFuncDef()
// was updated alongside this.
void Resolver::visitExternFuncDecl(ExternFuncDeclExprAST &Node) {
  for (const auto &[ParamName, ParamType] : Node.getParams())
    validateNamedType(ParamType, Node.getLoc());
  validateNamedType(Node.getReturnType(), Node.getLoc());
}
