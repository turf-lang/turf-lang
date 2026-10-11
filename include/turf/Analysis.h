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
  // Also run the flow checks a real compile runs on each function's
  // control-flow graph during Codegen (runFunctionFlowPasses(), Analysis.h):
  // missing return, unreachable code, use before assignment, double free
  // (E017), use after free (E018), unchecked nullable references, escaping
  // references, unused variables and dead stores, recursion with no base
  // case, constructors that miss a field. Like RunLint, it covers the
  // analysed file's own functions, not those of the modules it imports.
  // As in a compile, a generic function is not checked (only its
  // instantiations are, and an analysis makes none), and neither is a
  // function that already has an error: a compile never gets this far with
  // one, and its flow would be judged on a half-checked body.
  bool RunFlowAnalysis = false;
  // Do not auto-import the prelude (lib/std/core: io, math, sys and the
  // primitive types' methods), so bare `printline` or `x.abs()` no longer
  // resolve; explicit `import` lines still work. Analysing the prelude is
  // almost all of the cost of analysing a small file, so this exists for
  // tools that analyse millions of small inputs (turf-fuzz's libFuzzer
  // engine). Not a language mode: an editor or a build never sets it.
  bool SkipPrelude = false;
  // The TurfLang installation root lib/std is found under. Empty means what
  // the CLI does: $TURF_HOME, else the directory above the running
  // executable.
  std::string InstallRoot;
  // A package's graph, the JSON `turf graph` prints (format 1, see
  // include/PackageGraph.h): which packages exist, where each one's src/ is
  // and who depends on whom. With it `import` resolves exactly as it does in
  // `turf build` -- the package's own modules anywhere under src/, its direct
  // dependencies, the standard library -- instead of only the standard
  // library and the files next to the analysed one. Empty means a single
  // file. The entry file may be any file of the root package's src/ or
  // tests/. A graph that cannot be loaded, or that the file is not part of,
  // becomes the first diagnostic (no code) and the file is analysed alone.
  std::string PackageGraphFile;
  // Asked at the two boundaries the pipeline already has: after parsing and
  // after resolving. When it returns true the analysis stops there and the
  // result reports cancelled(). Meant for an editor, where the text a
  // keystroke produced is already out of date by the time a slow analysis of
  // it finishes. Empty (the default) is never called. It runs on the thread
  // that called analyze(), inside the analysis, so it must be quick and must
  // not call analyze() itself.
  std::function<bool()> ShouldCancel;
};

// A reserved word. Read from the lexer's own table, so a tool offering
// keywords (completion, a highlighter) cannot fall behind the language.
enum class KeywordKind {
  Keyword, // control flow and declarations: fn, if, compare, recover, pub, ...
  Type,    // a built-in type's name: int, string, ptr, ...
  Literal, // true, false, null
};

struct Keyword {
  std::string Name;
  KeywordKind Kind = KeywordKind::Keyword;
};

// Every reserved word, sorted by name, including the synonyms (`mut` and
// `mutable`, `pub` and `public`).
const std::vector<Keyword> &keywords();

// The prelude modules whose public free functions can be called by their bare
// names with no import (`printline("hi")`, `abs(-3)`, `exit(1)`): `io`, `math`
// and `sys` (FREEZE.md 6.25, F-PRE-2). Read from the compiler's own list, so a
// tool that completes or explains a bare call cannot fall behind the language.
// Every prelude module (these and `int`, `double`, `bool`, `char`, `strings`,
// `array`) is also spellable as `m::` in every file without an import; that
// part needs no list, because the analysis already reports those modules as
// imported by the entry file (`AnalysisResult::symbols().isModuleImportedBy`).
const std::vector<std::string> &preludeBareModules();

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

  // Every name a bare `import <name>;` could legally name from the analysed
  // file, sorted and without duplicates -- for `import <cursor>` completion.
  // Worked out inside the analysis, so it follows the context the analysis
  // ran in: with a package graph it is the standard library, the package's
  // own modules and its direct dependencies (and nothing ambient); without
  // one, the standard library, installed packages and the files next to the
  // analysed one. It lists names, not whether the module is valid. Empty for
  // a cancelled result.
  const std::vector<std::string> &importableModules() const;

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

// The file `import <ModuleName>;` loads when written in ImportingFile,
// resolved with the same installation and package graph (Options.InstallRoot,
// Options.PackageGraphFile) that analyze() applies to that file; "" when the
// name resolves to no single file (not found, or ambiguous). For a tool that
// opens the file behind a module name (go to definition, hover
// documentation) outside any analysis: resolving it with the compiler's own
// single-file rules would miss a project's modules and dependencies. Leaves
// every piece of global compiler state as it found it.
std::string resolveModuleFile(const std::string &ModuleName,
                              const std::string &ImportingFile,
                              const AnalysisOptions &Options = {});

} // namespace turf

#endif
