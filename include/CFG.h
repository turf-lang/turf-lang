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
  void addStatement(ExprAST *Stmt) { Statements.push_back(Stmt); }
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
  TurfType ReturnType;      // Return type of the function
  SourceLocation FuncLoc;
  std::vector<std::unique_ptr<TurfBasicBlock>> Blocks; // All blocks (owned)
  TurfBasicBlock *EntryBlock; // First block (entry point)
  TurfBasicBlock *ExitBlock;  // Virtual exit block (all returns jump here)
  BlockID NextID;             // Next available block ID

public:
  explicit CFG(std::string FunctionName, TurfType ReturnType = TURF_VOID)
      : FunctionName(std::move(FunctionName)), ReturnType(ReturnType),
        EntryBlock(nullptr), ExitBlock(nullptr), NextID(0) {}

  // Accessors
  const std::string &getFunctionName() const { return FunctionName; }
  TurfType getReturnType() const { return ReturnType; }
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

public:
  CFGBuilder()
      : CurrentCFG(nullptr), CurrentBlock(nullptr), LoopContinueTarget(nullptr),
        LoopBreakTarget(nullptr), LastTerminatorLoc{0, 0} {}

  // Build CFG for a function
  std::unique_ptr<CFG> buildCFG(const std::string &FunctionName,
                                TurfType ReturnType, ExprAST *Body);

private:
  // Visit methods for different AST nodes
  void visitExpr(ExprAST *E);
  void visitBlock(ExprAST *E);
  void visitIf(ExprAST *E);
  void visitWhile(ExprAST *E);
  void visitReturn(ExprAST *E);
  void visitBreak(ExprAST *E);
  void visitContinue(ExprAST *E);
  void visitFor(ExprAST *E);

  // Utility methods
  TurfBasicBlock *createBlock(std::string Name);
  void setInsertPoint(TurfBasicBlock *BB) { CurrentBlock = BB; }
  void finishBlock(TerminatorKind Kind, SourceLocation Loc);
};

// Global CFG storage (for diagnostics during compilation)
extern std::vector<std::unique_ptr<CFG>> GlobalCFGs;

#endif
