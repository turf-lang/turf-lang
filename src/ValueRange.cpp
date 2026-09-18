#include "ValueRange.h"
#include "AST.h"
#include "Errors.h"

#include "llvm/ADT/APInt.h"

#include <functional>
#include <optional>
#include <queue>
#include <unordered_map>

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
    // SEM-001 (docs/planning/ARCHITECTURE_OVERHAUL.md §8): this used to be
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
                      if (iv < 0 || iv >= sz) {
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
