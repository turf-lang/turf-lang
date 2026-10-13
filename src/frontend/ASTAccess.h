#ifndef AST_ACCESS_H
#define AST_ACCESS_H

#include "frontend/AST.h"
#include "sema/Intrinsics.h"

#include <unordered_map>


// Call to a user-defined function: name(arg1, arg2, ...)
class FuncCallExprAST : public ExprAST {
  std::unique_ptr<ExprAST> Callee;
  std::vector<std::unique_ptr<ExprAST>> Args;
  SourceLocation Loc;

public:
  FuncCallExprAST(SourceLocation Loc, std::unique_ptr<ExprAST> Callee,
                  std::vector<std::unique_ptr<ExprAST>> Args)
      : Callee(std::move(Callee)), Args(std::move(Args)), Loc(Loc) {}
  const ExprAST *getCallee() const { return Callee.get(); }
  const SourceLocation &getLoc() const { return Loc; }
  const std::vector<std::unique_ptr<ExprAST>> &getArgs() const { return Args; }

  /// All overloads matching the call name.  TypeChecker narrows this to one.
  /// clone() returns a fresh node, so this is empty in cloned trees.
  std::vector<std::shared_ptr<ResolverFunctionSymbol>> ResolvedCandidates;
  const std::vector<std::shared_ptr<ResolverFunctionSymbol>> &
  getResolvedCandidates() const {
    return ResolvedCandidates;
  }

  /// The single overload selected by TypeChecker::visitFuncCall() after
  /// narrowing ResolvedCandidates to one match (arity + type compatible).
  /// For bare intrinsics (typeof, sizeof, …) without a ResolvedCandidates
  /// entry this field is left null — the TypeChecker still sets the node's
  /// type directly.
  /// clone() returns a fresh node, so this is null in cloned trees — same
  /// clone-semantics contract as ResolvedSymbol. ✓
  std::shared_ptr<ResolverFunctionSymbol> ResolvedCallTarget;
  const ResolverFunctionSymbol *getResolvedCallTarget() const {
    return ResolvedCallTarget.get();
  }

  /// Which language-level bare intrinsic (`heap`, `typeof`, `gc_alloc`, ...)
  /// this call is, or BareIntrinsic::None. Set by the Resolver ONLY when no
  /// declared function of the callee's name exists, so a program's own
  /// `heap()` is an ordinary call and never this. Every pass after the
  /// Resolver reads this tag instead of comparing the callee's name
  /// (src/sema/Intrinsics.h).
  /// clone() returns a fresh node, so this is None in cloned trees -- the
  /// same clone-semantics contract as ResolvedCallTarget; the Resolver sets it
  /// again when it re-resolves the clone.
  BareIntrinsic ResolvedIntrinsic = BareIntrinsic::None;
  bool isIntrinsic(BareIntrinsic I) const { return ResolvedIntrinsic == I; }

  /// TYPE-002 (docs/development/changes/TYPE-002.md, S11b): the concrete
  /// type-parameter-name -> concrete-type map TypeChecker::visitFuncCall()
  /// builds via collectGenericSubstitution() to resolve this call's return
  /// type (TypeCheckerCall.cpp). Previously computed and immediately
  /// discarded, function-local; persisted here so the monomorphizer can
  /// read it back as an instantiation request without recomputing it.
  /// Empty for the overwhelming majority of calls, which aren't generic at
  /// all -- an empty map is not itself an instantiation request.
  /// clone() returns a fresh node, so this is empty in cloned trees — same
  /// clone-semantics contract as ResolvedCallTarget.
  std::unordered_map<std::string, TurfTypePtr> GenericSubstitution;

  /// TYPE-002 (S13b): for a `StructName::default()`/`StructName::new(...)`
  /// construction call (ScopeAccessKind::DefaultConstruct/ConstructorCall)
  /// whose result is being bound to a GenericType-typed declaration (e.g.
  /// `mut b: Box<Big> = Box::default();`), the CONCRETE instantiation type
  /// (`Box<Big>`, not the bare `Box` this call's own getType() is always
  /// stuck at -- see U-5, docs/development/changes/TYPE-002.md: a
  /// construction call structurally CANNOT carry its own type argument,
  /// only the enclosing declared type can). Set by
  /// TypeChecker::visitVarDecl() when it discovers this exact shape,
  /// nullptr otherwise (the overwhelming majority of calls). Read by
  /// FuncCallExprAST::codegen()'s DefaultConstruct/ConstructorCall branch
  /// (CodegenFunc.cpp) so the alloca it builds uses the per-instantiation
  /// TurfStructType (drainStructRequests(), S13a) instead of always the
  /// shared, erasure-era base struct -- without this, `Box<Big>`'s
  /// declared-type alloca (VarDeclExprAST, sized correctly) and its
  /// initializer's OWN alloca (this call, sized from the WRONG, shared
  /// "Box" layout) silently disagree.
  /// clone() returns a fresh node, so this is null in cloned trees — same
  /// clone-semantics contract as ResolvedCallTarget.
  TurfTypePtr ConstructedType;

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitFuncCall(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<FuncCallExprAST>(Loc, CloneExprAST(Callee.get()),
                                             CloneExprList(Args));
    // Note: ResolvedCandidates, ResolvedCallTarget, GenericSubstitution, and
    // ConstructedType are intentionally NOT copied — cloned ASTs are stale
    // until re-resolved. ✓
  }
};

// What a `Left::Right` scope-access expression turns out to mean, decided
// by the Resolver (the parser stays fully agnostic -- see ScopeAccessExprAST
// below). Unresolved is the state every node starts in; a fresh clone()
// resets back to it since a cloned tree is stale until re-resolved, same
// convention as FuncDefExprAST::ResolvedDef.
enum class ScopeAccessKind {
    Unresolved,
    EnumVariant,      // Color::Red
    ModuleFunctionRef, // io::printline (as a call target)
    ModuleTypeRef,     // list::List<int> (in type position)
    StaticMethodRef,   // StructName::method(...) (as a call target, no instance)
    ModuleConstantRef, // math::PI -- a qualified reference to an `export immut` constant
    ConstructorCall,   // StructName::new(...) (as a call target)
    DefaultConstruct,  // StructName::default() (as a call target)
    // ErrorType::Variant -- a bare, non-callable reference to a user-
    // declared `error` type's variant
    // (docs/architecture/errors_and_recover.md), e.g. the
    // `MathError::DivisionByZero` in `err == MathError::DivisionByZero` or
    // `return MathError::DivisionByZero;`. Semantically identical to
    // EnumVariant (bare name, no payload, rejected if called) -- kept as a
    // separate kind rather than reusing EnumVariant only because error types
    // lower differently in Codegen (a tag string, not an integer ordinal) and
    // participate in `!T`/`recover`/AnyErrorType comparisons that plain enums
    // don't.
    ErrorVariantRef,
    // `ErrorType::Variant(args)` -- the construction of an error value, as a
    // call target. A variant with N >= 1 declared payload fields takes those N
    // arguments, or N + 1 where the extra trailing `string` is the message
    // (LEC-004, FREEZE.md §6.18); a variant with no fields takes exactly one
    // argument, its message (STD-002, ARCHITECTURE_OVERHAUL.md
    // §8/§15.6(c)). Any other count is rejected by Resolver::visitFuncCall()'s
    // isErrorTypeName() branch (ResolverCall.cpp). Reuses `Left::Right(args)`'s
    // existing generic call parsing (§2.3/§2.5 of errors_and_recover.md) with
    // zero new grammar, exactly like ConstructorCall/DefaultConstruct above.
    // (Named ErrorVariantConstructWithMessage until LEC-004 gave variants
    // fields of their own.)
    ErrorVariantConstruct,
};

// `Left::Right` -- the general namespace-resolution operator. Originally
// only ever meant an enum-variant reference (EnumName::Variant); now also
// covers module-qualified function/type access (io::printline,
// list::List<int>). Renamed from EnumAccessExprAST when this was
// generalized: the class is otherwise unchanged in shape (still exactly
// two names + a location), because the parser was ALREADY fully agnostic
// about what `Identifier '::' Identifier` means -- it just records the two
// names and lets the Resolver decide, so widening what the Resolver is
// willing to decide is all a new kind of `::` usage needs.
class ScopeAccessExprAST : public ExprAST {
    std::string    LeftName;
    std::string    RightName;
    SourceLocation LeftLoc;
    SourceLocation RightLoc;
    SourceLocation Loc;
    // The `Module` in `Module::EnumName::Variant` (empty for the ordinary
    // `EnumName::Variant` form). A THIRD name is otherwise unrepresentable
    // in this node's two-name shape -- `Module::EnumName` on its own parses
    // as an ordinary ScopeAccessExprAST(LeftName=Module, RightName=EnumName)
    // like any other `Left::Right`, but when the PARSER sees yet another
    // `::` immediately follow, it knows the pair it just built was actually
    // a module-qualified ENUM NAME, not the enum-and-variant pair itself --
    // reinterpreted as ModuleQualifier=LeftName, LeftName=RightName (the
    // enum), RightName=the real variant that follows the second `::`. Only
    // meaningful when ResolvedKind == EnumVariant; validated the same way
    // as a qualified type/struct-literal (Resolver::validateModuleQualifier()).
    std::string ModuleQualifier;
    // FDN-003: LeftName has been rewritten from a spelling to a
    // TypeIdentity by the Resolver (resolveScopeLeftType()). Needed for the
    // same reason as NamedType's own flag (Types.h): a main-file type's
    // identity is its bare spelling, so the text alone can't say "done".
    bool LeftIsTypeIdentity = false;
public:
    // Set by the Resolver (visitScopeAccess / visitFuncCall for the call
    // form) once it decides what this node actually refers to. A future
    // LSP driving the same Resolver can read this post-resolution to know
    // what kind of hover/go-to-definition target this is.
    ScopeAccessKind ResolvedKind = ScopeAccessKind::Unresolved;

    /// Set by the Resolver on a bare `ErrorType::Variant` that is a direct
    /// operand of `==`/`!=` or the head of a pattern, where only the variant's
    /// IDENTITY is read. Only there may it name a variant that has payload
    /// fields: anywhere else a bare reference is a value, and a variant with
    /// fields has no value without its payload (LEC-004, FREEZE.md §6.18).
    /// Resolve-time state: never copied by clone().
    bool IsTagOperand = false;

    /// Populated by the Resolver when ResolvedKind == ModuleConstantRef
    /// (a qualified `module::CONST` value reference) -- see
    /// VariableExprAST::ResolvedConstant's doc comment for the storage-
    /// lifetime reasoning, identical here.
    const ResolverConstantSymbol *ResolvedConstant = nullptr;

    ScopeAccessExprAST(SourceLocation Loc, std::string LeftName,
                       SourceLocation LeftLoc, std::string RightName,
                       SourceLocation RightLoc, std::string ModuleQualifier = "")
        : LeftName(std::move(LeftName)), RightName(std::move(RightName)),
          LeftLoc(LeftLoc), RightLoc(RightLoc), Loc(Loc),
          ModuleQualifier(std::move(ModuleQualifier)) {}
    // Convenience overload for call sites that don't need distinct
    // left/right locations (both default to the whole expression's Loc).
    ScopeAccessExprAST(SourceLocation Loc, std::string LeftName, std::string RightName)
        : ScopeAccessExprAST(Loc, std::move(LeftName), Loc, std::move(RightName), Loc) {}

    const std::string& getLeftName()  const { return LeftName; }
    const std::string& getRightName() const { return RightName; }
    const SourceLocation& getLeftLoc()  const { return LeftLoc; }
    const SourceLocation& getRightLoc() const { return RightLoc; }
    const SourceLocation& getLoc()   const { return Loc; }
    const std::string& getModuleQualifier() const { return ModuleQualifier; }
    bool isLeftTypeIdentity() const { return LeftIsTypeIdentity; }
    /// FDN-003: the Resolver's one write -- see LeftIsTypeIdentity.
    void resolveLeftTypeTo(std::string Identity) {
        LeftName = std::move(Identity);
        LeftIsTypeIdentity = true;
    }
    // Pre-existing call sites written against the enum-only meaning of this
    // node (TypeChecker's compare/matches exhaustiveness checks, Codegen's
    // enum-variant codegen) keep working unchanged via these aliases.
    const std::string& getEnumName() const { return LeftName; }
    const std::string& getVariant()  const { return RightName; }
    llvm::Value* codegen() override;
    void accept(ASTVisitor& V) override { V.visitScopeAccess(*this); }
    std::unique_ptr<ExprAST> clone() const override {
        auto Cloned = std::make_unique<ScopeAccessExprAST>(Loc, LeftName, LeftLoc,
                                                            RightName, RightLoc,
                                                            ModuleQualifier);
        Cloned->LeftIsTypeIdentity = LeftIsTypeIdentity;
        return Cloned;
    }
};

// Base.Field
class FieldAccessExprAST : public ExprAST {
  std::unique_ptr<ExprAST> Base;
  std::string FieldName;
  SourceLocation Loc;      ///< location of the '.' token
  SourceLocation NameLoc;  ///< location of the field name token itself -- see issue #230

public:
  FieldAccessExprAST(SourceLocation Loc, std::unique_ptr<ExprAST> Base,
                     std::string FieldName, SourceLocation NameLoc)
      : Loc(Loc), Base(std::move(Base)), FieldName(std::move(FieldName)),
        NameLoc(NameLoc) {}

  const SourceLocation &getLoc() const { return Loc; }
  const SourceLocation &getNameLoc() const { return NameLoc; }
  const ExprAST *getBase() const { return Base.get(); }
  const std::string &getFieldName() const { return FieldName; }

  llvm::Value *codegen() override;
  llvm::Value *codegenLValue() override;
  void accept(ASTVisitor &V) override { V.visitFieldAccess(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<FieldAccessExprAST>(Loc, CloneExprAST(Base.get()),
                                                FieldName, NameLoc);
  }
};

// Array literal: [expr, expr, ...]
class ArrayLiteralExprAST : public ExprAST {
  std::vector<std::unique_ptr<ExprAST>> Elements;
  SourceLocation Loc;

public:
  TurfTypePtr CachedType;
  ArrayLiteralExprAST(SourceLocation Loc, std::vector<std::unique_ptr<ExprAST>> Elements)
      : Elements(std::move(Elements)), Loc(Loc) {}

  const SourceLocation &getLoc() const { return Loc; }
  const std::vector<std::unique_ptr<ExprAST>> &getElements() const { return Elements; }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitArrayLiteral(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    std::vector<std::unique_ptr<ExprAST>> ClonedElements;
    for (const auto &E : Elements)
      ClonedElements.push_back(CloneExprAST(E.get()));
    auto Cloned = std::make_unique<ArrayLiteralExprAST>(Loc, std::move(ClonedElements));
    Cloned->CachedType = CachedType;
    return Cloned;
  }
};

// Array element access: arr[index]
class ArrayAccessExprAST : public ExprAST {
  std::unique_ptr<ExprAST> Base;
  std::unique_ptr<ExprAST> Index;
  SourceLocation Loc;

public:
  ArrayAccessExprAST(SourceLocation Loc, std::unique_ptr<ExprAST> Base,
                     std::unique_ptr<ExprAST> Index)
      : Base(std::move(Base)), Index(std::move(Index)), Loc(Loc) {}

  const SourceLocation &getLoc() const { return Loc; }
  const ExprAST *getBase() const { return Base.get(); }
  const ExprAST *getIndex() const { return Index.get(); }

  /// LANG-001: `operator[]` (read) candidates -- gathered by
  /// Resolver::visitArrayAccess() whenever the base ISN'T already a real
  /// array/pointer/string (those keep their existing builtin indexing
  /// unchanged; a struct base is the only case an `operator[]` overload can
  /// apply to at all, since builtins can't declare one -- see
  /// Resolver::validateOperatorDecl()'s all-primitive rejection, which
  /// still runs for `operator[]`'s self type via the ordinary struct/enum
  /// check). Mirrors BinaryExprAST's ResolvedOperatorCandidates/Target
  /// pattern exactly. Empty/null in cloned trees.
  std::vector<std::shared_ptr<ResolverFunctionSymbol>> ResolvedIndexGetCandidates;
  std::shared_ptr<ResolverFunctionSymbol> ResolvedIndexGetTarget;

  /// `operator[]=` (write) candidates -- gathered alongside the read
  /// candidates above (the Resolver can't yet know whether THIS particular
  /// access will end up as an assignment target; that's decided later, in
  /// TypeChecker::visitAssignment(), by exact-matching (BaseType, IndexType,
  /// RHSType) against these). Never consulted by an ordinary read.
  std::vector<std::shared_ptr<ResolverFunctionSymbol>> ResolvedIndexSetCandidates;
  std::shared_ptr<ResolverFunctionSymbol> ResolvedIndexSetTarget;

  /// OPT-002: set by a `ValueRangePass::markElidableChecks()` pre-pass that
  /// runs BEFORE `Body->codegen()` (FuncDefExprAST::codegen(), CodegenFunc.cpp)
  /// when the index provably resolves to the same in-range constant on every
  /// path reaching this access -- the same constant-propagation dataflow the
  /// EXISTING post-codegen `ValueRangePass::runOnCFG()` invocation already
  /// uses to raise `ArrayBoundsError` for the out-of-range case (that call
  /// site is untouched: a provably-OOB access still gets that diagnostic,
  /// at the same point, exactly as before this field existed). Codegen
  /// (CodegenArray.cpp) skips `EmitRuntimeBoundsCheck` when this is true --
  /// F-OPT-2 (ARCHITECTURE_OVERHAUL.md §17.3): removal by proof only, never
  /// by `-O` level. Not copied by clone() -- same contract as
  /// ResolvedIndexGetTarget; a cloned (e.g. monomorphized) tree is
  /// re-analyzed by its own FuncDefExprAST::codegen() call.
  bool ProvenInBounds = false;

  llvm::Value *codegen() override;
  llvm::Value *codegenLValue() override;
  void accept(ASTVisitor &V) override { V.visitArrayAccess(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<ArrayAccessExprAST>(Loc, CloneExprAST(Base.get()),
                                                CloneExprAST(Index.get()));
  }
};

// Range slicing: `arr[start..end]`, `arr[start..]`, `arr[..end]`, `arr[..]`.
// A view, not a copy: the result is a `T[]` slice into the base array's own
// storage (the same `{ ptr, i64 }` a `T[]` parameter receives). `end` is
// exclusive, as in `0..n`; a negative bound counts from the end, as in
// `arr[-1]`. Its own node rather than an ArrayAccessExprAST flag because
// every pass treats an ArrayAccessExprAST as ONE element -- an lvalue slot,
// a bounds-checked index, an `operator[]` site -- and a slice is none of
// those.
class SliceExprAST : public ExprAST {
  std::unique_ptr<ExprAST> Base;
  std::unique_ptr<ExprAST> Start; ///< null when omitted (`arr[..e]`) = 0
  std::unique_ptr<ExprAST> End;   ///< null when omitted (`arr[s..]`) = length
  SourceLocation Loc;             ///< the '[' token

public:
  SliceExprAST(SourceLocation Loc, std::unique_ptr<ExprAST> Base,
               std::unique_ptr<ExprAST> Start, std::unique_ptr<ExprAST> End)
      : Base(std::move(Base)), Start(std::move(Start)), End(std::move(End)),
        Loc(Loc) {}

  const SourceLocation &getLoc() const { return Loc; }
  const ExprAST *getBase() const { return Base.get(); }
  const ExprAST *getStart() const { return Start.get(); }
  const ExprAST *getEnd() const { return End.get(); }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitSlice(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<SliceExprAST>(Loc, CloneExprAST(Base.get()),
                                          CloneExprAST(Start.get()),
                                          CloneExprAST(End.get()));
  }
};

// list method call: nums.push(val)  /  nums.pop()  /  nums.length
// Note: list<T> *declaration* is now a plain VarDeclExprAST with a
// GenericType("list", [elemType]) — no separate ListDeclExprAST needed.
class MethodCallExprAST : public ExprAST {
  std::unique_ptr<ExprAST> Base;
  std::string Method;
  std::vector<std::unique_ptr<ExprAST>> Args;
  SourceLocation Loc;      ///< location of the '.' token
  SourceLocation NameLoc;  ///< location of the method name token itself -- see issue #230

public:
  MethodCallExprAST(SourceLocation Loc, std::unique_ptr<ExprAST> Base,
                    std::string Method, SourceLocation NameLoc,
                    std::vector<std::unique_ptr<ExprAST>> Args = {})
      : Base(std::move(Base)), Method(std::move(Method)), Args(std::move(Args)),
        Loc(Loc), NameLoc(NameLoc) {}

  const SourceLocation &getLoc() const { return Loc; }
  const SourceLocation &getNameLoc() const { return NameLoc; }
  const ExprAST *getBase() const { return Base.get(); }
  const std::string &getMethod() const { return Method; }
  const std::vector<std::unique_ptr<ExprAST>> &getArgs() const { return Args; }

  /// The single overload selected by TypeChecker::visitMethodCall() after
  /// narrowing module-qualified candidates (e.g. io.printline).
  /// null until TypeChecker runs; also null in cloned trees (not copied). ✓
  std::shared_ptr<ResolverFunctionSymbol> ResolvedCallTarget;
  const ResolverFunctionSymbol *getResolvedCallTarget() const {
    return ResolvedCallTarget.get();
  }

  /// True when TypeChecker resolved this call via UFCS (a free function
  /// taking the receiver as its first argument, per
  /// STRING_UFCS_OPERATOR_CHAR_RFC.md §5) rather than a real bound struct
  /// method. Codegen needs to know which convention to use for passing the
  /// receiver -- by-value, like an ordinary argument, vs the self-pointer
  /// convention every bound method uses. Not copied by clone() -- same
  /// contract as ResolvedCallTarget.
  bool ResolvedViaUFCS = false;

  /// True when the UFCS callee takes a struct and the receiver is a pointer
  /// to it (`self.f()` inside a method): Codegen loads the receiver and
  /// passes the value (TypeChecker::tryUFCSOnReceiver()). Not copied by
  /// clone() -- same contract as ResolvedViaUFCS.
  bool UFCSLoadsReceiver = false;

  /// True once TypeChecker::visitMethodCall() confirms this is a
  /// `.transition(State)` call on a `TurfMachineType`-typed receiver (see
  /// src/sema/Types.h's TurfMachineType doc comment). Codegen branches on
  /// this the same way it branches on ResolvedViaUFCS -- a machine
  /// transition needs a completely different lowering (a plain store or a
  /// runtime-guarded switch, never an ordinary call). Not copied by
  /// clone() -- same contract as ResolvedCallTarget/ResolvedViaUFCS.
  bool IsMachineTransition = false;

  /// True only when IsMachineTransition is also true AND
  /// TypeChecker::visitCompare() statically narrowed the receiver to one
  /// specific state for the compare arm this call sits in, and verified the
  /// declared edge exists -- see TypeChecker's MachineNarrowStack_. Lets
  /// Codegen skip the runtime switch/panic guard entirely and emit a plain,
  /// zero-cost store, since the transition's validity is already proven.
  bool TransitionCompileTimeProven = false;

  /// TYPE-002 (docs/development/changes/TYPE-002.md, S11b): mirrors
  /// FuncCallExprAST::GenericSubstitution, but built from a different
  /// source for a method call -- the RECEIVER's own GenericType (e.g.
  /// `l: List<int>` => T=int), recovered by scanning the struct's fields
  /// (TypeChecker::visitMethodCall, TypeCheckerCall.cpp), not by
  /// unifying arguments against declared parameter types. Empty for a
  /// non-generic method call or a call on a non-generic struct.
  /// clone() returns a fresh node, so this is empty in cloned trees — same
  /// clone-semantics contract as ResolvedCallTarget.
  std::unordered_map<std::string, TurfTypePtr> GenericSubstitution;

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitMethodCall(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<MethodCallExprAST>(Loc, CloneExprAST(Base.get()),
                                               Method, NameLoc, CloneExprList(Args));
    // Note: ResolvedCallTarget, ResolvedViaUFCS, UFCSLoadsReceiver, IsMachineTransition,
    // TransitionCompileTimeProven, and GenericSubstitution intentionally
    // NOT copied — cloned ASTs are stale until re-resolved. ✓
  }
};

#endif
