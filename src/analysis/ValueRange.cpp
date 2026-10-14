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

void ValueRangePass::runOnCFG(CFG &Cfg, const std::vector<SymbolID> & /*InitialAssigned*/) {
  // InitialAssigned (parameter SymbolIDs from the codegen-time SymbolTable)
  // is intentionally unused: parameters never have a statically-known
  // constant value anyway, so there is nothing useful to seed IN[] with --
  // an absent entry in ConstMap already means "unknown", same as the old
  // `= std::nullopt` seeding did. Kept in the signature to match
  // DefiniteAssignmentPass::runOnCFG's identical (also-unused) parameter,
  // and so Codegen.cpp's call site needs no changes.
  const auto &Blocks = Cfg.getBlocks();
  size_t N = Blocks.size();
  if (N == 0) return;

  std::unordered_map<const TurfBasicBlock *, size_t> index;
  for (size_t i = 0; i < N; ++i) index[Blocks[i].get()] = i;

  std::vector<ConstMap> IN(N), OUT(N);

  std::queue<size_t> work;
  for (size_t i = 0; i < N; ++i) work.push(i);

  while (!work.empty()) {
    size_t b = work.front(); work.pop();
    TurfBasicBlock *BB = Blocks[b].get();

    // IN = meet of preds: for constants, require all preds agree on same const
    ConstMap newIN;
    const auto &Preds = BB->getPredecessors();
    if (Preds.empty()) {
      newIN = IN[b];
    } else {
      bool first = true;
      for (auto *P : Preds) {
        size_t pi = index[P];
        if (first) { newIN = OUT[pi]; first = false; }
        else {
          // join: keep value only if equal in both maps, else unknown
          ConstMap tmp;
          for (const auto &kv : newIN) {
            const VariableSymbol *sid = kv.first;
            auto it = OUT[pi].find(sid);
            if (it != OUT[pi].end() && kv.second.has_value() && it->second.has_value() && kv.second == it->second) {
              tmp[sid] = kv.second;
            } else {
              tmp[sid] = std::nullopt;
            }
          }
          newIN.swap(tmp);
        }
      }
    }

    if (newIN != IN[b]) IN[b] = newIN;

    // Transfer: propagate constants through statements
    ConstMap cur = IN[b];
    for (ExprAST *Stmt : BB->getStatements()) {
        // Check for array accesses inside this statement and emit OOB diagnostics
        std::function<void(const ExprAST *)> checkAccesses = [&](const ExprAST *N) {
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
        };
        checkAccesses(Stmt);

      // Handle var decl
      if (auto Vd = dynamic_cast<VarDeclExprAST *>(Stmt)) {
        auto val = evalConstExpr(Vd->getInitializer(), cur);
        const VariableSymbol *S = Vd->getResolvedSymbol();
        if (S) cur[S] = val;
        continue;
      }
      if (auto A = dynamic_cast<AssignmentExprAST *>(Stmt)) {
        // LHS variable assign
        if (auto L = dynamic_cast<const VariableExprAST *>(A->getLHS())) {
          auto val = evalConstExpr(A->getRHS(), cur);
          const VariableSymbol *S = L->getResolvedSymbol();
          if (S) cur[S] = val;
        }
        continue;
      }
      // For other statements, we attempt to evaluate but don't change state
      // (could be extended later)
    }

    if (cur != OUT[b]) {
      OUT[b] = cur;
      for (auto *S : BB->getSuccessors()) {
        size_t si = index[S];
        work.push(si);
      }
    }
  }

}

// ─── OPT-002: check-elision pre-pass ───────────────────────────────────────
// (ARCHITECTURE_OVERHAUL.md §8 OPT-002, DESIGN_NOTES.md §1 "Bounds Check
// Elimination"). Everything below is invoked from a SEPARATE call
// (markElidableChecks(), at the bottom of this file) on a throwaway CFG
// built BEFORE Body->codegen() -- see that call site's own comment
// (CodegenFunc.cpp) for why runOnCFG() above, which only ever runs AFTER
// Body->codegen(), cannot be reused for this: its results would describe
// checks Codegen has already emitted. runOnCFG() itself is completely
// unmodified by any of this -- ArrayBoundsError still fires at exactly the
// point and in exactly the way it always has.

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
// negating INT64_MIN has no representable int64 result.
static bool negProvablySafe(long long V) { return V != INT64_MIN; }

// A value this dataflow believes is a known constant is only trustworthy
// for ELISION purposes (as opposed to runOnCFG()'s diagnostic use, where a
// missed detection just forgoes a compile-time error and the RUNTIME check
// still catches the real bug) if the variable's address is never taken
// anywhere in the enclosing function. `&x` (AddressOfExprAST) lets that
// reference be handed to a call that mutates `x` through it -- something
// this local, non-aliasing-aware dataflow has no way to see. Eliding a
// runtime check has no fallback: an unsound proof here would be a genuine
// memory-safety regression, not a missed diagnostic, so this pass insists
// on the coarser, unconditionally-safe "address never taken" precondition
// before trusting Cur for a given symbol at all -- see
// collectAddressTaken() below for how AddressTaken is populated.
static std::optional<long long> evalConstExprElidable(
    const ExprAST *E, const ConstMap &Cur,
    const std::unordered_set<const VariableSymbol *> &AddressTaken) {
  if (auto *V = dynamic_cast<const VariableExprAST *>(E)) {
    const VariableSymbol *S = V->getResolvedSymbol();
    if (S && AddressTaken.count(S)) return std::nullopt;
  }
  return evalConstExpr(E, Cur);
}

// Whole-function walk (over every statement CFGBuilder placed in every
// block -- it never splits one AST statement across two blocks, so this
// sees the entire function body) collecting every VariableSymbol whose
// address is taken anywhere at all, conservatively: this pass never tries
// to reason about what happens to the reference afterward, only whether
// one was ever formed.
//
// An ASTChildWalker (LEC-005, INV-LINT-1), because here a node the walk does
// not reach is not a missed warning but a MISCOMPILE: an unseen `&x` leaves
// `x` looking never-aliased, so a range fact about it survives a call that
// changes it, and a bounds/overflow check is elided with no fallback.
// This was a hand-written chain of dynamic_casts with no branch for
// `recover`, `defer`, `unsafe`, `compare`, do-while or for-each -- confirmed
// live, `mutate(&i) recover run { ... }; return a[i];` with `i` set to 99
// through the reference read `a[99]` of a 3-element array with no check and
// exit 0 -- LEC-004 had already hit the same hole once for `try` and patched
// that one node. Every node kind is now reached by construction.
namespace {
class AddressTakenCollector : public ASTChildWalker {
  std::unordered_set<const VariableSymbol *> &Out;

public:
  explicit AddressTakenCollector(std::unordered_set<const VariableSymbol *> &Out)
      : Out(Out) {}

  void visitAddressOf(AddressOfExprAST &Node) override {
    if (auto *V = dynamic_cast<const VariableExprAST *>(Node.getOperand()))
      if (const VariableSymbol *S = V->getResolvedSymbol())
        Out.insert(S);
    ASTChildWalker::visitAddressOf(Node); // `&f(&x)`: the inner one counts too
  }
};
} // namespace

static void collectAddressTaken(const ExprAST *N,
                                std::unordered_set<const VariableSymbol *> &Out) {
  if (!N) return;
  AddressTakenCollector Collector(Out);
  const_cast<ExprAST *>(N)->accept(Collector);
}

// The dataflow transfer for one top-level CFG statement -- shared by both
// the fixed-point convergence loop and the final marking pass below so the
// two can never drift apart. Extends runOnCFG()'s own inline transfer
// (VarDecl / plain-variable Assignment) with the one thing this pass cannot
// inherit that gap on: CompoundAssignmentExprAST (`x += ...`) and
// PostfixExprAST (`x++`/`x--`) also mutate a tracked variable, and
// runOnCFG() never accounted for that (a pre-existing precision gap in that
// diagnostic pass, out of OPT-002's scope to fix -- it only costs that pass
// a missed compile-time diagnostic, and the runtime check remains the real
// safety net there). Here, invalidating (rather than silently keeping a
// stale value) is the only sound choice: this pass computing its own new
// value for the RHS of `+=`/`++` and reusing it as elision "proof" is left
// to a future session (this one only needs to never be WRONG, not to be
// maximally precise) -- see this group's change record.
static void applyTransfer(ExprAST *Stmt, ConstMap &Cur) {
  if (auto *Vd = dynamic_cast<VarDeclExprAST *>(Stmt)) {
    auto Val = evalConstExpr(Vd->getInitializer(), Cur);
    if (const VariableSymbol *S = Vd->getResolvedSymbol()) Cur[S] = Val;
    return;
  }
  if (auto *A = dynamic_cast<AssignmentExprAST *>(Stmt)) {
    if (auto *L = dynamic_cast<const VariableExprAST *>(A->getLHS())) {
      auto Val = evalConstExpr(A->getRHS(), Cur);
      if (const VariableSymbol *S = L->getResolvedSymbol()) Cur[S] = Val;
    }
    return;
  }
  if (auto *CA = dynamic_cast<CompoundAssignmentExprAST *>(Stmt)) {
    if (auto *L = dynamic_cast<const VariableExprAST *>(CA->getLHS()))
      if (const VariableSymbol *S = L->getResolvedSymbol()) Cur[S] = std::nullopt;
    return;
  }
  if (auto *P = dynamic_cast<PostfixExprAST *>(Stmt)) {
    if (auto *V = dynamic_cast<const VariableExprAST *>(P->getOperand()))
      if (const VariableSymbol *S = V->getResolvedSymbol()) Cur[S] = std::nullopt;
    return;
  }
}

// Marks every node this dataflow proves safe, recursing with the same node
// coverage as collectAddressTaken()/checkAccesses() above. Needs a
// non-const ExprAST* throughout (the entire point is to mutate
// ProvenInBounds/ProvenNoOverflow), so getters whose declared return type is
// `const ExprAST *` are const_cast back -- these are still genuinely
// non-const objects owned by the same Body tree Codegen is about to walk;
// the const on those particular getters is a read-only-consumer API
// convention elsewhere, not a true const object.
static void markNode(ExprAST *N, const ConstMap &Cur,
                     const std::unordered_set<const VariableSymbol *> &AddressTaken) {
  if (!N) return;

  if (auto *AA = dynamic_cast<ArrayAccessExprAST *>(N)) {
    auto IdxVal = evalConstExprElidable(AA->getIndex(), Cur, AddressTaken);
    if (IdxVal.has_value()) {
      if (auto *BaseVar = dynamic_cast<const VariableExprAST *>(AA->getBase())) {
        const VariableSymbol *S = BaseVar->getResolvedSymbol();
        if (S && S->Type && S->Type->isArray() && !AddressTaken.count(S)) {
          if (const auto *AT = dynamic_cast<const ArrayType *>(S->Type.get())) {
            int Sz = AT->getSize();
            long long Iv = *IdxVal;
            if (Sz > 0 && Iv >= 0 && Iv < Sz)
              AA->ProvenInBounds = true;
          }
        }
      }
    }
    markNode(const_cast<ExprAST *>(AA->getBase()), Cur, AddressTaken);
    markNode(const_cast<ExprAST *>(AA->getIndex()), Cur, AddressTaken);
    return;
  }

  if (auto *B = dynamic_cast<BinaryExprAST *>(N)) {
    auto Lv = evalConstExprElidable(B->getLHS(), Cur, AddressTaken);
    auto Rv = evalConstExprElidable(B->getRHS(), Cur, AddressTaken);
    if (Lv.has_value() && Rv.has_value()) {
      int Op = B->getOp();
      bool Safe = false;
      if (Op == '+' || Op == '-' || Op == '*')
        Safe = arithProvablySafe(static_cast<char>(Op), *Lv, *Rv);
      else if (Op == TOK_LSHIFT)
        Safe = shlProvablySafe(*Lv, *Rv);
      if (Safe) B->ProvenNoOverflow = true;
    }
    markNode(const_cast<ExprAST *>(B->getLHS()), Cur, AddressTaken);
    markNode(const_cast<ExprAST *>(B->getRHS()), Cur, AddressTaken);
    return;
  }

  if (auto *U = dynamic_cast<UnaryExprAST *>(N)) {
    if (U->getOpcode() == '-') {
      auto Ov = evalConstExprElidable(U->getOperand(), Cur, AddressTaken);
      if (Ov.has_value() && negProvablySafe(*Ov)) U->ProvenNoOverflow = true;
    }
    markNode(const_cast<ExprAST *>(U->getOperand()), Cur, AddressTaken);
    return;
  }

  if (auto *Tr = dynamic_cast<TryExprAST *>(N)) { // LEC-004
    markNode(Tr->getOperand(), Cur, AddressTaken);
    return;
  }

  if (auto *CA = dynamic_cast<CompoundAssignmentExprAST *>(N)) {
    int Op = CA->getOpcode();
    if (Op == '+' || Op == '-' || Op == '*' || Op == TOK_LSHIFT) {
      auto Lv = evalConstExprElidable(CA->getLHS(), Cur, AddressTaken);
      auto Rv = evalConstExprElidable(CA->getRHS(), Cur, AddressTaken);
      if (Lv.has_value() && Rv.has_value()) {
        bool Safe = (Op == TOK_LSHIFT) ? shlProvablySafe(*Lv, *Rv)
                                       : arithProvablySafe(static_cast<char>(Op), *Lv, *Rv);
        if (Safe) CA->ProvenNoOverflow = true;
      }
    }
    markNode(const_cast<ExprAST *>(CA->getLHS()), Cur, AddressTaken);
    markNode(const_cast<ExprAST *>(CA->getRHS()), Cur, AddressTaken);
    return;
  }

  if (auto *P = dynamic_cast<PostfixExprAST *>(N)) {
    auto Ov = evalConstExprElidable(P->getOperand(), Cur, AddressTaken);
    if (Ov.has_value()) {
      bool Safe = (P->getOpcode() == '+') ? arithProvablySafe('+', *Ov, 1)
                                          : arithProvablySafe('-', *Ov, 1);
      if (Safe) P->ProvenNoOverflow = true;
    }
    markNode(const_cast<ExprAST *>(P->getOperand()), Cur, AddressTaken);
    return;
  }

  if (auto *A = dynamic_cast<AssignmentExprAST *>(N)) {
    markNode(const_cast<ExprAST *>(A->getRHS()), Cur, AddressTaken);
    markNode(const_cast<ExprAST *>(A->getLHS()), Cur, AddressTaken);
  } else if (auto *Vd = dynamic_cast<VarDeclExprAST *>(N)) {
    markNode(const_cast<ExprAST *>(Vd->getInitializer()), Cur, AddressTaken);
  } else if (auto *If = dynamic_cast<IfExprAST *>(N)) {
    for (const auto &Br : If->getBranches()) {
      markNode(const_cast<ExprAST *>(Br.Cond.get()), Cur, AddressTaken);
      markNode(const_cast<ExprAST *>(Br.Body.get()), Cur, AddressTaken);
    }
    markNode(const_cast<ExprAST *>(If->getElse()), Cur, AddressTaken);
  } else if (auto *Bl = dynamic_cast<BlockExprAST *>(N)) {
    for (const auto &E2 : Bl->getExpressions()) markNode(E2.get(), Cur, AddressTaken);
  } else if (auto *F = dynamic_cast<FuncCallExprAST *>(N)) {
    for (const auto &Arg : F->getArgs()) markNode(Arg.get(), Cur, AddressTaken);
    markNode(const_cast<ExprAST *>(F->getCallee()), Cur, AddressTaken);
  } else if (auto *R = dynamic_cast<ReturnExprAST *>(N)) {
    markNode(const_cast<ExprAST *>(R->getVal()), Cur, AddressTaken);
  } else if (auto *M = dynamic_cast<MethodCallExprAST *>(N)) {
    markNode(const_cast<ExprAST *>(M->getBase()), Cur, AddressTaken);
    for (const auto &Arg : M->getArgs()) markNode(Arg.get(), Cur, AddressTaken);
  } else if (auto *FA = dynamic_cast<FieldAccessExprAST *>(N)) {
    markNode(const_cast<ExprAST *>(FA->getBase()), Cur, AddressTaken);
  } else if (auto *AL = dynamic_cast<ArrayLiteralExprAST *>(N)) {
    for (const auto &E2 : AL->getElements()) markNode(E2.get(), Cur, AddressTaken);
  } else if (auto *AD = dynamic_cast<ArrayDeclExprAST *>(N)) {
    for (const auto &E2 : AD->getInitList()) markNode(E2.get(), Cur, AddressTaken);
    if (AD->getInitExpr()) markNode(const_cast<ExprAST *>(AD->getInitExpr().get()), Cur, AddressTaken);
  } else if (auto *C = dynamic_cast<CastExprAST *>(N)) {
    markNode(const_cast<ExprAST *>(C->getOperand()), Cur, AddressTaken);
  }
}

void ValueRangePass::markElidableChecks(CFG &Cfg,
                                        const std::vector<SymbolID> & /*InitialAssigned*/) {
  const auto &Blocks = Cfg.getBlocks();
  size_t N = Blocks.size();
  if (N == 0) return;

  std::unordered_set<const VariableSymbol *> AddressTaken;
  for (size_t i = 0; i < N; ++i)
    for (ExprAST *Stmt : Blocks[i]->getStatements())
      collectAddressTaken(Stmt, AddressTaken);

  std::unordered_map<const TurfBasicBlock *, size_t> index;
  for (size_t i = 0; i < N; ++i) index[Blocks[i].get()] = i;

  std::vector<ConstMap> IN(N), OUT(N);
  std::queue<size_t> work;
  for (size_t i = 0; i < N; ++i) work.push(i);

  // Phase 1: converge IN[]/OUT[] -- transfer only, no marking, so every
  // mark in Phase 2 comes from dataflow state already proven stable on
  // EVERY path, never a not-yet-converged intermediate estimate.
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
    for (ExprAST *Stmt : BB->getStatements()) applyTransfer(Stmt, Cur);

    if (Cur != OUT[b]) {
      OUT[b] = Cur;
      for (auto *S : BB->getSuccessors()) work.push(index[S]);
    }
  }

  // Phase 2: one final walk per block from the converged IN[], marking
  // every node this dataflow proves safe.
  for (size_t i = 0; i < N; ++i) {
    ConstMap Cur = IN[i];
    for (ExprAST *Stmt : Blocks[i]->getStatements()) {
      markNode(Stmt, Cur, AddressTaken);
      applyTransfer(Stmt, Cur);
    }
  }
}
