#ifndef COMPILER_FIX_H
#define COMPILER_FIX_H

// TDP-2-001: a fix the compiler prints with an error, in every build, with no
// model and no flag.
//
// A raise site that knows the exact edit attaches it as fix-its
// (SLM::DiagnosticHints::FixIts, through the withFixIt() family in Errors.h).
// At print time (DiagnosticEngine::flushAll()) each such error is turned into
// a CompilerFix here, applied to an in-memory copy of the program and checked
// again through the query facade (include/turf/Analysis.h). It is printed only
// when that check shows the error gone and no error the original program did
// not already have. Nothing is ever written to the user's files.
//
// Every fix printed by a fail_*.tr test is pinned by that test with
// `// EXPECT-FIX: <description>`, and the test runner applies it and checks
// that the program then compiles (TestRunner.cpp).

#include "diagnostics/ErrorCategory.h"

#include <string>
#include <vector>

struct CompilerFix {
  // One edit, against the ORIGINAL text: replace the 1-based byte columns
  // [StartCol, EndCol) of Line with Replacement.
  struct Edit {
    int Line = 0;
    int StartCol = 1;
    int EndCol = 1;
    std::string Replacement;
  };

  // "replace 'nmae' with 'name'": what the `Fix:` line says.
  std::string Description;
  // Sorted by position, never overlapping, never adding or removing a line.
  std::vector<Edit> Edits;
  // The lines [FirstLine, LastLine] as they read after the edits.
  int FirstLine = 0;
  int LastLine = 0;
  std::vector<std::string> NewLines;
  // The whole file after the edits, one '\n' after each line: what
  // --smart-write-fix writes.
  std::string PatchedFile;

  bool empty() const { return Edits.empty(); }
};

namespace CompilerFixes {

// Set by the turfc driver for a compile or a check. Off everywhere else (the
// query facade, the test runner's own process), so a fix is only ever looked
// for where it will be printed.
extern bool Enabled;
// The package graph of the compile (--package-graph), so the re-check resolves
// imports exactly as the compile did.
extern std::string PackageGraphFile;

// At most this many errors get a fix checked in one compile. Each check is one
// analysis of the program; the first errors are the ones a reader fixes first.
inline constexpr int MaxChecksPerCompile = 10;

// Builds the fix that `Hints` describes for an error at `ErrorLine` of a file
// whose text is `Lines`. False when there is nothing to build: no fix-its, an
// edit outside the text, two edits that overlap or tie (their order would be a
// guess), a replacement that spans lines, or edits that change nothing.
bool build(const SLM::DiagnosticHints &Hints, int ErrorLine,
           const std::vector<std::string> &Lines, CompilerFix &Out);

// Applies `Fix` to an in-memory copy of the program and analyses it again.
// True only when the original program's own analysis reports `Code` at
// `ErrorLine` of `File`, and the patched program reports no error there with
// that code and no error the original did not have.
bool check(const CompilerFix &Fix, const std::string &File,
           const std::string &Code, int ErrorLine);

// The `Fix:` block printDiag() prints under the source snippet: the
// description, then each changed line as it reads after the fix, with the new
// text marked.
std::string render(const CompilerFix &Fix);

} // namespace CompilerFixes

#endif
