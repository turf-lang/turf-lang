#ifndef FIX_SCORE_H
#define FIX_SCORE_H
// SL-1 (TDP-2-008): the Fix Score. Every fix the compiler shows, its own
// (`Fix:`) or the --smart model's, is described by facts computed after its
// re-check, and the facts reduce to a tier:
//
//   proven     a rule fix, valid, the only one that passes, no flag;
//   verified   a model fix (or the model's cached one), valid, no flag;
//   plausible  valid, but a flag is raised: shown with the flag spelled out;
//   rejected   not valid: never shown.
//
// Facts: valid (the hard gate, decided by the re-check), minimality (token
// edit distance against the error line's length), ambiguity (how many
// different fixes pass) and provenance, and SL-5's checks of what the fix does
// to the whole program, read from the file before and after it and from one
// analysis of each (describeProgram()): semantic loss (calls, asserts,
// returns or assignments the fix removes, counted on the token stream, since
// the file may not parse before the fix), API surface (a declaration changed
// or removed, a `pub` item or a struct field added), capability (`unsafe`,
// `extern`, a system import, a raw pointer, `mem::`) and the warning delta.
// There are no weights: any flag makes the fix plausible, so every check can
// only lower the tier. The one threshold, minimality's, is fitted on the train
// split of the --smart benchmark (scripts/slm/slm_benchmark/fit_fix_score.py)
// and frozen here.
#include <string>
#include <vector>

namespace FixScore {

enum class Tier { Rejected, Plausible, Verified, Proven };
enum class Provenance { Rule, Model, Cache };

struct Facts {
  Provenance From = Provenance::Rule;
  bool Valid = false;
  // New warnings: a code for one in the text the fix wrote, a code and its
  // line ("E163 (line 2)") for one elsewhere that the fix caused.
  std::vector<std::string> NewWarnings;
  int TokenDistance = 0; // token edit distance, replaced lines -> new lines
  int LinesTouched = 0;  // max(replaced lines, new lines)
  int SpanTokens = 0;    // tokens on the error's line
  bool Wide = false;
  // What the fix removes: "a call", "an assert", "a `return`", "an assignment".
  std::vector<std::string> Lost;
  // Declarations the fix changes or removes, as they read before it.
  std::vector<std::string> SurfaceChanges;
  // What the fix adds to the program's surface, one sentence each: a `pub`
  // item, a field of a struct the program already had.
  std::vector<std::string> SurfaceAdded;
  // Where the fix reaches further than the program did, one sentence each
  // ("it adds `unsafe`", "it imports `os`").
  std::vector<std::string> Capabilities;
  // Different fixes that pass the checks; 1 when this is the only one, 0 when
  // not measured (a model fix without --smart=thorough).
  int Candidates = 1;
};

// The minimality threshold (fit_fix_score.py, train split of 2026-11-09:
// 336 single-mutation cases, the 95th percentile of the right fix's
// distance / error-line tokens is 1.00 and of its distance 6).
inline constexpr float kWideRatio = 1.00f;
inline constexpr int kWideFloor = 6;

// The tokens of `Text`: comments dropped, a string or char literal one token.
std::vector<std::string> tokenize(const std::string &Text);

// Fills minimality: `Before` are the lines the fix replaces, `After` what
// replaces them, `ErrorLine` the text of the error's line.
void describeEdit(Facts &F, const std::vector<std::string> &Before,
                  const std::vector<std::string> &After,
                  const std::string &ErrorLine);

// A diagnostic of one analysis of the fixed file, before or after the fix.
struct ProgramDiag {
  int Line = 0, Col = 0; // 1-based
  std::string Code, Message;
  bool Error = false;
};

// SL-5: a fix as the whole file sees it. `BeforeDiags` and `AfterDiags` come
// from the same analysis (same depth, same options) of the two texts, and
// hold the fixed file's diagnostics only.
struct Program {
  std::vector<std::string> Before, After; // the whole file
  int Start = 0, End = 0; // the 1-based lines of Before the fix replaces
  int ErrorLine = 0;      // in Before
  std::vector<ProgramDiag> BeforeDiags, AfterDiags;
};

// Fills every fact but validity, ambiguity and provenance: minimality, then
// SL-5's checks over the whole file. A declaration on the error's own line is
// what the error is about, so changing it is not a surface change. A warning
// the fix caused is one in the text it wrote (warningsInChangedText()), or one
// elsewhere the original did not have: in a function the original analysed
// in full (no error in it, no syntax error anywhere), or one whose message
// names something the fix removed or added.
void describeProgram(Facts &F, const Program &P);

// The capability check alone: one sentence per way `After` reaches further
// than `Before` (`unsafe`, `extern`, a new import of `os`, `fs`, `http` or
// `io`, a raw pointer, a `mem::` call).
std::vector<std::string> capabilityDiff(const std::vector<std::string> &Before,
                                        const std::vector<std::string> &After);

// A warning the program has after a fix, on one of the lines it writes.
struct Warning {
  int Index = 0; // into `After`
  int Col = 0;   // 1-based
  std::string Code;
};
// The warnings that point into text the fix changed (a line's text between
// what it kept at the start and at the end, or a line it added). A warning
// elsewhere on such a line is about text the fix kept: the original may have
// lacked it only because its error stopped the checks that report it.
std::vector<std::string> warningsInChangedText(const std::vector<std::string> &Before,
                                               const std::vector<std::string> &After,
                                               const std::vector<Warning> &Warnings);

Tier tier(const Facts &F);
const char *tierName(Tier T);
// One plain sentence per raised flag ("it removes a call"), in a fixed order.
std::vector<std::string> flags(const Facts &F);
// "Check before applying: it removes a call; it adds warning E163." -- or ""
// for a fix with no flag.
std::string flagLine(const Facts &F);

} // namespace FixScore

#endif // FIX_SCORE_H
