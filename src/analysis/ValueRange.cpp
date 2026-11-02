#include "analysis/ValueRange.h"
#include "analysis/AnalysisFacts.h"
#include "diagnostics/LintLevels.h"
#include "frontend/AST.h"
#include "frontend/ASTChildWalker.h"
#include "diagnostics/Errors.h"
#include "frontend/Parser.h" // IsLibStdWarningLocation

#include "llvm/ADT/APInt.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
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
// the time this pass runs. Symbols()->LookupSymbol(name) walks the
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
  // NM-2: the `int` variables the interval analysis keeps a range for: the
  // Trackable ones, plus this function's parameters and range-`for`
  // variables, under the same every-occurrence-accounted-for rule.
  std::unordered_set<const VariableSymbol *> Ranged;
  // Seen in a position the scan does not recognise as a read or a handled
  // write (always contains every variable whose address is taken).
  std::unordered_set<const VariableSymbol *> Tainted;
};

class TrackabilityScan : public ASTChildWalker {
  const ExprAST *Root = nullptr; // the CFG statement being scanned
  std::unordered_set<const VariableSymbol *> &Declared;
  std::unordered_set<const VariableSymbol *> &Tainted;
  std::unordered_set<const VariableSymbol *> &Read;

  void taint(const ExprAST *E) {
    if (auto *V = dynamic_cast<const VariableExprAST *>(E))
      if (const VariableSymbol *S = V->getResolvedSymbol())
        Tainted.insert(S);
  }
  // A child whose value is only read: a bare variable there is a read, and
  // anything else is scanned.
  void read(const ExprAST *C) {
    if (auto *V = dynamic_cast<const VariableExprAST *>(C)) {
      if (const VariableSymbol *S = V->getResolvedSymbol())
        Read.insert(S);
    } else if (C) {
      visitChild(C);
    }
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
                   std::unordered_set<const VariableSymbol *> &Tainted,
                   std::unordered_set<const VariableSymbol *> &Read)
      : Declared(Declared), Tainted(Tainted), Read(Read) {}

  void scanStatement(ExprAST *Stmt) {
    Root = Stmt;
    // A bare variable as a whole statement is a test (`while go`, `if ok`):
    // a read.
    if (dynamic_cast<const VariableExprAST *>(Stmt)) {
      read(Stmt);
      return;
    }
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
  std::unordered_set<const VariableSymbol *> Declared, Read, LoopVars;
  TrackedVars TV;
  TrackabilityScan Scan(Declared, TV.Tainted, Read);
  // Every statement CFGBuilder placed in a block: between them they hold the
  // whole function body (CFGBuilder never splits one AST statement across
  // blocks, and a `defer`'s statement is reached inside its DeferExprAST,
  // where it is never the root), plus each range `for`'s `step`, which runs
  // as a whole statement in the loop's step block.
  for (const auto &BB : Cfg.getBlocks()) {
    for (ExprAST *Stmt : BB->getStatements())
      Scan.scanStatement(Stmt);
    if (BB->ForStep && BB->ForStep->getStep())
      Scan.scanStatement(const_cast<ExprAST *>(BB->ForStep->getStep()));
    if (BB->ForTest)
      if (const VariableSymbol *S = BB->ForTest->getResolvedLoopVar())
        LoopVars.insert(S);
  }
  auto IsInt = [](const VariableSymbol *S) { return S->Type && S->Type->isInt(); };
  for (const VariableSymbol *S : Declared)
    if (!TV.Tainted.count(S)) {
      TV.Trackable.insert(S);
      if (IsInt(S))
        TV.Ranged.insert(S);
    }
  for (const VariableSymbol *S : Read)
    if (S->IsParameter && IsInt(S) && !TV.Tainted.count(S))
      TV.Ranged.insert(S);
  for (const VariableSymbol *S : LoopVars)
    if (IsInt(S) && !TV.Tainted.count(S))
      TV.Ranged.insert(S);
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

// ─── NM-2: intervals ───────────────────────────────────────────────────────
// (TDP-2-002, issue #348). A second dataflow over the same CFG and the same
// TrackedVars, keeping a closed interval [Lo, Hi] for each Ranged variable
// instead of one constant. It is what check elision (markElidableChecks),
// E181 and AN-3 (b) read; E001 keeps the constant dataflow above, so no
// program is rejected on a range.
//
// What it adds over constants:
//   - a value narrowed on the edge out of a test: in `if i < n { ... }` the
//     `then` block sees `i <= n.Hi - 1`, the `else` block `i >= n.Lo`, and
//     an edge whose narrowed range is empty is never taken;
//   - a range `for`: its variable starts at the start's range, the test
//     bounds it by the end (in the direction the start and end prove), and
//     its `step` runs in the step block;
//   - arithmetic on ranges, and `%`, `&` and `>>`, which bound a value even
//     when nothing is known about the other operand;
//   - loop-carried values: a block's entry state is the hull of its
//     predecessors', widened to the type's limit on a side that is still
//     growing after a few visits, so the solve ends.
//
// Soundness rests on three rules. A range only ever holds for an `int`
// value (Ranged is `int` variables only; a comparison narrows only when both
// sides are `int`, per AnalysisFacts::IntExprs). A checked operation's result
// is clamped to the `int` limits, since past them the program has already
// panicked. And every variable whose writes the transfer below does not see
// is never Ranged (computeTrackedVars). A value the analysis cannot bound is
// the full range, which proves nothing and keeps every check.
namespace {
// GCC and Clang (every compiler this project builds with) have a 128-bit
// integer; exact for any product or sum of two `int`s.
__extension__ typedef __int128 Wide;
constexpr long long IntMin = LLONG_MIN, IntMax = LLONG_MAX;

struct Range {
  long long Lo = IntMin, Hi = IntMax;
  bool full() const { return Lo == IntMin && Hi == IntMax; }
  bool operator==(const Range &O) const { return Lo == O.Lo && Hi == O.Hi; }
  bool operator!=(const Range &O) const { return !(*this == O); }
};

// Keys: a Ranged VariableSymbol, or a range `for`'s start/end expression (the
// values the loop fixed before its first test). An absent key is the full
// range.
using RangeMap = std::unordered_map<const void *, Range>;

Range lookup(const RangeMap &M, const void *K) {
  auto It = M.find(K);
  return It == M.end() ? Range{} : It->second;
}
void store(RangeMap &M, const void *K, Range R) {
  if (R.full())
    M.erase(K);
  else
    M[K] = R;
}
// The values of [Lo, Hi] that an `int` can hold (a checked operation that
// leaves them has panicked).
Range clamp(Wide Lo, Wide Hi) {
  Range R;
  R.Lo = Lo < IntMin ? IntMin : Lo > IntMax ? IntMax : static_cast<long long>(Lo);
  R.Hi = Hi > IntMax ? IntMax : Hi < IntMin ? IntMin : static_cast<long long>(Hi);
  return R;
}
Range hull(const Range &A, const Range &B) {
  return {std::min(A.Lo, B.Lo), std::max(A.Hi, B.Hi)};
}
Wide wabs(long long V) { return V < 0 ? -static_cast<Wide>(V) : static_cast<Wide>(V); }

// The exact range of L op R for + - *, before clamping.
std::pair<Wide, Wide> arithCorners(int Op, const Range &L, const Range &R) {
  Wide C[4];
  const long long Ls[2] = {L.Lo, L.Hi}, Rs[2] = {R.Lo, R.Hi};
  int N = 0;
  for (long long A : Ls)
    for (long long B : Rs) {
      Wide WA = A, WB = B;
      C[N++] = Op == '+' ? WA + WB : Op == '-' ? WA - WB : WA * WB;
    }
  return {*std::min_element(C, C + 4), *std::max_element(C, C + 4)};
}

class RangeEval {
public:
  RangeEval(const TrackedVars &TV) : TV(TV), F(Facts()) {}

  // Whether E is an `int` value: an integer literal, a Ranged variable, or
  // an expression the TypeChecker typed `int`.
  bool isInt(const ExprAST *E) const {
    if (auto *N = dynamic_cast<const NumberExprAST *>(E))
      return N->isInteger();
    if (auto *V = dynamic_cast<const VariableExprAST *>(E))
      if (V->getResolvedSymbol() && TV.Ranged.count(V->getResolvedSymbol()))
        return true;
    return F && E && F->isInt(E->getNodeID());
  }

  Range eval(const ExprAST *E, const RangeMap &M) const {
    if (!E) return {};
    if (auto *N = dynamic_cast<const NumberExprAST *>(E)) {
      if (!N->isInteger()) return {};
      long long V = N->getIntVal();
      return {V, V};
    }
    if (auto *V = dynamic_cast<const VariableExprAST *>(E)) {
      const VariableSymbol *S = V->getResolvedSymbol();
      return S && TV.Ranged.count(S) ? lookup(M, S) : Range{};
    }
    if (auto *A = dynamic_cast<const AssignmentExprAST *>(E))
      return eval(A->getRHS(), M);
    // An enum's COUNT: its number of variants, far below 2^31.
    if (auto *SA = dynamic_cast<const ScopeAccessExprAST *>(E))
      if (SA->getRightName() == "COUNT")
        return {0, INT32_MAX};
    if (auto *U = dynamic_cast<const UnaryExprAST *>(E)) {
      if (U->getOpcode() != '-') return {};
      Range R = eval(U->getOperand(), M);
      if (R.full()) return {};
      return clamp(-static_cast<Wide>(R.Hi), -static_cast<Wide>(R.Lo));
    }
    if (auto *B = dynamic_cast<const BinaryExprAST *>(E))
      return binary(B->getOp(), B->getLHS(), B->getRHS(), M);
    return {};
  }

  // L op R, shared by a binary expression and `op=`.
  Range binary(int Op, const ExprAST *LE, const ExprAST *RE, const RangeMap &M) const {
    Range L = eval(LE, M), R = eval(RE, M);
    switch (Op) {
    case '+': case '-': case '*': {
      // A full operand may be a `double`; a bounded one is an `int`.
      if (L.full() || R.full()) return {};
      auto [Lo, Hi] = arithCorners(Op, L, R);
      return clamp(Lo, Hi);
    }
    case '/': {
      if (L.full() || R.full() || (R.Lo <= 0 && R.Hi >= 0)) return {};
      Wide C[4];
      int N = 0;
      for (long long A : {L.Lo, L.Hi})
        for (long long D : {R.Lo, R.Hi})
          C[N++] = static_cast<Wide>(A) / static_cast<Wide>(D);
      return clamp(*std::min_element(C, C + 4), *std::max_element(C, C + 4));
    }
    case '%': {
      // The remainder takes the dividend's sign and is smaller than the
      // divisor in magnitude, whatever the dividend is.
      if (R.full() || !isInt(LE)) return {};
      Wide Max = std::max(wabs(R.Lo), wabs(R.Hi)) - 1;
      if (Max < 0) return {};
      Range Out = clamp(-Max, Max);
      if (L.Lo >= 0) Out = {0, std::min<long long>(Out.Hi, L.Hi)};
      else if (L.Hi <= 0) Out = {std::max<long long>(Out.Lo, L.Lo), 0};
      return Out;
    }
    case '&': {
      // `x & m` with a non-negative side is within [0, that side's Hi].
      if (!isInt(LE) || !isInt(RE)) return {};
      bool LNonNeg = !L.full() && L.Lo >= 0, RNonNeg = !R.full() && R.Lo >= 0;
      if (LNonNeg && RNonNeg) return {0, std::min(L.Hi, R.Hi)};
      if (LNonNeg) return {0, L.Hi};
      if (RNonNeg) return {0, R.Hi};
      return {};
    }
    case TOK_RSHIFT: {
      // Arithmetic shift by a proven amount in [0, 63] (anything else panics).
      if (R.full() || !isInt(LE)) return {};
      long long KLo = std::max(R.Lo, 0LL), KHi = std::min(R.Hi, 63LL);
      if (KLo > KHi) return {};
      long long C[4] = {L.Lo >> KLo, L.Lo >> KHi, L.Hi >> KLo, L.Hi >> KHi};
      return {*std::min_element(C, C + 4), *std::max_element(C, C + 4)};
    }
    default:
      return {};
    }
  }

  // Narrows M to the states in which Cond evaluates to Truth; false when
  // there are none (the edge is never taken).
  bool narrow(const ExprAST *Cond, bool Truth, RangeMap &M) const {
    auto *B = dynamic_cast<const BinaryExprAST *>(Cond);
    if (!B) return true;
    int Op = B->getOp();
    if (Op == TOK_AND) {
      if (!Truth) return true;
      return narrow(B->getLHS(), true, M) && narrow(B->getRHS(), true, M);
    }
    if (Op == TOK_OR) {
      if (Truth) return true;
      return narrow(B->getLHS(), false, M) && narrow(B->getRHS(), false, M);
    }
    // `c == true`, `c != true` (how `!c` is spelled), and the `false` forms.
    if (Op == TOK_EQ || Op == TOK_NEQ) {
      const BoolExprAST *Lit = dynamic_cast<const BoolExprAST *>(B->getRHS());
      const ExprAST *Other = B->getLHS();
      if (!Lit) {
        Lit = dynamic_cast<const BoolExprAST *>(B->getLHS());
        Other = B->getRHS();
      }
      if (Lit)
        return narrow(Other, (Op == TOK_EQ) == (Truth == Lit->getVal()), M);
    }
    if (Op != '<' && Op != '>' && Op != TOK_LEQ && Op != TOK_GEQ &&
        Op != TOK_EQ && Op != TOK_NEQ)
      return true;
    if (!isInt(B->getLHS()) || !isInt(B->getRHS()))
      return true;
    if (!Truth)
      Op = negated(Op);
    const Range L = eval(B->getLHS(), M), R = eval(B->getRHS(), M);
    const int Swapped = swapped(Op);
    return relate(B->getLHS(), Op, R, M) && relate(B->getRHS(), Swapped, L, M);
  }

private:
  // `!(a op b)` is `a negated(op) b`; `a op b` is `b swapped(op) a`.
  static int negated(int Op) {
    switch (Op) {
    case '<': return TOK_GEQ;
    case '>': return TOK_LEQ;
    case TOK_LEQ: return '>';
    case TOK_GEQ: return '<';
    case TOK_EQ: return TOK_NEQ;
    default: return TOK_EQ;
    }
  }
  static int swapped(int Op) {
    switch (Op) {
    case '<': return '>';
    case '>': return '<';
    case TOK_LEQ: return TOK_GEQ;
    case TOK_GEQ: return TOK_LEQ;
    default: return Op;
    }
  }

  // Narrows the Ranged variable X (if it is one) to the values for which
  // `X op E` can hold with E in ER; false when there are none.
  bool relate(const ExprAST *X, int Op, const Range &ER, RangeMap &M) const {
    auto *V = dynamic_cast<const VariableExprAST *>(X);
    const VariableSymbol *S = V ? V->getResolvedSymbol() : nullptr;
    Range Cur = S && TV.Ranged.count(S) ? lookup(M, S) : eval(X, M);
    Wide Lo = Cur.Lo, Hi = Cur.Hi;
    switch (Op) {
    case '<': Hi = std::min<Wide>(Hi, static_cast<Wide>(ER.Hi) - 1); break;
    case TOK_LEQ: Hi = std::min<Wide>(Hi, ER.Hi); break;
    case '>': Lo = std::max<Wide>(Lo, static_cast<Wide>(ER.Lo) + 1); break;
    case TOK_GEQ: Lo = std::max<Wide>(Lo, ER.Lo); break;
    case TOK_EQ: Lo = std::max<Wide>(Lo, ER.Lo); Hi = std::min<Wide>(Hi, ER.Hi); break;
    case TOK_NEQ:
      if (ER.Lo == ER.Hi) {
        if (Lo == ER.Lo) ++Lo;
        else if (Hi == ER.Hi) --Hi;
      }
      break;
    }
    if (Lo > Hi) return false;
    if (S && TV.Ranged.count(S))
      store(M, S, {static_cast<long long>(Lo), static_cast<long long>(Hi)});
    return true;
  }

  const TrackedVars &TV;
  const AnalysisFacts *F;
};
} // namespace

// The interval transfer for one whole statement (the four writes
// applyTransfer() handles, now computed instead of forgotten).
static void applyRangeTransfer(const ExprAST *Stmt, RangeMap &M, const TrackedVars &TV,
                               const RangeEval &Ev) {
  auto Set = [&](const VariableSymbol *S, Range R) {
    if (S && TV.Ranged.count(S)) store(M, S, R);
  };
  if (auto *Vd = dynamic_cast<const VarDeclExprAST *>(Stmt)) {
    Set(Vd->getResolvedSymbol(), Ev.eval(Vd->getInitializer(), M));
  } else if (auto *A = dynamic_cast<const AssignmentExprAST *>(Stmt)) {
    if (auto *L = dynamic_cast<const VariableExprAST *>(A->getLHS()))
      Set(L->getResolvedSymbol(), Ev.eval(A->getRHS(), M));
  } else if (auto *CA = dynamic_cast<const CompoundAssignmentExprAST *>(Stmt)) {
    if (auto *L = dynamic_cast<const VariableExprAST *>(CA->getLHS()))
      Set(L->getResolvedSymbol(), Ev.binary(CA->getOpcode(), CA->getLHS(), CA->getRHS(), M));
  } else if (auto *P = dynamic_cast<const PostfixExprAST *>(Stmt)) {
    if (auto *V = dynamic_cast<const VariableExprAST *>(P->getOperand())) {
      Range R = Ev.eval(V, M);
      Wide D = P->getOpcode() == '+' ? 1 : -1;
      Set(V->getResolvedSymbol(), R.full() ? Range{} : clamp(R.Lo + D, R.Hi + D));
    }
  }
}

namespace {
struct RangeSolution {
  std::vector<RangeMap> IN;
  std::vector<bool> Reached; // false: no path from entry the analysis allows
  std::set<CFG::Edge> DeadEdges; // from a reached block, proven never taken
};
} // namespace

// The state on the edge P -> S, given P's exit state; nullopt when the edge
// is never taken.
static std::optional<RangeMap> edgeState(const TurfBasicBlock *P, const TurfBasicBlock *S,
                                         RangeMap M, const TrackedVars &TV,
                                         const RangeEval &Ev) {
  // Entering a range `for` (from anywhere but its own step block): the
  // variable takes the start's value, and the start and end are fixed.
  if (const ForExprAST *For = S->ForTest; For && P->ForStep != For) {
    store(M, For->getStart(), Ev.eval(For->getStart(), M));
    store(M, For->getEnd(), Ev.eval(For->getEnd(), M));
    if (const VariableSymbol *V = For->getResolvedLoopVar(); V && TV.Ranged.count(V))
      store(M, V, Ev.eval(For->getStart(), M));
  }
  // Into a range `for`'s body: the loop runs while `i < end` when the start
  // was below the end, else while `i > end` (CodegenControlFlow.cpp).
  if (const ForExprAST *For = P->ForTest; For && S == P->TrueSucc) {
    const VariableSymbol *V = For->getResolvedLoopVar();
    if (V && TV.Ranged.count(V)) {
      Range Start = lookup(M, For->getStart()), End = lookup(M, For->getEnd());
      Range I = lookup(M, V);
      Wide Lo = I.Lo, Hi = I.Hi;
      if (Start.Hi < End.Lo) Hi = std::min<Wide>(Hi, static_cast<Wide>(End.Hi) - 1);
      if (Start.Lo >= End.Hi) Lo = std::max<Wide>(Lo, static_cast<Wide>(End.Lo) + 1);
      if (Lo > Hi) return std::nullopt;
      store(M, V, {static_cast<long long>(Lo), static_cast<long long>(Hi)});
    }
  }
  if (P->BranchCond && P->TrueSucc != P->FalseSucc) {
    if (S == P->TrueSucc && !Ev.narrow(P->BranchCond, true, M)) return std::nullopt;
    if (S == P->FalseSucc && !Ev.narrow(P->BranchCond, false, M)) return std::nullopt;
  }
  return M;
}

namespace {
// A depth-first walk from the entry: each block's reverse-postorder number
// (N for a block the walk never reaches), the loop heads (targets of an edge
// back to a block still on the walk's stack) and those back edges. Every
// cycle passes through a loop head.
struct DepthFirst {
  std::vector<size_t> Rpo;
  std::vector<bool> LoopHead;
  std::vector<std::pair<size_t, size_t>> BackEdges; // (from, head)
};
} // namespace

static DepthFirst depthFirst(const CFG &Cfg,
                             const std::unordered_map<const TurfBasicBlock *, size_t> &Index) {
  const auto &Blocks = Cfg.getBlocks();
  const size_t N = Blocks.size();
  DepthFirst DF;
  DF.Rpo.assign(N, N);
  DF.LoopHead.assign(N, false);
  if (!Cfg.getEntryBlock()) return DF;
  std::vector<int> State(N, 0); // 0 new, 1 on the stack, 2 done
  const size_t Entry = Index.at(Cfg.getEntryBlock());
  std::vector<std::pair<size_t, size_t>> Stack{{Entry, 0}};
  State[Entry] = 1;
  size_t Next = N;
  while (!Stack.empty()) {
    auto &[B, I] = Stack.back();
    const auto &Succs = Blocks[B]->getSuccessors();
    if (I < Succs.size()) {
      size_t S = Index.at(Succs[I++]);
      if (State[S] == 1) {
        DF.LoopHead[S] = true;
        DF.BackEdges.push_back({B, S});
      } else if (State[S] == 0) {
        State[S] = 1;
        Stack.push_back({S, 0});
      }
      continue;
    }
    State[B] = 2;
    DF.Rpo[B] = --Next;
    Stack.pop_back();
  }
  return DF;
}

static RangeSolution solveRanges(const CFG &Cfg, const TrackedVars &TV) {
  const auto &Blocks = Cfg.getBlocks();
  const size_t N = Blocks.size();
  std::unordered_map<const TurfBasicBlock *, size_t> Index;
  for (size_t I = 0; I < N; ++I) Index[Blocks[I].get()] = I;
  RangeEval Ev(TV);

  RangeSolution Sol;
  Sol.IN.assign(N, {});
  Sol.Reached.assign(N, false);
  std::vector<RangeMap> OUT(N);
  std::vector<unsigned> Updates(N, 0);
  constexpr unsigned WidenAfter = 3;

  const TurfBasicBlock *Entry = Cfg.getEntryBlock();
  if (!Entry) return Sol;
  const DepthFirst DF = depthFirst(Cfg, Index);
  const std::vector<size_t> &Rpo = DF.Rpo;
  const std::vector<bool> &LoopHead = DF.LoopHead;
  auto ByRpo = [&](size_t A, size_t B) { return Rpo[A] != Rpo[B] ? Rpo[A] < Rpo[B] : A < B; };
  std::set<size_t, decltype(ByRpo)> Work(ByRpo);
  Work.insert(Index[Entry]);
  bool First = true;
  // A bound on the work that the widening above already guarantees; past
  // it the solve gives up and claims nothing (every block unreached, so
  // every check stays and no edge is dead).
  size_t Budget = 64 * (N + 1) * (TV.Ranged.size() + 2) + 10000;
  while (!Work.empty()) {
    if (Budget-- == 0) {
      Sol.IN.assign(N, {});
      Sol.Reached.assign(N, false);
      return Sol;
    }
    size_t B = *Work.begin();
    Work.erase(Work.begin());
    const TurfBasicBlock *BB = Blocks[B].get();

    std::optional<RangeMap> NewIN;
    if (BB == Entry && First) {
      NewIN = RangeMap{};
      First = false;
    }
    for (const TurfBasicBlock *P : BB->getPredecessors()) {
      size_t PI = Index[P];
      if (!Sol.Reached[PI]) continue;
      std::optional<RangeMap> E = edgeState(P, BB, OUT[PI], TV, Ev);
      if (!E) continue;
      if (!NewIN) {
        NewIN = std::move(*E);
        continue;
      }
      RangeMap Joined;
      for (const auto &[K, R] : *NewIN)
        if (auto It = E->find(K); It != E->end())
          store(Joined, K, hull(R, It->second));
      NewIN = std::move(Joined);
    }
    if (BB == Entry && !NewIN) NewIN = RangeMap{};
    if (!NewIN) continue; // every way in is proven never taken

    if (Sol.Reached[B]) {
      if (*NewIN == Sol.IN[B]) continue;
      if (LoopHead[B] && ++Updates[B] > WidenAfter) {
        // old widened by new: a side that moved goes to the type's limit,
        // and a key missing from either is full. Each key can only grow
        // from here, at most twice, so the solve ends.
        RangeMap Widened;
        for (const auto &[K, R] : *NewIN) {
          auto It = Sol.IN[B].find(K);
          if (It == Sol.IN[B].end()) continue;
          Range W = It->second;
          if (R.Lo < W.Lo) W.Lo = IntMin;
          if (R.Hi > W.Hi) W.Hi = IntMax;
          store(Widened, K, W);
        }
        NewIN = std::move(Widened);
        if (*NewIN == Sol.IN[B]) continue;
      }
    }
    Sol.Reached[B] = true;
    Sol.IN[B] = *NewIN;

    RangeMap Cur = *NewIN;
    for (const ExprAST *Stmt : BB->getStatements())
      applyRangeTransfer(Stmt, Cur, TV, Ev);
    if (BB->ForStep && BB->ForStep->getStep())
      applyRangeTransfer(BB->ForStep->getStep(), Cur, TV, Ev);
    OUT[B] = std::move(Cur);
    for (const TurfBasicBlock *S : BB->getSuccessors())
      Work.insert(Index[S]);
  }

  // The edges out of a reached block that its final state proves are never
  // taken.
  for (size_t B = 0; B < N; ++B) {
    if (!Sol.Reached[B]) continue;
    const TurfBasicBlock *BB = Blocks[B].get();
    for (const TurfBasicBlock *S : BB->getSuccessors())
      if (!edgeState(BB, S, OUT[B], TV, Ev))
        Sol.DeadEdges.insert({BB, S});
  }
  return Sol;
}

// Where an expression starts, for a diagnostic about the whole expression.
static SourceLocation locationOf(const ExprAST *N) {
  if (auto *V = dynamic_cast<const VariableExprAST *>(N)) return V->getLoc();
  if (auto *B = dynamic_cast<const BinaryExprAST *>(N)) return locationOf(B->getLHS());
  if (auto *U = dynamic_cast<const UnaryExprAST *>(N)) return U->getLoc();
  if (auto *C = dynamic_cast<const FuncCallExprAST *>(N)) return C->getLoc();
  if (auto *M = dynamic_cast<const MethodCallExprAST *>(N)) return M->getLoc();
  if (auto *F = dynamic_cast<const FieldAccessExprAST *>(N)) return F->getLoc();
  if (auto *A = dynamic_cast<const ArrayAccessExprAST *>(N)) return A->getLoc();
  if (auto *Num = dynamic_cast<const NumberExprAST *>(N)) return Num->getLoc();
  if (auto *P = dynamic_cast<const PostfixExprAST *>(N)) return locationOf(P->getOperand());
  if (auto *C = dynamic_cast<const CastExprAST *>(N)) return locationOf(C->getOperand());
  if (auto *S = dynamic_cast<const ScopeAccessExprAST *>(N)) return S->getLoc();
  if (auto *R = dynamic_cast<const ReturnExprAST *>(N)) return R->getLoc();
  return {};
}

// An `int` whose value is within +-2^53 for a reason the constant dataflow
// does not track: an enum's `COUNT` (its number of variants), or a constant
// `double` literal converted to `int`.
static bool boundedByConstruction(const ExprAST *N) {
  if (auto *S = dynamic_cast<const ScopeAccessExprAST *>(N))
    return S->getRightName() == "COUNT";
  if (auto *C = dynamic_cast<const CastExprAST *>(N))
    if (auto *Num = dynamic_cast<const NumberExprAST *>(C->getOperand()))
      return !Num->isInteger() && std::fabs(Num->getDoubleVal()) <= 9007199254740992.0;
  return false;
}

// AN-6 (E181): an `int` converted to `double` (AnalysisFacts::IntToDouble)
// that the interval dataflow cannot prove within +-2^53, where every int converts
// exactly. Opt-in: nothing is done unless E181 is turned on.
static void reportPrecisionLoss(const ExprAST *N, const RangeMap &Ranges,
                                const RangeEval &Ev) {
  AnalysisFacts *F = Facts();
  if (!F || F->IntToDouble.empty() || !F->IntToDouble.count(N->getNodeID()))
    return;
  if (LintLevels::levelOf("E181") == LintLevels::Level::Allow)
    return;
  constexpr long long Exact = 1LL << 53;
  if (Range R = Ev.eval(N, Ranges); R.Lo >= -Exact && R.Hi <= Exact)
    return;
  if (boundedByConstruction(N))
    return;
  IntToDoublePrecisionWarning(locationOf(N)).emit();
}

// AN-1 (E191): `p[i]` through a raw pointer, where the ranges give `i` no
// evidence of staying inside what `p` points to: not proven non-negative,
// or with no upper bound below the type's limit (`i < n` with nothing known
// about `n` bounds `i` only by the largest `int`).
static void reportUnboundedPointerIndex(const ExprAST *N, const RangeMap &Ranges,
                                        const RangeEval &Ev) {
  auto *AA = dynamic_cast<const ArrayAccessExprAST *>(N);
  if (!AA || LintLevels::levelOf("E191") == LintLevels::Level::Allow) return;
  auto *Base = dynamic_cast<const VariableExprAST *>(AA->getBase());
  const VariableSymbol *S = Base ? Base->getResolvedSymbol() : nullptr;
  if (!S || !S->Type || !S->Type->isPointer() || S->Type->isReference() ||
      S->Type->isNullableReference())
    return;
  Range I = Ev.eval(AA->getIndex(), Ranges);
  if (I.Lo >= 0 && I.Hi < IntMax - 1) return;
  if (IsLibStdWarningLocation(AA->getLoc().Filename)) return;
  UncheckedPointerIndexWarning(AA->getLoc(), Base->getName()).emit();
}

// A compile-time out-of-range constant index (E001), reported once the
// dataflow has converged, so every report comes from a value proven on every
// path reaching the access.
// An index written as a constant: literals, `-` and size parameters,
// combined with `+`, `-` and `*` -- what Codegen folds to a constant and
// bounds-checks at compile time (CodegenArray.cpp). No variable is read, so
// a program the constant dataflow accepts is never rejected through it.
static std::optional<long long> literalIndex(const ExprAST *E) {
  if (auto *N = dynamic_cast<const NumberExprAST *>(E))
    return N->isInteger() ? std::optional<long long>(N->getIntVal()) : std::nullopt;
  if (auto *V = dynamic_cast<const VariableExprAST *>(E))
    return V->SizeValue ? std::optional<long long>(*V->SizeValue) : std::nullopt;
  if (auto *U = dynamic_cast<const UnaryExprAST *>(E)) {
    auto X = U->getOpcode() == '-' ? literalIndex(U->getOperand()) : std::nullopt;
    return X && *X != LLONG_MIN ? std::optional<long long>(-*X) : std::nullopt;
  }
  if (auto *B = dynamic_cast<const BinaryExprAST *>(E)) {
    auto L = literalIndex(B->getLHS()), R = literalIndex(B->getRHS());
    if (!L || !R) return std::nullopt;
    __int128 W = B->getOp() == '+' ? (__int128)*L + *R
               : B->getOp() == '-' ? (__int128)*L - *R
               : B->getOp() == '*' ? (__int128)*L * *R : (__int128)LLONG_MAX + 1;
    if (W > LLONG_MAX || W < LLONG_MIN) return std::nullopt;
    return static_cast<long long>(W);
  }
  return std::nullopt;
}

static void reportConstantOutOfBounds(const ExprAST *N, const ConstMap &cur,
                                      const RangeMap &Ranges, const RangeEval &Ev) {
  if (!N) return;
  reportPrecisionLoss(N, Ranges, Ev);
  reportUnboundedPointerIndex(N, Ranges, Ev);
  if (auto AA = dynamic_cast<const ArrayAccessExprAST *>(N)) {
    auto idxVal = evalConstExpr(AA->getIndex(), cur);
    if (!idxVal) idxVal = literalIndex(AA->getIndex());
    if (idxVal.has_value()) {
      if (auto BaseVar = dynamic_cast<const VariableExprAST *>(AA->getBase())) {
        const VariableSymbol *S = BaseVar->getResolvedSymbol();
        // A safe reference to one value is one element (CodegenArray.cpp):
        // `r[0]` and `r[-1]` are the value, any other index is out of bounds.
        const auto *Ref = S && S->Type ? dynamic_cast<const TurfPointerType *>(S->Type.get()) : nullptr;
        // Only a written constant, as Codegen folds: an index through a
        // variable stays a run-time check here.
        auto Lit = literalIndex(AA->getIndex());
        if (Ref && Ref->isReference() && Ref->getPointee() && !Ref->getPointee()->isArray()) {
          long long iv = Lit ? *Lit : 0;
          if (Lit && (iv < -1 || iv >= 1)) {
            ArrayBoundsError err(AA->getLoc(), BaseVar->getName(), iv, 1);
            err.raise();
          }
        } else if (S && S->Type && S->Type->isArray()) {
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
  auto checkAccesses = [&](const ExprAST *C) { reportConstantOutOfBounds(C, cur, Ranges, Ev); };
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

// ─── AN-2: loops that never end (E184, E185, E186) ─────────────────────────
// TDP-2-007, issue #350. Warnings only, forever: termination is undecidable,
// so each case reports only what the CFG and the ranges prove, and a loop
// with a way out the CFG does not model (a `?`, a call that ends the
// program) is left alone by the first two.

namespace {
// What a loop's statements contain that the CFG's edges do not show.
class LoopContents : public ASTChildWalker {
public:
  bool HasTry = false;       // `?`: an early return the CFG does not draw
  bool HasNoReturn = false;  // sys::panic / sys::abort / sys::exit

  static bool isNoReturn(const FuncCallExprAST &Call) {
    const ResolverFunctionSymbol *F = Call.getResolvedCallTarget();
    return F && F->SourceModule == "sys" &&
           (F->BaseName == "panic" || F->BaseName == "abort" || F->BaseName == "exit");
  }
  void visitTry(TryExprAST &Node) override {
    HasTry = true;
    ASTChildWalker::visitTry(Node);
  }
  void visitFuncCall(FuncCallExprAST &Node) override {
    HasNoReturn |= isNoReturn(Node);
    ASTChildWalker::visitFuncCall(Node);
  }
  void scan(const ExprAST *E) { visitChild(E); }
};

// The variables a loop test reads, when it is built only of variables,
// literals and operators (anything else may change without a write here).
bool testVariables(const ExprAST *E, std::unordered_set<const VariableSymbol *> &Out) {
  if (!E) return false;
  if (dynamic_cast<const NumberExprAST *>(E) || dynamic_cast<const BoolExprAST *>(E))
    return true;
  if (auto *V = dynamic_cast<const VariableExprAST *>(E)) {
    if (!V->getResolvedSymbol()) return false;
    Out.insert(V->getResolvedSymbol());
    return true;
  }
  if (auto *B = dynamic_cast<const BinaryExprAST *>(E))
    return testVariables(B->getLHS(), Out) && testVariables(B->getRHS(), Out);
  if (auto *U = dynamic_cast<const UnaryExprAST *>(E))
    return testVariables(U->getOperand(), Out);
  if (auto *C = dynamic_cast<const CastExprAST *>(E))
    return testVariables(C->getOperand(), Out);
  return false;
}

// The variable a whole-statement write assigns (the only writes a Trackable
// or Ranged variable has, computeTrackedVars()).
const VariableSymbol *writtenBy(const ExprAST *Stmt) {
  const ExprAST *Target = nullptr;
  if (auto *A = dynamic_cast<const AssignmentExprAST *>(Stmt)) Target = A->getLHS();
  else if (auto *CA = dynamic_cast<const CompoundAssignmentExprAST *>(Stmt)) Target = CA->getLHS();
  else if (auto *P = dynamic_cast<const PostfixExprAST *>(Stmt)) Target = P->getOperand();
  else if (auto *Vd = dynamic_cast<const VarDeclExprAST *>(Stmt)) return Vd->getResolvedSymbol();
  auto *V = dynamic_cast<const VariableExprAST *>(Target);
  return V ? V->getResolvedSymbol() : nullptr;
}

std::string joinNames(const std::unordered_set<const VariableSymbol *> &Vars) {
  std::vector<std::string> Names;
  for (const VariableSymbol *S : Vars) Names.push_back("`" + S->Name + "`");
  std::sort(Names.begin(), Names.end());
  std::string Out;
  for (size_t I = 0; I < Names.size(); ++I)
    Out += (I == 0 ? "" : I + 1 == Names.size() ? " and " : ", ") + Names[I];
  return Out;
}
} // namespace

static void reportEndlessLoops(const CFG &Cfg, const RangeSolution &RS, const TrackedVars &TV) {
  const bool Unchanged = LintLevels::levelOf("E184") != LintLevels::Level::Allow;
  const bool DeadExits = LintLevels::levelOf("E185") != LintLevels::Level::Allow;
  const bool NoReturnExits = LintLevels::levelOf("E186") != LintLevels::Level::Allow;
  if (!Unchanged && !DeadExits && !NoReturnExits) return;
  const auto &Blocks = Cfg.getBlocks();
  const size_t N = Blocks.size();
  std::unordered_map<const TurfBasicBlock *, size_t> Index;
  for (size_t I = 0; I < N; ++I) Index[Blocks[I].get()] = I;
  const DepthFirst DF = depthFirst(Cfg, Index);

  // Natural loops, one per head: the head and every block that reaches one
  // of its back edges without passing through it.
  std::map<size_t, std::set<size_t>> Loops;
  for (const auto &[From, Head] : DF.BackEdges) {
    std::set<size_t> &L = Loops[Head];
    L.insert(Head);
    std::vector<size_t> Work;
    if (L.insert(From).second) Work.push_back(From);
    while (!Work.empty()) {
      size_t B = Work.back();
      Work.pop_back();
      for (const TurfBasicBlock *P : Blocks[B]->getPredecessors())
        if (size_t PI = Index.at(P); L.insert(PI).second) Work.push_back(PI);
    }
  }

  for (const auto &[Head, L] : Loops) {
    const TurfBasicBlock *H = Blocks[Head].get();
    if (!RS.Reached[Head]) continue;
    LoopContents Contents;
    for (size_t B : L) {
      for (const ExprAST *Stmt : Blocks[B]->getStatements()) Contents.scan(Stmt);
      if (Blocks[B]->ForStep) Contents.scan(Blocks[B]->ForStep->getStep());
    }
    if (Contents.HasTry) continue;

    std::vector<CFG::Edge> Exits;
    for (size_t B : L)
      for (const TurfBasicBlock *S : Blocks[B]->getSuccessors())
        if (!L.count(Index.at(S))) Exits.push_back({Blocks[B].get(), S});
    if (Exits.empty()) continue; // no way out at all: E157 / hasUnbreakableLoop()

    // The loop's own test: a `while` or `do while` block whose false edge
    // leaves the loop and whose true edge stays in it.
    const TurfBasicBlock *Test = nullptr;
    for (size_t B : L) {
      const TurfBasicBlock *BB = Blocks[B].get();
      if (BB->BranchCond && BB->TrueSucc && BB->FalseSucc && L.count(Index.at(BB->TrueSucc)) &&
          !L.count(Index.at(BB->FalseSucc)) && (BB == H || BB->getSuccessors().front() == H))
        Test = BB;
    }
    SourceLocation Loc = H->ForTest ? H->ForTest->getLoc()
                       : Test ? Test->getTerminatorLoc() : H->getTerminatorLoc();
    if (!Loc.Line || IsLibStdWarningLocation(Loc.Filename)) continue;

    auto IsDead = [&](const CFG::Edge &E) {
      return !RS.Reached[Index.at(E.first)] || RS.DeadEdges.count(E);
    };

    // E184: the test reads only variables nothing in the loop writes, and
    // its false edge is the only way out.
    if (Test && !Contents.HasNoReturn && Exits.size() == 1 && Exits[0].first == Test &&
        !RS.DeadEdges.count({Test, Test->TrueSucc})) {
      std::unordered_set<const VariableSymbol *> Vars;
      bool Simple = testVariables(Test->BranchCond, Vars) && !Vars.empty();
      for (const VariableSymbol *V : Vars)
        Simple = Simple && (TV.Trackable.count(V) || TV.Ranged.count(V));
      if (Simple) {
        bool Written = false;
        for (size_t B : L) {
          for (const ExprAST *Stmt : Blocks[B]->getStatements())
            Written |= Vars.count(writtenBy(Stmt)) > 0;
          if (Blocks[B]->ForStep)
            Written |= Vars.count(writtenBy(Blocks[B]->ForStep->getStep())) > 0;
        }
        if (!Written) {
          if (Unchanged) LoopTestNeverChangesWarning(Loc, joinNames(Vars)).emit();
          continue;
        }
      }
    }

    // E185: every way out is an edge the ranges prove is never taken.
    if (!Contents.HasNoReturn &&
        std::all_of(Exits.begin(), Exits.end(), IsDead)) {
      if (DeadExits) LoopExitsNeverTakenWarning(Loc).emit();
      continue;
    }

    // E186: every way out that can be taken passes a call that ends the
    // program: in the block it leaves from, or in the `break`/`return`
    // block it leads to (which a natural loop does not contain).
    auto EndsProgram = [](const TurfBasicBlock *BB) {
      LoopContents Block;
      for (const ExprAST *Stmt : BB->getStatements()) Block.scan(Stmt);
      return Block.HasNoReturn;
    };
    bool AnyLive = false, AllBehindNoReturn = true;
    for (const CFG::Edge &E : Exits) {
      if (IsDead(E)) continue;
      AnyLive = true;
      const TurfBasicBlock *To = E.second;
      bool Leaves = (To->getTerminator() == TerminatorKind::Break ||
                     To->getTerminator() == TerminatorKind::Return) &&
                    To->getPredecessors().size() == 1;
      AllBehindNoReturn = AllBehindNoReturn && (EndsProgram(E.first) || (Leaves && EndsProgram(To)));
    }
    if (AnyLive && AllBehindNoReturn && NoReturnExits)
      LoopExitsBehindNoReturnWarning(Loc).emit();
  }
}

// AN-3 (b) (E183): a recursive function whose base case the interval
// dataflow proves is never reached, so every path that runs calls the
// function again. E010 already covers a function with no base case at all.
static void reportUnreachableBaseCase(const CFG &Cfg, const RangeSolution &RS) {
  if (RS.DeadEdges.empty() || LintLevels::levelOf("E183") == LintLevels::Level::Allow)
    return;
  if (Cfg.allPathsRecurse() || !Cfg.allPathsRecurseWithout(RS.DeadEdges))
    return;
  const auto BaseBlocks = Cfg.blocksReachingExitWithoutSelfCall();
  std::set<std::pair<unsigned, unsigned>> Seen;
  for (const auto &[From, To] : RS.DeadEdges) {
    if (!BaseBlocks.count(To))
      continue;
    SourceLocation Loc = From->getTerminatorLoc();
    if (!To->getStatements().empty())
      if (SourceLocation First = locationOf(To->getStatements().front()); First.Line)
        Loc = First;
    if (!Loc.Line || IsLibStdWarningLocation(Loc.Filename) || !Seen.insert({Loc.Line, Loc.Col}).second)
      continue;
    UnreachableBaseCaseWarning(Loc, Cfg.getFunctionName()).emit();
  }
}

void ValueRangePass::runOnCFG(CFG &Cfg, const std::vector<SymbolID> & /*InitialAssigned*/) {
  // InitialAssigned (parameter SymbolIDs from the codegen-time SymbolTable)
  // is intentionally unused: the constant dataflow never tracks a
  // parameter, and the interval dataflow finds this function's parameters
  // itself (computeTrackedVars()). Kept in the signature to match
  // DefiniteAssignmentPass::runOnCFG's identical (also-unused) parameter.
  const auto &Blocks = Cfg.getBlocks();
  if (Blocks.empty()) return;
  TrackedVars TV = computeTrackedVars(Cfg);
  std::vector<ConstMap> IN = solveConstants(Cfg, TV);
  RangeSolution RS = solveRanges(Cfg, TV);
  RangeEval Ev(TV);
  for (size_t i = 0; i < Blocks.size(); ++i) {
    ConstMap Cur = IN[i];
    RangeMap Ranges = RS.IN[i];
    for (ExprAST *Stmt : Blocks[i]->getStatements()) {
      reportConstantOutOfBounds(Stmt, Cur, Ranges, Ev);
      applyTransfer(Stmt, Cur, TV);
      applyRangeTransfer(Stmt, Ranges, TV, Ev);
    }
  }
  reportUnreachableBaseCase(Cfg, RS);
  reportEndlessLoops(Cfg, RS, TV);
}

// ─── OPT-002: check-elision pre-pass ───────────────────────────────────────
// (ARCHITECTURE_OVERHAUL.md §8 OPT-002, DESIGN_NOTES.md §1 "Bounds Check
// Elimination"). markElidableChecks() runs the same dataflow as runOnCFG() on
// a throwaway CFG built BEFORE Body->codegen() -- see that call site's own
// comment (CodegenFunc.cpp) for why runOnCFG(), which only ever runs AFTER
// Body->codegen(), cannot be reused for this: its results would describe
// checks Codegen has already emitted.

// Whether checked `op` (`+`/`-`/`*`) gives an `int` for every pair of
// operands in L x R: the extreme results are at the corners.
static bool arithProvablySafe(int Op, const Range &L, const Range &R) {
  if (L.full() || R.full()) return false;
  auto [Lo, Hi] = arithCorners(Op, L, R);
  return Lo >= IntMin && Hi <= IntMax;
}

// Mirrors EmitCheckedShl()'s own recovery check exactly (CodegenCore.cpp):
// the shift amount must be in [0,64), and shifting the result back right by
// the same amount (arithmetic, sign-preserving) must recover L. A value
// that fits at the largest amount fits at every smaller one, and the values
// that fit form an interval, so the ends of L at R.Hi decide.
static bool shlFits(long long L, long long R) {
  llvm::APInt LV(64, static_cast<uint64_t>(L), /*isSigned=*/true);
  return LV.shl(static_cast<unsigned>(R)).ashr(static_cast<unsigned>(R)) == LV;
}
static bool shlProvablySafe(const Range &L, const Range &R) {
  if (L.full() || R.Lo < 0 || R.Hi >= 64) return false;
  return shlFits(L.Lo, R.Hi) && shlFits(L.Hi, R.Hi);
}

// Mirrors EmitCheckedIntNegate()'s one overflow case (CodegenCore.cpp):
// negating the type's minimum has no representable result. The node's type
// is not known here and an `int32` negation stays 32 bits wide (unlike
// int32 arithmetic, which widens to int), so a range holding INT32_MIN is
// never proved safe either: that costs one kept check on an `int`, never a
// missed one.
static bool negProvablySafe(const Range &V) {
  return !V.full() && V.Lo != IntMin && !(V.Lo <= INT32_MIN && INT32_MIN <= V.Hi);
}

namespace {
// What markElidableChecks() proved in the function being marked, printed
// with TURF_ELISION_STATS=1 (BENCHMARKS.md counts them over the corpus).
struct ElisionStats {
  unsigned long Overflow = 0, Bounds = 0;
};
ElisionStats Stats;
} // namespace

// Records every node this dataflow proves safe in the compilation's
// AnalysisFacts (ProvenInBounds / ProvenNoOverflow, keyed by node id), which
// Codegen reads. The walk takes non-const nodes for the visitors it shares,
// so getters returning `const ExprAST *` are const_cast back. A node kind with no case here is simply
// never marked, which keeps its checks. Within one statement no Ranged
// variable changes (computeTrackedVars()), so the statement's entry state
// holds for every node in it.
static void markNode(ExprAST *N, const RangeMap &Cur, const TrackedVars &TV,
                     const RangeEval &Ev) {
  if (!N) return;
  auto Mark = [&](ExprAST *C) { markNode(C, Cur, TV, Ev); };
  auto Cast = [](const ExprAST *C) { return const_cast<ExprAST *>(C); };

  if (auto *AA = dynamic_cast<ArrayAccessExprAST *>(N)) {
    // A negative index counts from the end (#256), and a proven-in-bounds index skips
    // that adjustment too, so only [0, size) is proven.
    Range Idx = Ev.eval(AA->getIndex(), Cur);
    if (auto *BaseVar = dynamic_cast<const VariableExprAST *>(AA->getBase())) {
      const VariableSymbol *S = BaseVar->getResolvedSymbol();
      if (S && S->Type && S->Type->isArray() && !TV.Tainted.count(S))
        if (const auto *AT = dynamic_cast<const ArrayType *>(S->Type.get()))
          if (AT->getSize() > 0 && Idx.Lo >= 0 && Idx.Hi < AT->getSize()) {
            Facts()->ProvenInBounds.insert(AA->getNodeID());
            ++Stats.Bounds;
          }
    }
    Mark(Cast(AA->getBase()));
    Mark(Cast(AA->getIndex()));
    return;
  }

  if (auto *B = dynamic_cast<BinaryExprAST *>(N)) {
    int Op = B->getOp();
    bool Safe = false;
    if (Op == '+' || Op == '-' || Op == '*')
      Safe = arithProvablySafe(Op, Ev.eval(B->getLHS(), Cur), Ev.eval(B->getRHS(), Cur));
    else if (Op == TOK_LSHIFT)
      Safe = shlProvablySafe(Ev.eval(B->getLHS(), Cur), Ev.eval(B->getRHS(), Cur));
    if (Safe) {
      Facts()->ProvenNoOverflow.insert(B->getNodeID());
      ++Stats.Overflow;
    }
    Mark(Cast(B->getLHS()));
    Mark(Cast(B->getRHS()));
    return;
  }

  if (auto *U = dynamic_cast<UnaryExprAST *>(N)) {
    if (U->getOpcode() == '-' && negProvablySafe(Ev.eval(U->getOperand(), Cur))) {
      Facts()->ProvenNoOverflow.insert(U->getNodeID());
      ++Stats.Overflow;
    }
    Mark(Cast(U->getOperand()));
    return;
  }

  if (auto *Tr = dynamic_cast<TryExprAST *>(N)) { // LEC-004
    Mark(Tr->getOperand());
    return;
  }

  if (auto *CA = dynamic_cast<CompoundAssignmentExprAST *>(N)) {
    int Op = CA->getOpcode();
    Range L = Ev.eval(CA->getLHS(), Cur), R = Ev.eval(CA->getRHS(), Cur);
    bool Safe = Op == TOK_LSHIFT ? shlProvablySafe(L, R)
              : (Op == '+' || Op == '-' || Op == '*') && arithProvablySafe(Op, L, R);
    if (Safe) {
      Facts()->ProvenNoOverflow.insert(CA->getNodeID());
      ++Stats.Overflow;
    }
    Mark(Cast(CA->getLHS()));
    Mark(Cast(CA->getRHS()));
    return;
  }

  if (auto *P = dynamic_cast<PostfixExprAST *>(N)) {
    Range V = Ev.eval(P->getOperand(), Cur);
    if (!V.full() && (P->getOpcode() == '+' ? V.Hi < IntMax : V.Lo > IntMin)) {
      Facts()->ProvenNoOverflow.insert(P->getNodeID());
      ++Stats.Overflow;
    }
    Mark(Cast(P->getOperand()));
    return;
  }

  if (auto *A = dynamic_cast<AssignmentExprAST *>(N)) {
    Mark(Cast(A->getRHS()));
    Mark(Cast(A->getLHS()));
  } else if (auto *Vd = dynamic_cast<VarDeclExprAST *>(N)) {
    Mark(Cast(Vd->getInitializer()));
  } else if (auto *If = dynamic_cast<IfExprAST *>(N)) {
    for (const auto &Br : If->getBranches()) {
      Mark(Cast(Br.Cond.get()));
      Mark(Cast(Br.Body.get()));
    }
    Mark(Cast(If->getElse()));
  } else if (auto *Bl = dynamic_cast<BlockExprAST *>(N)) {
    for (const auto &E2 : Bl->getExpressions()) Mark(E2.get());
  } else if (auto *F = dynamic_cast<FuncCallExprAST *>(N)) {
    for (const auto &Arg : F->getArgs()) Mark(Arg.get());
    Mark(Cast(F->getCallee()));
  } else if (auto *R = dynamic_cast<ReturnExprAST *>(N)) {
    Mark(Cast(R->getVal()));
  } else if (auto *M = dynamic_cast<MethodCallExprAST *>(N)) {
    Mark(Cast(M->getBase()));
    for (const auto &Arg : M->getArgs()) Mark(Arg.get());
  } else if (auto *FA = dynamic_cast<FieldAccessExprAST *>(N)) {
    Mark(Cast(FA->getBase()));
  } else if (auto *AL = dynamic_cast<ArrayLiteralExprAST *>(N)) {
    for (const auto &E2 : AL->getElements()) Mark(E2.get());
  } else if (auto *AD = dynamic_cast<ArrayDeclExprAST *>(N)) {
    for (const auto &E2 : AD->getInitList()) Mark(E2.get());
    if (AD->getInitExpr()) Mark(Cast(AD->getInitExpr().get()));
  } else if (auto *C = dynamic_cast<CastExprAST *>(N)) {
    Mark(Cast(C->getOperand()));
  }
}

void ValueRangePass::markElidableChecks(CFG &Cfg,
                                        const std::vector<SymbolID> & /*InitialAssigned*/) {
  const auto &Blocks = Cfg.getBlocks();
  if (Blocks.empty()) return;
  // TURF_NO_ELISION=1 keeps every check: the differential fuzz
  // (scripts/dev/fuzz/elision_diff.py) compares a program built with and
  // without, since a wrong range would silently drop a needed check.
  if (std::getenv("TURF_NO_ELISION") || !Facts()) return;
  TrackedVars TV = computeTrackedVars(Cfg);
  // Converge first, then mark in one walk per block from the converged IN[],
  // so every mark comes from dataflow state proven on EVERY path, never a
  // not-yet-converged intermediate estimate. A block the analysis never
  // reaches is walked with nothing known, which proves only literals.
  RangeSolution RS = solveRanges(Cfg, TV);
  RangeEval Ev(TV);
  Stats = {};
  for (size_t i = 0; i < Blocks.size(); ++i) {
    RangeMap Cur = RS.IN[i];
    for (ExprAST *Stmt : Blocks[i]->getStatements()) {
      markNode(Stmt, Cur, TV, Ev);
      applyRangeTransfer(Stmt, Cur, TV, Ev);
    }
  }
  if (std::getenv("TURF_ELISION_STATS"))
    std::fprintf(stderr, "[elision] %s overflow=%lu bounds=%lu\n",
                 Cfg.getFunctionName().c_str(), Stats.Overflow, Stats.Bounds);
}
