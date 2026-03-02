#include "CFG.h"
#include "AST.h"
#include "Errors.h"
#include <algorithm>
#include <iostream>
#include <queue>

// Global CFG storage
std::vector<std::unique_ptr<CFG>> GlobalCFGs;

// ============================================================================
// CFG Implementation
// ============================================================================

TurfBasicBlock *CFG::createBlock(std::string Name) {
  auto BB = std::make_unique<TurfBasicBlock>(NextID++, std::move(Name));
  TurfBasicBlock *Ptr = BB.get();
  Blocks.push_back(std::move(BB));
  return Ptr;
}

void CFG::addEdge(TurfBasicBlock *From, TurfBasicBlock *To) {
  if (!From || !To)
    return;
  From->addSuccessor(To);
  To->addPredecessor(From);
}

void CFG::computeReachability() {
  // Reset all blocks to unreachable
  for (auto &BB : Blocks) {
    BB->setReachable(false);
  }

  if (!EntryBlock)
    return;

  // BFS from entry block
  std::queue<TurfBasicBlock *> Worklist;
  Worklist.push(EntryBlock);
  EntryBlock->setReachable(true);

  while (!Worklist.empty()) {
    TurfBasicBlock *Current = Worklist.front();
    Worklist.pop();

    for (TurfBasicBlock *Successor : Current->getSuccessors()) {
      if (!Successor->isReachable()) {
        Successor->setReachable(true);
        Worklist.push(Successor);
      }
    }
  }
}

std::vector<TurfBasicBlock *> CFG::getUnreachableBlocks() const {
  std::vector<TurfBasicBlock *> Unreachable;
  for (auto &BB : Blocks) {
    // Skip the virtual exit block
    if (BB.get() == ExitBlock)
      continue;
    if (!BB->isReachable() && !BB->isEmpty()) {
      Unreachable.push_back(BB.get());
    }
  }
  return Unreachable;
}

bool CFG::allPathsReturn() const {
  if (!EntryBlock || !ExitBlock)
    return false;

  // Check if the exit block has any incoming edges from non-return terminators
  // A proper return means all paths that reach exit have explicit return
  // statements
  for (TurfBasicBlock *Pred : ExitBlock->getPredecessors()) {
    if (Pred->isReachable() &&
        Pred->getTerminator() != TerminatorKind::Return) {
      // Found a path to exit that doesn't end with explicit return
      return false;
    }
  }

  return true;
}

std::vector<TurfBasicBlock *> CFG::getDeadBranches() const {
  std::vector<TurfBasicBlock *> Dead;

  for (auto &BB : Blocks) {
    if (!BB->isReachable() && BB.get() != ExitBlock) {
      Dead.push_back(BB.get());
    }
  }

  return Dead;
}

void CFG::reportFlowDiagnostics() const {
  // Report unreachable code
  auto Unreachable = getUnreachableBlocks();
  for (TurfBasicBlock *BB : Unreachable) {
    if (!BB->getStatements().empty()) {
      // Report on first statement in unreachable block
      std::cerr << "Warning: Unreachable code in block '" << BB->getName()
                << "' of function '" << FunctionName << "'\n";
    }
  }

  // Report missing return paths - ERROR for non-void functions, ignore for void
  if (ReturnType != TURF_VOID && !allPathsReturn()) {
    MissingReturnError(FunctionName).raise();
  }

  // Report dead branches
  auto Dead = getDeadBranches();
  for (TurfBasicBlock *BB : Dead) {
    if (BB->getTerminator() == TerminatorKind::Branch) {
      SourceLocation Loc = BB->getTerminatorLoc();
      std::cerr << "Warning: Dead branch in function '" << FunctionName
                << "' at line " << Loc.Line << "\n";
    }
  }
}

void CFG::print() const {
  std::cout << "CFG for function '" << FunctionName << "':\n";
  std::cout << "  Entry: " << (EntryBlock ? EntryBlock->getName() : "none")
            << "\n";
  std::cout << "  Exit: " << (ExitBlock ? ExitBlock->getName() : "none")
            << "\n";
  std::cout << "  Blocks:\n";

  for (auto &BB : Blocks) {
    std::cout << "    BB" << BB->getID() << " (" << BB->getName() << ")";
    std::cout << " [" << (BB->isReachable() ? "reachable" : "unreachable")
              << "]";
    std::cout << " - " << BB->getStatements().size() << " statements\n";

    if (!BB->getPredecessors().empty()) {
      std::cout << "      Predecessors: ";
      for (TurfBasicBlock *Pred : BB->getPredecessors()) {
        std::cout << "BB" << Pred->getID() << " ";
      }
      std::cout << "\n";
    }

    if (!BB->getSuccessors().empty()) {
      std::cout << "      Successors: ";
      for (TurfBasicBlock *Succ : BB->getSuccessors()) {
        std::cout << "BB" << Succ->getID() << " ";
      }
      std::cout << "\n";
    }

    const char *TermStr = "none";
    switch (BB->getTerminator()) {
    case TerminatorKind::Return:
      TermStr = "return";
      break;
    case TerminatorKind::Break:
      TermStr = "break";
      break;
    case TerminatorKind::Continue:
      TermStr = "continue";
      break;
    case TerminatorKind::Branch:
      TermStr = "branch";
      break;
    case TerminatorKind::Unconditional:
      TermStr = "jump";
      break;
    case TerminatorKind::None:
      TermStr = "fallthrough";
      break;
    }
    std::cout << "      Terminator: " << TermStr << "\n";
  }
  std::cout << "\n";
}

// ============================================================================
// CFGBuilder Implementation
// ============================================================================

std::unique_ptr<CFG> CFGBuilder::buildCFG(const std::string &FunctionName,
                                          TurfType ReturnType, ExprAST *Body) {
  auto CFGPtr = std::make_unique<CFG>(FunctionName, ReturnType);
  CurrentCFG = CFGPtr.get();

  // Create entry and exit blocks
  TurfBasicBlock *Entry = CurrentCFG->createBlock("entry");
  TurfBasicBlock *Exit = CurrentCFG->createBlock("exit");
  CurrentCFG->setEntryBlock(Entry);
  CurrentCFG->setExitBlock(Exit);

  // Start building from entry
  setInsertPoint(Entry);

  // Visit the function body
  visitExpr(Body);

  // If current block doesn't have a terminator, link to exit
  if (CurrentBlock && !CurrentBlock->hasExplicitTerminator()) {
    CurrentCFG->addEdge(CurrentBlock, Exit);
  }

  // Compute reachability
  CurrentCFG->computeReachability();

  return CFGPtr;
}

TurfBasicBlock *CFGBuilder::createBlock(std::string Name) {
  return CurrentCFG->createBlock(std::move(Name));
}

void CFGBuilder::finishBlock(TerminatorKind Kind, SourceLocation Loc) {
  if (CurrentBlock) {
    CurrentBlock->setTerminator(Kind, Loc);
  }
}

void CFGBuilder::visitExpr(ExprAST *E) {
  if (!E)
    return;

  // Check if current block already has an explicit terminator or is null
  // If so, statements after it are unreachable - report error immediately
  if (!CurrentBlock || CurrentBlock->hasExplicitTerminator()) {
    // Report the error using the location of the last terminator we encountered
    StatementAfterTerminatorError(LastTerminatorLoc, "return/break/continue")
        .raise();
    return;
  }

  // Determine expression type and dispatch
  if (dynamic_cast<BlockExprAST *>(E)) {
    visitBlock(E);
  } else if (dynamic_cast<IfExprAST *>(E)) {
    visitIf(E);
  } else if (dynamic_cast<WhileExprAST *>(E)) {
    visitWhile(E);
  } else if (dynamic_cast<ForExprAST *>(E)) {
    visitFor(E);
  } else if (dynamic_cast<ReturnExprAST *>(E)) {
    visitReturn(E);
  } else if (dynamic_cast<BreakExprAST *>(E)) {
    visitBreak(E);
  } else if (dynamic_cast<ContinueExprAST *>(E)) {
    visitContinue(E);
  } else {
    // Regular statement - add to current block
    CurrentBlock->addStatement(E);
  }
}

void CFGBuilder::visitBlock(ExprAST *E) {
  auto *Block = dynamic_cast<BlockExprAST *>(E);
  if (!Block)
    return;

  // Visit each statement in the block sequentially
  for (const auto &Expr : Block->getExpressions()) {
    visitExpr(Expr.get());
    // Continue processing all statements - visitExpr will handle unreachable
    // code
  }
}

void CFGBuilder::visitIf(ExprAST *E) {
  auto *If = dynamic_cast<IfExprAST *>(E);
  if (!If)
    return;

  TurfBasicBlock *IfEntry = CurrentBlock;
  TurfBasicBlock *ThenBB = createBlock("if.then");
  TurfBasicBlock *MergeBB = createBlock("if.end");

  // Entry branches to then
  CurrentCFG->addEdge(IfEntry, ThenBB);
  IfEntry->setTerminator(TerminatorKind::Branch, SourceLocation{});

  // Build then branch
  setInsertPoint(ThenBB);
  if (If->getThen()) {
    visitExpr(If->getThen());
  }
  bool ThenTerminates = !CurrentBlock;
  if (!ThenTerminates) {
    CurrentCFG->addEdge(CurrentBlock, MergeBB);
  }

  // Build else branch (only if it exists)
  bool ElseTerminates = false;
  if (If->getElse()) {
    TurfBasicBlock *ElseBB = createBlock("if.else");
    CurrentCFG->addEdge(IfEntry, ElseBB);
    setInsertPoint(ElseBB);
    visitExpr(If->getElse());
    ElseTerminates = !CurrentBlock;
    if (!ElseTerminates) {
      CurrentCFG->addEdge(CurrentBlock, MergeBB);
    }
  } else {
    // No else: if condition is false, fall through to merge
    CurrentCFG->addEdge(IfEntry, MergeBB);
  }

  // Continue from merge point only if at least one branch doesn't terminate
  if (ThenTerminates && ElseTerminates) {
    setInsertPoint(nullptr);
  } else {
    setInsertPoint(MergeBB);
  }
}

void CFGBuilder::visitWhile(ExprAST *E) {
  auto *While = dynamic_cast<WhileExprAST *>(E);
  if (!While)
    return;

  TurfBasicBlock *WhileEntry = CurrentBlock;

  // Create blocks
  TurfBasicBlock *CondBB = createBlock("while.cond");
  TurfBasicBlock *BodyBB = createBlock("while.body");
  TurfBasicBlock *ExitBB = createBlock("while.end");

  // Entry jumps to condition
  CurrentCFG->addEdge(WhileEntry, CondBB);
  WhileEntry->setTerminator(TerminatorKind::Unconditional, SourceLocation{});

  // Condition branches to body or exit
  CurrentCFG->addEdge(CondBB, BodyBB);
  CurrentCFG->addEdge(CondBB, ExitBB);
  CondBB->setTerminator(TerminatorKind::Branch, SourceLocation{});

  // Save loop targets for break/continue
  TurfBasicBlock *SavedBreak = LoopBreakTarget;
  TurfBasicBlock *SavedContinue = LoopContinueTarget;
  LoopBreakTarget = ExitBB;
  LoopContinueTarget = CondBB;

  // Build body
  setInsertPoint(BodyBB);
  if (While->getBody()) {
    visitExpr(While->getBody());
  }
  // Loop back to condition if no explicit terminator
  if (CurrentBlock && !CurrentBlock->hasExplicitTerminator()) {
    CurrentCFG->addEdge(CurrentBlock, CondBB);
  }

  // Restore loop targets
  LoopBreakTarget = SavedBreak;
  LoopContinueTarget = SavedContinue;

  // Continue from exit
  setInsertPoint(ExitBB);
}

void CFGBuilder::visitFor(ExprAST *E) {
  auto *For = dynamic_cast<ForExprAST *>(E);
  if (!For)
    return;

  TurfBasicBlock *ForEntry = CurrentBlock;

  // Create blocks
  TurfBasicBlock *CondBB = createBlock("for.cond");
  TurfBasicBlock *BodyBB = createBlock("for.body");
  TurfBasicBlock *StepBB = createBlock("for.step");
  TurfBasicBlock *ExitBB = createBlock("for.end");

  // Entry jumps to condition
  CurrentCFG->addEdge(ForEntry, CondBB);
  ForEntry->setTerminator(TerminatorKind::Unconditional, SourceLocation{});

  // Condition branches to body or exit
  CurrentCFG->addEdge(CondBB, BodyBB);
  CurrentCFG->addEdge(CondBB, ExitBB);
  CondBB->setTerminator(TerminatorKind::Branch, SourceLocation{});

  // Save loop targets for break/continue
  TurfBasicBlock *SavedBreak = LoopBreakTarget;
  TurfBasicBlock *SavedContinue = LoopContinueTarget;
  LoopBreakTarget = ExitBB;
  LoopContinueTarget = StepBB; // continue in for-loop goes to step, not cond

  // Build body
  setInsertPoint(BodyBB);
  if (For->getBody()) {
    visitExpr(For->getBody());
  }
  // Fall through to step if no explicit terminator
  if (CurrentBlock && !CurrentBlock->hasExplicitTerminator()) {
    CurrentCFG->addEdge(CurrentBlock, StepBB);
  }

  // Step block loops back to condition
  CurrentCFG->addEdge(StepBB, CondBB);
  StepBB->setTerminator(TerminatorKind::Unconditional, SourceLocation{});

  // Restore loop targets
  LoopBreakTarget = SavedBreak;
  LoopContinueTarget = SavedContinue;

  // Continue from exit
  setInsertPoint(ExitBB);
}

void CFGBuilder::visitReturn(ExprAST *E) {
  auto *Ret = dynamic_cast<ReturnExprAST *>(E);
  if (!Ret || !CurrentBlock)
    return;

  CurrentBlock->addStatement(Ret);
  LastTerminatorLoc = Ret->getLoc();
  CurrentBlock->setTerminator(TerminatorKind::Return, LastTerminatorLoc);

  // Link to exit block
  CurrentCFG->addEdge(CurrentBlock, CurrentCFG->getExitBlock());

  // No further statements in this block
  CurrentBlock = nullptr;
}

void CFGBuilder::visitBreak(ExprAST *E) {
  auto *Brk = dynamic_cast<BreakExprAST *>(E);
  if (!Brk || !CurrentBlock)
    return;

  if (!LoopBreakTarget) {
    std::cerr << "Error: 'break' outside of loop\n";
    return;
  }

  CurrentBlock->addStatement(Brk);
  LastTerminatorLoc = Brk->getLoc();
  CurrentBlock->setTerminator(TerminatorKind::Break, LastTerminatorLoc);
  CurrentCFG->addEdge(CurrentBlock, LoopBreakTarget);

  // No further statements in this block
  CurrentBlock = nullptr;
}

void CFGBuilder::visitContinue(ExprAST *E) {
  auto *Cont = dynamic_cast<ContinueExprAST *>(E);
  if (!Cont || !CurrentBlock)
    return;

  if (!LoopContinueTarget) {
    std::cerr << "Error: 'continue' outside of loop\n";
    return;
  }

  CurrentBlock->addStatement(Cont);
  LastTerminatorLoc = Cont->getLoc();
  CurrentBlock->setTerminator(TerminatorKind::Continue, LastTerminatorLoc);
  CurrentCFG->addEdge(CurrentBlock, LoopContinueTarget);

  // No further statements in this block
  CurrentBlock = nullptr;
}
