#ifndef AST_H
#define AST_H

#include "Lexer.h"
#include "Types.h"
#include "llvm/IR/Value.h"
#include <memory>
#include <string>
#include <vector>
#include <set>

// Base Expressions Class: Everything, "5", "5 + 10", etc. are expressions
class ExprAST {
public:
  virtual ~ExprAST() = default;

  virtual llvm::Value *codegen() = 0;
  virtual std::unique_ptr<ExprAST> clone() const = 0;

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

// ────────────────────────────────────────────────────────────────────────────
// AST Diffing: Compare two ASTs and identify changed nodes
// ────────────────────────────────────────────────────────────────────────────
// CollectChangedNodeIDs: Traverse both ASTs in parallel and identify nodes that
// differ structurally. Used by sandbox validation to skip re-checking unchanged
// subtrees. Returns the set of node IDs that differ between the two trees.
std::set<uint64_t> CollectChangedNodeIDs(const ExprAST *OriginalAST,
                                         const ExprAST *ModifiedAST);

// Helper: Structural equality check for AST nodes (without considering codegen)
bool AreNodesStructurallyEqual(const ExprAST *NodeA, const ExprAST *NodeB);

// Number Node (Leaf)
class NumberExprAST : public ExprAST {
  double Val;       // Actual value, like "5", "5.0", etc.
  long long IntVal; // Exact integer value when IsInteger is true
  bool IsInteger = false;

public:
  explicit NumberExprAST(double Val) : Val(Val), IntVal(0), IsInteger(false) {}
  explicit NumberExprAST(long long Val)
      : Val(static_cast<double>(Val)), IntVal(Val), IsInteger(true) {}

  bool isInteger() const { return IsInteger; }
  long long getIntVal() const { return IntVal; }
  double getDoubleVal() const { return Val; }

  llvm::Value *codegen() override;
  std::unique_ptr<ExprAST> clone() const override {
    return IsInteger ? std::make_unique<NumberExprAST>(IntVal)
                     : std::make_unique<NumberExprAST>(Val);
  }
};

// Binary Operation Node (Branch): Holds the Operator ('+'), the Left side (A),
// and the Right side (B).
class BinaryExprAST : public ExprAST {
  int Op; // The operator, like '+', '-', etc.
  SourceLocation Loc;
  std::unique_ptr<ExprAST> LHS;
  std::unique_ptr<ExprAST> RHS;

public:
  BinaryExprAST(SourceLocation Loc, char Op, std::unique_ptr<ExprAST> LHS,
                std::unique_ptr<ExprAST> RHS)
      : Loc(Loc), Op(Op), LHS(std::move(LHS)), RHS(std::move(RHS)) {}

  const SourceLocation &getLoc() const { return Loc; }
  int getOp() const { return Op; }
  const ExprAST *getLHS() const { return LHS.get(); }
  const ExprAST *getRHS() const { return RHS.get(); }

  llvm::Value *codegen() override;
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<BinaryExprAST>(Loc, static_cast<char>(Op),
                                           CloneExprAST(LHS.get()),
                                           CloneExprAST(RHS.get()));
  }
};

// Assignment Node, represents things like "x = 5 + 2"
class AssignmentExprAST : public ExprAST {
  std::string Name;
  SourceLocation Loc;
  std::unique_ptr<ExprAST> RHS;

public:
  AssignmentExprAST(SourceLocation Loc, std::string Name,
                    std::unique_ptr<ExprAST> RHS)
      : Name(std::move(Name)), Loc(Loc), RHS(std::move(RHS)) {}

  const SourceLocation &getLoc() const { return Loc; }
  const std::string &getName() const { return Name; }
  const ExprAST *getRHS() const { return RHS.get(); }

  llvm::Value *codegen() override;
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<AssignmentExprAST>(Loc, Name,
                                               CloneExprAST(RHS.get()));
  }
};

// Variable Node, represents variable name like "x", "y", etc.
class VariableExprAST : public ExprAST {
  std::string Name;
  SourceLocation Loc;

public:
  VariableExprAST(SourceLocation Loc, std::string Name)
      : Loc(Loc), Name(Name) {}

  const std::string &getName() const { return Name; }

  llvm::Value *codegen() override;
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<VariableExprAST>(Loc, Name);
  }
};

// A single condition + body pair (used for if and each elseif)
struct CondBranch {
  SourceLocation Loc; // location of 'if' or 'elseif' keyword
  std::unique_ptr<ExprAST> Cond;
  std::unique_ptr<ExprAST> Body;
};

// If-Expr AST, represents if-else branch
class IfExprAST : public ExprAST {
  // The first entry is the 'if' branch.
  // Subsequent entries are 'elseif' branches.
  std::vector<CondBranch> Branches;

  // Optional final 'else' body (nullptr if absent).
  std::unique_ptr<ExprAST> ElseBody;
  SourceLocation ElseLoc;

public:
  IfExprAST(std::vector<CondBranch> Branches,
            std::unique_ptr<ExprAST> ElseBody,
            SourceLocation ElseLoc = {0, 0})
      : Branches(std::move(Branches)), ElseBody(std::move(ElseBody)),
        ElseLoc(ElseLoc) {}

  // Primary location is the 'if' keyword
  const SourceLocation &getLoc() const { return Branches.front().Loc; }
  const SourceLocation &getElseLoc() const { return ElseLoc; }

  const std::vector<CondBranch> &getBranches() const { return Branches; }
  ExprAST *getElseBody() const { return ElseBody.get(); }

  // Convenience: backward-compatible accessors for single if/else
  ExprAST *getThen() const { return Branches.front().Body.get(); }
  ExprAST *getElse() const { return ElseBody.get(); }

  llvm::Value *codegen() override;
  std::unique_ptr<ExprAST> clone() const override {
    std::vector<CondBranch> ClonedBranches;
    ClonedBranches.reserve(Branches.size());
    for (const auto &Branch : Branches) {
      ClonedBranches.push_back({Branch.Loc, CloneExprAST(Branch.Cond.get()),
                                CloneExprAST(Branch.Body.get())});
    }
    return std::make_unique<IfExprAST>(std::move(ClonedBranches),
                                       CloneExprAST(ElseBody.get()), ElseLoc);
  }
};

class UnaryExprAST : public ExprAST {
  int Opcode;
  std::unique_ptr<ExprAST> Operand;

public:
  UnaryExprAST(char Opcode, std::unique_ptr<ExprAST> Operand)
      : Opcode(Opcode), Operand(std::move(Operand)) {}

  llvm::Value *codegen() override;
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<UnaryExprAST>(static_cast<char>(Opcode),
                                          CloneExprAST(Operand.get()));
  }
};

// Block Expression, represents { expr1; expr2; ... }
class BlockExprAST : public ExprAST {
  std::vector<std::unique_ptr<ExprAST>> Expressions;

public:
  BlockExprAST(std::vector<std::unique_ptr<ExprAST>> Expressions)
      : Expressions(std::move(Expressions)) {}

  const std::vector<std::unique_ptr<ExprAST>> &getExpressions() const {
    return Expressions;
  }

  llvm::Value *codegen() override;
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<BlockExprAST>(CloneExprList(Expressions));
  }
};

// Generic node for any builtin function call, e.g. print(x).
class BuiltinCallExprAST : public ExprAST {
  std::string Name;
  std::vector<std::unique_ptr<ExprAST>> Args;
  SourceLocation Loc;

public:
  BuiltinCallExprAST(SourceLocation Loc, std::string Name,
                     std::vector<std::unique_ptr<ExprAST>> Args)
      : Loc(Loc), Name(std::move(Name)), Args(std::move(Args)) {}

  const SourceLocation &getLoc() const { return Loc; }

  llvm::Value *codegen() override;
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<BuiltinCallExprAST>(Loc, Name,
                                                CloneExprList(Args));
  }
};

class WhileExprAST : public ExprAST {
  SourceLocation Loc;
  std::unique_ptr<ExprAST> Cond, Body;

public:
  WhileExprAST(SourceLocation Loc, std::unique_ptr<ExprAST> Cond,
               std::unique_ptr<ExprAST> Body)
      : Loc(Loc), Cond(std::move(Cond)), Body(std::move(Body)) {}

  const SourceLocation &getLoc() const { return Loc; }
  ExprAST *getBody() const { return Body.get(); }
  const ExprAST *getCond() const { return Cond.get(); }

  llvm::Value *codegen() override;
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<WhileExprAST>(Loc, CloneExprAST(Cond.get()),
                                          CloneExprAST(Body.get()));
  }
};

// For-loop: for <var> in <start>..<end> step <step_expr> { body }
class ForExprAST : public ExprAST {
  std::string VarName;
  SourceLocation Loc;
  std::unique_ptr<ExprAST> Start, End, Step, Body;

public:
  ForExprAST(SourceLocation Loc, std::string VarName,
             std::unique_ptr<ExprAST> Start, std::unique_ptr<ExprAST> End,
             std::unique_ptr<ExprAST> Step, std::unique_ptr<ExprAST> Body)
      : Loc(Loc), VarName(std::move(VarName)), Start(std::move(Start)),
        End(std::move(End)), Step(std::move(Step)), Body(std::move(Body)) {}

  ExprAST *getBody() const { return Body.get(); }
  llvm::Value *codegen() override;
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<ForExprAST>(Loc, VarName,
                                        CloneExprAST(Start.get()),
                                        CloneExprAST(End.get()),
                                        CloneExprAST(Step.get()),
                                        CloneExprAST(Body.get()));
  }
};

class BreakExprAST : public ExprAST {
  SourceLocation Loc;

public:
  BreakExprAST(SourceLocation Loc) : Loc(Loc) {}
  const SourceLocation &getLoc() const { return Loc; }
  llvm::Value *codegen() override;
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<BreakExprAST>(Loc);
  }
};

class ContinueExprAST : public ExprAST {
  SourceLocation Loc;

public:
  ContinueExprAST(SourceLocation Loc) : Loc(Loc) {}
  const SourceLocation &getLoc() const { return Loc; }
  llvm::Value *codegen() override;
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<ContinueExprAST>(Loc);
  }
};

class VarDeclExprAST : public ExprAST {
  std::string Name;
  TurfType Type;
  std::unique_ptr<ExprAST> InitVal;
  SourceLocation Loc;

public:
  VarDeclExprAST(SourceLocation Loc, std::string Name, TurfType Type,
                 std::unique_ptr<ExprAST> InitVal)
      : Name(std::move(Name)), Type(Type), InitVal(std::move(InitVal)),
        Loc(Loc) {}

  const SourceLocation &getLoc() const { return Loc; }
  const std::string &getName() const { return Name; }
  TurfType getType() const { return Type; }

  llvm::Value *codegen() override;
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<VarDeclExprAST>(Loc, Name, Type,
                                            CloneExprAST(InitVal.get()));
  }
};

class BoolExprAST : public ExprAST {
  bool Val;

public:
  BoolExprAST(bool Val) : Val(Val) {}
  bool getVal() const { return Val; }

  llvm::Value *codegen() override;
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<BoolExprAST>(Val);
  }
};

class StringExprAST : public ExprAST {
  std::string Val;

public:
  StringExprAST(std::string Val) : Val(std::move(Val)) {}
  const std::string &getValue() const { return Val; }
  llvm::Value *codegen() override;
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<StringExprAST>(Val);
  }
};

// Explicit type conversion: int(expr) / double(expr)
class CastExprAST : public ExprAST {
  TurfType DestType;
  std::unique_ptr<ExprAST> Operand;
  SourceLocation Loc;

public:
  CastExprAST(SourceLocation Loc, TurfType DestType,
              std::unique_ptr<ExprAST> Operand)
      : DestType(DestType), Operand(std::move(Operand)), Loc(Loc) {}
  llvm::Value *codegen() override;
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<CastExprAST>(Loc, DestType,
                                         CloneExprAST(Operand.get()));
  }
};

// A single function parameter descriptor.
struct ParamDecl {
  TurfType Type;
  std::string Name;
};

// return expr;  or  return;
class ReturnExprAST : public ExprAST {
  std::unique_ptr<ExprAST> Val;
  SourceLocation Loc;

public:
  ReturnExprAST(SourceLocation Loc, std::unique_ptr<ExprAST> Val)
      : Loc(Loc), Val(std::move(Val)) {}

  const SourceLocation &getLoc() const { return Loc; }
  llvm::Value *codegen() override;
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<ReturnExprAST>(Loc, CloneExprAST(Val.get()));
  }
};

// fn RetType Name(params...) { body }
class FuncDefExprAST : public ExprAST {
  std::string Name;
  TurfType ReturnType;
  std::vector<ParamDecl> Params;
  std::unique_ptr<ExprAST> Body;
  SourceLocation Loc;

public:
  FuncDefExprAST(SourceLocation Loc, std::string Name, TurfType ReturnType,
                 std::vector<ParamDecl> Params, std::unique_ptr<ExprAST> Body)
      : Loc(Loc), Name(std::move(Name)), ReturnType(ReturnType),
        Params(std::move(Params)), Body(std::move(Body)) {}

  llvm::Value *codegen() override;
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<FuncDefExprAST>(Loc, Name, ReturnType, Params,
                                            CloneExprAST(Body.get()));
  }
};

// Call to a user-defined function: name(arg1, arg2, ...)
class FuncCallExprAST : public ExprAST {
  std::string Name;
  std::vector<std::unique_ptr<ExprAST>> Args;
  SourceLocation Loc;

public:
  FuncCallExprAST(SourceLocation Loc, std::string Name,
                  std::vector<std::unique_ptr<ExprAST>> Args)
      : Loc(Loc), Name(std::move(Name)), Args(std::move(Args)) {}

  llvm::Value *codegen() override;
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<FuncCallExprAST>(Loc, Name, CloneExprList(Args));
  }
};

// Array declaration: int[5] arr   or   int[3] arr = [1, 2, 3]
class ArrayDeclExprAST : public ExprAST {
  std::string Name;
  TurfType ElementType;  // e.g. TURF_INT
  int Size;              // compile-time constant size
  std::vector<std::unique_ptr<ExprAST>> InitList; // optional initializer
  SourceLocation Loc;

public:
  ArrayDeclExprAST(SourceLocation Loc, std::string Name, TurfType ElementType,
                   int Size, std::vector<std::unique_ptr<ExprAST>> InitList)
      : Loc(Loc), Name(std::move(Name)), ElementType(ElementType), Size(Size),
        InitList(std::move(InitList)) {}

  const SourceLocation &getLoc() const { return Loc; }
  const std::string &getName() const { return Name; }
  TurfType getElementType() const { return ElementType; }
  int getSize() const { return Size; }

  llvm::Value *codegen() override;
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<ArrayDeclExprAST>(Loc, Name, ElementType, Size,
                                              CloneExprList(InitList));
  }
};

// Array element access: arr[index]
class ArrayAccessExprAST : public ExprAST {
  std::string Name;
  std::unique_ptr<ExprAST> Index;
  SourceLocation Loc;

public:
  ArrayAccessExprAST(SourceLocation Loc, std::string Name,
                     std::unique_ptr<ExprAST> Index)
      : Loc(Loc), Name(std::move(Name)), Index(std::move(Index)) {}

  const SourceLocation &getLoc() const { return Loc; }
  const std::string &getName() const { return Name; }

  llvm::Value *codegen() override;
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<ArrayAccessExprAST>(Loc, Name,
                                                CloneExprAST(Index.get()));
  }
};

// Array element assignment: arr[index] = value
class ArrayAssignExprAST : public ExprAST {
  std::string Name;
  std::unique_ptr<ExprAST> Index;
  std::unique_ptr<ExprAST> RHS;
  SourceLocation Loc;

public:
  ArrayAssignExprAST(SourceLocation Loc, std::string Name,
                     std::unique_ptr<ExprAST> Index,
                     std::unique_ptr<ExprAST> RHS)
      : Loc(Loc), Name(std::move(Name)), Index(std::move(Index)),
        RHS(std::move(RHS)) {}

  const SourceLocation &getLoc() const { return Loc; }
  const std::string &getName() const { return Name; }

  llvm::Value *codegen() override;
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<ArrayAssignExprAST>(Loc, Name,
                                                CloneExprAST(Index.get()),
                                                CloneExprAST(RHS.get()));
  }
};

// Array length: arr.length
class ArrayLengthExprAST : public ExprAST {
  std::string Name;
  SourceLocation Loc;

public:
  ArrayLengthExprAST(SourceLocation Loc, std::string Name)
      : Loc(Loc), Name(std::move(Name)) {}

  const SourceLocation &getLoc() const { return Loc; }
  const std::string &getName() const { return Name; }

  llvm::Value *codegen() override;
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<ArrayLengthExprAST>(Loc, Name);
  }
};

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

// ────────────────────────────────────────────────────────────────────────────
// AST Diffing Implementation
// ────────────────────────────────────────────────────────────────────────────
// Forward declaration for recursive diffing
// ────────────────────────────────────────────────────────────────────────────
// AST Diffing Implementation
// ────────────────────────────────────────────────────────────────────────────
// Recursive internal implementation of AST diffing
inline std::set<uint64_t> CollectChangedNodeIDsInternal(
    const ExprAST *OriginalAST, const ExprAST *ModifiedAST,
    std::set<uint64_t> &ChangedIDs) {
  
  // Base case: null nodes
  if (!OriginalAST || !ModifiedAST) {
    if (OriginalAST) ChangedIDs.insert(OriginalAST->getNodeID());
    if (ModifiedAST) ChangedIDs.insert(ModifiedAST->getNodeID());
    return ChangedIDs;
  }
  
  // Check if the root nodes have different types
  if (typeid(*OriginalAST) != typeid(*ModifiedAST)) {
    ChangedIDs.insert(OriginalAST->getNodeID());
    ChangedIDs.insert(ModifiedAST->getNodeID());
    return ChangedIDs;
  }
  
  // For now, mark this node as changed since we're comparing different instances
  // In production, would do detailed structural comparison here
  ChangedIDs.insert(OriginalAST->getNodeID());
  ChangedIDs.insert(ModifiedAST->getNodeID());
  
  // TODO: Implement per-node-type diffing
  // - For BinaryExprAST: compare operator, then recursively diff LHS and RHS
  // - For BlockExprAST: zip child lists, recursively diff each pair
  // - For IfExprAST: recursively diff condition, then, else branches
  // - For all other nodes: similar recursive traversal
  
  return ChangedIDs;
}

// Main entry point: Collect all changed node IDs
inline std::set<uint64_t> CollectChangedNodeIDs(const ExprAST *OriginalAST,
                                                 const ExprAST *ModifiedAST) {
  std::set<uint64_t> ChangedIDs;
  if (!OriginalAST || !ModifiedAST) {
    if (OriginalAST) ChangedIDs.insert(OriginalAST->getNodeID());
    if (ModifiedAST) ChangedIDs.insert(ModifiedAST->getNodeID());
    return ChangedIDs;
  }
  CollectChangedNodeIDsInternal(OriginalAST, ModifiedAST, ChangedIDs);
  return ChangedIDs;
}

// Structural equality check (compares node type and key attributes)
inline bool AreNodesStructurallyEqual(const ExprAST *NodeA, const ExprAST *NodeB) {
  if (!NodeA || !NodeB) return NodeA == NodeB;
  
  // For now, use type-based comparison via RTTI or node ID comparison
  // In production, would need type-specific equality checks
  // This is a placeholder that returns false for different node IDs
  // (nodes are different instances, hence different IDs)
  return typeid(*NodeA) == typeid(*NodeB);
}

#endif
