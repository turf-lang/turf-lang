#ifndef AST_H
#define AST_H

#include "ASTVisitor.h"
#include "Lexer.h"
#include "Types.h"
#include "llvm/IR/Value.h"
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
  void accept(ASTVisitor &V) override { V.visitNumber(*this); }
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
  void accept(ASTVisitor &V) override { V.visitBinary(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<BinaryExprAST>(Loc, static_cast<char>(Op),
                                           CloneExprAST(LHS.get()),
                                           CloneExprAST(RHS.get()));
  }
};

// Assignment Node, represents things like "x = 5 + 2"
class AssignmentExprAST : public ExprAST {
  std::unique_ptr<ExprAST> LHS;
  SourceLocation Loc;
  std::unique_ptr<ExprAST> RHS;

public:
  AssignmentExprAST(SourceLocation Loc, std::unique_ptr<ExprAST> LHS,
                    std::unique_ptr<ExprAST> RHS)
      : LHS(std::move(LHS)), Loc(Loc), RHS(std::move(RHS)) {}

  const SourceLocation &getLoc() const { return Loc; }
  const ExprAST *getLHS() const { return LHS.get(); }
  const ExprAST *getRHS() const { return RHS.get(); }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitAssignment(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<AssignmentExprAST>(Loc, CloneExprAST(LHS.get()),
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
  llvm::Value *codegenLValue() override;
  void accept(ASTVisitor &V) override { V.visitVariable(*this); }
  const SourceLocation &getLoc() const { return Loc; }
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
  IfExprAST(std::vector<CondBranch> Branches, std::unique_ptr<ExprAST> ElseBody,
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
  void accept(ASTVisitor &V) override { V.visitIf(*this); }
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
  void accept(ASTVisitor &V) override { V.visitUnary(*this); }
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
  void accept(ASTVisitor &V) override { V.visitBlock(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<BlockExprAST>(CloneExprList(Expressions));
  }
};

class ModuleImportExprAST : public ExprAST {
  std::string ModuleName;
  std::unique_ptr<ExprAST> Body;

public:
  ModuleImportExprAST(std::string ModuleName, std::unique_ptr<ExprAST> Body)
      : ModuleName(std::move(ModuleName)), Body(std::move(Body)) {}

  const std::string &getModuleName() const { return ModuleName; }
  const ExprAST *getBody() const { return Body.get(); }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitModuleImport(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<ModuleImportExprAST>(ModuleName,
                                                 CloneExprAST(Body.get()));
  }
};

class SizeOfExprAST : public ExprAST {
  SourceLocation Loc;
  TurfTypePtr TypeArg;

public:
  SizeOfExprAST(SourceLocation Loc, TurfTypePtr TypeArg)
      : Loc(Loc), TypeArg(std::move(TypeArg)) {}

  const SourceLocation &getLoc() const { return Loc; }
  TurfTypePtr getTypeArg() const { return TypeArg; }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitSizeOf(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<SizeOfExprAST>(Loc, TypeArg);
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
  void accept(ASTVisitor &V) override { V.visitWhile(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<WhileExprAST>(Loc, CloneExprAST(Cond.get()),
                                          CloneExprAST(Body.get()));
  }
};
// For-loop: for <var> in <start>..<end> step <step_expr> { body }
// Note: ForExprAST desugars to init + while(cond) { body; step } during Codegen.
// The AST node is kept for clean visitor dispatch and future TypeChecker support.
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

  const SourceLocation &getLoc() const { return Loc; }  ///< Location of the 'for' keyword.
  const std::string &getVarName() const { return VarName; }
  ExprAST *getBody() const { return Body.get(); }
  const ExprAST *getStart() const { return Start.get(); }
  const ExprAST *getEnd() const { return End.get(); }
  const ExprAST *getStep() const { return Step.get(); }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitFor(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<ForExprAST>(
        Loc, VarName, CloneExprAST(Start.get()), CloneExprAST(End.get()),
        CloneExprAST(Step.get()), CloneExprAST(Body.get()));
  }
};

class BreakExprAST : public ExprAST {
  SourceLocation Loc;

public:
  BreakExprAST(SourceLocation Loc) : Loc(Loc) {}
  const SourceLocation &getLoc() const { return Loc; }
  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitBreak(*this); }
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
  void accept(ASTVisitor &V) override { V.visitContinue(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<ContinueExprAST>(Loc);
  }
};

class VarDeclExprAST : public ExprAST {
  std::string Name;
  TurfTypePtr Type;
  std::unique_ptr<ExprAST> InitVal;
  SourceLocation Loc;

public:
  VarDeclExprAST(SourceLocation Loc, std::string Name, TurfTypePtr Type,
                 std::unique_ptr<ExprAST> InitVal)
      : Name(std::move(Name)), Type(std::move(Type)),
        InitVal(std::move(InitVal)), Loc(Loc) {}

  const SourceLocation &getLoc() const { return Loc; }
  const std::string &getName() const { return Name; }
  const TurfType *getType() const { return Type.get(); }
  TurfTypePtr getTypePtr() const { return Type; }
  const ExprAST *getInitializer() const { return InitVal.get(); }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitVarDecl(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<VarDeclExprAST>(Loc, Name, Type,
                                            CloneExprAST(InitVal.get()));
  }
  // Kept private access for codegen
  const ExprAST *getInitVal() const { return InitVal.get(); }
};

class BoolExprAST : public ExprAST {
  bool Val;

public:
  BoolExprAST(bool Val) : Val(Val) {}
  bool getVal() const { return Val; }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitBool(*this); }
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
  void accept(ASTVisitor &V) override { V.visitString(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<StringExprAST>(Val);
  }
};

class NullExprAST : public ExprAST {
  SourceLocation Loc;

public:
  NullExprAST(SourceLocation Loc) : Loc(Loc) {}
  const SourceLocation &getLoc() const { return Loc; }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitNull(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<NullExprAST>(Loc);
  }
};

// Explicit type conversion: int(expr) / double(expr)
class CastExprAST : public ExprAST {
  TurfTypePtr DestType;
  std::unique_ptr<ExprAST> Operand;
  SourceLocation Loc;

public:
  CastExprAST(SourceLocation Loc, TurfTypePtr DestType,
              std::unique_ptr<ExprAST> Operand)
      : DestType(std::move(DestType)), Operand(std::move(Operand)), Loc(Loc) {}

  const TurfType *getDestType() const { return DestType.get(); }
  TurfTypePtr getDestTypePtr() const { return DestType; }
  const ExprAST *getOperand() const { return Operand.get(); }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitCast(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<CastExprAST>(Loc, DestType,
                                         CloneExprAST(Operand.get()));
  }
  // Kept for codegen access
  const SourceLocation &getLoc() const { return Loc; }
};

// A single function parameter descriptor.
struct ParamDecl {
  TurfTypePtr Type;
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
  void accept(ASTVisitor &V) override { V.visitReturn(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<ReturnExprAST>(Loc, CloneExprAST(Val.get()));
  }
};

// raise expr
class RaiseExprAST : public ExprAST {
  std::unique_ptr<ExprAST> Payload;
  SourceLocation Loc;

public:
  RaiseExprAST(SourceLocation Loc, std::unique_ptr<ExprAST> Payload)
      : Loc(Loc), Payload(std::move(Payload)) {}

  const SourceLocation &getLoc() const { return Loc; }
  const ExprAST *getPayload() const { return Payload.get(); }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitRaise(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<RaiseExprAST>(Loc, CloneExprAST(Payload.get()));
  }
};

// try { ... } onError (err) { ... }
class TryExprAST : public ExprAST {
  std::unique_ptr<ExprAST> TryBlock;
  std::string PayloadName;
  std::unique_ptr<ExprAST> CatchBlock;
  SourceLocation Loc;

public:
  TryExprAST(SourceLocation Loc, std::unique_ptr<ExprAST> TryBlock,
             std::string PayloadName, std::unique_ptr<ExprAST> CatchBlock)
      : Loc(Loc), TryBlock(std::move(TryBlock)),
        PayloadName(std::move(PayloadName)), CatchBlock(std::move(CatchBlock)) {
  }

  const SourceLocation &getLoc() const { return Loc; }
  const ExprAST *getTryBlock() const { return TryBlock.get(); }
  const std::string &getPayloadName() const { return PayloadName; }
  const ExprAST *getCatchBlock() const { return CatchBlock.get(); }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitTry(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<TryExprAST>(Loc, CloneExprAST(TryBlock.get()),
                                        PayloadName,
                                        CloneExprAST(CatchBlock.get()));
  }
};

// fn RetType Name<T>(params...) { body }
class FuncDefExprAST : public ExprAST {
  std::string Name;
  TurfTypePtr ReturnType;
  std::vector<ParamDecl> Params;
  std::unique_ptr<ExprAST> Body;
  std::string IntrinsicName;
  std::vector<std::string> TypeParams;
  SourceLocation Loc;

public:
  FuncDefExprAST(SourceLocation Loc, std::string Name, TurfTypePtr ReturnType,
                 std::vector<ParamDecl> Params, std::unique_ptr<ExprAST> Body,
                 std::string IntrinsicName = "",
                 std::vector<std::string> TypeParams = {})
      : Loc(Loc), Name(std::move(Name)), ReturnType(std::move(ReturnType)),
        Params(std::move(Params)), Body(std::move(Body)),
        IntrinsicName(std::move(IntrinsicName)), TypeParams(std::move(TypeParams)) {}

  const std::string &getName() const { return Name; }
  const TurfType *getReturnType() const { return ReturnType.get(); }
  TurfTypePtr getReturnTypePtr() const { return ReturnType; }
  ExprAST *getBodyMut() const { return Body.get(); }
  const ExprAST *getBody() const { return Body.get(); }
  const std::vector<ParamDecl> &getParams() const { return Params; }
  const std::vector<std::string> &getTypeParams() const { return TypeParams; }

  const std::string &getIntrinsicName() const { return IntrinsicName; }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitFuncDef(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<FuncDefExprAST>(
        Loc, Name, ReturnType, Params, CloneExprAST(Body.get()), IntrinsicName, TypeParams);
  }
};

// Call to a user-defined function: name(arg1, arg2, ...)
class FuncCallExprAST : public ExprAST {
  std::unique_ptr<ExprAST> Callee;
  std::vector<std::unique_ptr<ExprAST>> Args;
  SourceLocation Loc;

public:
  FuncCallExprAST(SourceLocation Loc, std::unique_ptr<ExprAST> Callee,
                  std::vector<std::unique_ptr<ExprAST>> Args)
      : Callee(std::move(Callee)), Args(std::move(Args)), Loc(Loc) {}
  const ExprAST *getCallee() const { return Callee.get(); }
  const SourceLocation &getLoc() const { return Loc; }
  const std::vector<std::unique_ptr<ExprAST>> &getArgs() const { return Args; }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitFuncCall(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<FuncCallExprAST>(Loc, CloneExprAST(Callee.get()),
                                             CloneExprList(Args));
  }
};

// Array declaration: int[5] arr   or   int[3] arr = [1, 2, 3]
class ArrayDeclExprAST : public ExprAST {
  std::string Name;
  TurfTypePtr ElementType;                        // e.g. TurfInt()
  int Size;                                       // compile-time constant size
  std::vector<std::unique_ptr<ExprAST>> InitList; // optional initializer
  SourceLocation Loc;

public:
  ArrayDeclExprAST(SourceLocation Loc, std::string Name,
                   TurfTypePtr ElementType, int Size,
                   std::vector<std::unique_ptr<ExprAST>> InitList)
      : Loc(Loc), Name(std::move(Name)), ElementType(std::move(ElementType)),
        Size(Size), InitList(std::move(InitList)) {}

  const SourceLocation &getLoc() const { return Loc; }
  const std::string &getName() const { return Name; }
  const TurfType *getElementType() const { return ElementType.get(); }
  TurfTypePtr getElementTypePtr() const { return ElementType; }
  int getSize() const { return Size; }
  const std::vector<std::unique_ptr<ExprAST>> &getInitList() const {
    return InitList;
  }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitArrayDecl(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<ArrayDeclExprAST>(Loc, Name, ElementType, Size,
                                              CloneExprList(InitList));
  }
};

// struct StructName<T> { type field1, type field2 }
class StructDeclExprAST : public ExprAST {
  std::string Name;
  std::vector<StructField> Fields;
  std::vector<std::string> TypeParams;
  SourceLocation Loc;

public:
  StructDeclExprAST(SourceLocation Loc, std::string Name,
                    std::vector<StructField> Fields,
                    std::vector<std::string> TypeParams = {})
      : Loc(Loc), Name(std::move(Name)), Fields(std::move(Fields)), TypeParams(std::move(TypeParams)) {}

  const SourceLocation &getLoc() const { return Loc; }
  const std::string &getName() const { return Name; }
  const std::vector<StructField> &getFields() const { return Fields; }
  const std::vector<std::string> &getTypeParams() const { return TypeParams; }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitStructDecl(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<StructDeclExprAST>(Loc, Name, Fields, TypeParams);
  }
};

// StructName(val1, val2)
class StructInitExprAST : public ExprAST {
  std::string Name;
  std::vector<std::unique_ptr<ExprAST>> Args;
  SourceLocation Loc;

public:
  StructInitExprAST(SourceLocation Loc, std::string Name,
                    std::vector<std::unique_ptr<ExprAST>> Args)
      : Loc(Loc), Name(std::move(Name)), Args(std::move(Args)) {}

  const SourceLocation &getLoc() const { return Loc; }
  const std::string &getName() const { return Name; }
  const std::vector<std::unique_ptr<ExprAST>> &getArgs() const { return Args; }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitStructInit(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<StructInitExprAST>(Loc, Name, CloneExprList(Args));
  }
};

// Base.Field
class FieldAccessExprAST : public ExprAST {
  std::unique_ptr<ExprAST> Base;
  std::string FieldName;
  SourceLocation Loc;

public:
  FieldAccessExprAST(SourceLocation Loc, std::unique_ptr<ExprAST> Base,
                     std::string FieldName)
      : Loc(Loc), Base(std::move(Base)), FieldName(std::move(FieldName)) {}

  const SourceLocation &getLoc() const { return Loc; }
  const ExprAST *getBase() const { return Base.get(); }
  const std::string &getFieldName() const { return FieldName; }

  llvm::Value *codegen() override;
  llvm::Value *codegenLValue() override;
  void accept(ASTVisitor &V) override { V.visitFieldAccess(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<FieldAccessExprAST>(Loc, CloneExprAST(Base.get()),
                                                FieldName);
  }
};

// Array element access: arr[index]
class ArrayAccessExprAST : public ExprAST {
  std::unique_ptr<ExprAST> Base;
  std::unique_ptr<ExprAST> Index;
  SourceLocation Loc;

public:
  ArrayAccessExprAST(SourceLocation Loc, std::unique_ptr<ExprAST> Base,
                     std::unique_ptr<ExprAST> Index)
      : Base(std::move(Base)), Index(std::move(Index)), Loc(Loc) {}

  const SourceLocation &getLoc() const { return Loc; }
  const ExprAST *getBase() const { return Base.get(); }
  const ExprAST *getIndex() const { return Index.get(); }

  llvm::Value *codegen() override;
  llvm::Value *codegenLValue() override;
  void accept(ASTVisitor &V) override { V.visitArrayAccess(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<ArrayAccessExprAST>(Loc, CloneExprAST(Base.get()),
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
  void accept(ASTVisitor &V) override { V.visitArrayAssign(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<ArrayAssignExprAST>(
        Loc, Name, CloneExprAST(Index.get()), CloneExprAST(RHS.get()));
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
  void accept(ASTVisitor &V) override { V.visitArrayLength(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<ArrayLengthExprAST>(Loc, Name);
  }
};

// list method call: nums.push(val)  /  nums.pop()  /  nums.length
// Note: list<T> *declaration* is now a plain VarDeclExprAST with a
// GenericType("list", [elemType]) — no separate ListDeclExprAST needed.
class MethodCallExprAST : public ExprAST {
  std::unique_ptr<ExprAST> Base;
  std::string Method;
  std::vector<std::unique_ptr<ExprAST>> Args;
  SourceLocation Loc;

public:
  MethodCallExprAST(SourceLocation Loc, std::unique_ptr<ExprAST> Base,
                    std::string Method,
                    std::vector<std::unique_ptr<ExprAST>> Args = {})
      : Base(std::move(Base)), Method(std::move(Method)), Args(std::move(Args)),
        Loc(Loc) {}

  const SourceLocation &getLoc() const { return Loc; }
  const ExprAST *getBase() const { return Base.get(); }
  const std::string &getMethod() const { return Method; }
  const std::vector<std::unique_ptr<ExprAST>> &getArgs() const { return Args; }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitMethodCall(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<MethodCallExprAST>(Loc, CloneExprAST(Base.get()),
                                               Method, CloneExprList(Args));
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

// Collect every node ID reachable from a tree. Used as a fallback when the
// baseline AST is missing so incremental validation still sees the whole tree.
inline void CollectAllNodeIDsInternal(const ExprAST *Node,
                                      std::set<uint64_t> &NodeIDs) {
  if (!Node)
    return;

  NodeIDs.insert(Node->getNodeID());

  if (const auto *Binary = dynamic_cast<const BinaryExprAST *>(Node)) {
    CollectAllNodeIDsInternal(Binary->getLHS(), NodeIDs);
    CollectAllNodeIDsInternal(Binary->getRHS(), NodeIDs);
    return;
  }

  if (const auto *Unary = dynamic_cast<const UnaryExprAST *>(Node)) {
    CollectAllNodeIDsInternal(Unary->getOperand(), NodeIDs);
    return;
  }

  if (const auto *Assign = dynamic_cast<const AssignmentExprAST *>(Node)) {
    CollectAllNodeIDsInternal(Assign->getLHS(), NodeIDs);
    CollectAllNodeIDsInternal(Assign->getRHS(), NodeIDs);
    return;
  }

  if (const auto *Decl = dynamic_cast<const VarDeclExprAST *>(Node)) {
    CollectAllNodeIDsInternal(Decl->getInitializer(), NodeIDs);
    return;
  }

  if (const auto *Cast = dynamic_cast<const CastExprAST *>(Node)) {
    CollectAllNodeIDsInternal(Cast->getOperand(), NodeIDs);
    return;
  }

  if (const auto *BlockNode = dynamic_cast<const BlockExprAST *>(Node)) {
    for (const auto &Expr : BlockNode->getExpressions())
      CollectAllNodeIDsInternal(Expr.get(), NodeIDs);
    return;
  }

  if (const auto *ModuleImportNode =
          dynamic_cast<const ModuleImportExprAST *>(Node)) {
    CollectAllNodeIDsInternal(ModuleImportNode->getBody(), NodeIDs);
    return;
  }

  if (const auto *IfNode = dynamic_cast<const IfExprAST *>(Node)) {
    for (const auto &Branch : IfNode->getBranches()) {
      CollectAllNodeIDsInternal(Branch.Cond.get(), NodeIDs);
      CollectAllNodeIDsInternal(Branch.Body.get(), NodeIDs);
    }
    CollectAllNodeIDsInternal(IfNode->getElseBody(), NodeIDs);
    return;
  }

  if (const auto *While = dynamic_cast<const WhileExprAST *>(Node)) {
    CollectAllNodeIDsInternal(While->getCond(), NodeIDs);
    CollectAllNodeIDsInternal(While->getBody(), NodeIDs);
    return;
  }

  if (const auto *For = dynamic_cast<const ForExprAST *>(Node)) {
    CollectAllNodeIDsInternal(For->getStart(), NodeIDs);
    CollectAllNodeIDsInternal(For->getEnd(), NodeIDs);
    CollectAllNodeIDsInternal(For->getStep(), NodeIDs);
    CollectAllNodeIDsInternal(For->getBody(), NodeIDs);
    return;
  }

  if (const auto *FuncCall = dynamic_cast<const FuncCallExprAST *>(Node)) {
    CollectAllNodeIDsInternal(FuncCall->getCallee(), NodeIDs);
    for (const auto &Arg : FuncCall->getArgs())
      CollectAllNodeIDsInternal(Arg.get(), NodeIDs);
    return;
  }

  if (const auto *FuncDef = dynamic_cast<const FuncDefExprAST *>(Node)) {
    CollectAllNodeIDsInternal(FuncDef->getBody(), NodeIDs);
    return;
  }

  if (const auto *ReturnNode = dynamic_cast<const ReturnExprAST *>(Node)) {
    CollectAllNodeIDsInternal(ReturnNode->getVal(), NodeIDs);
    return;
  }

  if (const auto *TryNode = dynamic_cast<const TryExprAST *>(Node)) {
    CollectAllNodeIDsInternal(TryNode->getTryBlock(), NodeIDs);
    CollectAllNodeIDsInternal(TryNode->getCatchBlock(), NodeIDs);
    return;
  }

  if (const auto *RaiseNode = dynamic_cast<const RaiseExprAST *>(Node)) {
    CollectAllNodeIDsInternal(RaiseNode->getPayload(), NodeIDs);
    return;
  }

  if (const auto *ArrayDecl = dynamic_cast<const ArrayDeclExprAST *>(Node)) {
    for (const auto &Init : ArrayDecl->getInitList())
      CollectAllNodeIDsInternal(Init.get(), NodeIDs);
    return;
  }

  if (const auto *ArrayAccess =
          dynamic_cast<const ArrayAccessExprAST *>(Node)) {
    CollectAllNodeIDsInternal(ArrayAccess->getBase(), NodeIDs);
    CollectAllNodeIDsInternal(ArrayAccess->getIndex(), NodeIDs);
    return;
  }

  if (const auto *ArrayAssign =
          dynamic_cast<const ArrayAssignExprAST *>(Node)) {
    CollectAllNodeIDsInternal(ArrayAssign->getIndex(), NodeIDs);
    CollectAllNodeIDsInternal(ArrayAssign->getRHS(), NodeIDs);
    return;
  }

  if (const auto *MethodCall = dynamic_cast<const MethodCallExprAST *>(Node)) {
    CollectAllNodeIDsInternal(MethodCall->getBase(), NodeIDs);
    for (const auto &A : MethodCall->getArgs()) {
      if (A)
        CollectAllNodeIDsInternal(A.get(), NodeIDs);
    }
    return;
  }

  if (const auto *StructDecl = dynamic_cast<const StructDeclExprAST *>(Node)) {
    return; // No sub-expressions
  }

  if (const auto *StructInit = dynamic_cast<const StructInitExprAST *>(Node)) {
    for (const auto &Arg : StructInit->getArgs())
      CollectAllNodeIDsInternal(Arg.get(), NodeIDs);
    return;
  }

  if (const auto *FieldAccess =
          dynamic_cast<const FieldAccessExprAST *>(Node)) {
    CollectAllNodeIDsInternal(FieldAccess->getBase(), NodeIDs);
    return;
  }

  if (dynamic_cast<const ArrayLengthExprAST *>(Node) ||
      dynamic_cast<const BreakExprAST *>(Node) ||
      dynamic_cast<const ContinueExprAST *>(Node) ||
      dynamic_cast<const NumberExprAST *>(Node) ||
      dynamic_cast<const BoolExprAST *>(Node) ||
      dynamic_cast<const StringExprAST *>(Node) ||
      dynamic_cast<const VariableExprAST *>(Node) ||
      dynamic_cast<const SizeOfExprAST *>(Node)) {
    return;
  }
}

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

  // NumberExprAST: Compare numeric values
  if (auto *OrigNum = dynamic_cast<const NumberExprAST *>(OriginalAST)) {
    auto *ModNum = dynamic_cast<const NumberExprAST *>(ModifiedAST);
    if (OrigNum->isInteger() != ModNum->isInteger() ||
        (OrigNum->isInteger() && OrigNum->getIntVal() != ModNum->getIntVal()) ||
        (!OrigNum->isInteger() &&
         OrigNum->getDoubleVal() != ModNum->getDoubleVal())) {
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

  // BinaryExprAST: Compare operator and recursively diff children
  if (auto *OrigBin = dynamic_cast<const BinaryExprAST *>(OriginalAST)) {
    auto *ModBin = dynamic_cast<const BinaryExprAST *>(ModifiedAST);
    bool nodeChanged = false;

    if (OrigBin->getOp() != ModBin->getOp()) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
      nodeChanged = true;
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
        OrigDecl->getType() != ModDecl->getType()) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    }
    // The initializer might be different
    // Can't directly access it, so traverse as children
    // This is handled implicitly by the type check above
    return ChangedIDs;
  }

  // CastExprAST: Compare destination type and recursively diff operand
  if (auto *OrigCast = dynamic_cast<const CastExprAST *>(OriginalAST)) {
    auto *ModCast = dynamic_cast<const CastExprAST *>(ModifiedAST);
    if (OrigCast->getDestType() != ModCast->getDestType()) {
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
    if (OrigFunc->getName() != ModFunc->getName() ||
        OrigFunc->getReturnType() != ModFunc->getReturnType()) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    }
    // Note: Parameter comparison would require getParams() accessor
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

  // TryExprAST: Recursively diff try/catch blocks
  if (auto *OrigTry = dynamic_cast<const TryExprAST *>(OriginalAST)) {
    auto *ModTry = dynamic_cast<const TryExprAST *>(ModifiedAST);
    if (OrigTry->getPayloadName() != ModTry->getPayloadName()) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    }
    CollectChangedNodeIDsInternal(OrigTry->getTryBlock(), ModTry->getTryBlock(),
                                  ChangedIDs);
    CollectChangedNodeIDsInternal(OrigTry->getCatchBlock(),
                                  ModTry->getCatchBlock(), ChangedIDs);
    return ChangedIDs;
  }

  // RaiseExprAST: Recursively diff payload
  if (auto *OrigRaise = dynamic_cast<const RaiseExprAST *>(OriginalAST)) {
    auto *ModRaise = dynamic_cast<const RaiseExprAST *>(ModifiedAST);
    CollectChangedNodeIDsInternal(OrigRaise->getPayload(),
                                  ModRaise->getPayload(), ChangedIDs);
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
        CollectChangedNodeIDsInternal(OrigInit[i].get(), ModInit[i].get(),
                                      ChangedIDs);
      }
    }
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

  // ArrayAssignExprAST: Compare name and recursively diff index and RHS
  if (auto *OrigAssign =
          dynamic_cast<const ArrayAssignExprAST *>(OriginalAST)) {
    auto *ModAssign = dynamic_cast<const ArrayAssignExprAST *>(ModifiedAST);
    if (OrigAssign->getName() != ModAssign->getName()) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    }
    CollectChangedNodeIDsInternal(OrigAssign->getIndex(), ModAssign->getIndex(),
                                  ChangedIDs);
    CollectChangedNodeIDsInternal(OrigAssign->getRHS(), ModAssign->getRHS(),
                                  ChangedIDs);
    return ChangedIDs;
  }

  // ArrayLengthExprAST: Compare name
  if (auto *OrigLen = dynamic_cast<const ArrayLengthExprAST *>(OriginalAST)) {
    auto *ModLen = dynamic_cast<const ArrayLengthExprAST *>(ModifiedAST);
    if (OrigLen->getName() != ModLen->getName()) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    }
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
