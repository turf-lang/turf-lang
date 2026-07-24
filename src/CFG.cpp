/**
 * @file CFG.cpp
 * @brief Control Flow Graph construction and analysis.
 * @details Implements TurfBasicBlock, CFG, and CFGBuilder to model the control
 * flow of TurfLang programs. Provides reachability analysis, dead code
 * detection, and return path validation.
 *
 * @author a2ys
 * @author AaravTandon24
 * @author Karan
 *
 * @copyright Copyright (c) 2026 TurfLang contributors. Licensed under the
 * Apache License.
 * @version 1.0.0
 * @date 2026-05-25
 */

#include "CFG.h"
#include "AST.h"
#include "Errors.h"
#include <algorithm>
#include <iostream>
#include <queue>

// Global CFG storage
std::vector<std::unique_ptr<CFG>> GlobalCFGs;
void ClearGlobalCFGs() { GlobalCFGs.clear(); }

/**
 * @brief Creates a new basic block within the CFG.
 *
 * @param Name The debugging name of the basic block (e.g., "entry", "if.then").
 * @return TurfBasicBlock* A pointer to the newly created basic block.
 */
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
  if (!ReturnType->isVoid() && !allPathsReturn()) {
    MissingReturnError(FuncLoc, FunctionName).raise();
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

/**
 * @brief Constructs a Control Flow Graph from an Abstract Syntax Tree.
 *
 * Visits the function body AST recursively to build a graph of basic blocks
 * connected by control flow edges (branches, loops, returns).
 *
 * @param FunctionName The name of the function being analyzed.
 * @param ReturnType The declared return type of the function.
 * @param Body The root AST node of the function body.
 * @return std::unique_ptr<CFG> The completed Control Flow Graph.
 */
std::unique_ptr<CFG> CFGBuilder::buildCFG(const std::string &FunctionName,
                                          TurfTypePtr ReturnType,
                                          ExprAST *Body,
                                          SourceLocation FuncLoc) {
  auto CFGPtr = std::make_unique<CFG>(FunctionName, std::move(ReturnType), FuncLoc);
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
  } else if (dynamic_cast<CompareExprAST *>(E)) {
    visitCompare(E);
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
  TurfBasicBlock *MergeBB = createBlock("if.end");

  const auto &Branches = If->getBranches();
  bool AllTerminate = true;

  TurfBasicBlock *CurrentCondBlock = IfEntry;

  for (size_t i = 0; i < Branches.size(); ++i) {
    TurfBasicBlock *ThenBB = createBlock(i == 0 ? "if.then" : "elseif.then");
    TurfBasicBlock *NextBB =
        (i + 1 < Branches.size())
            ? createBlock("elseif.cond")
            : (If->getElseBody() ? createBlock("if.else") : MergeBB);

    // Add the condition expression as a statement so the DAP can detect
    // reads of uninitialized variables used as if/elseif conditions.
    if (Branches[i].Cond)
      CurrentCondBlock->addStatement(Branches[i].Cond.get());

    CurrentCFG->addEdge(CurrentCondBlock, ThenBB);
    CurrentCFG->addEdge(CurrentCondBlock, NextBB);
    CurrentCondBlock->setTerminator(TerminatorKind::Branch, Branches[i].Loc);

    // Build then/elseif body
    setInsertPoint(ThenBB);
    visitExpr(Branches[i].Body.get());
    if (CurrentBlock && !CurrentBlock->hasExplicitTerminator()) {
      CurrentCFG->addEdge(CurrentBlock, MergeBB);
    } else {
      // This branch terminates (return/break/continue)
    }

    bool BranchTerminates =
        !CurrentBlock || CurrentBlock->hasExplicitTerminator();
    if (!BranchTerminates)
      AllTerminate = false;

    CurrentCondBlock = NextBB;
  }

  // Handle else
  if (If->getElseBody()) {
    setInsertPoint(CurrentCondBlock); // This is the else block
    visitExpr(If->getElseBody());
    bool ElseTerminates =
        !CurrentBlock || CurrentBlock->hasExplicitTerminator();
    if (!ElseTerminates) {
      CurrentCFG->addEdge(CurrentBlock, MergeBB);
      AllTerminate = false;
    }
  } else {
    // No else: the last NextBB is already MergeBB
    AllTerminate = false; // no else means the "fall-through" path exists
  }

  if (AllTerminate) {
    setInsertPoint(nullptr);
  } else {
    setInsertPoint(MergeBB);
  }
}

/**
 * @brief Model a `compare { matches ...: ... done ... otherwise: ... }`
 *        construct's control flow, mirroring visitIf()'s per-branch/merge
 *        structure.
 *
 * Before this existed, CompareExprAST had no case in visitExpr()'s dispatch
 * at all, so it fell into the generic "regular statement, add to current
 * block" branch -- an opaque, non-terminating statement as far as the CFG
 * was concerned, regardless of what was actually inside it. This produced a
 * confirmed, reproducible FALSE "missing return" error for a function whose
 * ENTIRE body is one exhaustive `compare` with a `return` in every arm and
 * no trailing return statement.
 *
 * `compare` is NEVER treated as a hard CFG terminator by itself, even when
 * every arm (and `otherwise`, if present) provably terminates -- i.e.
 * `AllTerminate` is always false here, unlike visitIf()'s equivalent logic.
 * Two earlier, more "precise" versions of this function were tried and both
 * had to be walked back:
 *   1. Treating a verified-exhaustive enum compare (`Cmp->SubjectIsEnum`,
 *      which TypeChecker only sets once every variant is covered) as
 *      unconditionally terminating when there's no `otherwise`.
 *   2. Treating ANY compare where every arm AND an explicit `otherwise`
 *      both terminate as unconditionally terminating.
 * Both are logically sound reasoning, but BOTH broke a widely-used,
 * clearly intentional idiom throughout this codebase's own test suite: a
 * compare that already covers every case (via exhaustive enum matching or
 * an explicit `otherwise`) with a `return` in every arm, immediately
 * followed by a redundant "belt and suspenders" trailing `return` that's
 * never actually reachable. Treating the compare as a hard terminator
 * turned that trailing return into a hard "unreachable code" error --
 * confirmed to break at least 7 existing tests each time. Since the
 * codebase clearly prefers "always allow a trailing statement after
 * compare" over precise unreachable-code detection for this construct,
 * this stays maximally conservative: entering/exiting via break/continue/
 * return *inside* an arm is still modeled correctly (so code INSIDE later
 * arms, or after an if-with-break inside an arm, etc., is unaffected), but
 * the compare's own arms/otherwise never influence whether code AFTER the
 * whole compare block is considered reachable.
 *
 * `Cmp->SubjectIsEnum` DOES still matter for one thing: whether the "no
 * arm matched" fallthrough EDGE itself is added when there's no explicit
 * `otherwise` (see below). That edge doesn't affect reachability -- nothing
 * downstream treats it specially for that purpose -- but it DOES feed into
 * DefiniteAssignmentPass's dataflow, which computes "is X assigned here" as
 * the intersection of "assigned at end" across every predecessor of a
 * block. Adding that edge unconditionally (regardless of exhaustiveness)
 * was also tried and reverted: a spurious edge representing "no arm ran at
 * all" is a predecessor where nothing is assigned, which drags the
 * intersection down to "unassigned" even when every real arm assigns the
 * same variable -- producing a false "used before it has a value" error
 * for a variable genuinely assigned on every reachable path through an
 * exhaustive compare. So: the edge is omitted when `SubjectIsEnum` is true
 * (TypeChecker already guarantees every variant is covered by some arm),
 * added whenever it's false and there's no `otherwise`.
 */
void CFGBuilder::visitCompare(ExprAST *E) {
  auto *Cmp = dynamic_cast<CompareExprAST *>(E);
  if (!Cmp)
    return;

  TurfBasicBlock *CmpEntry = CurrentBlock;
  TurfBasicBlock *MergeBB = createBlock("compare.end");

  // Add the subject expression as a statement so the DAP can detect reads
  // of uninitialized variables used as the compare subject.
  if (Cmp->getSubject())
    CmpEntry->addStatement(const_cast<ExprAST *>(Cmp->getSubject()));
  CmpEntry->setTerminator(TerminatorKind::Branch, Cmp->getLoc());

  for (const auto &Arm : Cmp->getArms()) {
    TurfBasicBlock *ArmBB = createBlock("compare.arm");
    CurrentCFG->addEdge(CmpEntry, ArmBB);

    setInsertPoint(ArmBB);
    visitExpr(Arm.Body.get());
    if (CurrentBlock && !CurrentBlock->hasExplicitTerminator())
      CurrentCFG->addEdge(CurrentBlock, MergeBB);
  }

  if (Cmp->getOtherBody()) {
    TurfBasicBlock *OtherBB = createBlock("compare.otherwise");
    CurrentCFG->addEdge(CmpEntry, OtherBB);

    setInsertPoint(OtherBB);
    visitExpr(Cmp->getOtherBodyMut());
    if (CurrentBlock && !CurrentBlock->hasExplicitTerminator())
      CurrentCFG->addEdge(CurrentBlock, MergeBB);
  } else if (!Cmp->SubjectIsEnum) {
    // No `otherwise`, and not a verified-exhaustive enum compare (the only
    // other way TypeChecker allows omitting `otherwise` -- see its
    // "Exhaustiveness gate for Enums"): the "nothing matched" fallthrough
    // path is genuinely possible and reaches MergeBB directly without
    // running any arm.
    //
    // Deliberately NOT added when SubjectIsEnum is true: doing so
    // unconditionally (regardless of exhaustiveness) was tried and reverted
    // -- it doesn't affect reachability/unreachable-code detection (see
    // this function's doc comment, `setInsertPoint(MergeBB)` runs either
    // way below), but it DOES corrupt DefiniteAssignmentPass's dataflow:
    // that pass computes "is X assigned at this point" as the INTERSECTION
    // of "assigned at end" across every predecessor block, and a spurious
    // edge to MergeBB directly from CmpEntry -- with the compare's actual
    // arms never having run at all -- is a predecessor where nothing is
    // assigned. That drags the intersection down to
    // "unassigned" even when every real arm assigns the same variable,
    // producing a false "used before it has a value" error for a variable
    // that's genuinely assigned on every reachable path through an
    // exhaustive compare.
    CurrentCFG->addEdge(CmpEntry, MergeBB);
  }
  // else: SubjectIsEnum && no otherwise -- no direct CmpEntry->MergeBB edge
  // is added; MergeBB's only predecessors are the arms themselves (or none,
  // if every arm terminates -- in which case MergeBB is simply unreachable,
  // which is fine: see this function's doc comment for why unreachable
  // trailing code is never flagged as an error for this construct anyway).

  // See this function's doc comment: `compare` never counts as a hard
  // terminator on its own, so execution always continues from MergeBB,
  // regardless of whether every arm/otherwise happened to terminate.
  setInsertPoint(MergeBB);
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
  WhileEntry->setTerminator(TerminatorKind::Unconditional, While->getLoc());

  // Condition branches to body or exit
  // Add the condition expression as a statement so the DAP can detect reads
  // of uninitialized variables used as while-loop conditions.
  if (While->getCond())
    CondBB->addStatement(const_cast<ExprAST *>(While->getCond()));
  CurrentCFG->addEdge(CondBB, BodyBB);
  CurrentCFG->addEdge(CondBB, ExitBB);
  CondBB->setTerminator(TerminatorKind::Branch, While->getLoc());

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
  // Add start/end expressions as statements in for.cond so the DAP can detect
  // reads of uninitialized range variables.
  if (For->getStart())
    CondBB->addStatement(const_cast<ExprAST *>(For->getStart()));
  if (For->getEnd())
    CondBB->addStatement(const_cast<ExprAST *>(For->getEnd()));
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
    ControlFlowError(Brk->getLoc(), "'break' used outside of a loop.").raise();
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
    ControlFlowError(Cont->getLoc(), "'continue' used outside of a loop.").raise();
    return;
  }

  CurrentBlock->addStatement(Cont);
  LastTerminatorLoc = Cont->getLoc();
  CurrentBlock->setTerminator(TerminatorKind::Continue, LastTerminatorLoc);
  CurrentCFG->addEdge(CurrentBlock, LoopContinueTarget);

  // No further statements in this block
  CurrentBlock = nullptr;
}
