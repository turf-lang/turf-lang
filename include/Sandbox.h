#ifndef SANDBOX_H
#define SANDBOX_H

#include "AST.h"
#include "Lexer.h"
#include "SLMIntegration.h"
#include "SymbolTable.h"
#include "Types.h"
#include "turf/Analysis.h"
#include <memory>
#include <string>
#include <vector>

// Sandbox: A safe testing ground for SLM-driven auto-fixes
//
// The Sandbox holds a deep copy of the compiler state (source, AST, symbol
// table) and allows speculative fixes to be validated without affecting the
// main compiler environment.
//
// Workflow:
// 1. Create a Sandbox from the current main compiler state
// 2. Apply the SLM fix in the sandbox
// 3. Re-validate the sandbox (parse + semantic check)
// 4. If valid and high confidence, commit the fix to the main state
// 5. If invalid, discard the sandbox changes and fall back to original error

class Sandbox {
public:
  enum class EditKind {
    UNKNOWN,
    EXPRESSION,
    VARIABLE_DECLARATION,
    ARRAY_DECLARATION,
    FUNCTION_DEFINITION,
    BLOCK_STRUCTURE,
  };

  // Represents a snapshot of compiler state at a point in time
  struct CompilerStateSnapshot {
    std::vector<std::string> SourceCode;          // Deep copy of source lines
    std::unique_ptr<ExprAST> AST;                 // Deep copy of AST root
    std::unique_ptr<SymbolTable> SymbolTableCopy; // Deep copy of symbol table
    // The host compile's diagnostics when the sandbox was created (only
    // OriginalSnapshot's is ever filled; a validation's own go in
    // ValidationDiagnostics).
    std::vector<DiagnosticEngine::Diag> Diagnostics;

    CompilerStateSnapshot() = default;
    CompilerStateSnapshot(const CompilerStateSnapshot &) = delete;
    CompilerStateSnapshot &operator=(const CompilerStateSnapshot &) = delete;
    CompilerStateSnapshot(CompilerStateSnapshot &&) = default;
    CompilerStateSnapshot &operator=(CompilerStateSnapshot &&) = default;
  };

  // Create a sandbox by capturing the current compiler state
  explicit Sandbox(const std::vector<std::string> &SourceLines);
  ~Sandbox() = default;

  // Disable copying; sandboxes are move-only
  Sandbox(const Sandbox &) = delete;
  Sandbox &operator=(const Sandbox &) = delete;
  Sandbox(Sandbox &&) = default;
  Sandbox &operator=(Sandbox &&) = default;

  // Fix Application
  // Replace the 1-based inclusive line range [startLine, endLine] with
  // newText. newText may itself contain embedded '\n's (a multi-line block);
  // it is split on those and spliced in, so the replacement can have a
  // different number of lines than the range it replaces. For a
  // single-line edit, pass startLine == endLine.
  bool applyFix(int startLine, int endLine, const std::string &newText);

  // Validation
  // Re-validate the entire sandbox source (parse + semantic check)
  // Returns true if validation succeeds (no new errors introduced)
  // All diagnostics during validation are captured and accessible via
  // getValidationDiagnostics()
  bool validate();

  // Incremental semantic validation: Only re-check changed nodes
  // (more efficient than full re-parse for small edits)
  // Returns true if validation succeeds with no semantic errors on changed
  // nodes
  bool validateIncremental();

  // State Inspection
  // Get a reference to the modified source code
  const std::vector<std::string> &getModifiedSource() const {
    return Snapshot.SourceCode;
  }

  // Get the original source (before any fixes)
  const std::vector<std::string> &getOriginalSource() const {
    return OriginalSnapshot.SourceCode;
  }

  // Get diagnostics from the most recent validate() call
  const std::vector<turf::Diagnostic> &getValidationDiagnostics() const {
    return ValidationDiagnostics;
  }

  // Check if the original error at errorLine is still present after fix
  bool isOriginalErrorResolved(int errorLine) const;

  // Incremental Validation Support
  // Get the set of node IDs that changed during this fix
  // Used for incremental semantic validation (only re-check changed nodes)
  const std::set<uint64_t> &getChangedNodeIDs() const { return ChangedNodeIDs; }

  // Commit/Reject
  // Commit the sandbox changes back to the main compiler state
  // Should only be called after a successful validate()
  void commitToMain();

  // Discard all sandbox changes (implicit on destruction if not committed)
  void reject();

  // Diagnostics Helpers
  // Log a debug message about sandbox operations
  void logDebug(const std::string &msg) const;

private:
  // Deep copy the provided source into our snapshot
  static std::vector<std::string>
  cloneSource(const std::vector<std::string> &source);

  // Deep copy the AST (stub for now; full implementation requires
  // per-node clone methods on all ExprAST subclasses)
  static std::unique_ptr<ExprAST> cloneAST(const ExprAST *original);

  // Deep copy the symbol table
  static std::unique_ptr<SymbolTable>
  cloneSymbolTable(const SymbolTable *original);

  // Classify the edited line so we know whether a localized update is safe.
  static EditKind classifyEditKind(const std::string &lineText);

  // Extract declaration metadata from a single-line declaration.
  static bool parseDeclarationLine(const std::string &lineText,
                                   std::string &name, TurfTypePtr &type,
                                   bool &isArray, int &arraySize);

  // Update the cloned symbol table for a localized line edit.
  void updateSymbolTableForEdit(int lineNumber, const std::string &newLine,
                                EditKind kind);

  // Apply source-level fix: splice the line range [startLine, endLine]
  // (1-based, inclusive) out and insert newLines in its place.
  void applySourceFix(int startLine, int endLine,
                      const std::vector<std::string> &newLines);

  // Update AST nodes that reference the modified line (stub)
  void updateASTAfterFix(int lineNumber);

  // Update symbol table after a fix (localized line-based update)
  void updateSymbolTableAfterFix(int lineNumber, const std::string &newLine,
                                 EditKind kind);

  // Determine whether a line change is safe for localized updates only.
  static bool isLocalizedEdit(EditKind kind);

  // Capture current compiler state into target snapshot
  static void captureCompilerState(CompilerStateSnapshot &target);

  // AST Diffing: Compare original and modified ASTs to find changed nodes
  // This enables incremental semantic validation (only re-check changed nodes)
  void computeChangedNodes();

  CompilerStateSnapshot OriginalSnapshot; // Original state (for rollback)
  CompilerStateSnapshot Snapshot;         // Current state (may be modified)
  EditKind LastEditKind = EditKind::UNKNOWN;
  int LastEditedLine = 0;
  std::string LastEditedLineText;
  bool LocalizedValidationOnly = false;
  std::vector<turf::Diagnostic> ValidationDiagnostics; // last validate()'s
  std::set<uint64_t> ChangedNodeIDs; // Nodes that changed during fix (for
                                     // incremental validation)
  bool IsValid = false;              // Last validation result
  bool IsCommitted = false; // Whether changes have been committed to main state
};

// Global Sandbox Management

// Create a sandbox from the current main compiler state
std::unique_ptr<Sandbox> CreateSandbox();

// Validate and optionally commit an SLM fix in a sandbox.
// `errorLine` is the line the original diagnostic fired on -- used only to
// check that diagnostic is actually gone afterward (isOriginalErrorResolved).
// `applyLine`/`applyEndLine` are the 1-based inclusive line range fixedText
// gets patched into -- normally both equal errorLine (a single-line edit),
// but may differ when suggestion.target_line/end_line point at a
// declaration/related site or a multi-line block instead (see
// AISuggestion::target_line and AISuggestion::end_line).
// Returns true if the fix was applied and committed successfully
bool ApplyAndValidateFix(Sandbox &sandbox, int errorLine, int applyLine,
                         int applyEndLine, const std::string &fixedText,
                         const SLM::AISuggestion &suggestion);

#endif
