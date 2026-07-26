#ifndef AST_H
#define AST_H

#include "ASTVisitor.h"
#include "Lexer.h"
#include "ResolverSymbolTable.h" // VariableSymbol — LLVM-free, safe to include here
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

// Compound Assignment Node, represents things like "x += 5"
class CompoundAssignmentExprAST : public ExprAST {
  SourceLocation Loc;
  int Opcode;
  std::unique_ptr<ExprAST> LHS;
  std::unique_ptr<ExprAST> RHS;

public:
  CompoundAssignmentExprAST(SourceLocation Loc, int Opcode,
                            std::unique_ptr<ExprAST> LHS,
                            std::unique_ptr<ExprAST> RHS)
      : Loc(Loc), Opcode(Opcode), LHS(std::move(LHS)), RHS(std::move(RHS)) {}

  const SourceLocation &getLoc() const { return Loc; }
  int getOpcode() const { return Opcode; }
  const ExprAST *getLHS() const { return LHS.get(); }
  const ExprAST *getRHS() const { return RHS.get(); }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitCompoundAssignment(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<CompoundAssignmentExprAST>(
        Loc, Opcode, CloneExprAST(LHS.get()), CloneExprAST(RHS.get()));
  }
};

// Postfix Node, represents things like "x++" or "x--"
class PostfixExprAST : public ExprAST {
  SourceLocation Loc;
  int Opcode;
  std::unique_ptr<ExprAST> Operand;

public:
  PostfixExprAST(SourceLocation Loc, int Opcode,
                 std::unique_ptr<ExprAST> Operand)
      : Loc(Loc), Opcode(Opcode), Operand(std::move(Operand)) {}

  const SourceLocation &getLoc() const { return Loc; }
  int getOpcode() const { return Opcode; }
  const ExprAST *getOperand() const { return Operand.get(); }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitPostfixExpr(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<PostfixExprAST>(Loc, Opcode,
                                            CloneExprAST(Operand.get()));
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

  /// Populated by the Resolver after a successful lookupVariable().
  /// clone() returns a fresh node, so this is nullptr in cloned trees —
  /// cloned ASTs must be re-resolved before TypeChecking/Codegen.
  std::shared_ptr<VariableSymbol> ResolvedSymbol;
  const VariableSymbol *getResolvedSymbol() const { return ResolvedSymbol.get(); }

  /// Populated by the Resolver when this node is the base of a module-qualified
  /// method call (e.g. `io` in `io.printline(...)`).  Empty for ordinary
  /// variables.  NOT copied by clone() — default std::string is "". ✓
  std::string ResolvedModuleName;
  bool IsStructName = false;
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<VariableExprAST>(Loc, Name);
    // Note: ResolvedSymbol is intentionally NOT copied — cloned ASTs are stale
    // until re-resolved. The default-constructed field is nullptr. ✓
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
  SourceLocation Loc;
  int Opcode;
  std::unique_ptr<ExprAST> Operand;

public:
  // Before this field existed, UnaryExprAST carried no source location at
  // all, and TypeChecker::visitUnary()'s type-mismatch diagnostics for '-'
  // and '~' had no location to report at all -- they hardcoded {0, 0}
  // directly, producing "error at line 0, col 0" with no code snippet
  // shown. Every construction site (Parser.cpp) now passes the operator
  // token's real location explicitly.
  UnaryExprAST(SourceLocation Loc, char Opcode, std::unique_ptr<ExprAST> Operand)
      : Loc(Loc), Opcode(Opcode), Operand(std::move(Operand)) {}

  const SourceLocation &getLoc() const { return Loc; }
  int getOpcode() const { return Opcode; }
  const ExprAST *getOperand() const { return Operand.get(); }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitUnary(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<UnaryExprAST>(Loc, static_cast<char>(Opcode),
                                          CloneExprAST(Operand.get()));
  }
}; // UnaryExprAST

// Address-of expression: &expr — evaluates to a pointer to the operand's
// storage location. The operand must be an lvalue (named variable or field).
class AddressOfExprAST : public ExprAST {
  SourceLocation Loc;
  std::unique_ptr<ExprAST> Operand;

public:
  AddressOfExprAST(SourceLocation Loc, std::unique_ptr<ExprAST> Operand)
      : Loc(Loc), Operand(std::move(Operand)) {}

  SourceLocation getLoc() const { return Loc; }
  const ExprAST *getOperand() const { return Operand.get(); }

  llvm::Value *codegen() override;
  // &expr is never itself an lvalue.
  llvm::Value *codegenLValue() override { return nullptr; }
  // Dispatch to the dedicated visitAddressOf hook so the TypeChecker and
  // Resolver can handle &expr as distinct from plain unary negation.
  void accept(ASTVisitor &V) override { V.visitAddressOf(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<AddressOfExprAST>(Loc, CloneExprAST(Operand.get()));
  }
};


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

// Which of the four `import` forms this statement used. Bare is the
// original, only-ever-supported form (`import io;`) -- qualified-access
// only, nothing pasted flat. The other three are on-demand: they each pull
// SPECIFIC symbols into the importing file's own flat (unqualified)
// namespace, instead of the old blanket "importing a module makes
// everything in it bare-visible" behavior.
enum class ImportMode {
  Bare,       // import io;
  Symbol,     // import io::printline;
  SymbolList, // import io::[print, input, printline];
  Wildcard,   // import io::[*];
};

// One requested name in a Symbol/SymbolList/Wildcard import. Carries its
// own SourceLocation (not just the enclosing import statement's) so
// collision diagnostics can point at exactly which name in a list caused
// the problem, and so a future LSP can offer go-to-definition on the
// individual symbol name, not just the import line as a whole.
struct ImportedSymbolRef {
  std::string Name;
  SourceLocation Loc;
};

class ModuleImportExprAST : public ExprAST {
  SourceLocation Loc;         ///< location of the `import` statement itself
  std::string ModuleName;
  std::string ResolvedPath;   ///< canonical path the module was actually loaded
                              ///< from (one of ParseImport()'s 3 search
                              ///< candidates) -- the identity key circular-
                              ///< import detection and per-file import
                              ///< scoping both key off of, so neither has to
                              ///< re-derive "what file is this" separately.
  std::unique_ptr<ExprAST> Body;
  ImportMode Mode = ImportMode::Bare;
  std::vector<ImportedSymbolRef> RequestedSymbols; // empty for Bare/Wildcard

public:
  ModuleImportExprAST(SourceLocation Loc, std::string ModuleName,
                      std::string ResolvedPath, std::unique_ptr<ExprAST> Body,
                      ImportMode Mode = ImportMode::Bare,
                      std::vector<ImportedSymbolRef> RequestedSymbols = {})
      : Loc(Loc), ModuleName(std::move(ModuleName)),
        ResolvedPath(std::move(ResolvedPath)), Body(std::move(Body)),
        Mode(Mode), RequestedSymbols(std::move(RequestedSymbols)) {}

  const SourceLocation &getLoc() const { return Loc; }
  const std::string &getModuleName() const { return ModuleName; }
  const std::string &getResolvedPath() const { return ResolvedPath; }
  const ExprAST *getBody() const { return Body.get(); }
  ImportMode getMode() const { return Mode; }
  const std::vector<ImportedSymbolRef> &getRequestedSymbols() const {
    return RequestedSymbols;
  }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitModuleImport(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<ModuleImportExprAST>(Loc, ModuleName, ResolvedPath,
                                                 CloneExprAST(Body.get()),
                                                 Mode, RequestedSymbols);
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

  /// Populated by the Resolver after successfully declaring the induction variable.
  /// Nulled by clone() semantics (fresh node, default nullptr). Cloned ASTs must be re-resolved.
  std::shared_ptr<VariableSymbol> ResolvedLoopVar;
  const VariableSymbol *getResolvedLoopVar() const { return ResolvedLoopVar.get(); }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitFor(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<ForExprAST>(
        Loc, VarName, CloneExprAST(Start.get()), CloneExprAST(End.get()),
        CloneExprAST(Step.get()), CloneExprAST(Body.get()));
    // Note: ResolvedLoopVar intentionally NOT copied. ✓
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

// ── Compare / Matches / Done ─────────────────────────────────────────────────
// A single arm in a compare statement.
// 'Values' holds all the stacked patterns:
//   matches (1):
//   matches (2):
//     body
//   done
// → one MatchesArm with Values = {1, 2} and the shared body.
struct MatchesArm {
  SourceLocation Loc;                           ///< location of the first 'matches'
  std::vector<std::unique_ptr<ExprAST>> Values; ///< one entry per stacked matches
  std::unique_ptr<ExprAST> Body;                ///< shared arm body (BlockExprAST)
};

/// compare (subject) { matches val: ... done  otherwise: ... }
class CompareExprAST : public ExprAST {
  SourceLocation Loc;
  std::unique_ptr<ExprAST> Subject;
  std::vector<MatchesArm> Arms;
  std::unique_ptr<ExprAST> OtherBody;  ///< nullptr if no 'otherwise'
  SourceLocation OtherLoc;

public:
  /// Set by TypeChecker when the subject is a string — selects strcmp chain codegen.
  bool SubjectIsString = false;
  bool SubjectIsEnum   = false;

  CompareExprAST(SourceLocation Loc, std::unique_ptr<ExprAST> Subject,
                 std::vector<MatchesArm> Arms,
                 std::unique_ptr<ExprAST> OtherBody,
                 SourceLocation OtherLoc)
      : Loc(Loc), Subject(std::move(Subject)), Arms(std::move(Arms)),
        OtherBody(std::move(OtherBody)), OtherLoc(OtherLoc) {}

  const SourceLocation &getLoc()       const { return Loc; }
  const SourceLocation &getOtherLoc()  const { return OtherLoc; }
  const ExprAST        *getSubject()   const { return Subject.get(); }
        ExprAST        *getSubjectMut()      { return Subject.get(); }
  const std::vector<MatchesArm> &getArms() const { return Arms; }
        std::vector<MatchesArm> &getArmsMut()    { return Arms; }
  const ExprAST *getOtherBody()        const { return OtherBody.get(); }
        ExprAST *getOtherBodyMut()           { return OtherBody.get(); }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitCompare(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    std::vector<MatchesArm> ClonedArms;
    ClonedArms.reserve(Arms.size());
    for (const auto &Arm : Arms) {
      MatchesArm CA;
      CA.Loc = Arm.Loc;
      for (const auto &V : Arm.Values)
        CA.Values.push_back(CloneExprAST(V.get()));
      CA.Body = CloneExprAST(Arm.Body.get());
      ClonedArms.push_back(std::move(CA));
    }
    return std::make_unique<CompareExprAST>(
        Loc, CloneExprAST(Subject.get()), std::move(ClonedArms),
        CloneExprAST(OtherBody.get()), OtherLoc);
  }
};

class VarDeclExprAST : public ExprAST {
  std::string Name;
  TurfTypePtr Type;
  std::unique_ptr<ExprAST> InitVal;
  SourceLocation Loc;
  bool IsImmutable;
  bool HasExplicitInit = true;

public:
  VarDeclExprAST(SourceLocation Loc, std::string Name, TurfTypePtr Type,
                 std::unique_ptr<ExprAST> InitVal, bool IsImmutable, bool HasExplicitInit = true)
      : Name(std::move(Name)), Type(std::move(Type)),
        InitVal(std::move(InitVal)), Loc(Loc), IsImmutable(IsImmutable),
        HasExplicitInit(HasExplicitInit) {}

  const SourceLocation &getLoc() const { return Loc; }
  const std::string &getName() const { return Name; }
  const TurfType *getType() const { return Type.get(); }
  TurfTypePtr getTypePtr() const { return Type; }
  void setTypePtr(TurfTypePtr NewType) { Type = std::move(NewType); }
  const ExprAST *getInitializer() const { return InitVal.get(); }
  bool isImmutable() const { return IsImmutable; }
  bool hasExplicitInit() const { return HasExplicitInit; }

  /// Populated by the Resolver after a successful declareVariable().
  /// Nulled by clone() semantics (fresh node, default nullptr). Cloned ASTs must be re-resolved.
  std::shared_ptr<VariableSymbol> ResolvedSymbol;
  const VariableSymbol *getResolvedSymbol() const { return ResolvedSymbol.get(); }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitVarDecl(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<VarDeclExprAST>(Loc, Name, Type,
                                            CloneExprAST(InitVal.get()), IsImmutable, HasExplicitInit);
    // Note: ResolvedSymbol intentionally NOT copied. ✓
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

class DefaultInitExprAST : public ExprAST {
  SourceLocation Loc;
  TurfTypePtr Type;

public:
  DefaultInitExprAST(SourceLocation Loc, TurfTypePtr Type)
      : Loc(Loc), Type(std::move(Type)) {}

  const SourceLocation &getLoc() const { return Loc; }
  TurfTypePtr getTypePtr() const { return Type; }
  void setTypePtr(TurfTypePtr NewType) { Type = std::move(NewType); }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitDefaultInit(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<DefaultInitExprAST>(Loc, Type);
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
  // Location of the parameter's NAME token. Defaults to {0,0} (via
  // SourceLocation's own default member values) for the handful of
  // synthetic ParamDecls the Parser builds without a real source position
  // (e.g. the implicit 'self' parameter) -- everywhere a parameter is
  // parsed from real user source, this is now populated, fixing
  // Resolver::visitFuncDef()'s duplicate-parameter-name diagnostic, which
  // used to hardcode {0,0} here and always report "declared here at line 0,
  // column 0" for the FIRST occurrence, regardless of where it actually was.
  SourceLocation Loc;
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


// fn RetType Name<T>(params...) { body }
class FuncDefExprAST : public ExprAST {
  std::string Name;
  TurfTypePtr ReturnType;
  std::vector<ParamDecl> Params;
  std::unique_ptr<ExprAST> Body;
  std::vector<std::string> TypeParams;
  SourceLocation Loc;

public:
  FuncDefExprAST(SourceLocation Loc, std::string Name, TurfTypePtr ReturnType,
                 std::vector<ParamDecl> Params, std::unique_ptr<ExprAST> Body,
                 std::vector<std::string> TypeParams = {})
      : Loc(Loc), Name(std::move(Name)), ReturnType(std::move(ReturnType)),
        Params(std::move(Params)), Body(std::move(Body)),
        TypeParams(std::move(TypeParams)) {}

  const std::string &getName() const { return Name; }
  const SourceLocation &getLoc() const { return Loc; }
  const TurfType *getReturnType() const { return ReturnType.get(); }
  TurfTypePtr getReturnTypePtr() const { return ReturnType; }
  ExprAST *getBodyMut() const { return Body.get(); }
  const ExprAST *getBody() const { return Body.get(); }
  const std::vector<ParamDecl> &getParams() const { return Params; }
  /// Mutable access used only by Resolver::visitFuncDef() to rewrite a
  /// parameter's NamedType("T") into a real TurfGenericParamType("T") when
  /// "T" names one of this function's own TypeParams -- ParseType() has no
  /// way to know that at parse time (see substituteTypeParams()'s doc
  /// comment in Resolver.cpp). Codegen and TypeChecker read Params directly
  /// from this same shared AST node, so mutating it here is what makes the
  /// substitution visible to every later pass without each one having to
  /// redo the same TypeParams lookup independently.
  std::vector<ParamDecl> &getParamsMut() { return Params; }
  const std::vector<std::string> &getTypeParams() const { return TypeParams; }
  /// Mutable return-type setter, used alongside getParamsMut() for the same
  /// generic-parameter substitution.
  void setReturnType(TurfTypePtr T) { ReturnType = std::move(T); }

  /// Populated by the Resolver after registering the function symbol.
  /// clone() returns a fresh node, so this is nullptr in cloned trees.
  std::shared_ptr<ResolverFunctionSymbol> ResolvedDef;
  const ResolverFunctionSymbol *getResolvedDef() const {
    return ResolvedDef.get();
  }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitFuncDef(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<FuncDefExprAST>(
        Loc, Name, ReturnType, Params, CloneExprAST(Body.get()), TypeParams);
    // Note: ResolvedDef is intentionally NOT copied — cloned ASTs are stale
    // until re-resolved. The default-constructed field is nullptr.
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

  /// All overloads matching the call name.  TypeChecker narrows this to one.
  /// clone() returns a fresh node, so this is empty in cloned trees.
  std::vector<std::shared_ptr<ResolverFunctionSymbol>> ResolvedCandidates;
  const std::vector<std::shared_ptr<ResolverFunctionSymbol>> &
  getResolvedCandidates() const {
    return ResolvedCandidates;
  }

  /// The single overload selected by TypeChecker::visitFuncCall() after
  /// narrowing ResolvedCandidates to one match (arity + type compatible).
  /// For bare intrinsics (typeof, lengthof, …) without a ResolvedCandidates
  /// entry this field is left null — the TypeChecker still sets the node's
  /// type directly.
  /// clone() returns a fresh node, so this is null in cloned trees — same
  /// clone-semantics contract as ResolvedSymbol. ✓
  std::shared_ptr<ResolverFunctionSymbol> ResolvedCallTarget;
  const ResolverFunctionSymbol *getResolvedCallTarget() const {
    return ResolvedCallTarget.get();
  }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitFuncCall(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<FuncCallExprAST>(Loc, CloneExprAST(Callee.get()),
                                             CloneExprList(Args));
    // Note: ResolvedCandidates and ResolvedCallTarget are intentionally NOT
    // copied — cloned ASTs are stale until re-resolved. ✓
  }
};

// extern fn Name(param: type, ...): ReturnType;
// Declares a C external function visible to this compilation unit.
// No function body is generated — only an LLVM ExternalLinkage prototype.
class ExternFuncDeclExprAST : public ExprAST {
  std::string Name_;
  std::vector<std::pair<std::string, TurfTypePtr>> Params_;
  TurfTypePtr ReturnType_;
  SourceLocation Loc_;
  bool IsVarArg_;        ///< true when the declaration ends with '...'
  std::string LibName_;  ///< non-empty when 'extern from "lib" fn' syntax used

public:
  ExternFuncDeclExprAST(
      SourceLocation Loc, std::string Name,
      std::vector<std::pair<std::string, TurfTypePtr>> Params,
      TurfTypePtr ReturnType,
      bool IsVarArg = false,
      std::string LibName = "")
      : Name_(std::move(Name)), Params_(std::move(Params)),
        ReturnType_(std::move(ReturnType)), Loc_(Loc),
        IsVarArg_(IsVarArg), LibName_(std::move(LibName)) {}

  const std::string &getName()     const { return Name_; }
  const auto        &getParams()   const { return Params_; }
  TurfTypePtr        getReturnType()const { return ReturnType_; }
  const SourceLocation &getLoc()   const { return Loc_; }
  bool               isVarArg()    const { return IsVarArg_; }
  const std::string &getLibName()  const { return LibName_; }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitExternFuncDecl(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<ExternFuncDeclExprAST>(Loc_, Name_, Params_,
                                                   ReturnType_, IsVarArg_,
                                                   LibName_);
  }
};

class EnumDeclExprAST : public ExprAST {
    std::string              Name;
    std::vector<std::string> Variants;
    TurfTypePtr              UnderlyingType;  // nullptr = TurfInt32 default
    SourceLocation           Loc;
public:
    EnumDeclExprAST(SourceLocation Loc, std::string Name,
                    std::vector<std::string> Variants,
                    TurfTypePtr UnderlyingType = nullptr)
        : Name(std::move(Name)), Variants(std::move(Variants)),
          UnderlyingType(std::move(UnderlyingType)), Loc(Loc) {}
    const std::string&              getName()     const { return Name; }
    const std::vector<std::string>& getVariants() const { return Variants; }
    const SourceLocation&           getLoc()      const { return Loc; }
    TurfTypePtr                     getUnderlyingType() const { return UnderlyingType; }
    llvm::Value* codegen() override;
    void accept(ASTVisitor& V) override { V.visitEnumDecl(*this); }
    std::unique_ptr<ExprAST> clone() const override {
        return std::make_unique<EnumDeclExprAST>(Loc, Name, Variants, UnderlyingType);
    }
};

// What a `Left::Right` scope-access expression turns out to mean, decided
// by the Resolver (the parser stays fully agnostic -- see ScopeAccessExprAST
// below). Unresolved is the state every node starts in; a fresh clone()
// resets back to it since a cloned tree is stale until re-resolved, same
// convention as FuncDefExprAST::ResolvedDef.
enum class ScopeAccessKind {
    Unresolved,
    EnumVariant,      // Color::Red
    ModuleFunctionRef, // io::printline (as a call target)
    ModuleTypeRef,     // list::List<int> (in type position)
    // StaticMethodRef is deliberately not a case yet -- true static struct
    // methods (StructName::method(...)) are a known, separate, deferred
    // language feature (see the crash-fix + "no true static methods"
    // finding this was built on top of). This enum's shape reserves the
    // natural slot for it without disturbing anything else once that
    // lands.
};

// `Left::Right` -- the general namespace-resolution operator. Originally
// only ever meant an enum-variant reference (EnumName::Variant); now also
// covers module-qualified function/type access (io::printline,
// list::List<int>). Renamed from EnumAccessExprAST when this was
// generalized: the class is otherwise unchanged in shape (still exactly
// two names + a location), because the parser was ALREADY fully agnostic
// about what `Identifier '::' Identifier` means -- it just records the two
// names and lets the Resolver decide, so widening what the Resolver is
// willing to decide is all a new kind of `::` usage needs.
class ScopeAccessExprAST : public ExprAST {
    std::string    LeftName;
    std::string    RightName;
    SourceLocation LeftLoc;
    SourceLocation RightLoc;
    SourceLocation Loc;
public:
    // Set by the Resolver (visitScopeAccess / visitFuncCall for the call
    // form) once it decides what this node actually refers to. A future
    // LSP driving the same Resolver can read this post-resolution to know
    // what kind of hover/go-to-definition target this is.
    ScopeAccessKind ResolvedKind = ScopeAccessKind::Unresolved;

    ScopeAccessExprAST(SourceLocation Loc, std::string LeftName,
                       SourceLocation LeftLoc, std::string RightName,
                       SourceLocation RightLoc)
        : LeftName(std::move(LeftName)), RightName(std::move(RightName)),
          LeftLoc(LeftLoc), RightLoc(RightLoc), Loc(Loc) {}
    // Convenience overload for call sites that don't need distinct
    // left/right locations (both default to the whole expression's Loc).
    ScopeAccessExprAST(SourceLocation Loc, std::string LeftName, std::string RightName)
        : ScopeAccessExprAST(Loc, std::move(LeftName), Loc, std::move(RightName), Loc) {}

    const std::string& getLeftName()  const { return LeftName; }
    const std::string& getRightName() const { return RightName; }
    const SourceLocation& getLeftLoc()  const { return LeftLoc; }
    const SourceLocation& getRightLoc() const { return RightLoc; }
    const SourceLocation& getLoc()   const { return Loc; }
    // Pre-existing call sites written against the enum-only meaning of this
    // node (TypeChecker's compare/matches exhaustiveness checks, Codegen's
    // enum-variant codegen) keep working unchanged via these aliases.
    const std::string& getEnumName() const { return LeftName; }
    const std::string& getVariant()  const { return RightName; }
    llvm::Value* codegen() override;
    void accept(ASTVisitor& V) override { V.visitScopeAccess(*this); }
    std::unique_ptr<ExprAST> clone() const override {
        auto Cloned = std::make_unique<ScopeAccessExprAST>(Loc, LeftName, LeftLoc,
                                                            RightName, RightLoc);
        return Cloned;
    }
};


// Array declaration: int[5] arr   or   int[3] arr = [1, 2, 3]
class ArrayDeclExprAST : public ExprAST {
  std::string Name;
  TurfTypePtr ElementType;                        // e.g. TurfInt()
  int Size;                                       // compile-time constant size
  std::vector<std::unique_ptr<ExprAST>> InitList; // optional initializer list
  std::unique_ptr<ExprAST> InitExpr;              // optional general initializer
  bool IsImmutable;
  SourceLocation Loc;

public:
  ArrayDeclExprAST(SourceLocation Loc, std::string Name,
                   TurfTypePtr ElementType, int Size,
                   std::vector<std::unique_ptr<ExprAST>> InitList,
                   std::unique_ptr<ExprAST> InitExpr,
                   bool IsImmutable = false)
      : Name(std::move(Name)), ElementType(std::move(ElementType)), Size(Size),
        InitList(std::move(InitList)), InitExpr(std::move(InitExpr)), IsImmutable(IsImmutable), Loc(Loc) {}

  const SourceLocation &getLoc() const { return Loc; }
  const std::string &getName() const { return Name; }
  const TurfType *getElementType() const { return ElementType.get(); }
  TurfTypePtr getElementTypePtr() const { return ElementType; }
  int getSize() const { return Size; }
  const std::vector<std::unique_ptr<ExprAST>> &getInitList() const {
    return InitList;
  }
  const std::unique_ptr<ExprAST> &getInitExpr() const { return InitExpr; }
  bool isImmutable() const { return IsImmutable; }

  /// Populated by the Resolver after a successful declareVariable().
  /// Nulled by clone() semantics (fresh node, default nullptr). Cloned ASTs must be re-resolved.
  std::shared_ptr<VariableSymbol> ResolvedSymbol;
  const VariableSymbol *getResolvedSymbol() const { return ResolvedSymbol.get(); }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitArrayDecl(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<ArrayDeclExprAST>(Loc, Name, ElementType, Size,
                                              CloneExprList(InitList), 
                                              InitExpr ? InitExpr->clone() : nullptr,
                                              IsImmutable);
    // Note: ResolvedSymbol intentionally NOT copied. ✓
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
  /// Mutable access used only by Resolver::visitStructDecl() to rewrite a
  /// field's NamedType("T") into a real TurfGenericParamType("T") -- see the
  /// identical rationale on FuncDefExprAST::getParamsMut().
  std::vector<StructField> &getFieldsMut() { return Fields; }
  const std::vector<std::string> &getTypeParams() const { return TypeParams; }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitStructDecl(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<StructDeclExprAST>(Loc, Name, Fields, TypeParams);
  }
};

// StructName { field: val, ... }
class StructInitExprAST : public ExprAST {
  std::string Name;
  std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>> NamedFields;
  SourceLocation Loc;
  // The `Module` in `Module::Name { ... }` (empty for the ordinary, bare
  // `Name { ... }` form). Validated by Resolver::visitStructInit via the
  // same validateModuleQualifier() a type annotation's NamedType/
  // GenericType qualifier goes through -- see that function's doc comment.
  std::string ModuleQualifier;

public:
  StructInitExprAST(
      SourceLocation Loc, std::string Name,
      std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>> NamedFields,
      std::string ModuleQualifier = "")
      : Loc(Loc), Name(std::move(Name)), NamedFields(std::move(NamedFields)),
        ModuleQualifier(std::move(ModuleQualifier)) {}

  const SourceLocation &getLoc() const { return Loc; }
  const std::string &getName() const { return Name; }
  const std::string &getModuleQualifier() const { return ModuleQualifier; }
  const std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>> &
  getNamedFields() const {
    return NamedFields;
  }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitStructInit(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>> ClonedFields;
    ClonedFields.reserve(NamedFields.size());
    for (const auto &[FieldName, Val] : NamedFields)
      ClonedFields.emplace_back(FieldName, CloneExprAST(Val.get()));
    return std::make_unique<StructInitExprAST>(Loc, Name,
                                               std::move(ClonedFields),
                                               ModuleQualifier);
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

// Array literal: [expr, expr, ...]
class ArrayLiteralExprAST : public ExprAST {
  std::vector<std::unique_ptr<ExprAST>> Elements;
  SourceLocation Loc;

public:
  TurfTypePtr CachedType;
  ArrayLiteralExprAST(SourceLocation Loc, std::vector<std::unique_ptr<ExprAST>> Elements)
      : Elements(std::move(Elements)), Loc(Loc) {}

  const SourceLocation &getLoc() const { return Loc; }
  const std::vector<std::unique_ptr<ExprAST>> &getElements() const { return Elements; }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitArrayLiteral(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    std::vector<std::unique_ptr<ExprAST>> ClonedElements;
    for (const auto &E : Elements)
      ClonedElements.push_back(CloneExprAST(E.get()));
    auto Cloned = std::make_unique<ArrayLiteralExprAST>(Loc, std::move(ClonedElements));
    Cloned->CachedType = CachedType;
    return Cloned;
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

  /// The single overload selected by TypeChecker::visitMethodCall() after
  /// narrowing module-qualified candidates (e.g. io.printline).
  /// null until TypeChecker runs; also null in cloned trees (not copied). ✓
  std::shared_ptr<ResolverFunctionSymbol> ResolvedCallTarget;
  const ResolverFunctionSymbol *getResolvedCallTarget() const {
    return ResolvedCallTarget.get();
  }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitMethodCall(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<MethodCallExprAST>(Loc, CloneExprAST(Base.get()),
                                               Method, CloneExprList(Args));
    // Note: ResolvedCallTarget intentionally NOT copied — cloned ASTs are
    // stale until re-resolved. ✓
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

  if (const auto *CompoundAssign =
          dynamic_cast<const CompoundAssignmentExprAST *>(Node)) {
    CollectAllNodeIDsInternal(CompoundAssign->getLHS(), NodeIDs);
    CollectAllNodeIDsInternal(CompoundAssign->getRHS(), NodeIDs);
    return;
  }

  if (const auto *Postfix = dynamic_cast<const PostfixExprAST *>(Node)) {
    CollectAllNodeIDsInternal(Postfix->getOperand(), NodeIDs);
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


  if (const auto *ArrayDecl = dynamic_cast<const ArrayDeclExprAST *>(Node)) {
    for (const auto &Init : ArrayDecl->getInitList())
      CollectAllNodeIDsInternal(Init.get(), NodeIDs);
    CollectAllNodeIDsInternal(ArrayDecl->getInitExpr().get(), NodeIDs);
    return;
  }

  if (const auto *ArrayAccess =
          dynamic_cast<const ArrayAccessExprAST *>(Node)) {
    CollectAllNodeIDsInternal(ArrayAccess->getBase(), NodeIDs);
    CollectAllNodeIDsInternal(ArrayAccess->getIndex(), NodeIDs);
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

  if (const auto *ArrayLit = dynamic_cast<const ArrayLiteralExprAST *>(Node)) {
    for (const auto &Elem : ArrayLit->getElements())
      CollectAllNodeIDsInternal(Elem.get(), NodeIDs);
    return;
  }


  if (const auto *StructInit = dynamic_cast<const StructInitExprAST *>(Node)) {
    for (const auto &[FieldName, Val] : StructInit->getNamedFields())
      CollectAllNodeIDsInternal(Val.get(), NodeIDs);
    return;
  }

  if (const auto *FieldAccess =
          dynamic_cast<const FieldAccessExprAST *>(Node)) {
    CollectAllNodeIDsInternal(FieldAccess->getBase(), NodeIDs);
    return;
  }

  if (dynamic_cast<const BreakExprAST *>(Node) ||
      dynamic_cast<const ContinueExprAST *>(Node) ||
      dynamic_cast<const NumberExprAST *>(Node) ||
      dynamic_cast<const BoolExprAST *>(Node) ||
      dynamic_cast<const StringExprAST *>(Node) ||
      dynamic_cast<const VariableExprAST *>(Node) ||
      dynamic_cast<const StructDeclExprAST *>(Node) ||
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
    if (OrigPost->getOpcode() != ModPost->getOpcode()) {
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

  // StructInitExprAST
  if (auto *OrigStructInit = dynamic_cast<const StructInitExprAST *>(OriginalAST)) {
    auto *ModStructInit = dynamic_cast<const StructInitExprAST *>(ModifiedAST);
    if (OrigStructInit->getName() != ModStructInit->getName() ||
        OrigStructInit->getNamedFields().size() != ModStructInit->getNamedFields().size()) {
      ChangedIDs.insert(OriginalAST->getNodeID());
      ChangedIDs.insert(ModifiedAST->getNodeID());
    } else {
      const auto &OrigFields = OrigStructInit->getNamedFields();
      const auto &ModFields = ModStructInit->getNamedFields();
      for (size_t i = 0; i < OrigFields.size(); ++i) {
        if (OrigFields[i].first != ModFields[i].first) {
          ChangedIDs.insert(OriginalAST->getNodeID());
          ChangedIDs.insert(ModifiedAST->getNodeID());
        }
        CollectChangedNodeIDsInternal(OrigFields[i].second.get(), ModFields[i].second.get(), ChangedIDs);
      }
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
