#ifndef AST_H
#define AST_H

#include "frontend/ASTVisitor.h"
#include "frontend/Lexer.h"
#include "resolve/ResolverSymbolTable.h" // VariableSymbol — LLVM-free, safe to include here
#include "sema/Types.h"
#include "llvm/IR/Value.h"

namespace llvm { class AllocaInst; }
#include <memory>
#include <set>
#include <string>
#include <typeinfo>
#include <vector>

extern std::string CompilerExecutablePath;

// Base Expressions Class: Everything, "5", "5 + 10", etc. are expressions
class ExprAST {
public:
  virtual ~ExprAST() = default;

  virtual llvm::Value *codegen() = 0;
  virtual llvm::Value *codegenLValue() { return nullptr; }
  virtual std::unique_ptr<ExprAST> clone() const = 0;

  // Visitor pattern: dispatch to the correct visit* method on the visitor.
  virtual void accept(ASTVisitor &V) = 0;

  // Node ID tracking for AST diffing
  static void ResetNodeIDCounter();
  uint64_t getNodeID() const { return NodeID; }

protected:
  // Each node gets a unique ID when created, for diffing purposes
  uint64_t NodeID;
  static uint64_t NextNodeID;

  ExprAST() : NodeID(NextNodeID++) {}
};

std::unique_ptr<ExprAST> CloneExprAST(const ExprAST *Node);
std::vector<std::unique_ptr<ExprAST>>
CloneExprList(const std::vector<std::unique_ptr<ExprAST>> &Nodes);

// AST Diffing: Compare two ASTs and identify changed nodes
// CollectChangedNodeIDs: Traverse both ASTs in parallel and identify nodes that
// differ structurally. Used by sandbox validation to skip re-checking unchanged
// subtrees. Returns the set of node IDs that differ between the two trees.
std::set<uint64_t> CollectChangedNodeIDs(const ExprAST *OriginalAST,
                                         const ExprAST *ModifiedAST);

// Helper: Structural equality check for AST nodes (without considering codegen)
bool AreNodesStructurallyEqual(const ExprAST *NodeA, const ExprAST *NodeB);

// Umbrella: every concrete AST node class used to live directly in this
// file, grouped by category (mirrors how Parser/Resolver/TypeChecker/
// Codegen are each split the same way). Split out so `#include "frontend/AST.h"`
// still pulls in every node type (existing call sites are unaffected),
// while a file that only needs one category can include that header
// directly instead.
#include "frontend/ASTExpr.h"   // literals + operators: Number/Bool/Char/String/Null/
                       // DefaultInit/Binary/Unary/AddressOf/Compare/Cast/
                       // Postfix/Assignment/CompoundAssignment/Variable/SizeOf
#include "frontend/ASTStmt.h"   // control flow: Block/If/While/DoWhile/For/ForEach/
                       // Break/Continue/Return/Recover
#include "frontend/ASTDecl.h"   // declarations: VarDecl/FuncDef/ExternFuncDecl/
                       // StructDecl/EnumDecl/ErrorDecl/ArrayDecl/ModuleImport
#include "frontend/ASTAccess.h" // calls + member/module access: FuncCall/MethodCall/
                       // FieldAccess/ArrayAccess/ArrayLiteral/ScopeAccess

// Cross-cutting traversal/diffing helpers (CloneExprAST, CollectChangedNodeIDs,
// AreNodesStructurallyEqual, ...) -- these dynamic_cast across every concrete
// node type above, so they must come last.
#include "frontend/ASTDiff.h"

#endif
