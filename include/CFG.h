#ifndef CFG_H
#define CFG_H

#include "Lexer.h"
#include "Types.h"
#include <memory>
#include <set>
#include <string>
#include <vector>

// Forward declarations
class ExprAST;

// Unique identifier for each basic block
using BlockID = size_t;

// Terminator type for a basic block
enum class TerminatorKind {
  None,         // No explicit terminator (fallthrough)
  Return,       // return statement
  Break,        // break statement
  Continue,     // continue statement
  Branch,       // Conditional branch (if)
  Unconditional // Unconditional jump (end of then/else, loop body)
};

// Represents a single basic block in the control flow graph
// A basic block is a maximal sequence of statements with:
// - Single entry point (at the beginning)
// - Single exit point (at the end)
// - No branches except at the end
class TurfBasicBlock {
private:
  BlockID ID;       // Unique identifier
  std::string Name; // Human-readable name (e.g., "entry", "if.then")
  std::vector<ExprAST *> Statements; // Statements in this block (non-owning)
  // Statements plus, interleaved where they run, each `defer`'s statement at
  // the exits of its scope (CFGBuilder's DeferScopes). Read by EscapePass
  // and LivenessAnalysisPass; every other pass still sees a `defer` where it
  // is written.
  std::vector<ExprAST *> StatementsWithDeferredRuns;
  std::vector<TurfBasicBlock *> Predecessors; // Blocks that can jump here
  std::vector<TurfBasicBlock *> Successors;   // Blocks this can jump to
  TerminatorKind Terminator;                  // How this block ends
  SourceLocation TerminatorLoc; // Location of terminator statement
  bool IsReachable;             // Whether this block is reachable from entry

public:
  TurfBasicBlock(BlockID ID, std::string Name)
      : ID(ID), Name(std::move(Name)), Terminator(TerminatorKind::None),
        IsReachable(false) {}

  // Accessors
  BlockID getID() const { return ID; }
  const std::string &getName() const { return Name; }
  const std::vector<ExprAST *> &getStatements() const { return Statements; }
  const std::vector<ExprAST *> &getStatementsWithDeferredRuns() const {
    return StatementsWithDeferredRuns;
  }
  const std::vector<TurfBasicBlock *> &getPredecessors() const {
    return Predecessors;
  }
  const std::vector<TurfBasicBlock *> &getSuccessors() const {
    return Successors;
  }
  TerminatorKind getTerminator() const { return Terminator; }
  const SourceLocation &getTerminatorLoc() const { return TerminatorLoc; }
  bool isReachable() const { return IsReachable; }
  bool isEmpty() const { return Statements.empty(); }
  
  // Mutators
  void addStatement(ExprAST *Stmt) {
    Statements.push_back(Stmt);
    StatementsWithDeferredRuns.push_back(Stmt);
  }
  // A deferred statement running here, at an exit of its scope.
  void addDeferredRun(ExprAST *Stmt) { StatementsWithDeferredRuns.push_back(Stmt); }
  void addPredecessor(TurfBasicBlock *BB) { Predecessors.push_back(BB); }
  void addSuccessor(TurfBasicBlock *BB) { Successors.push_back(BB); }
  void setTerminator(TerminatorKind Kind, SourceLocation Loc) {
    Terminator = Kind;
    TerminatorLoc = Loc;
  }
  void setReachable(bool R) { IsReachable = R; }

  // Returns true if this block has a terminator that prevents fallthrough
  bool hasExplicitTerminator() const {
    return Terminator == TerminatorKind::Return ||
           Terminator == TerminatorKind::Break ||
           Terminator == TerminatorKind::Continue;
  }
};

// Represents the control flow graph for a function
class CFG {
private:
  std::string FunctionName; // Name of the function this CFG represents
  // Mangled identity of this function (e.g. "turf_assert_bool_string"),
  // distinct from FunctionName ("assert") whenever overloading is in play.
  // Empty when the caller didn't supply one (buildCFG's mangled-name param
  // defaults to ""). Exists solely so allPathsRecurse()/blockContainsSelfCall
  // can tell "this function calls ITSELF" apart from "this function calls a
  // same-named sibling overload" -- see that function's doc comment for the
  // false-positive this fixes. FunctionName stays the plain, user-facing
  // name for every diagnostic message; only self-call detection needs the
  // mangled form.
  std::string MangledFunctionName;
  TurfTypePtr ReturnType;   // Return type of the function
  SourceLocation FuncLoc;
  std::vector<std::unique_ptr<TurfBasicBlock>> Blocks; // All blocks (owned)
  TurfBasicBlock *EntryBlock; // First block (entry point)
  TurfBasicBlock *ExitBlock;  // Virtual exit block (all returns jump here)
  BlockID NextID;             // Next available block ID

public:
  // FuncLoc defaults to {0, 0} (not left uninitialized): SourceLocation is a
  // plain aggregate with no default member initializers, so before this
  // explicit default, a CFG built via the single-arg constructor (still
  // used by anything that doesn't pass a location) left FuncLoc containing
  // whatever garbage happened to be on the stack. reportFlowDiagnostics()
  // passes FuncLoc straight into MissingReturnError() with no validation --
  // confirmed to print things like "at line 1986079736, col 30179" instead
  // of the function's real declaration site.
  explicit CFG(std::string FunctionName, TurfTypePtr ReturnType = TurfVoid(),
              SourceLocation FuncLoc = {0, 0},
              std::string MangledFunctionName = "")
      : FunctionName(std::move(FunctionName)),
        MangledFunctionName(std::move(MangledFunctionName)),
        ReturnType(std::move(ReturnType)),
        FuncLoc(FuncLoc), EntryBlock(nullptr), ExitBlock(nullptr), NextID(0) {}

  // Accessors
  const std::string &getFunctionName() const { return FunctionName; }
  const std::string &getMangledFunctionName() const { return MangledFunctionName; }
  const TurfType *getReturnType() const { return ReturnType.get(); }
  TurfTypePtr getReturnTypePtr() const { return ReturnType; }
  TurfBasicBlock *getEntryBlock() const { return EntryBlock; }
  TurfBasicBlock *getExitBlock() const { return ExitBlock; }
  const std::vector<std::unique_ptr<TurfBasicBlock>> &getBlocks() const {
    return Blocks;
  }

  // Block creation
  TurfBasicBlock *createBlock(std::string Name);

  // Set entry/exit blocks
  void setEntryBlock(TurfBasicBlock *BB) { EntryBlock = BB; }
  void setExitBlock(TurfBasicBlock *BB) { ExitBlock = BB; }

  // Link two blocks (adds edge BB1 -> BB2)
  void addEdge(TurfBasicBlock *From, TurfBasicBlock *To);

  // Analysis passes
  void computeReachability(); // Mark reachable blocks from entry
  std::vector<TurfBasicBlock *>
  getUnreachableBlocks() const; // Get unreachable blocks
  bool allPathsReturn() const;  // Check if all paths end in return
  std::vector<TurfBasicBlock *>
  getDeadBranches() const; // Get dead conditional branches

  // CFG Phase 1 analysis passes (Issue #185)
  // M = E - N + 2P (P=1 per function). Values above 15 indicate functions
  // that are hard to read, test, and maintain.
  int calculateCyclomaticComplexity() const;
  // True when the CFG contains a back-edge cycle where no block in the cycle
  // has a Break/Return terminator AND no block has a successor outside the
  // cycle — i.e. the loop cannot exit by any path.
  bool hasUnbreakableLoop() const;

  // CFG Phase 2 (Issue #186): Infinite Recursion Detection.
  // Returns true when every path from EntryBlock to ExitBlock passes through
  // at least one block that contains a direct call to `FunctionName` — i.e.
  // no base case exists. Uses DFS with a visited-set to handle CFG cycles
  // (loops within a recursive function) without infinite traversal.
  bool allPathsRecurse() const;

  // Diagnostics
  void reportFlowDiagnostics() const;

  // Utility
  void print() const; // Debug print CFG structure
};

// Builder for constructing CFGs from AST
class CFGBuilder {
private:
  CFG *CurrentCFG;              // CFG being built
  TurfBasicBlock *CurrentBlock; // Current insertion point
  TurfBasicBlock
      *LoopContinueTarget; // Target for 'continue' (innermost loop header)
  TurfBasicBlock *LoopBreakTarget;  // Target for 'break' (innermost loop exit)
  SourceLocation LastTerminatorLoc; // Location of last terminator encountered
  // One entry per `{ }` being built, innermost last: the statements of the
  // `defer`s written directly in it, in order. Run (LIFO) at each exit of
  // that scope -- see emitDeferredRuns().
  std::vector<std::vector<ExprAST *>> DeferScopes;
  // DeferScopes.size() at the innermost loop's body: `break`/`continue`
  // leave the scopes above it.
  size_t LoopDeferDepth = 0;

public:
  CFGBuilder()
      : CurrentCFG(nullptr), CurrentBlock(nullptr), LoopContinueTarget(nullptr),
        LoopBreakTarget(nullptr), LastTerminatorLoc{0, 0} {}

  // Build CFG for a function. MangledFunctionName, when supplied, lets
  // allPathsRecurse() distinguish real self-recursion from a call to a
  // same-named sibling overload (see CFG::MangledFunctionName's doc comment).
  std::unique_ptr<CFG> buildCFG(const std::string &FunctionName,
                                TurfTypePtr ReturnType, ExprAST *Body,
                                SourceLocation FuncLoc = {0, 0},
                                const std::string &MangledFunctionName = "");

  // A CFG of one block nested inside a function (NullabilityPass analyses a
  // `recover run { }` block this way). A `break`/`continue` aimed at a loop
  // outside the block leaves the region: an edge to its exit, like `return`,
  // instead of the "outside of a loop" error a whole function would get.
  std::unique_ptr<CFG> buildRegionCFG(const std::string &Name, ExprAST *Body,
                                      SourceLocation Loc);

private:
  // Visit methods for different AST nodes
  void visitExpr(ExprAST *E);
  void visitBlock(ExprAST *E);
  void visitIf(ExprAST *E);
  void visitWhile(ExprAST *E);
  void visitDoWhile(ExprAST *E);
  void visitReturn(ExprAST *E);
  void visitBreak(ExprAST *E);
  void visitContinue(ExprAST *E);
  void visitCompare(ExprAST *E);
  void visitFor(ExprAST *E);
  void visitForEach(ExprAST *E);

  // Records, in the current block, the deferred statements of every scope
  // from the innermost down to (not including) depth Depth, LIFO.
  void emitDeferredRuns(size_t Depth);

  // Utility methods
  TurfBasicBlock *createBlock(std::string Name);
  void setInsertPoint(TurfBasicBlock *BB) { CurrentBlock = BB; }
  void finishBlock(TerminatorKind Kind, SourceLocation Loc);
};

// Global CFG storage (for diagnostics during compilation)
extern std::vector<std::unique_ptr<CFG>> GlobalCFGs;
void ClearGlobalCFGs();

#endif
