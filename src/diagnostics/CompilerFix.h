#ifndef COMPILER_FIX_H
#define COMPILER_FIX_H

// TDP-2-001: a fix the compiler prints with an error or a warning, in every
// build, with no model and no flag.
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
#include "diagnostics/FixScore.h"

#include <functional>
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
  // SL-1: the Fix Score of a checked fix (FixScore.h). A `plausible` fix is
  // printed with its flags; a fix that is not valid is never kept.
  FixScore::Facts Score;

  bool empty() const { return Edits.empty(); }
};

// SL-7: where a call's arguments are written, recorded by the parser for the
// fix of a call with the wrong number of arguments. 1-based columns; an end is
// the column just after the last character.
struct CallArgSpans {
  struct Span {
    int Line = 0, StartCol = 0, EndLine = 0, EndCol = 0;
  };
  std::vector<Span> Args;
  int CloseLine = 0, CloseCol = 0; // the ')'
  std::string File;                // the file the call is written in

  bool empty() const { return CloseLine == 0; }
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
// `ErrorLine` of `File`, and the patched program reports no diagnostic there
// with that code and no error the original did not have. With `IsWarning` the
// diagnostic being fixed is a warning (a retired spelling's `Fix:`, SY-2 …
// SY-5), looked for among the warnings before and after.
// With `Scored`, also fills its BeforeDiags and AfterDiags: `File`'s
// diagnostics in the original program's analysis and the patched one's, for
// the Fix Score (FixScore::describeProgram(), SL-5).
bool check(const CompilerFix &Fix, const std::string &File,
           const std::string &Code, int ErrorLine, bool IsWarning = false,
           FixScore::Program *Scored = nullptr);

// The `Fix:` block printDiag() prints under the source snippet: the
// description, then each changed line as it reads after the fix, with the new
// text marked.
std::string render(const CompilerFix &Fix);

// SL-7: the fixes to choose from for a call to `Callee` written as `Spans`,
// whose overloads take the parameters named in `Overloads` (one list each,
// `self` left out). For an overload that takes one argument fewer, deleting
// each argument; for one that takes one more, passing a variable for the
// missing parameter, where a variable of that parameter's name may be
// visible (`IsVariable`; a site that keeps no scopes says yes to every name). The re-check keeps the one that compiles
// (DiagnosticHints::FixAlternatives). Empty when the call spans lines, no
// overload is one argument away, or there are more alternatives than
// MaxArityAlternatives.
inline constexpr int MaxArityAlternatives = 6;
std::vector<SLM::DiagnosticHints::Alternative>
arityAlternatives(const std::string &Callee, const CallArgSpans &Spans,
                  const std::vector<std::vector<std::string>> &Overloads,
                  const std::function<bool(const std::string &)> &IsVariable);

} // namespace CompilerFixes

#endif
