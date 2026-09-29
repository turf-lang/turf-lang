/**
 * @file ResolverDecl.cpp
 * @brief Declaration resolution: fn/struct/enum/error/extern/array/module-import, type-name validation.
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
#include "ErrorsFlow.h"      // SemanticError
#include "ResolverSymbolTable.h"
#include "ResolverInternal.h"
#include "SuggestionIndex.h" // SuggestionIndex::suggestVisible
#include "Types.h"           // RankedSuggestions

#include <memory>
#include <unordered_set>


// Functions already given NeverReturnsErrorWarning in this compile, by
// "file:line:col". Per compile, not per Resolver: main.cpp resolves every
// function with two Resolvers (see visitFuncDef()). It used to be a
// function-local static keyed by line and column alone, so it never reset
// -- a second analysis in one process (an editor re-checking the same file
// on every keystroke) silently lost the warning -- and one file's function
// at 10:1 hid the warning for another file's at 10:1.
static std::unordered_set<std::string> NeverReturnsErrorWarned;
static std::vector<std::unordered_set<std::string>> SavedResolverCompileStates;

void PushResolverCompileState() {
  SavedResolverCompileStates.push_back(std::move(NeverReturnsErrorWarned));
  NeverReturnsErrorWarned.clear();
}

void PopResolverCompileState() {
  NeverReturnsErrorWarned = std::move(SavedResolverCompileStates.back());
  SavedResolverCompileStates.pop_back();
}

/// LEC-002: a declaration inside a function body is local to it -- no other
/// module can ever name it -- so `pub` there means nothing and is an error
/// (FREEZE.md §6.15), not a silently ignored bit. Done here rather than in the
/// Parser for the same reason `export` is (ExportOutsideTopLevelError): the
/// Parser cannot cheaply tell "inside a function body" from "inside a
/// top-level `unsafe { }` block that is about to be flattened", and the
/// Resolver's FunctionNestingDepth_ already can.
static void rejectLocalVisibility(const VisibleDecl &D, int FunctionNestingDepth) {
  if (FunctionNestingDepth > 0 && D.isPublic())
    MisplacedVisibilityError(D.getVisibility().Loc,
                             MisplacedVisibilityError::Where::InsideFunction)
        .raise();
}

void Resolver::visitFuncDef(FuncDefExprAST &Node) {
  if (FunctionNestingDepth_ > 0) {
    NestedFunctionError(Node.getLoc(), Node.getName()).raise();
    return;
  }

  // LEC-002 stage 5: a `pub` signature may not mention this module's private
  // types. The receiver (`self`) is skipped: a `pub` method of a private type
  // is not a leak, since no importer can obtain that type's value except
  // through some other public item, which is checked in its own right.
  if (Node.isPublic()) {
    std::vector<const TurfType *> Mentioned;
    for (const auto &P : Node.getParams())
      if (P.Name != "self")
        Mentioned.push_back(P.Type.get());
    Mentioned.push_back(Node.getReturnType());
    const std::string Shown = TypeIdentity::bareName(Node.getName());
    checkNoPrivateInPublic(Node.getNameLoc(),
                           Shown.find('.') != std::string::npos ? "method" : "function",
                           Shown, Mentioned);
  }

  // `new`/`default`/`static` reservation checks. These are all syntactically
  // valid (ParseFuncDef() parses them fine -- see its own comment on why:
  // this mirrors how `export mut` parses fine and is rejected here, in the
  // Resolver, rather than in the parser). Internal method names are always
  // "StructName.method" (ParseFuncDef()'s '.'-declaration branch); an
  // ordinary free function's name never contains '.', so this split is
  // unambiguous and correctly a no-op for anything not struct-bound.
  {
    const std::string &FullName = Node.getName();
    size_t Dot = FullName.find('.');
    bool IsStructBound = Dot != std::string::npos;
    std::string MethodPart = IsStructBound ? FullName.substr(Dot + 1) : FullName;

    if (IsStructBound && MethodPart == "default") {
      // `default()` is compiler-synthesized (recursively default-inits
      // every field, reusing GetDefaultValue()) and can never be
      // hand-written -- there is no user code to run, so a body here would
      // just be silently unreachable in practice (StructName::default()
      // never calls it, see Resolver::visitFuncCall's DefaultConstruct
      // dispatch).
      ReservedConstructorNameError(Node.getLoc(), FullName.substr(0, Dot)).raise();
      return;
    }
    if (Node.isStaticDecl() && !IsStructBound) {
      // `static` only makes sense as "no self" on a struct-bound method --
      // on a free function it's meaningless (a free function never had
      // self to begin with).
      StaticOutsideStructError(Node.getLoc(), FullName).raise();
      return;
    }
    if (Node.isStaticDecl() && IsStructBound && MethodPart == "new") {
      // `new` always constructs via `self` (StructName::new(...) allocates
      // a fresh instance and calls this with that as self) -- 'static'
      // would mean no self at all, contradicting its entire purpose.
      ConstructorCannotBeStaticError(Node.getLoc(), FullName.substr(0, Dot)).raise();
      return;
    }

    // An instance method (anything with `self`, `new` included) on a
    // GENERIC struct must be generic itself: `self`'s layout depends on the
    // struct's type arguments, so the method needs one monomorphized copy
    // per instantiation, and only a declaration with type parameters is a
    // template the monomorphizer can clone. `fn Box.count()` on `struct
    // Box<T>` used to parse, resolve and typecheck, then die at its first
    // call as "Internal error: no generic method declaration found for
    // 'Box.count' during monomorphization" -- in the struct's own file or
    // on an imported struct alike. Rejected here with the exact fix rather
    // than inferred: silently giving the method the struct's parameter
    // names would re-bind any `T` its body already uses to mean a real
    // type. Static methods have no `self` and work as ordinary functions.
    // Skipped while resolving a monomorphized clone
    // (SuppressUnqualifiedVisibilityCheck_ is set only then --
    // resolveMonomorphizedClone(), Resolver.h): a clone is a concrete copy
    // of a generic method whose TypeParams were cleared on purpose.
    if (IsStructBound && !Node.isStaticDecl() && Node.getTypeParams().empty() &&
        !SuppressUnqualifiedVisibilityCheck_) {
      auto Generic = GenericStructIdentities_.find(FullName.substr(0, Dot));
      if (Generic != GenericStructIdentities_.end()) {
        const std::string Bare = TypeIdentity::bareName(Generic->first);
        std::string Params;
        for (size_t i = 0; i < Generic->second.size(); ++i)
          Params += (i ? ", " : "") + Generic->second[i];
        TypeError(Node.getNameLoc(),
                  "'" + Bare + "' is generic, so its method '" + MethodPart +
                      "' must declare its type parameters too.",
                  "Declare it as 'fn<" + Params + "> " + Bare + "." +
                      MethodPart + "(...)' -- the same parameters as 'struct " +
                      Bare + "<" + Params + ">'.",
                  "Missing type parameters!")
            .raise();
        return;
      }
    }
  }

  // `main` is always compiled as the real C-ABI entry point with a fixed
  // (argc: i32, argv: ptr) LLVM signature (see CodegenFunc.cpp's argv/argc
  // plumbing) -- there is no language syntax to declare argc/argv as Turf
  // params, and Codegen's alloca loop for "main" never looks at Params at
  // all. A declared `fn main(x: int): int` used to parse and typecheck
  // fine, then silently drop `x` (never alloca'd, never in NamedValues),
  // turning any reference to it in main's body into a confusing "undefined
  // variable" codegen error. Reject the declaration itself, up front, with
  // a diagnostic that names the actual constraint.
  if (Node.getName() == "main" && !Node.getParams().empty()) {
    SemanticError(Node.getLoc(),
                   "'main' cannot declare parameters -- it is the "
                   "program's entry point and always has the implicit "
                   "C-ABI signature (argc, argv).")
        .raise();
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
    validateNamedType(Param.Type, Node.getLoc(), Node.getTypeParams(),
                      /*AllowBareGeneric=*/Param.Name == "self");
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
      SymTable_.declareVariable(Param.Name, Param.Type, ParamLoc,
                                 /*IsImmutable=*/false, /*IsParameter=*/true);
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

  // "Never returns an error" warning (docs/architecture/errors_and_recover.md):
  // a `!T`- declared function is PERMITTED, not obliged, to fail -- so this is
  // a warning, not a hard error. Checked here (once per function, after the
  // body is fully resolved) rather than inside buildFunctionSymbol() itself,
  // which runs multiple times per function (Pass 1 hoisting, then again
  // here) and would otherwise emit this warning 2-3x for the same function.
  // Filters Sym->PossibleErrorTypeNames through isErrorTypeName() since that
  // list is collected unfiltered (see its own doc comment) -- by now (Pass
  // 2, body fully walked) every error type in the program is registered, so
  // this filtering is finally safe to do.
  //
  // Deduped by source location across Resolver instances (see
  // NeverReturnsErrorWarned below): this compiler parses and resolves
  // every function TWICE on two entirely separate AST instances -- once in
  // main.cpp's pre-pass (hoisting/self-reference/codegen), once again in
  // its main pass (a fresh re-parse, used only for linting) -- confirmed
  // live via this exact warning firing twice for one function. Every OTHER
  // existing warning happens to dodge this by living in Codegen (which
  // only runs once), which only runs once per function; this is the first
  // Resolver-level warning, so it's the first to hit it.
  if (Node.getReturnType() && Node.getReturnType()->isErrorUnion()) {
    // OVH-012: Sym->ReturnsReboundError (`json::parse`'s `recover as err
    // run { return err; };` shape -- see its own doc comment,
    // ResolverSymbolBase.h) is just as real a "this function can fail" as
    // any name in PossibleErrorTypeNames -- it names no specific error type
    // (there is none to name, `err` is type-erased AnyErrorType), so it
    // can't go through the isErrorTypeName() filter below at all.
    bool ReturnsRealError = Sym->ReturnsReboundError;
    for (const auto &Name : Sym->PossibleErrorTypeNames) {
      if (SymTable_.isErrorTypeName(Name)) {
        ReturnsRealError = true;
        break;
      }
    }
    SourceLocation WarnLoc = Node.getLoc();
    std::string LocKey = WarnLoc.Filename + ":" + std::to_string(WarnLoc.Line) +
                         ":" + std::to_string(WarnLoc.Col);
    if (!ReturnsRealError && NeverReturnsErrorWarned.insert(LocKey).second)
      NeverReturnsErrorWarning(WarnLoc, Node.getName()).emit();
  }

  SymTable_.exitScope();
  --FunctionNestingDepth_;
}


// Pass 1 hoists only TOP-LEVEL declarations (hoistTypes(), which never
// enters a function body), so a type declared inside a function body is
// first seen here -- registered now, exactly like a local struct
// (visitStructDecl()). Before, these three visitors were no-ops on the
// premise "Pass 1 already hoisted it", true only at the top level: a local
// `enum E { A, B }` parsed fine and then its own `E::B` failed as "'E'
// isn't a known enum, struct, or module". A top-level declaration reaching
// here is already registered at this same location, which
// registerLocalTypeDecl() recognizes (see there).
void Resolver::visitEnumDecl(EnumDeclExprAST &Node) {
  rejectLocalVisibility(Node, FunctionNestingDepth_);
  registerLocalTypeDecl(Node, Node.getLoc());
}


void Resolver::visitMachineDecl(MachineDeclExprAST &Node) {
  rejectLocalVisibility(Node, FunctionNestingDepth_);
  registerLocalTypeDecl(Node, Node.getLoc());
}


void Resolver::visitErrorDecl(ErrorDeclExprAST &Node) {
  rejectLocalVisibility(Node, FunctionNestingDepth_);
  registerLocalTypeDecl(Node, Node.getLoc());
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
    const std::vector<std::string> &TypeParams, bool AllowBareGeneric) const {
  if (!Ty)
    return;

  // The set of names that are always valid without being a declared struct.
  static const std::unordered_set<std::string> KnownPrimitives = {
      "int", "double", "bool", "string", "void", "null", "ptr", "void*"};

  // FDN-003: whatever this function checks, it also RESOLVES -- the checked
  // spelling is rewritten in place to its TypeIdentity (NamedType::
  // resolveTo()), so the TypeChecker and Codegen only ever see identities.
  // The positions this function does not itself descend into (a second
  // pointer level, an error union's success type) are resolved silently by
  // the walk at the end, so no spelling survives Pass 2 unresolved just
  // because its position was never validated.
  struct ResolveRest {
    const Resolver &R;
    const TurfTypePtr &Ty;
    const std::vector<std::string> &TypeParams;
    ~ResolveRest() {
      R.resolveTypeSpellingsSilently(Ty, R.currentModuleName(), TypeParams);
    }
  } Rest{*this, Ty, TypeParams};

  TurfType *BaseType = Ty.get();

  // Unwrap one level of pointer (e.g. Node* -> check "Node").
  if (auto *PT = dynamic_cast<TurfPointerType *>(BaseType))
    BaseType = PT->getPointee().get();

  // Unwrap any number of array dimensions (e.g. User[5][2] -> check "User").
  while (auto *AT = dynamic_cast<ArrayType *>(BaseType))
    BaseType = AT->getElementTypePtr().get();

  // Recurse into a function type's own parameter/return types (e.g.
  // `fn(NoSuchStruct): int`) -- same reasoning as the GenericType case just
  // below: without this, a misspelled type buried inside a `fn(...): ...`
  // annotation would compile with zero diagnostics and only surface later as
  // a confusing, misattributed Codegen-side opaque-struct issue.
  if (const auto *FT = dynamic_cast<const FuncType *>(BaseType)) {
    for (const auto &Param : FT->getParamTypes())
      validateNamedType(Param, Loc, TypeParams);
    validateNamedType(FT->getReturnType(), Loc, TypeParams);
    return;
  }

  // Spelling -> identity, with the diagnostic for each way that can fail.
  // `Candidates` feeds the "did you mean" of an UnknownTypeError only.
  auto ResolveOrReport = [&](const std::string &Qualifier, const std::string &Spelling,
                             const std::vector<std::string> &Candidates)
      -> std::optional<std::string> {
    const std::vector<std::string> Everywhere = SymTable_.identitiesWithBareName(Spelling);
    if (!Qualifier.empty()) {
      std::string Id = TypeIdentity::make(Qualifier, Spelling);
      if (SymTable_.hasModule(Qualifier) && SymTable_.isKnownTypeIdentity(Id)) {
        // LEC-002: a qualified spelling of another module's private type.
        if (!SymTable_.isTypeVisibleFrom(Id, currentModuleName())) {
          raisePrivateType(Loc, Id);
          return std::nullopt;
        }
        return Id;
      }
      if (Everywhere.empty()) {
        RankedSuggestions Suggestions;
        Suggestions.Local = SuggestionIndex::suggestVisible(Spelling, Candidates);
        UnknownTypeError(Loc, Spelling, Suggestions).raise();
      } else {
        validateModuleQualifier(Qualifier, Spelling, Loc);
      }
      return std::nullopt;
    }
    // A monomorphized clone (resolveMonomorphizedClone()): every spelling its
    // TEMPLATE wrote was resolved when the template itself was resolved; an
    // unresolved bare name here can only be a SUBSTITUTED type argument from
    // the call site's module (e.g. main's `Node` inside `list`'s clone), and
    // re-reading it in the template's module could silently turn it into
    // that module's own, unrelated `Node`. Leave it exactly as it is.
    if (SuppressUnqualifiedVisibilityCheck_ && !Everywhere.empty())
      return std::nullopt;
    auto Visible = SymTable_.unqualifiedTypeCandidates(currentModuleName(), Spelling);
    if (Visible.size() == 1)
      return Visible.front();
    if (Visible.size() > 1) {
      AmbiguousTypeReferenceError(Loc, Spelling, Visible).raise();
      return std::nullopt;
    }
    if (Everywhere.empty()) {
      RankedSuggestions Suggestions;
      Suggestions.Local = SuggestionIndex::suggestVisible(Spelling, Candidates);
      UnknownTypeError(Loc, Spelling, Suggestions).raise();
      return std::nullopt;
    }
    UnqualifiedTypeNotVisibleError(Loc, Spelling,
                                   TypeIdentity::owningModule(Everywhere.front()))
        .raise();
    return std::nullopt;
  };

  // Recurse into a generic type's own type arguments (e.g. List<NoSuchType>)
  // -- this function never used to look inside GenericType at all, so an
  // unknown type argument slipped through validation entirely and only
  // surfaced later as a confusing, misattributed error far from the real
  // cause (e.g. "No overload of 'List.push' matches argument types (int)",
  // blaming the call's argument instead of the undeclared type argument).
  if (auto *GT = dynamic_cast<GenericType *>(BaseType)) {
    // The generic's OWN base name (e.g. "List" in List<int>) is validated
    // too -- confirmed live, before it was: `mut x: NonExistentGeneric<int>
    // = 5;` produced an unrelated type-mismatch error, never "unknown type".
    const std::string BaseName = GT->getBaseName();
    bool BaseIsOwnTypeParam =
        GT->getModule().empty() &&
        std::find(TypeParams.begin(), TypeParams.end(), BaseName) != TypeParams.end();
    if (!GT->isBaseResolved() && !BaseIsOwnTypeParam) {
      // Candidates match exactly what is a valid generic base (a struct or
      // enum) -- see the NamedType branch below for why primitives/errors
      // are excluded here specifically.
      std::vector<std::string> Candidates;
      for (const auto &Id : SymTable_.getAllStructNames())
        Candidates.push_back(TypeIdentity::bareName(Id));
      for (const auto &Id : SymTable_.getAllEnumNames())
        Candidates.push_back(TypeIdentity::bareName(Id));
      if (auto Id = ResolveOrReport(GT->getModule(), BaseName, Candidates))
        GT->resolveBaseTo(*Id);
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
  // Visibility (FDN-003 wording of the pre-existing rule): a bare spelling
  // means the current file's/module's own type of that name, or one it
  // on-demand-imported (`import Module::Type;` / `Module::[Type,...]` /
  // `Module::[*]`). A plain `import Module;` alone grants qualified access
  // (`Module::Type`) but not bare access, exactly mirroring bare
  // function-call visibility.
  if (auto *NT = dynamic_cast<NamedType *>(BaseType)) {
    const std::string TypeName = NT->getName();
    bool IsOwnTypeParam =
        NT->getModule().empty() &&
        std::find(TypeParams.begin(), TypeParams.end(), TypeName) !=
        TypeParams.end();
    if (IsOwnTypeParam || KnownPrimitives.count(TypeName))
      return;
    // See GenericStructIdentities_'s doc comment (Resolver.h). `self` is the
    // one legitimate bare spelling (AllowBareGeneric): ParseFuncDef() builds
    // it from the receiver name alone, and the monomorphizer rewrites it to
    // the instantiation on every clone.
    auto RejectBareGeneric = [&](const std::string &Id) {
      if (AllowBareGeneric || !GenericStructIdentities_.count(Id))
        return;
      const std::string Bare = TypeIdentity::bareName(Id);
      TypeError(Loc,
                "'" + Bare + "' is generic -- it needs type arguments here.",
                "Write the instantiation you mean, e.g. '" + Bare + "<int>'.",
                "Missing type arguments!")
          .raise();
    };
    if (NT->isResolvedIdentity()) {
      RejectBareGeneric(TypeName);
      return;
    }
    // Candidates must cover every name this function actually accepts, not
    // just struct names -- a primitive typo (e.g. "itn" for "int") could
    // otherwise never surface a suggestion. TypeParams is included too (the
    // enclosing fn<T>/struct<T>'s own generic parameters, valid here but
    // not a registry entry).
    std::vector<std::string> Candidates;
    for (const auto &Id : SymTable_.getAllStructNames())
      Candidates.push_back(TypeIdentity::bareName(Id));
    for (const auto &Id : SymTable_.getAllEnumNames())
      Candidates.push_back(TypeIdentity::bareName(Id));
    for (const auto &Id : SymTable_.getAllErrorTypeNames())
      Candidates.push_back(TypeIdentity::bareName(Id));
    Candidates.insert(Candidates.end(), KnownPrimitives.begin(), KnownPrimitives.end());
    Candidates.insert(Candidates.end(), TypeParams.begin(), TypeParams.end());
    if (auto Id = ResolveOrReport(NT->getModule(), TypeName, Candidates)) {
      NT->resolveTo(*Id);
      RejectBareGeneric(*Id);
    }
  }
}


// See the doc comment on the declaration in Resolver.h.
std::vector<std::string> Resolver::unknownQualifierSuggestionCandidates() const {
  std::vector<std::string> Candidates = SymTable_.getAllEnumNames();
  std::vector<std::string> Structs = SymTable_.getAllStructNames();
  Candidates.insert(Candidates.end(), Structs.begin(), Structs.end());
  std::vector<std::string> Errors = SymTable_.getAllErrorTypeNames();
  Candidates.insert(Candidates.end(), Errors.begin(), Errors.end());
  std::vector<std::string> Modules = SymTable_.getAllModuleNames();
  Candidates.insert(Candidates.end(), Modules.begin(), Modules.end());
  return Candidates;
}


// See the doc comment on the declaration in Resolver.h.
bool Resolver::validateModuleQualifier(const std::string &Qualifier,
                                       const std::string &TypeName,
                                       SourceLocation Loc) const {
  if (Qualifier.empty())
    return true;

  if (!SymTable_.hasModule(Qualifier)) {
    // Unlike the general `Left::Right` fallback (unknownQualifierSuggestion
    // Candidates(), Resolver.h), Qualifier here is syntactically ALWAYS a
    // module position (`Module::TypeName`) -- modules are never nested and
    // a struct/enum/error-type can never legally appear here (see
    // ResolverCall.cpp's "modules aren't nested inside each other" check),
    // so the only name-kind worth suggesting is another module. This used
    // to search enum names instead, which could never actually match what
    // was being typed here.
    RankedSuggestions Suggestions;
    auto ModuleSugg = SuggestionIndex::suggestVisible(Qualifier, SymTable_.getAllModuleNames());
    for (const auto &S : ModuleSugg)
      Suggestions.Local.push_back(S + "::" + TypeName);
    UnknownScopeQualifierError(Loc, Qualifier, Suggestions).raise();
    return false;
  }

  // FDN-003: @p TypeName may be a spelling (`List` in `list::List`) or an
  // identity the caller already resolved (`list::List`); either way the
  // question is "is Qualifier::<bare name> a registered type".
  const std::string Bare = TypeIdentity::bareName(TypeName);
  if (!SymTable_.isKnownTypeIdentity(TypeIdentity::make(Qualifier, Bare))) {
    RankedSuggestions Suggestions;
    for (const auto &Id : SymTable_.identitiesWithBareName(Bare)) {
      const std::string Owner = TypeIdentity::owningModule(Id);
      if (!Owner.empty())
        Suggestions.Module[Owner].push_back(Bare);
    }
    UnknownModuleSymbolError(Loc, Qualifier, Bare, Suggestions).raise();
    return false;
  }

  // LEC-002: the qualifier is right and the type exists -- but it may still
  // be another module's private type.
  const std::string Id = TypeIdentity::make(Qualifier, Bare);
  if (!SymTable_.isTypeVisibleFrom(Id, currentModuleName())) {
    raisePrivateType(Loc, Id);
    return false;
  }

  return true;
}


void Resolver::visitStructDecl(StructDeclExprAST &Node) {
  rejectLocalVisibility(Node, FunctionNestingDepth_);
  if (FunctionNestingDepth_ > 0)
    for (const auto &F : Node.getFields())
      if (F.Vis.IsPublic)
        MisplacedVisibilityError(F.Vis.Loc,
                                 MisplacedVisibilityError::Where::InsideFunction)
            .raise();
  // LEC-002 stage 5: a `pub` field may not have a type this module keeps
  // private (checkNoPrivateInPublic() explains the scope).
  for (const auto &F : Node.getFields())
    if (F.Vis.IsPublic)
      checkNoPrivateInPublic(F.NameLoc, "field",
                             TypeIdentity::bareName(Node.getName()) + "." + F.Name,
                             {F.Type.get()});
  // Pass 1 (hoistTopLevel) already registered this struct's name -- but ONLY
  // for a struct declared at the TOP LEVEL of a file or an imported module's
  // body. hoistTopLevel() never recurses into a FUNCTION BODY looking for
  // nested declarations, so a struct declared LOCALLY inside a function
  // (`fn f() { struct Point {...}; mut p: Point = ...; }` -- a supported,
  // already-tested pattern, see tests/06_structs/02_struct_local.tr) is
  // never hoisted at all. Pass 2 visits statements in textual order, so by
  // the time THIS node is visited, it's still safe to register it here for
  // the first time -- any later statement in the same or a nested scope
  // that references the name will see it correctly.
  //
  // A registration that finds the name taken is a real duplicate only if
  // it came from a DIFFERENT declaration. The same node is legitimately
  // seen twice -- a top-level struct already hoisted by Pass 1, a local
  // struct inside a generic function whose body is cloned per
  // instantiation -- and both report the declaration's own location. This
  // used to be `if (!isKnownStruct(name)) register`, which also silently
  // skipped a genuinely SECOND declaration: two functions each declaring a
  // local `struct P` (or a local `P` shadowing a top-level one) shared ONE
  // registration and one layout, and whichever the Parser saw last won --
  // the other function's `p.x` failed as "Struct 'P' has no field named
  // 'x'", or read the wrong offset. A type's identity is (module, name)
  // (FREEZE.md §6.11), with no function-level scope, so this is E034.
  {
    SourceLocation Loc = Node.getLoc();
    if (auto Prev = SymTable_.registerStruct(Node.getName(), {Loc.Line, Loc.Col},
                                             currentModuleName())) {
      if (Prev->Line != Loc.Line || Prev->Col != Loc.Col) {
        DuplicateTypeNameError(Loc, Node.getName(), "struct",
                               {Prev->Line, Prev->Col})
            .raise();
        return;
      }
    }
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

    // OVH-006: GlobalSymbolTable's registered TurfStructType for this name
    // (ParseStructDecl(), built at PARSE time, directly from the as-parsed
    // Fields) is a SEPARATE object from this AST node -- the substitution
    // two lines up only ever updated the node's OWN field list, so every
    // downstream consumer that resolves a field type by looking the struct
    // back up via GlobalSymbolTable->LookupType(Name) (TypeCheckerCall.cpp's
    // visitFieldAccess/visitMethodCall, CodegenCore.cpp's mirrors) saw the
    // stale, unsubstituted NamedType("T") forever, never a real
    // TurfGenericParamType. That NamedType then had to be told apart from an
    // ordinary struct reference by a name-lookup heuristic ("not a
    // registered type => must be a generic param") -- which silently broke
    // the moment a real, unrelated struct happened to be declared under the
    // same name as the type parameter (`struct T { x: int }` alongside
    // `List<T>`, confirmed live: `l.pop()` on a `List<int>` resolved to the
    // user's own struct `T`, not `int`, because "T" WAS registered
    // elsewhere the instant that struct existed). Re-registering here, with
    // the just-substituted field list, makes the registered type agree with
    // the AST node: one representation, not two, and every one of those
    // NamedType-vs-registered-type heuristics simply never fires for a
    // struct's own type parameter again, regardless of what else in the
    // program happens to be named the same.
    if (GlobalSymbolTable)
      GlobalSymbolTable->RegisterType(
          Node.getName(),
          std::make_shared<TurfStructType>(Node.getName(), Node.getFields()));
  }

  // In Pass 2 (here) we validate every field's type via validateNamedType().
  for (const auto &Field : Node.getFields())
    validateNamedType(Field.Type, Node.getLoc(), Node.getTypeParams());
}


void Resolver::visitArrayDecl(ArrayDeclExprAST &Node) {
  // FDN-003: `mut a: Node[4];`'s element type is a spelling too.
  resolveTypeSpellingsSilently(Node.getElementTypePtr(), currentModuleName(),
                               CurrentFuncTypeParams_);

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

  // LANG-001: gather BOTH `operator[]` and `operator[]=` candidates
  // unconditionally, here, regardless of whether this particular access
  // turns out to be a read or an assignment target -- the Resolver has no
  // type information yet (that's the TypeChecker's job) and, symmetrically,
  // no syntactic way to know if this node sits on the LHS of '=' until
  // TypeChecker::visitAssignment() looks at it. Mirrors visitBinary()'s own
  // "gather everything visible by name, let TypeChecker's exact-match
  // narrow it" split. A plain array/pointer/string base simply never
  // matches anything here (no `operator[]` is ever declared with a
  // primitive self type -- see validateOperatorDecl()'s all-primitive/
  // orphan checks), so these stay empty and TypeChecker's existing builtin
  // indexing path is completely unaffected.
  Node.ResolvedIndexGetCandidates.clear();
  Node.ResolvedIndexSetCandidates.clear();
  for (const auto *S : SymTable_.lookupCurrentFileFunctions(OperatorIndexGetName()))
    Node.ResolvedIndexGetCandidates.push_back(
        std::make_shared<ResolverFunctionSymbol>(*S));
  for (const auto &M : SymTable_.findInModules(OperatorIndexGetName()))
    Node.ResolvedIndexGetCandidates.push_back(
        std::make_shared<ResolverFunctionSymbol>(*M.Symbol));
  for (const auto *S : SymTable_.lookupCurrentFileFunctions(OperatorIndexSetName()))
    Node.ResolvedIndexSetCandidates.push_back(
        std::make_shared<ResolverFunctionSymbol>(*S));
  for (const auto &M : SymTable_.findInModules(OperatorIndexSetName()))
    Node.ResolvedIndexSetCandidates.push_back(
        std::make_shared<ResolverFunctionSymbol>(*M.Symbol));
}


void Resolver::visitModuleImport(ModuleImportExprAST &Node) {
  ModuleStack_.push_back(Node.getModuleName());
  visitChild(Node.getBody());
  ModuleStack_.pop_back();
}


// ExternFuncDeclExprAST: for a FILE-LEVEL extern, hoistTopLevel already
// registered the symbol in Pass 1, so the only work left here is validating
// that the parameter and return type names are real -- see
// validateNamedType()'s doc comment. This used to be a complete no-op,
// meaning an extern fn parameter or return type referencing an unknown
// struct name was never checked anywhere in the compiler; ordinary `fn`
// definitions had the same gap until visitFuncDef() was updated alongside
// this.
//
// A FUNCTION-LOCAL extern (LANG-004 item 3, `extern fn` now legal inside a
// function body -- see ParseBlock()'s TOK_EXTERN branch) is a genuinely
// different case: hoistTopLevel() never enters a function body (see its own
// doc comment; that's the whole reason Pass 1/Pass 2 are split), so a
// function-local extern's symbol is NEVER registered before this visit --
// unlike every other declaration this function handles. It must be
// registered here instead, the moment Pass 2's sequential walk reaches it,
// mirroring hoistTopLevel()'s own ExternFuncDeclExprAST branch verbatim.
// `FunctionNestingDepth_ > 0` (incremented only while visiting a function
// body, see visitFuncDef()) is what distinguishes the two cases -- at depth
// 0 this is the ordinary top-level extern already hoisted, and registering
// it again here would be redundant (registerCurrentFileFunction() dedups by
// arity+param-types so it's harmless, but there's no reason to pay for it).
//
// Known limitation, not fixed here: `CurrentFileFunctions_` is a flat,
// file-wide registry with no block-scoping concept (nothing in this
// compiler's declaration model has one -- a nested `fn` is rejected
// outright, see NestedFunctionError above). A function-local extern
// therefore becomes callable from anywhere later in the file's sequential
// Pass-2 walk, not just from within its own enclosing function -- genuine
// lexical scoping would need a real scoped-function-symbol mechanism, which
// is out of scope for this item (see docs/planning/ARCHITECTURE_OVERHAUL.md
// §8 LANG-004: "Low value, but it is one switch case").
void Resolver::visitExternFuncDecl(ExternFuncDeclExprAST &Node) {
  rejectLocalVisibility(Node, FunctionNestingDepth_);
  for (const auto &[ParamName, ParamType] : Node.getParams())
    validateNamedType(ParamType, Node.getLoc());
  validateNamedType(Node.getReturnType(), Node.getLoc());

  // LEC-002 stage 5: a `pub extern fn` is a public signature like any other.
  if (Node.isPublic()) {
    std::vector<const TurfType *> Mentioned;
    for (const auto &[ParamName, ParamType] : Node.getParams())
      Mentioned.push_back(ParamType.get());
    Mentioned.push_back(Node.getReturnType().get());
    checkNoPrivateInPublic(Node.getNameLoc(), "extern function", Node.getName(), Mentioned);
  }

  if (FunctionNestingDepth_ > 0) {
    ResolverFunctionSymbol Sym;
    Sym.BaseName = Node.getName();
    Sym.SourceModule = ""; // extern fns are in the global (file) namespace
    Sym.ReturnType = Node.getReturnType();
    Sym.IsExtern = true; // see IsExtern's doc comment in ResolverSymbolTable.h
    Sym.IsVarArg = Node.isVarArg();
    for (const auto &[PName, PType] : Node.getParams())
      Sym.Params.push_back({PName, PType});
    SymTable_.registerCurrentFileFunction(Sym);
  }
}
