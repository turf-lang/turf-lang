/**
 * @file ResolverDecl.cpp
 * @brief Declaration resolution: fn/struct/enum/error/extern/array/module-import, type-name validation.
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


void Resolver::visitFuncDef(FuncDefExprAST &Node) {
  if (FunctionNestingDepth_ > 0) {
    NestedFunctionError(Node.getLoc(), Node.getName()).raise();
    return;
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

  // "Never returns an error" warning (docs/planning/Errors.md): a `!T`-
  // declared function is PERMITTED, not obliged, to fail -- so this is a
  // warning, not a hard error. Checked here (once per function, after the
  // body is fully resolved) rather than inside buildFunctionSymbol() itself,
  // which runs multiple times per function (Pass 1 hoisting, then again
  // here) and would otherwise emit this warning 2-3x for the same function.
  // Filters Sym->PossibleErrorTypeNames through isErrorTypeName() since that
  // list is collected unfiltered (see its own doc comment) -- by now (Pass
  // 2, body fully walked) every error type in the program is registered, so
  // this filtering is finally safe to do.
  //
  // Deduped by source location (process-lifetime static, same convention as
  // ImportedFiles/ImportStack elsewhere in this file): this compiler parses
  // and resolves every function TWICE on two entirely separate AST
  // instances -- once in main.cpp's pre-pass (hoisting/self-reference/
  // codegen), once again in its main pass (a fresh re-parse, used only for
  // linting) -- confirmed live via this exact warning firing twice for one
  // function. Every OTHER existing warning happens to dodge this by living
  // in Codegen (which only runs once), which only runs once per function;
  // this is the first Resolver-level warning, so it's the first to hit it.
  if (Node.getReturnType() && Node.getReturnType()->isErrorUnion()) {
    bool ReturnsRealError = false;
    for (const auto &Name : Sym->PossibleErrorTypeNames) {
      if (SymTable_.isErrorTypeName(Name)) {
        ReturnsRealError = true;
        break;
      }
    }
    static std::unordered_set<long long> WarnedLocations;
    SourceLocation WarnLoc = Node.getLoc();
    long long LocKey = (static_cast<long long>(WarnLoc.Line) << 32) |
                       static_cast<unsigned>(WarnLoc.Col);
    if (!ReturnsRealError && WarnedLocations.insert(LocKey).second)
      NeverReturnsErrorWarning(WarnLoc, Node.getName()).emit();
  }

  SymTable_.exitScope();
  --FunctionNestingDepth_;
}


void Resolver::visitEnumDecl(EnumDeclExprAST &Node) {
  // Pass 1 already hoisted the enum to the registry.
}


void Resolver::visitMachineDecl(MachineDeclExprAST &Node) {
  // Pass 1 already hoisted the machine (states + edges) to the registry,
  // same as visitEnumDecl -- nothing left to do at Pass 2 for the
  // DECLARATION itself.
}


void Resolver::visitErrorDecl(ErrorDeclExprAST &Node) {
  // Pass 1 already hoisted the error type to the registry, same as
  // visitEnumDecl -- nothing left to do at Pass 2 for the DECLARATION
  // itself (variant field types were already fully specified/parsed; there
  // are no generic type parameters on an error type to substitute, unlike
  // visitStructDecl/visitFuncDef).
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
        !SymTable_.isEnumName(TypeName) &&
        !SymTable_.isErrorTypeName(TypeName)) {
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
