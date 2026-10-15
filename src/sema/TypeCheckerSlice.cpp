/**
 * @file TypeCheckerSlice.cpp
 * @brief The two rules that keep an unsized `T[]` parameter safe (issue #260).
 * @details A `T[]` parameter is a slice: a pointer into the caller's array
 *          plus its length (getSliceLLVMType(), CodegenCore.cpp). That makes
 *          it the caller's own storage, borrowed for one call, which is what
 *          these two checks protect:
 *
 *          - E140: `T[]` may only be a parameter's own type. Anywhere else it
 *            could outlive the array it points into.
 *          - E141: an `immut` array may not be handed to a `T[]` parameter
 *            the callee writes through, or the call would change it.
 *
 * @author a2ys
 * @copyright Copyright (c) 2026 TurfLang contributors.  Apache License.
 */

#include "sema/TypeChecker.h"

#include "frontend/AST.h"
#include "frontend/ASTChildWalker.h"
#include "diagnostics/Errors.h"
#include "diagnostics/ErrorCodes.h"

#include <optional>
#include <string>
#include <unordered_set>


// The variable an lvalue/argument expression is rooted at, looking through
// field accesses, indexing and slicing: `a`, `a[i]`, `a[1..]`, `s.arr`,
// `s.rows[i][j]` all name `a`/`s`. Null for anything else (a call result, a
// literal).
static const VariableExprAST *rootVariable(const ExprAST *E) {
  while (E) {
    if (const auto *V = dynamic_cast<const VariableExprAST *>(E))
      return V;
    if (const auto *AA = dynamic_cast<const ArrayAccessExprAST *>(E))
      E = AA->getBase();
    else if (const auto *Sl = dynamic_cast<const SliceExprAST *>(E))
      E = Sl->getBase();
    else if (const auto *FA = dynamic_cast<const FieldAccessExprAST *>(E))
      E = FA->getBase();
    else
      return nullptr;
  }
  return nullptr;
}


// ─── E140: where `T[]` may appear ────────────────────────────────────────────

// The first unsized array inside @p T that is not @p T itself (when
// @p AllowAtTop), or null. Every wrapper a type can have is looked through:
// an element type, a pointee (`&T`, `&?T`, `T*`), a generic type argument, a
// fallible success type, a function type's parameters and return type.
static const ArrayType *findMisplacedUnsized(const TurfType *T, bool AllowAtTop) {
  if (!T)
    return nullptr;
  if (const auto *AT = dynamic_cast<const ArrayType *>(T)) {
    if (AT->getSize() == -1 && !AllowAtTop)
      return AT;
    return findMisplacedUnsized(AT->getElementType(), false);
  }
  if (const auto *PT = dynamic_cast<const TurfPointerType *>(T))
    return findMisplacedUnsized(PT->getPointee().get(), false);
  if (const auto *GT = dynamic_cast<const GenericType *>(T)) {
    for (const auto &Arg : GT->getTypeArgs())
      if (const ArrayType *Found = findMisplacedUnsized(Arg.get(), false))
        return Found;
    return nullptr;
  }
  if (const auto *EU = dynamic_cast<const TurfErrorUnionType *>(T))
    return findMisplacedUnsized(EU->getSuccessTypePtr().get(), false);
  if (const auto *FT = dynamic_cast<const FuncType *>(T)) {
    // A function VALUE with a `T[]` parameter is refused too: a call through
    // it has no declaration to check for E141, so an `immut` array could be
    // changed through it unseen.
    for (const auto &P : FT->getParamTypes())
      if (const ArrayType *Found = findMisplacedUnsized(P.get(), false))
        return Found;
    return findMisplacedUnsized(FT->getReturnType().get(), false);
  }
  return nullptr;
}


bool TypeChecker::checkUnsizedArrayPlacement(const TurfType *T, bool AllowAtTop,
                                             SourceLocation Loc,
                                             const std::string &What,
                                             const std::string &Hint) {
  const ArrayType *Bad = findMisplacedUnsized(T, AllowAtTop);
  if (!Bad)
    return false;
  std::string Msg = What + " can't use the unsized array type '" +
                    Bad->getName() + "'";
  if (Bad != T)
    Msg += " (inside '" + T->getName() + "')";
  Msg += ".";
  reportError(Loc, ErrCode::UnsizedArrayPlacement, Msg, {},
              Hint.empty()
                  ? "'" + Bad->getName() + "' only works as a function "
                    "parameter's own type, where it accepts an array of any "
                    "length. Everywhere else an array needs its size, e.g. '" +
                    Bad->getElementType()->getName() + "[5]'."
                  : Hint,
              "Unsized array here!");
  return true;
}


void TypeChecker::checkSliceTypeArguments(
    const std::unordered_map<std::string, TurfTypePtr> &Subst,
    const std::string &CalleeName, SourceLocation Loc) {
  for (const auto &[Name, Ty] : Subst) {
    if (checkUnsizedArrayPlacement(
            Ty.get(), /*AllowAtTop=*/false, Loc,
            "The type parameter '" + Name + "' of '" + CalleeName + "'",
            "A type parameter can't stand for an unsized array: a '" + Name +
                "' value may be stored or returned, and a slice may not "
                "outlive the call it was passed to. To take an array of any "
                "length, declare the parameter as '" + Name + "[]' instead."))
      return;
  }
}


// ─── Range slicing: `arr[a..b]` ─────────────────────────────────────────────

// The value of a bound when it is a constant the checker can fold (`2`, `-1`,
// `3 + 1`); nullopt otherwise.
std::optional<long long> TypeChecker::constantSliceBound(const ExprAST *E) {
  if (!E)
    return std::nullopt;
  auto V = tryFoldUntypedIntConstant(E, /*IsTopLevel=*/true);
  if (!V || !V->isSignedIntN(64))
    return std::nullopt;
  return V->getSExtValue();
}


void TypeChecker::visitSlice(SliceExprAST &Node) {
  if (Node.getBase())
    const_cast<ExprAST *>(Node.getBase())->accept(*this);
  if (Node.getStart())
    const_cast<ExprAST *>(Node.getStart())->accept(*this);
  if (Node.getEnd())
    const_cast<ExprAST *>(Node.getEnd())->accept(*this);

  TurfTypePtr BaseTy = getType(Node.getBase());
  if (!BaseTy) {
    setType(&Node, nullptr); // already reported
    return;
  }

  // E142: only a built-in array has the contiguous elements a view needs.
  const auto *ArrTy = dynamic_cast<const ArrayType *>(BaseTy.get());
  if (!ArrTy) {
    std::string Hint;
    if (BaseTy->isString())
      Hint = "A string can't be sliced with [a..b]: it is not an array of "
             "elements but text ending at a zero byte. Use "
             "strings::substring(s, start, end) for a new string.";
    else if (dynamic_cast<const GenericType *>(BaseTy.get()) ||
             dynamic_cast<const TurfStructType *>(BaseTy.get()))
      Hint = "Only a built-in array (like int[5], or an int[] parameter) can "
             "be sliced. To take part of a collection, copy the elements you "
             "need in a loop.";
    else
      Hint = "Only a built-in array (like int[5], or an int[] parameter) can "
             "be sliced.";
    reportError(Node.getLoc(), ErrCode::SliceOfNonArray,
                "Cannot slice a value of type '" + BaseTy->getName() +
                    "' with [a..b].",
                {}, Hint, "Not an array!");
    setType(&Node, nullptr);
    return;
  }

  // Bounds are whole numbers, exactly as an index is (E002).
  for (const ExprAST *Bound : {Node.getStart(), Node.getEnd()}) {
    TurfTypePtr BTy = Bound ? getType(Bound) : nullptr;
    if (BTy && !BTy->isInt() && !BTy->isInt32()) {
      reportError(Node.getLoc(), ErrCode::ArrayNonIntegerIndex,
                  "You used a value of type '" + BTy->getName() +
                      "' as a slice bound of '" + BaseTy->getName() + "'.",
                  {},
                  "Slice bounds are ints, like arr[1..3] or arr[i..j]. To "
                  "turn a number or a char into an int: (x to int).",
                  "Index must be an integer!");
      setType(&Node, nullptr);
      return;
    }
  }

  // E143: a range that is provably wrong before the program runs. With a
  // known length (a `T[N]` base), both bounds are normalized the way a
  // negative index is and must satisfy 0 <= start <= end <= N. Without one
  // (a `T[]` base), only start > end can be proven, and only when both
  // bounds have the same sign -- `[-1..2]` depends on the length. Anything
  // else is checked at run time (CodegenArray.cpp, SliceExprAST::codegen()).
  const std::optional<long long> S = constantSliceBound(Node.getStart());
  const std::optional<long long> E = constantSliceBound(Node.getEnd());
  const long long N = ArrTy->getSize();
  auto Show = [&](const ExprAST *B, std::optional<long long> V, const char *Dflt) {
    return B ? (V ? std::to_string(*V) : std::string("?")) : std::string(Dflt);
  };
  const std::string Written =
      "[" + Show(Node.getStart(), S, "") + ".." + Show(Node.getEnd(), E, "") + "]";
  std::string Problem;
  if (N >= 0) {
    long long SN = S ? (*S < 0 ? *S + N : *S) : 0;
    long long EN = E ? (*E < 0 ? *E + N : *E) : N;
    if ((S || !Node.getStart()) && (SN < 0 || SN > N))
      Problem = "the start is outside 0.." + std::to_string(N);
    else if ((E || !Node.getEnd()) && (EN < 0 || EN > N))
      Problem = "the end is outside 0.." + std::to_string(N);
    else if ((S || !Node.getStart()) && (E || !Node.getEnd()) && SN > EN)
      Problem = "the start comes after the end";
  } else if (S && E && ((*S >= 0) == (*E >= 0)) && *S > *E) {
    Problem = "the start comes after the end";
  }
  if (!Problem.empty()) {
    reportError(Node.getLoc(), ErrCode::SliceRangeOutOfBounds,
                "The slice " + Written + " of '" + BaseTy->getName() +
                    "' is out of range: " + Problem + ".",
                {},
                "A slice [start..end] needs 0 <= start <= end <= length; the "
                "end is exclusive, so arr[0..length] is the whole array and "
                "arr[i..i] is empty. A negative bound counts from the end "
                "(arr[-2..] is the last two elements).",
                "Slice out of range!");
    setType(&Node, nullptr);
    return;
  }

  setType(&Node, makeArrayType(ArrTy->getElementTypePtr(), -1));
}


bool TypeChecker::checkMutableViewSource(const ExprAST *Source,
                                         SourceLocation Loc,
                                         const std::string &ViewName) {
  if (!Source)
    return false;
  TurfTypePtr SrcTy = getType(Source);
  if (!SrcTy || !SrcTy->isArray())
    return false;
  const VariableExprAST *Root = rootVariable(Source);
  if (!Root)
    return false;
  const bool Immutable =
      (Root->getResolvedSymbol() && Root->getResolvedSymbol()->IsImmutable) ||
      Root->ResolvedConstant;
  if (!Immutable)
    return false;
  std::vector<RelatedLocation> Related;
  if (Root->getResolvedSymbol())
    Related.push_back({toSourceLocation(Root->getResolvedSymbol()->DeclLoc),
                       "declared immutable here"});
  reportError(Loc, ErrCode::MutableViewOfImmutableArray,
              "'" + ViewName + "' is a 'mut' view of '" + Root->getName() +
                  "', which is immutable.",
              Related,
              "A slice shares the array it looks at, so writing through '" +
                  ViewName + "' would change '" + Root->getName() +
                  "'. Declare '" + ViewName + "' with 'immut' if you only "
                  "read it, or make '" + Root->getName() + "' 'mut'.",
              "Mutable view of an immutable array!");
  return true;
}


// ─── E141: writing through a slice into an immutable array ──────────────────

// The set of variables that view one slice parameter's array: the
// parameter itself, plus every local `T[]` made from it (`mut s: int[] =
// p[1..]`, or reassigned later -- issue #260's range slicing).
using ViewSet = std::unordered_set<const VariableSymbol *>;

static bool inViews(const ExprAST *E, const ViewSet &Views) {
  const VariableExprAST *Root = rootVariable(E);
  return Root && Views.count(Root->getResolvedSymbol());
}


// Is @p E an element (or a part of an element) of an array in @p Views:
// `p[i]`, `p[i].x`, `p[i][j]`, `p[1..][0]`? A bare `p` is not -- assigning to
// the view itself rebinds it, it does not write the caller's array.
static bool isSliceElementPlace(const ExprAST *E, const ViewSet &Views) {
  bool SawIndex = false;
  while (E) {
    if (const auto *V = dynamic_cast<const VariableExprAST *>(E))
      return SawIndex && Views.count(V->getResolvedSymbol());
    if (const auto *AA = dynamic_cast<const ArrayAccessExprAST *>(E)) {
      // An `operator[]` read is a copy (E098 covers writes to it), not a slot.
      if (AA->ResolvedIndexGetTarget)
        return false;
      SawIndex = true;
      E = AA->getBase();
    } else if (const auto *Sl = dynamic_cast<const SliceExprAST *>(E)) {
      E = Sl->getBase();
    } else if (const auto *FA = dynamic_cast<const FieldAccessExprAST *>(E)) {
      E = FA->getBase();
    } else {
      return false;
    }
  }
  return false;
}


namespace {

// Grows a ViewSet with the local `T[]` variables made from one of its
// members. Run to a fixed point: `b` made from `a` made from `p`.
class SliceAliasCollector : public ASTChildWalker {
public:
  explicit SliceAliasCollector(ViewSet &Views) : Views(Views) {}
  bool Grew = false;

  void visitVarDecl(VarDeclExprAST &Node) override {
    if (isUnsized(Node.getResolvedSymbol()) && Node.getInitializer() &&
        inViews(Node.getInitializer(), Views))
      add(Node.getResolvedSymbol());
    ASTChildWalker::visitVarDecl(Node);
  }
  void visitArrayDecl(ArrayDeclExprAST &Node) override {
    if (isUnsized(Node.getResolvedSymbol()) && Node.getInitExpr() &&
        inViews(Node.getInitExpr().get(), Views))
      add(Node.getResolvedSymbol());
    ASTChildWalker::visitArrayDecl(Node);
  }
  void visitAssignment(AssignmentExprAST &Node) override {
    const auto *L = dynamic_cast<const VariableExprAST *>(Node.getLHS());
    if (L && isUnsized(L->getResolvedSymbol()) && inViews(Node.getRHS(), Views))
      add(L->getResolvedSymbol());
    ASTChildWalker::visitAssignment(Node);
  }

private:
  ViewSet &Views;
  static bool isUnsized(const VariableSymbol *S) {
    const auto *AT = S ? dynamic_cast<const ArrayType *>(S->Type.get()) : nullptr;
    return AT && AT->getSize() == -1;
  }
  void add(const VariableSymbol *S) {
    if (S && Views.insert(S).second)
      Grew = true;
  }
};


// Finds a write through one slice parameter (or a view of it) in a function
// body. Variables are identified by VariableSymbol (the Resolver binds every
// use to its declaration), so a local that shadows a name is not mistaken
// for it.
class SliceWriteFinder : public ASTChildWalker {
public:
  SliceWriteFinder(TypeChecker &TC, const ViewSet &Views)
      : TC(TC), Views(Views) {}

  bool Found = false;

  void visitAssignment(AssignmentExprAST &Node) override {
    if (isSliceElementPlace(Node.getLHS(), Views))
      Found = true;
    ASTChildWalker::visitAssignment(Node);
  }
  void visitCompoundAssignment(CompoundAssignmentExprAST &Node) override {
    if (isSliceElementPlace(Node.getLHS(), Views))
      Found = true;
    ASTChildWalker::visitCompoundAssignment(Node);
  }
  void visitPostfixExpr(PostfixExprAST &Node) override {
    if (isSliceElementPlace(Node.getOperand(), Views))
      Found = true;
    ASTChildWalker::visitPostfixExpr(Node);
  }

  void visitFuncCall(FuncCallExprAST &Node) override {
    if (const auto *Target = Node.getResolvedCallTarget())
      checkHandOff(Node.getArgs(), nullptr, *Target, 0);
    ASTChildWalker::visitFuncCall(Node);
  }

  void visitMethodCall(MethodCallExprAST &Node) override {
    if (const auto *Target = Node.getResolvedCallTarget()) {
      if (Node.ResolvedViaUFCS) {
        checkHandOff(Node.getArgs(), Node.getBase(), *Target, 1);
      } else {
        const auto *BaseVar = dynamic_cast<const VariableExprAST *>(Node.getBase());
        const bool IsStatic = BaseVar && BaseVar->IsStructName;
        checkHandOff(Node.getArgs(), nullptr, *Target, IsStatic ? 0 : 1);
        // `p[i].bump()`, where bump() changes its receiver: the receiver is
        // the element itself (methods take `self` by address).
        if (!IsStatic && isSliceElementPlace(Node.getBase(), Views) &&
            (Target->MutatesSelf || TC.mutatesSelfTransitively(Target->Decl)))
          Found = true;
      }
    }
    ASTChildWalker::visitMethodCall(Node);
  }

private:
  TypeChecker &TC;
  const ViewSet &Views;

  // The array handed on -- `p`, `p[1..]`, `view`, `p[0]` of an `int[][3]` --
  // to another function's `T[]` parameter that writes.
  void checkHandOff(const std::vector<std::unique_ptr<ExprAST>> &Args,
                    const ExprAST *Receiver,
                    const ResolverFunctionSymbol &Target, size_t ParamOffset) {
    auto Check = [&](const ExprAST *Arg, size_t ParamIdx) {
      if (!inViews(Arg, Views))
        return;
      if (ParamIdx >= Target.Params.size() ||
          !dynamic_cast<const ArrayType *>(Target.Params[ParamIdx].Type.get()) ||
          static_cast<const ArrayType *>(Target.Params[ParamIdx].Type.get())->getSize() != -1)
        return;
      if (TC.targetWritesThroughSlice(Target, ParamIdx))
        Found = true;
    };
    if (Receiver)
      Check(Receiver, 0);
    for (size_t I = 0; I < Args.size(); ++I)
      Check(Args[I].get(), I + ParamOffset);
  }
};

} // namespace


bool TypeChecker::mutatesSelfTransitively(const FuncDefExprAST *Decl) const {
  return RST_.mutatesSelfTransitively(Decl);
}


// Maps a call target's parameter index onto its declaration's own parameter
// list. They differ only for a constructor candidate, whose Params the
// Resolver strips of `self` (stripSelf(), ResolverCall.cpp).
bool TypeChecker::targetWritesThroughSlice(const ResolverFunctionSymbol &Target,
                                           size_t ParamIdx) {
  const FuncDefExprAST *Decl = Target.Decl;
  if (!Decl)
    return false; // an extern fn cannot take a `T[]` (E140)
  const auto &DeclParams = Decl->getParams();
  size_t Offset = 0;
  if (!DeclParams.empty() && DeclParams[0].Name == "self" &&
      (Target.Params.empty() || Target.Params[0].Name != "self"))
    Offset = 1;
  return writesThroughSliceParam(Decl, ParamIdx + Offset);
}


bool TypeChecker::writesThroughSliceParam(const FuncDefExprAST *Decl,
                                          size_t ParamIdx) {
  auto Key = std::make_pair(Decl, ParamIdx);
  auto It = SliceWriteMemo_.find(Key);
  if (It != SliceWriteMemo_.end())
    return It->second == 2;

  const auto &Params = Decl->getParams();
  if (ParamIdx >= Params.size() || !Decl->getBody()) {
    SliceWriteMemo_[Key] = 1;
    return false;
  }

  // The parameter's VariableSymbol is the one every use in the body
  // resolves to; find it from the first such use. A parameter the body never
  // names cannot be written.
  const VariableSymbol *ParamSym = nullptr;
  {
    class FindParam : public ASTChildWalker {
    public:
      const std::string &Name;
      const VariableSymbol *Sym = nullptr;
      explicit FindParam(const std::string &Name) : Name(Name) {}
      void visitVariable(VariableExprAST &Node) override {
        if (!Sym && Node.getName() == Name && Node.getResolvedSymbol() &&
            Node.getResolvedSymbol()->IsParameter)
          Sym = Node.getResolvedSymbol();
      }
    } Finder(Params[ParamIdx].Name);
    Finder.visitChild(Decl->getBody());
    ParamSym = Finder.Sym;
  }
  if (!ParamSym) {
    SliceWriteMemo_[Key] = 1;
    return false;
  }

  SliceWriteMemo_[Key] = 0; // in progress: a recursive call contributes nothing
  ViewSet Views{ParamSym};
  for (bool Grew = true; Grew;) {
    SliceAliasCollector Aliases(Views);
    Aliases.visitChild(Decl->getBody());
    Grew = Aliases.Grew;
  }
  SliceWriteFinder Finder(*this, Views);
  Finder.visitChild(Decl->getBody());
  SliceWriteMemo_[Key] = Finder.Found ? 2 : 1;
  return Finder.Found;
}


void TypeChecker::noteSliceArguments(
    const std::vector<std::unique_ptr<ExprAST>> &Args, const ExprAST *Receiver,
    const std::shared_ptr<ResolverFunctionSymbol> &Target, size_t ParamOffset,
    SourceLocation Loc) {
  if (!Target || !Target->Decl)
    return;
  auto Note = [&](const ExprAST *Arg, size_t ParamIdx) {
    if (ParamIdx >= Target->Params.size())
      return;
    const auto *PT = dynamic_cast<const ArrayType *>(Target->Params[ParamIdx].Type.get());
    if (!PT || PT->getSize() != -1)
      return;
    const VariableExprAST *Root = rootVariable(Arg);
    if (!Root || !Root->getResolvedSymbol() || !Root->getResolvedSymbol()->IsImmutable)
      return;
    PendingSliceWrites_.push_back({Loc, Root->getResolvedSymbol(),
                                   Root->getName(), Target, ParamIdx});
  };
  if (Receiver)
    Note(Receiver, 0);
  for (size_t I = 0; I < Args.size(); ++I)
    Note(Args[I].get(), I + ParamOffset);
}


void TypeChecker::checkSliceWrites() {
  // Move out first: reportError() may longjmp (per-declaration recovery),
  // and an entry must never be reported twice.
  std::vector<PendingSliceWrite> PendingSelf;
  PendingSelf.swap(PendingSelfSliceWrites_);
  for (const auto &P : PendingSelf) {
    // `self` is the declaration's parameter 0; writesThroughSliceParam()
    // counts a write to an element of a view rooted at it, or handing such
    // a view to a writing `T[]` parameter. Direct `self.x = v` writes were
    // already refused through MutatesSelf.
    const FuncDefExprAST *Decl = P.Callee->Decl;
    if (!Decl || Decl->getParams().empty() || Decl->getParams()[0].Name != "self" ||
        !writesThroughSliceParam(Decl, 0))
      continue;
    ImmutableAssignmentError(P.Loc, P.ArrayName)
        .withRelated(toSourceLocation(P.Array->DeclLoc), "declared immutable here")
        .withRelated(toSourceLocation(P.Callee->DeclLoc),
                     "'" + P.Callee->BaseName +
                         "' changes it through a slice of one of its arrays")
        .raise();
  }

  std::vector<PendingSliceWrite> Pending;
  Pending.swap(PendingSliceWrites_);
  for (const auto &P : Pending) {
    if (!targetWritesThroughSlice(*P.Callee, P.ParamIdx))
      continue;
    const auto &Param = P.Callee->Params[P.ParamIdx];
    std::vector<RelatedLocation> Related;
    Related.push_back({toSourceLocation(P.Array->DeclLoc), "declared immutable here"});
    Related.push_back({toSourceLocation(P.Callee->DeclLoc),
                       "'" + P.Callee->BaseName + "' changes its parameter '" +
                           Param.Name + "'"});
    reportError(P.Loc, ErrCode::ImmutableArrayWrittenThroughSlice,
                "'" + P.ArrayName + "' is immutable, but '" + P.Callee->BaseName +
                    "' writes to the array it is given as '" + Param.Name + "'.",
                Related,
                "An unsized parameter ('" + Param.Type->getName() +
                    "') shares the caller's array instead of copying it, so "
                    "the change would land in '" + P.ArrayName +
                    "'. Declare '" + P.ArrayName +
                    "' with 'mut', or copy it into a 'mut' array first and "
                    "pass the copy.",
                "Immutable array changed!");
  }
}
