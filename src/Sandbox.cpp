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
#include "CompileBuffer.h"
#include "Lexer.h"
#include "Lint.h"
#include "Parser.h"
#include "SLMIntegration.h"

#include <algorithm>
#include <cctype>
#include <cstring>
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

// Splits text on '\n' into individual lines, for splicing a (possibly
// multi-line) AI-suggested replacement block into the source-line vector.
// A single-line input with no embedded '\n' yields a one-element vector,
// matching the previous single-line applyFix() behavior exactly.
static std::vector<std::string> splitLines(const std::string &text) {
  std::vector<std::string> lines;
  size_t start = 0;
  while (true) {
    size_t pos = text.find('\n', start);
    if (pos == std::string::npos) {
      lines.push_back(text.substr(start));
      break;
    }
    lines.push_back(text.substr(start, pos - start));
    start = pos + 1;
  }
  return lines;
}

// Joins source lines back into a single buffer for CompileBufferBestEffort()
// (CompileBuffer.h), which takes an in-memory buffer rather than a file path
// -- no temp file needed.
static std::string joinSourceLines(const std::vector<std::string> &source) {
  std::string joined;
  for (const auto &line : source) {
    joined += line;
    joined += '\n';
  }
  return joined;
}

// Runs the real semantic pipeline (parse -> resolve -> typecheck, via
// CompileBufferBestEffort()) over `source`, then additionally runs the
// built-in Linter (shadowing/unreachable-code/void-assignment/etc.) over the
// resulting AST -- CompileBufferBestEffort() deliberately does not lint (see
// its own doc comment), so this is the one extra step needed to match what
// a real compile of this buffer would actually report.
//
// This replaces what used to be TWO separate, much shallower passes here
// (runParseOnlyPass()/runParseAndLintPass(), removed) that only ever
// re-parsed and re-linted a fix, never re-resolved or re-typechecked it --
// meaning a "fix" that remained syntactically valid but was semantically
// wrong (e.g. still didn't compile per the TypeChecker, or was a byte-for-
// byte no-op) could pass validation simply because nothing downstream of
// the parser ever ran against it. Found via a real, reproducible case: a
// SLM-proposed replacement_line identical to the original erroring line was
// reported as "validated" by the old code.
static bool runFullSemanticPass(const std::vector<std::string> &sourceLines,
                                const std::string &asIfPath,
                                std::unique_ptr<ExprAST> &parsedASTOut,
                                std::vector<DiagnosticEngine::Diag> &diagsOut) {
  CompileBufferResult Result =
      CompileBufferBestEffort(joinSourceLines(sourceLines), asIfPath);

  bool astOk = Result.AST != nullptr;
  diagsOut = std::move(Result.Diagnostics);
  parsedASTOut = std::move(Result.AST);

  // Run the built-in Linter over the same AST CompileBufferBestEffort() just
  // resolved/typechecked, so lint-level errors (e.g. void-function-result
  // assignment) are still caught -- CompileBufferBestEffort() itself never
  // lints. Guarded by the same per-call recovery pattern used everywhere
  // else in this file: LintExpression() can raise a TurfError, which
  // longjmps, and there is no other setjmp() active at this call site.
  if (parsedASTOut) {
    auto savedDiagnostics = DiagnosticEngine::getDiagnostics();
    auto savedCollectAll = DiagnosticEngine::getCollectAllDiagnostics();
    DiagnosticEngine::clearDiagnostics();
    DiagnosticEngine::setCollectAllDiagnostics(true);

    jmp_buf savedJmpBuf;
    std::memcpy(savedJmpBuf, g_recoverJmp, sizeof(jmp_buf));
    bool savedRecoverActive = g_recoverActive;
    g_recoverActive = true;
    if (setjmp(g_recoverJmp) == 0) {
      LintExpression(parsedASTOut.get());
    }
    g_recoverActive = savedRecoverActive;
    std::memcpy(g_recoverJmp, savedJmpBuf, sizeof(jmp_buf));

    auto lintDiags = DiagnosticEngine::getDiagnostics();
    DiagnosticEngine::setCollectAllDiagnostics(savedCollectAll);
    DiagnosticEngine::restoreDiagnostics(savedDiagnostics);

    diagsOut.insert(diagsOut.end(), lintDiags.begin(), lintDiags.end());
  }

  // A fix is "valid" when it produces a parseable AST with no ERROR-level
  // diagnostics. Warnings must NOT count as validation failures: they don't
  // stop a real compile (`turf file.tr` emits the binary and exits 0 with
  // warnings present), and a warning is frequently pre-existing in the
  // original source rather than introduced by the fix. Counting warnings here
  // was silently sinking otherwise-correct fixes -- found while benchmarking:
  // fail_machine_duplicate_edge.tr's correct fix (drop the duplicate edge)
  // re-parsed cleanly with zero errors but ONE warning ("Idle has no incoming
  // transitions", inherent to that machine and present in the original too),
  // and the old `diagsOut.empty()` check rejected it on that warning alone,
  // across all 3 retry attempts. This affects any category whose valid fix
  // legitimately still warns (shadowing, unreachable code, a state machine
  // with a source/sink state, ...), not just this one case. The authoritative
  // "did we make things worse" check is isOriginalErrorResolved(), which
  // already compares ERROR lines only (it filters `!IsWarning`) -- so leaving
  // warnings in diagsOut here is correct: they stay visible to that check
  // without falsely failing this one.
  bool hasError = false;
  for (const auto &d : diagsOut) {
    if (!d.IsWarning) {
      hasError = true;
      break;
    }
  }
  return astOk && !hasError;
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

  // Deliberately NOT cloning GlobalSymbolTable here (used to be an
  // unconditional cloneSymbolTable() call on every CreateSandbox()). This is
  // the only call site of captureCompilerState() (constructor -> Original-
  // Snapshot); Snapshot's own SymbolTableCopy is never populated by this
  // function at all (see the constructor -- it's left null, deferred). A
  // full SymbolTable copy-construct deep-clones the ENTIRE symbol universe
  // (every stdlib module's function registry, every declared symbol) --
  // real cost for any program importing a few stdlib modules -- yet nothing
  // in this class ever reads OriginalSnapshot.SymbolTableCopy back
  // (confirmed: no reference anywhere below, or in Sandbox.h's accessors).
  // With SLMIntegration.cpp's retry loop calling CreateSandbox() up to 3x
  // per diagnostic, that was 3x a full, unread deep clone for nothing.

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

bool Sandbox::applyFix(int startLine, int endLine, const std::string &newText) {
  // Validate the range (1-based, inclusive)
  if (startLine < 1 || endLine < startLine ||
      endLine > static_cast<int>(Snapshot.SourceCode.size())) {
    logDebug("applyFix: line range [" + std::to_string(startLine) + ", " +
             std::to_string(endLine) + "] out of bounds");
    return false;
  }

  std::vector<std::string> newLines = splitLines(newText);

  // Apply the source-level fix
  LastEditedLine = startLine;
  LastEditedLineText = newText;
  // The classify/symbol-table heuristics below only make sense for a true
  // single-line edit (one old line replaced by one new line); a genuine
  // range edit is, by definition, a structural block change.
  bool isSingleLineEdit = (startLine == endLine && newLines.size() == 1);
  LastEditKind = isSingleLineEdit ? classifyEditKind(newText)
                                 : EditKind::BLOCK_STRUCTURE;
  LocalizedValidationOnly =
      isSingleLineEdit && isLocalizedEdit(LastEditKind);
  applySourceFix(startLine, endLine, newLines);

  // Update AST and symbol table
  updateASTAfterFix(startLine);
  if (isSingleLineEdit)
    updateSymbolTableAfterFix(startLine, newText, LastEditKind);

  logDebug("Applied fix to line(s) [" + std::to_string(startLine) + ", " +
           std::to_string(endLine) + "]");
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
  
  static const std::regex arrayPattern(
      R"(^\s*(?:mut|immut)\s+([A-Za-z_][A-Za-z0-9_]*)\s*:\s*(int|double|float|bool|string)\s*\[\s*(\d+)\s*\]\s*(=|;|$))");
  if (std::regex_search(trimmed, arrayPattern))
    return EditKind::ARRAY_DECLARATION;

  static const std::regex scalarPattern(
      R"(^\s*(?:mut|immut)\s+([A-Za-z_][A-Za-z0-9_]*)\s*:\s*(int|double|float|bool|string)\s*(=|;|$))");
  if (std::regex_search(trimmed, scalarPattern))
    return EditKind::VARIABLE_DECLARATION;

  return EditKind::EXPRESSION;
}

bool Sandbox::parseDeclarationLine(const std::string &lineText,
                                   std::string &name, TurfTypePtr &type,
                                   bool &isArray, int &arraySize) {
  static const std::regex arrayPattern(
      R"(^\s*(?:mut|immut)\s+([A-Za-z_][A-Za-z0-9_]*)\s*:\s*(int|double|float|bool|string)\s*\[\s*(\d+)\s*\]\s*(=|;|$))");
  static const std::regex scalarPattern(
      R"(^\s*(?:mut|immut)\s+([A-Za-z_][A-Za-z0-9_]*)\s*:\s*(int|double|float|bool|string)\s*(=|;|$))");

  std::smatch match;
  if (std::regex_search(lineText, match, arrayPattern)) {
    name = match[1];
    std::string typeName = match[2];
    std::string sizeText = match[3];
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

  name = match[1];
  std::string typeName = match[2];
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

void Sandbox::applySourceFix(int startLine, int endLine,
                             const std::vector<std::string> &newLines) {
  // Convert from 1-based inclusive [startLine, endLine] to 0-based
  // [beginIdx, endIdx) for std::vector::erase/insert.
  size_t beginIdx = startLine - 1;
  size_t endIdx = endLine; // one past the last replaced element
  if (beginIdx >= Snapshot.SourceCode.size() ||
      endIdx > Snapshot.SourceCode.size()) {
    return;
  }
  auto it = Snapshot.SourceCode.erase(Snapshot.SourceCode.begin() + beginIdx,
                                      Snapshot.SourceCode.begin() + endIdx);
  Snapshot.SourceCode.insert(it, newLines.begin(), newLines.end());
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

  if (Symbol *S = Snapshot.SymbolTableCopy->LookupSymbol(name)) {
    S->Type = type;
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
  // (CompileBufferBestEffort() saves/restores everything it directly
  // touches itself -- see its own doc comment -- but SourceLines and the
  // diagnostics list are still ours to manage here since Snapshot.Diagnostics
  // needs to observe the sandboxed compile's diagnostics specifically).
  auto savedSourceLines = SourceLines;
  auto savedDiagnostics = DiagnosticEngine::getDiagnostics();

  SourceLines = Snapshot.SourceCode;
  DiagnosticEngine::clearDiagnostics();

  bool validationSucceeded = runFullSemanticPass(
      Snapshot.SourceCode, SourceFilename, Snapshot.AST, Snapshot.Diagnostics);

  // If validation succeeded, compute which AST nodes changed
  // This enables incremental semantic validation in future passes
  if (validationSucceeded) {
    computeChangedNodes();
  }

  // Restore global state
  SourceLines = savedSourceLines;
  // Restore diagnostics without re-triggering SLM augmentation
  DiagnosticEngine::restoreDiagnostics(savedDiagnostics);

  IsValid = validationSucceeded;

  logDebug(std::string("Validation ") +
           (validationSucceeded ? "PASSED" : "FAILED"));

  return validationSucceeded;
}

bool Sandbox::validateIncremental() {
  // This used to have a separate "fast path" for single-expression edits
  // that only re-parsed and selectively re-linted changed nodes, skipping
  // the Resolver/TypeChecker entirely on the theory that a localized edit
  // couldn't need them. That theory was wrong: it meant NO edit ever got a
  // real semantic re-check (full validate() only ran the same shallow
  // parse+lint pass anyway -- see runFullSemanticPass()'s doc comment for
  // the real bug this caused), and it could never re-validate the specific
  // error category (TYPE_MISMATCH, UNDECLARED_VAR, ...) most AI-suggested
  // fixes exist to resolve. Always doing the full, correct pass here is the
  // fix; keeping this as a separate entry point (rather than having callers
  // call validate() directly) preserves the call sites' existing intent
  // ("something changed, please tell me if it's still broken") without
  // implying a semantic shortcut that no longer exists.
  Snapshot.Diagnostics.clear();
  ChangedNodeIDs.clear();
  return validate();
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
  // NOTE: This method is currently incomplete and unused (dormant).
  // Do not rely on it as a working commit path until these TODOs are resolved,
  // as it will desync SourceLines from the global AST and symbol table.
  
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
 * Modifies the target line range in the sandboxed source, runs a full
 * semantic pass, and confirms that the original semantic error is resolved
 * without introducing new cascading errors.
 *
 * @param sandbox The active Sandbox instance.
 * @param errorLine The 1-based line the original diagnostic fired on --
 *                  used only to confirm that diagnostic is actually gone.
 * @param applyLine The 1-based first source line of the range fixedText is
 *                  patched into. Equal to errorLine in the common case;
 *                  differs when the model targets a declaration/related
 *                  line instead (see AISuggestion::target_line).
 * @param applyEndLine The 1-based last source line (inclusive) of the range
 *                  fixedText is patched into. Equal to applyLine for a
 *                  single-line edit (the common case); greater when the fix
 *                  is a multi-line block replacement (see
 *                  AISuggestion::end_line).
 * @param fixedText The replacement text for [applyLine, applyEndLine] --
 *                  may itself contain embedded '\n's for a multi-line block.
 * @param suggestion The AI suggestion metadata (confidence, fix_mode).
 * @return true If the fix is valid and resolves the error.
 * @return false If validation fails or confidence is too low.
 */
bool ApplyAndValidateFix(Sandbox &sandbox, int errorLine, int applyLine,
                         int applyEndLine, const std::string &fixedText,
                         const SLM::AISuggestion &suggestion) {
  // NOTE: there used to be a confidence gate here ("only proceed if
  // suggestion.confidence >= 0.9") that ran BEFORE the sandbox validation
  // below. That was backwards: at this point in the call chain
  // (validateAndApplySuggestion -> verifySuggestion -> here),
  // suggestion.confidence is still the PRE-sandbox structural prior computed
  // by computeStructuralPrior() in SLMIntegration.cpp -- explicitly
  // documented there as "NOT the final confidence... meant to estimate
  // plausibility" before empirical proof exists. finalizeConfidence() (also
  // in SLMIntegration.cpp) is the function that's supposed to incorporate
  // the sandbox's verdict and can promote a modest structural prior to
  // >= 0.9 once the fix is PROVEN correct -- but that promotion can never
  // happen if the sandbox is never given a chance to run in the first
  // place. Gating on confidence >= 0.9 here meant only fixes that ALREADY
  // looked near-perfect on cheap structural heuristics alone (deterministic
  // category + MODIFY + non-empty + single clean line, with no room for a
  // missed keyword-match bonus) ever reached the sandbox at all -- silently
  // discarding the sandbox's actual value proposition. The real,
  // post-sandbox confidence gate that should decide what the user sees
  // lives where it belongs: Lexer.cpp's DiagnosticEngine::add(), checking
  // `ai.confidence >= 0.8f` AFTER validateAndApplySuggestion() has already
  // run finalizeConfidence().

  // Check that fix_mode is MODIFY (not NONE)
  if (suggestion.fix_mode != "MODIFY") {
    std::cerr << Colors::BRIGHT_BLACK << "[Sandbox] " << Colors::BRIGHT_YELLOW
              << "Fix rejected: fix_mode is '" << suggestion.fix_mode
              << "', expected 'MODIFY'" << Colors::RESET << "\n";
    return false;
  }

  // Apply the fix
  if (!sandbox.applyFix(applyLine, applyEndLine, fixedText)) {
    std::cerr << Colors::BRIGHT_BLACK << "[Sandbox] " << Colors::BRIGHT_YELLOW
              << "Failed to apply fix at line(s) [" << applyLine << ", "
              << applyEndLine << "]" << Colors::RESET << "\n";
    return false;
  }

  // Validate the fix (using incremental validation for efficiency)
  if (!sandbox.validateIncremental()) {
    std::cerr << Colors::BRIGHT_BLACK << "[Sandbox] " << Colors::BRIGHT_YELLOW
              << "Validation failed after fix" << Colors::RESET << "\n";
    return false;
  }

  // Check that the original error is resolved. Deliberately keyed on
  // errorLine (not applyLine): the diagnostic fired at errorLine, so that's
  // the line that must now be clean, even when the patch itself landed
  // elsewhere (e.g. a declaration line fixing a later use-before-init read).
  if (!sandbox.isOriginalErrorResolved(errorLine)) {
    std::cerr << Colors::BRIGHT_BLACK << "[Sandbox] " << Colors::BRIGHT_YELLOW
              << "Original error still present at line " << errorLine
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
