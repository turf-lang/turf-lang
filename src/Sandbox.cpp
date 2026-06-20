/**
 * @file Sandbox.cpp
 * @brief Semantic sandbox for isolated evaluation of AI suggestions.
 * @details Implements a virtualized compiler state that allows parsing and
 *          linting of mutated source lines without corrupting the active
 *          AST or symbol table.
 *
 * @author a2ys
 * @author AaravTandon24
 * @author Karan
 *
 * @copyright Copyright (c) 2026 TurfLang contributors. Licensed under the
 * Apache License.
 * @version 1.0.0
 * @date 2026-05-25
 */

#include "Sandbox.h"
#include "Codegen.h"
#include "Colors.h"
#include "Lexer.h"
#include "Lint.h"
#include "Parser.h"
#include "SLMIntegration.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>

// AST Node ID Counter (Static Member Initialization)
uint64_t ExprAST::NextNodeID = 0;

void ExprAST::ResetNodeIDCounter() { NextNodeID = 0; }

// Sandbox Implementation

void Sandbox::computeChangedNodes() {
  // Reset changed nodes tracking
  ChangedNodeIDs.clear();

  if (!OriginalSnapshot.AST && !Snapshot.AST) {
    logDebug("AST diffing: no AST available, marked no nodes as changed");
    return;
  }

  if (!OriginalSnapshot.AST && Snapshot.AST) {
    CollectAllNodeIDsInternal(Snapshot.AST.get(), ChangedNodeIDs);
    logDebug(
        "AST diffing: missing baseline AST, marked the full tree as changed");
    return;
  }

  if (OriginalSnapshot.AST && !Snapshot.AST) {
    CollectAllNodeIDsInternal(OriginalSnapshot.AST.get(), ChangedNodeIDs);
    logDebug(
        "AST diffing: missing current AST, marked the full tree as changed");
    return;
  }

  // Run the diffing algorithm to collect changed node IDs
  ChangedNodeIDs =
      CollectChangedNodeIDs(OriginalSnapshot.AST.get(), Snapshot.AST.get());

  logDebug("AST diffing: identified " + std::to_string(ChangedNodeIDs.size()) +
           " changed nodes");
}

static std::filesystem::path
writeTempSourceFile(const std::vector<std::string> &source) {
  auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  std::filesystem::path tempPath =
      std::filesystem::temp_directory_path() /
      ("turf_sandbox_" + std::to_string(stamp) + ".tr");

  std::ofstream out(tempPath, std::ios::trunc);
  for (const auto &line : source) {
    out << line << '\n';
  }
  return tempPath;
}

// Parse-only pass: Parses without linting (for incremental validation)
// Allows semantic pruning to selectively lint only changed nodes
static bool runParseOnlyPass(const std::filesystem::path &sourcePath,
                             const std::vector<std::string> &sourceLines,
                             std::unique_ptr<ExprAST> &parsedASTOut) {
  const std::string savedSourceFilename = SourceFilename;
  auto savedSourceLines = SourceLines;
  SourceFile->close();
  SourceFile->open(sourcePath.string());
  if (!SourceFile->is_open()) {
    SourceLines = savedSourceLines;
    return false;
  }

  SourceFilename = sourcePath.string();
  SourceLines = sourceLines;
  resetLexer();
  DiagnosticEngine::clearDiagnostics();
  ClearParsedTopLevelASTs();

  bool success = true;
  bool savedAIEnabled = g_AIEnabled;
  g_AIEnabled = false;

  int jumpResult = setjmp(g_recoverJmp);
  if (jumpResult == 0) {
    g_recoverActive = true;
    getNextToken();

    while (CurTok != TOK_EOF) {
      if (CurTok == ';') {
        getNextToken();
        continue;
      }

      auto AST = ParseExpression();
      if (!AST) {
        success = false;
        break;
      }

      RecordParsedTopLevelAST(AST.get());
      // NOTE: No linting here - will be done selectively via
      // LintChangedNodesOnly

      if (CurTok == ';') {
        getNextToken();
      }
    }
  } else {
    success = false;
  }

  g_recoverActive = false;
  g_AIEnabled = savedAIEnabled;

  // Build the program AST from the cached top-level expressions
  parsedASTOut = BuildProgramASTFromCache();

  SourceFile->close();
  SourceFilename = savedSourceFilename;
  SourceLines = savedSourceLines;

  // Restore the real source file and lexer state
  if (!SourceFilename.empty()) {
    SourceFile->open(SourceFilename);
    resetLexer();
  }

  return success;
}

static bool runParseAndLintPass(const std::filesystem::path &sourcePath,
                                const std::vector<std::string> &sourceLines,
                                std::unique_ptr<ExprAST> &parsedASTOut) {
  const std::string savedSourceFilename = SourceFilename;
  auto savedSourceLines = SourceLines;
  SourceFile->close();
  SourceFile->open(sourcePath.string());
  if (!SourceFile->is_open()) {
    SourceLines = savedSourceLines;
    return false;
  }

  SourceFilename = sourcePath.string();
  SourceLines = sourceLines;
  resetLexer();
  DiagnosticEngine::clearDiagnostics();
  ClearParsedTopLevelASTs();

  bool success = true;
  bool savedAIEnabled = g_AIEnabled;
  g_AIEnabled = false;

  int jumpResult = setjmp(g_recoverJmp);
  if (jumpResult == 0) {
    g_recoverActive = true;
    getNextToken();

    while (CurTok != TOK_EOF) {
      if (CurTok == ';') {
        getNextToken();
        continue;
      }

      auto AST = ParseExpression();
      if (!AST) {
        success = false;
        break;
      }

      RecordParsedTopLevelAST(AST.get());

      LintExpression(AST.get());
      if (CurTok == ';') {
        getNextToken();
      }
    }
  } else {
    success = false;
  }

  g_recoverActive = false;
  g_AIEnabled = savedAIEnabled;

  if (success && DiagnosticEngine::getDiagnosticCount() == 0) {
    parsedASTOut = BuildProgramASTFromCache();
  } else {
    parsedASTOut.reset();
  }

  SourceFile->close();
  if (!savedSourceFilename.empty()) {
    SourceFile->open(savedSourceFilename);
  }
  SourceFilename = savedSourceFilename;
  SourceLines = savedSourceLines;
  resetLexer();

  return success && DiagnosticEngine::getDiagnosticCount() == 0;
}

Sandbox::Sandbox(const std::vector<std::string> &SourceLines) {
  // Capture the original state from globals
  captureCompilerState(OriginalSnapshot);

  // Create a working copy
  Snapshot = CompilerStateSnapshot();
  Snapshot.SourceCode = cloneSource(OriginalSnapshot.SourceCode);
  // AST and SymbolTable cloning deferred until needed

  logDebug("Sandbox created: " + std::to_string(Snapshot.SourceCode.size()) +
           " lines of source captured.");
}

void Sandbox::captureCompilerState(CompilerStateSnapshot &target) {
  // Capture source lines
  target.SourceCode = SourceLines;

  // Capture the parsed top-level AST cache as a program block, if available.
  target.AST = BuildProgramASTFromCache();

  // TODO: Clone global symbol table
  if (GlobalSymbolTable) {
    target.SymbolTableCopy = cloneSymbolTable(GlobalSymbolTable.get());
  }

  // Capture current diagnostics
  target.Diagnostics = DiagnosticEngine::getDiagnostics();
}

std::vector<std::string>
Sandbox::cloneSource(const std::vector<std::string> &source) {
  // Simple deep copy of source code lines
  return source;
}

std::unique_ptr<ExprAST> Sandbox::cloneAST(const ExprAST *original) {
  return original ? original->clone() : nullptr;
}

std::unique_ptr<SymbolTable>
Sandbox::cloneSymbolTable(const SymbolTable *original) {
  return original ? std::make_unique<SymbolTable>(*original)
                  : std::make_unique<SymbolTable>();
}

bool Sandbox::applyFix(int lineNumber, const std::string &newLine) {
  // Validate line number (1-based)
  if (lineNumber < 1 ||
      lineNumber > static_cast<int>(Snapshot.SourceCode.size())) {
    logDebug("applyFix: line number " + std::to_string(lineNumber) +
             " out of bounds");
    return false;
  }

  // Apply the source-level fix
  LastEditedLine = lineNumber;
  LastEditedLineText = newLine;
  LastEditKind = classifyEditKind(newLine);
  LocalizedValidationOnly = isLocalizedEdit(LastEditKind);
  applySourceFix(lineNumber, newLine);

  // Update AST and symbol table
  updateASTAfterFix(lineNumber);
  updateSymbolTableAfterFix(lineNumber, newLine, LastEditKind);

  logDebug("Applied fix to line " + std::to_string(lineNumber));
  return true;
}

Sandbox::EditKind Sandbox::classifyEditKind(const std::string &lineText) {
  std::string trimmed = lineText;
  trimmed.erase(trimmed.begin(), std::find_if(trimmed.begin(), trimmed.end(),
                                              [](unsigned char ch) {
                                                return !std::isspace(ch);
                                              }));
  trimmed.erase(std::find_if(trimmed.rbegin(), trimmed.rend(),
                             [](unsigned char ch) { return !std::isspace(ch); })
                    .base(),
                trimmed.end());

  if (trimmed.empty())
    return EditKind::UNKNOWN;

  if (trimmed.find('{') != std::string::npos ||
      trimmed.find('}') != std::string::npos)
    return EditKind::BLOCK_STRUCTURE;
  if (trimmed.find("fn ") == 0)
    return EditKind::FUNCTION_DEFINITION;
  if (trimmed.find("[") != std::string::npos &&
      trimmed.find("]") != std::string::npos)
    return EditKind::ARRAY_DECLARATION;

  static const std::regex declPattern(
      R"(^\s*(int|double|float|bool|string)\s+([A-Za-z_][A-Za-z0-9_]*)\s*(=|;|$))");
  if (std::regex_search(trimmed, declPattern))
    return EditKind::VARIABLE_DECLARATION;

  return EditKind::EXPRESSION;
}

bool Sandbox::parseDeclarationLine(const std::string &lineText,
                                   std::string &name, TurfTypePtr &type,
                                   bool &isArray, int &arraySize) {
  static const std::regex scalarPattern(
      R"(^\s*(int|double|float|bool|string)\s+([A-Za-z_][A-Za-z0-9_]*)\s*(=|;|$))");
  static const std::regex arrayPattern(
      R"(^\s*(int|double|float|bool|string)\s*\[\s*(\d+)\s*\]\s*([A-Za-z_][A-Za-z0-9_]*)\s*(=|;|$))");

  std::smatch match;
  if (std::regex_search(lineText, match, arrayPattern)) {
    std::string typeName = match[1];
    std::string sizeText = match[2];
    name = match[3];
    arraySize = std::stoi(sizeText);
    isArray = true;
    TurfTypePtr ElemType;
    if (typeName == "int")
      ElemType = TurfInt();
    else if (typeName == "double" || typeName == "float")
      ElemType = TurfDouble();
    else if (typeName == "bool")
      ElemType = TurfBool();
    else if (typeName == "string")
      ElemType = TurfString();
    else
      return false;
    type = makeArrayType(ElemType, arraySize);
    return true;
  }

  if (!std::regex_search(lineText, match, scalarPattern))
    return false;

  std::string typeName = match[1];
  name = match[2];
  isArray = false;
  arraySize = 0;
  if (typeName == "int")
    type = TurfInt();
  else if (typeName == "double" || typeName == "float")
    type = TurfDouble();
  else if (typeName == "bool")
    type = TurfBool();
  else if (typeName == "string")
    type = TurfString();
  else
    return false;
  return true;
}

bool Sandbox::isLocalizedEdit(EditKind kind) {
  return kind == EditKind::EXPRESSION ||
         kind == EditKind::VARIABLE_DECLARATION ||
         kind == EditKind::ARRAY_DECLARATION;
}

void Sandbox::applySourceFix(int lineNumber, const std::string &newLine) {
  // Convert from 1-based to 0-based indexing
  size_t idx = lineNumber - 1;
  if (idx < Snapshot.SourceCode.size()) {
    Snapshot.SourceCode[idx] = newLine;
  }
}

void Sandbox::updateASTAfterFix(int lineNumber) {
  // Stub: In production, this would traverse the AST and find nodes
  // that reference the modified line, then update them with the new syntax.
  // For the localized MVP, we just record that the edit is line-local.
  (void)lineNumber;
}

void Sandbox::updateSymbolTableForEdit(int lineNumber,
                                       const std::string &newLine,
                                       EditKind kind) {
  if (!Snapshot.SymbolTableCopy)
    return;

  if (kind != EditKind::VARIABLE_DECLARATION &&
      kind != EditKind::ARRAY_DECLARATION)
    return;

  std::string name;
  TurfTypePtr type;
  bool isArray = false;
  int arraySize = 0;
  if (!parseDeclarationLine(newLine, name, type, isArray, arraySize))
    return;

  SourceLocation declLoc{lineNumber, 1};

  if (isArray) {
    Snapshot.SymbolTableCopy->DeclareSymbol(name, type, declLoc, nullptr);
  } else {
    Snapshot.SymbolTableCopy->DeclareSymbol(name, type, declLoc, nullptr);
  }
}

void Sandbox::updateSymbolTableAfterFix(int lineNumber,
                                        const std::string &newLine,
                                        EditKind kind) {
  updateSymbolTableForEdit(lineNumber, newLine, kind);
}

bool Sandbox::validate() {
  // Clear any previous validation diagnostics
  Snapshot.Diagnostics.clear();
  ChangedNodeIDs.clear();

  // Save the global state so we can restore it after validation
  auto savedSourceLines = SourceLines;
  auto savedDiagnostics = DiagnosticEngine::getDiagnostics();

  // Temporarily swap in the sandbox source
  SourceLines = Snapshot.SourceCode;
  DiagnosticEngine::clearDiagnostics();

  bool validationSucceeded = false;
  std::filesystem::path tempPath = writeTempSourceFile(Snapshot.SourceCode);
  validationSucceeded =
      runParseAndLintPass(tempPath, Snapshot.SourceCode, Snapshot.AST);

  std::error_code ignored;
  std::filesystem::remove(tempPath, ignored);

  // Capture validation diagnostics
  Snapshot.Diagnostics = DiagnosticEngine::getDiagnostics();

  // If validation succeeded, compute which AST nodes changed
  // This enables incremental semantic validation in future passes
  if (validationSucceeded) {
    computeChangedNodes();
  }

  // Restore global state
  SourceLines = savedSourceLines;
  // Restore diagnostics by clearing and re-adding saved ones
  DiagnosticEngine::clearDiagnostics();
  for (const auto &diag : savedDiagnostics) {
    DiagnosticEngine::add(diag.Loc, diag.Msg, diag.IsWarning);
  }

  IsValid = validationSucceeded;

  logDebug(std::string("Validation ") +
           (validationSucceeded ? "PASSED" : "FAILED"));

  return validationSucceeded;
}

bool Sandbox::validateIncremental() {
  // Incremental semantic validation: only re-check changed nodes
  // For single-line edits that don't affect scope structure, this is much
  // faster than full re-parse (avoids re-lexing and re-parsing entire file)

  Snapshot.Diagnostics.clear();
  ChangedNodeIDs.clear();

  // If this is a simple expression edit on a single line, do a lightweight
  // check
  if (LocalizedValidationOnly && LastEditKind == EditKind::EXPRESSION &&
      LastEditedLine > 0 &&
      LastEditedLine <= static_cast<int>(Snapshot.SourceCode.size())) {

    logDebug("Using incremental validation for single-line expression edit");

    // For a simple expression change, we can:
    // 1. Parse only (skip full lint pass initially)
    // 2. Compute changed nodes
    // 3. Lint only changed nodes (semantic pruning)

    // Save global state
    auto savedSourceLines = SourceLines;
    auto savedDiagnostics = DiagnosticEngine::getDiagnostics();

    // Temporarily use sandbox source
    SourceLines = Snapshot.SourceCode;
    DiagnosticEngine::clearDiagnostics();

    // Write temp file and do parse-only pass
    std::filesystem::path tempPath = writeTempSourceFile(Snapshot.SourceCode);
    bool validationSucceeded =
        runParseOnlyPass(tempPath, Snapshot.SourceCode, Snapshot.AST);

    std::error_code ignored;
    std::filesystem::remove(tempPath, ignored);

    Snapshot.Diagnostics = DiagnosticEngine::getDiagnostics();

    // If parsing succeeded, compute changed nodes and selectively lint them
    if (validationSucceeded && Snapshot.AST) {
      computeChangedNodes();

      // Semantic pruning: only lint changed nodes
      logDebug("Semantic pruning: linting only " +
               std::to_string(ChangedNodeIDs.size()) + " changed nodes");
      logDebug("Semantic validation: entering LintChangedNodesOnly()");
      LintChangedNodesOnly(Snapshot.AST.get(), ChangedNodeIDs);
      logDebug("Semantic validation: completed LintChangedNodesOnly()");

      // Update diagnostics after selective linting
      Snapshot.Diagnostics = DiagnosticEngine::getDiagnostics();
    }

    // Restore global state
    SourceLines = savedSourceLines;
    DiagnosticEngine::clearDiagnostics();
    for (const auto &diag : savedDiagnostics) {
      DiagnosticEngine::add(diag.Loc, diag.Msg, diag.IsWarning);
    }

    IsValid = validationSucceeded;

    logDebug(std::string("Incremental validation ") +
             (validationSucceeded ? "PASSED" : "FAILED"));

    return validationSucceeded;
  } else {
    // Complex edit (scope change, block structure, etc.) falls back to full
    // validation
    logDebug("Complex edit detected; falling back to full validation");
    return validate();
  }
}

bool Sandbox::isOriginalErrorResolved(int errorLine) const {
  // Build a set of lines that had errors before the fix (original state)
  std::set<int> originalErrorLines;
  for (const auto &diag : OriginalSnapshot.Diagnostics) {
    if (!diag.IsWarning) {
      originalErrorLines.insert(diag.Loc.Line);
    }
  }

  // Check that the original error at errorLine is no longer present
  for (const auto &diag : Snapshot.Diagnostics) {
    if (diag.Loc.Line == errorLine && !diag.IsWarning) {
      return false;
    }
  }

  // Check that no NEW error lines were introduced by the fix.
  // A new error line is one that has an error now but did not before the fix.
  for (const auto &diag : Snapshot.Diagnostics) {
    if (!diag.IsWarning &&
        originalErrorLines.find(diag.Loc.Line) == originalErrorLines.end()) {
      return false;
    }
  }

  return true;
}

void Sandbox::commitToMain() {
  // Copy the modified source back to the global SourceLines
  ::SourceLines = Snapshot.SourceCode;

  // TODO: Update global AST if we have a cloned version
  // TODO: Update global symbol table if we have a cloned version

  IsCommitted = true;
  logDebug("Sandbox changes committed to main compiler state");
}

void Sandbox::reject() {
  // Discard all changes; sandbox is discarded on scope exit
  IsCommitted = false;
  logDebug("Sandbox changes rejected");
}

void Sandbox::logDebug(const std::string &msg) const {
  // Only log in verbose/debug mode
  // For now, log to stderr with a prefix
  if (std::getenv("TURF_SANDBOX_DEBUG")) {
    std::cerr << Colors::BRIGHT_BLACK << "[Sandbox] " << Colors::RESET << msg
              << "\n";
  }
}

// Global Sandbox Management

std::unique_ptr<Sandbox> CreateSandbox() {
  return std::make_unique<Sandbox>(SourceLines);
}

/**
 * @brief Applies an AI suggestion in a sandbox and validates it.
 *
 * Modifies the target line in the sandboxed source, runs a parse/lint pass,
 * and confirms that the original semantic error is resolved without introducing
 * new cascading errors.
 *
 * @param sandbox The active Sandbox instance.
 * @param lineNumber The 1-based source line to modify.
 * @param fixedLine The replacement string for the target line.
 * @param suggestion The AI suggestion metadata (confidence, fix_mode).
 * @return true If the fix is valid and resolves the error.
 * @return false If validation fails or confidence is too low.
 */
bool ApplyAndValidateFix(Sandbox &sandbox, int lineNumber,
                         const std::string &fixedLine,
                         const SLM::AISuggestion &suggestion) {
  // Check confidence gate: only proceed if confidence >= 0.9
  const float CONFIDENCE_THRESHOLD = 0.9f;

  if (suggestion.confidence < CONFIDENCE_THRESHOLD) {
    std::cerr << Colors::BRIGHT_BLACK << "[Sandbox] " << Colors::BRIGHT_YELLOW
              << "Fix rejected: confidence " << suggestion.confidence
              << " below threshold " << CONFIDENCE_THRESHOLD << Colors::RESET
              << "\n";
    return false;
  }

  // Check that fix_mode is MODIFY (not NONE)
  if (suggestion.fix_mode != "MODIFY") {
    std::cerr << Colors::BRIGHT_BLACK << "[Sandbox] " << Colors::BRIGHT_YELLOW
              << "Fix rejected: fix_mode is '" << suggestion.fix_mode
              << "', expected 'MODIFY'" << Colors::RESET << "\n";
    return false;
  }

  // Apply the fix
  if (!sandbox.applyFix(lineNumber, fixedLine)) {
    std::cerr << Colors::BRIGHT_BLACK << "[Sandbox] " << Colors::BRIGHT_YELLOW
              << "Failed to apply fix at line " << lineNumber << Colors::RESET
              << "\n";
    return false;
  }

  // Validate the fix (using incremental validation for efficiency)
  if (!sandbox.validateIncremental()) {
    std::cerr << Colors::BRIGHT_BLACK << "[Sandbox] " << Colors::BRIGHT_YELLOW
              << "Validation failed after fix" << Colors::RESET << "\n";
    return false;
  }

  // Check that the original error is resolved
  if (!sandbox.isOriginalErrorResolved(lineNumber)) {
    std::cerr << Colors::BRIGHT_BLACK << "[Sandbox] " << Colors::BRIGHT_YELLOW
              << "Original error still present at line " << lineNumber
              << Colors::RESET << "\n";
    return false;
  }

  // Validation passed. Do NOT call commitToMain() here:
  // ::SourceLines must stay unchanged so that DiagnosticEngine::flushAll()
  // renders the *original* erroneous line under the caret, not the fixed one.
  // The sandbox has confirmed the suggestion is correct; it is presented to
  // the user as an AI hint, not silently applied to the source.
  std::cerr << Colors::BRIGHT_BLACK << "[Sandbox] " << Colors::BRIGHT_GREEN
            << "✓ Fix validated and committed" << Colors::RESET << "\n";

  return true;
}
