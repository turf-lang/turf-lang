/**
 * @file AnalysisFacade.cpp
 * @brief Implements the compiler query facade -- see include/turf/Analysis.h.
 * @details Two halves. GlobalStateScope is the only code in the compiler
 *          that saves and restores the front end's global state around an
 *          isolated compile; analyze() runs the passes inside one. The rest
 *          is the position index the queries answer from, built while the
 *          Resolver walks the program.
 *
 * @author a2ys
 * @copyright Copyright (c) 2026 TurfLang contributors. Licensed under the
 * Apache License.
 */

#include "turf/Analysis.h"

#include "AST.h"
#include "Lexer.h"
#include "Lint.h"
#include "Parser.h"
#include "Resolver.h"
#include "SymbolTable.h"
#include "TypeChecker.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"

#include <csetjmp>
#include <cstring>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace turf {

namespace {

// ── Global state ────────────────────────────────────────────────────────────
//
// Every global the Lexer -> TypeChecker pipeline reads or writes, saved on
// construction and put back on destruction, with the compile in between
// starting from a fresh-process state. Anything added to the front end that
// one compile accumulates and a second compile must not see belongs here
// (or behind one of the Push/Pop pairs this calls); tests/api/ runs two
// different programs back to back and against fresh runs to catch a
// missing one.
//
// Deliberately NOT here:
//   - GlobalSourceFile, the ifstream a CLI compile reads its input through.
//     This scope never opens, closes, or seeks it -- analyze() points
//     SourceFile at its own in-memory stream instead -- so a host compile
//     that is mid-parse when an analysis runs inside it (the SLM Sandbox)
//     resumes at the same byte.
//   - BinopPrecedence and the builtin registry: filled once, identically,
//     and never modified after.
//   - IndexModuleDir()'s directory index (ParserModule.cpp): a cache of
//     what exists under lib/std, not per-compile state.
//   - ExprAST::NextNodeID: only ever compared for equality within one tree.
//   - Codegen's globals and GlobalCFGs: no pass analyze() runs touches them.
class GlobalStateScope {
public:
  GlobalStateScope() {
    // Lexer cursor (the same set ParseFile()'s LexerStateGuard keeps, plus
    // what that guard can leave alone because it never outlives a compile).
    SavedSourceFile_ = SourceFile;
    SavedLastChar_ = GetLastChar();
    SavedCurTok_ = CurTok;
    SavedCurLoc_ = CurLoc;
    SavedCurLine_ = CurLine;
    SavedCurCol_ = CurCol;
    SavedNumLitSpelling_ = NumLitSpelling;
    SavedNumLitClean_ = NumLitClean;
    SavedNumLitRadix_ = NumLitRadix;
    SavedBoolVal_ = BoolVal;
    SavedCharVal_ = CharVal;
    SavedIdentifierStr_ = IdentifierStr;
    SavedStringVal_ = StringVal;
    SavedBraceDepth_ = g_braceDepth;
    SavedCurlyDepth_ = g_curlyDepth;
    SavedParseRecursionDepth_ = g_parseRecursionDepth;
    SavedProtoOnlyPass_ = g_protoOnlyPass;

    SavedSourceFilename_ = SourceFilename;
    SavedMainSourceFilename_ = MainSourceFilename;
    SavedSourceLines_ = std::move(SourceLines);
    SourceLines.clear();
    // Swapped rather than copied: it holds every line of every file the
    // host compile has read, lib/std included.
    SavedAllSourceLines_.swap(AllSourceLines);

    std::memcpy(SavedRecoverJmp_, g_recoverJmp, sizeof(jmp_buf));
    SavedRecoverActive_ = g_recoverActive;
    SavedAIEnabled_ = g_AIEnabled;
    SavedAllowUnsafeModules_ = g_AllowUnsafeModules;
    SavedCompilerExecutablePath_ = CompilerExecutablePath;

    SavedDiagnostics_ = DiagnosticEngine::saveState();
    DiagnosticEngine::restoreState({});

    // InitializeCompilerGlobals() replaces GlobalSymbolTable outright; the
    // host's must survive it.
    SavedGlobalSymbolTable_ = std::move(GlobalSymbolTable);
    InitializeCompilerGlobals();

    PushParserCompileState();
    PushResolverCompileState();

    g_braceDepth = 0;
    g_curlyDepth = 0;
    g_parseRecursionDepth = 0;
    g_protoOnlyPass = false;
    g_recoverActive = false;
  }

  ~GlobalStateScope() {
    PopResolverCompileState();
    PopParserCompileState();

    GlobalSymbolTable = std::move(SavedGlobalSymbolTable_);
    DiagnosticEngine::restoreState(std::move(SavedDiagnostics_));

    CompilerExecutablePath = SavedCompilerExecutablePath_;
    g_AllowUnsafeModules = SavedAllowUnsafeModules_;
    g_AIEnabled = SavedAIEnabled_;
    g_recoverActive = SavedRecoverActive_;
    std::memcpy(g_recoverJmp, SavedRecoverJmp_, sizeof(jmp_buf));

    AllSourceLines.swap(SavedAllSourceLines_);
    SourceLines = std::move(SavedSourceLines_);
    MainSourceFilename = SavedMainSourceFilename_;
    SourceFilename = SavedSourceFilename_;

    g_protoOnlyPass = SavedProtoOnlyPass_;
    g_parseRecursionDepth = SavedParseRecursionDepth_;
    g_braceDepth = SavedBraceDepth_;
    g_curlyDepth = SavedCurlyDepth_;
    StringVal = SavedStringVal_;
    IdentifierStr = SavedIdentifierStr_;
    CharVal = SavedCharVal_;
    BoolVal = SavedBoolVal_;
    NumLitRadix = SavedNumLitRadix_;
    NumLitClean = SavedNumLitClean_;
    NumLitSpelling = SavedNumLitSpelling_;
    CurCol = SavedCurCol_;
    CurLine = SavedCurLine_;
    CurLoc = SavedCurLoc_;
    CurTok = SavedCurTok_;
    SetLastChar(SavedLastChar_);
    SourceFile = SavedSourceFile_;
  }

  GlobalStateScope(const GlobalStateScope &) = delete;
  GlobalStateScope &operator=(const GlobalStateScope &) = delete;

private:
  std::istream *SavedSourceFile_;
  int SavedLastChar_, SavedCurTok_, SavedCurLine_, SavedCurCol_;
  SourceLocation SavedCurLoc_;
  std::string SavedNumLitSpelling_, SavedNumLitClean_;
  int SavedNumLitRadix_;
  bool SavedBoolVal_;
  char SavedCharVal_;
  std::string SavedIdentifierStr_, SavedStringVal_;
  int SavedBraceDepth_, SavedCurlyDepth_, SavedParseRecursionDepth_;
  bool SavedProtoOnlyPass_;
  std::string SavedSourceFilename_, SavedMainSourceFilename_;
  std::vector<std::string> SavedSourceLines_;
  std::map<std::string, std::vector<std::string>> SavedAllSourceLines_;
  jmp_buf SavedRecoverJmp_;
  bool SavedRecoverActive_, SavedAIEnabled_, SavedAllowUnsafeModules_;
  std::string SavedCompilerExecutablePath_;
  DiagnosticEngine::State SavedDiagnostics_;
  std::unique_ptr<SymbolTable> SavedGlobalSymbolTable_;
};

// One analysis at a time per process: everything GlobalStateScope saves is
// shared by every thread. Recursive because an analysis may legitimately
// start inside another on the same thread.
std::recursive_mutex AnalysisMutex;

// ── Node spans ──────────────────────────────────────────────────────────────

// Where a node starts and, for a node that names something, how long that
// name is. ExprAST has no virtual getLoc() -- each node class defines its
// own -- hence a visitor. Kinds not overridden (BoolExprAST, CharExprAST,
// StringExprAST, BlockExprAST, ...) carry no location and are skipped.
class NodeSpanVisitor : public ASTVisitor {
public:
  SourceLocation Loc;
  unsigned NameLen = 0;
  bool Found = false;

#define TURF_SPAN_LOC(Method, Type)                                          \
  void Method(Type &Node) override { set(Node.getLoc(), 0); }
  TURF_SPAN_LOC(visitNumber, NumberExprAST)
  TURF_SPAN_LOC(visitNull, NullExprAST)
  TURF_SPAN_LOC(visitBinary, BinaryExprAST)
  TURF_SPAN_LOC(visitUnary, UnaryExprAST)
  TURF_SPAN_LOC(visitAddressOf, AddressOfExprAST)
  TURF_SPAN_LOC(visitAssignment, AssignmentExprAST)
  TURF_SPAN_LOC(visitCompoundAssignment, CompoundAssignmentExprAST)
  TURF_SPAN_LOC(visitPostfixExpr, PostfixExprAST)
  TURF_SPAN_LOC(visitCast, CastExprAST)
  TURF_SPAN_LOC(visitIf, IfExprAST)
  TURF_SPAN_LOC(visitWhile, WhileExprAST)
  TURF_SPAN_LOC(visitDoWhile, DoWhileExprAST)
  TURF_SPAN_LOC(visitFor, ForExprAST)
  TURF_SPAN_LOC(visitForEach, ForEachExprAST)
  TURF_SPAN_LOC(visitBreak, BreakExprAST)
  TURF_SPAN_LOC(visitContinue, ContinueExprAST)
  TURF_SPAN_LOC(visitReturn, ReturnExprAST)
  TURF_SPAN_LOC(visitCompare, CompareExprAST)
  TURF_SPAN_LOC(visitWildcardPattern, WildcardPatternExprAST)
  TURF_SPAN_LOC(visitStructPattern, StructPatternExprAST)
  TURF_SPAN_LOC(visitErrorVariantPattern, ErrorVariantPatternExprAST) // LEC-004
  TURF_SPAN_LOC(visitFuncCall, FuncCallExprAST)
  TURF_SPAN_LOC(visitEnumDecl, EnumDeclExprAST)
  TURF_SPAN_LOC(visitMachineDecl, MachineDeclExprAST)
  TURF_SPAN_LOC(visitErrorDecl, ErrorDeclExprAST)
  TURF_SPAN_LOC(visitRecoverExpr, RecoverExprAST)
  TURF_SPAN_LOC(visitTry, TryExprAST) // LEC-004
  TURF_SPAN_LOC(visitArrayDecl, ArrayDeclExprAST)
  TURF_SPAN_LOC(visitArrayLiteral, ArrayLiteralExprAST)
  TURF_SPAN_LOC(visitArrayAccess, ArrayAccessExprAST)
  TURF_SPAN_LOC(visitSlice, SliceExprAST)
  TURF_SPAN_LOC(visitSizeOf, SizeOfExprAST)
#undef TURF_SPAN_LOC

#define TURF_SPAN_NAME(Method, Type, LocGetter, NameGetter)                  \
  void Method(Type &Node) override {                                         \
    set(Node.LocGetter(), Node.NameGetter().size());                         \
  }
  TURF_SPAN_NAME(visitVariable, VariableExprAST, getLoc, getName)
  TURF_SPAN_NAME(visitBindingPattern, BindingPatternExprAST, getLoc, getName)
  TURF_SPAN_NAME(visitVarDecl, VarDeclExprAST, getNameLoc, getName)
  TURF_SPAN_NAME(visitFuncDef, FuncDefExprAST, getNameLoc, getName)
  TURF_SPAN_NAME(visitExternFuncDecl, ExternFuncDeclExprAST, getNameLoc,
                 getName)
  TURF_SPAN_NAME(visitMethodCall, MethodCallExprAST, getNameLoc, getMethod)
  TURF_SPAN_NAME(visitFieldAccess, FieldAccessExprAST, getNameLoc,
                 getFieldName)
  TURF_SPAN_NAME(visitScopeAccess, ScopeAccessExprAST, getRightLoc,
                 getRightName)
  TURF_SPAN_NAME(visitStructDecl, StructDeclExprAST, getNameLoc, getName)
  TURF_SPAN_NAME(visitModuleImport, ModuleImportExprAST, getNameLoc,
                 getModuleName)
#undef TURF_SPAN_NAME

private:
  void set(const SourceLocation &L, size_t Len) {
    Loc = L;
    NameLen = static_cast<unsigned>(Len);
    Found = true;
  }
};

NodeSpanVisitor spanOf(const ExprAST &Node) {
  NodeSpanVisitor V;
  const_cast<ExprAST &>(Node).accept(V);
  return V;
}

// The key a file is indexed and queried under: its real path when it has
// one on disk, else the path as given -- so "./a.tr", "a.tr" and a symlink
// to it all meet, and an unsaved buffer that exists nowhere still works.
std::string fileKey(const std::string &Path) {
  llvm::SmallString<256> Real;
  if (llvm::sys::fs::real_path(Path, Real))
    return Path;
  return Real.str().str();
}

std::string stripAnsi(const std::string &S) {
  std::string Out;
  Out.reserve(S.size());
  for (size_t I = 0; I < S.size();) {
    if (S[I] == '\x1b' && I + 1 < S.size() && S[I + 1] == '[') {
      size_t J = I + 2;
      while (J < S.size() && S[J] != 'm')
        ++J;
      I = J < S.size() ? J + 1 : J;
    } else {
      Out += S[I++];
    }
  }
  return Out;
}

Location toLocation(const SourceLocation &L, const std::string &Fallback) {
  return {L.Filename.empty() ? Fallback : L.Filename,
          static_cast<unsigned>(L.Line < 0 ? 0 : L.Line),
          static_cast<unsigned>(L.Col < 0 ? 0 : L.Col)};
}

Diagnostic toDiagnostic(const DiagnosticEngine::Diag &D,
                        const std::string &EntryPath) {
  Diagnostic Out;
  Out.Level = D.IsWarning ? Severity::Warning : Severity::Error;
  Out.Code = D.Code;
  Out.Message = stripAnsi(D.Msg);
  Out.Hint = stripAnsi(D.Hint);
  Out.Where = toLocation(D.Loc, D.Filename.empty() ? EntryPath : D.Filename);
  for (const auto &R : D.RelatedLocations)
    Out.Related.push_back(
        {toLocation(R.Loc, Out.Where.Path), stripAnsi(R.Description)});
  return Out;
}

bool isDeclarationStart(int Tok) {
  return Tok == TOK_FN || Tok == TOK_STRUCT || Tok == TOK_ENUM ||
         Tok == TOK_MACHINE || Tok == TOK_AT || Tok == TOK_EXTERN ||
         Tok == TOK_EXPORT || Tok == TOK_OPERATOR || Tok == TOK_ERROR ||
         Tok == TOK_UNSAFE || Tok == TOK_PUB;
}

// The kinds main.cpp accepts from its "anything else" branch. Any other
// statement at the top level is a hard error there -- printed and exited
// on, never recorded -- so it is reported here as a diagnostic instead.
bool isAllowedTopLevelStatement(const ExprAST *AST) {
  return dynamic_cast<const FuncDefExprAST *>(AST) ||
         dynamic_cast<const StructDeclExprAST *>(AST) ||
         dynamic_cast<const EnumDeclExprAST *>(AST) ||
         dynamic_cast<const ModuleImportExprAST *>(AST) ||
         dynamic_cast<const ExternFuncDeclExprAST *>(AST);
}

// The entry file's top-level declarations, parsed exactly as main.cpp's
// mandatory pass parses them (leading `unsafe module;`, prelude, the
// OVH-011 self-import check, then one declaration or statement at a time),
// except that every failure is recovered from at the next top-level
// boundary instead of ending the compile.
std::vector<std::unique_ptr<ExprAST>> parseEntryFile(std::size_t &PreludeCount) {
  getNextToken();

  // Parsed before the prelude -- only here is the file's first token still
  // current -- but placed after it: preludeImportCount() promises a prefix.
  std::vector<std::unique_ptr<ExprAST>> Leading;
  std::unique_ptr<ExprAST> LeadingUnsafeBlock;
  g_recoverActive = true;
  if (setjmp(g_recoverJmp) == 0) {
    if (!ParseLeadingUnsafeModuleDecl(LeadingUnsafeBlock) && LeadingUnsafeBlock)
      AppendFlattenedTopLevelDecl(std::move(LeadingUnsafeBlock), Leading,
                                  true);
  } else {
    SkipToNextTopLevelBoundary();
  }
  g_recoverActive = false;

  std::vector<std::unique_ptr<ExprAST>> Program = ParsePreludeImports(CurLoc);
  PreludeCount = Program.size();
  for (auto &AST : Leading)
    Program.push_back(std::move(AST));

  if (ClaimEntryFileImportSlot(SourceFilename))
    return Program;

  while (CurTok != TOK_EOF) {
    if (CurTok == ';') {
      getNextToken();
      continue;
    }
    const int StartTok = CurTok;
    g_recoverActive = true;
    if (setjmp(g_recoverJmp) == 0) {
      auto AST = ParseExpression();
      if (!AST) {
        SkipToNextTopLevelBoundary();
      } else if (isDeclarationStart(StartTok)) {
        if (CurTok == ';')
          LogErrorAt(CurLoc, ErrCode::UnexpectedToken, "Unexpected ';'",
                     SLM::ErrorCategory::SYNTAX_ERROR);
        AppendFlattenedTopLevelDecl(std::move(AST), Program, true);
      } else {
        if (CurTok != ';')
          LogErrorAt(CurLoc, ErrCode::MissingSemicolon, "Expected ';' after statement",
                     SLM::ErrorCategory::SYNTAX_ERROR, MissingSemicolonHints());
        else
          getNextToken();
        if (StartTok == TOK_IMPORT || isAllowedTopLevelStatement(AST.get())) {
          RecordParsedTopLevelAST(AST.get());
          Program.push_back(std::move(AST));
        } else {
          LogErrorAt(spanOf(*AST).Loc, ErrCode::StatementOutsideFunction,
                     "Statements are not allowed outside of a function. "
                     "Please define 'fn main(): int' and move this code "
                     "inside.",
                     SLM::ErrorCategory::SYNTAX_ERROR);
        }
      }
    } else {
      SkipToNextTopLevelBoundary();
    }
    g_recoverActive = false;
  }
  return Program;
}

} // namespace

// ── Result ──────────────────────────────────────────────────────────────────

struct AnalysisResult::Impl {
  std::string EntryPath;
  std::string EntryKey;

  std::vector<std::unique_ptr<ExprAST>> Program;
  std::size_t PreludeCount = 0;
  std::unique_ptr<Resolver> R;
  std::unique_ptr<TypeChecker> TC;
  // The analysis's own GlobalSymbolTable. Nothing queries it after
  // analyze() returns, but types the TypeChecker inferred may point into
  // what it registered, so it lives as long as they do.
  std::unique_ptr<SymbolTable> Types;
  std::vector<Diagnostic> Diagnostics;
  // Module name -> the file an import of it loaded, resolved while the
  // analysis's install root was in force (see definitionOf()).
  std::unordered_map<std::string, std::string> ModuleFiles;

  // One entry per located node the Resolver reached in a buffer file.
  struct IndexedNode {
    const ExprAST *Node;
    std::string File; // fileKey()
    std::string Path; // as the lexer stamped it -- what queries report
    unsigned Line, Col, NameLen;
    std::size_t Scope; // index into Scopes
  };
  std::vector<IndexedNode> Index;
  // Variables visible at a node, as the Resolver saw them there.
  // Consecutive nodes usually share one, so each distinct run is stored
  // once.
  std::vector<std::vector<std::shared_ptr<VariableSymbol>>> Scopes;
  // A call's callee node (a bare VariableExprAST, or the ScopeAccessExprAST
  // of `m::f(...)`) -> the call. The callee is where the function's name is
  // written, but the resolved target lives on the call.
  std::unordered_map<const ExprAST *, const FuncCallExprAST *> CalleeOwner;

  const IndexedNode *nodeAt(const std::string &Path, unsigned Line,
                            unsigned Col) const {
    const std::string Key = fileKey(Path);
    const IndexedNode *Best = nullptr;
    for (const auto &N : Index) {
      if (N.NameLen == 0 || N.Line != Line || N.File != Key)
        continue;
      if (Col < N.Col || Col >= N.Col + N.NameLen)
        continue;
      if (!Best || N.Col >= Best->Col)
        Best = &N;
    }
    return Best;
  }

  std::optional<Location> functionDecl(const ResolverFunctionSymbol *F,
                                       const std::string &UseFile) const {
    if (!F || F->DeclLoc.Line <= 0)
      return std::nullopt;
    std::string File = UseFile;
    if (F->SourceModule.empty()) {
      File = EntryPath;
    } else if (auto It = ModuleFiles.find(F->SourceModule);
               It != ModuleFiles.end()) {
      File = It->second;
    }
    return Location{File, static_cast<unsigned>(F->DeclLoc.Line),
                    static_cast<unsigned>(F->DeclLoc.Col)};
  }
};

AnalysisResult::AnalysisResult(std::unique_ptr<Impl> I) : I_(std::move(I)) {}
AnalysisResult::AnalysisResult(AnalysisResult &&) noexcept = default;
AnalysisResult &AnalysisResult::operator=(AnalysisResult &&) noexcept = default;
AnalysisResult::~AnalysisResult() = default;

const std::vector<Diagnostic> &AnalysisResult::diagnostics() const {
  return I_->Diagnostics;
}

bool AnalysisResult::succeeded() const {
  for (const auto &D : I_->Diagnostics)
    if (D.Level == Severity::Error)
      return false;
  return true;
}

const TurfType *AnalysisResult::typeOf(const ExprAST *Node) const {
  if (!I_->TC || !Node)
    return nullptr;
  return I_->TC->getType(Node).get();
}

const TurfType *AnalysisResult::typeAt(const std::string &Path, unsigned Line,
                                       unsigned Col) const {
  const auto *N = I_->nodeAt(Path, Line, Col);
  if (!N)
    return nullptr;
  // A declaration's own node is typed `void` (it is a statement); the
  // declared variable's type is what a tool means by "the type here".
  if (auto *Decl = dynamic_cast<const VarDeclExprAST *>(N->Node))
    return Decl->getResolvedSymbol() ? Decl->getResolvedSymbol()->Type.get()
                                     : nullptr;
  if (const TurfType *T = typeOf(N->Node))
    return T;
  if (auto *Var = dynamic_cast<const VariableExprAST *>(N->Node))
    if (Var->getResolvedSymbol())
      return Var->getResolvedSymbol()->Type.get();
  // The name of a function being called: the call's result.
  if (auto It = I_->CalleeOwner.find(N->Node); It != I_->CalleeOwner.end())
    return typeOf(It->second);
  return nullptr;
}

std::optional<Location> AnalysisResult::definitionOf(const std::string &Path,
                                                     unsigned Line,
                                                     unsigned Col) const {
  const auto *N = I_->nodeAt(Path, Line, Col);
  if (!N)
    return std::nullopt;
  const ExprAST *Node = N->Node;

  // A declaration's definition is itself.
  if (dynamic_cast<const VarDeclExprAST *>(Node) ||
      dynamic_cast<const FuncDefExprAST *>(Node) ||
      dynamic_cast<const ExternFuncDeclExprAST *>(Node) ||
      dynamic_cast<const StructDeclExprAST *>(Node))
    return Location{N->Path, N->Line, N->Col};

  if (auto *Var = dynamic_cast<const VariableExprAST *>(Node)) {
    // A local or parameter is always declared in the file that uses it.
    if (const VariableSymbol *Sym = Var->getResolvedSymbol())
      return Location{N->Path, static_cast<unsigned>(Sym->DeclLoc.Line),
                      static_cast<unsigned>(Sym->DeclLoc.Col)};
  }
  if (auto It = I_->CalleeOwner.find(Node); It != I_->CalleeOwner.end())
    return I_->functionDecl(It->second->getResolvedCallTarget(), N->Path);
  if (auto *Call = dynamic_cast<const MethodCallExprAST *>(Node))
    return I_->functionDecl(Call->getResolvedCallTarget(), N->Path);
  return std::nullopt;
}

std::vector<SymbolInfo> AnalysisResult::symbolsInScope(const std::string &Path,
                                                       unsigned Line,
                                                       unsigned Col) const {
  const std::string Key = fileKey(Path);
  const Impl::IndexedNode *Latest = nullptr;
  for (const auto &N : I_->Index) {
    if (N.File != Key)
      continue;
    if (N.Line > Line || (N.Line == Line && N.Col > Col))
      continue;
    if (!Latest || N.Line > Latest->Line ||
        (N.Line == Latest->Line && N.Col >= Latest->Col))
      Latest = &N;
  }

  std::vector<SymbolInfo> Out;
  if (Latest) {
    for (const auto &Sym : I_->Scopes[Latest->Scope]) {
      Out.push_back({Sym->Name,
                     Sym->IsParameter ? SymbolKind::Parameter
                                      : SymbolKind::Variable,
                     Sym->Type.get(),
                     Location{Latest->Path,
                              static_cast<unsigned>(Sym->DeclLoc.Line),
                              static_cast<unsigned>(Sym->DeclLoc.Col)}});
    }
  }

  // Functions and modules are only known relative to the entry file: the
  // Resolver's "current file" IS the entry file, and an imported module's
  // own functions are reached through its name, not unqualified.
  if (Key != I_->EntryKey || !I_->R)
    return Out;
  const ResolverSymbolTable &ST = I_->R->symbolTable();
  for (const auto &Name : ST.getAllCurrentFileFunctionNames()) {
    auto Overloads = ST.lookupCurrentFileFunctions(Name);
    std::optional<Location> Decl =
        Overloads.empty() ? std::nullopt
                          : I_->functionDecl(Overloads.front(), I_->EntryPath);
    Out.push_back({Name, SymbolKind::Function, nullptr, Decl});
  }
  for (const auto &Name : ST.getAllModuleNames())
    Out.push_back({Name, SymbolKind::Module, nullptr, std::nullopt});
  return Out;
}

const std::vector<std::unique_ptr<ExprAST>> &AnalysisResult::program() const {
  return I_->Program;
}

std::size_t AnalysisResult::preludeImportCount() const {
  return I_->PreludeCount;
}

const ResolverSymbolTable &AnalysisResult::symbols() const {
  return I_->R->symbolTable();
}

// ── analyze() ───────────────────────────────────────────────────────────────

AnalysisResult analyze(const std::vector<SourceBuffer> &Buffers,
                       const AnalysisOptions &Options) {
  auto I = std::make_unique<AnalysisResult::Impl>();
  I->R = std::make_unique<Resolver>(); // symbols() is never dangling
  if (Buffers.empty())
    return AnalysisResult(std::move(I));

  std::lock_guard<std::recursive_mutex> Lock(AnalysisMutex);
  GlobalStateScope Scope;

  const SourceBuffer &Entry = Buffers.front();
  I->EntryPath = Entry.Path;
  I->EntryKey = fileKey(Entry.Path);

  g_AIEnabled = false; // a query, not a --smart compile
  g_AllowUnsafeModules = Options.AllowUnsafeModules;
  DiagnosticEngine::setCollectAllDiagnostics(true);
  // TurfInstallRoot() falls back to the running executable's location; an
  // embedder that is not the `turf` binary never set this.
  if (CompilerExecutablePath.empty())
    CompilerExecutablePath =
        llvm::sys::fs::getMainExecutable(nullptr, (void *)&fileKey);
  if (!Options.InstallRoot.empty())
    SetInstallRootOverride(Options.InstallRoot);

  std::unordered_map<std::string, bool> BufferFiles; // fileKey -> true
  for (const auto &B : Buffers) {
    SetSourceOverlay(B.Path, B.Text);
    BufferFiles[fileKey(B.Path)] = true;
  }

  std::istringstream EntryStream(Entry.Text);
  SetSourceBuffer(EntryStream);
  SourceFilename = Entry.Path;
  MainSourceFilename = Entry.Path;
  {
    std::istringstream Lines(Entry.Text);
    std::string Line;
    while (std::getline(Lines, Line))
      SourceLines.push_back(Line);
  }
  AllSourceLines[Entry.Path] = SourceLines;
  resetLexer();
  CurTok = 0;

  I->Program = parseEntryFile(I->PreludeCount);

  // The position index, filled as the Resolver reaches each node. Only
  // nodes in files the caller supplied are indexed: those are the files a
  // tool asks about, and lib/std alone is thousands of nodes.
  std::unordered_map<std::string, std::string> KeyOfFilename;
  std::vector<std::string> LastNames;
  bool HaveScope = false;
  auto IndexNode = [&](const ExprAST &Node,
                       const std::vector<std::string> &Names) {
    NodeSpanVisitor Span = spanOf(Node);
    if (!Span.Found || Span.Loc.Line <= 0)
      return;
    auto KeyIt = KeyOfFilename.find(Span.Loc.Filename);
    if (KeyIt == KeyOfFilename.end())
      KeyIt = KeyOfFilename
                  .emplace(Span.Loc.Filename, fileKey(Span.Loc.Filename))
                  .first;
    if (!BufferFiles.count(KeyIt->second))
      return;

    if (!HaveScope || Names != LastNames) {
      std::vector<std::shared_ptr<VariableSymbol>> Visible;
      for (const auto &Name : Names)
        if (auto Sym = I->R->symbolTable().lookupVariable(Name))
          Visible.push_back(std::move(Sym));
      I->Scopes.push_back(std::move(Visible));
      LastNames = Names;
      HaveScope = true;
    }
    I->Index.push_back({&Node, KeyIt->second, Span.Loc.Filename,
                        static_cast<unsigned>(Span.Loc.Line),
                        static_cast<unsigned>(Span.Loc.Col < 0 ? 0
                                                               : Span.Loc.Col),
                        Span.NameLen, I->Scopes.size() - 1});
  };
  I->R->setScopeSnapshotCallback([&](const ExprAST &Node,
                                     const std::vector<std::string> &Names) {
    IndexNode(Node, Names);
    // The Resolver looks a callee's name up without dispatching the callee
    // node as a child, so it is indexed here, from its call.
    if (auto *Call = dynamic_cast<const FuncCallExprAST *>(&Node)) {
      if (const ExprAST *Callee = Call->getCallee()) {
        I->CalleeOwner[Callee] = Call;
        IndexNode(*Callee, Names);
      }
    }
  });

  // Each pass recovers per top-level declaration, so one bad function
  // still leaves every other one resolved, typed, and queryable. A Pass-1
  // (hoisting) failure is the one thing that still aborts resolveAll()
  // whole; the TypeChecker is then skipped, as main.cpp skips it, rather
  // than run against a half-registered program.
  // volatile: assigned after a longjmp back to the setjmp below.
  volatile bool HoistingClean = true;
  g_recoverActive = true;
  if (setjmp(g_recoverJmp) == 0)
    I->R->resolveAll(I->Program, /*RecoverPerDeclaration=*/true);
  else
    HoistingClean = false;
  g_recoverActive = false;
  I->R->setScopeSnapshotCallback(nullptr);

  if (HoistingClean) {
    I->TC = std::make_unique<TypeChecker>(I->R->symbolTable());
    g_recoverActive = true;
    if (setjmp(g_recoverJmp) == 0)
      I->TC->checkAll(I->Program, /*RecoverPerDeclaration=*/true);
    g_recoverActive = false;
  }

  if (Options.RunLint) {
    for (auto &AST : I->Program) {
      g_recoverActive = true;
      if (setjmp(g_recoverJmp) == 0)
        LintExpression(AST.get());
      g_recoverActive = false;
    }
  }

  // Resolved now, while this analysis's install root is the one in force.
  for (const auto &Name : I->R->symbolTable().getAllModuleNames()) {
    std::string File = ResolveModuleFile(Name, Entry.Path);
    if (!File.empty())
      I->ModuleFiles.emplace(Name, std::move(File));
  }

  for (const auto &D : DiagnosticEngine::getDiagnostics()) {
    if (D.IsWarning && !Options.IncludeStdlibWarnings &&
        IsLibStdWarningLocation(D.Filename))
      continue;
    I->Diagnostics.push_back(toDiagnostic(D, Entry.Path));
  }

  I->Types = std::move(GlobalSymbolTable);
  return AnalysisResult(std::move(I));
}

} // namespace turf
