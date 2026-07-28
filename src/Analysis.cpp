#include "Analysis.h"
#include "Errors.h"
#include "AST.h"
#include "SymbolTable.h"
#include "ResolverSymbolTable.h"

#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <functional>

// Use raw VariableSymbol* as the definiteness key.
// VariableSymbol objects are owned by shared_ptrs attached to AST nodes and
// survive the entire compilation of a function — unlike SymbolID from
// GlobalSymbolTable which is erased when a lexical scope exits.
using AssignSet = std::unordered_set<const VariableSymbol *>;

namespace {

// ─── AnalysisVisitor ─────────────────────────────────────────────────────────
// Walks an expression and fires onRead/onDef callbacks with the VariableSymbol*
// that the Resolver attached to each variable reference / declaration.

class AnalysisVisitor : public ASTVisitor {
  const std::function<void(SourceLocation, const VariableSymbol *)> &onRead;
  const std::function<void(SourceLocation, const VariableSymbol *)> &onDef;

  void visitChild(const ExprAST *Node) {
    if (Node)
      const_cast<ExprAST *>(Node)->accept(*this);
  }

public:
  AnalysisVisitor(
      const std::function<void(SourceLocation, const VariableSymbol *)> &onRead,
      const std::function<void(SourceLocation, const VariableSymbol *)> &onDef)
      : onRead(onRead), onDef(onDef) {}

  void visitVariable(VariableExprAST &Node) override {
    if (Node.ResolvedSymbol)
      onRead(Node.getLoc(), Node.ResolvedSymbol.get());
  }

  void visitVarDecl(VarDeclExprAST &Node) override {
    visitChild(Node.getInitializer());
    // Only mark as "defined" when there is an explicit initializer.
    // Primitive locals declared without an initializer (e.g. `mut x: int;`)
    // are intentionally left out so the DAP catches reads before assignment.
    if (Node.getInitializer() && Node.ResolvedSymbol)
      onDef(Node.getLoc(), Node.ResolvedSymbol.get());
  }

  void visitAssignment(AssignmentExprAST &Node) override {
    visitChild(Node.getRHS());
    if (auto *L = dynamic_cast<const VariableExprAST *>(Node.getLHS())) {
      if (L->ResolvedSymbol)
        onDef(Node.getLoc(), L->ResolvedSymbol.get());
    } else {
      visitChild(Node.getLHS());
    }
  }

  void visitCompoundAssignment(CompoundAssignmentExprAST &Node) override {
    visitChild(Node.getRHS());
    if (auto *L = dynamic_cast<const VariableExprAST *>(Node.getLHS())) {
      if (L->ResolvedSymbol) {
        onRead(Node.getLoc(), L->ResolvedSymbol.get());
        onDef(Node.getLoc(),  L->ResolvedSymbol.get());
      }
    } else {
      visitChild(Node.getLHS());
    }
  }

  void visitPostfixExpr(PostfixExprAST &Node) override {
    if (auto *L = dynamic_cast<const VariableExprAST *>(Node.getOperand())) {
      if (L->ResolvedSymbol) {
        onRead(Node.getLoc(), L->ResolvedSymbol.get());
        onDef(Node.getLoc(),  L->ResolvedSymbol.get());
      }
    } else {
      visitChild(Node.getOperand());
    }
  }

  void visitBinary(BinaryExprAST &Node) override {
    visitChild(Node.getLHS());
    visitChild(Node.getRHS());
  }

  void visitUnary(UnaryExprAST &Node) override {
    visitChild(Node.getOperand());
  }

  void visitCast(CastExprAST &Node) override { visitChild(Node.getOperand()); }

  void visitBlock(BlockExprAST &Node) override {
    for (const auto &E : Node.getExpressions())
      visitChild(E.get());
  }

  void visitIf(IfExprAST &Node) override {
    for (const auto &Branch : Node.getBranches()) {
      visitChild(Branch.Cond.get());
      visitChild(Branch.Body.get());
    }
    visitChild(Node.getElse());
  }

  void visitWhile(WhileExprAST &Node) override {
    visitChild(Node.getCond());
    visitChild(Node.getBody());
  }

  void visitFor(ForExprAST &Node) override {
    visitChild(Node.getStart());
    visitChild(Node.getEnd());
    visitChild(Node.getStep());
    visitChild(Node.getBody());
  }

  void visitReturn(ReturnExprAST &Node) override { visitChild(Node.getVal()); }

  void visitFuncCall(FuncCallExprAST &Node) override {
    for (const auto &Arg : Node.getArgs())
      visitChild(Arg.get());
  }

  void visitMethodCall(MethodCallExprAST &Node) override {
    visitChild(Node.getBase());
    for (const auto &Arg : Node.getArgs())
      visitChild(Arg.get());
  }

  void visitFieldAccess(FieldAccessExprAST &Node) override {
    visitChild(Node.getBase());
  }

  void visitArrayAccess(ArrayAccessExprAST &Node) override {
    visitChild(Node.getBase());
    visitChild(Node.getIndex());
  }

  void visitArrayLiteral(ArrayLiteralExprAST &Node) override {
    for (const auto &Elem : Node.getElements())
      visitChild(Elem.get());
  }

  void visitArrayDecl(ArrayDeclExprAST &Node) override {
    for (const auto &Init : Node.getInitList())
      visitChild(Init.get());
    if (Node.getInitExpr())
      visitChild(Node.getInitExpr().get());
    // Arrays always get storage allocated — treat as defined immediately.
    if (Node.ResolvedSymbol)
      onDef(Node.getLoc(), Node.ResolvedSymbol.get());
  }

  void visitDefaultInit(DefaultInitExprAST &Node) override {
    (void)Node;
  }
};

// ─── BodyDeclCollector ────────────────────────────────────────────────────────
// A simple visitor that collects ALL locally-declared VariableSymbol* — both
// those with and without an explicit initializer. This set is used to
// distinguish local variables (which must be assigned before use) from function
// parameters (which are always defined when the function is called).

class BodyDeclCollector : public ASTVisitor {
  void visitChild(const ExprAST *Node) {
    if (Node)
      const_cast<ExprAST *>(Node)->accept(*this);
  }

public:
  AssignSet LocalDecls; // All locally-declared symbols (with or without init)

  void visitVarDecl(VarDeclExprAST &Node) override {
    if (Node.ResolvedSymbol)
      LocalDecls.insert(Node.ResolvedSymbol.get());
    visitChild(Node.getInitializer());
  }

  void visitArrayDecl(ArrayDeclExprAST &Node) override {
    if (Node.ResolvedSymbol)
      LocalDecls.insert(Node.ResolvedSymbol.get());
    for (const auto &I : Node.getInitList()) visitChild(I.get());
    if (Node.getInitExpr()) visitChild(Node.getInitExpr().get());
  }

  // Recurse into compound statements
  void visitBlock(BlockExprAST &Node) override {
    for (const auto &E : Node.getExpressions()) visitChild(E.get());
  }
  void visitIf(IfExprAST &Node) override {
    for (const auto &Br : Node.getBranches()) {
      visitChild(Br.Cond.get()); visitChild(Br.Body.get());
    }
    visitChild(Node.getElse());
  }
  void visitWhile(WhileExprAST &Node) override {
    visitChild(Node.getCond()); visitChild(Node.getBody());
  }
  void visitFor(ForExprAST &Node) override {
    visitChild(Node.getStart()); visitChild(Node.getEnd());
    visitChild(Node.getStep()); visitChild(Node.getBody());
  }
  void visitReturn(ReturnExprAST &Node) override { visitChild(Node.getVal()); }
  void visitAssignment(AssignmentExprAST &Node) override {
    visitChild(Node.getLHS()); visitChild(Node.getRHS());
  }
  void visitBinary(BinaryExprAST &Node) override {
    visitChild(Node.getLHS()); visitChild(Node.getRHS());
  }
};

} // namespace

// ─── collectReadsAndDefs ─────────────────────────────────────────────────────

static void collectReadsAndDefs(
    const ExprAST *E,
    const std::function<void(SourceLocation, const VariableSymbol *)> &onRead,
    const std::function<void(SourceLocation, const VariableSymbol *)> &onDef) {
  if (!E) return;
  AssignSet dummy;
  AnalysisVisitor Visitor(onRead, onDef);
  const_cast<ExprAST *>(E)->accept(Visitor);
}

// ─── DefiniteAssignmentPass ──────────────────────────────────────────────────

void DefiniteAssignmentPass::runOnCFG(CFG &Cfg,
                                      const std::vector<SymbolID> &/*InitialAssigned*/) {
  const auto &Blocks = Cfg.getBlocks();
  size_t N = Blocks.size();
  if (N == 0) return;

  std::unordered_map<const TurfBasicBlock *, size_t> index;
  for (size_t i = 0; i < N; ++i) index[Blocks[i].get()] = i;

  // ── Step 0: Collect all locally-declared symbols (the "body" set) ─────────
  // These are symbols introduced by VarDeclExprAST or ArrayDeclExprAST in the
  // function body. Function parameters are NOT in this set — they are always
  // considered pre-initialized and must never be flagged.
  AssignSet AllBodyDeclared;
  for (size_t b = 0; b < N; ++b) {
    for (ExprAST *Stmt : Blocks[b]->getStatements()) {
      BodyDeclCollector Coll;
      if (Stmt) const_cast<ExprAST *>(static_cast<const ExprAST *>(Stmt))->accept(Coll);
      for (const VariableSymbol *Sym : Coll.LocalDecls)
        AllBodyDeclared.insert(Sym);
    }
  }

  // ── Step 1: Dataflow (no error reporting) ─────────────────────────────────
  // Transfer : OUT[b] = IN[b] ∪ Defs(b)
  // Meet     : IN[b]  = ∩ OUT[pred] for all predecessors p
  // Entry IN starts empty; all other blocks start at the universal set.

  TurfBasicBlock *Entry = Cfg.getEntryBlock();
  size_t entryIdx = 0;
  if (Entry) entryIdx = index[Entry];

  std::vector<AssignSet> IN(N), OUT(N);
  // Non-entry blocks: init to "all locals" (universal set for intersection)
  for (size_t i = 0; i < N; ++i)
    if (i != entryIdx) IN[i] = AllBodyDeclared;

  auto computeOUT = [&](size_t b) -> AssignSet {
    AssignSet cur = IN[b];
    for (ExprAST *Stmt : Blocks[b]->getStatements()) {
      collectReadsAndDefs(Stmt,
        [](SourceLocation, const VariableSymbol *) { /* no-op */ },
        [&](SourceLocation, const VariableSymbol *Sym) { cur.insert(Sym); });
    }
    return cur;
  };

  for (size_t i = 0; i < N; ++i) OUT[i] = computeOUT(i);

  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t b = 0; b < N; ++b) {
      if (b == entryIdx) continue;

      // Meet: intersect OUT of all predecessors.
      AssignSet newIN = AllBodyDeclared;
      bool hasPred = false;
      for (size_t p = 0; p < N; ++p) {
        bool isPred = false;
        for (const auto *succ : Blocks[p]->getSuccessors()) {
          if (index.count(succ) && index.at(succ) == b) { isPred = true; break; }
        }
        if (!isPred) continue;
        hasPred = true;
        AssignSet intersected;
        for (const VariableSymbol *sym : newIN)
          if (OUT[p].count(sym)) intersected.insert(sym);
        newIN = std::move(intersected);
      }
      if (!hasPred) continue;

      if (newIN != IN[b]) {
        IN[b]  = newIN;
        OUT[b] = computeOUT(b);
        changed = true;
      }
    }
  }

  // ── Step 2: Report errors using fixed-point IN sets ───────────────────────
  // Only flag a read as "uninitialized" if the symbol was locally DECLARED
  // (i.e. it is in AllBodyDeclared) but NOT yet in the current assigned set.
  // Reads of function parameters (not in AllBodyDeclared) are always fine.
  for (size_t b = 0; b < N; ++b) {
    AssignSet cur = IN[b];
    for (ExprAST *Stmt : Blocks[b]->getStatements()) {
      collectReadsAndDefs(Stmt,
        [&](SourceLocation Loc, const VariableSymbol *Sym) {
          if (AllBodyDeclared.count(Sym) && !cur.count(Sym)) {
            DefiniteAssignmentError err(Loc, Sym->Name);
            err.withRelated({Sym->DeclLoc.Line, Sym->DeclLoc.Col},
                            "declared here");
            err.raise();
          }
        },
        [&](SourceLocation, const VariableSymbol *Sym) {
          cur.insert(Sym);
        });
    }
  }
}

// ─── LivenessAnalysisPass ─────────────────────────────────────────────────────
//
// Classic backward data-flow liveness analysis.
//
// Notation:
//   Gen[B]     = variables READ before being written in block B (upward exposed uses)
//   Kill[B]    = variables WRITTEN in block B
//   LiveIn[B]  = variables live at the entry of B
//   LiveOut[B] = variables live at the exit of B
//
// Equations:
//   LiveOut[B] = ∪ LiveIn[S]  for each successor S of B
//   LiveIn[B]  = Gen[B] ∪ (LiveOut[B] − Kill[B])
//
// Fixed-point is reached when no LiveIn set changes (worklist convergence).
//
// After convergence, dead stores and unused variables are reported.

void LivenessAnalysisPass::runOnCFG(
    CFG &Cfg,
    const std::unordered_set<const VariableSymbol *> &ParamSymbols) {

  const auto &Blocks = Cfg.getBlocks();
  size_t N = Blocks.size();
  if (N == 0) return;

  // ── Step 0: Identify all body-declared locals ─────────────────────────────
  // Mirrors DefiniteAssignmentPass's AllBodyDeclared. Parameters are excluded
  // so we never warn about an unused parameter.
  AssignSet AllBodyDeclared;
  // Also collect the SourceLocation of each VarDecl for the declaration site
  // in UnusedVariableWarning.
  std::unordered_map<const VariableSymbol *, SourceLocation> DeclLocMap;
  for (size_t b = 0; b < N; ++b) {
    for (ExprAST *Stmt : Blocks[b]->getStatements()) {
      BodyDeclCollector Coll;
      if (Stmt) const_cast<ExprAST *>(Stmt)->accept(Coll);
      for (const VariableSymbol *Sym : Coll.LocalDecls) {
        AllBodyDeclared.insert(Sym);
        // DeclLoc from ResolverSourceLocation → SourceLocation
        DeclLocMap[Sym] = {Sym->DeclLoc.Line, Sym->DeclLoc.Col};
      }
    }
  }

  // ── Step 1: Compute Gen and Kill per block ────────────────────────────────
  // Walk each block's statements IN ORDER. For each statement:
  //   - A READ of variable V that has NOT yet been written in this block
  //     adds V to Gen[B] (upward-exposed use).
  //   - A WRITE to V adds V to Kill[B].
  // "Written in this block before this point" is tracked via a local set.

  using LiveSet = std::unordered_set<const VariableSymbol *>;
  std::vector<LiveSet> Gen(N), Kill(N);

  for (size_t b = 0; b < N; ++b) {
    LiveSet WrittenSoFar; // tracks defs seen in this block so far (left to right)
    for (ExprAST *Stmt : Blocks[b]->getStatements()) {
      collectReadsAndDefs(Stmt,
        [&](SourceLocation, const VariableSymbol *Sym) {
          // Read: if not already written in this block, it's upward-exposed
          if (!WrittenSoFar.count(Sym))
            Gen[b].insert(Sym);
        },
        [&](SourceLocation, const VariableSymbol *Sym) {
          // Write (def)
          Kill[b].insert(Sym);
          WrittenSoFar.insert(Sym);
        });
    }
  }

  // ── Step 2: Backward data-flow fixed-point ────────────────────────────────
  // Build a block-index map for successor lookup.
  std::unordered_map<const TurfBasicBlock *, size_t> IndexOf;
  for (size_t i = 0; i < N; ++i) IndexOf[Blocks[i].get()] = i;

  std::vector<LiveSet> LiveIn(N), LiveOut(N);
  // Initialize LiveIn[B] = Gen[B]
  for (size_t i = 0; i < N; ++i) LiveIn[i] = Gen[i];

  // Worklist: all block indices
  std::vector<bool> InWorklist(N, true);
  // Process in reverse order (approximates reverse postorder for backward DFA)
  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (size_t b = N; b-- > 0;) {
      // LiveOut[B] = union of LiveIn of all successors
      LiveSet NewOut;
      for (const TurfBasicBlock *Succ : Blocks[b]->getSuccessors()) {
        auto It = IndexOf.find(Succ);
        if (It == IndexOf.end()) continue;
        for (const VariableSymbol *V : LiveIn[It->second])
          NewOut.insert(V);
      }
      // LiveIn[B] = Gen[B] ∪ (LiveOut[B] − Kill[B])
      LiveSet NewIn = Gen[b];
      for (const VariableSymbol *V : NewOut)
        if (!Kill[b].count(V))
          NewIn.insert(V);

      if (NewOut != LiveOut[b] || NewIn != LiveIn[b]) {
        LiveOut[b] = std::move(NewOut);
        LiveIn[b] = std::move(NewIn);
        Changed = true;
      }
    }
  }

  // ── Step 3: Report UnusedVariableWarning ─────────────────────────────────
  // A variable declared in the body that never appears in ANY Gen set across
  // all blocks is never read anywhere. Flag it at its declaration site.
  LiveSet EverRead;
  for (size_t b = 0; b < N; ++b)
    for (const VariableSymbol *V : Gen[b])
      EverRead.insert(V);

  for (const VariableSymbol *Sym : AllBodyDeclared) {
    if (!EverRead.count(Sym) && !ParamSymbols.count(Sym)) {
      auto LocIt = DeclLocMap.find(Sym);
      SourceLocation Loc = (LocIt != DeclLocMap.end()) ? LocIt->second : SourceLocation{0, 0};
      UnusedVariableWarning(Loc, Sym->Name).emit();
    }
  }

  // ── Step 4: Report DeadStoreWarning ──────────────────────────────────────
  // Walk each block again, simulating the live set from LiveOut[B] backward.
  // At each write (def) to variable V: if V is NOT in the current live set
  // at that point, the store is dead.
  for (size_t b = 0; b < N; ++b) {
    // current = set of variables live AFTER the current statement
    // We start at the end of the block with LiveOut[b] and walk backward.
    LiveSet Current = LiveOut[b];
    const auto &Stmts = Blocks[b]->getStatements();
    // Collect (def-location, symbol) pairs forward first, then process backward
    struct DefEntry { SourceLocation Loc; const VariableSymbol *Sym; };
    std::vector<DefEntry> Defs;
    std::vector<DefEntry> Reads;

    for (ExprAST *Stmt : Stmts) {
      collectReadsAndDefs(Stmt,
        [&](SourceLocation L, const VariableSymbol *S) { Reads.push_back({L, S}); },
        [&](SourceLocation L, const VariableSymbol *S) { Defs.push_back({L, S}); });
    }
    // For dead-store detection we need statement-level granularity:
    // re-walk in statement order, maintaining a "live after" set that
    // shrinks when we pass a use and grows when we pass a def.
    LiveSet LiveAfter = LiveOut[b];
    for (size_t s = Stmts.size(); s-- > 0;) {
      // Collect reads and defs for this single statement
      std::vector<std::pair<SourceLocation, const VariableSymbol *>> StmtDefs, StmtReads;
      collectReadsAndDefs(Stmts[s],
        [&](SourceLocation L, const VariableSymbol *S) { StmtReads.push_back({L, S}); },
        [&](SourceLocation L, const VariableSymbol *S) { StmtDefs.push_back({L, S}); });

      // Any def whose target is NOT in LiveAfter is a dead store.
      // Only flag locals (in AllBodyDeclared), not parameters.
      for (auto &[Loc, Sym] : StmtDefs) {
        if (AllBodyDeclared.count(Sym) && !LiveAfter.count(Sym)) {
          DeadStoreWarning(Loc, Sym->Name).emit();
        }
      }
      // Update LiveAfter: reads make variables live (add), defs kill liveness (remove)
      // Process in backward order for the statement: first reads (going backward)
      // then defs.
      for (auto &[Loc, Sym] : StmtReads)  LiveAfter.insert(Sym);
      for (auto &[Loc, Sym] : StmtDefs)   LiveAfter.erase(Sym);
    }
  }
}

// ─── ConstructorFieldAssignmentPass ──────────────────────────────────────────

namespace {

// Walks an expression and fires onFieldDef(fieldName) for every direct
// `self.<field> = expr` / `self.<field> += expr` assignment found -- NOT
// `self.a.b = x` or `self.arr[i] = x` (the LHS must be a FieldAccessExprAST
// directly on `self`, nothing nested further), and NOT through a helper
// function call (`self.setup()`) -- this is intraprocedural only, same
// conservative scope as MutatesSelfField() (Resolver.cpp) already uses for
// the closely-related "does this method mutate self" scan. Mirrors
// AnalysisVisitor's exact traversal shape above (same node types recursed
// into) so every place a `self.field = ...` could legally appear inside a
// constructor body is actually reached.
class SelfFieldDefVisitor : public ASTVisitor {
  const std::function<void(const std::string &)> &onFieldDef;

  void visitChild(const ExprAST *Node) {
    if (Node)
      const_cast<ExprAST *>(Node)->accept(*this);
  }

  // Returns the field name if LHS is exactly `self.<field>`, else nullptr.
  static const std::string *asSelfFieldName(const ExprAST *LHS) {
    const auto *FA = dynamic_cast<const FieldAccessExprAST *>(LHS);
    if (!FA)
      return nullptr;
    const auto *Base = dynamic_cast<const VariableExprAST *>(FA->getBase());
    if (!Base || Base->getName() != "self")
      return nullptr;
    return &FA->getFieldName();
  }

public:
  explicit SelfFieldDefVisitor(const std::function<void(const std::string &)> &cb)
      : onFieldDef(cb) {}

  void visitAssignment(AssignmentExprAST &Node) override {
    visitChild(Node.getRHS());
    if (const auto *F = asSelfFieldName(Node.getLHS()))
      onFieldDef(*F);
    else
      visitChild(Node.getLHS());
  }

  void visitCompoundAssignment(CompoundAssignmentExprAST &Node) override {
    visitChild(Node.getRHS());
    if (const auto *F = asSelfFieldName(Node.getLHS()))
      onFieldDef(*F);
    else
      visitChild(Node.getLHS());
  }

  void visitPostfixExpr(PostfixExprAST &Node) override {
    // `self.field++`/`--` -- not a realistic constructor pattern, but
    // handled for consistency: it both reads and re-assigns the field,
    // so it counts as a def the same way a compound assignment does.
    if (const auto *F = asSelfFieldName(Node.getOperand()))
      onFieldDef(*F);
    else
      visitChild(Node.getOperand());
  }

  void visitVarDecl(VarDeclExprAST &Node) override {
    visitChild(Node.getInitializer());
  }

  void visitBinary(BinaryExprAST &Node) override {
    visitChild(Node.getLHS());
    visitChild(Node.getRHS());
  }

  void visitUnary(UnaryExprAST &Node) override { visitChild(Node.getOperand()); }

  void visitCast(CastExprAST &Node) override { visitChild(Node.getOperand()); }

  void visitBlock(BlockExprAST &Node) override {
    for (const auto &E : Node.getExpressions())
      visitChild(E.get());
  }

  void visitIf(IfExprAST &Node) override {
    for (const auto &Branch : Node.getBranches()) {
      visitChild(Branch.Cond.get());
      visitChild(Branch.Body.get());
    }
    visitChild(Node.getElse());
  }

  void visitWhile(WhileExprAST &Node) override {
    visitChild(Node.getCond());
    visitChild(Node.getBody());
  }

  void visitFor(ForExprAST &Node) override {
    visitChild(Node.getStart());
    visitChild(Node.getEnd());
    visitChild(Node.getStep());
    visitChild(Node.getBody());
  }

  void visitCompare(CompareExprAST &Node) override {
    visitChild(Node.getSubjectMut());
    for (auto &Arm : Node.getArmsMut()) {
      for (auto &Val : Arm.Values)
        visitChild(Val.get());
      visitChild(Arm.Body.get());
    }
    visitChild(Node.getOtherBodyMut());
  }

  void visitReturn(ReturnExprAST &Node) override { visitChild(Node.getVal()); }

  void visitFuncCall(FuncCallExprAST &Node) override {
    for (const auto &Arg : Node.getArgs())
      visitChild(Arg.get());
  }

  void visitMethodCall(MethodCallExprAST &Node) override {
    visitChild(Node.getBase());
    for (const auto &Arg : Node.getArgs())
      visitChild(Arg.get());
  }

  void visitFieldAccess(FieldAccessExprAST &Node) override {
    visitChild(Node.getBase());
  }

  void visitArrayAccess(ArrayAccessExprAST &Node) override {
    visitChild(Node.getBase());
    visitChild(Node.getIndex());
  }

  void visitArrayLiteral(ArrayLiteralExprAST &Node) override {
    for (const auto &Elem : Node.getElements())
      visitChild(Elem.get());
  }

  void visitArrayDecl(ArrayDeclExprAST &Node) override {
    for (const auto &Init : Node.getInitList())
      visitChild(Init.get());
    if (Node.getInitExpr())
      visitChild(Node.getInitExpr().get());
  }
};

} // namespace

void ConstructorFieldAssignmentPass::runOnCFG(
    CFG &Cfg, const std::string &StructName,
    const std::vector<std::string> &RequiredFields,
    SourceLocation ConstructorLoc) {
  const auto &Blocks = Cfg.getBlocks();
  size_t N = Blocks.size();
  if (N == 0 || RequiredFields.empty())
    return;

  using FieldSet = std::unordered_set<std::string>;
  const FieldSet AllFields(RequiredFields.begin(), RequiredFields.end());

  std::unordered_map<const TurfBasicBlock *, size_t> index;
  for (size_t i = 0; i < N; ++i)
    index[Blocks[i].get()] = i;

  TurfBasicBlock *Entry = Cfg.getEntryBlock();
  size_t entryIdx = Entry && index.count(Entry) ? index[Entry] : 0;

  // ── Dataflow (mirrors DefiniteAssignmentPass exactly) ─────────────────────
  // Transfer : OUT[b] = IN[b] ∪ FieldsAssignedIn(b)
  // Meet     : IN[b]  = ∩ OUT[pred] for all predecessors p
  // Entry IN starts empty (nothing is assigned before the constructor
  // starts); every other block starts at the universal set (AllFields) so
  // intersection with real predecessor data is what narrows it down.
  std::vector<FieldSet> IN(N), OUT(N);
  for (size_t i = 0; i < N; ++i)
    if (i != entryIdx)
      IN[i] = AllFields;

  auto computeOUT = [&](size_t b) {
    FieldSet cur = IN[b];
    for (ExprAST *Stmt : Blocks[b]->getStatements()) {
      SelfFieldDefVisitor V([&](const std::string &Field) { cur.insert(Field); });
      if (Stmt)
        const_cast<ExprAST *>(static_cast<const ExprAST *>(Stmt))->accept(V);
    }
    return cur;
  };
  for (size_t i = 0; i < N; ++i)
    OUT[i] = computeOUT(i);

  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t b = 0; b < N; ++b) {
      if (b == entryIdx)
        continue;

      FieldSet newIN = AllFields;
      bool hasPred = false;
      for (size_t p = 0; p < N; ++p) {
        bool isPred = false;
        for (const auto *succ : Blocks[p]->getSuccessors()) {
          if (index.count(succ) && index.at(succ) == b) {
            isPred = true;
            break;
          }
        }
        if (!isPred)
          continue;
        hasPred = true;
        FieldSet intersected;
        for (const auto &f : newIN)
          if (OUT[p].count(f))
            intersected.insert(f);
        newIN = std::move(intersected);
      }
      if (!hasPred)
        continue;

      if (newIN != IN[b]) {
        IN[b] = newIN;
        OUT[b] = computeOUT(b);
        changed = true;
      }
    }
  }

  // ── Check: is every field assigned on every path to the CFG's exit? ──────
  // Cfg.getExitBlock() is the single virtual sink every return AND the
  // natural fall-through end of the function already target (CFGBuilder::
  // buildCFG links a terminator-less final block to it too) -- its fixed-
  // point IN set is exactly "assigned on every path from entry to the end
  // of the constructor". A totally unreachable exit (e.g. an infinite loop
  // with no return -- already reported separately by InfiniteLoopWarning/
  // MissingReturnError) leaves IN at its initial universal value, which
  // conservatively reports no missing fields rather than a confusing
  // second diagnostic for the same root cause.
  TurfBasicBlock *ExitBB = Cfg.getExitBlock();
  FieldSet AssignedOnEveryPath = AllFields;
  if (ExitBB && index.count(ExitBB))
    AssignedOnEveryPath = IN[index.at(ExitBB)];

  std::vector<std::string> Missing;
  for (const auto &Field : RequiredFields)
    if (!AssignedOnEveryPath.count(Field))
      Missing.push_back(Field);

  if (!Missing.empty())
    IncompleteConstructorError(ConstructorLoc, StructName, Missing).raise();
}
