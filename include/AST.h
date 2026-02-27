#ifndef AST_H
#define AST_H

#include "Lexer.h"
#include "Types.h"
#include "llvm/IR/Value.h"
#include <memory>

// Base Expressions Class: Everything, "5", "5 + 10", etc. are expressions
class ExprAST {
public:
  virtual ~ExprAST() = default;

  virtual llvm::Value *codegen() = 0;
};

// Number Node (Leaf)
class NumberExprAST : public ExprAST {
  double Val;         // Actual value, like "5", "5.0", etc.
  long long IntVal;   // Exact integer value when IsInteger is true
  bool IsInteger = false;

public:
  explicit NumberExprAST(double Val) : Val(Val), IntVal(0), IsInteger(false) {}
  explicit NumberExprAST(long long Val)
      : Val(static_cast<double>(Val)), IntVal(Val), IsInteger(true) {}

  bool isInteger() const { return IsInteger; }
  long long getIntVal() const { return IntVal; }
  double getDoubleVal() const { return Val; }

  llvm::Value *codegen() override;
};

// Binary Operation Node (Branch): Holds the Operator ('+'), the Left side (A),
// and the Right side (B).
class BinaryExprAST : public ExprAST {
  int Op; // The operator, like '+', '-', etc.
  std::unique_ptr<ExprAST> LHS;
  std::unique_ptr<ExprAST> RHS;

public:
  BinaryExprAST(char Op, std::unique_ptr<ExprAST> LHS,
                std::unique_ptr<ExprAST> RHS)
      : Op(Op), LHS(std::move(LHS)), RHS(std::move(RHS)) {}

  llvm::Value *codegen() override;
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

  llvm::Value *codegen() override;
};

// Variable Node, represents variable name like "x", "y", etc.
class VariableExprAST : public ExprAST {
  std::string Name;
  SourceLocation Loc;

public:
  VariableExprAST(SourceLocation Loc, std::string Name)
      : Loc(Loc), Name(Name) {}

  llvm::Value *codegen() override;
};

// If-Expr AST, represents if-else branch
class IfExprAST : public ExprAST {
  std::unique_ptr<ExprAST> Cond, Then, Else;

public:
  IfExprAST(std::unique_ptr<ExprAST> Cond, std::unique_ptr<ExprAST> Then,
            std::unique_ptr<ExprAST> Else)
      : Cond(std::move(Cond)), Then(std::move(Then)), Else(std::move(Else)) {}

  llvm::Value *codegen() override;
};

class UnaryExprAST : public ExprAST {
  int Opcode;
  std::unique_ptr<ExprAST> Operand;

public:
  UnaryExprAST(char Opcode, std::unique_ptr<ExprAST> Operand)
      : Opcode(Opcode), Operand(std::move(Operand)) {}

  llvm::Value *codegen() override;
};

// Block Expression, represents { expr1; expr2; ... }
class BlockExprAST : public ExprAST {
  std::vector<std::unique_ptr<ExprAST>> Expressions;

public:
  BlockExprAST(std::vector<std::unique_ptr<ExprAST>> Expressions)
      : Expressions(std::move(Expressions)) {}

  llvm::Value *codegen() override;
};

// Generic node for any builtin function call, e.g. print(x).
class BuiltinCallExprAST : public ExprAST {
  std::string Name;
  std::vector<std::unique_ptr<ExprAST>> Args;

public:
  BuiltinCallExprAST(std::string Name,
                     std::vector<std::unique_ptr<ExprAST>> Args)
      : Name(std::move(Name)), Args(std::move(Args)) {}

  llvm::Value *codegen() override;
};


class WhileExprAST : public ExprAST {
  std::unique_ptr<ExprAST> Cond, Body;

public:
  WhileExprAST(std::unique_ptr<ExprAST> Cond, std::unique_ptr<ExprAST> Body)
      : Cond(std::move(Cond)), Body(std::move(Body)) {}

  llvm::Value *codegen() override;
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
};

class BoolExprAST : public ExprAST {
  bool Val;

public:
  BoolExprAST(bool Val) : Val(Val) {}
  llvm::Value *codegen() override;
};

class StringExprAST : public ExprAST {
  std::string Val;

public:
  StringExprAST(std::string Val) : Val(std::move(Val)) {}
  const std::string &getValue() const { return Val; }
  llvm::Value *codegen() override;
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
};

#endif
