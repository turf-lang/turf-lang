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

  int getOpcode() const { return Opcode; }
  const ExprAST *getOperand() const { return Operand.get(); }

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
  const std::string &getName() const { return Name; }
  const std::vector<std::unique_ptr<ExprAST>> &getArgs() const { return Args; }

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

  const std::string &getVarName() const { return VarName; }
  ExprAST *getBody() const { return Body.get(); }
  const ExprAST *getStart() const { return Start.get(); }
  const ExprAST *getEnd() const { return End.get(); }
  const ExprAST *getStep() const { return Step.get(); }
  
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
  
  TurfType getDestType() const { return DestType; }
  const ExprAST *getOperand() const { return Operand.get(); }
  
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
  const ExprAST *getVal() const { return Val.get(); }
  
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

  const std::string &getName() const { return Name; }
  TurfType getReturnType() const { return ReturnType; }
  const ExprAST *getBody() const { return Body.get(); }
  const std::vector<ParamDecl> &getParams() const { return Params; }

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

  const std::string &getName() const { return Name; }
  const std::vector<std::unique_ptr<ExprAST>> &getArgs() const { return Args; }

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
  const std::vector<std::unique_ptr<ExprAST>> &getInitList() const { return InitList; }

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
  const ExprAST *getIndex() const { return Index.get(); }

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
  const ExprAST *getIndex() const { return Index.get(); }
  const ExprAST *getRHS() const { return RHS.get(); }

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

// list<T> declaration: list<int> nums
class ListDeclExprAST : public ExprAST {
  std::string Name;
  TurfType ElementType; // e.g. TURF_INT
  SourceLocation Loc;

public:
  ListDeclExprAST(SourceLocation Loc, std::string Name, TurfType ElementType)
      : Loc(Loc), Name(std::move(Name)), ElementType(ElementType) {}

  const SourceLocation &getLoc() const { return Loc; }
  const std::string &getName() const { return Name; }
  TurfType getElementType() const { return ElementType; }

  llvm::Value *codegen() override;
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<ListDeclExprAST>(Loc, Name, ElementType);
  }
};

// list method call: nums.push(val)  /  nums.pop()  /  nums.length
class ListMethodCallExprAST : public ExprAST {
  std::string ListName;   // variable name
  std::string Method;     // "push", "pop", or "length"
  std::unique_ptr<ExprAST> Arg; // non-null only for push
  SourceLocation Loc;

public:
  ListMethodCallExprAST(SourceLocation Loc, std::string ListName,
                        std::string Method,
                        std::unique_ptr<ExprAST> Arg = nullptr)
      : Loc(Loc), ListName(std::move(ListName)), Method(std::move(Method)),
        Arg(std::move(Arg)) {}

  const SourceLocation &getLoc() const { return Loc; }
  const std::string &getListName() const { return ListName; }
  const std::string &getMethod() const { return Method; }
  const ExprAST *getArg() const { return Arg.get(); }

  llvm::Value *codegen() override;
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<ListMethodCallExprAST>(Loc, ListName, Method,
                                                   CloneExprAST(Arg.get()));
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
// AST Diffing Implementation: Per-node-type structural comparison
// ────────────────────────────────────────────────────────────────────────────
// Forward declaration for recursive diffing
inline std::set<uint64_t> CollectChangedNodeIDsInternal(
    const ExprAST *OriginalAST, const ExprAST *ModifiedAST,
    std::set<uint64_t> &ChangedIDs);

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
  
  // ============================================================================
  // NumberExprAST: Compare numeric values
  // ============================================================================
  if (auto *OrigNum = dynamic_cast<const NumberExprAST*>(OriginalAST)) {
    auto *ModNum = dynamic_cast<const NumberExprAST*>(ModifiedAST);
    if (OrigNum->isInteger() != ModNum->isInteger() ||
        (OrigNum->isInteger() && OrigNum->getIntVal() != ModNum->getIntVal()) ||
        (!OrigNum->isInteger() && OrigNum->getDoubleVal() != ModNum->getDoubleVal())) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    }
    return ChangedIDs;
  }
  
  // ============================================================================
  // BoolExprAST: Compare boolean values
  // ============================================================================
  if (auto *OrigBool = dynamic_cast<const BoolExprAST*>(OriginalAST)) {
    auto *ModBool = dynamic_cast<const BoolExprAST*>(ModifiedAST);
    if (OrigBool->getVal() != ModBool->getVal()) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    }
    return ChangedIDs;
  }
  
  // ============================================================================
  // StringExprAST: Compare string values
  // ============================================================================
  if (auto *OrigStr = dynamic_cast<const StringExprAST*>(OriginalAST)) {
    auto *ModStr = dynamic_cast<const StringExprAST*>(ModifiedAST);
    if (OrigStr->getValue() != ModStr->getValue()) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    }
    return ChangedIDs;
  }
  
  // ============================================================================
  // VariableExprAST: Compare variable names
  // ============================================================================
  if (auto *OrigVar = dynamic_cast<const VariableExprAST*>(OriginalAST)) {
    auto *ModVar = dynamic_cast<const VariableExprAST*>(ModifiedAST);
    if (OrigVar->getName() != ModVar->getName()) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    }
    return ChangedIDs;
  }
  
  // ============================================================================
  // BinaryExprAST: Compare operator and recursively diff children
  // ============================================================================
  if (auto *OrigBin = dynamic_cast<const BinaryExprAST*>(OriginalAST)) {
    auto *ModBin = dynamic_cast<const BinaryExprAST*>(ModifiedAST);
    bool nodeChanged = false;
    
    if (OrigBin->getOp() != ModBin->getOp()) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
      nodeChanged = true;
    }
    
    // Recursively diff children
    CollectChangedNodeIDsInternal(OrigBin->getLHS(), ModBin->getLHS(), ChangedIDs);
    CollectChangedNodeIDsInternal(OrigBin->getRHS(), ModBin->getRHS(), ChangedIDs);
    return ChangedIDs;
  }
  
  // ============================================================================
  // UnaryExprAST: Compare operator and recursively diff operand
  // ============================================================================
  if (auto *OrigUnary = dynamic_cast<const UnaryExprAST*>(OriginalAST)) {
    auto *ModUnary = dynamic_cast<const UnaryExprAST*>(ModifiedAST);
    if (OrigUnary->getOpcode() != ModUnary->getOpcode()) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    }
    CollectChangedNodeIDsInternal(OrigUnary->getOperand(), ModUnary->getOperand(), ChangedIDs);
    return ChangedIDs;
  }
  
  // ============================================================================
  // AssignmentExprAST: Compare variable name and recursively diff RHS
  // ============================================================================
  if (auto *OrigAssign = dynamic_cast<const AssignmentExprAST*>(OriginalAST)) {
    auto *ModAssign = dynamic_cast<const AssignmentExprAST*>(ModifiedAST);
    if (OrigAssign->getName() != ModAssign->getName()) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    }
    CollectChangedNodeIDsInternal(OrigAssign->getRHS(), ModAssign->getRHS(), ChangedIDs);
    return ChangedIDs;
  }
  
  // ============================================================================
  // VarDeclExprAST: Compare name, type, and recursively diff initializer
  // ============================================================================
  if (auto *OrigDecl = dynamic_cast<const VarDeclExprAST*>(OriginalAST)) {
    auto *ModDecl = dynamic_cast<const VarDeclExprAST*>(ModifiedAST);
    if (OrigDecl->getName() != ModDecl->getName() || 
        OrigDecl->getType() != ModDecl->getType()) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    }
    // The initializer might be different
    // Can't directly access it, so traverse as children
    // This is handled implicitly by the type check above
    return ChangedIDs;
  }
  
  // ============================================================================
  // CastExprAST: Compare destination type and recursively diff operand
  // ============================================================================
  if (auto *OrigCast = dynamic_cast<const CastExprAST*>(OriginalAST)) {
    auto *ModCast = dynamic_cast<const CastExprAST*>(ModifiedAST);
    if (OrigCast->getDestType() != ModCast->getDestType()) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    }
    CollectChangedNodeIDsInternal(OrigCast->getOperand(), ModCast->getOperand(), ChangedIDs);
    return ChangedIDs;
  }
  
  // ============================================================================
  // BlockExprAST: Recursively diff all expressions in order
  // ============================================================================
  if (auto *OrigBlock = dynamic_cast<const BlockExprAST*>(OriginalAST)) {
    auto *ModBlock = dynamic_cast<const BlockExprAST*>(ModifiedAST);
    const auto &OrigExprs = OrigBlock->getExpressions();
    const auto &ModExprs = ModBlock->getExpressions();
    
    // If size differs, mark as changed
    if (OrigExprs.size() != ModExprs.size()) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    } else {
      // Recursively diff each pair of expressions
      for (size_t i = 0; i < OrigExprs.size(); ++i) {
        CollectChangedNodeIDsInternal(OrigExprs[i].get(), ModExprs[i].get(), ChangedIDs);
      }
    }
    return ChangedIDs;
  }
  
  // ============================================================================
  // IfExprAST: Recursively diff all branches and else body
  // ============================================================================
  if (auto *OrigIf = dynamic_cast<const IfExprAST*>(OriginalAST)) {
    auto *ModIf = dynamic_cast<const IfExprAST*>(ModifiedAST);
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
    CollectChangedNodeIDsInternal(OrigIf->getElseBody(), ModIf->getElseBody(), ChangedIDs);
    return ChangedIDs;
  }
  
  // ============================================================================
  // WhileExprAST: Recursively diff condition and body
  // ============================================================================
  if (auto *OrigWhile = dynamic_cast<const WhileExprAST*>(OriginalAST)) {
    auto *ModWhile = dynamic_cast<const WhileExprAST*>(ModifiedAST);
    CollectChangedNodeIDsInternal(OrigWhile->getCond(), ModWhile->getCond(), ChangedIDs);
    CollectChangedNodeIDsInternal(OrigWhile->getBody(), ModWhile->getBody(), ChangedIDs);
    return ChangedIDs;
  }
  
  // ============================================================================
  // ForExprAST: Recursively diff loop variable, range, and body
  // ============================================================================
  if (auto *OrigFor = dynamic_cast<const ForExprAST*>(OriginalAST)) {
    auto *ModFor = dynamic_cast<const ForExprAST*>(ModifiedAST);
    if (OrigFor->getVarName() != ModFor->getVarName()) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    }
    CollectChangedNodeIDsInternal(OrigFor->getStart(), ModFor->getStart(), ChangedIDs);
    CollectChangedNodeIDsInternal(OrigFor->getEnd(), ModFor->getEnd(), ChangedIDs);
    CollectChangedNodeIDsInternal(OrigFor->getStep(), ModFor->getStep(), ChangedIDs);
    CollectChangedNodeIDsInternal(OrigFor->getBody(), ModFor->getBody(), ChangedIDs);
    return ChangedIDs;
  }
  
  // ============================================================================
  // BuiltinCallExprAST: Compare name and recursively diff arguments
  // ============================================================================
  if (auto *OrigCall = dynamic_cast<const BuiltinCallExprAST*>(OriginalAST)) {
    auto *ModCall = dynamic_cast<const BuiltinCallExprAST*>(ModifiedAST);
    if (OrigCall->getName() != ModCall->getName()) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    }
    
    const auto &OrigArgs = OrigCall->getArgs();
    const auto &ModArgs = ModCall->getArgs();
    if (OrigArgs.size() != ModArgs.size()) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    } else {
      for (size_t i = 0; i < OrigArgs.size(); ++i) {
        CollectChangedNodeIDsInternal(OrigArgs[i].get(), ModArgs[i].get(), ChangedIDs);
      }
    }
    return ChangedIDs;
  }
  
  // ============================================================================
  // FuncCallExprAST: Compare name and recursively diff arguments
  // ============================================================================
  if (auto *OrigCall = dynamic_cast<const FuncCallExprAST*>(OriginalAST)) {
    auto *ModCall = dynamic_cast<const FuncCallExprAST*>(ModifiedAST);
    if (OrigCall->getName() != ModCall->getName()) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    }
    
    const auto &OrigArgs = OrigCall->getArgs();
    const auto &ModArgs = ModCall->getArgs();
    if (OrigArgs.size() != ModArgs.size()) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    } else {
      for (size_t i = 0; i < OrigArgs.size(); ++i) {
        CollectChangedNodeIDsInternal(OrigArgs[i].get(), ModArgs[i].get(), ChangedIDs);
      }
    }
    return ChangedIDs;
  }
  
  // ============================================================================
  // FuncDefExprAST: Compare signature and recursively diff body
  // ============================================================================
  if (auto *OrigFunc = dynamic_cast<const FuncDefExprAST*>(OriginalAST)) {
    auto *ModFunc = dynamic_cast<const FuncDefExprAST*>(ModifiedAST);
    if (OrigFunc->getName() != ModFunc->getName() ||
        OrigFunc->getReturnType() != ModFunc->getReturnType()) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    }
    // Note: Parameter comparison would require getParams() accessor
    CollectChangedNodeIDsInternal(OrigFunc->getBody(), ModFunc->getBody(), ChangedIDs);
    return ChangedIDs;
  }
  
  // ============================================================================
  // ReturnExprAST: Recursively diff return value
  // ============================================================================
  if (auto *OrigRet = dynamic_cast<const ReturnExprAST*>(OriginalAST)) {
    auto *ModRet = dynamic_cast<const ReturnExprAST*>(ModifiedAST);
    CollectChangedNodeIDsInternal(OrigRet->getVal(), ModRet->getVal(), ChangedIDs);
    return ChangedIDs;
  }
  
  // ============================================================================
  // BreakExprAST / ContinueExprAST: No children, no state to compare
  // ============================================================================
  if (dynamic_cast<const BreakExprAST*>(OriginalAST) ||
      dynamic_cast<const ContinueExprAST*>(OriginalAST)) {
    // These have no children and no state, so no difference
    return ChangedIDs;
  }
  
  // ============================================================================
  // ArrayDeclExprAST: Compare name, element type, size, and recursively diff init
  // ============================================================================
  if (auto *OrigArr = dynamic_cast<const ArrayDeclExprAST*>(OriginalAST)) {
    auto *ModArr = dynamic_cast<const ArrayDeclExprAST*>(ModifiedAST);
    if (OrigArr->getName() != ModArr->getName() ||
        OrigArr->getElementType() != ModArr->getElementType() ||
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
        CollectChangedNodeIDsInternal(OrigInit[i].get(), ModInit[i].get(), ChangedIDs);
      }
    }
    return ChangedIDs;
  }
  
  // ============================================================================
  // ArrayAccessExprAST: Compare name and recursively diff index
  // ============================================================================
  if (auto *OrigAccess = dynamic_cast<const ArrayAccessExprAST*>(OriginalAST)) {
    auto *ModAccess = dynamic_cast<const ArrayAccessExprAST*>(ModifiedAST);
    if (OrigAccess->getName() != ModAccess->getName()) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    }
    CollectChangedNodeIDsInternal(OrigAccess->getIndex(), ModAccess->getIndex(), ChangedIDs);
    return ChangedIDs;
  }
  
  // ============================================================================
  // ArrayAssignExprAST: Compare name and recursively diff index and RHS
  // ============================================================================
  if (auto *OrigAssign = dynamic_cast<const ArrayAssignExprAST*>(OriginalAST)) {
    auto *ModAssign = dynamic_cast<const ArrayAssignExprAST*>(ModifiedAST);
    if (OrigAssign->getName() != ModAssign->getName()) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    }
    CollectChangedNodeIDsInternal(OrigAssign->getIndex(), ModAssign->getIndex(), ChangedIDs);
    CollectChangedNodeIDsInternal(OrigAssign->getRHS(), ModAssign->getRHS(), ChangedIDs);
    return ChangedIDs;
  }
  
  // ============================================================================
  // ArrayLengthExprAST: Compare name
  // ============================================================================
  if (auto *OrigLen = dynamic_cast<const ArrayLengthExprAST*>(OriginalAST)) {
    auto *ModLen = dynamic_cast<const ArrayLengthExprAST*>(ModifiedAST);
    if (OrigLen->getName() != ModLen->getName()) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    }
    return ChangedIDs;
  }

  // ============================================================================
  // ListDeclExprAST: Compare name and element type
  // ============================================================================
  if (auto *OrigList = dynamic_cast<const ListDeclExprAST*>(OriginalAST)) {
    auto *ModList = dynamic_cast<const ListDeclExprAST*>(ModifiedAST);
    if (OrigList->getName() != ModList->getName() ||
        OrigList->getElementType() != ModList->getElementType()) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    }
    return ChangedIDs;
  }

  // ============================================================================
  // ListMethodCallExprAST: Compare list name, method, and argument
  // ============================================================================
  if (auto *OrigMeth = dynamic_cast<const ListMethodCallExprAST*>(OriginalAST)) {
    auto *ModMeth = dynamic_cast<const ListMethodCallExprAST*>(ModifiedAST);
    if (OrigMeth->getListName() != ModMeth->getListName() ||
        OrigMeth->getMethod() != ModMeth->getMethod()) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    }
    CollectChangedNodeIDsInternal(OrigMeth->getArg(), ModMeth->getArg(), ChangedIDs);
    return ChangedIDs;
  }

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
