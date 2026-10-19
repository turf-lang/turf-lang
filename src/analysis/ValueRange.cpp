#include "analysis/ValueRange.h"
#include "frontend/AST.h"
#include "frontend/ASTChildWalker.h"
#include "diagnostics/Errors.h"

#include "llvm/ADT/APInt.h"

#include <climits>
#include <functional>
#include <optional>
#include <queue>
#include <unordered_map>
#include <unordered_set>

// Use raw VariableSymbol* as the constant-tracking key, mirroring the fix
// already applied to DefiniteAssignmentPass (see Analysis.cpp). This pass
// runs inside FuncDefExprAST::codegen() AFTER Body->codegen() has already
// fully executed -- which means every EnterScope()/ExitScope() pair opened
// by nested BlockExprAST::codegen() calls (including the function's own
// top-level body block, see BlockExprAST::codegen()) has already unwound by
// the time this pass runs. GlobalSymbolTable->LookupSymbol(name) walks the
// *current* scope stack, so at this point it can no longer find ANY local
// variable of the function being analyzed -- not just ones declared in
// nested blocks, but even top-level ones, since the body's own block scope
// has already been popped. That silently disabled this pass's constant
// tracking (and therefore its compile-time out-of-bounds detection)
// entirely: `mut i: int = 5; arr[i]` at the very top of a function body
// never fired a diagnostic, only the separate runtime bounds check did.
// VariableSymbol* (populated by the Resolver via ResolvedSymbol on the AST
// node itself) has no such lifetime issue -- it's a shared_ptr-owned object
// that lives for as long as the AST does, independent of the SymbolTable's
// scope stack.
using ConstMap = std::unordered_map<const VariableSymbol *, std::optional<long long>>;

static std::optional<long long> evalConstExpr(const ExprAST *E, const ConstMap &M) {
  if (!E) return std::nullopt;
  if (auto N = dynamic_cast<const NumberExprAST *>(E)) {
    if (N->isInteger()) return N->getIntVal();
    return std::nullopt;
  }
  if (auto V = dynamic_cast<const VariableExprAST *>(E)) {
    const VariableSymbol *S = V->getResolvedSymbol();
    if (!S) return std::nullopt;
    auto it = M.find(S);
    if (it != M.end()) return it->second;
    return std::nullopt;
  }
  if (auto B = dynamic_cast<const BinaryExprAST *>(E)) {
    auto L = evalConstExpr(B->getLHS(), M);
    auto R = evalConstExpr(B->getRHS(), M);
    if (!L.has_value() || !R.has_value()) return std::nullopt;
    // SEM-001 (docs/archive/ARCHITECTURE_OVERHAUL.md §8): this used to be
    // plain `long long lv + rv` -- signed-overflow UB in the compiler's
    // OWN C++, confirmed by reading this exact line before the fix (not
    // hypothetical: e.g. two folded constants near INT64_MAX/INT64_MIN
    // feeding an array index expression could hit it for real). Checked
    // llvm::APSInt arithmetic replaces it: on provable overflow, this pass
    // gives up on the value (returns nullopt, i.e. "unknown from here"),
    // the same conservative fallback already used for division by zero
    // just below -- SEM-001 does not add a NEW diagnostic for compile-time-
    // provable arithmetic overflow (representability/overflow ERRORS are
    // out of this group's scope entirely, see its own "Desired behavior"
    // item 4); this only removes the UB, it does not change what this pass
    // reports. ArrayBoundsError (this pass's only consumer of the returned
    // value) still fires exactly when it did before: a provably-out-of-
    // range constant index. Widened to 65 bits (one more than `long long`
    // needs) before the checked op so the SIGNED overflow flag genuinely
    // means "the true mathematical result doesn't fit in 64 bits", not an
    // artifact of computing at exactly 64.
    llvm::APInt lv(65, static_cast<uint64_t>(*L), /*isSigned=*/true);
    llvm::APInt rv(65, static_cast<uint64_t>(*R), /*isSigned=*/true);
    bool Overflow = false;
    llvm::APInt Result;
    switch (B->getOp()) {
    case '+': Result = lv.sadd_ov(rv, Overflow); break;
    case '-': Result = lv.ssub_ov(rv, Overflow); break;
    case '*': Result = lv.smul_ov(rv, Overflow); break;
    case '/':
      if (rv.isZero()) return std::nullopt;
      Result = lv.sdiv_ov(rv, Overflow);
      break;
    default: return std::nullopt;
    }
    if (Overflow || !Result.isSignedIntN(64)) return std::nullopt;
    return Result.getSExtValue();
  }
  if (auto A = dynamic_cast<const AssignmentExprAST *>(E)) {
    return evalConstExpr(A->getRHS(), M);
  }
  // For other nodes, conservatively unknown
  return std::nullopt;
}

// ─── Which variables the dataflow may hold a constant for ──────────────────
// The transfer function (applyTransfer() below) understands exactly four
// writes, each only as a whole CFG statement: `mut x = e`, `x = e`, `x op= e`
// and `x++`/`x--`. A variable written any other way -- an `x++` inside a call
// argument, `x = e` inside a `defer` (whose statement runs at scope exit, not
// where the CFG lists it), `&x` handed to a function, a global another
// function assigns -- changes without the dataflow seeing it, and a constant
// kept for it is stale. For the bounds diagnostic a stale constant rejects a
// valid program; for OPT-002 it removes a runtime check that was needed, so
// an overflowing `x + 1` wraps (and, as `nsw`, is poison to LLVM) and an
// out-of-range `a[i]` reads past the array.
//
// The earlier design trusted every variable and listed the writes it knew
// were unsafe (`&x`, then `&x` inside `try`, then inside `recover`), so each
// construct nobody had listed reopened the hole. This scan inverts that: a
// variable is tracked only when it is declared by a whole-statement `mut x`
// in this function and EVERY occurrence of it anywhere in the function is
// either one of the four handled writes as a whole statement or a plain read
// in a position known to be a read. Any other occurrence, including one in a
// node kind added later, rejects the variable (visitVariable() below is the
// default for an occurrence no parent claimed). Precision lost this way
// costs a runtime check that stays in, never a wrong answer.
namespace {
struct TrackedVars {
  // Declared here, every occurrence accounted for: constants may be kept.
  std::unordered_set<const VariableSymbol *> Trackable;
  // Seen in a position the scan does not recognise as a read or a handled
  // write (always contains every variable whose address is taken).
  std::unordered_set<const VariableSymbol *> Tainted;
};

class TrackabilityScan : public ASTChildWalker {
  const ExprAST *Root = nullptr; // the CFG statement being scanned
  std::unordered_set<const VariableSymbol *> &Declared;
  std::unordered_set<const VariableSymbol *> &Tainted;

  void taint(const ExprAST *E) {
    if (auto *V = dynamic_cast<const VariableExprAST *>(E))
      if (const VariableSymbol *S = V->getResolvedSymbol())
        Tainted.insert(S);
  }
  // A child whose value is only read: a bare variable there is a read, and
  // anything else is scanned.
  void read(const ExprAST *C) {
    if (C && !dynamic_cast<const VariableExprAST *>(C))
      visitChild(C);
  }
  // The target of `=`/`op=`/`++`: fine as a whole statement, which the
  // transfer function sees; anywhere else it is a write the dataflow misses.
  void writeTarget(const ExprAST *Stmt, const ExprAST *Target) {
    if (dynamic_cast<const VariableExprAST *>(Target)) {
      if (Stmt != Root)
        taint(Target);
    } else {
      visitChild(Target); // `a[i] = v`, `s.f = v`: the base is not rebound
    }
  }

public:
  TrackabilityScan(std::unordered_set<const VariableSymbol *> &Declared,
                   std::unordered_set<const VariableSymbol *> &Tainted)
      : Declared(Declared), Tainted(Tainted) {}

  void scanStatement(ExprAST *Stmt) {
    Root = Stmt;
    visitChild(Stmt);
  }

  void visitVariable(VariableExprAST &Node) override { taint(&Node); }

  void visitVarDecl(VarDeclExprAST &Node) override {
    if (&Node == Root)
      if (const VariableSymbol *S = Node.getResolvedSymbol())
        Declared.insert(S);
    read(Node.getInitializer());
  }
  void visitAssignment(AssignmentExprAST &Node) override {
    writeTarget(&Node, Node.getLHS());
    read(Node.getRHS());
  }
  void visitCompoundAssignment(CompoundAssignmentExprAST &Node) override {
    writeTarget(&Node, Node.getLHS());
    read(Node.getRHS());
  }
  void visitPostfixExpr(PostfixExprAST &Node) override {
    writeTarget(&Node, Node.getOperand());
  }

  void visitBinary(BinaryExprAST &Node) override {
    read(Node.getLHS());
    read(Node.getRHS());
  }
  void visitUnary(UnaryExprAST &Node) override { read(Node.getOperand()); }
  void visitCast(CastExprAST &Node) override { read(Node.getOperand()); }
  void visitReturn(ReturnExprAST &Node) override { read(Node.getVal()); }
  // Arguments are passed by value: a callee can only change a caller's
  // variable through `&x`, which reaches visitVariable() via visitAddressOf().
  void visitFuncCall(FuncCallExprAST &Node) override {
    visitChild(Node.getCallee());
    for (const auto &Arg : Node.getArgs())
      read(Arg.get());
  }
  void visitMethodCall(MethodCallExprAST &Node) override {
    visitChild(Node.getBase()); // a receiver may be passed by reference
    for (const auto &Arg : Node.getArgs())
      read(Arg.get());
  }
  // An element access never rebinds the array, so its base is a read.
  void visitArrayAccess(ArrayAccessExprAST &Node) override {
    read(Node.getBase());
    read(Node.getIndex());
  }
  void visitArrayLiteral(ArrayLiteralExprAST &Node) override {
    for (const auto &Elem : Node.getElements())
      read(Elem.get());
  }
};
} // namespace

static TrackedVars computeTrackedVars(const CFG &Cfg) {
  std::unordered_set<const VariableSymbol *> Declared;
  TrackedVars TV;
  TrackabilityScan Scan(Declared, TV.Tainted);
  // Every statement CFGBuilder placed in a block: between them they hold the
  // whole function body (CFGBuilder never splits one AST statement across
  // blocks, and a `defer`'s statement is reached inside its DeferExprAST,
  // where it is never the root).
  for (const auto &BB : Cfg.getBlocks())
    for (ExprAST *Stmt : BB->getStatements())
      Scan.scanStatement(Stmt);
  for (const VariableSymbol *S : Declared)
    if (!TV.Tainted.count(S))
      TV.Trackable.insert(S);
  return TV;
}

// The dataflow transfer for one top-level CFG statement, shared by the
// diagnostic pass and OPT-002 and by each pass's convergence loop and final
// walk, so none of them can drift apart. Only a Trackable variable ever gets
// an entry, so evalConstExpr() can never read a constant for any other.
// `op=` and `++`/`--` forget the value rather than compute it: losing that
// precision only keeps a check.
static void applyTransfer(ExprAST *Stmt, ConstMap &Cur, const TrackedVars &TV) {
  auto Set = [&](const VariableSymbol *S, std::optional<long long> V) {
    if (S && TV.Trackable.count(S)) Cur[S] = V;
  };
  if (auto *Vd = dynamic_cast<VarDeclExprAST *>(Stmt)) {
    Set(Vd->getResolvedSymbol(), evalConstExpr(Vd->getInitializer(), Cur));
    return;
  }
  if (auto *A = dynamic_cast<AssignmentExprAST *>(Stmt)) {
    if (auto *L = dynamic_cast<const VariableExprAST *>(A->getLHS()))
      Set(L->getResolvedSymbol(), evalConstExpr(A->getRHS(), Cur));
    return;
  }
  if (auto *CA = dynamic_cast<CompoundAssignmentExprAST *>(Stmt)) {
    if (auto *L = dynamic_cast<const VariableExprAST *>(CA->getLHS()))
      Set(L->getResolvedSymbol(), std::nullopt);
    return;
  }
  if (auto *P = dynamic_cast<PostfixExprAST *>(Stmt)) {
    if (auto *V = dynamic_cast<const VariableExprAST *>(P->getOperand()))
      Set(V->getResolvedSymbol(), std::nullopt);
    return;
  }
}

// Forward constant propagation to a fixed point; returns each block's IN[].
// A variable keeps a constant at a join only when every predecessor agrees
// on it, and an entry absent from a map means "unknown".
static std::vector<ConstMap> solveConstants(const CFG &Cfg, const TrackedVars &TV) {
  const auto &Blocks = Cfg.getBlocks();
  size_t N = Blocks.size();
  std::unordered_map<const TurfBasicBlock *, size_t> index;
  for (size_t i = 0; i < N; ++i) index[Blocks[i].get()] = i;

  std::vector<ConstMap> IN(N), OUT(N);
  std::queue<size_t> work;
  for (size_t i = 0; i < N; ++i) work.push(i);

  while (!work.empty()) {
    size_t b = work.front(); work.pop();
    TurfBasicBlock *BB = Blocks[b].get();

    ConstMap NewIN;
    const auto &Preds = BB->getPredecessors();
    if (Preds.empty()) {
      NewIN = IN[b];
    } else {
      bool First = true;
      for (auto *P : Preds) {
        size_t Pi = index[P];
        if (First) { NewIN = OUT[Pi]; First = false; }
        else {
          ConstMap Tmp;
          for (const auto &Kv : NewIN) {
            auto It = OUT[Pi].find(Kv.first);
            if (It != OUT[Pi].end() && Kv.second.has_value() && It->second.has_value() && Kv.second == It->second)
              Tmp[Kv.first] = Kv.second;
            else
              Tmp[Kv.first] = std::nullopt;
          }
          NewIN.swap(Tmp);
        }
      }
    }
    if (NewIN != IN[b]) IN[b] = NewIN;

    ConstMap Cur = IN[b];
    for (ExprAST *Stmt : BB->getStatements()) applyTransfer(Stmt, Cur, TV);

    if (Cur != OUT[b]) {
      OUT[b] = Cur;
      for (auto *S : BB->getSuccessors()) work.push(index[S]);
    }
  }
  return IN;
}

// A compile-time out-of-range constant index (E001), reported once the
// dataflow has converged, so every report comes from a value proven on every
// path reaching the access.
static void reportConstantOutOfBounds(const ExprAST *N, const ConstMap &cur) {
  if (!N) return;
  if (auto AA = dynamic_cast<const ArrayAccessExprAST *>(N)) {
    auto idxVal = evalConstExpr(AA->getIndex(), cur);
    if (idxVal.has_value()) {
      if (auto BaseVar = dynamic_cast<const VariableExprAST *>(AA->getBase())) {
        const VariableSymbol *S = BaseVar->getResolvedSymbol();
        if (S && S->Type && S->Type->isArray()) {
          const ArrayType *AT = dynamic_cast<const ArrayType *>(S->Type.get());
          if (AT) {
            int sz = AT->getSize();
            if (sz > 0) {
              long long iv = *idxVal;
              // #256: negative indices count from the end, so the
              // valid range is -sz <= iv < sz (CodegenArray.cpp).
              if (iv < -static_cast<long long>(sz) || iv >= sz) {
                ArrayBoundsError err(AA->getLoc(), BaseVar->getName(), iv, sz);
                err.raise();
              }
            }
          }
        }
      }
    }
  }

  // Recurse into children (basic traversal)
  auto checkAccesses = [&](const ExprAST *C) { reportConstantOutOfBounds(C, cur); };
  if (auto B = dynamic_cast<const BinaryExprAST *>(N)) { checkAccesses(B->getLHS()); checkAccesses(B->getRHS()); }
  else if (auto U = dynamic_cast<const UnaryExprAST *>(N)) checkAccesses(U->getOperand());
  else if (auto Tr = dynamic_cast<const TryExprAST *>(N)) checkAccesses(Tr->getOperand()); // LEC-004
  else if (auto A = dynamic_cast<const AssignmentExprAST *>(N)) { checkAccesses(A->getRHS()); checkAccesses(A->getLHS()); }
  else if (auto Vd = dynamic_cast<const VarDeclExprAST *>(N)) checkAccesses(Vd->getInitializer());
  else if (auto If = dynamic_cast<const IfExprAST *>(N)) {
    for (const auto &Br : If->getBranches()) { checkAccesses(Br.Cond.get()); checkAccesses(Br.Body.get()); }
    checkAccesses(If->getElse());
  } else if (auto Bl = dynamic_cast<const BlockExprAST *>(N)) {
    for (const auto &E2 : Bl->getExpressions()) checkAccesses(E2.get());
  } else if (auto F = dynamic_cast<const FuncCallExprAST *>(N)) {
    for (const auto &Arg : F->getArgs()) checkAccesses(Arg.get());
    checkAccesses(F->getCallee());
  } else if (auto ArrA = dynamic_cast<const ArrayAccessExprAST *>(N)) {
    checkAccesses(ArrA->getBase()); checkAccesses(ArrA->getIndex());
  } else if (auto R = dynamic_cast<const ReturnExprAST *>(N)) {
    checkAccesses(R->getVal());
  } else if (auto CA = dynamic_cast<const CompoundAssignmentExprAST *>(N)) {
    checkAccesses(CA->getLHS()); checkAccesses(CA->getRHS());
  } else if (auto P = dynamic_cast<const PostfixExprAST *>(N)) {
    checkAccesses(P->getOperand());
  } else if (auto M = dynamic_cast<const MethodCallExprAST *>(N)) {
    checkAccesses(M->getBase()); for (const auto &Arg : M->getArgs()) checkAccesses(Arg.get());
  } else if (auto FA = dynamic_cast<const FieldAccessExprAST *>(N)) {
    checkAccesses(FA->getBase());
  } else if (auto AL = dynamic_cast<const ArrayLiteralExprAST *>(N)) {
    for (const auto &E2 : AL->getElements()) checkAccesses(E2.get());
  } else if (auto AD = dynamic_cast<const ArrayDeclExprAST *>(N)) {
    for (const auto &E2 : AD->getInitList()) checkAccesses(E2.get());
    if (AD->getInitExpr()) checkAccesses(AD->getInitExpr().get());
  } else if (auto C = dynamic_cast<const CastExprAST *>(N)) {
    checkAccesses(C->getOperand());
  }
}

void ValueRangePass::runOnCFG(CFG &Cfg, const std::vector<SymbolID> & /*InitialAssigned*/) {
  // InitialAssigned (parameter SymbolIDs from the codegen-time SymbolTable)
  // is intentionally unused: parameters are never tracked (see
  // computeTrackedVars()). Kept in the signature to match
  // DefiniteAssignmentPass::runOnCFG's identical (also-unused) parameter.
  const auto &Blocks = Cfg.getBlocks();
  if (Blocks.empty()) return;
  TrackedVars TV = computeTrackedVars(Cfg);
  std::vector<ConstMap> IN = solveConstants(Cfg, TV);
  for (size_t i = 0; i < Blocks.size(); ++i) {
    ConstMap Cur = IN[i];
    for (ExprAST *Stmt : Blocks[i]->getStatements()) {
      reportConstantOutOfBounds(Stmt, Cur);
      applyTransfer(Stmt, Cur, TV);
    }
  }
}

// ─── OPT-002: check-elision pre-pass ───────────────────────────────────────
// (ARCHITECTURE_OVERHAUL.md §8 OPT-002, DESIGN_NOTES.md §1 "Bounds Check
// Elimination"). markElidableChecks() runs the same dataflow as runOnCFG() on
// a throwaway CFG built BEFORE Body->codegen() -- see that call site's own
// comment (CodegenFunc.cpp) for why runOnCFG(), which only ever runs AFTER
// Body->codegen(), cannot be reused for this: its results would describe
// checks Codegen has already emitted.

// Whether checked `op` (`+`/`-`/`*`) applied to two EXACT int64 constants
// can be proven not to overflow -- the same llvm::APInt s{add,sub,mul}_ov
// idiom evalConstExpr()'s own BinaryExprAST case already uses above, reused
// here (rather than calling evalConstExpr recursively) because this needs a
// bool answer for two ALREADY-evaluated operands, not a new folded value.
static bool arithProvablySafe(char Op, long long L, long long R) {
  llvm::APInt LV(65, static_cast<uint64_t>(L), /*isSigned=*/true);
  llvm::APInt RV(65, static_cast<uint64_t>(R), /*isSigned=*/true);
  bool Overflow = false;
  llvm::APInt Result;
  switch (Op) {
  case '+': Result = LV.sadd_ov(RV, Overflow); break;
  case '-': Result = LV.ssub_ov(RV, Overflow); break;
  case '*': Result = LV.smul_ov(RV, Overflow); break;
  default: return false;
  }
  return !Overflow && Result.isSignedIntN(64);
}

// Mirrors EmitCheckedShl()'s own recovery check exactly (CodegenCore.cpp):
// the shift amount must be in [0,64), and shifting the result back right by
// the same amount (arithmetic, sign-preserving) must recover L.
static bool shlProvablySafe(long long L, long long R) {
  if (R < 0 || R >= 64) return false;
  llvm::APInt LV(64, static_cast<uint64_t>(L), /*isSigned=*/true);
  llvm::APInt Shifted = LV.shl(static_cast<unsigned>(R));
  llvm::APInt Recovered = Shifted.ashr(static_cast<unsigned>(R));
  return Recovered == LV;
}

// Mirrors EmitCheckedIntNegate()'s one overflow case (CodegenCore.cpp):
// negating the type's minimum has no representable result. The node's type
// is not known here and an `int32` negation stays 32 bits wide (unlike
// int32 arithmetic, which widens to int), so INT32_MIN is never proved safe
// either: that costs one kept check on an `int`, never a missed one.
static bool negProvablySafe(long long V) { return V != INT64_MIN && V != INT32_MIN; }

// Marks every node this dataflow proves safe. Needs a non-const ExprAST*
// throughout (the entire point is to mutate ProvenInBounds/ProvenNoOverflow),
// so getters whose declared return type is `const ExprAST *` are const_cast
// back -- these are still genuinely non-const objects owned by the same Body
// tree Codegen is about to walk. A node kind with no case here is simply
// never marked, which keeps its checks.
static void markNode(ExprAST *N, const ConstMap &Cur,
                     const TrackedVars &TV) {
  if (!N) return;

  if (auto *AA = dynamic_cast<ArrayAccessExprAST *>(N)) {
    auto IdxVal = evalConstExpr(AA->getIndex(), Cur);
    if (IdxVal.has_value()) {
      if (auto *BaseVar = dynamic_cast<const VariableExprAST *>(AA->getBase())) {
        const VariableSymbol *S = BaseVar->getResolvedSymbol();
        if (S && S->Type && S->Type->isArray() && !TV.Tainted.count(S)) {
          if (const auto *AT = dynamic_cast<const ArrayType *>(S->Type.get())) {
            int Sz = AT->getSize();
            long long Iv = *IdxVal;
            if (Sz > 0 && Iv >= 0 && Iv < Sz)
              AA->ProvenInBounds = true;
          }
        }
      }
    }
    markNode(const_cast<ExprAST *>(AA->getBase()), Cur, TV);
    markNode(const_cast<ExprAST *>(AA->getIndex()), Cur, TV);
    return;
  }

  if (auto *B = dynamic_cast<BinaryExprAST *>(N)) {
    auto Lv = evalConstExpr(B->getLHS(), Cur);
    auto Rv = evalConstExpr(B->getRHS(), Cur);
    if (Lv.has_value() && Rv.has_value()) {
      int Op = B->getOp();
      bool Safe = false;
      if (Op == '+' || Op == '-' || Op == '*')
        Safe = arithProvablySafe(static_cast<char>(Op), *Lv, *Rv);
      else if (Op == TOK_LSHIFT)
        Safe = shlProvablySafe(*Lv, *Rv);
      if (Safe) B->ProvenNoOverflow = true;
    }
    markNode(const_cast<ExprAST *>(B->getLHS()), Cur, TV);
    markNode(const_cast<ExprAST *>(B->getRHS()), Cur, TV);
    return;
  }

  if (auto *U = dynamic_cast<UnaryExprAST *>(N)) {
    if (U->getOpcode() == '-') {
      auto Ov = evalConstExpr(U->getOperand(), Cur);
      if (Ov.has_value() && negProvablySafe(*Ov)) U->ProvenNoOverflow = true;
    }
    markNode(const_cast<ExprAST *>(U->getOperand()), Cur, TV);
    return;
  }

  if (auto *Tr = dynamic_cast<TryExprAST *>(N)) { // LEC-004
    markNode(Tr->getOperand(), Cur, TV);
    return;
  }

  if (auto *CA = dynamic_cast<CompoundAssignmentExprAST *>(N)) {
    int Op = CA->getOpcode();
    if (Op == '+' || Op == '-' || Op == '*' || Op == TOK_LSHIFT) {
      auto Lv = evalConstExpr(CA->getLHS(), Cur);
      auto Rv = evalConstExpr(CA->getRHS(), Cur);
      if (Lv.has_value() && Rv.has_value()) {
        bool Safe = (Op == TOK_LSHIFT) ? shlProvablySafe(*Lv, *Rv)
                                       : arithProvablySafe(static_cast<char>(Op), *Lv, *Rv);
        if (Safe) CA->ProvenNoOverflow = true;
      }
    }
    markNode(const_cast<ExprAST *>(CA->getLHS()), Cur, TV);
    markNode(const_cast<ExprAST *>(CA->getRHS()), Cur, TV);
    return;
  }

  if (auto *P = dynamic_cast<PostfixExprAST *>(N)) {
    auto Ov = evalConstExpr(P->getOperand(), Cur);
    if (Ov.has_value()) {
      bool Safe = (P->getOpcode() == '+') ? arithProvablySafe('+', *Ov, 1)
                                          : arithProvablySafe('-', *Ov, 1);
      if (Safe) P->ProvenNoOverflow = true;
    }
    markNode(const_cast<ExprAST *>(P->getOperand()), Cur, TV);
    return;
  }

  if (auto *A = dynamic_cast<AssignmentExprAST *>(N)) {
    markNode(const_cast<ExprAST *>(A->getRHS()), Cur, TV);
    markNode(const_cast<ExprAST *>(A->getLHS()), Cur, TV);
  } else if (auto *Vd = dynamic_cast<VarDeclExprAST *>(N)) {
    markNode(const_cast<ExprAST *>(Vd->getInitializer()), Cur, TV);
  } else if (auto *If = dynamic_cast<IfExprAST *>(N)) {
    for (const auto &Br : If->getBranches()) {
      markNode(const_cast<ExprAST *>(Br.Cond.get()), Cur, TV);
      markNode(const_cast<ExprAST *>(Br.Body.get()), Cur, TV);
    }
    markNode(const_cast<ExprAST *>(If->getElse()), Cur, TV);
  } else if (auto *Bl = dynamic_cast<BlockExprAST *>(N)) {
    for (const auto &E2 : Bl->getExpressions()) markNode(E2.get(), Cur, TV);
  } else if (auto *F = dynamic_cast<FuncCallExprAST *>(N)) {
    for (const auto &Arg : F->getArgs()) markNode(Arg.get(), Cur, TV);
    markNode(const_cast<ExprAST *>(F->getCallee()), Cur, TV);
  } else if (auto *R = dynamic_cast<ReturnExprAST *>(N)) {
    markNode(const_cast<ExprAST *>(R->getVal()), Cur, TV);
  } else if (auto *M = dynamic_cast<MethodCallExprAST *>(N)) {
    markNode(const_cast<ExprAST *>(M->getBase()), Cur, TV);
    for (const auto &Arg : M->getArgs()) markNode(Arg.get(), Cur, TV);
  } else if (auto *FA = dynamic_cast<FieldAccessExprAST *>(N)) {
    markNode(const_cast<ExprAST *>(FA->getBase()), Cur, TV);
  } else if (auto *AL = dynamic_cast<ArrayLiteralExprAST *>(N)) {
    for (const auto &E2 : AL->getElements()) markNode(E2.get(), Cur, TV);
  } else if (auto *AD = dynamic_cast<ArrayDeclExprAST *>(N)) {
    for (const auto &E2 : AD->getInitList()) markNode(E2.get(), Cur, TV);
    if (AD->getInitExpr()) markNode(const_cast<ExprAST *>(AD->getInitExpr().get()), Cur, TV);
  } else if (auto *C = dynamic_cast<CastExprAST *>(N)) {
    markNode(const_cast<ExprAST *>(C->getOperand()), Cur, TV);
  }
}

void ValueRangePass::markElidableChecks(CFG &Cfg,
                                        const std::vector<SymbolID> & /*InitialAssigned*/) {
  const auto &Blocks = Cfg.getBlocks();
  if (Blocks.empty()) return;
  TrackedVars TV = computeTrackedVars(Cfg);
  // Converge first, then mark in one walk per block from the converged IN[],
  // so every mark comes from dataflow state proven on EVERY path, never a
  // not-yet-converged intermediate estimate.
  std::vector<ConstMap> IN = solveConstants(Cfg, TV);
  for (size_t i = 0; i < Blocks.size(); ++i) {
    ConstMap Cur = IN[i];
    for (ExprAST *Stmt : Blocks[i]->getStatements()) {
      markNode(Stmt, Cur, TV);
      applyTransfer(Stmt, Cur, TV);
    }
  }
}
