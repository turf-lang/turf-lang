// TDP-2-001: building, checking and printing the compiler's own fixes. See
// CompilerFix.h for the contract.

#include "diagnostics/CompilerFix.h"

#include "diagnostics/Colors.h"
#include "frontend/Lexer.h"
#include "turf/Analysis.h"

#include <algorithm>
#include <filesystem>
#include <tuple>

namespace CompilerFixes {

bool Enabled = false;
std::string PackageGraphFile;

namespace {

std::string trim(const std::string &S) {
  const size_t B = S.find_first_not_of(" \t");
  if (B == std::string::npos)
    return "";
  return S.substr(B, S.find_last_not_of(" \t") - B + 1);
}

// The text of one edit, for the description: "replace 'nmae' with 'name'",
// "insert ';' at the end of line 2", "remove 'static'".
std::string describeEdit(const CompilerFix::Edit &E, const std::string &Line) {
  const std::string Old = Line.substr(E.StartCol - 1, E.EndCol - E.StartCol);
  if (Old.empty()) {
    const std::string What = "insert '" + trim(E.Replacement) + "'";
    // At the end of the line's text (blanks after it do not count), or
    // after the unbroken run of text it lands against: `fn` -> `fn<T>`.
    const size_t At = E.StartCol - 1;
    if (Line.find_first_not_of(" \t", At) == std::string::npos)
      return What + " at the end of line " + std::to_string(E.Line);
    size_t From = At;
    while (From > 0 && Line[From - 1] != ' ' && Line[From - 1] != '\t')
      --From;
    if (From == At)
      return What;
    return What + " after '" + Line.substr(From, At - From) + "'";
  }
  if (E.Replacement.empty())
    return "remove '" + trim(Old) + "'";
  return "replace '" + trim(Old) + "' with '" + trim(E.Replacement) + "'";
}

std::string joinLines(const std::vector<std::string> &Lines) {
  std::string Out;
  for (const auto &L : Lines) {
    Out += L;
    Out += '\n';
  }
  return Out;
}

bool samePath(const std::string &A, const std::string &B) {
  if (A == B)
    return true;
  std::error_code EA, EB;
  const auto CA = std::filesystem::weakly_canonical(A, EA);
  const auto CB = std::filesystem::weakly_canonical(B, EB);
  return !EA && !EB && CA == CB;
}

// An error as the check compares it: which file, which line, which code. Not
// the column: an edit on a line moves the columns of whatever follows it.
using ErrorKey = std::tuple<std::string, unsigned, std::string>;

std::vector<ErrorKey> errorsOf(const turf::AnalysisResult &R) {
  std::vector<ErrorKey> Out;
  for (const auto &D : R.diagnostics())
    if (D.Level == turf::Severity::Error)
      Out.emplace_back(D.Where.Path, D.Where.Line, D.Code);
  return Out;
}

bool sameError(const ErrorKey &A, const ErrorKey &B) {
  return std::get<1>(A) == std::get<1>(B) && std::get<2>(A) == std::get<2>(B) &&
         samePath(std::get<0>(A), std::get<0>(B));
}

turf::AnalysisOptions checkOptions() {
  turf::AnalysisOptions O;
  O.AllowUnsafeModules = g_AllowUnsafeModules;
  O.RunLint = true;
  O.RunFlowAnalysis = true;
  O.PackageGraphFile = PackageGraphFile;
  return O;
}

// The program as the compile saw it: the entry file's text, the one every
// check of this compile starts from.
const std::vector<std::string> *entryLines() {
  auto It = AllSourceLines.find(MainSourceFilename);
  return It == AllSourceLines.end() ? nullptr : &It->second;
}

// The original program's errors, analysed once per compile.
const std::vector<ErrorKey> &baselineErrors() {
  static bool Done = false;
  static std::vector<ErrorKey> Errors;
  if (!Done) {
    Done = true;
    if (const auto *Lines = entryLines())
      Errors = errorsOf(turf::analyze({{MainSourceFilename, joinLines(*Lines)}},
                                      checkOptions()));
  }
  return Errors;
}

} // namespace

bool build(const SLM::DiagnosticHints &Hints, int ErrorLine,
           const std::vector<std::string> &Lines, CompilerFix &Out) {
  Out = CompilerFix{};
  const int N = static_cast<int>(Lines.size());
  for (const SLM::FixIt &F : Hints.FixIts) {
    const int Line = F.Line == 0 ? ErrorLine : F.Line;
    if (Line < 1 || Line > N ||
        F.Replacement.find('\n') != std::string::npos)
      return false;
    const int Len = static_cast<int>(Lines[Line - 1].size());
    if (F.StartCol < 1 || F.EndCol < F.StartCol || F.EndCol > Len + 1)
      return false;
    Out.Edits.push_back({Line, F.StartCol, F.EndCol, F.Replacement});
  }
  if (Out.Edits.empty())
    return false;
  std::sort(Out.Edits.begin(), Out.Edits.end(),
            [](const CompilerFix::Edit &A, const CompilerFix::Edit &B) {
              return A.Line != B.Line ? A.Line < B.Line : A.StartCol < B.StartCol;
            });
  for (size_t I = 1; I < Out.Edits.size(); ++I) {
    const auto &P = Out.Edits[I - 1], &E = Out.Edits[I];
    if (E.Line == P.Line && (E.StartCol < P.EndCol || E.StartCol == P.StartCol))
      return false;
  }

  Out.FirstLine = Out.Edits.front().Line;
  Out.LastLine = Out.Edits.back().Line;
  bool Changed = false;
  std::vector<std::string> Said;
  size_t Next = 0;
  for (int Line = Out.FirstLine; Line <= Out.LastLine; ++Line) {
    const std::string &Orig = Lines[Line - 1];
    std::string New;
    size_t At = 0;
    for (; Next < Out.Edits.size() && Out.Edits[Next].Line == Line; ++Next) {
      const auto &E = Out.Edits[Next];
      New += Orig.substr(At, E.StartCol - 1 - At);
      New += E.Replacement;
      At = E.EndCol - 1;
      if (Orig.compare(E.StartCol - 1, E.EndCol - E.StartCol, E.Replacement) != 0)
        Changed = true;
      const std::string Text = describeEdit(E, Orig);
      if (std::find(Said.begin(), Said.end(), Text) == Said.end())
        Said.push_back(Text);
    }
    New += Orig.substr(At);
    Out.NewLines.push_back(std::move(New));
  }
  if (!Changed)
    return false;

  if (!Hints.FixDescription.empty()) {
    Out.Description = Hints.FixDescription;
  } else {
    for (size_t I = 0; I < Said.size(); ++I)
      Out.Description += (I == 0 ? "" : I + 1 == Said.size() ? " and " : ", ") + Said[I];
  }

  std::vector<std::string> Patched = Lines;
  for (int Line = Out.FirstLine; Line <= Out.LastLine; ++Line)
    Patched[Line - 1] = Out.NewLines[Line - Out.FirstLine];
  Out.PatchedFile = joinLines(Patched);
  return true;
}

bool check(const CompilerFix &Fix, const std::string &File,
           const std::string &Code, int ErrorLine) {
  // An analysis reports through the same DiagnosticEngine, and must never
  // come back here.
  static bool Checking = false;
  if (Checking || Fix.empty() || Code.empty())
    return false;
  const auto *Entry = entryLines();
  if (!Entry)
    return false;
  struct Guard {
    Guard() { Checking = true; }
    ~Guard() { Checking = false; }
  } G;

  const ErrorKey Original{File, static_cast<unsigned>(ErrorLine), Code};
  const auto &Before = baselineErrors();
  // The analysis must see the error the compile saw, or a clean re-check would
  // prove nothing (a package build analysed without its graph stops at the
  // imports, long before the line the error is on).
  if (std::none_of(Before.begin(), Before.end(),
                   [&](const ErrorKey &K) { return sameError(K, Original); }))
    return false;

  std::vector<turf::SourceBuffer> Buffers;
  if (samePath(File, MainSourceFilename)) {
    Buffers.push_back({MainSourceFilename, Fix.PatchedFile});
  } else {
    Buffers.push_back({MainSourceFilename, joinLines(*Entry)});
    Buffers.push_back({File, Fix.PatchedFile});
  }
  const auto After = errorsOf(turf::analyze(Buffers, checkOptions()));

  // Every error left must be one the original program had, other than the
  // one being fixed: as many of each as before, never more.
  std::vector<ErrorKey> Allowed;
  for (const auto &K : Before)
    if (!sameError(K, Original))
      Allowed.push_back(K);
  for (const auto &K : After) {
    auto It = std::find_if(Allowed.begin(), Allowed.end(),
                           [&](const ErrorKey &A) { return sameError(A, K); });
    if (It == Allowed.end())
      return false;
    Allowed.erase(It);
  }
  return true;
}

std::string render(const CompilerFix &Fix) {
  if (Fix.empty())
    return "";
  const std::string Width = std::to_string(Fix.LastLine);
  std::string Out = "\n  " + Colors::BOLD + Colors::BRIGHT_GREEN + "Fix: " +
                    Colors::RESET + Colors::BRIGHT_GREEN + Fix.Description +
                    Colors::RESET + "\n";
  size_t Next = 0;
  for (int Line = Fix.FirstLine; Line <= Fix.LastLine; ++Line) {
    // Where each edit's new text sits on the new line, for the marks.
    std::string Marks;
    int Shift = 0;
    bool Touched = false;
    for (; Next < Fix.Edits.size() && Fix.Edits[Next].Line == Line; ++Next) {
      const auto &E = Fix.Edits[Next];
      const int Start = E.StartCol - 1 + Shift;
      const int Len = static_cast<int>(E.Replacement.size());
      if (static_cast<int>(Marks.size()) < Start)
        Marks.append(Start - Marks.size(), ' ');
      Marks += Len > 0 ? std::string(Len, '~') : std::string("^");
      Shift += Len - (E.EndCol - E.StartCol);
      Touched = true;
    }
    // A line between two edits that no edit touches is left out.
    if (!Touched)
      continue;
    std::string Num = std::to_string(Line);
    Num.insert(0, Width.size() - Num.size(), ' ');
    Out += Colors::BRIGHT_BLACK + "  " + Num + " │ " + Colors::RESET +
           Fix.NewLines[Line - Fix.FirstLine] + "\n" + Colors::BRIGHT_BLACK +
           "  " + std::string(Width.size(), ' ') + " │ " + Colors::RESET +
           Colors::BRIGHT_GREEN + Marks + Colors::RESET + "\n";
  }
  return Out;
}

} // namespace CompilerFixes
