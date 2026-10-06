/**
 * @file turf/Analysis.h
 * @brief The compiler query facade (SMP-001): the one supported way for a
 *        tool to run the front end over source text and ask questions
 *        about the result.
 * @details `turf::analyze()` runs Lexer -> Parser -> Resolver -> TypeChecker
 *          (never Codegen) over a set of in-memory buffers and hands back an
 *          `AnalysisResult` that owns everything the analysis produced.
 *
 *          The front end is built on process-wide mutable globals (the
 *          lexer cursor, `GlobalSymbolTable`, the diagnostic list, the
 *          import caches, the setjmp recovery buffer -- see
 *          docs/planning/ARCHITECTURE_SIMPLIFICATION.md §1). Every tool that
 *          drove the pipeline directly had to save and restore those around
 *          each run, and every tool got a different subset wrong: turf-lsp
 *          stopped building at all, and three pieces of per-compile state
 *          were never reset by anyone (see docs/development/changes/
 *          SMP-001.md, "Removed assumptions"). That save/restore now lives
 *          in exactly one place -- this facade's implementation
 *          (src/AnalysisFacade.cpp) -- so a refactor of the pipeline either
 *          updates it or fails tests/api/, instead of silently breaking an
 *          out-of-tree tool.
 *
 *          Stability (ROADMAP_v2.0.0.md, SMP-001 completion criterion): the
 *          declarations in this header are the 2.x tool API, semver-stable
 *          for 2.x, EXCEPT the members grouped under "Internal
 *          representation" in AnalysisResult, which expose the compiler's
 *          own AST and symbol-table types and change whenever those do.
 *
 *          Threading: analyses are serialized by an internal mutex (one at a
 *          time per process -- the globals underneath are not per-thread).
 *          Calling analyze() from inside another analyze() on the same
 *          thread is safe; the inner call restores exactly what it found.
 *          An AnalysisResult is immutable and needs no global state after
 *          analyze() returns, so its queries may run on any thread,
 *          concurrently with each other and with a later analyze().
 *
 * @author a2ys
 * @copyright Copyright (c) 2026 TurfLang contributors. Licensed under the
 * Apache License.
 */

#ifndef TURF_ANALYSIS_H
#define TURF_ANALYSIS_H

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class ExprAST;
class TurfType;
class ResolverSymbolTable;

namespace turf {

// One source file's text. `Path` is what the file is called for import
// resolution and in every location this API reports; the text itself is
// never read from disk when a buffer supplies it.
struct SourceBuffer {
  std::string Path;
  std::string Text;
};

struct AnalysisOptions {
  // Honor `unsafe module;` in files outside the install root -- the CLI's
  // --allow-unsafe-modules.
  bool AllowUnsafeModules = false;
  // Report warnings anchored inside lib/std. Off by default for the same
  // reason the CLI's --stdlib-warnings is (OVH-012): the user did not write
  // that code and cannot fix it.
  bool IncludeStdlibWarnings = false;
  // Also run the built-in linter over the analysed program (shadowing,
  // unreachable code, void-result assignment, ...). A real compile lints
  // during Codegen, which this facade never runs, so lint findings are
  // absent unless asked for here.
  bool RunLint = false;
  // The TurfLang installation root lib/std is found under. Empty means what
  // the CLI does: $TURF_HOME, else the directory above the running
  // executable.
  std::string InstallRoot;
  // Asked at the two boundaries the pipeline already has: after parsing and
  // after resolving. When it returns true the analysis stops there and the
  // result reports cancelled(). Meant for an editor, where the text a
  // keystroke produced is already out of date by the time a slow analysis of
  // it finishes. Empty (the default) is never called. It runs on the thread
  // that called analyze(), inside the analysis, so it must be quick and must
  // not call analyze() itself.
  std::function<bool()> ShouldCancel;
};

// A 1-based position in a named file -- the compiler's own convention
// everywhere (not the 0-based lines of the Language Server Protocol).
// Col counts bytes.
struct Location {
  std::string Path;
  unsigned Line = 0;
  unsigned Col = 0;

  bool operator==(const Location &O) const {
    return Path == O.Path && Line == O.Line && Col == O.Col;
  }
};

struct RelatedInformation {
  Location Where;
  std::string Message;
};

enum class Severity { Error, Warning };

struct Diagnostic {
  Severity Level = Severity::Error;
  // Stable "E0NN" code (ErrorCodes.h), or empty for an untyped diagnostic.
  std::string Code;
  // Plain text: the CLI's ANSI color escapes are stripped.
  std::string Message;
  std::string Hint;
  Location Where;
  std::vector<RelatedInformation> Related;
};

enum class SymbolKind { Variable, Parameter, Function, Module };

struct SymbolInfo {
  std::string Name;
  SymbolKind Kind = SymbolKind::Variable;
  // Null for a module, or when the type is not known.
  const TurfType *Type = nullptr;
  // Where it is declared; unset for a module.
  std::optional<Location> Declaration;
};

class AnalysisResult {
public:
  AnalysisResult(AnalysisResult &&) noexcept;
  AnalysisResult &operator=(AnalysisResult &&) noexcept;
  ~AnalysisResult();

  // Every diagnostic, in the order the passes produced it. All of them --
  // not the CLI's one-error-per-line cap.
  const std::vector<Diagnostic> &diagnostics() const;

  // True when no diagnostic is an error.
  bool succeeded() const;

  // True when AnalysisOptions::ShouldCancel stopped the analysis. Such a
  // result is empty, not partial: no diagnostics (so succeeded() is true --
  // check this first), every query answers nothing, program() is empty.
  bool cancelled() const;

  // The type of the expression whose identifier covers (Line, Col): a
  // variable use or declaration, a field access, or a call's result.
  // Null when nothing typed is there. Owned by this result.
  const TurfType *typeAt(const std::string &Path, unsigned Line,
                         unsigned Col) const;

  // Where the name covering (Line, Col) is declared: a variable, parameter,
  // or a function / method (including one in an imported module).
  std::optional<Location> definitionOf(const std::string &Path, unsigned Line,
                                       unsigned Col) const;

  // The names usable unqualified at (Line, Col): the variables and
  // parameters in scope, then (in the entry file) its own functions and
  // the modules it imported. Positions are matched against the nearest
  // analysed construct at or before (Line, Col).
  std::vector<SymbolInfo> symbolsInScope(const std::string &Path,
                                         unsigned Line, unsigned Col) const;

  // ── Internal representation ─────────────────────────────────────────────
  // NOT covered by the 2.x stability promise: these are the compiler's own
  // types and change with it. For tools that need more than the queries
  // above (semantic highlighting, completion over a module's members).

  // The entry file's top-level declarations, preceded by
  // preludeImportCount() synthetic `import int; ...` nodes that exist in
  // no source text.
  const std::vector<std::unique_ptr<ExprAST>> &program() const;
  std::size_t preludeImportCount() const;
  // The Resolver's symbol table. Scope-sensitive lookups on it reflect the
  // state at the end of resolution (the global scope), not any position.
  const ResolverSymbolTable &symbols() const;
  // The TypeChecker's inferred type for any node in program(); null when
  // it inferred none.
  const TurfType *typeOf(const ExprAST *Node) const;

  struct Impl;

private:
  friend AnalysisResult analyze(const std::vector<SourceBuffer> &,
                                const AnalysisOptions &);
  explicit AnalysisResult(std::unique_ptr<Impl> I);
  std::unique_ptr<Impl> I_;
};

// Analyses Buffers[0] as the program's entry file. Every other buffer
// replaces the on-disk text of the file with the same path whenever an
// import loads that file -- an editor's unsaved changes to a module. (An
// import still has to resolve to a file that exists on disk; a buffer
// alone does not make a new module importable.) Buffers must not be empty.
//
// A problem in the analysed source -- a syntax error, a missing import, a
// type error -- becomes a diagnostic; it never ends the process. Every
// piece of global compiler state is left exactly as analyze() found it.
AnalysisResult analyze(const std::vector<SourceBuffer> &Buffers,
                       const AnalysisOptions &Options = {});

} // namespace turf

#endif
