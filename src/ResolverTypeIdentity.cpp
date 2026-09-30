/**
 * @file ResolverTypeIdentity.cpp
 * @brief FDN-003: module-scoped type identity -- turning a type's SPELLING
 *        into its TypeIdentity (Types.h), in Pass 1 for declarations and on
 *        demand for `Left::Right` expressions.
 * @details Part of the Resolver name-resolution pass. See
 *          docs/development/changes/FDN-003.md and
 *          docs/architecture/modules_and_scope_resolution.md §2.6.
 *
 * @author a2ys
 * @copyright Copyright (c) 2026 TurfLang contributors.  Apache License.
 */

#include "Resolver.h"

#include "AST.h"
#include "Errors.h"
#include "ResolverInternal.h"
#include "Types.h"

#include <algorithm>
#include <unordered_set>

namespace {

// Spellings that are never a user type, whatever module they appear in.
// Mirrors validateNamedType()'s own KnownPrimitives set (ResolverDecl.cpp).
bool isPrimitiveSpelling(const std::string &Name) {
  static const std::unordered_set<std::string> Primitives = {
      "int", "int32", "double", "bool", "char", "string",
      "void", "null", "ptr", "void*"};
  return Primitives.count(Name) > 0;
}

bool isTypeParam(const std::string &Name, const std::vector<std::string> &TypeParams) {
  return std::find(TypeParams.begin(), TypeParams.end(), Name) != TypeParams.end();
}

} // namespace

// ─── Pass 1a: every type declaration, before anything else ──────────────────

void Resolver::hoistTypes(const std::vector<std::unique_ptr<ExprAST>> &Nodes) {
  for (const auto &Node : Nodes)
    hoistTypesIn(Node.get(), "");
}

void Resolver::hoistTypesIn(const ExprAST *Node, const std::string &ModuleName) {
  if (!Node)
    return;

  // Every declaration's getName() is already its TypeIdentity -- the Parser
  // named it from the module it was parsing (ParseStructDecl() and friends,
  // CurrentParseModule()). A duplicate here is therefore always two
  // declarations of the same name in the SAME module: two modules declaring
  // `Node` register two different identities and never meet.
  if (const auto *Struct = dynamic_cast<const StructDeclExprAST *>(Node)) {
    SourceLocation Loc = Struct->getLoc();
    if (auto PrevLoc = SymTable_.registerStruct(
            Struct->getName(), {Loc.Line, Loc.Col}, ModuleName)) {
      DuplicateTypeNameError(Loc, Struct->getName(), "struct",
                             {PrevLoc->Line, PrevLoc->Col})
          .raise();
    }
    SymTable_.markTypeVisibility(Struct->getName(),
                                 {Struct->isPublic(), {Loc.Line, Loc.Col}, "struct"});
    // Keep the AST node itself, not just its name -- checkStructLayoutAcyclicity()
    // needs the actual field types, which ResolverTypeRegistry never stores.
    HoistedStructDecls_.push_back(Struct);
    if (!Struct->getTypeParams().empty())
      GenericStructIdentities_[Struct->getName()] = Struct->getTypeParams();
    return;
  }

  if (const auto *Enum = dynamic_cast<const EnumDeclExprAST *>(Node)) {
    auto ET = std::make_shared<TurfEnumType>(Enum->getName(), Enum->getVariants(),
                                             Enum->getUnderlyingType());
    SourceLocation Loc = Enum->getLoc();
    if (auto PrevLoc = SymTable_.registerEnumType(
            Enum->getName(), {Loc.Line, Loc.Col}, ET, ModuleName)) {
      DuplicateTypeNameError(Loc, Enum->getName(), "enum",
                             {PrevLoc->Line, PrevLoc->Col})
          .raise();
    }
    SymTable_.markTypeVisibility(Enum->getName(),
                                 {Enum->isPublic(), {Loc.Line, Loc.Col}, "enum"});
    return;
  }

  // A `machine` registers through the SAME registerEnumType() enums use (a
  // TurfMachineType IS-A TurfEnumType, see its doc comment in Types.h) --
  // that one decision is what lets MachineName::State access, `compare`/
  // `matches` exhaustiveness, and Codegen's constant-inlining all work on a
  // machine subject with no further code in those paths.
  if (const auto *Machine = dynamic_cast<const MachineDeclExprAST *>(Node)) {
    std::map<std::string, std::set<std::string>> Edges;
    for (const auto &E : Machine->getEdges())
      Edges[E.From].insert(E.To);
    auto MT = std::make_shared<TurfMachineType>(Machine->getName(),
                                                Machine->getStates(), Edges);
    SourceLocation Loc = Machine->getLoc();
    if (auto PrevLoc = SymTable_.registerEnumType(
            Machine->getName(), {Loc.Line, Loc.Col}, MT, ModuleName)) {
      DuplicateTypeNameError(Loc, Machine->getName(), "state machine",
                             {PrevLoc->Line, PrevLoc->Col})
          .raise();
    }
    SymTable_.markTypeVisibility(Machine->getName(),
                                 {Machine->isPublic(), {Loc.Line, Loc.Col}, "machine"});
    return;
  }

  if (const auto *ErrDecl = dynamic_cast<const ErrorDeclExprAST *>(Node)) {
    auto ET = std::make_shared<TurfUserErrorType>(
        ErrDecl->getName(), ErrDecl->getVariantInfos(), ErrDecl->isBareForm());
    SourceLocation Loc = ErrDecl->getLoc();
    if (auto PrevLoc = SymTable_.registerErrorType(
            ErrDecl->getName(), {Loc.Line, Loc.Col}, ET, ModuleName)) {
      DuplicateTypeNameError(Loc, ErrDecl->getName(), "error",
                             {PrevLoc->Line, PrevLoc->Col})
          .raise();
    }
    SymTable_.markTypeVisibility(ErrDecl->getName(),
                                 {ErrDecl->isPublic(), {Loc.Line, Loc.Col}, "error"});
    return;
  }

  if (const auto *Import = dynamic_cast<const ModuleImportExprAST *>(Node)) {
    const std::string &Inner = Import->getModuleName();
    // Registered here, not only in hoistModuleBody(): Pass 1b resolves
    // qualified spellings (`list::List`) before hoistModuleBody() runs, and
    // a qualifier is only valid if its module is known.
    SymTable_.registerModule(Inner);
    if (const auto *Block = dynamic_cast<const BlockExprAST *>(Import->getBody()))
      for (const auto &Expr : Block->getExpressions())
        hoistTypesIn(Expr.get(), Inner);
    // ContextLabel is the module whose file WROTE this import -- the same
    // rule registerOnDemandImports() follows.
    registerOnDemandTypeImports(*Import, ModuleName);
    return;
  }
}

void Resolver::registerLocalTypeDecl(const ExprAST &Node, SourceLocation Loc) {
  // Same registration hoistTypesIn() does, but a name already taken is a
  // duplicate only if it was taken by a DIFFERENT declaration: the same
  // node is legitimately seen more than once (a top-level declaration
  // hoisted by Pass 1 and visited again by Pass 2; a declaration inside a
  // generic function's body, visited once per cloned instantiation) and
  // always reports its own location -- the rule visitStructDecl() uses too.
  const std::string Module = currentModuleName();
  std::optional<ResolverSourceLocation> Prev;
  std::string Name, Kind;
  if (const auto *Enum = dynamic_cast<const EnumDeclExprAST *>(&Node)) {
    Name = Enum->getName();
    Kind = "enum";
    Prev = SymTable_.registerEnumType(
        Name, {Loc.Line, Loc.Col},
        std::make_shared<TurfEnumType>(Name, Enum->getVariants(), Enum->getUnderlyingType()),
        Module);
  } else if (const auto *Machine = dynamic_cast<const MachineDeclExprAST *>(&Node)) {
    std::map<std::string, std::set<std::string>> Edges;
    for (const auto &E : Machine->getEdges())
      Edges[E.From].insert(E.To);
    Name = Machine->getName();
    Kind = "state machine";
    Prev = SymTable_.registerEnumType(
        Name, {Loc.Line, Loc.Col},
        std::make_shared<TurfMachineType>(Name, Machine->getStates(), Edges), Module);
  } else if (const auto *ErrDecl = dynamic_cast<const ErrorDeclExprAST *>(&Node)) {
    Name = ErrDecl->getName();
    Kind = "error";
    Prev = SymTable_.registerErrorType(
        Name, {Loc.Line, Loc.Col},
        std::make_shared<TurfUserErrorType>(Name, ErrDecl->getVariantInfos(),
                                            ErrDecl->isBareForm()),
        Module);
  } else {
    return;
  }
  if (Prev && (Prev->Line != Loc.Line || Prev->Col != Loc.Col))
    DuplicateTypeNameError(Loc, Name, Kind, {Prev->Line, Prev->Col}).raise();
}

void Resolver::registerOnDemandTypeImports(const ModuleImportExprAST &Import,
                                           const std::string &ContextLabel) {
  if (Import.getMode() == ImportMode::Bare)
    return;
  const std::string &SourceModule = Import.getModuleName();

  auto Register = [&](const std::string &BareName, SourceLocation Loc) {
    const std::string Id = TypeIdentity::make(SourceModule, BareName);
    if (!SymTable_.isKnownTypeIdentity(Id))
      return; // not a type of SourceModule -- registerOnDemandImports() decides
              // whether it is something else or an unknown symbol.
    // LEC-002: a private type is never flat-imported. A `[*]` import just
    // skips it; an explicit `import m::Hidden;` is reported by
    // registerOnDemandImports(), which sees every kind of name at once.
    if (!SymTable_.isTypeVisibleFrom(Id, ContextLabel))
      return;
    ResolverSymbolTable::FlatImportedSymbol Sym{SourceModule, {Loc.Line, Loc.Col}};
    if (auto Prev = SymTable_.registerFlatTypeImport(ContextLabel, BareName, Sym)) {
      if (Prev->SourceModule != SourceModule) {
        ImportSymbolCollisionError(Loc, BareName, Prev->SourceModule, SourceModule,
                                   toSourceLocation(Prev->ImportLoc))
            .raise();
      }
    }
  };

  if (Import.getMode() == ImportMode::Wildcard) {
    for (const auto &Name : SymTable_.getModuleTypeNames(SourceModule))
      Register(Name, Import.getLoc());
    return;
  }
  for (const auto &Ref : Import.getRequestedSymbols())
    Register(Ref.Name, Ref.Loc);
}

// ─── Pass 1b: declaration-level spellings ───────────────────────────────────

void Resolver::resolveDeclarationSpellings(
    const std::vector<std::unique_ptr<ExprAST>> &Nodes) {
  for (const auto &Node : Nodes)
    resolveDeclarationSpellingsIn(Node.get(), "");
}

void Resolver::resolveDeclarationSpellingsIn(ExprAST *Node, const std::string &Module) {
  if (!Node)
    return;

  if (auto *Struct = dynamic_cast<StructDeclExprAST *>(Node)) {
    for (auto &Field : Struct->getFieldsMut())
      resolveTypeSpellingsSilently(Field.Type, Module, Struct->getTypeParams());
    return;
  }

  // LEC-004: a payload field's type is a spelling like a struct field's. The
  // registered TurfUserErrorTypes share these very TurfType objects, so the
  // rewrite is seen by every one of them.
  if (auto *Err = dynamic_cast<ErrorDeclExprAST *>(Node)) {
    for (auto &Variant : Err->getVariantInfosMut())
      for (auto &Field : Variant.Fields)
        resolveTypeSpellingsSilently(Field.Type, Module, {});
    return;
  }

  if (auto *Func = dynamic_cast<FuncDefExprAST *>(Node)) {
    const auto &TypeParams = Func->getTypeParams();
    // A method's receiver: `fn Recv.method(...)` is registered, looked up
    // and mangled as "<Recv's identity>.method" -- the same string
    // TypeChecker/Codegen build from the receiver VALUE's type, which is
    // always an identity. The receiver may be another module's struct
    // (`fn Pt.twice()` on an imported `Pt` is legal), so this is a real
    // visibility lookup, not "prefix the current module".
    const std::string &FullName = Func->getName();
    size_t Dot = FullName.find('.');
    if (Dot != std::string::npos) {
      std::string Receiver = FullName.substr(0, Dot);
      if (!TypeIdentity::isModuleQualified(Receiver)) {
        if (auto Id = tryResolveTypeSpelling("", Receiver, Module))
          Func->setName(*Id + FullName.substr(Dot));
      }
    }
    for (auto &P : Func->getParamsMut())
      resolveTypeSpellingsSilently(P.Type, Module, TypeParams);
    resolveTypeSpellingsSilently(Func->getReturnTypePtr(), Module, TypeParams);
    return;
  }

  if (auto *Extern = dynamic_cast<ExternFuncDeclExprAST *>(Node)) {
    for (const auto &Param : Extern->getParams())
      resolveTypeSpellingsSilently(Param.second, Module, {});
    resolveTypeSpellingsSilently(Extern->getReturnType(), Module, {});
    return;
  }

  if (auto *Import = dynamic_cast<ModuleImportExprAST *>(Node)) {
    if (auto *Block = dynamic_cast<BlockExprAST *>(const_cast<ExprAST *>(Import->getBody())))
      for (const auto &Expr : Block->getExpressions())
        resolveDeclarationSpellingsIn(Expr.get(), Import->getModuleName());
    return;
  }
}

// ─── Spelling -> identity ───────────────────────────────────────────────────

std::optional<std::string>
Resolver::tryResolveTypeSpelling(const std::string &Qualifier, const std::string &Name,
                                 const std::string &Context) const {
  if (TypeIdentity::isModuleQualified(Name))
    return SymTable_.isKnownTypeIdentity(Name) ? std::optional<std::string>(Name)
                                               : std::nullopt;
  if (!Qualifier.empty()) {
    if (!SymTable_.hasModule(Qualifier))
      return std::nullopt;
    std::string Id = TypeIdentity::make(Qualifier, Name);
    // LEC-002: a private type stays UNRESOLVED here, silently, so Pass 2's
    // validateNamedType() sees an unresolved spelling and reports it (with
    // the location and hints). Resolving it here would mark the spelling
    // resolved and Pass 2 would never look at it again.
    return SymTable_.isKnownTypeIdentity(Id) &&
                   SymTable_.isTypeVisibleFrom(Id, Context)
               ? std::optional<std::string>(Id)
               : std::nullopt;
  }
  // Resolving a monomorphized clone: an unresolved bare name can only be a
  // type argument substituted in from the CALL SITE's module, never
  // something the template wrote (that was resolved with the template).
  // Reading it in the template's module could pick that module's own,
  // unrelated same-named type -- see validateNamedType()'s identical rule.
  if (SuppressUnqualifiedVisibilityCheck_)
    return std::nullopt;
  auto Candidates = SymTable_.unqualifiedTypeCandidates(Context, Name);
  if (Candidates.size() == 1)
    return Candidates.front();
  return std::nullopt;
}

void Resolver::resolveTypeSpellingsSilently(const TurfTypePtr &Ty,
                                            const std::string &Context,
                                            const std::vector<std::string> &TypeParams) const {
  if (!Ty)
    return;
  TurfType *T = Ty.get();

  if (auto *NT = dynamic_cast<NamedType *>(T)) {
    if (NT->isResolvedIdentity() || isPrimitiveSpelling(NT->getName()) ||
        (NT->getModule().empty() && isTypeParam(NT->getName(), TypeParams)))
      return;
    if (auto Id = tryResolveTypeSpelling(NT->getModule(), NT->getName(), Context))
      NT->resolveTo(*Id);
    return;
  }
  if (auto *GT = dynamic_cast<GenericType *>(T)) {
    if (!GT->isBaseResolved() &&
        !(GT->getModule().empty() && isTypeParam(GT->getBaseName(), TypeParams))) {
      if (auto Id = tryResolveTypeSpelling(GT->getModule(), GT->getBaseName(), Context))
        GT->resolveBaseTo(*Id);
    }
    for (const auto &Arg : GT->getTypeArgs())
      resolveTypeSpellingsSilently(Arg, Context, TypeParams);
    return;
  }
  // TurfReferenceType and TurfNullableReferenceType are TurfPointerType
  // subclasses (Types.h), so this one branch covers `T*`, `&T` and `&?T`.
  if (auto *PT = dynamic_cast<TurfPointerType *>(T)) {
    resolveTypeSpellingsSilently(PT->getPointee(), Context, TypeParams);
    return;
  }
  if (auto *AT = dynamic_cast<ArrayType *>(T)) {
    resolveTypeSpellingsSilently(AT->getElementTypePtr(), Context, TypeParams);
    return;
  }
  if (auto *EU = dynamic_cast<TurfErrorUnionType *>(T)) {
    resolveTypeSpellingsSilently(EU->getSuccessTypePtr(), Context, TypeParams);
    return;
  }
  if (auto *FT = dynamic_cast<FuncType *>(T)) {
    for (const auto &P : FT->getParamTypes())
      resolveTypeSpellingsSilently(P, Context, TypeParams);
    resolveTypeSpellingsSilently(FT->getReturnType(), Context, TypeParams);
    return;
  }
}

std::string Resolver::resolveScopeLeftType(ScopeAccessExprAST &Node, SourceLocation Loc) {
  if (Node.isLeftTypeIdentity())
    return Node.getLeftName();

  const std::string Left = Node.getLeftName();
  const std::string &Qualifier = Node.getModuleQualifier();

  if (TypeIdentity::isModuleQualified(Left)) {
    if (!SymTable_.isKnownTypeIdentity(Left))
      return "";
    Node.resolveLeftTypeTo(Left);
    return Left;
  }

  if (!Qualifier.empty()) {
    std::string Id = TypeIdentity::make(Qualifier, Left);
    if (SymTable_.isKnownTypeIdentity(Id)) {
      // LEC-002: `m::Hidden::default()` / `m::Hidden::Variant` on a type m
      // did not mark `pub`.
      if (!SymTable_.isTypeVisibleFrom(Id, currentModuleName())) {
        raisePrivateType(Loc, Id);
        return "";
      }
      Node.resolveLeftTypeTo(Id);
      return Id;
    }
    // `Q::Left::Right` where Left is a type somewhere, just not in Q: report
    // it the way a wrongly-qualified type annotation is reported (unknown
    // module, or "module Q has nothing named Left"), rather than letting the
    // caller fall through to "Left isn't a known enum or module".
    if (!SymTable_.identitiesWithBareName(Left).empty())
      validateModuleQualifier(Qualifier, Left, Loc);
    return "";
  }

  auto Candidates = SymTable_.unqualifiedTypeCandidates(currentModuleName(), Left);
  if (Candidates.size() > 1) {
    AmbiguousTypeReferenceError(Loc, Left, Candidates).raise();
    return "";
  }
  if (Candidates.empty())
    Candidates = SymTable_.identitiesWithBareName(Left);
  if (Candidates.size() > 1) {
    AmbiguousTypeReferenceError(Loc, Left, Candidates).raise();
    return "";
  }
  if (Candidates.empty())
    return "";
  Node.resolveLeftTypeTo(Candidates.front());
  return Candidates.front();
}
