/**
 * @file ResolverStmt.cpp
 * @brief Statement/control-flow resolution: var decl, block, if/while/do-while/for/for-each, return.
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
#include "ResolverSymbolTable.h"
#include "ResolverInternal.h"
#include "SuggestionIndex.h" // SuggestionIndex::suggestVisible
#include "Types.h"           // RankedSuggestions

#include <memory>
#include <unordered_set>



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

  // Rewrite a bare `T` (or `T*`, `T[]`, `List<T>`, ...) in this local
  // variable's declared type into a real TurfGenericParamType, exactly like
  // buildFunctionSymbol() already does for a generic function/method's own
  // parameters/return type (ResolverDecl.cpp) and visitStructDecl() already
  // does for a generic struct's fields -- see substituteTypeParams()'s own
  // doc comment (ResolverCore.cpp) for the full rationale. A plain
  // VarDeclExprAST inside a generic function/method BODY was the one
  // remaining place ParseType()'s NamedType("T") placeholder never got
  // converted: validateNamedType() above only checks the name is a known
  // type-param (harmless leniency), it never rewrites the AST, so
  // typesCompatible()'s type-erasure fast path (Types.cpp, matches on a
  // real TurfGenericParamType specifically) never fired for a local like
  // `immutable key: T = self.data[i];` inside `fn<T> List.sort(...)`.
  // Confirmed live: that exact line rejected with "Cannot initialise 'int'
  // variable 'keyInt' with value of type 'T'" even though Codegen has
  // always handled this correctly (T erases to a plain 64-bit slot).
  // Non-generic contexts (CurrentFuncTypeParams_ empty) are a no-op, same
  // guard substituteTypeParams() already has internally.
  Node.setTypePtr(substituteTypeParams(Node.getTypePtr(), CurrentFuncTypeParams_));

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

// `unsafe { }` (issue #238): a pure lexical gate, not a Resolver concern --
// all raw-pointer-use enforcement lives in the TypeChecker (see
// TypeChecker::visitUnsafeBlock()'s UnsafeDepth_ tracking). Resolver only
// needs to keep resolving names inside the body; Body is always a
// BlockExprAST (see ParseUnsafeExpr()), so delegating straight to it
// reuses visitBlock()'s own enterScope/exitScope bracket above instead of
// duplicating it here.
void Resolver::visitUnsafeBlock(UnsafeBlockExprAST &Node) {
  visitChild(Node.getBodyMut());
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


void Resolver::visitDoWhile(DoWhileExprAST &Node) {
  visitChild(Node.getBody());
  visitChild(Node.getCond());
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


void Resolver::visitForEach(ForEachExprAST &Node) {
  // Mirrors visitFor()'s structure:
  //   1. Resolve the iterable in the *outer* scope (loop vars don't exist yet).
  //   2. Push a new scope, declare the loop variable(s).
  //   3. Resolve the body inside that scope.
  //   4. Exit the scope so the loop variable(s) don't leak.

  visitChild(Node.getIterable());

  SymTable_.enterScope();

  ResolverSourceLocation Loc{Node.getLoc().Line, Node.getLoc().Col};

  // Value var gets a placeholder int type here -- the Resolver doesn't do
  // type inference (that's TypeChecker's job). TypeChecker::visitForEach()
  // overwrites Node.ResolvedValueVar->Type with the array's real element
  // type once it's known, same "placeholder now, refine later" contract
  // ForExprAST's induction variable already relies on (see its own comment
  // above).
  try {
    Node.ResolvedValueVar =
        SymTable_.declareVariable(Node.getValueVarName(), TurfInt(), Loc);
  } catch (const std::runtime_error &) {
    auto Prev = SymTable_.lookupVariable(Node.getValueVarName());
    SourceLocation ErrLoc = Node.getLoc();
    if (Prev) {
      SourceLocation PrevLoc{Prev->DeclLoc.Line, Prev->DeclLoc.Col};
      DuplicateDeclarationError(ErrLoc, Node.getValueVarName(), PrevLoc).raise();
    } else {
      SyntaxError(ErrLoc, "Loop variable '" + Node.getValueVarName() +
                               "' conflicts with an existing declaration.")
          .raise();
    }
  }

  // Index var (paired form only) is genuinely always int -- not a
  // placeholder, TypeChecker doesn't need to touch it. Declaring it in the
  // same scope as the value var means `for (x, x) in arr` naturally hits
  // declareVariable()'s same-scope-duplicate throw below.
  if (Node.hasIndexVar()) {
    try {
      Node.ResolvedIndexVar =
          SymTable_.declareVariable(Node.getIndexVarName(), TurfInt(), Loc);
    } catch (const std::runtime_error &) {
      auto Prev = SymTable_.lookupVariable(Node.getIndexVarName());
      SourceLocation ErrLoc = Node.getLoc();
      if (Prev) {
        SourceLocation PrevLoc{Prev->DeclLoc.Line, Prev->DeclLoc.Col};
        DuplicateDeclarationError(ErrLoc, Node.getIndexVarName(), PrevLoc).raise();
      } else {
        SyntaxError(ErrLoc, "Loop variable '" + Node.getIndexVarName() +
                                 "' conflicts with an existing declaration.")
            .raise();
      }
    }
  }

  visitChild(Node.getBody());

  SymTable_.exitScope();
}


void Resolver::visitReturn(ReturnExprAST &Node) {
  visitChild(Node.getVal());
}
