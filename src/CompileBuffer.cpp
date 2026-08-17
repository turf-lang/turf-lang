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

CompileBufferResult CompileBufferBestEffort(const std::string &Source,
                                            const std::string &AsIfPath) {
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
  // of the buffer (matches main.cpp's own PreR/PreTC pattern). No codegen:
  // this function only ever produces diagnostics + a queryable AST.
  Resolver R;
  g_recoverActive = true;
  if (setjmp(g_recoverJmp) == 0) {
    R.resolveAll(BufferASTs);
  }
  g_recoverActive = false;

  TypeChecker TC(R.symbolTable());
  g_recoverActive = true;
  if (setjmp(g_recoverJmp) == 0) {
    TC.checkAll(BufferASTs);
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
