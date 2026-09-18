#ifndef AST_EXPR_H
#define AST_EXPR_H

#include "AST.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APSInt.h"

/**
 * @brief A numeric literal. SEM-001 (docs/planning/ARCHITECTURE_OVERHAUL.md
 * §8, FREEZE.md §6.2 row 2): before this shape existed, an integer literal
 * was collapsed into a `long long` at LEX time (`strtoll`, `errno`
 * unchecked -- a 48-digit literal silently became `9223372036854775807`)
 * and a `double Val` copy was populated even when `IsInteger` was true, for
 * no consumer that ever needed it. Now the literal's value is carried
 * EXACTLY -- `llvm::APSInt` (arbitrary-precision, until SEM-002 checks it
 * against a destination type) for an integer, `llvm::APFloat` (the
 * exactly-rounded IEEE double, plus an explicit overflow flag) for a float
 * -- as a genuine tagged union: only one of IntVal_/FloatVal_ is ever
 * meaningful, selected by IsInteger_.
 *
 * `getIntVal()`/`getDoubleVal()` remain as narrow, TRUNCATING convenience
 * accessors (matching the pre-SEM-001 `long long`/`double` surface almost
 * every existing consumer only ever needed -- lint heuristics, debug
 * printing guarded by isInteger(), a compile-time array-bounds pass that
 * only ever deals in `int`-range indices) so this reshape did not force
 * every one of those call sites to become bignum-aware. A consumer that
 * genuinely needs the untruncated value (constant folding across a wide
 * literal, `--dump-ast`'s own round-trip proof, sandbox AST diffing) uses
 * getIntValExact()/getFloatValExact() instead -- see each call site's own
 * comment for which one it needed and why.
 */
class NumberExprAST : public ExprAST {
  llvm::APSInt IntVal_;    // exact, arbitrary-precision; meaningful iff IsInteger_
  llvm::APFloat FloatVal_; // exact rounded double; meaningful iff !IsInteger_
  bool IsInteger_;
  // True for a literal actually written in the source; false for a
  // compiler-synthesized constant (e.g. the desugared `1` in `x++`,
  // ParserStmt.cpp's default range bounds). SEM-002/later passes will use
  // this to decide whether a representability error should cite the
  // user's own source text or stay silent about an internal synthesis --
  // not consulted by anything in SEM-001 itself.
  bool IsUntyped_;
  // Float-only: true if decimal->binary rounding overflowed to +/-inf
  // (FREEZE.md §6.2 row 2's explicit "plus a flag" requirement). Always
  // false for a compiler-synthesized float and for any integer.
  bool Overflowed_ = false;
  // The literal's exact original source text (e.g. "0x1_000", "1.5e-3"),
  // empty for a compiler-synthesized constant. For diagnostics and for
  // --dump-ast's round-trip proof -- NOT necessarily reproducible from
  // IntVal_/FloatVal_ alone (radix, separators, and trailing zeros are all
  // lost once parsed into a value).
  std::string Spelling_;
  SourceLocation Loc_;

  // Full-fidelity constructor: every OTHER constructor below (and clone())
  // delegates here so there is exactly one place that initializes all
  // seven fields together -- see clone()'s own comment for why this
  // matters (a naive clone() built from the two "public convenience"
  // constructors alone silently lost IsUntyped_/Spelling_/Loc_).
  NumberExprAST(llvm::APSInt IV, llvm::APFloat FV, bool IsInteger,
               bool IsUntyped, bool Overflowed, std::string Spelling,
               SourceLocation Loc)
      : IntVal_(std::move(IV)), FloatVal_(std::move(FV)), IsInteger_(IsInteger),
        IsUntyped_(IsUntyped), Overflowed_(Overflowed),
        Spelling_(std::move(Spelling)), Loc_(Loc) {}

public:
  // Compiler-synthesized integer/float constants (IsUntyped_ = false, no
  // source spelling, a default-constructed {0,0,""} location) -- every
  // pre-existing call site that built a NumberExprAST directly
  // (ParserStmt.cpp's desugared range bounds, ParserCore.cpp's `x++`/`x--`
  // desugaring) keeps compiling unchanged through these two.
  explicit NumberExprAST(double Val)
      : NumberExprAST(llvm::APSInt::get(0), llvm::APFloat(Val), false, false,
                      false, "", SourceLocation{}) {}
  explicit NumberExprAST(long long Val)
      : NumberExprAST(llvm::APSInt::get(Val), llvm::APFloat::IEEEdouble(), true,
                      false, false, "", SourceLocation{}) {}

  // A real source literal, built by ParseNumberExpr (ParserLiteral.cpp)
  // from the lexer's own exact scan (NumLitSpelling/NumLitClean/
  // NumLitRadix, Lexer.h) -- IsUntyped_ = true.
  NumberExprAST(llvm::APSInt Val, std::string Spelling, SourceLocation Loc)
      : NumberExprAST(std::move(Val), llvm::APFloat::IEEEdouble(), true, true,
                      false, std::move(Spelling), Loc) {}
  NumberExprAST(llvm::APFloat Val, bool Overflowed, std::string Spelling,
               SourceLocation Loc)
      : NumberExprAST(llvm::APSInt::get(0), std::move(Val), false, true,
                      Overflowed, std::move(Spelling), Loc) {}

  bool isInteger() const { return IsInteger_; }
  bool isUntyped() const { return IsUntyped_; }
  bool overflowed() const { return Overflowed_; }
  const std::string &getSpelling() const { return Spelling_; }
  const SourceLocation &getLoc() const { return Loc_; }

  /// Exact, arbitrary-precision value. Only meaningful when isInteger().
  const llvm::APSInt &getIntValExact() const { return IntVal_; }
  /// Exact rounded value (see overflowed() for whether rounding it FROM the
  /// source text hit +/-inf). Only meaningful when !isInteger().
  const llvm::APFloat &getFloatValExact() const { return FloatVal_; }

  /// Narrow, truncating convenience accessor -- see the class doc comment.
  /// Safe against a wider-than-64-bit IntVal_ (never asserts/UB): widens or
  /// truncates to exactly 64 bits first, THEN extracts, so a literal like
  /// the 48-digit example in §10.2 returns a well-defined (if meaningless)
  /// value here instead of crashing -- exactness for that case lives only
  /// in getIntValExact().
  long long getIntVal() const { return IntVal_.extOrTrunc(64).getSExtValue(); }
  /// Only meaningful when !isInteger() -- every existing call site already
  /// guards with isInteger() before calling this (Lint.cpp, ASTPrinter).
  double getDoubleVal() const { return FloatVal_.convertToDouble(); }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitNumber(*this); }
  // Uses the private full-fidelity constructor directly (legal from a
  // member function regardless of access) so a clone of a
  // compiler-synthesized node stays compiler-synthesized (IsUntyped_
  // false) and a clone of a real source literal keeps its exact spelling
  // and location -- going through either public convenience constructor
  // instead would have silently reset IsUntyped_ to the wrong value for
  // one of the two cases.
  std::unique_ptr<ExprAST> clone() const override {
    return std::unique_ptr<NumberExprAST>(new NumberExprAST(
        IntVal_, FloatVal_, IsInteger_, IsUntyped_, Overflowed_, Spelling_,
        Loc_));
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

  // True only for the `x != true` shape ParseUnary() synthesizes to desugar
  // a literal `!x` in the source (see Parser.cpp's ParseUnary, "Desugar: !x
  // -> x != true"). Lint.cpp's redundant-boolean-comparison check would
  // otherwise treat EVERY `!x` in the entire language as if the user had
  // literally written the comparison `x != true` and nag them to "remove"
  // a comparison that doesn't exist in their source at all -- this flag is
  // how that check tells the two apart. Not preserved by construction (only
  // ParseUnary's desugar site sets it); clone() below does propagate it so
  // cloned generic-instantiation bodies don't regain the false positive.
  bool IsDesugaredNot = false;

  const SourceLocation &getLoc() const { return Loc; }
  int getOp() const { return Op; }
  const ExprAST *getLHS() const { return LHS.get(); }
  const ExprAST *getRHS() const { return RHS.get(); }

  /// `operator<sym>` overloads whose name matches this node's operator
  /// token, gathered by Resolver::visitBinary() (empty for non-overloadable
  /// tokens like bitwise/`&&`/`||`, and for whitelisted tokens with no
  /// visible operator declared at all -- e.g. plain `int + int`). Mirrors
  /// FuncCallExprAST::ResolvedCandidates's two-phase pattern (see its doc
  /// comment). clone() returns a fresh node, so this is empty in cloned trees.
  std::vector<std::shared_ptr<ResolverFunctionSymbol>> ResolvedOperatorCandidates;
  const std::vector<std::shared_ptr<ResolverFunctionSymbol>> &
  getResolvedOperatorCandidates() const {
    return ResolvedOperatorCandidates;
  }

  /// The single overload TypeChecker::visitBinary() selected from
  /// ResolvedOperatorCandidates by EXACT operand-type match (no numeric
  /// coercion -- see STRING_UFCS_OPERATOR_CHAR_RFC.md §4.2). Null means "no
  /// operator overload applies" -- Codegen falls through to the builtin
  /// arithmetic/comparison switch, exactly as it always has.
  /// clone() returns a fresh node, so this is null in cloned trees — same
  /// clone-semantics contract as ResolvedCallTarget.
  std::shared_ptr<ResolverFunctionSymbol> ResolvedOperatorTarget;
  const ResolverFunctionSymbol *getResolvedOperatorTarget() const {
    return ResolvedOperatorTarget.get();
  }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitBinary(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    auto Cloned = std::make_unique<BinaryExprAST>(Loc, static_cast<char>(Op),
                                                   CloneExprAST(LHS.get()),
                                                   CloneExprAST(RHS.get()));
    Cloned->IsDesugaredNot = IsDesugaredNot;
    // Note: ResolvedOperatorCandidates and ResolvedOperatorTarget are
    // intentionally NOT copied — cloned ASTs are stale until re-resolved.
    return Cloned;
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

  /// Populated by the Resolver instead of ResolvedSymbol when this bare name
  /// refers to an `export immut` constant (self-reference in the declaring
  /// file, or an on-demand-imported bare name) rather than a real scoped
  /// variable -- exported constants have no alloca/storage (see
  /// ResolverConstantSymbol's doc comment), so Codegen checks this field
  /// FIRST and emits the literal value directly, never falling through to
  /// the ordinary ResolvedSymbol/NamedValues lookup chain. Points into a
  /// long-lived unordered_map owned by the Resolver's symbol table (never
  /// erased during a compile), so the raw pointer stays valid throughout.
  /// Not copied by clone() -- same reasoning as ResolvedSymbol.
  const ResolverConstantSymbol *ResolvedConstant = nullptr;

  /// Populated by the Resolver instead of ResolvedSymbol/ResolvedConstant when
  /// this bare name refers to a real top-level `fn` used as a first-class
  /// function VALUE (e.g. `double_it` in `mutable f: fn(int): int =
  /// double_it;` or `apply(double_it, 5)`) rather than a local variable or
  /// constant. Only set here in visitVariable() -- a bare name in CALL
  /// position (the direct callee of a FuncCallExprAST) never reaches
  /// visitVariable() at all, and is resolved via the ordinary
  /// ResolvedCandidates/ResolvedCallTarget path on FuncCallExprAST instead
  /// (see Resolver::visitFuncCall's VarCallee branch). A distinct field from
  /// ResolvedSymbol because a function value has no alloca/storage of its
  /// own to look up -- Codegen returns the llvm::Function* directly. Not
  /// copied by clone() -- same reasoning as ResolvedSymbol/ResolvedConstant.
  std::shared_ptr<ResolverFunctionSymbol> ResolvedFunctionRef;

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

// A single-byte char literal ('x'). Deliberately a distinct AST node from
// StringExprAST -- see STRING_UFCS_OPERATOR_CHAR_RFC.md §3: char is a byte,
// not a codepoint, with no arithmetic and only built-in comparisons.
class CharExprAST : public ExprAST {
  char Val;

public:
  explicit CharExprAST(char Val) : Val(Val) {}
  char getVal() const { return Val; }

  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitChar(*this); }
  std::unique_ptr<ExprAST> clone() const override {
    return std::make_unique<CharExprAST>(Val);
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

#endif
