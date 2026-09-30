#include "Analysis.h"
#include "Errors.h"
#include "AST.h"
#include "SymbolTable.h"
#include "ResolverSymbolTable.h"

#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <optional>

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
    } else if (auto *AA = dynamic_cast<const ArrayAccessExprAST *>(Node.getLHS())) {
      // SEM-004 (§12.6): an element write (`arr[i] = v`) counts as assigning
      // the WHOLE array -- elements are not tracked individually, the same
      // granularity ConstructorFieldAssignmentPass already uses for
      // `self.<field>`. This ONLY applies when the base is a genuine
      // fixed-size T[N] local: its storage IS the alloca (no separate
      // pointer value gets read to compute the element address), so the
      // write is purely a definition, same as writing a struct field. A
      // `string`/`ptr`/`T*`/unsized-`T[]` base is representationally a
      // pointer -- indexing it genuinely READS the pointer value stored in
      // the base variable to compute the target address, so those must fall
      // through to the ordinary read treatment below (required: `mut buf:
      // string; buf[0] = 'x';` must still be E008, not a silent write
      // through an unassigned pointer).
      visitChild(AA->getIndex());
      const auto *BaseVar = dynamic_cast<const VariableExprAST *>(AA->getBase());
      const auto *BaseArrTy = (BaseVar && BaseVar->ResolvedSymbol)
          ? dynamic_cast<const ::ArrayType *>(BaseVar->ResolvedSymbol->Type.get())
          : nullptr;
      if (BaseArrTy && BaseArrTy->getSize() > 0) {
        onDef(Node.getLoc(), BaseVar->ResolvedSymbol.get());
      } else {
        visitChild(AA->getBase());
      }
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

  void visitForEach(ForEachExprAST &Node) override {
    visitChild(Node.getIterable());
    visitChild(Node.getBody());
  }

  // LEC-003: a `compare` header's pattern is a CFG statement (see
  // CFGBuilder::visitCompare()), so this is where a binding becomes a
  // DEFINITION -- on the edge into the arm, exactly as FREEZE.md §6.17 says.
  // Without it a binding would be an "undeclared-here" local that no pass ever
  // saw assigned, and a binding read in the arm body could never be proven
  // defined. A struct pattern's value sub-patterns (`x: 0`) read nothing
  // (literals and enum variants), so only the bindings matter.
  void visitBindingPattern(BindingPatternExprAST &Node) override {
    if (Node.ResolvedVar)
      onDef(Node.getLoc(), Node.ResolvedVar.get());
  }

  void visitStructPattern(StructPatternExprAST &Node) override {
    for (auto &F : Node.getFieldsMut())
      visitChild(F.Sub.get());
  }

  void visitReturn(ReturnExprAST &Node) override { visitChild(Node.getVal()); }

  // LANG-003: without this, a variable read or defined ONLY inside a
  // `defer <statement>;` would be invisible to DefiniteAssignmentPass/
  // LivenessAnalysisPass (both built on this visitor via
  // collectReadsAndDefs()) -- ASTVisitor's default visitDefer() is a no-op,
  // so a deferred read of an uninitialized local would go uncaught, and a
  // local only ever read via `defer` would be falsely flagged
  // UnusedVariableWarning/DeadStoreWarning. The defer's registration point
  // (this textual position, not whenever it actually runs later) is
  // exactly where the read/def dataflow-wise needs to be recorded: the
  // deferred value doesn't exist yet, but the SYMBOL must already be
  // assigned by here for the eventual deferred use to be sound, which is
  // precisely what "treat it as an ordinary read/def right here" checks.
  void visitDefer(DeferExprAST &Node) override { visitChild(Node.getStmt()); }

  // OVH-012: without this, a variable read ONLY as the operand of a
  // `recover` expression's WRAPPED call (`immut numStr: string = ...;
  // strings::parse_double(numStr) recover run { ... };`, lib/std/json/
  // json.tr) was invisible to both DefiniteAssignmentPass and
  // LivenessAnalysisPass -- ASTVisitor's default visitRecoverExpr() is a
  // no-op, so `numStr` was falsely flagged BOTH UnusedVariableWarning
  // ("declared but never used") and DeadStoreWarning ("assigned but never
  // read"), confirmed live. Same reasoning as visitDefer() just above (LANG-003).
  // Wrapped is always visited (it's evaluated unconditionally); ProduceExpr
  // only runs on the failure path of `recover produce`, Block only on the
  // failure path of `recover run`/`recover as err run` -- mutually
  // exclusive per RecoverKind, so exactly one of the two is ever non-null.
  void visitRecoverExpr(RecoverExprAST &Node) override {
    visitChild(Node.getWrapped());
    if (Node.getKind() == RecoverKind::Produce)
      visitChild(Node.getProduceExpr());
    else
      visitChild(Node.getBlock());
  }

  void visitFuncCall(FuncCallExprAST &Node) override {
    // First-class functions: `f(x)` where `f` is a local variable/parameter
    // of fn(...):... type is a genuine READ of `f` -- unlike an ordinary
    // direct call (`double_it(5)`), whose VariableExprAST callee never gets
    // a ResolvedSymbol attached (Resolver::visitFuncCall intercepts it
    // before visitVariable() ever runs; see its comment), so visitVariable()
    // above is naturally a no-op for that case and only fires onRead for the
    // genuine indirect-call shape. Without this, `mut f: fn(int):int = ...;
    // f(3);` falsely warned "'f' is declared but never used" / "assigned but
    // never read" -- confirmed live before this fix.
    visitChild(Node.getCallee());
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
    // SEM-004 (§12.6, FREEZE.md §6.4): storage is always zero-filled
    // (ArrayDeclExprAST::codegen() memsets unconditionally), but that's a
    // runtime guarantee, not a static one -- an array declared with NEITHER
    // an init list nor a general initializer is statically unassigned, same
    // as any other body-declared local. It only becomes "defined" here, at
    // declaration, when an initializer was actually given; an element write
    // (`arr[0] = 1`) is the other way to assign it, handled in
    // visitAssignment below.
    if ((!Node.getInitList().empty() || Node.getInitExpr()) && Node.ResolvedSymbol)
      onDef(Node.getLoc(), Node.ResolvedSymbol.get());
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
  /// The subset of LocalDecls that are `compare` pattern bindings (LEC-003).
  /// A binding is immutable and defined exactly once, on entry to its arm, so
  /// "assigned but never read" would only repeat the unused-variable warning
  /// the same name already gets -- LivenessAnalysisPass skips dead-store
  /// reports for these.
  AssignSet PatternBindings;

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
  void visitForEach(ForEachExprAST &Node) override {
    visitChild(Node.getIterable()); visitChild(Node.getBody());
  }
  // LEC-003: a binding is a body-declared local, like a `mut` -- it MUST be in
  // this set, or DefiniteAssignmentPass treats it as parameter-like (never
  // flagged) and LivenessAnalysisPass never reports it unused.
  void visitBindingPattern(BindingPatternExprAST &Node) override {
    if (Node.ResolvedVar) {
      LocalDecls.insert(Node.ResolvedVar.get());
      PatternBindings.insert(Node.ResolvedVar.get());
    }
  }
  void visitStructPattern(StructPatternExprAST &Node) override {
    for (auto &F : Node.getFieldsMut())
      visitChild(F.Sub.get());
  }
  void visitReturn(ReturnExprAST &Node) override { visitChild(Node.getVal()); }
  void visitAssignment(AssignmentExprAST &Node) override {
    visitChild(Node.getLHS()); visitChild(Node.getRHS());
  }
  void visitBinary(BinaryExprAST &Node) override {
    visitChild(Node.getLHS()); visitChild(Node.getRHS());
  }
  // LANG-003: a `defer` could (rarely) wrap a nested loop whose body
  // declares its own local (e.g. `defer for (i in 0..n) { mut x = ...; }`)
  // -- recurse in so DefiniteAssignmentPass's "AllBodyDeclared" set still
  // sees it, same reasoning as AnalysisVisitor::visitDefer() above.
  void visitDefer(DeferExprAST &Node) override { visitChild(Node.getStmt()); }
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
            // SEM-004 (§8): name the type in the message, and suggest
            // 'T::default()' specifically for a struct -- the mechanical
            // migration this group's own breaking change calls for. Only
            // structs actually have a callable '::default()'
            // (ResolverCall.cpp's DefaultConstruct dispatch is gated on
            // SymTable_.isKnownStruct()); an enum/array/generic local's
            // fix is an ordinary explicit initializer instead, so
            // suggesting '::default()' for those would name a construct
            // that doesn't parse.
            const std::string TypeName = Sym->Type ? Sym->Type->getName() : "";
            const bool SuggestDefault =
                Sym->Type && dynamic_cast<const TurfStructType *>(Sym->Type.get());
            DefiniteAssignmentError err(Loc, Sym->Name, TypeName, SuggestDefault);
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
  // The pattern bindings among them (LEC-003): see BodyDeclCollector.
  AssignSet BindingSyms;
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
      for (const VariableSymbol *Sym : Coll.PatternBindings)
        BindingSyms.insert(Sym);
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

  // EverRead tracks every variable read ANYWHERE in the function, completely
  // independent of Gen[]. Gen[b] only records upward-exposed reads (a read
  // not preceded by a write to the same variable earlier in the same
  // block) -- that's the correct input for the Step 2 backward dataflow
  // and Step 4's dead-store detection, but it systematically misses the
  // single most common local-variable pattern in the entire language:
  // declare-then-use within the same block (`mut x: int = 5; return x;`).
  // That read is never upward-exposed (the declaration's own write shadows
  // it in WrittenSoFar), so a naive "was this variable ever in some Gen[b]"
  // check -- which Step 3 used to do -- flagged it as unused even though it
  // was read on the very next line. EverRead exists so Step 3 can answer
  // "was this ever read" without disturbing Gen[]'s dataflow semantics.
  LiveSet EverRead;

  for (size_t b = 0; b < N; ++b) {
    LiveSet WrittenSoFar; // tracks defs seen in this block so far (left to right)
    for (ExprAST *Stmt : Blocks[b]->getStatements()) {
      collectReadsAndDefs(Stmt,
        [&](SourceLocation, const VariableSymbol *Sym) {
          EverRead.insert(Sym);
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
  // A variable declared in the body that's never in EverRead (collected
  // above, in Step 1) is never read anywhere. Flag it at its declaration site.
  for (const VariableSymbol *Sym : AllBodyDeclared) {
    // A name starting with '_' says "deliberately unused" -- the convention
    // the warning's own hint recommends, which only became true once `_name`
    // lexed (LEC-003, FREEZE.md §6.17).
    if (!Sym->Name.empty() && Sym->Name[0] == '_')
      continue;
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
        if (AllBodyDeclared.count(Sym) && !LiveAfter.count(Sym) &&
            !BindingSyms.count(Sym) &&
            // `_name`: same "deliberately unused" convention as Step 3.
            !(!Sym->Name.empty() && Sym->Name[0] == '_')) {
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
  std::function<void(const std::string &)> onFieldDef;

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
  explicit SelfFieldDefVisitor(std::function<void(const std::string &)> cb)
      : onFieldDef(std::move(cb)) {}

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

  void visitForEach(ForEachExprAST &Node) override {
    visitChild(Node.getIterable());
    visitChild(Node.getBody());
  }

  void visitCompare(CompareExprAST &Node) override {
    visitChild(Node.getSubjectMut());
    for (auto &Arm : Node.getArmsMut()) {
      for (auto &Val : Arm.Values)
        visitChild(Val.get());
      for (auto &G : Arm.Guards)
        visitChild(G.get());
      visitChild(Arm.Body.get());
    }
    visitChild(Node.getOtherBodyMut());
  }

  void visitReturn(ReturnExprAST &Node) override { visitChild(Node.getVal()); }

  // LANG-003: a `self.field = ...` cleanup line is exactly the sort of
  // thing a constructor might reasonably `defer` -- recurse in so
  // ConstructorFieldAssignmentPass still sees it as a real field def.
  void visitDefer(DeferExprAST &Node) override { visitChild(Node.getStmt()); }

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

// ─── NullabilityPass (LANG-002) ──────────────────────────────────────────────
// See include/Analysis.h's class doc comment for the overall shape (a
// DefiniteAssignmentPass-style forward dataflow, but two-state: a symbol is
// either in the current NonNullSet ("proven non-null here") or it isn't
// ("not proven" -- covers both "still nullable" and "just compared null").

using NonNullSet = std::unordered_set<const VariableSymbol *>;

namespace {

// Set intersection -- the meet operation at a CFG merge point. A symbol
// proven non-null along only ONE of two incoming paths is NOT proven
// non-null at the merge (FREEZE-adjacent §8 LANG-002 wording: "meet =
// join-to-Nullable on disagreement" -- absence from the intersection IS
// that join, under this pass's two-state simplification).
NonNullSet meetNonNull(const NonNullSet &A, const NonNullSet &B) {
  NonNullSet R;
  for (const VariableSymbol *S : A)
    if (B.count(S))
      R.insert(S);
  return R;
}

bool isTrackedNullableRef(const VariableSymbol *Sym) {
  return Sym && Sym->Type && Sym->Type->isNullableReference();
}

// Collects every &?T-typed VariableSymbol* touched anywhere in the function
// -- both body-declared locals (VarDeclExprAST) and PARAMETERS, discovered
// the same way AnalysisVisitor discovers parameter reads elsewhere in this
// file: a parameter never gets its own VarDeclExprAST in the body, but any
// VariableExprAST read of it still carries a Resolver-attached
// ResolvedSymbol, so scanning reads finds it too. A &?T parameter that is
// never read anywhere has nothing to narrow or check, so missing it here is
// harmless by construction.
class NullableRefCollector : public ASTVisitor {
  void visitChild(const ExprAST *Node) {
    if (Node)
      const_cast<ExprAST *>(Node)->accept(*this);
  }
  void addIfTracked(const VariableSymbol *Sym) {
    if (isTrackedNullableRef(Sym))
      Tracked.insert(Sym);
  }

public:
  std::unordered_set<const VariableSymbol *> Tracked;

  void visitVariable(VariableExprAST &Node) override {
    addIfTracked(Node.getResolvedSymbol());
  }
  void visitVarDecl(VarDeclExprAST &Node) override {
    addIfTracked(Node.getResolvedSymbol());
    visitChild(Node.getInitializer());
  }
  void visitArrayDecl(ArrayDeclExprAST &Node) override {
    for (const auto &I : Node.getInitList())
      visitChild(I.get());
    if (Node.getInitExpr())
      visitChild(Node.getInitExpr().get());
  }
  void visitBlock(BlockExprAST &Node) override {
    for (const auto &E : Node.getExpressions())
      visitChild(E.get());
  }
  void visitIf(IfExprAST &Node) override {
    for (const auto &Br : Node.getBranches()) {
      visitChild(Br.Cond.get());
      visitChild(Br.Body.get());
    }
    visitChild(Node.getElse());
  }
  void visitWhile(WhileExprAST &Node) override {
    visitChild(Node.getCond());
    visitChild(Node.getBody());
  }
  void visitDoWhile(DoWhileExprAST &Node) override {
    visitChild(Node.getCond());
    visitChild(Node.getBody());
  }
  void visitFor(ForExprAST &Node) override {
    visitChild(Node.getStart());
    visitChild(Node.getEnd());
    visitChild(Node.getStep());
    visitChild(Node.getBody());
  }
  void visitForEach(ForEachExprAST &Node) override {
    visitChild(Node.getIterable());
    visitChild(Node.getBody());
  }
  void visitReturn(ReturnExprAST &Node) override { visitChild(Node.getVal()); }
  // LANG-003: discovers every &?T-typed symbol the function touches AT ALL
  // (see this class's own doc comment above) -- one only ever read/assigned
  // inside a `defer` must still end up Tracked, or NullabilityPass would
  // silently skip checking it everywhere else too.
  void visitDefer(DeferExprAST &Node) override { visitChild(Node.getStmt()); }
  void visitAssignment(AssignmentExprAST &Node) override {
    visitChild(Node.getLHS());
    visitChild(Node.getRHS());
  }
  void visitCompoundAssignment(CompoundAssignmentExprAST &Node) override {
    visitChild(Node.getLHS());
    visitChild(Node.getRHS());
  }
  void visitPostfixExpr(PostfixExprAST &Node) override { visitChild(Node.getOperand()); }
  void visitBinary(BinaryExprAST &Node) override {
    visitChild(Node.getLHS());
    visitChild(Node.getRHS());
  }
  void visitUnary(UnaryExprAST &Node) override { visitChild(Node.getOperand()); }
  void visitAddressOf(AddressOfExprAST &Node) override { visitChild(Node.getOperand()); }
  void visitCast(CastExprAST &Node) override { visitChild(Node.getOperand()); }
  void visitFieldAccess(FieldAccessExprAST &Node) override { visitChild(Node.getBase()); }
  void visitArrayAccess(ArrayAccessExprAST &Node) override {
    visitChild(Node.getBase());
    visitChild(Node.getIndex());
  }
  void visitArrayLiteral(ArrayLiteralExprAST &Node) override {
    for (const auto &E : Node.getElements())
      visitChild(E.get());
  }
  void visitFuncCall(FuncCallExprAST &Node) override {
    visitChild(Node.getCallee());
    for (const auto &A : Node.getArgs())
      visitChild(A.get());
  }
  void visitMethodCall(MethodCallExprAST &Node) override {
    visitChild(Node.getBase());
    for (const auto &A : Node.getArgs())
      visitChild(A.get());
  }
  void visitCompare(CompareExprAST &Node) override {
    visitChild(Node.getSubject());
    for (const auto &Arm : Node.getArms()) {
      for (const auto &V : Arm.Values)
        visitChild(V.get());
      for (const auto &G : Arm.Guards)
        visitChild(G.get());
      visitChild(Arm.Body.get());
    }
    visitChild(Node.getOtherBody());
  }
};

// If exactly one of LHS/RHS is a literal `null` and the other is a bare,
// TRACKED variable reference, returns that variable's symbol -- the only
// shape this pass narrows on (direct comparison), per LANG-002's own
// "deliberately intraprocedural... narrowing from direct comparisons only"
// scoping (§8's Risks paragraph). Anything else (a field access compared to
// null, a function call's result, two variables compared to each other)
// returns nullptr -- correctly falls through to "no narrowing", never a
// false claim of safety.
const VariableSymbol *nullNarrowCandidate(
    const ExprAST *LHS, const ExprAST *RHS,
    const std::unordered_set<const VariableSymbol *> &Tracked) {
  const ExprAST *VarSide = nullptr;
  if (dynamic_cast<const NullExprAST *>(RHS))
    VarSide = LHS;
  else if (dynamic_cast<const NullExprAST *>(LHS))
    VarSide = RHS;
  else
    return nullptr;
  const auto *VE = dynamic_cast<const VariableExprAST *>(VarSide);
  if (!VE)
    return nullptr;
  const VariableSymbol *Sym = VE->getResolvedSymbol();
  if (!Sym || !Tracked.count(Sym))
    return nullptr;
  return Sym;
}

// Is `RHS`'s VALUE provably non-null, given the CURRENT NonNullSet `In`, for
// the purpose of updating a tracked &?T variable's own state at a
// VarDecl/Assignment? Three shapes are recognized -- anything else is
// conservatively Nullable:
//   - a literal `null`                          -> never non-null
//   - `&expr` (AddressOfExprAST)                 -> always non-null (&T)
//   - a bare variable reference, either another
//     TRACKED &?T (copy its current state) or a
//     plain `&T` (always non-null, widening in)  -> as described
// A function call, field access, or any other expression shape defaults to
// Nullable -- this pass only ever tracks VARIABLE state (DESIGN_NOTES.md
// §1's own framing), never a general expression's provenance.
bool classifyRHSNonNull(const ExprAST *RHS, const NonNullSet &In,
                        const std::unordered_set<const VariableSymbol *> &Tracked) {
  if (!RHS)
    return false;
  if (dynamic_cast<const NullExprAST *>(RHS))
    return false;
  if (dynamic_cast<const AddressOfExprAST *>(RHS))
    return true;
  if (const auto *VE = dynamic_cast<const VariableExprAST *>(RHS)) {
    const VariableSymbol *Sym = VE->getResolvedSymbol();
    if (!Sym)
      return false;
    if (Tracked.count(Sym))
      return In.count(Sym) != 0;
    if (Sym->Type && Sym->Type->isReference() && !Sym->Type->isNullableReference())
      return true; // a plain &T widening in is always non-null
    return false;
  }
  return false;
}

/**
 * @brief Evaluates ONE statement/expression against an incoming NonNullSet,
 *        both (a) computing the narrowed sets that hold when it evaluates
 *        true/false (only meaningful for a genuine branch CONDITION -- every
 *        other statement kind sets OnTrue == OnFalse == its own single
 *        resulting map) and (b) -- when DoReport is true -- raising
 *        PossibleNullDereferenceError/UnnarrowedNullableCastError for any
 *        unguarded `&?T` use found along the way.
 *
 * Mirrors AnalysisVisitor's own shape exactly (a plain ASTVisitor subclass,
 * one instance per sub-expression, recursing via `Node.accept(*this)`) --
 * NOT the dynamic_cast-chain style CFG.cpp itself uses -- so this reuses the
 * codebase's existing tree-walking idiom for a pass instead of introducing
 * a second one. `checkOnly()` is the recursion helper for an operand that
 * ISN'T itself a condition (an ordinary sub-expression, e.g. a function
 * call's arguments): it still needs full recursive checking, but its own
 * narrowing outcome is irrelevant to the caller and discarded.
 *
 * DoReport is threaded through so this SAME logic can run twice per block,
 * exactly like DefiniteAssignmentPass's own two-step shape (Analysis.cpp,
 * `runOnCFG` above): once silently, during fixed-point dataflow (an
 * intermediate, not-yet-converged IN set must never raise a real user-
 * facing diagnostic), and once for real after convergence, using the final
 * fixed-point IN sets.
 */
class NarrowingVisitor : public ASTVisitor {
  const std::unordered_set<const VariableSymbol *> &Tracked;
  bool DoReport;

  static void checkOnly(const ExprAST *E, const NonNullSet &In,
                        const std::unordered_set<const VariableSymbol *> &Tracked,
                        bool DoReport) {
    if (!E)
      return;
    NarrowingVisitor V(In, Tracked, DoReport);
    const_cast<ExprAST *>(E)->accept(V);
  }

public:
  NonNullSet OnTrue, OnFalse;

  NarrowingVisitor(const NonNullSet &In,
                   const std::unordered_set<const VariableSymbol *> &Tracked,
                   bool DoReport)
      : Tracked(Tracked), DoReport(DoReport), OnTrue(In), OnFalse(In) {}

  void visitBinary(BinaryExprAST &Node) override {
    if (Node.getOp() == TOK_AND) {
      NarrowingVisitor L(OnTrue, Tracked, DoReport);
      const_cast<ExprAST *>(Node.getLHS())->accept(L);
      NarrowingVisitor R(L.OnTrue, Tracked, DoReport); // RHS runs only if LHS true
      const_cast<ExprAST *>(Node.getRHS())->accept(R);
      OnTrue = R.OnTrue;
      OnFalse = meetNonNull(L.OnFalse, R.OnFalse);
      return;
    }
    if (Node.getOp() == TOK_OR) {
      NarrowingVisitor L(OnTrue, Tracked, DoReport);
      const_cast<ExprAST *>(Node.getLHS())->accept(L);
      NarrowingVisitor R(L.OnFalse, Tracked, DoReport); // RHS runs only if LHS false
      const_cast<ExprAST *>(Node.getRHS())->accept(R);
      OnFalse = R.OnFalse;
      OnTrue = meetNonNull(L.OnTrue, R.OnTrue);
      return;
    }
    if (Node.getOp() == TOK_EQ || Node.getOp() == TOK_NEQ) {
      if (const VariableSymbol *Sym =
              nullNarrowCandidate(Node.getLHS(), Node.getRHS(), Tracked)) {
        NonNullSet T = OnTrue, F = OnTrue;
        if (Node.getOp() == TOK_NEQ) { T.insert(Sym); F.erase(Sym); }
        else                          { T.erase(Sym);  F.insert(Sym); }
        OnTrue = std::move(T);
        OnFalse = std::move(F);
        return;
      }
    }
    checkOnly(Node.getLHS(), OnTrue, Tracked, DoReport);
    checkOnly(Node.getRHS(), OnTrue, Tracked, DoReport);
  }

  void visitFieldAccess(FieldAccessExprAST &Node) override {
    checkOnly(Node.getBase(), OnTrue, Tracked, DoReport);
    if (const auto *BaseVar = dynamic_cast<const VariableExprAST *>(Node.getBase())) {
      const VariableSymbol *Sym = BaseVar->getResolvedSymbol();
      if (Sym && Tracked.count(Sym) && !OnTrue.count(Sym) && DoReport) {
        PossibleNullDereferenceError Err(Node.getLoc(), BaseVar->getName(),
                                         Node.getFieldName());
        Err.withRelated({Sym->DeclLoc.Line, Sym->DeclLoc.Col}, "declared here");
        Err.raise();
      }
    }
  }

  void visitCast(CastExprAST &Node) override {
    checkOnly(Node.getOperand(), OnTrue, Tracked, DoReport);
    const TurfType *Dest = Node.getDestType();
    if (Dest && Dest->isReference() && !Dest->isNullableReference()) {
      if (const auto *BaseVar =
              dynamic_cast<const VariableExprAST *>(Node.getOperand())) {
        const VariableSymbol *Sym = BaseVar->getResolvedSymbol();
        if (Sym && Tracked.count(Sym) && !OnTrue.count(Sym) && DoReport)
          UnnarrowedNullableCastError(Node.getLoc(), BaseVar->getName()).raise();
      }
    }
  }

  void visitVarDecl(VarDeclExprAST &Node) override {
    checkOnly(Node.getInitializer(), OnTrue, Tracked, DoReport);
    const VariableSymbol *Sym = Node.getResolvedSymbol();
    if (Sym && Tracked.count(Sym) && Node.getInitializer()) {
      NonNullSet Updated = OnTrue;
      if (classifyRHSNonNull(Node.getInitializer(), OnTrue, Tracked))
        Updated.insert(Sym);
      else
        Updated.erase(Sym);
      OnTrue = OnFalse = std::move(Updated);
    }
  }

  void visitAssignment(AssignmentExprAST &Node) override {
    checkOnly(Node.getRHS(), OnTrue, Tracked, DoReport);
    if (const auto *LV = dynamic_cast<const VariableExprAST *>(Node.getLHS())) {
      const VariableSymbol *Sym = LV->getResolvedSymbol();
      if (Sym && Tracked.count(Sym)) {
        NonNullSet Updated = OnTrue;
        if (classifyRHSNonNull(Node.getRHS(), OnTrue, Tracked))
          Updated.insert(Sym);
        else
          Updated.erase(Sym);
        OnTrue = OnFalse = std::move(Updated);
        return;
      }
    }
    checkOnly(Node.getLHS(), OnTrue, Tracked, DoReport);
  }

  void visitCompoundAssignment(CompoundAssignmentExprAST &Node) override {
    checkOnly(Node.getLHS(), OnTrue, Tracked, DoReport);
    checkOnly(Node.getRHS(), OnTrue, Tracked, DoReport);
  }
  void visitPostfixExpr(PostfixExprAST &Node) override {
    checkOnly(Node.getOperand(), OnTrue, Tracked, DoReport);
  }
  void visitUnary(UnaryExprAST &Node) override {
    checkOnly(Node.getOperand(), OnTrue, Tracked, DoReport);
  }
  void visitAddressOf(AddressOfExprAST &Node) override {
    checkOnly(Node.getOperand(), OnTrue, Tracked, DoReport);
  }
  void visitReturn(ReturnExprAST &Node) override {
    checkOnly(Node.getVal(), OnTrue, Tracked, DoReport);
  }
  void visitFuncCall(FuncCallExprAST &Node) override {
    checkOnly(Node.getCallee(), OnTrue, Tracked, DoReport);
    for (const auto &A : Node.getArgs())
      checkOnly(A.get(), OnTrue, Tracked, DoReport);
  }
  void visitMethodCall(MethodCallExprAST &Node) override {
    checkOnly(Node.getBase(), OnTrue, Tracked, DoReport);
    for (const auto &A : Node.getArgs())
      checkOnly(A.get(), OnTrue, Tracked, DoReport);
  }
  void visitArrayAccess(ArrayAccessExprAST &Node) override {
    checkOnly(Node.getBase(), OnTrue, Tracked, DoReport);
    checkOnly(Node.getIndex(), OnTrue, Tracked, DoReport);
  }
  void visitArrayLiteral(ArrayLiteralExprAST &Node) override {
    for (const auto &E : Node.getElements())
      checkOnly(E.get(), OnTrue, Tracked, DoReport);
  }
  void visitArrayDecl(ArrayDeclExprAST &Node) override {
    for (const auto &I : Node.getInitList())
      checkOnly(I.get(), OnTrue, Tracked, DoReport);
    if (Node.getInitExpr())
      checkOnly(Node.getInitExpr().get(), OnTrue, Tracked, DoReport);
  }
  void visitCompare(CompareExprAST &Node) override {
    checkOnly(Node.getSubject(), OnTrue, Tracked, DoReport);
  }
};

struct NarrowedStatement {
  NonNullSet OnTrue, OnFalse;
};

NarrowedStatement evalStatement(const ExprAST *Stmt, const NonNullSet &In,
                                const std::unordered_set<const VariableSymbol *> &Tracked,
                                bool DoReport) {
  if (!Stmt)
    return {In, In};
  NarrowingVisitor V(In, Tracked, DoReport);
  const_cast<ExprAST *>(Stmt)->accept(V);
  return {V.OnTrue, V.OnFalse};
}

} // namespace

void NullabilityPass::runOnCFG(CFG &Cfg) {
  const auto &Blocks = Cfg.getBlocks();
  size_t N = Blocks.size();
  if (N == 0)
    return;

  // Discover every &?T-typed symbol the function touches at all, across
  // every block -- the SAME approach DefiniteAssignmentPass uses for its own
  // "AllBodyDeclared" set (Step 0 above), just keyed on type instead of "was
  // this declared here" (see NullableRefCollector's own doc comment).
  NullableRefCollector Collector;
  for (size_t b = 0; b < N; ++b)
    for (ExprAST *Stmt : Blocks[b]->getStatements())
      if (Stmt) const_cast<ExprAST *>(static_cast<const ExprAST *>(Stmt))->accept(Collector);
  const auto &Tracked = Collector.Tracked;
  if (Tracked.empty())
    return; // nothing this pass could possibly flag -- skip the walk entirely

  std::unordered_map<const TurfBasicBlock *, size_t> index;
  for (size_t i = 0; i < N; ++i)
    index[Blocks[i].get()] = i;

  // ── Step 1: silent dataflow (no diagnostics) ──────────────────────────────
  // Transfer: process a block's statements sequentially from IN[b], threading
  // OnTrue through every statement but the last (an intermediate statement
  // is never itself a branch condition). The LAST statement's OnTrue/OnFalse
  // become this block's two EDGE-specific outputs when it ends in a genuine
  // 2-way Branch (an `if`/`while`/elseif condition, per CFG.cpp's own
  // construction -- see NullabilityPass's class doc comment for why a
  // >2-successor Branch, e.g. `compare`, is safe to treat the same way: its
  // own condition/subject never narrows, so both slots end up identical
  // regardless of which one a given successor index actually reads).
  // Meet at merge points: set intersection (meetNonNull), same shape as
  // DefiniteAssignmentPass's own fixed point above.
  TurfBasicBlock *Entry = Cfg.getEntryBlock();
  size_t entryIdx = Entry ? index[Entry] : 0;

  std::vector<NonNullSet> IN(N);
  std::vector<NonNullSet> SingleOut(N), TrueOut(N), FalseOut(N);
  std::vector<bool> HasEdgeSplit(N, false);

  auto computeBlockOutputs = [&](size_t b, bool DoReport) {
    NonNullSet Cur = IN[b];
    const auto &Stmts = Blocks[b]->getStatements();
    for (size_t s = 0; s < Stmts.size(); ++s) {
      bool IsLast = (s + 1 == Stmts.size());
      auto Result = evalStatement(Stmts[s], Cur, Tracked, DoReport);
      if (IsLast) {
        SingleOut[b] = Result.OnTrue;
        bool Split = Blocks[b]->getTerminator() == TerminatorKind::Branch &&
                     Blocks[b]->getSuccessors().size() == 2;
        HasEdgeSplit[b] = Split;
        if (Split) {
          TrueOut[b] = Result.OnTrue;
          FalseOut[b] = Result.OnFalse;
        }
      }
      Cur = Result.OnTrue;
    }
    if (Stmts.empty()) {
      SingleOut[b] = Cur;
      HasEdgeSplit[b] = false;
    }
  };

  for (size_t i = 0; i < N; ++i)
    computeBlockOutputs(i, /*DoReport=*/false);

  auto edgeSetFor = [&](size_t p, size_t succIdx) -> const NonNullSet & {
    if (HasEdgeSplit[p])
      return succIdx == 0 ? TrueOut[p] : FalseOut[p];
    return SingleOut[p];
  };

  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t b = 0; b < N; ++b) {
      if (b == entryIdx)
        continue;

      NonNullSet newIN;
      bool hasPred = false;
      bool first = true;
      for (size_t p = 0; p < N; ++p) {
        const auto &Succs = Blocks[p]->getSuccessors();
        for (size_t si = 0; si < Succs.size(); ++si) {
          if (!index.count(Succs[si]) || index.at(Succs[si]) != b)
            continue;
          hasPred = true;
          const NonNullSet &Contribution = edgeSetFor(p, si);
          newIN = first ? Contribution : meetNonNull(newIN, Contribution);
          first = false;
        }
      }
      if (!hasPred)
        continue;

      if (newIN != IN[b]) {
        IN[b] = std::move(newIN);
        computeBlockOutputs(b, /*DoReport=*/false);
        changed = true;
      }
    }
  }

  // ── Step 2: report, using the fixed-point IN sets ─────────────────────────
  for (size_t b = 0; b < N; ++b)
    computeBlockOutputs(b, /*DoReport=*/true);
}

// ═════════════════════════════════════════════════════════════════════════
// FDN-002 -- EscapePass (docs/development/changes/FDN-002.md, FREEZE.md
// §6.13/F-MEM-5). See EscapePass's own class doc comment, Analysis.h, for
// the rule; this is the implementation.
// ═════════════════════════════════════════════════════════════════════════
namespace {

using ProvMap = std::unordered_map<const VariableSymbol *, RefProvenance>;

// A parameter (including `self`) or unsized-array (`T[]`) parameter is the
// only kind of symbol this pass ever gives an IMPLICIT provenance to (Param)
// when it has no explicit entry in the current map -- everything else
// (an ordinary local that hasn't been assigned yet on this path) is
// genuinely Unknown until something assigns it. `Type->isPointer()` covers
// &T/&?T (TurfReferenceType/TurfNullableReferenceType both derive from
// TurfPointerType) and `self`/raw T* (plain TurfPointerType) uniformly --
// see TurfReferenceType's own doc comment, Types.h, for why that hierarchy
// exists.
bool isUnsizedArrayParam(const VariableSymbol *Sym) {
  if (!Sym || !Sym->IsParameter)
    return false;
  const auto *AT = dynamic_cast<const ArrayType *>(Sym->Type.get());
  return AT && AT->getSize() < 0;
}

bool isTrackableHandle(const VariableSymbol *Sym) {
  if (!Sym || !Sym->Type)
    return false;
  if (Sym->Type->isPointer())
    return true; // &T, &?T, self, raw T*
  return isUnsizedArrayParam(Sym);
}

// A cast destination that "loses" the type system's memory of where a
// pointer aliases -- `ptr` itself, or a raw `T*` (FREEZE.md §6.6 groups
// both in the same "raw-and-gated" row). Never true for &T/&?T, which stay
// tracked through a widening cast.
bool isRawPointerDest(const TurfType *T) {
  return T && T->isPointer() && !T->isReference() && !T->isNullableReference();
}

std::optional<RefProvenance> lookupProv(const VariableSymbol *Sym,
                                        const ProvMap &Map) {
  if (!Sym)
    return std::nullopt;
  auto It = Map.find(Sym);
  if (It != Map.end())
    return It->second;
  if (Sym->IsParameter && isTrackableHandle(Sym))
    return RefProvenance::Param; // untouched parameter -- still what the caller gave us
  return std::nullopt;
}

std::optional<RefProvenance> classifyProvenance(const ExprAST *E, const ProvMap &Map);

// The operand of `&expr`, where `expr` is a (possibly nested) field access
// chain rooted at a plain variable. Two genuinely different cases:
//   - the ROOT variable is itself a handle (&T/&?T/self/T[] param): the
//     address lands inside whatever THAT handle already points to, so the
//     result is the handle's OWN provenance (tracked, or its implicit
//     Param default) -- `&param.field` through a `param: &Node` never
//     touches this frame's storage at all.
//   - the ROOT variable is a plain value (a local, or a BY-VALUE parameter
//     with no `&`): its storage IS this frame's own stack slot, so `&`
//     anything reached through it -- the variable itself, or one of its
//     fields -- is Stack, full stop.
// Anything whose root isn't a plain variable at all (e.g. `&f().field`) is
// Unknown: this pass never chases through a call's return value.
//
// Deliberately does NOT recurse through ArrayAccessExprAST (`&expr[i]`),
// unlike the FieldAccessExprAST case just above -- verified live while
// sweeping lib/std/showcase for regressions (FDN-002.md's own account):
// `&l.data[1]` (tests/41_gc/03_interior_pointer_retention.tr) indexes
// `l.data`, a raw `T*` that is ALWAYS itself heap-allocated (every stdlib
// collection's backing buffer) -- the '[i]' there is pointer arithmetic
// through an already-dereferenced handle, not an address inside the ROOT
// variable `l`'s own stack storage, but this pass has no per-field type
// information to tell that apart from a genuine `&fixedArr[i]` on a plain
// local `T[N]`. Rather than guess, `&expr[i]` for ANY base is Unknown --
// the documented residual gap (INV-MEM-3): a dangling `&localFixedArr[i]`
// returned directly is NOT caught by this milestone.
std::optional<RefProvenance> classifyAddressOfProvenance(const ExprAST *E,
                                                          const ProvMap &Map) {
  if (const auto *FA = dynamic_cast<const FieldAccessExprAST *>(E))
    return classifyAddressOfProvenance(FA->getBase(), Map);
  if (const auto *VE = dynamic_cast<const VariableExprAST *>(E)) {
    const VariableSymbol *Sym = VE->getResolvedSymbol();
    if (!Sym)
      return std::nullopt;
    if (isTrackableHandle(Sym))
      return lookupProv(Sym, Map);
    return RefProvenance::Stack; // a plain value var's OWN storage is this frame's
  }
  return std::nullopt;
}

// Classifies the provenance of an arbitrary expression's VALUE, when that
// value is itself reference-shaped (an &T/&?T/raw-pointer result, or an
// unsized-array-to-ptr cast). Deliberately a small, closed set of shapes --
// see EscapePass's class doc comment (Analysis.h) and FDN-002.md's "Known
// pitfalls": anything not listed here is Unknown, on purpose, never a false
// positive.
std::optional<RefProvenance> classifyProvenance(const ExprAST *E,
                                                 const ProvMap &Map) {
  if (!E)
    return std::nullopt;

  if (const auto *AO = dynamic_cast<const AddressOfExprAST *>(E))
    return classifyAddressOfProvenance(AO->getOperand(), Map);

  if (const auto *VE = dynamic_cast<const VariableExprAST *>(E))
    return lookupProv(VE->getResolvedSymbol(), Map);

  if (const auto *FC = dynamic_cast<const FuncCallExprAST *>(E)) {
    if (const auto *Callee = dynamic_cast<const VariableExprAST *>(FC->getCallee()))
      if (Callee->getName() == "heap")
        return RefProvenance::Heap; // FDN-001: always a fresh GC-heap block
    return std::nullopt; // no interprocedural summaries -- an ordinary call's result is Unknown
  }

  if (const auto *CE = dynamic_cast<const CastExprAST *>(E)) {
    std::optional<RefProvenance> Inner = classifyProvenance(CE->getOperand(), Map);
    bool ToRawPtr = isRawPointerDest(CE->getDestType());
    // D23 (§27, absorbed into FDN-002): casting an unsized T[] parameter to
    // a raw pointer loses the type system's only memory of where it
    // aliases -- treat the CAST's own result as Stack (unsafe to let
    // escape) even though the operand itself was Param. The one exception
    // carved out of the "raw-pointer casts exit tracking" rule just below,
    // because THIS conversion is exactly what erases the array's aliasing
    // information in the first place.
    if (ToRawPtr && Inner == RefProvenance::Param) {
      if (const auto *InnerVE = dynamic_cast<const VariableExprAST *>(CE->getOperand()))
        if (isUnsizedArrayParam(InnerVE->getResolvedSymbol()))
          return RefProvenance::Stack;
    }
    // Every other cast INTO the raw/unsafe-gated pointer world (`to ptr`,
    // `to T*`) exits this pass's tracking entirely, regardless of the
    // operand's own provenance -- `unsafe` is the user's own explicit
    // opt-out of the compiler's safety net (FREEZE.md §6.6's raw-pointer
    // row is a separate, ungoverned category), not a hole for EscapePass
    // to keep policing. Verified live: without this,
    // `tests/38_monomorphization/05_struct_multi_instantiation.tr`'s
    // `ptrs.push(&pt1 to Point*)` (inside `unsafe { }`, pt1 and ptrs both
    // local to and fully consumed within the SAME frame) false-positived --
    // a real regression this fix removes. An ordinary &T -> &?T widening
    // (never raw) keeps the operand's real provenance via `return Inner`.
    if (ToRawPtr)
      return std::nullopt;
    return Inner;
  }

  return std::nullopt;
}

// The fixed, curated allowlist FDN-002.md's "Known pitfalls" warns about --
// a new stdlib collection or a new mutator on an existing one needs an
// entry added here BY HAND, or this pass silently won't check it. Matched
// by (receiver's struct/generic base name, method name), not SourceModule
// -- verified live while writing this: struct-bound method calls (as
// opposed to a UFCS free-function call) resolve through a per-type method
// table that never populates ResolverFunctionSymbol::SourceModule at all
// (it stays "" regardless of which module declared the method), so that
// field is USELESS for this check. The self parameter's type IS reliable:
// `fn<T> List.push(item: T)`'s `self` is always synthesized as a raw
// pointer to the receiver's own type (self's own exemption, FREEZE.md
// §6.6), and FDN-003 module-scoped identity means its GenericType's
// BaseName is qualified ("list::List<&Node>*" for `lst.push(&local)` in
// this pass's own test, confirmed by direct inspection) -- stripping the
// "module::" prefix leaves exactly the receiver's bare struct name, stable
// across ordinary calls AND monomorphized generic clones alike (the clone
// keeps the same GenericType wrapper, just with concrete type arguments).
std::string stripModuleQualifier(const std::string &Name) {
  size_t Pos = Name.rfind("::");
  return Pos == std::string::npos ? Name : Name.substr(Pos + 2);
}

bool isCollectionMutatorCall(const MethodCallExprAST &Node) {
  const ResolverFunctionSymbol *Target = Node.getResolvedCallTarget();
  if (!Target || Target->Params.empty() || !Target->Params[0].Type)
    return false;
  static const std::unordered_set<std::string> MutatorNames = {
      "push", "set", "put", "add", "enqueue"};
  if (!MutatorNames.count(Node.getMethod()))
    return false;

  const TurfType *SelfTy = Target->Params[0].Type.get();
  if (const auto *PtrTy = dynamic_cast<const TurfPointerType *>(SelfTy))
    SelfTy = PtrTy->getPointee().get();
  const auto *GT = dynamic_cast<const GenericType *>(SelfTy);
  if (!GT)
    return false;

  static const std::unordered_set<std::string> MutatorStructs = {
      "List", "Map", "Set", "Stack", "Queue"};
  return MutatorStructs.count(stripModuleQualifier(GT->getBaseName()));
}

// Walks every statement, both TRANSFERRING provenance (VarDecl/plain-
// variable Assignment) and, when DoReport, checking every sink FDN-002.md
// lists. A recursive ASTVisitor over one statement at a time -- same shape
// NarrowingVisitor (LANG-002, above in this file) already uses for the
// identical reason: this codebase's existing tree-walking idiom for a CFG
// pass, not the dynamic_cast-chain style CFG.cpp itself uses.
class EscapeCheckVisitor : public ASTVisitor {
  ProvMap &Map;
  bool DoReport;

  void recurse(const ExprAST *E) {
    if (!E)
      return;
    const_cast<ExprAST *>(E)->accept(*this);
  }

  void flagIfStack(const ExprAST *ValueExpr, SourceLocation Loc,
                   const std::string &SinkDescription) {
    if (!DoReport)
      return;
    if (classifyProvenance(ValueExpr, Map) == RefProvenance::Stack)
      DanglingReferenceEscapeError(Loc, SinkDescription).raise();
  }

public:
  EscapeCheckVisitor(ProvMap &Map, bool DoReport) : Map(Map), DoReport(DoReport) {}

  void visitVarDecl(VarDeclExprAST &Node) override {
    recurse(Node.getInitializer());
    const VariableSymbol *Sym = Node.getResolvedSymbol();
    if (Sym && isTrackableHandle(Sym) && Node.getInitializer()) {
      auto P = classifyProvenance(Node.getInitializer(), Map);
      if (P)
        Map[Sym] = *P;
      else
        Map.erase(Sym);
    }
  }
  void visitArrayDecl(ArrayDeclExprAST &Node) override {
    for (const auto &I : Node.getInitList())
      recurse(I.get());
    if (Node.getInitExpr())
      recurse(Node.getInitExpr().get());
  }
  void visitAssignment(AssignmentExprAST &Node) override {
    recurse(Node.getLHS());
    recurse(Node.getRHS());

    if (const auto *FA = dynamic_cast<const FieldAccessExprAST *>(Node.getLHS())) {
      // Sink: storing a Stack-provenance value into a field reached through
      // a Param- or Heap-provenance base. A base this pass can't classify
      // (Unknown) is NOT flagged -- see the class doc comment's residual
      // gap (a field two hops deep is one such case).
      auto BaseProv = classifyProvenance(FA->getBase(), Map);
      if (BaseProv == RefProvenance::Param || BaseProv == RefProvenance::Heap)
        flagIfStack(Node.getRHS(), Node.getLoc(),
                    "stored into a field that outlives this function");
      return;
    }
    if (const auto *AA = dynamic_cast<const ArrayAccessExprAST *>(Node.getLHS())) {
      // Sink: a real `operator[]=` dispatch (LANG-001) -- unconditional,
      // regardless of the collection's OWN provenance, for the same reason
      // isCollectionMutatorCall() below is unconditional (its backing
      // storage always outlives this one expression). An ordinary built-in
      // array/fixed-array write (no ResolvedIndexSetTarget) is NOT a sink:
      // a stack array that never itself escapes is not made unsafe by
      // storing a same-frame reference into it.
      if (AA->ResolvedIndexSetTarget)
        flagIfStack(Node.getRHS(), Node.getLoc(), "pushed into a collection");
      return;
    }
    if (const auto *VE = dynamic_cast<const VariableExprAST *>(Node.getLHS())) {
      // Ordinary reassignment -- TRANSFER, not a sink.
      const VariableSymbol *Sym = VE->getResolvedSymbol();
      if (Sym && isTrackableHandle(Sym)) {
        auto P = classifyProvenance(Node.getRHS(), Map);
        if (P)
          Map[Sym] = *P;
        else
          Map.erase(Sym);
      }
    }
  }
  void visitCompoundAssignment(CompoundAssignmentExprAST &Node) override {
    recurse(Node.getLHS());
    recurse(Node.getRHS());
  }
  void visitReturn(ReturnExprAST &Node) override {
    recurse(Node.getVal());
    if (Node.getVal())
      flagIfStack(Node.getVal(), Node.getLoc(), "returned");
  }
  void visitFuncCall(FuncCallExprAST &Node) override {
    recurse(Node.getCallee());
    for (const auto &A : Node.getArgs())
      recurse(A.get());
    // Sink: heap()'s own operand. Only ever fires when the operand is
    // ITSELF reference-shaped (heap(&local), or heap(x) where x is already
    // a &T/&?T/raw pointer) -- classifyProvenance() returns Unknown for a
    // plain by-value struct/primitive operand, which is exactly right: this
    // pass does not track provenance through a struct's OWN fields (the
    // documented residual gap), so `heap(structWithADanglingField)` is not
    // caught here.
    if (DoReport) {
      if (const auto *Callee = dynamic_cast<const VariableExprAST *>(Node.getCallee()))
        if (Callee->getName() == "heap" && !Node.getArgs().empty())
          flagIfStack(Node.getArgs()[0].get(), Node.getLoc(), "boxed with heap()");
    }
  }
  void visitMethodCall(MethodCallExprAST &Node) override {
    recurse(Node.getBase());
    for (const auto &A : Node.getArgs())
      recurse(A.get());
    if (DoReport && isCollectionMutatorCall(Node)) {
      for (const auto &A : Node.getArgs())
        flagIfStack(A.get(), Node.getLoc(), "pushed into a collection");
    }
  }
  void visitPostfixExpr(PostfixExprAST &Node) override { recurse(Node.getOperand()); }
  void visitUnary(UnaryExprAST &Node) override { recurse(Node.getOperand()); }
  void visitAddressOf(AddressOfExprAST &Node) override { recurse(Node.getOperand()); }
  void visitCast(CastExprAST &Node) override { recurse(Node.getOperand()); }
  void visitBinary(BinaryExprAST &Node) override {
    recurse(Node.getLHS());
    recurse(Node.getRHS());
  }
  void visitFieldAccess(FieldAccessExprAST &Node) override { recurse(Node.getBase()); }
  void visitArrayAccess(ArrayAccessExprAST &Node) override {
    recurse(Node.getBase());
    recurse(Node.getIndex());
  }
  void visitArrayLiteral(ArrayLiteralExprAST &Node) override {
    for (const auto &E : Node.getElements())
      recurse(E.get());
  }
  void visitCompare(CompareExprAST &Node) override {
    recurse(Node.getSubject());
    for (const auto &Arm : Node.getArms()) {
      for (const auto &V : Arm.Values)
        recurse(V.get());
      // A guard is an ordinary expression and can hold anything an `if`
      // condition can -- including a call that takes `&local`.
      for (const auto &G : Arm.Guards)
        recurse(G.get());
      recurse(Arm.Body.get());
    }
    recurse(Node.getOtherBody());
  }
};

ProvMap meetProv(const ProvMap &A, const ProvMap &B) {
  ProvMap R;
  for (const auto &KV : A) {
    auto It = B.find(KV.first);
    if (It != B.end() && It->second == KV.second)
      R.emplace(KV.first, KV.second);
  }
  return R;
}

} // namespace

void EscapePass::runOnCFG(CFG &Cfg) {
  const auto &Blocks = Cfg.getBlocks();
  size_t N = Blocks.size();
  if (N == 0)
    return;

  std::unordered_map<const TurfBasicBlock *, size_t> index;
  for (size_t i = 0; i < N; ++i)
    index[Blocks[i].get()] = i;

  TurfBasicBlock *Entry = Cfg.getEntryBlock();
  size_t entryIdx = Entry ? index[Entry] : 0;

  std::vector<ProvMap> IN(N), OUT(N);

  auto computeOut = [&](size_t b, bool DoReport) {
    ProvMap Cur = IN[b];
    for (ExprAST *Stmt : Blocks[b]->getStatements()) {
      if (!Stmt)
        continue;
      EscapeCheckVisitor V(Cur, DoReport);
      const_cast<ExprAST *>(static_cast<const ExprAST *>(Stmt))->accept(V);
    }
    OUT[b] = std::move(Cur);
  };

  // ── Step 1: silent dataflow to a fixed point ──────────────────────────────
  for (size_t i = 0; i < N; ++i)
    computeOut(i, /*DoReport=*/false);

  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t b = 0; b < N; ++b) {
      if (b == entryIdx)
        continue;

      ProvMap newIN;
      bool first = true;
      bool hasPred = false;
      for (size_t p = 0; p < N; ++p) {
        const auto &Succs = Blocks[p]->getSuccessors();
        for (auto *S : Succs) {
          if (!index.count(S) || index.at(S) != b)
            continue;
          hasPred = true;
          newIN = first ? OUT[p] : meetProv(newIN, OUT[p]);
          first = false;
        }
      }
      if (!hasPred)
        continue;

      if (newIN != IN[b]) {
        IN[b] = std::move(newIN);
        computeOut(b, /*DoReport=*/false);
        changed = true;
      }
    }
  }

  // ── Step 2: report, using the fixed-point IN sets ─────────────────────────
  for (size_t b = 0; b < N; ++b)
    computeOut(b, /*DoReport=*/true);
}
