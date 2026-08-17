/**
 * @file CompileBuffer.cpp
 * @brief Implements CompileBufferBestEffort() -- see CompileBuffer.h.
 *
 * @author a2ys
 * @copyright Copyright (c) 2026 TurfLang contributors. Licensed under the
 * Apache License.
 */

#include "CompileBuffer.h"
#include "Parser.h"
#include "Resolver.h"
#include "TypeChecker.h"

#include <cstring>
#include <sstream>

// ExprAST has no virtual getLoc() -- each concrete node class defines its
// own (a codebase-wide convention, not a common base-class member), so
// GetVisibleNamesAtPosition() below can't just call Node.getLoc() on the
// `const ExprAST&` the Resolver's scope-snapshot callback hands it. This
// small internal visitor bridges that gap generically via the existing
// ASTVisitor double-dispatch, instead of a large dynamic_cast chain.
// visitNumber()/visitBool()/visitChar()/visitString()/visitBlock() are
// deliberately NOT overridden -- NumberExprAST, BoolExprAST, CharExprAST,
// StringExprAST, and BlockExprAST are the only node types with no
// SourceLocation field at all (a separate, pre-existing gap -- confirmed
// live via a real compile error, not assumed), so the base class's
// default no-op correctly leaves Found false for them. BlockExprAST
// specifically is a non-issue for this callback's actual use: it's only
// ever the outer bracket around statements that DO each carry their own
// location, never itself the natural "closest preceding declaration" a
// position query would want to match against.
namespace {
class LocationExtractorVisitor : public ASTVisitor {
public:
  SourceLocation Loc;
  bool Found = false;

#define TURF_EXTRACT_LOC(Method, Type)                                       \
  void Method(Type &Node) override {                                         \
    Loc = Node.getLoc();                                                     \
    Found = true;                                                            \
  }
  TURF_EXTRACT_LOC(visitNull, NullExprAST)
  TURF_EXTRACT_LOC(visitDefaultInit, DefaultInitExprAST)
  TURF_EXTRACT_LOC(visitVariable, VariableExprAST)
  TURF_EXTRACT_LOC(visitVarDecl, VarDeclExprAST)
  TURF_EXTRACT_LOC(visitBinary, BinaryExprAST)
  TURF_EXTRACT_LOC(visitUnary, UnaryExprAST)
  TURF_EXTRACT_LOC(visitAddressOf, AddressOfExprAST)
  TURF_EXTRACT_LOC(visitAssignment, AssignmentExprAST)
  TURF_EXTRACT_LOC(visitCompoundAssignment, CompoundAssignmentExprAST)
  TURF_EXTRACT_LOC(visitPostfixExpr, PostfixExprAST)
  TURF_EXTRACT_LOC(visitCast, CastExprAST)
  TURF_EXTRACT_LOC(visitIf, IfExprAST)
  TURF_EXTRACT_LOC(visitWhile, WhileExprAST)
  TURF_EXTRACT_LOC(visitDoWhile, DoWhileExprAST)
  TURF_EXTRACT_LOC(visitFor, ForExprAST)
  TURF_EXTRACT_LOC(visitForEach, ForEachExprAST)
  TURF_EXTRACT_LOC(visitBreak, BreakExprAST)
  TURF_EXTRACT_LOC(visitContinue, ContinueExprAST)
  TURF_EXTRACT_LOC(visitReturn, ReturnExprAST)
  TURF_EXTRACT_LOC(visitCompare, CompareExprAST)
  TURF_EXTRACT_LOC(visitFuncDef, FuncDefExprAST)
  TURF_EXTRACT_LOC(visitFuncCall, FuncCallExprAST)
  TURF_EXTRACT_LOC(visitMethodCall, MethodCallExprAST)
  TURF_EXTRACT_LOC(visitFieldAccess, FieldAccessExprAST)
  TURF_EXTRACT_LOC(visitEnumDecl, EnumDeclExprAST)
  TURF_EXTRACT_LOC(visitMachineDecl, MachineDeclExprAST)
  TURF_EXTRACT_LOC(visitScopeAccess, ScopeAccessExprAST)
  TURF_EXTRACT_LOC(visitErrorDecl, ErrorDeclExprAST)
  TURF_EXTRACT_LOC(visitRecoverExpr, RecoverExprAST)
  TURF_EXTRACT_LOC(visitStructDecl, StructDeclExprAST)
  TURF_EXTRACT_LOC(visitArrayDecl, ArrayDeclExprAST)
  TURF_EXTRACT_LOC(visitArrayLiteral, ArrayLiteralExprAST)
  TURF_EXTRACT_LOC(visitArrayAccess, ArrayAccessExprAST)
  TURF_EXTRACT_LOC(visitModuleImport, ModuleImportExprAST)
  TURF_EXTRACT_LOC(visitSizeOf, SizeOfExprAST)
  TURF_EXTRACT_LOC(visitExternFuncDecl, ExternFuncDeclExprAST)
#undef TURF_EXTRACT_LOC
};

// Returns {Loc, true} if Node's concrete type carries a SourceLocation,
// {{}, false} otherwise (NumberExprAST/BoolExprAST/CharExprAST/
// StringExprAST/BlockExprAST only -- see the visitor's own doc comment).
std::pair<SourceLocation, bool> GetAnyNodeLocation(const ExprAST &Node) {
  LocationExtractorVisitor V;
  const_cast<ExprAST &>(Node).accept(V);
  return {V.Loc, V.Found};
}
} // namespace

// Shared by CompileBufferBestEffort() and GetVisibleNamesAtPosition(): the
// full save-state/point-at-buffer/parse/resolve/typecheck/restore-state
// dance, parameterized only by an optional Resolver scope-snapshot callback
// (issue #244) so GetVisibleNamesAtPosition() can hook it without
// duplicating the (deliberately careful, easy to get subtly wrong) global
// save/restore logic. Returns the same result CompileBufferBestEffort()
// does; the snapshot callback (if any) does its own separate bookkeeping
// via closure capture while resolveAll() runs.
static CompileBufferResult
CompileBufferInternal(const std::string &Source, const std::string &AsIfPath,
                      Resolver::ScopeSnapshotCallback SnapshotCB) {
  // See Parser.h's doc comment -- idempotent, safe (and required) on every
  // call: an embedder must not have to separately remember to call this
  // once at process start before its first CompileBufferBestEffort() call.
  InitializeCompilerGlobals();

  // ── Save every piece of global lexer/parser state this function touches,
  // so a call here is fully isolated from -- and doesn't leak into -- any
  // other compile in the same process (another CompileBufferBestEffort()
  // call, or a real main.cpp compile / Sandbox.cpp pass that happens to be
  // mid-flight). Mirrors Sandbox.cpp's own save/restore discipline.
  const std::string SavedSourceFilename = SourceFilename;
  auto SavedSourceLines = SourceLines;
  const bool SavedAIEnabled = g_AIEnabled;
  const bool SavedCollectAll = DiagnosticEngine::getCollectAllDiagnostics();
  auto SavedDiagnostics = DiagnosticEngine::getDiagnostics();
  jmp_buf SavedJmpBuf;
  std::memcpy(SavedJmpBuf, g_recoverJmp, sizeof(jmp_buf));
  const bool SavedRecoverActive = g_recoverActive;
  auto SavedASTs = SwapParsedTopLevelASTs({});
  PushImportCycleState();

  // Point the lexer at the buffer directly -- no temp file (issue #232).
  // SourceLines is what printDiag()'s caret snippet and the SLM prompt
  // context are built from (see DiagnosticEngine::add()), so it must be
  // populated from Source the same way main.cpp populates it from a real
  // file's lines, not left stale from whatever the previous compile set it
  // to.
  std::istringstream Buffer(Source);
  SetSourceBuffer(Buffer);
  SourceFilename = AsIfPath;
  SourceLines.clear();
  {
    std::istringstream LineReader(Source);
    std::string Line;
    while (std::getline(LineReader, Line))
      SourceLines.push_back(Line);
  }
  AllSourceLines[AsIfPath] = SourceLines;

  DiagnosticEngine::clearDiagnostics();
  DiagnosticEngine::setCollectAllDiagnostics(true);
  g_AIEnabled = false; // this is a diagnostics/AST query, not a --smart compile
  resetLexer();

  std::vector<std::unique_ptr<ExprAST>> BufferASTs;
  for (auto &PreludeAST : ParsePreludeImports(CurLoc))
    BufferASTs.push_back(std::move(PreludeAST));

  // Per-statement recovery (issue #228): one syntax error resyncs to the
  // next top-level boundary and keeps going, instead of abandoning the
  // rest of the buffer -- see the identical pattern in Sandbox.cpp's
  // runParseOnlyPass()/runParseAndLintPass() for the full rationale.
  getNextToken();
  while (CurTok != TOK_EOF) {
    if (CurTok == ';') {
      getNextToken();
      continue;
    }

    g_recoverActive = true;
    if (setjmp(g_recoverJmp) == 0) {
      auto AST = ParseExpression();
      if (AST) {
        BufferASTs.push_back(std::move(AST));
        if (CurTok == ';') {
          getNextToken();
        }
      } else {
        SkipToNextTopLevelBoundary();
      }
    } else {
      SkipToNextTopLevelBoundary();
    }
  }
  g_recoverActive = false;

  // ── Resolve, then type-check -- each in its own recovery guard so an
  // error in one doesn't discard everything already resolved for the rest
  // of the buffer. No codegen: this function only ever produces
  // diagnostics + a queryable AST.
  //
  // RecoverPerDeclaration=true (issue #246): resolveAll()/checkAll() each
  // recover per TOP-LEVEL DECLARATION internally now, not just per whole
  // call -- one declaration's semantic error no longer prevents every
  // OTHER declaration in the buffer from being resolved/checked and
  // having its own errors surfaced too. The outer setjmp() here remains
  // as the safety net for a Pass-1 hoisting failure (resolveAll() never
  // recovers per-declaration for Pass 1 -- see RecoverPerDeclaration's own
  // doc comment, Resolver.h, for why) and for symmetry with checkAll(),
  // which has no separate pass to distinguish.
  Resolver R;
  if (SnapshotCB)
    R.setScopeSnapshotCallback(std::move(SnapshotCB));
  g_recoverActive = true;
  if (setjmp(g_recoverJmp) == 0) {
    R.resolveAll(BufferASTs, /*RecoverPerDeclaration=*/true);
  }
  g_recoverActive = false;

  TypeChecker TC(R.symbolTable());
  g_recoverActive = true;
  if (setjmp(g_recoverJmp) == 0) {
    TC.checkAll(BufferASTs, /*RecoverPerDeclaration=*/true);
  }
  g_recoverActive = false;

  CompileBufferResult Result;
  Result.AST = std::make_unique<BlockExprAST>(std::move(BufferASTs));
  Result.Diagnostics = DiagnosticEngine::getDiagnostics();
  Result.Clean = Result.Diagnostics.empty();

  // ── Restore everything saved above.
  CloseSourceFile();
  SourceFilename = SavedSourceFilename;
  SourceLines = SavedSourceLines;
  g_AIEnabled = SavedAIEnabled;
  DiagnosticEngine::setCollectAllDiagnostics(SavedCollectAll);
  DiagnosticEngine::restoreDiagnostics(SavedDiagnostics);
  std::memcpy(g_recoverJmp, SavedJmpBuf, sizeof(jmp_buf));
  g_recoverActive = SavedRecoverActive;
  SwapParsedTopLevelASTs(std::move(SavedASTs));
  PopImportCycleState();
  if (!SourceFilename.empty()) {
    OpenSourceFile(SourceFilename);
    resetLexer();
  }

  return Result;
}

CompileBufferResult CompileBufferBestEffort(const std::string &Source,
                                            const std::string &AsIfPath) {
  return CompileBufferInternal(Source, AsIfPath, /*SnapshotCB=*/nullptr);
}

std::vector<std::string> GetVisibleNamesAtPosition(const std::string &Source,
                                                    const std::string &AsIfPath,
                                                    int Line, int Col) {
  // Track the LATEST node visited (in Pass-2 walk order, which for a
  // top-to-bottom, non-reordering resolve walk tracks source order for the
  // common case) whose own start position is still at-or-before the target
  // (Line, Col) -- i.e. "the scope as of the most recent point in the
  // source at or before the cursor." Deliberately a simple heuristic, not
  // a precise span-containment test (#243's end-position data only covers
  // identifier/keyword tokens today, not every node) -- documented as such
  // on this function's own declaration in CompileBuffer.h.
  bool Found = false;
  int BestLine = -1, BestCol = -1;
  std::vector<std::string> BestNames;

  auto SnapshotCB = [&](const ExprAST &Node,
                       const std::vector<std::string> &VisibleNames) {
    auto [NodeLoc, HasLoc] = GetAnyNodeLocation(Node);
    if (!HasLoc)
      return;
    bool AtOrBeforeTarget = NodeLoc.Line < Line ||
                            (NodeLoc.Line == Line && NodeLoc.Col <= Col);
    if (!AtOrBeforeTarget)
      return;
    bool IsLaterThanBest = !Found || NodeLoc.Line > BestLine ||
                           (NodeLoc.Line == BestLine && NodeLoc.Col > BestCol);
    if (!IsLaterThanBest)
      return;
    Found = true;
    BestLine = NodeLoc.Line;
    BestCol = NodeLoc.Col;
    BestNames = VisibleNames;
  };

  CompileBufferInternal(Source, AsIfPath, SnapshotCB);
  return BestNames;
}
