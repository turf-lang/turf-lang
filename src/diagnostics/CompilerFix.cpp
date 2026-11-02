// TDP-2-001: building, checking and printing the compiler's own fixes. See
// CompilerFix.h for the contract.

#include "diagnostics/CompilerFix.h"

#include "diagnostics/Colors.h"
#include "frontend/Lexer.h"
#include "frontend/TestRegistry.h"
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

std::vector<ErrorKey> errorsOf(const turf::AnalysisResult &R,
                               turf::Severity Level = turf::Severity::Error) {
  std::vector<ErrorKey> Out;
  for (const auto &D : R.diagnostics())
    if (D.Level == Level)
      Out.emplace_back(D.Where.Path, D.Where.Line, D.Code);
  return Out;
}

bool sameError(const ErrorKey &A, const ErrorKey &B) {
  return std::get<1>(A) == std::get<1>(B) && std::get<2>(A) == std::get<2>(B) &&
         samePath(std::get<0>(A), std::get<0>(B));
}

// Analyses @p Buffers reporting every error, a compile_error test's included
// (TDP-2-006, TestRegistry::Mode::Raw): a fix for a test's expected error is
// checked against that error like any other, and the other tests' errors are
// in the program before and after alike.
turf::AnalysisResult analyzeAll(const std::vector<turf::SourceBuffer> &Buffers,
                                const turf::AnalysisOptions &Options) {
  const TestRegistry::Mode Saved = TestRegistry::mode();
  TestRegistry::setMode(TestRegistry::Mode::Raw);
  turf::AnalysisResult R = turf::analyze(Buffers, Options);
  TestRegistry::setMode(Saved);
  return R;
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

// The original program's errors and warnings, analysed once per compile.
struct Baseline {
  std::vector<ErrorKey> Errors, Warnings;
};
const Baseline &baseline() {
  static bool Done = false;
  static Baseline B;
  if (!Done) {
    Done = true;
    if (const auto *Lines = entryLines()) {
      const auto R = analyzeAll({{MainSourceFilename, joinLines(*Lines)}},
                                checkOptions());
      B.Errors = errorsOf(R);
      B.Warnings = errorsOf(R, turf::Severity::Warning);
    }
  }
  return B;
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
           const std::string &Code, int ErrorLine, bool IsWarning) {
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
  const auto &Base = baseline();
  const auto &Before = Base.Errors;
  // The analysis must see the diagnostic the compile saw, or a clean re-check
  // would prove nothing (a package build analysed without its graph stops at
  // the imports, long before the line the error is on). A warning is looked
  // for among the warnings: the analysis runs at the compile's lint levels,
  // so a warning that was printed is reported there too.
  const auto &Seen = IsWarning ? Base.Warnings : Before;
  if (std::none_of(Seen.begin(), Seen.end(),
                   [&](const ErrorKey &K) { return sameError(K, Original); }))
    return false;

  std::vector<turf::SourceBuffer> Buffers;
  if (samePath(File, MainSourceFilename)) {
    Buffers.push_back({MainSourceFilename, Fix.PatchedFile});
  } else {
    Buffers.push_back({MainSourceFilename, joinLines(*Entry)});
    Buffers.push_back({File, Fix.PatchedFile});
  }
  const auto Patched = analyzeAll(Buffers, checkOptions());
  const auto After = errorsOf(Patched);

  // A warning's fix must take that warning away, as an error's fix takes the
  // error away below (the error list never had the warning in it).
  if (IsWarning) {
    const auto Left = errorsOf(Patched, turf::Severity::Warning);
    if (std::any_of(Left.begin(), Left.end(),
                    [&](const ErrorKey &K) { return sameError(K, Original); }))
      return false;
  }

  // Every error left must be one the original program had, other than the
  // one being fixed: as many of each as before, never more.
  std::vector<ErrorKey> Allowed;
  for (const auto &K : Before)
    if (IsWarning || !sameError(K, Original))
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

std::vector<SLM::DiagnosticHints::Alternative>
arityAlternatives(const std::string &Callee, const CallArgSpans &Spans,
                  const std::vector<std::vector<std::string>> &Overloads,
                  const std::function<bool(const std::string &)> &IsVariable) {
  using Alternative = SLM::DiagnosticHints::Alternative;
  std::vector<Alternative> Out;
  if (Spans.empty())
    return Out;
  // Every edit stays on one line, so the whole call has to be on one.
  const int Line = Spans.CloseLine;
  for (const auto &A : Spans.Args)
    if (A.Line != Line || A.EndLine != Line || A.StartCol < 1 || A.EndCol <= A.StartCol)
      return Out;
  const std::string *Text = FixItSourceLine(SourceLocation{Line, 1, Spans.File});
  if (!Text)
    return Out;
  const int N = static_cast<int>(Spans.Args.size());
  auto argText = [&](int I) {
    const auto &A = Spans.Args[I];
    return Text->substr(A.StartCol - 1, A.EndCol - A.StartCol);
  };
  auto add = [&](int StartCol, int EndCol, const std::string &Replacement,
                 const std::string &Description, const std::string &Reason) {
    for (const auto &Have : Out)
      if (Have.FixIts.front().StartCol == StartCol &&
          Have.FixIts.front().EndCol == EndCol &&
          Have.FixIts.front().Replacement == Replacement)
        return;
    Out.push_back({{{Line, StartCol, EndCol, Replacement}}, Reason, Description});
  };
  auto count = [](size_t K) {
    return std::to_string(K) + (K == 1 ? " argument" : " arguments");
  };

  for (const auto &Params : Overloads) {
    const int Want = static_cast<int>(Params.size());
    if (Want == N - 1) {
      // One argument too many: delete one, with the ',' that joins it to
      // its neighbour.
      for (int I = 0; I < N; ++I) {
        const auto &A = Spans.Args[I];
        int From = A.StartCol, To = A.EndCol;
        if (I + 1 < N)
          To = Spans.Args[I + 1].StartCol;
        else if (I > 0)
          From = Spans.Args[I - 1].EndCol;
        add(From, To, "", "remove the argument '" + argText(I) + "'",
            "'" + Callee + "' takes " + count(Want) + " and this call passes " +
                std::to_string(N) + "; without '" + argText(I) +
                "' the others match its parameters.");
      }
    } else if (Want == N + 1) {
      // One argument too few: pass the variable named like the parameter.
      for (int K = 0; K < Want; ++K) {
        const std::string &Name = Params[K];
        if (Name.empty() || !IsVariable(Name))
          continue;
        // Not where it would push an argument already named for its
        // parameter out of place (`area(width)` misses `height`, not a
        // second `width`), and not a variable the call already passes.
        bool Displaces = false;
        for (int J = 0; J < N && !Displaces; ++J)
          Displaces = argText(J) == Name || (J >= K && argText(J) == Params[J]);
        if (Displaces)
          continue;
        int Col;
        std::string Insert;
        if (K < N) {
          Col = Spans.Args[K].StartCol;
          Insert = Name + ", ";
        } else if (N > 0) {
          Col = Spans.Args[N - 1].EndCol;
          Insert = ", " + Name;
        } else {
          Col = Spans.CloseCol;
          Insert = Name;
        }
        add(Col, Col, Insert,
            "pass '" + Name + "' as argument " + std::to_string(K + 1),
            "'" + Callee + "' takes " + count(Want) + " and this call passes " +
                std::to_string(N) + "; its parameter '" + Name +
                "' is missing, and a variable of that name is in scope.");
      }
    }
  }
  if (static_cast<int>(Out.size()) > MaxArityAlternatives)
    Out.clear();
  return Out;
}

} // namespace CompilerFixes
