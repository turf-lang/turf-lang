#ifndef AST_STMT_H
#define AST_STMT_H

#include "AST.h"


// A single condition + body pair (used for if and each elseif)
struct CondBranch {
  SourceLocation Loc; // location of 'if' or 'elseif' keyword
  std::unique_ptr<ExprAST> Cond;
  std::unique_ptr<ExprAST> Body;
};

// If-Expr AST, represents if-else branch
class IfExprAST : public ExprAST {
  // The first entry is the 'if' branch.
  // Subsequent entries are 'elseif' branches.
  std::vector<CondBranch> Branches;

  // Optional final 'else' body (nullptr if absent).
  std::unique_ptr<ExprAST> ElseBody;
  SourceLocation ElseLoc;

public:
  IfExprAST(std::vector<CondBranch> Branches, std::unique_ptr<ExprAST> ElseBody,
            SourceLocation ElseLoc = {0, 0})
      : Branches(std::move(Branches)), ElseBody(std::move(ElseBody)),
        ElseLoc(ElseLoc) {}

  // Primary location is the 'if' keyword
  const SourceLocation &getLoc() const { return Branches.front().Loc; }
  const SourceLocation &getElseLoc() const { return ElseLoc; }

  const std::vector<CondBranch> &getBranches() const { return Branches; }
  ExprAST *getElseBody() const { return ElseBody.get(); }

  // Convenience: backward-compatible accessors for single if/else
  ExprAST *getThen() const { return Branches.front().Body.get(); }
  ExprAST *getElse() const { return ElseBody.get(); }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitIf(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    std::vector<CondBranch> ClonedBranches;
    ClonedBranches.reserve(Branches.size());
    for (const auto &Branch : Branches) {
      ClonedBranches.push_back({Branch.Loc, CloneExprAST(Branch.Cond.get()),
                                CloneExprAST(Branch.Body.get())});
    }
    return std::make_unique<IfExprAST>(std::move(ClonedBranches),
                                       CloneExprAST(ElseBody.get()), ElseLoc);
  }
};


class BlockExprAST : public ExprAST {
  std::vector<std::unique_ptr<ExprAST>> Expressions;

public:
  BlockExprAST(std::vector<std::unique_ptr<ExprAST>> Expressions)
      : Expressions(std::move(Expressions)) {}

  const std::vector<std::unique_ptr<ExprAST>> &getExpressions() const {
    return Expressions;
  }

  /// Moves the child list out, leaving this block empty. Used only by the
  /// top-level `unsafe { }` flattening (issue #238, see
  /// UnsafeBlockExprAST::takeChildren() and Parser.h's
  /// AppendFlattenedTopLevelDecl()) -- everywhere else, prefer
  /// getExpressions().
  std::vector<std::unique_ptr<ExprAST>> takeExpressions() {
    return std::move(Expressions);
  }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitBlock(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<BlockExprAST>(CloneExprList(Expressions));
  }
};

// `unsafe { }` block (issue #238) -- a lexical gate, not a new control-flow
// construct: doesn't affect break/continue/return, purely marks its body
// (a plain BlockExprAST) as authorized to declare/cast/dereference raw
// pointer types (`ptr`, `T*`). Carries the `unsafe` keyword's own
// SourceLocation (unlike BlockExprAST, which has none) so the TypeChecker
// can point a "missing unsafe" diagnostic somewhere sensible relative to
// where the block DOES exist, and so callers can request it back.
class UnsafeBlockExprAST : public ExprAST {
  SourceLocation Loc;
  std::unique_ptr<ExprAST> Body; // always a BlockExprAST, from ParseBlock()

public:
  UnsafeBlockExprAST(SourceLocation Loc, std::unique_ptr<ExprAST> Body)
      : Loc(Loc), Body(std::move(Body)) {}

  const SourceLocation &getLoc() const { return Loc; }
  const ExprAST *getBody() const { return Body.get(); }
  ExprAST *getBodyMut() const { return Body.get(); }

  /// Moves this block's children out (its Body is always a BlockExprAST,
  /// from ParseBlock() -- see the constructor's callers). Used only by
  /// Parser.h's AppendFlattenedTopLevelDecl() to splice a TOP-LEVEL
  /// `unsafe { }`'s contents directly into a flat top-level declaration
  /// list -- see that function's own doc comment for why. Returns empty
  /// if Body somehow isn't a BlockExprAST (never true for a
  /// parser-constructed node, only relevant to a hand-built AST).
  std::vector<std::unique_ptr<ExprAST>> takeChildren() {
    if (auto *B = dynamic_cast<BlockExprAST *>(Body.get()))
      return B->takeExpressions();
    return {};
  }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitUnsafeBlock(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<UnsafeBlockExprAST>(Loc, CloneExprAST(Body.get()));
  }
};

class WhileExprAST : public ExprAST {
  SourceLocation Loc;
  std::unique_ptr<ExprAST> Cond, Body;

public:
  WhileExprAST(SourceLocation Loc, std::unique_ptr<ExprAST> Cond,
               std::unique_ptr<ExprAST> Body)
      : Loc(Loc), Cond(std::move(Cond)), Body(std::move(Body)) {}

  const SourceLocation &getLoc() const { return Loc; }
  ExprAST *getBody() const { return Body.get(); }
  const ExprAST *getCond() const { return Cond.get(); }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitWhile(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<WhileExprAST>(Loc, CloneExprAST(Cond.get()),
                                          CloneExprAST(Body.get()));
  }
};

// do-while loop: do { body } while (cond);
// Key difference from WhileExprAST: the body always executes at least once,
// since the condition is only checked after the first pass through the body.
class DoWhileExprAST : public ExprAST {
  SourceLocation Loc;
  std::unique_ptr<ExprAST> Body, Cond;

public:
  DoWhileExprAST(SourceLocation Loc, std::unique_ptr<ExprAST> Body,
                 std::unique_ptr<ExprAST> Cond)
      : Loc(Loc), Body(std::move(Body)), Cond(std::move(Cond)) {}

  const SourceLocation &getLoc() const { return Loc; }
  ExprAST *getBody() const { return Body.get(); }
  const ExprAST *getCond() const { return Cond.get(); }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitDoWhile(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<DoWhileExprAST>(Loc, CloneExprAST(Body.get()),
                                            CloneExprAST(Cond.get()));
  }
};
// For-loop: for <var> in <start>..<end> step <step_expr> { body }
// Note: ForExprAST desugars to init + while(cond) { body; step } during Codegen.
// The AST node is kept for clean visitor dispatch and future TypeChecker support.
class ForExprAST : public ExprAST {
  std::string VarName;
  SourceLocation Loc;
  std::unique_ptr<ExprAST> Start, End, Step, Body;

public:
  ForExprAST(SourceLocation Loc, std::string VarName,
             std::unique_ptr<ExprAST> Start, std::unique_ptr<ExprAST> End,
             std::unique_ptr<ExprAST> Step, std::unique_ptr<ExprAST> Body)
      : Loc(Loc), VarName(std::move(VarName)), Start(std::move(Start)),
        End(std::move(End)), Step(std::move(Step)), Body(std::move(Body)) {}

  const SourceLocation &getLoc() const { return Loc; }  ///< Location of the 'for' keyword.
  const std::string &getVarName() const { return VarName; }
  ExprAST *getBody() const { return Body.get(); }
  const ExprAST *getStart() const { return Start.get(); }
  const ExprAST *getEnd() const { return End.get(); }
  const ExprAST *getStep() const { return Step.get(); }

  /// Populated by the Resolver after successfully declaring the induction variable.
  /// Nulled by clone() semantics (fresh node, default nullptr). Cloned ASTs must be re-resolved.
  std::shared_ptr<VariableSymbol> ResolvedLoopVar;
  const VariableSymbol *getResolvedLoopVar() const { return ResolvedLoopVar.get(); }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitFor(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<ForExprAST>(
        Loc, VarName, CloneExprAST(Start.get()), CloneExprAST(End.get()),
        CloneExprAST(Step.get()), CloneExprAST(Body.get()));
    // Note: ResolvedLoopVar intentionally NOT copied. ✓
  }
};

// For-each loop over an array: for <value> in <iterable> { body }
//                          or:  for (<index>, <value>) in <iterable> { body }
// IndexVarName is empty for the single-var form. Desugars to a compiler-
// generated 0..Size-1 counted loop during Codegen (see ForEachExprAST::codegen()).
class ForEachExprAST : public ExprAST {
  std::string ValueVarName;
  std::string IndexVarName; // empty = single-var form
  SourceLocation Loc;
  std::unique_ptr<ExprAST> Iterable, Body;

public:
  ForEachExprAST(SourceLocation Loc, std::string ValueVarName,
                 std::string IndexVarName, std::unique_ptr<ExprAST> Iterable,
                 std::unique_ptr<ExprAST> Body)
      : ValueVarName(std::move(ValueVarName)),
        IndexVarName(std::move(IndexVarName)), Loc(Loc),
        Iterable(std::move(Iterable)), Body(std::move(Body)) {}

  const SourceLocation &getLoc() const { return Loc; }  ///< Location of the 'for' keyword.
  const std::string &getValueVarName() const { return ValueVarName; }
  const std::string &getIndexVarName() const { return IndexVarName; }
  bool hasIndexVar() const { return !IndexVarName.empty(); }
  ExprAST *getBody() const { return Body.get(); }
  const ExprAST *getIterable() const { return Iterable.get(); }

  /// Populated by the Resolver after successfully declaring the loop
  /// variable(s). Nulled by clone() semantics (fresh node, default nullptr).
  /// Cloned ASTs must be re-resolved.
  std::shared_ptr<VariableSymbol> ResolvedValueVar;
  std::shared_ptr<VariableSymbol> ResolvedIndexVar;
  const VariableSymbol *getResolvedValueVar() const { return ResolvedValueVar.get(); }
  const VariableSymbol *getResolvedIndexVar() const { return ResolvedIndexVar.get(); }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitForEach(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<ForEachExprAST>(
        Loc, ValueVarName, IndexVarName, CloneExprAST(Iterable.get()),
        CloneExprAST(Body.get()));
    // Note: ResolvedValueVar/ResolvedIndexVar intentionally NOT copied. ✓
  }
};

class BreakExprAST : public ExprAST {
  SourceLocation Loc;

public:
  BreakExprAST(SourceLocation Loc) : Loc(Loc) {}
  const SourceLocation &getLoc() const { return Loc; }
  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitBreak(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<BreakExprAST>(Loc);
  }
};

class ContinueExprAST : public ExprAST {
  SourceLocation Loc;

public:
  ContinueExprAST(SourceLocation Loc) : Loc(Loc) {}
  const SourceLocation &getLoc() const { return Loc; }
  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitContinue(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<ContinueExprAST>(Loc);
  }
};

// return expr;  or  return;
class ReturnExprAST : public ExprAST {
  std::unique_ptr<ExprAST> Val;
  SourceLocation Loc;

public:
  ReturnExprAST(SourceLocation Loc, std::unique_ptr<ExprAST> Val)
      : Loc(Loc), Val(std::move(Val)) {}

  const SourceLocation &getLoc() const { return Loc; }
  const ExprAST *getVal() const { return Val.get(); }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitReturn(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<ReturnExprAST>(Loc, CloneExprAST(Val.get()));
  }
};

// Which of the three `recover` forms this expression uses
// (docs/architecture/errors_and_recover.md): `recover produce value`, `recover
// run { ... }`, or `recover as err run { ... }`.
enum class RecoverKind {
    Produce, // recover produce <expr>
    Run,     // recover run { ... }
    AsRun,   // recover as <name> run { ... }
};

// `<expr> recover ...` -- handles the failure case of a `!T`-typed call
// expression at the call site (docs/architecture/errors_and_recover.md).
// `Wrapped` is the `!T` expression being recovered; `Block` (Run/AsRun) is a
// `BlockExprAST`, same as any other `{ ... }` body in this language
// (if/while/function bodies). `BoundName` (AsRun only) names the variable the
// failing error value is bound to inside `Block` (e.g. `err`).
class RecoverExprAST : public ExprAST {
    std::unique_ptr<ExprAST> Wrapped;
    RecoverKind              Kind;
    std::unique_ptr<ExprAST> ProduceExpr; // Kind == Produce
    std::string              BoundName;   // Kind == AsRun
    std::unique_ptr<ExprAST> Block;       // Kind == Run / AsRun (a BlockExprAST)
    SourceLocation           Loc;
public:
    RecoverExprAST(SourceLocation Loc, std::unique_ptr<ExprAST> Wrapped,
                   RecoverKind Kind, std::unique_ptr<ExprAST> ProduceExpr,
                   std::string BoundName, std::unique_ptr<ExprAST> Block)
        : Wrapped(std::move(Wrapped)), Kind(Kind),
          ProduceExpr(std::move(ProduceExpr)), BoundName(std::move(BoundName)),
          Block(std::move(Block)), Loc(Loc) {}

    ExprAST*             getWrapped()     const { return Wrapped.get(); }
    RecoverKind           getKind()        const { return Kind; }
    ExprAST*             getProduceExpr() const { return ProduceExpr.get(); }
    const std::string&    getBoundName()   const { return BoundName; }
    ExprAST*             getBlock()       const { return Block.get(); }
    const SourceLocation& getLoc()         const { return Loc; }

    /// The wrapped expression's success type T
    /// (docs/architecture/errors_and_recover.md), set by
    /// TypeChecker::visitRecoverExpr and read by Codegen -- needed there to
    /// know how to unbox the FallibleValue's boxed successBits slot back to T's
    /// real LLVM representation (Codegen can't derive this from the runtime
    /// value alone, since every `!T` lowers to the SAME FallibleValue struct
    /// regardless of T; see getFallibleValueType()'s doc comment in
    /// Codegen.cpp). Same cross-pass-communication pattern as
    /// ResolvedKind/ResolvedCandidates elsewhere in this file. Nulled by
    /// clone() (a cloned tree is stale until re-typechecked).
    TurfTypePtr ResolvedSuccessType;

    llvm::Value* codegen() override;
    void accept(ASTVisitor& V) override { V.visitRecoverExpr(*this); }
    std::unique_ptr<ExprAST> clone() const override {
        return std::make_unique<RecoverExprAST>(
            Loc, Wrapped ? Wrapped->clone() : nullptr, Kind,
            ProduceExpr ? ProduceExpr->clone() : nullptr, BoundName,
            Block ? Block->clone() : nullptr);
    }
};

// `defer <statement>;` -- registers Stmt to run when the ENCLOSING `{ }`
// BLOCK exits, by any path (fallthrough, return, break, continue) except a
// panic (TurfLang has no unwinding -- a panic aborts, LANG-003,
// ARCHITECTURE_OVERHAUL.md §8), in reverse (LIFO) order relative to any
// other `defer` registered earlier in the SAME block. Deliberately
// block-scoped, not function-scoped: a `defer` inside a loop body's `{ }`
// runs once per iteration, since that block's own basic blocks are simply
// re-executed by the loop's own back edge -- see Codegen.cpp's
// PendingDeferFrames/FlushPendingDefers for how this falls out of ordinary
// straight-line lowering with no extra runtime bookkeeping.
//
// `Stmt` is parsed via the ordinary ParsePrimaryBase() dispatch (like
// return/break/continue alongside it, ParserCore.cpp), so it can itself BE
// a return/break/continue -- that's syntactically legal and is exactly the
// shape the Resolver's dedicated check (FindEscapingControlFlow(),
// ResolverStmt.cpp) exists to reject: "a deferred statement may not return,
// break, or continue out of itself" (there is no way for such a jump to
// target only "the defer statement" and stop -- it always escapes to the
// enclosing function/loop, which is never what a reader expects here).
class DeferExprAST : public ExprAST {
  SourceLocation Loc;
  std::unique_ptr<ExprAST> Stmt;

public:
  DeferExprAST(SourceLocation Loc, std::unique_ptr<ExprAST> Stmt)
      : Loc(Loc), Stmt(std::move(Stmt)) {}

  const SourceLocation &getLoc() const { return Loc; }
  ExprAST *getStmt() const { return Stmt.get(); }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitDefer(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<DeferExprAST>(Loc, CloneExprAST(Stmt.get()));
  }
};

#endif
