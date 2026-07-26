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
  }
  // All other top-level nodes (e.g. standalone expressions, variable decls)
  // are not declarations and have nothing to hoist.
}

void Resolver::hoistModuleBody(const ExprAST *Body,
                               const std::string &ModuleName) {
  const auto *Block = dynamic_cast<const BlockExprAST *>(Body);
  if (!Block)
    return;

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
              Struct->getName(), {Loc.Line, Loc.Col})) {
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
              Enum->getName(), {Loc.Line, Loc.Col}, ET)) {
        DuplicateTypeNameError(Loc, Enum->getName(), "enum",
                               {PrevLoc->Line, PrevLoc->Col})
            .raise();
      }
    } else if (const auto *Import =
                   dynamic_cast<const ModuleImportExprAST *>(Expr.get())) {
      // Nested module imports (unusual but possible)
      hoistModuleBody(Import->getBody(), Import->getModuleName());
    }
  }
}


// ─── Core Issue #4 visitors ───────────────────────────────────────────────────

// Walk VarDecl nodes: recurse into the initializer first (so the RHS can't
// see the variable being declared, matching Turf's semantics), then add the
// name to the current scope.
void Resolver::visitVarDecl(VarDeclExprAST &Node) {
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
  if (!VarCallee) {
    visitChild(Node.getCallee());
    return;
  }

  const std::string &CalleeName = VarCallee->getName();

  auto CurrentFileMatches = SymTable_.lookupCurrentFileFunctions(CalleeName);
  auto ModuleMatches = SymTable_.findInModules(CalleeName);

  if (CurrentFileMatches.empty() && ModuleMatches.empty()) {
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
    // Those stdlib functions are found by findInModules() above (when their
    // module is imported), so they never reach this branch.  Do NOT add them
    // here — they should produce a proper "did you mean io.printline?" error
    // when called without a qualifier.
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
  // If the base is a plain identifier that names a known imported module
  // (e.g. `io` in `io.printline(...)`), skip variable-resolution on it.
  // Module names live in the module registry, not the variable scope stack,
  // so calling visitVariable("io") would always produce a spurious
  // ReferenceError.  The module import visitor already populated the Tier-2
  // registry; nothing more needs to be done for the base in this case.
  //
  // We do, however, mark the base node with its resolved module name so
  // that the ASTPrinter can show a proper badge (Issue #5 completeness).
  //
  // IMPORTANT — variable shadowing: a locally-declared variable of the same
  // spelling as a module ALWAYS takes priority over the module, exactly like
  // ordinary lexical scoping everywhere else in the language. This is not a
  // hypothetical: `io` is auto-imported into EVERY Turf program via
  // lib/std/prelude.tr, so a variable literally named `io` used to always be
  // misread as the io module — `mut io: Thing = ...; io.show();` raised
  // "Unknown function 'io.show'" instead of calling Thing's struct method,
  // because the module check ran before any check for an actual declared
  // variable of that name. ExistingVar is looked up ONCE here and reused
  // below (in the arity-check gate) so both places agree on the same
  // variable-wins decision instead of independently re-deriving it.
  auto *BaseVar = dynamic_cast<VariableExprAST *>(
      const_cast<ExprAST *>(Node.getBase()));
  std::shared_ptr<VariableSymbol> ExistingVar;
  bool BaseIsModule = false;
  if (BaseVar) {
    ExistingVar = SymTable_.lookupVariable(BaseVar->getName());
    BaseIsModule = !ExistingVar && SymTable_.hasModule(BaseVar->getName());

    if (ExistingVar) {
      // Base is a declared variable — resolve it directly (equivalent to
      // what visitVariable() would do, without re-running its lookup).
      BaseVar->ResolvedSymbol = ExistingVar;
    } else if (BaseIsModule) {
      // Base is a module qualifier — annotate it and skip variable resolution.
      BaseVar->ResolvedModuleName = BaseVar->getName();
    } else if (SymTable_.isKnownStruct(BaseVar->getName())) {
      // Base is a struct type name (static method call)
      BaseVar->IsStructName = true;
    } else {
      // Base is neither a declared variable, a module, nor a struct name —
      // resolve normally so visitVariable() raises the proper ReferenceError.
      visitChild(Node.getBase());
    }
  } else {
    // Base is a complex expression — resolve normally.
    visitChild(Node.getBase());
  }
  // Always resolve argument expressions regardless.
  for (const auto &Arg : Node.getArgs())
    visitChild(Arg.get());

  // ── Module method arity check ─────────────────────────────────────────────
  // If the base is a known module, look up the method in the module registry
  // and verify that at least one overload accepts the supplied argument count.
  // This mirrors the arity check in visitFuncCall — all done from symbol table
  // data, no hardcoded maps.
  //
  // Uses BaseIsModule (computed above, variable-priority-aware) rather than
  // re-checking SymTable_.hasModule() directly — otherwise a variable that
  // shadows a module name would correctly resolve as a variable above, but
  // still incorrectly trigger a module-only arity check down here.
  if (BaseIsModule) {
    const std::string &ModuleName = BaseVar->getName();
    const std::string &MethodName = Node.getMethod();
    const int ArgCount = static_cast<int>(Node.getArgs().size());

    // Look up all overloads of this method in the module.
    auto ModuleMatches = SymTable_.findInModules(MethodName);
    // Filter to those belonging to this specific module.
    std::vector<const ResolverFunctionSymbol *> MethodOverloads;
    for (const auto &Match : ModuleMatches) {
      if (Match.ModuleName == ModuleName)
        MethodOverloads.push_back(Match.Symbol);
    }

    if (!MethodOverloads.empty()) {
      // Method name exists in this module — check arity.
      bool AnyArityMatch = false;
      std::vector<std::string> AllSigs;
      std::vector<const ResolverFunctionSymbol *> ArityMatches;
      for (const auto *Sym : MethodOverloads) {
        AllSigs.push_back(ModuleName + "." + Sym->signature());
        if (Sym->arity() == ArgCount) {
          AnyArityMatch = true;
          ArityMatches.push_back(Sym);
        }
      }
      if (!AnyArityMatch) {
        auto Err = WrongArityError(Node.getLoc(),
                        ModuleName + "." + MethodName,
                        ArgCount, AllSigs);
        for (const auto *Cand : MethodOverloads)
          Err.withRelated(toSourceLocation(Cand->DeclLoc), "candidate");
        Err.raise();
        return;
      }

      // ── Set ResolvedCallTarget for the unambiguous case ──────────────────
      // When exactly one overload matches the arity, set it directly so that
      // Codegen can read mangledName() in the pre-pass before TypeChecker runs.
      // TypeChecker handles the multi-overload narrowing via type compatibility.
      if (ArityMatches.size() == 1) {
        Node.ResolvedCallTarget = std::make_shared<ResolverFunctionSymbol>(*ArityMatches[0]);
      }
    } else {
      // Method does not exist in this module.
      RankedSuggestions Suggestions;
      std::vector<std::string> ModuleNames = SuggestionIndex::suggestVisible(
          MethodName, SymTable_.getModuleFunctionNames(ModuleName));
      
      for (const auto &SuggestedName : ModuleNames) {
        auto Matches = SymTable_.findInModules(SuggestedName);
        for (const auto &Match : Matches) {
          if (Match.ModuleName == ModuleName) {
            Suggestions.Module[ModuleName].push_back(Match.Symbol->signature());
          }
        }
      }
      
      UnknownFunctionError(Node.getLoc(), ModuleName + "." + MethodName, Suggestions).raise();
      return;
    }
  }
}

void Resolver::visitFieldAccess(FieldAccessExprAST &Node) {
  visitChild(Node.getBase());
}

void Resolver::visitEnumDecl(EnumDeclExprAST &Node) {
  // Pass 1 already hoisted the enum to the registry.
}

void Resolver::visitEnumAccess(EnumAccessExprAST &Node) {
  std::string EnumName = Node.getEnumName();
  std::string Variant = Node.getVariant();

  if (!SymTable_.isEnumName(EnumName)) {
    auto Sugg = SuggestionIndex::suggestVisible(EnumName, SymTable_.getAllEnumNames());
    std::string Msg = "Use of undeclared enum '" + EnumName + "'.";
    if (!Sugg.empty()) {
      Msg += "\nDid you mean:\n";
      for (const auto& s : Sugg) Msg += "    " + s + "\n";
    }
    LogErrorAt(Node.getLoc(), Msg);
    return;
  }

  if (Variant != "COUNT" && Variant != "VALUES" && Variant != "NAMES") {
    auto ET = SymTable_.lookupEnumType(EnumName);
    if (ET && !ET->hasVariant(Variant)) {
      std::string Msg = "Enum '" + EnumName + "' has no variant '" + Variant + "'.";
      auto Sugg = SuggestionIndex::suggestVisible(Variant, ET->getVariants());
      if (!Sugg.empty()) {
        Msg += "\nDid you mean:\n";
        for (const auto& s : Sugg) Msg += "    " + s + "\n";
      }
      LogErrorAt(Node.getLoc(), Msg);
    }
  }
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
    }
  }
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
    SymTable_.registerStruct(Node.getName(), {Loc.Line, Loc.Col});
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
