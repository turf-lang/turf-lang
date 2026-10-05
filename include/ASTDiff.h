#ifndef AST_DIFF_H
#define AST_DIFF_H

// AST diffing/traversal helpers used by the sandbox incremental-validation
// subsystem (Sandbox.cpp). Kept separate from the 4 node-category headers
// (ASTExpr.h/ASTStmt.h/ASTDecl.h/ASTAccess.h) because these functions
// dynamic_cast across EVERY concrete AST node type, so they must be
// compiled after all of them are visible -- they don't belong to any one
// category the way the node classes themselves do.
#include "AST.h"

inline std::unique_ptr<ExprAST> CloneExprAST(const ExprAST *Node) {
  return Node ? Node->clone() : nullptr;
}

inline std::vector<std::unique_ptr<ExprAST>>
CloneExprList(const std::vector<std::unique_ptr<ExprAST>> &Nodes) {
  std::vector<std::unique_ptr<ExprAST>> Result;
  Result.reserve(Nodes.size());
  for (const auto &Node : Nodes) {
    Result.push_back(CloneExprAST(Node.get()));
  }
  return Result;
}

// CollectAllNodeIDsInternal(): every node ID reachable from a tree (the
// Sandbox's fallback when the baseline AST is missing, so incremental
// validation still sees the whole tree). Defined in ASTChildWalker.h as an
// ASTChildWalker -- it was a hand-written dynamic_cast chain that stopped at any
// node kind it had no branch for, so the IDs under a `recover`, `defer`,
// `unsafe` block or `compare` were never collected and incremental validation
// never linted them (LEC-005, INV-LINT-1). Declared here for the diffing code
// below; the definition needs the walker, which cannot be included from here
// (AST.h includes this file last).
inline void CollectAllNodeIDsInternal(const ExprAST *Node,
                                      std::set<uint64_t> &NodeIDs);

// AST Diffing Implementation: Per-node-type structural comparison
// Forward declaration for recursive diffing
inline std::set<uint64_t>
CollectChangedNodeIDsInternal(const ExprAST *OriginalAST,
                              const ExprAST *ModifiedAST,
                              std::set<uint64_t> &ChangedIDs);

// Recursive internal implementation of AST diffing
inline std::set<uint64_t>
CollectChangedNodeIDsInternal(const ExprAST *OriginalAST,
                              const ExprAST *ModifiedAST,
                              std::set<uint64_t> &ChangedIDs) {

  // Base case: null nodes
  if (!OriginalAST || !ModifiedAST) {
    if (OriginalAST)
      ChangedIDs.insert(OriginalAST->getNodeID());
    if (ModifiedAST)
      ChangedIDs.insert(ModifiedAST->getNodeID());
    return ChangedIDs;
  }

  // Check if the root nodes have different types
  if (typeid(*OriginalAST) != typeid(*ModifiedAST)) {
    ChangedIDs.insert(OriginalAST->getNodeID());
    ChangedIDs.insert(ModifiedAST->getNodeID());
    return ChangedIDs;
  }

  // MethodCallExprAST: Compare method, diff base and arguments
  if (auto *OrigMeth = dynamic_cast<const MethodCallExprAST *>(OriginalAST)) {
    auto *ModMeth = dynamic_cast<const MethodCallExprAST *>(ModifiedAST);

    if (OrigMeth->getMethod() != ModMeth->getMethod() ||
        OrigMeth->getArgs().size() != ModMeth->getArgs().size()) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    }

    CollectChangedNodeIDsInternal(OrigMeth->getBase(), ModMeth->getBase(),
                                  ChangedIDs);
    for (size_t i = 0; i < OrigMeth->getArgs().size(); ++i) {
      CollectChangedNodeIDsInternal(OrigMeth->getArgs()[i].get(),
                                    ModMeth->getArgs()[i].get(), ChangedIDs);
    }
    return ChangedIDs;
  }

  // NumberExprAST: Compare numeric values. Exact accessors, not
  // getIntVal()/getDoubleVal() -- this pass exists specifically to decide
  // whether a subtree needs re-checking, and the narrow, TRUNCATING
  // getIntVal() could make two genuinely DIFFERENT wide literals (e.g. two
  // distinct 48-digit numbers that happen to share the same low 64 bits)
  // compare as unchanged, silently skipping the re-check a real edit needs
  // (SEM-001, docs/planning/ARCHITECTURE_OVERHAUL.md §8 -- see this
  // group's own Risks note calling out ASTDiff/clone() as a ripple point).
  // llvm::APSInt::operator== compares by VALUE across differing bit
  // widths, so this is correct even when Orig/Mod happen to have been
  // stored at different widths for the same true value.
  if (auto *OrigNum = dynamic_cast<const NumberExprAST *>(OriginalAST)) {
    auto *ModNum = dynamic_cast<const NumberExprAST *>(ModifiedAST);
    if (OrigNum->isInteger() != ModNum->isInteger() ||
        (OrigNum->isInteger() &&
         OrigNum->getIntValExact() != ModNum->getIntValExact()) ||
        (!OrigNum->isInteger() &&
         !OrigNum->getFloatValExact().bitwiseIsEqual(ModNum->getFloatValExact()))) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    }
    return ChangedIDs;
  }

  // BoolExprAST: Compare boolean values
  if (auto *OrigBool = dynamic_cast<const BoolExprAST *>(OriginalAST)) {
    auto *ModBool = dynamic_cast<const BoolExprAST *>(ModifiedAST);
    if (OrigBool->getVal() != ModBool->getVal()) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    }
    return ChangedIDs;
  }

  // StringExprAST: Compare string values
  if (auto *OrigStr = dynamic_cast<const StringExprAST *>(OriginalAST)) {
    auto *ModStr = dynamic_cast<const StringExprAST *>(ModifiedAST);
    if (OrigStr->getValue() != ModStr->getValue()) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    }
    return ChangedIDs;
  }

  // VariableExprAST: Compare variable names
  if (auto *OrigVar = dynamic_cast<const VariableExprAST *>(OriginalAST)) {
    auto *ModVar = dynamic_cast<const VariableExprAST *>(ModifiedAST);
    if (OrigVar->getName() != ModVar->getName()) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    }
    return ChangedIDs;
  }

  if (auto *OrigComp =
          dynamic_cast<const CompoundAssignmentExprAST *>(OriginalAST)) {
    auto *ModComp =
        dynamic_cast<const CompoundAssignmentExprAST *>(ModifiedAST);
    if (OrigComp->getOpcode() != ModComp->getOpcode()) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    }
    CollectChangedNodeIDsInternal(OrigComp->getLHS(), ModComp->getLHS(),
                                  ChangedIDs);
    CollectChangedNodeIDsInternal(OrigComp->getRHS(), ModComp->getRHS(),
                                  ChangedIDs);
    return ChangedIDs;
  }

  if (auto *OrigPost = dynamic_cast<const PostfixExprAST *>(OriginalAST)) {
    auto *ModPost = dynamic_cast<const PostfixExprAST *>(ModifiedAST);
    // LANG-001 follow-up: IsPrefix, not just Opcode, since `x++` and `++x`
    // now share this one node type (see its own doc comment) but are
    // observably different code (old value vs. new value) -- comparing
    // Opcode alone would miss a suggested fix that silently swapped one
    // for the other.
    if (OrigPost->getOpcode() != ModPost->getOpcode() ||
        OrigPost->IsPrefix != ModPost->IsPrefix) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    }
    CollectChangedNodeIDsInternal(OrigPost->getOperand(), ModPost->getOperand(),
                                  ChangedIDs);
    return ChangedIDs;
  }

  // BinaryExprAST: Compare operator and recursively diff children
  if (auto *OrigBin = dynamic_cast<const BinaryExprAST *>(OriginalAST)) {
    auto *ModBin = dynamic_cast<const BinaryExprAST *>(ModifiedAST);
    if (OrigBin->getOp() != ModBin->getOp()) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    }

    // Recursively diff children
    CollectChangedNodeIDsInternal(OrigBin->getLHS(), ModBin->getLHS(),
                                  ChangedIDs);
    CollectChangedNodeIDsInternal(OrigBin->getRHS(), ModBin->getRHS(),
                                  ChangedIDs);
    return ChangedIDs;
  }

  // UnaryExprAST: Compare operator and recursively diff operand
  if (auto *OrigUnary = dynamic_cast<const UnaryExprAST *>(OriginalAST)) {
    auto *ModUnary = dynamic_cast<const UnaryExprAST *>(ModifiedAST);
    if (OrigUnary->getOpcode() != ModUnary->getOpcode()) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    }
    CollectChangedNodeIDsInternal(OrigUnary->getOperand(),
                                  ModUnary->getOperand(), ChangedIDs);
    return ChangedIDs;
  }

  // AssignmentExprAST: Recursively diff LHS and RHS
  if (auto *OrigAssign = dynamic_cast<const AssignmentExprAST *>(OriginalAST)) {
    auto *ModAssign = dynamic_cast<const AssignmentExprAST *>(ModifiedAST);
    CollectChangedNodeIDsInternal(OrigAssign->getLHS(), ModAssign->getLHS(),
                                  ChangedIDs);
    CollectChangedNodeIDsInternal(OrigAssign->getRHS(), ModAssign->getRHS(),
                                  ChangedIDs);
    return ChangedIDs;
  }

  // VarDeclExprAST: Compare name, type, and recursively diff initializer
  if (auto *OrigDecl = dynamic_cast<const VarDeclExprAST *>(OriginalAST)) {
    auto *ModDecl = dynamic_cast<const VarDeclExprAST *>(ModifiedAST);
    if (OrigDecl->getName() != ModDecl->getName() ||
        !OrigDecl->getType()->isEquivalent(ModDecl->getType())) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    }
    CollectChangedNodeIDsInternal(OrigDecl->getInitializer(),
                                  ModDecl->getInitializer(), ChangedIDs);
    return ChangedIDs;
  }

  // CastExprAST: Compare destination type and recursively diff operand
  if (auto *OrigCast = dynamic_cast<const CastExprAST *>(OriginalAST)) {
    auto *ModCast = dynamic_cast<const CastExprAST *>(ModifiedAST);
    if (!OrigCast->getDestType()->isEquivalent(ModCast->getDestType())) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    }
    CollectChangedNodeIDsInternal(OrigCast->getOperand(), ModCast->getOperand(),
                                  ChangedIDs);
    return ChangedIDs;
  }

  // BlockExprAST: Recursively diff all expressions in order
  if (auto *OrigBlock = dynamic_cast<const BlockExprAST *>(OriginalAST)) {
    auto *ModBlock = dynamic_cast<const BlockExprAST *>(ModifiedAST);
    const auto &OrigExprs = OrigBlock->getExpressions();
    const auto &ModExprs = ModBlock->getExpressions();

    // If size differs, mark as changed
    if (OrigExprs.size() != ModExprs.size()) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    } else {
      // Recursively diff each pair of expressions
      for (size_t i = 0; i < OrigExprs.size(); ++i) {
        CollectChangedNodeIDsInternal(OrigExprs[i].get(), ModExprs[i].get(),
                                      ChangedIDs);
      }
    }
    return ChangedIDs;
  }

  // IfExprAST: Recursively diff all branches and else body
  if (auto *OrigIf = dynamic_cast<const IfExprAST *>(OriginalAST)) {
    auto *ModIf = dynamic_cast<const IfExprAST *>(ModifiedAST);
    const auto &OrigBranches = OrigIf->getBranches();
    const auto &ModBranches = ModIf->getBranches();

    // If branch count differs, mark as changed
    if (OrigBranches.size() != ModBranches.size()) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    } else {
      // Recursively diff each branch's condition and body
      for (size_t i = 0; i < OrigBranches.size(); ++i) {
        CollectChangedNodeIDsInternal(OrigBranches[i].Cond.get(),
                                      ModBranches[i].Cond.get(), ChangedIDs);
        CollectChangedNodeIDsInternal(OrigBranches[i].Body.get(),
                                      ModBranches[i].Body.get(), ChangedIDs);
      }
    }

    // Diff else bodies
    CollectChangedNodeIDsInternal(OrigIf->getElseBody(), ModIf->getElseBody(),
                                  ChangedIDs);
    return ChangedIDs;
  }

  // WhileExprAST: Recursively diff condition and body
  if (auto *OrigWhile = dynamic_cast<const WhileExprAST *>(OriginalAST)) {
    auto *ModWhile = dynamic_cast<const WhileExprAST *>(ModifiedAST);
    CollectChangedNodeIDsInternal(OrigWhile->getCond(), ModWhile->getCond(),
                                  ChangedIDs);
    CollectChangedNodeIDsInternal(OrigWhile->getBody(), ModWhile->getBody(),
                                  ChangedIDs);
    return ChangedIDs;
  }

  // DoWhileExprAST: Recursively diff body and condition
  if (auto *OrigDoWhile = dynamic_cast<const DoWhileExprAST *>(OriginalAST)) {
    auto *ModDoWhile = dynamic_cast<const DoWhileExprAST *>(ModifiedAST);
    CollectChangedNodeIDsInternal(OrigDoWhile->getBody(), ModDoWhile->getBody(),
                                  ChangedIDs);
    CollectChangedNodeIDsInternal(OrigDoWhile->getCond(), ModDoWhile->getCond(),
                                  ChangedIDs);
    return ChangedIDs;
  }

  // ForExprAST: Recursively diff loop variable, range, and body
  if (auto *OrigFor = dynamic_cast<const ForExprAST *>(OriginalAST)) {
    auto *ModFor = dynamic_cast<const ForExprAST *>(ModifiedAST);
    if (OrigFor->getVarName() != ModFor->getVarName()) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    }
    CollectChangedNodeIDsInternal(OrigFor->getStart(), ModFor->getStart(),
                                  ChangedIDs);
    CollectChangedNodeIDsInternal(OrigFor->getEnd(), ModFor->getEnd(),
                                  ChangedIDs);
    CollectChangedNodeIDsInternal(OrigFor->getStep(), ModFor->getStep(),
                                  ChangedIDs);
    CollectChangedNodeIDsInternal(OrigFor->getBody(), ModFor->getBody(),
                                  ChangedIDs);
    return ChangedIDs;
  }

  // ForEachExprAST: Recursively diff loop variable(s), iterable, and body
  if (auto *OrigForEach = dynamic_cast<const ForEachExprAST *>(OriginalAST)) {
    auto *ModForEach = dynamic_cast<const ForEachExprAST *>(ModifiedAST);
    if (OrigForEach->getValueVarName() != ModForEach->getValueVarName() ||
        OrigForEach->getIndexVarName() != ModForEach->getIndexVarName()) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    }
    CollectChangedNodeIDsInternal(OrigForEach->getIterable(),
                                  ModForEach->getIterable(), ChangedIDs);
    CollectChangedNodeIDsInternal(OrigForEach->getBody(), ModForEach->getBody(),
                                  ChangedIDs);
    return ChangedIDs;
  }

  // FuncCallExprAST: Recursively diff callee and arguments
  if (auto *OrigCall = dynamic_cast<const FuncCallExprAST *>(OriginalAST)) {
    auto *ModCall = dynamic_cast<const FuncCallExprAST *>(ModifiedAST);
    CollectChangedNodeIDsInternal(OrigCall->getCallee(), ModCall->getCallee(),
                                  ChangedIDs);

    const auto &OrigArgs = OrigCall->getArgs();
    const auto &ModArgs = ModCall->getArgs();
    if (OrigArgs.size() != ModArgs.size()) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    } else {
      for (size_t i = 0; i < OrigArgs.size(); ++i) {
        CollectChangedNodeIDsInternal(OrigArgs[i].get(), ModArgs[i].get(),
                                      ChangedIDs);
      }
    }
    return ChangedIDs;
  }

  // FuncDefExprAST: Compare signature and recursively diff body
  if (auto *OrigFunc = dynamic_cast<const FuncDefExprAST *>(OriginalAST)) {
    auto *ModFunc = dynamic_cast<const FuncDefExprAST *>(ModifiedAST);
    bool signatureChanged = false;
    if (OrigFunc->getName() != ModFunc->getName() ||
        !OrigFunc->getReturnType()->isEquivalent(ModFunc->getReturnType())) {
      signatureChanged = true;
    } else if (OrigFunc->getParams().size() != ModFunc->getParams().size()) {
      signatureChanged = true;
    } else {
      for (size_t i = 0; i < OrigFunc->getParams().size(); ++i) {
        if (OrigFunc->getParams()[i].Name != ModFunc->getParams()[i].Name ||
            !OrigFunc->getParams()[i].Type->isEquivalent(ModFunc->getParams()[i].Type.get())) {
          signatureChanged = true;
          break;
        }
      }
    }
    if (signatureChanged) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    }
    CollectChangedNodeIDsInternal(OrigFunc->getBody(), ModFunc->getBody(),
                                  ChangedIDs);
    return ChangedIDs;
  }

  // ReturnExprAST: Recursively diff return value
  if (auto *OrigRet = dynamic_cast<const ReturnExprAST *>(OriginalAST)) {
    auto *ModRet = dynamic_cast<const ReturnExprAST *>(ModifiedAST);
    CollectChangedNodeIDsInternal(OrigRet->getVal(), ModRet->getVal(),
                                  ChangedIDs);
    return ChangedIDs;
  }


  // BreakExprAST / ContinueExprAST: No children, no state to compare
  if (dynamic_cast<const BreakExprAST *>(OriginalAST) ||
      dynamic_cast<const ContinueExprAST *>(OriginalAST)) {
    // These have no children and no state, so no difference
    return ChangedIDs;
  }

  // ArrayDeclExprAST: Compare name, element type, size, and recursively diff
  // init
  if (auto *OrigArr = dynamic_cast<const ArrayDeclExprAST *>(OriginalAST)) {
    auto *ModArr = dynamic_cast<const ArrayDeclExprAST *>(ModifiedAST);
    if (OrigArr->getName() != ModArr->getName() ||
        !OrigArr->getElementType()->isEquivalent(ModArr->getElementType()) ||
        OrigArr->getSize() != ModArr->getSize()) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    }

    const auto &OrigInit = OrigArr->getInitList();
    const auto &ModInit = ModArr->getInitList();
    if (OrigInit.size() != ModInit.size()) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    } else {
      for (size_t i = 0; i < OrigInit.size(); ++i) {
        CollectChangedNodeIDsInternal(OrigInit[i].get(), ModInit[i].get(),
                                      ChangedIDs);
      }
    }
    CollectChangedNodeIDsInternal(OrigArr->getInitExpr().get(), ModArr->getInitExpr().get(), ChangedIDs);
    return ChangedIDs;
  }

  // ArrayAccessExprAST: Recursively diff base and index
  if (auto *OrigAccess =
          dynamic_cast<const ArrayAccessExprAST *>(OriginalAST)) {
    auto *ModAccess = dynamic_cast<const ArrayAccessExprAST *>(ModifiedAST);
    CollectChangedNodeIDsInternal(OrigAccess->getBase(), ModAccess->getBase(),
                                  ChangedIDs);
    CollectChangedNodeIDsInternal(OrigAccess->getIndex(), ModAccess->getIndex(),
                                  ChangedIDs);
    return ChangedIDs;
  }

  // SliceExprAST: base and both (optional) bounds
  if (auto *OrigSlice = dynamic_cast<const SliceExprAST *>(OriginalAST)) {
    auto *ModSlice = dynamic_cast<const SliceExprAST *>(ModifiedAST);
    CollectChangedNodeIDsInternal(OrigSlice->getBase(), ModSlice->getBase(),
                                  ChangedIDs);
    CollectChangedNodeIDsInternal(OrigSlice->getStart(), ModSlice->getStart(),
                                  ChangedIDs);
    CollectChangedNodeIDsInternal(OrigSlice->getEnd(), ModSlice->getEnd(),
                                  ChangedIDs);
    return ChangedIDs;
  }



  // SizeOfExprAST: Compare type
  if (auto *OrigSizeOf = dynamic_cast<const SizeOfExprAST *>(OriginalAST)) {
    auto *ModSizeOf = dynamic_cast<const SizeOfExprAST *>(ModifiedAST);
    if (!OrigSizeOf->getTypeArg()->isEquivalent(
            ModSizeOf->getTypeArg().get())) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    }
    return ChangedIDs;
  }

  // ArrayLiteralExprAST
  if (auto *OrigArrLit = dynamic_cast<const ArrayLiteralExprAST *>(OriginalAST)) {
    auto *ModArrLit = dynamic_cast<const ArrayLiteralExprAST *>(ModifiedAST);
    const auto &OrigElems = OrigArrLit->getElements();
    const auto &ModElems = ModArrLit->getElements();
    if (OrigElems.size() != ModElems.size()) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    } else {
      for (size_t i = 0; i < OrigElems.size(); ++i) {
        CollectChangedNodeIDsInternal(OrigElems[i].get(), ModElems[i].get(), ChangedIDs);
      }
    }
    return ChangedIDs;
  }

  // StructDeclExprAST
  if (auto *OrigStructDecl = dynamic_cast<const StructDeclExprAST *>(OriginalAST)) {
    auto *ModStructDecl = dynamic_cast<const StructDeclExprAST *>(ModifiedAST);
    bool structChanged = false;
    if (OrigStructDecl->getName() != ModStructDecl->getName() ||
        OrigStructDecl->getTypeParams() != ModStructDecl->getTypeParams() ||
        OrigStructDecl->getFields().size() != ModStructDecl->getFields().size()) {
      structChanged = true;
    } else {
      const auto &OrigFields = OrigStructDecl->getFields();
      const auto &ModFields = ModStructDecl->getFields();
      for (size_t i = 0; i < OrigFields.size(); ++i) {
        if (OrigFields[i].Name != ModFields[i].Name ||
            !OrigFields[i].Type->isEquivalent(ModFields[i].Type.get())) {
          structChanged = true;
          break;
        }
      }
    }
    if (structChanged) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    }
    return ChangedIDs;
  }

  // FieldAccessExprAST
  if (auto *OrigFieldAccess = dynamic_cast<const FieldAccessExprAST *>(OriginalAST)) {
    auto *ModFieldAccess = dynamic_cast<const FieldAccessExprAST *>(ModifiedAST);
    if (OrigFieldAccess->getFieldName() != ModFieldAccess->getFieldName()) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    }
    CollectChangedNodeIDsInternal(OrigFieldAccess->getBase(), ModFieldAccess->getBase(), ChangedIDs);
    return ChangedIDs;
  }

  // ModuleImportExprAST
  if (auto *OrigImport = dynamic_cast<const ModuleImportExprAST *>(OriginalAST)) {
    auto *ModImport = dynamic_cast<const ModuleImportExprAST *>(ModifiedAST);
    if (OrigImport->getModuleName() != ModImport->getModuleName()) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    }
    CollectChangedNodeIDsInternal(OrigImport->getBody(), ModImport->getBody(), ChangedIDs);
    return ChangedIDs;
  }

  // ExternFuncDeclExprAST
  if (auto *OrigExtern = dynamic_cast<const ExternFuncDeclExprAST *>(OriginalAST)) {
    auto *ModExtern = dynamic_cast<const ExternFuncDeclExprAST *>(ModifiedAST);
    bool signatureChanged = false;
    if (OrigExtern->getName() != ModExtern->getName() ||
        OrigExtern->isVarArg() != ModExtern->isVarArg() ||
        OrigExtern->getLibName() != ModExtern->getLibName() ||
        !OrigExtern->getReturnType()->isEquivalent(ModExtern->getReturnType().get())) {
        signatureChanged = true;
    } else if (OrigExtern->getParams().size() != ModExtern->getParams().size()) {
        signatureChanged = true;
    } else {
        for (size_t i = 0; i < OrigExtern->getParams().size(); ++i) {
            if (OrigExtern->getParams()[i].first != ModExtern->getParams()[i].first ||
                !OrigExtern->getParams()[i].second->isEquivalent(ModExtern->getParams()[i].second.get())) {
                signatureChanged = true;
                break;
            }
        }
    }
    if (signatureChanged) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    }
    return ChangedIDs;
  }

  // list<T> declarations are now VarDeclExprAST; compare as VarDeclExprAST
  // below.

  // Mark both as changed to be safe
  ChangedIDs.insert(OriginalAST->getNodeID());
  ChangedIDs.insert(ModifiedAST->getNodeID());
  return ChangedIDs;
}

// Main entry point: Collect all changed node IDs
inline std::set<uint64_t> CollectChangedNodeIDs(const ExprAST *OriginalAST,
                                                const ExprAST *ModifiedAST) {
  std::set<uint64_t> ChangedIDs;
  if (!OriginalAST || !ModifiedAST) {
    if (OriginalAST)
      ChangedIDs.insert(OriginalAST->getNodeID());
    if (ModifiedAST)
      ChangedIDs.insert(ModifiedAST->getNodeID());
    return ChangedIDs;
  }
  CollectChangedNodeIDsInternal(OriginalAST, ModifiedAST, ChangedIDs);
  return ChangedIDs;
}

// Structural equality check (compares node type and key attributes)
inline bool AreNodesStructurallyEqual(const ExprAST *NodeA,
                                      const ExprAST *NodeB) {
  if (!NodeA || !NodeB)
    return NodeA == NodeB;

  // For now, use type-based comparison via RTTI or node ID comparison
  // In production, would need type-specific equality checks
  // This is a placeholder that returns false for different node IDs
  // (nodes are different instances, hence different IDs)
  return typeid(*NodeA) == typeid(*NodeB);
}

#endif
