#ifndef AST_DECL_H
#define AST_DECL_H

#include "AST.h"


// Which of the four `import` forms this statement used. Bare is the
// original, only-ever-supported form (`import io;`) -- qualified-access
// only, nothing pasted flat. The other three are on-demand: they each pull
// SPECIFIC symbols into the importing file's own flat (unqualified)
// namespace, instead of the old blanket "importing a module makes
// everything in it bare-visible" behavior.
enum class ImportMode {
  Bare,       // import io;
  Symbol,     // import io::printline;
  SymbolList, // import io::[print, input, printline];
  Wildcard,   // import io::[*];
};

// One requested name in a Symbol/SymbolList/Wildcard import. Carries its
// own SourceLocation (not just the enclosing import statement's) so
// collision diagnostics can point at exactly which name in a list caused
// the problem, and so a future LSP can offer go-to-definition on the
// individual symbol name, not just the import line as a whole.
struct ImportedSymbolRef {
  std::string Name;
  SourceLocation Loc;
};

class ModuleImportExprAST : public ExprAST {
  SourceLocation Loc;         ///< location of the `import` statement itself
  std::string ModuleName;
  // Location of the module NAME token specifically (distinct from Loc, the
  // `import` keyword's own position) -- see issue #230. Defaults equal to
  // Loc for prelude imports (ParsePreludeImports()), which are synthesized
  // with no real second source position to point at.
  SourceLocation NameLoc;
  std::string ResolvedPath;   ///< canonical path the module was actually loaded
                              ///< from (one of ParseImport()'s 3 search
                              ///< candidates) -- the identity key circular-
                              ///< import detection and per-file import
                              ///< scoping both key off of, so neither has to
                              ///< re-derive "what file is this" separately.
  std::unique_ptr<ExprAST> Body;
  ImportMode Mode = ImportMode::Bare;
  std::vector<ImportedSymbolRef> RequestedSymbols; // empty for Bare/Wildcard

public:
  ModuleImportExprAST(SourceLocation Loc, std::string ModuleName,
                      SourceLocation NameLoc, std::string ResolvedPath,
                      std::unique_ptr<ExprAST> Body,
                      ImportMode Mode = ImportMode::Bare,
                      std::vector<ImportedSymbolRef> RequestedSymbols = {})
      : Loc(Loc), ModuleName(std::move(ModuleName)), NameLoc(NameLoc),
        ResolvedPath(std::move(ResolvedPath)), Body(std::move(Body)),
        Mode(Mode), RequestedSymbols(std::move(RequestedSymbols)) {}

  const SourceLocation &getLoc() const { return Loc; }
  const SourceLocation &getNameLoc() const { return NameLoc; }
  const std::string &getModuleName() const { return ModuleName; }
  const std::string &getResolvedPath() const { return ResolvedPath; }
  const ExprAST *getBody() const { return Body.get(); }
  ImportMode getMode() const { return Mode; }
  const std::vector<ImportedSymbolRef> &getRequestedSymbols() const {
    return RequestedSymbols;
  }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitModuleImport(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<ModuleImportExprAST>(Loc, ModuleName, NameLoc,
                                                 ResolvedPath,
                                                 CloneExprAST(Body.get()),
                                                 Mode, RequestedSymbols);
  }
};

class VarDeclExprAST : public ExprAST {
  std::string Name;
  TurfTypePtr Type;
  std::unique_ptr<ExprAST> InitVal;
  SourceLocation Loc;      ///< location of the 'mutable'/'immutable' keyword
  SourceLocation NameLoc;  ///< location of the variable's own name token -- see issue #230
  // Location of the declared TYPE annotation (`: int` in `mut x: int = 5;`),
  // distinct from NameLoc. Parser::ParseVarDeclBody() already captures this
  // into a local `TypeLoc` to anchor a void-type error message; this keeps
  // it on the node instead of discarding it, for the same reason as
  // ParamDecl::TypeLoc (see its doc comment) -- TurfType itself carries no
  // source span. Equal to {0,0} default only in the theoretical case of a
  // node built without going through the real parser.
  SourceLocation TypeLoc;
  bool IsImmutable;
  bool HasExplicitInit = true;
  // Set only by `export immut NAME: TYPE = LITERAL;` (see ParseExportDecl()
  // in Parser.cpp). A top-level-only construct enforced semantically by the
  // Resolver (not the parser -- grammar-wise this node parses in any
  // statement position, exactly like an ordinary VarDeclExprAST; Resolver
  // raises ExportOutsideTopLevelError if FunctionNestingDepth_ > 0 when it
  // visits one). A static property of the declaration itself (like
  // IsImmutable), not resolve-time state, so unlike ResolvedSymbol it IS
  // copied by clone().
  bool IsExported = false;

public:
  VarDeclExprAST(SourceLocation Loc, std::string Name, SourceLocation NameLoc,
                 TurfTypePtr Type, std::unique_ptr<ExprAST> InitVal,
                 bool IsImmutable, bool HasExplicitInit = true,
                 SourceLocation TypeLoc = {})
      : Name(std::move(Name)), Type(std::move(Type)),
        InitVal(std::move(InitVal)), Loc(Loc), NameLoc(NameLoc), TypeLoc(TypeLoc),
        IsImmutable(IsImmutable), HasExplicitInit(HasExplicitInit) {}

  const SourceLocation &getLoc() const { return Loc; }
  const SourceLocation &getNameLoc() const { return NameLoc; }
  const SourceLocation &getTypeLoc() const { return TypeLoc; }
  const std::string &getName() const { return Name; }
  const TurfType *getType() const { return Type.get(); }
  TurfTypePtr getTypePtr() const { return Type; }
  void setTypePtr(TurfTypePtr NewType) { Type = std::move(NewType); }
  const ExprAST *getInitializer() const { return InitVal.get(); }
  bool isImmutable() const { return IsImmutable; }
  bool hasExplicitInit() const { return HasExplicitInit; }
  bool isExported() const { return IsExported; }
  void setExported(bool V) { IsExported = V; }

  /// Populated by the Resolver after a successful declareVariable().
  /// Nulled by clone() semantics (fresh node, default nullptr). Cloned ASTs must be re-resolved.
  std::shared_ptr<VariableSymbol> ResolvedSymbol;
  const VariableSymbol *getResolvedSymbol() const { return ResolvedSymbol.get(); }

  llvm::Value *codegen() override;
  /// `-g` only: the DW_TAG_variable for this local, at @p Alloca (CodegenVar.cpp).
  void emitLocalDebugVariable(llvm::AllocaInst *Alloca) const;
  void accept(ASTVisitor &V) override { V.visitVarDecl(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    auto Cloned = std::make_unique<VarDeclExprAST>(Loc, Name, NameLoc, Type,
                                            CloneExprAST(InitVal.get()), IsImmutable, HasExplicitInit, TypeLoc);
    Cloned->IsExported = IsExported;
    return Cloned;
    // Note: ResolvedSymbol intentionally NOT copied. ✓
  }
  // Kept private access for codegen
  const ExprAST *getInitVal() const { return InitVal.get(); }
};

// A single function parameter descriptor.
struct ParamDecl {
  TurfTypePtr Type;
  std::string Name;
  // Location of the parameter's NAME token. Defaults to {0,0} (via
  // SourceLocation's own default member values) for the handful of
  // synthetic ParamDecls the Parser builds without a real source position
  // (e.g. the implicit 'self' parameter) -- everywhere a parameter is
  // parsed from real user source, this is now populated, fixing
  // Resolver::visitFuncDef()'s duplicate-parameter-name diagnostic, which
  // used to hardcode {0,0} here and always report "declared here at line 0,
  // column 0" for the FIRST occurrence, regardless of where it actually was.
  SourceLocation Loc;
  // Location of the parameter's TYPE annotation (the token right after the
  // ':'), distinct from Loc (the parameter NAME's location) above. Every
  // real parse site already computes this into a local `ParamTypeLoc` just
  // to point a void-type error at it, then discarded it -- this just keeps
  // it. Needed for tooling (e.g. an LSP semantic-token span over `: int` in
  // `fn f(x: int)`), which a TurfTypePtr alone can't provide: TurfType is a
  // semantic value object with no source span of its own (types can be
  // shared/interned across unrelated syntactic occurrences). Defaults to
  // {0,0} like Loc for the synthetic 'self' parameter.
  SourceLocation TypeLoc;
};


// fn RetType Name<T>(params...) { body }
class FuncDefExprAST : public ExprAST {
  std::string Name;
  TurfTypePtr ReturnType;
  std::vector<ParamDecl> Params;
  std::unique_ptr<ExprAST> Body;
  std::vector<std::string> TypeParams;
  SourceLocation Loc;      ///< location of the 'fn'/'operator' keyword
  // Location of the function/method's own NAME token -- for `Struct.method`,
  // this is the METHOD name's position, not the struct name's (the more
  // useful hover/rename target). For a synthesized operator-overload name
  // (ParseOperatorDecl(), no real identifier token backs the name), this
  // equals the operator token's own location. See issue #230.
  SourceLocation NameLoc;
  // Whether the `static` keyword was written at declaration (`fn Rect.area()
  // static: int { ... }`). This is NOT redundant with "self absent from
  // Params": a free, non-struct-bound function also has no self, so
  // Resolver::visitFuncDef() needs this explicit signal to reject `static`
  // written on something that isn't a struct-bound method at all (a case
  // where "self absent" is already unconditionally true and tells it
  // nothing). For a genuine struct-bound method, self-presence and
  // !IsStaticDecl are kept in lockstep by the Parser (ParseFuncDef()), so
  // every OTHER call site can keep using self-presence alone, unchanged.
  bool IsStaticDecl = false;
  // STD-002: `@caller_location` written immediately before `fn` (parsed by
  // ParserCore.cpp's TOK_AT case). Deliberately NOT a Params entry -- see
  // CallerLocationExprAST's own doc comment (ASTExpr.h) for why keeping it
  // out of Params is what keeps this feature's footprint confined to
  // Codegen alone (arity checking, overload resolution, and mangling all
  // stay completely unaware this flag exists).
  bool HasCallerLocation = false;

public:
  FuncDefExprAST(SourceLocation Loc, std::string Name, SourceLocation NameLoc,
                 TurfTypePtr ReturnType,
                 std::vector<ParamDecl> Params, std::unique_ptr<ExprAST> Body,
                 std::vector<std::string> TypeParams = {},
                 bool IsStaticDecl = false,
                 bool HasCallerLocation = false)
      : Loc(Loc), NameLoc(NameLoc), Name(std::move(Name)), ReturnType(std::move(ReturnType)),
        Params(std::move(Params)), Body(std::move(Body)),
        TypeParams(std::move(TypeParams)), IsStaticDecl(IsStaticDecl),
        HasCallerLocation(HasCallerLocation) {}

  const std::string &getName() const { return Name; }
  /// TYPE-002 (S12a): renames a monomorphized clone to its unique,
  /// collision-free internal name (Monomorphize::instantiatedDeclName())
  /// BEFORE it is (re-)resolved -- see drainFreeFunctionRequests()'s doc
  /// comment for why this must happen before Resolver::resolve() runs on
  /// the clone. Never used on an ordinary (non-cloned) declaration.
  void setName(std::string N) { Name = std::move(N); }
  bool isStaticDecl() const { return IsStaticDecl; }
  bool hasCallerLocation() const { return HasCallerLocation; }
  const SourceLocation &getLoc() const { return Loc; }
  const SourceLocation &getNameLoc() const { return NameLoc; }
  const TurfType *getReturnType() const { return ReturnType.get(); }
  TurfTypePtr getReturnTypePtr() const { return ReturnType; }
  ExprAST *getBodyMut() const { return Body.get(); }
  const ExprAST *getBody() const { return Body.get(); }
  const std::vector<ParamDecl> &getParams() const { return Params; }
  /// Mutable access used only by Resolver::visitFuncDef() to rewrite a
  /// parameter's NamedType("T") into a real TurfGenericParamType("T") when
  /// "T" names one of this function's own TypeParams -- ParseType() has no
  /// way to know that at parse time (see substituteTypeParams()'s doc
  /// comment in Resolver.cpp). Codegen and TypeChecker read Params directly
  /// from this same shared AST node, so mutating it here is what makes the
  /// substitution visible to every later pass without each one having to
  /// redo the same TypeParams lookup independently.
  std::vector<ParamDecl> &getParamsMut() { return Params; }
  const std::vector<std::string> &getTypeParams() const { return TypeParams; }
  /// Mutable return-type setter, used alongside getParamsMut() for the same
  /// generic-parameter substitution.
  void setReturnType(TurfTypePtr T) { ReturnType = std::move(T); }
  /// TYPE-002 (S12a): once drainFreeFunctionRequests() has substituted
  /// every TurfGenericParamType in this clone's Params/ReturnType/body
  /// with a concrete type, this is no longer generic -- clearing
  /// TypeParams makes that true structurally, not just in effect: it stops
  /// Resolver::visitFuncDef()'s own substituteTypeParams() call from
  /// re-applying the (now-inapplicable) name->TurfGenericParamType
  /// transform when the clone is re-resolved, and is what sub-pass
  /// 2c/3a/3b's template-skip check (main.cpp) uses to tell a genuine,
  /// still-uninstantiated template apart from a finished clone. Never used
  /// on an ordinary (non-cloned) declaration.
  void clearTypeParams() { TypeParams.clear(); }

  /// Populated by the Resolver after registering the function symbol.
  /// clone() returns a fresh node, so this is nullptr in cloned trees.
  std::shared_ptr<ResolverFunctionSymbol> ResolvedDef;
  const ResolverFunctionSymbol *getResolvedDef() const {
    return ResolvedDef.get();
  }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitFuncDef(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<FuncDefExprAST>(
        Loc, Name, NameLoc, ReturnType, Params, CloneExprAST(Body.get()), TypeParams,
        IsStaticDecl, HasCallerLocation);
    // Note: ResolvedDef is intentionally NOT copied — cloned ASTs are stale
    // until re-resolved. The default-constructed field is nullptr.
  }
};

// extern fn Name(param: type, ...): ReturnType;
// Declares a C external function visible to this compilation unit.
// No function body is generated — only an LLVM ExternalLinkage prototype.
class ExternFuncDeclExprAST : public ExprAST {
  std::string Name_;
  std::vector<std::pair<std::string, TurfTypePtr>> Params_;
  TurfTypePtr ReturnType_;
  SourceLocation Loc_;     ///< location of the 'extern' keyword
  SourceLocation NameLoc_; ///< location of the function name token -- see issue #230
  bool IsVarArg_;        ///< true when the declaration ends with '...'
  std::string LibName_;  ///< non-empty when 'extern from "lib" fn' syntax used

public:
  ExternFuncDeclExprAST(
      SourceLocation Loc, std::string Name, SourceLocation NameLoc,
      std::vector<std::pair<std::string, TurfTypePtr>> Params,
      TurfTypePtr ReturnType,
      bool IsVarArg = false,
      std::string LibName = "")
      : Name_(std::move(Name)), Params_(std::move(Params)),
        ReturnType_(std::move(ReturnType)), Loc_(Loc), NameLoc_(NameLoc),
        IsVarArg_(IsVarArg), LibName_(std::move(LibName)) {}

  const std::string &getName()     const { return Name_; }
  const auto        &getParams()   const { return Params_; }
  TurfTypePtr        getReturnType()const { return ReturnType_; }
  const SourceLocation &getLoc()   const { return Loc_; }
  const SourceLocation &getNameLoc() const { return NameLoc_; }
  bool               isVarArg()    const { return IsVarArg_; }
  const std::string &getLibName()  const { return LibName_; }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitExternFuncDecl(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<ExternFuncDeclExprAST>(Loc_, Name_, NameLoc_, Params_,
                                                   ReturnType_, IsVarArg_,
                                                   LibName_);
  }
};

class EnumDeclExprAST : public ExprAST {
    std::string              Name;
    std::vector<std::string> Variants;
    TurfTypePtr              UnderlyingType;  // nullptr = TurfInt32 default
    SourceLocation           Loc;
public:
    EnumDeclExprAST(SourceLocation Loc, std::string Name,
                    std::vector<std::string> Variants,
                    TurfTypePtr UnderlyingType = nullptr)
        : Name(std::move(Name)), Variants(std::move(Variants)),
          UnderlyingType(std::move(UnderlyingType)), Loc(Loc) {}
    const std::string&              getName()     const { return Name; }
    const std::vector<std::string>& getVariants() const { return Variants; }
    const SourceLocation&           getLoc()      const { return Loc; }
    TurfTypePtr                     getUnderlyingType() const { return UnderlyingType; }
    llvm::Value* codegen() override;
    void accept(ASTVisitor& V) override { V.visitEnumDecl(*this); }
    std::unique_ptr<ExprAST> clone() const override {
        return std::make_unique<EnumDeclExprAST>(Loc, Name, Variants, UnderlyingType);
    }
};

// One declared transition edge inside a `machine` block: `From -> To`.
struct MachineEdge {
    std::string    From;
    std::string    To;
    SourceLocation Loc;
};

// `machine Name { State -> State, ... }` -- an enum whose variants ("states")
// carry a compiler-checked transition graph. Kept as its own AST node rather
// than bolting an edge list onto EnumDeclExprAST, for the same reason
// ErrorDeclExprAST stays distinct from EnumDeclExprAST below despite a
// near-identical variant-name shape: the declaration's own semantics
// (edge validation, a dedicated TurfMachineType) diverge enough to earn a
// separate node, even though `MachineName::State` access downstream reuses
// the ordinary enum-variant machinery unchanged (TurfMachineType subclasses
// TurfEnumType -- see include/Types.h).
class MachineDeclExprAST : public ExprAST {
    std::string               Name;
    std::vector<std::string>  States;   // first-appearance order = ordinal, same role as EnumDeclExprAST::Variants
    std::vector<MachineEdge>  Edges;    // declared transitions, in source order
    SourceLocation            Loc;
public:
    MachineDeclExprAST(SourceLocation Loc, std::string Name,
                       std::vector<std::string> States,
                       std::vector<MachineEdge> Edges)
        : Name(std::move(Name)), States(std::move(States)),
          Edges(std::move(Edges)), Loc(Loc) {}
    const std::string&              getName()   const { return Name; }
    const std::vector<std::string>& getStates() const { return States; }
    const std::vector<MachineEdge>& getEdges()  const { return Edges; }
    const SourceLocation&           getLoc()    const { return Loc; }
    llvm::Value* codegen() override;
    void accept(ASTVisitor& V) override { V.visitMachineDecl(*this); }
    std::unique_ptr<ExprAST> clone() const override {
        return std::make_unique<MachineDeclExprAST>(Loc, Name, States, Edges);
    }
};

// `error Name { Variant, ... }` (docs/architecture/errors_and_recover.md).
// Named ErrorDeclExprAST (not e.g. ErrorExprAST) deliberately to stay well
// clear of TurfError (include/Errors.h), the compiler's own internal diagnostic
// base class -- an unrelated concept that happens to share the English word
// "error". `IsBareForm` distinguishes the sugar `error Name;` (a single
// implicit `DEFAULT` variant) from the full braced form. Variants are bare
// names only, exactly like EnumDeclExprAST -- no payload fields (deferred,
// see errors_and_recover.md §5.1).
class ErrorDeclExprAST : public ExprAST {
    std::string              Name;
    std::vector<std::string> Variants;
    bool                     IsBareForm;
    SourceLocation           Loc;
public:
    ErrorDeclExprAST(SourceLocation Loc, std::string Name,
                     std::vector<std::string> Variants, bool IsBareForm)
        : Name(std::move(Name)), Variants(std::move(Variants)),
          IsBareForm(IsBareForm), Loc(Loc) {}
    const std::string&              getName()       const { return Name; }
    const std::vector<std::string>& getVariants()    const { return Variants; }
    bool                             isBareForm()     const { return IsBareForm; }
    const SourceLocation&            getLoc()         const { return Loc; }
    llvm::Value* codegen() override;
    void accept(ASTVisitor& V) override { V.visitErrorDecl(*this); }
    std::unique_ptr<ExprAST> clone() const override {
        return std::make_unique<ErrorDeclExprAST>(Loc, Name, Variants, IsBareForm);
    }
};


// Array declaration: int[5] arr   or   int[3] arr = [1, 2, 3]
class ArrayDeclExprAST : public ExprAST {
  std::string Name;
  TurfTypePtr ElementType;                        // e.g. TurfInt()
  int Size;                                       // compile-time constant size
  std::vector<std::unique_ptr<ExprAST>> InitList; // optional initializer list
  std::unique_ptr<ExprAST> InitExpr;              // optional general initializer
  bool IsImmutable;
  SourceLocation Loc;

public:
  ArrayDeclExprAST(SourceLocation Loc, std::string Name,
                   TurfTypePtr ElementType, int Size,
                   std::vector<std::unique_ptr<ExprAST>> InitList,
                   std::unique_ptr<ExprAST> InitExpr,
                   bool IsImmutable = false)
      : Name(std::move(Name)), ElementType(std::move(ElementType)), Size(Size),
        InitList(std::move(InitList)), InitExpr(std::move(InitExpr)), IsImmutable(IsImmutable), Loc(Loc) {}

  const SourceLocation &getLoc() const { return Loc; }
  const std::string &getName() const { return Name; }
  const TurfType *getElementType() const { return ElementType.get(); }
  TurfTypePtr getElementTypePtr() const { return ElementType; }
  /// TYPE-002 (S12a): lets the monomorphizer rewrite a generic function
  /// body's own `T[N] arr;` declaration to a concrete element type on a
  /// clone -- mirrors VarDeclExprAST::setTypePtr()'s existing contract.
  void setElementTypePtr(TurfTypePtr T) { ElementType = std::move(T); }
  int getSize() const { return Size; }
  const std::vector<std::unique_ptr<ExprAST>> &getInitList() const {
    return InitList;
  }
  const std::unique_ptr<ExprAST> &getInitExpr() const { return InitExpr; }
  bool isImmutable() const { return IsImmutable; }

  /// Populated by the Resolver after a successful declareVariable().
  /// Nulled by clone() semantics (fresh node, default nullptr). Cloned ASTs must be re-resolved.
  std::shared_ptr<VariableSymbol> ResolvedSymbol;
  const VariableSymbol *getResolvedSymbol() const { return ResolvedSymbol.get(); }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitArrayDecl(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<ArrayDeclExprAST>(Loc, Name, ElementType, Size,
                                              CloneExprList(InitList), 
                                              InitExpr ? InitExpr->clone() : nullptr,
                                              IsImmutable);
    // Note: ResolvedSymbol intentionally NOT copied. ✓
  }
};

// struct StructName<T> { type field1, type field2 }
class StructDeclExprAST : public ExprAST {
  std::string Name;
  std::vector<StructField> Fields;
  std::vector<std::string> TypeParams;
  SourceLocation Loc;      ///< location of the 'struct' keyword
  SourceLocation NameLoc;  ///< location of the struct's own name token -- see issue #230

public:
  StructDeclExprAST(SourceLocation Loc, std::string Name, SourceLocation NameLoc,
                    std::vector<StructField> Fields,
                    std::vector<std::string> TypeParams = {})
      : Loc(Loc), NameLoc(NameLoc), Name(std::move(Name)), Fields(std::move(Fields)), TypeParams(std::move(TypeParams)) {}

  const SourceLocation &getLoc() const { return Loc; }
  const SourceLocation &getNameLoc() const { return NameLoc; }
  const std::string &getName() const { return Name; }
  const std::vector<StructField> &getFields() const { return Fields; }
  /// Mutable access used only by Resolver::visitStructDecl() to rewrite a
  /// field's NamedType("T") into a real TurfGenericParamType("T") -- see the
  /// identical rationale on FuncDefExprAST::getParamsMut().
  std::vector<StructField> &getFieldsMut() { return Fields; }
  const std::vector<std::string> &getTypeParams() const { return TypeParams; }

  /// TYPE-002 (S13a): rename a monomorphized clone to its unique mangled
  /// name (Monomorphize::instantiatedDeclName()) -- same contract as
  /// FuncDefExprAST::setName() (S12a): must run BEFORE the clone is
  /// re-resolved, since Resolver::visitStructDecl() registers whatever
  /// name the node currently has.
  void setName(std::string NewName) { Name = std::move(NewName); }
  /// TYPE-002 (S13a): mark a clone "no longer generic" once every field's
  /// TurfGenericParamType has been substituted with a concrete type -- makes
  /// Resolver::visitStructDecl()'s own field-substitution loop a documented
  /// no-op (it only fires when getTypeParams() is non-empty) when the clone
  /// is re-resolved. Unlike FuncDefExprAST, main.cpp's sub-pass 2c codegens
  /// every StructDeclExprAST unconditionally (no TypeParams-empty skip guard
  /// exists or is needed for structs -- StructDeclExprAST::codegen()'s own
  /// by-NAME early-return guard already makes running it twice for the same
  /// name safe, and a generic TEMPLATE's own struct decl is deliberately
  /// left in PrePassASTs and still codegens the shared, erasure-era layout
  /// every existing (non-monomorphized) reference still needs -- see
  /// drainStructRequests()'s own doc comment, Monomorphize.cpp).
  void clearTypeParams() { TypeParams.clear(); }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitStructDecl(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<StructDeclExprAST>(Loc, Name, NameLoc, Fields, TypeParams);
  }
};

#endif
