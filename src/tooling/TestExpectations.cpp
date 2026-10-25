// REL-002: the expectation markers and the diagnostic register behind
// `turfc --test` (TestExpectations.h). A line-for-line port of the shell
// functions scripts/test_runner.sh used to carry (check_diag_expectations,
// check_runtime_expectations, check_exit_expectation, check_diag_policy,
// check_runtime_policy), kept as plain string functions so that the rules can
// be read, and tested, without a process in sight.

#include "tooling/TestExpectations.h"

#include "llvm/Support/JSON.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

namespace TestExpect {

namespace {

std::vector<std::string> SplitLines(const std::string &Text) {
  std::vector<std::string> Lines;
  std::string Cur;
  std::istringstream In(Text);
  while (std::getline(In, Cur)) {
    if (!Cur.empty() && Cur.back() == '\r')
      Cur.pop_back();
    Lines.push_back(Cur);
  }
  return Lines;
}

std::string Trim(const std::string &S) {
  size_t B = 0, E = S.size();
  while (B < E && std::isspace(static_cast<unsigned char>(S[B])))
    ++B;
  while (E > B && std::isspace(static_cast<unsigned char>(S[E - 1])))
    --E;
  return S.substr(B, E - B);
}

bool Contains(const std::string &Hay, const std::string &Needle) {
  return Hay.find(Needle) != std::string::npos;
}

/// The marker lines of @p Source whose comment matches @p LinePattern, with the
/// leading `//` and blanks removed ("EXPECT-DIAG: E011").
std::vector<std::string> MarkerLines(const std::string &Source, const char *LinePattern) {
  std::vector<std::string> Out;
  const std::regex Re(LinePattern);
  const std::regex Lead("^[ \t]*//[ \t]*");
  for (const auto &L : SplitLines(Source))
    if (std::regex_search(L, Re))
      Out.push_back(std::regex_replace(L, Lead, "", std::regex_constants::format_first_only));
  return Out;
}

/// "Exxx" with a diagnostic header's code spelling: `[E011 · control flow]`
/// puts a space after the code, a bare `[E011]` a bracket.
bool HasCode(const std::string &Plain, const std::string &Code) {
  return std::regex_search(Plain, std::regex("\\[" + Code + "[ \\]]"));
}

std::string FirstToken(const std::string &Line) {
  size_t B = Line.find_first_not_of(" \t");
  if (B == std::string::npos)
    return "";
  size_t E = Line.find_first_of(" \t", B);
  return Line.substr(B, E == std::string::npos ? std::string::npos : E - B);
}

std::string FieldValue(const std::string &Line, const char *Pattern) {
  std::smatch M;
  if (std::regex_search(Line, M, std::regex(Pattern)))
    return M[1].str();
  return "";
}

} // namespace

bool HasTestFunctions(const std::string &Source) {
  for (const auto &L : SplitLines(Source)) {
    const size_t B = L.find_first_not_of(" \t");
    if (B == std::string::npos || L.compare(B, 5, "@test") != 0)
      continue;
    const size_t After = B + 5;
    if (After == L.size() ||
        !(std::isalnum(static_cast<unsigned char>(L[After])) || L[After] == '_'))
      return true;
  }
  return false;
}

bool ParseManifest(const std::string &Text, std::vector<TestCase> &Cases, bool &HasMain,
                   std::string &Error) {
  auto Parsed = llvm::json::parse(Text);
  if (!Parsed) {
    Error = "the test build's manifest is not valid JSON (" +
            llvm::toString(Parsed.takeError()) + ")";
    return false;
  }
  const llvm::json::Object *Top = Parsed->getAsObject();
  const llvm::json::Array *Tests = Top ? Top->getArray("tests") : nullptr;
  if (!Top || Top->getInteger("format") != 1 || !Tests) {
    Error = "the test build's manifest is not format 1";
    return false;
  }
  HasMain = Top->getBoolean("has_main").value_or(false);
  auto Str = [](const llvm::json::Object &O, const char *K) {
    auto V = O.getString(K);
    return V ? V->str() : std::string();
  };
  auto Diags = [&](const llvm::json::Object &O, const char *K) {
    std::vector<TestCase::Diag> Out;
    if (const llvm::json::Array *A = O.getArray(K))
      for (const auto &V : *A)
        if (const llvm::json::Object *D = V.getAsObject())
          Out.push_back({Str(*D, "code"), Str(*D, "message"), Str(*D, "fix"),
                         static_cast<int>(D->getInteger("line").value_or(0))});
    return Out;
  };
  for (const auto &V : *Tests) {
    const llvm::json::Object *O = V.getAsObject();
    if (!O)
      continue;
    TestCase C;
    C.Name = Str(*O, "name");
    C.Line = static_cast<int>(O->getInteger("line").value_or(0));
    C.Outcome = Str(*O, "outcome");
    C.Code = Str(*O, "code");
    C.Text = Str(*O, "text");
    C.Error = Str(*O, "error");
    C.HasFix = O->getString("fix").has_value();
    C.Fix = Str(*O, "fix");
    C.Status = static_cast<int>(O->getInteger("status").value_or(0));
    C.Run = static_cast<int>(O->getInteger("run").value_or(-1));
    C.Errors = Diags(*O, "errors");
    C.Warnings = Diags(*O, "warnings");
    Cases.push_back(std::move(C));
  }
  return true;
}

namespace {

std::string Quote(const std::string &S) { return "\"" + S + "\""; }

std::string Describe(const TestCase::Diag &D) {
  return (D.Code.empty() ? std::string("(no code)") : D.Code) + " at line " +
         std::to_string(D.Line) + ": " + Trim(D.Message);
}

// The lines the test program reports through (EmitTestHarness(),
// src/codegen/CodegenFunc.cpp).
const char *const ReturnedLine = "turf-test: returned";
const char *const ErrorLinePrefix = "turf-test: error ";

} // namespace

std::vector<std::string> JudgeCompileError(const TestCase &C) {
  std::vector<std::string> Out;
  const std::string Want = C.Code + (C.Text.empty() ? "" : " " + Quote(C.Text));
  if (C.Errors.empty())
    return {"its body compiled without an error; expected " + Want};
  bool TextFound = false;
  for (const auto &D : C.Errors) {
    if (D.Code != C.Code)
      Out.push_back("its body has another error, " + Describe(D) + "; expected " + C.Code);
    else if (Contains(D.Message, C.Text))
      TextFound = true;
  }
  if (!Out.empty())
    return Out;
  if (!TextFound) {
    Out.push_back("no " + C.Code + " message contains " + Quote(C.Text) + ":");
    for (const auto &D : C.Errors)
      Out.push_back("  " + Describe(D));
    return Out;
  }
  if (C.HasFix) {
    bool FixFound = false;
    for (const auto &D : C.Errors)
      FixFound = FixFound || (!D.Fix.empty() && Contains(D.Fix, C.Fix));
    if (!FixFound) {
      Out.push_back("the compiler printed no fix containing " + Quote(C.Fix) + ":");
      for (const auto &D : C.Errors)
        Out.push_back("  " + D.Code + " at line " + std::to_string(D.Line) + ": " +
                      (D.Fix.empty() ? std::string("no fix") : "Fix: " + D.Fix));
    }
  }
  return Out;
}

std::vector<std::string> JudgeWarnings(const TestCase &C) {
  std::vector<std::string> Out;
  if (C.Outcome == "no_warnings") {
    for (const auto &D : C.Warnings)
      Out.push_back("its body compiled with a warning, " + Describe(D));
    return Out;
  }
  if (C.Outcome != "warning")
    return Out;
  for (const auto &D : C.Warnings)
    if (D.Code == C.Code && Contains(D.Message, C.Text))
      return Out;
  Out.push_back("its body compiled without warning " + C.Code +
                (C.Text.empty() ? "" : " " + Quote(C.Text)) +
                (C.Warnings.empty() ? "" : "; its warnings:"));
  for (const auto &D : C.Warnings)
    Out.push_back("  " + Describe(D));
  return Out;
}

std::string ProgramOutput(const std::string &Output) {
  std::string Out;
  for (const auto &L : SplitLines(Output))
    if (L != ReturnedLine && L.rfind(ErrorLinePrefix, 0) != 0)
      Out += L + "\n";
  // The report line starts on a line of its own, after a blank one.
  while (!Out.empty() && (Out.back() == '\n'))
    Out.pop_back();
  return Out;
}

std::vector<std::string> JudgeRun(const TestCase &C, int Status, bool TimedOut,
                                  const std::string &Output) {
  bool Returned = false;
  std::string ReturnedError;
  for (const auto &L : SplitLines(Output)) {
    if (L == ReturnedLine)
      Returned = true;
    else if (L.rfind(ErrorLinePrefix, 0) == 0)
      ReturnedError = L.substr(std::string(ErrorLinePrefix).size());
  }
  const bool Panicked = !Returned && ReturnedError.empty() && Status == 134;
  // How the test ended, in words, for any outcome it was not.
  auto Ended = [&]() -> std::string {
    if (TimedOut)
      return "it did not finish (timed out)";
    if (!ReturnedError.empty())
      return "it returned the error " + ReturnedError;
    if (Returned)
      return "it returned";
    if (Panicked)
      return "it panicked";
    return "it ended with exit status " + std::to_string(Status);
  };

  if (C.Outcome == "panic") {
    if (!Panicked || TimedOut)
      return {Ended() + "; expected a panic" +
              (C.Text.empty() ? std::string() : " with " + Quote(C.Text))};
    if (!Contains(Output, C.Text))
      return {"it panicked, but its output does not contain " + Quote(C.Text)};
    return {};
  }
  if (C.Outcome == "exit") {
    if (TimedOut || Returned || !ReturnedError.empty() || Status != C.Status)
      return {Ended() + "; expected it to end through sys::exit(" + std::to_string(C.Status) +
              ")"};
    return {};
  }
  if (C.Outcome == "error") {
    if (ReturnedError == C.Error && !TimedOut)
      return {};
    return {Ended() + "; expected it to return the error " + C.Error};
  }
  // returns, warning, no_warnings: the body returns, and without an error.
  if (Returned && !TimedOut)
    return {};
  return {Ended() + "; expected it to return"};
}

std::string StripAnsi(const std::string &Text) {
  std::string Out;
  Out.reserve(Text.size());
  for (size_t I = 0; I < Text.size(); ++I) {
    if (Text[I] == '\033' && I + 1 < Text.size() && Text[I + 1] == '[') {
      size_t J = I + 2;
      while (J < Text.size() && (std::isdigit(static_cast<unsigned char>(Text[J])) || Text[J] == ';'))
        ++J;
      if (J < Text.size() && Text[J] == 'm') {
        I = J;
        continue;
      }
    }
    Out += Text[I];
  }
  return Out;
}

bool IsRuntimeAbortTest(const std::string &Source) {
  return Contains(Source, "RUNTIME_ABORT_EXPECTED");
}

std::vector<std::string> CheckDiag(const std::string &Source, const std::string &CompileOutput) {
  std::vector<std::string> Missed;
  const auto Markers = MarkerLines(Source, "^[ \t]*//[ \t]*EXPECT(-NO)?-DIAG:");
  if (Markers.empty())
    return Missed;
  const std::string Plain = StripAnsi(CompileOutput);
  const std::regex CodeOnly("^E[0-9]{3}$");
  for (const auto &Line : Markers) {
    const size_t Colon = Line.find(':');
    const std::string Kind = Line.substr(0, Colon);
    const std::string Arg = Trim(Line.substr(Colon + 1));
    if (Arg.empty())
      continue;
    if (Kind == "EXPECT-NO-DIAG") {
      if (Contains(Plain, Arg))
        Missed.push_back("EXPECT-NO-DIAG violated: the compiler output contains '" + Arg + "'");
    } else if (std::regex_match(Arg, CodeOnly)) {
      if (!HasCode(Plain, Arg))
        Missed.push_back("EXPECT-DIAG missed: no [" + Arg + "] in the compiler output");
    } else if (!Contains(Plain, Arg)) {
      Missed.push_back("EXPECT-DIAG missed: the compiler output does not contain '" + Arg + "'");
    }
  }
  return Missed;
}

namespace {
/// The text after "Fix: " of every fix line in @p CompileOutput.
std::vector<std::string> PrintedFixes(const std::string &CompileOutput) {
  std::vector<std::string> Out;
  for (const auto &L : SplitLines(StripAnsi(CompileOutput))) {
    const std::string T = Trim(L);
    if (T.rfind("Fix: ", 0) == 0)
      Out.push_back(Trim(T.substr(5)));
  }
  return Out;
}
} // namespace

size_t CountFixes(const std::string &CompileOutput) {
  return PrintedFixes(CompileOutput).size();
}

bool HasFixMarker(const std::string &Source) {
  return !MarkerLines(Source, "^[ \t]*//[ \t]*EXPECT-FIX:").empty();
}

std::vector<std::string> CheckFix(const std::string &Source, const std::string &CompileOutput,
                                  bool RequireAllPinned) {
  std::vector<std::string> Missed;
  std::vector<std::string> Pinned;
  for (const auto &Line : MarkerLines(Source, "^[ \t]*//[ \t]*EXPECT-FIX:"))
    if (const std::string Arg = Trim(Line.substr(Line.find(':') + 1)); !Arg.empty())
      Pinned.push_back(Arg);
  std::vector<std::string> Printed = PrintedFixes(CompileOutput);
  for (const auto &P : Pinned) {
    auto It = std::find(Printed.begin(), Printed.end(), P);
    if (It == Printed.end())
      Missed.push_back("EXPECT-FIX missed: the compiler output has no 'Fix: " + P + "'");
    else
      Printed.erase(It);
  }
  if (RequireAllPinned)
    for (const auto &P : Printed)
      Missed.push_back("EXPECT-FIX missing: the compiler printed 'Fix: " + P +
                     "', and every printed fix is pinned with '// EXPECT-FIX: " + P + "'");
  return Missed;
}

std::vector<std::string> CheckRuntime(const std::string &Source, const std::string &ProgramOutput) {
  std::vector<std::string> Missed;
  for (const auto &Line : MarkerLines(Source, "^[ \t]*//[ \t]*EXPECT-RUNTIME:")) {
    const std::string Arg = Trim(Line.substr(Line.find(':') + 1));
    if (!Arg.empty() && !Contains(ProgramOutput, Arg))
      Missed.push_back("EXPECT-RUNTIME missed: the program's output does not contain '" + Arg + "'");
  }
  return Missed;
}

std::vector<std::string> CheckExit(const std::string &Source, int Status) {
  const auto Lines = MarkerLines(Source, "^[ \t]*//[ \t]*EXPECT-EXIT:");
  if (Lines.empty())
    return {};
  const std::string Want = Trim(Lines.front().substr(Lines.front().find(':') + 1));
  if (Want.empty() || Want == std::to_string(Status))
    return {};
  return {"EXPECT-EXIT missed: the program exited with " + std::to_string(Status) + ", not " + Want};
}

bool Register::load(const std::string &Path, std::string &Error) {
  std::ifstream In(Path);
  if (!In) {
    Error = "cannot read the diagnostic register " + Path;
    return false;
  }
  ByKey_.clear();
  Counts_.clear();
  std::string Line;
  while (std::getline(In, Line)) {
    std::istringstream Tok(Line);
    std::string Kind, Key;
    Tok >> Kind >> Key;
    if (Kind.empty() || Kind[0] == '#')
      continue;
    ++Counts_[Kind];
    if (!Key.empty())
      ByKey_[Key].push_back({Kind, Line});
  }
  return true;
}

size_t Register::count(const std::string &Kind) const {
  auto It = Counts_.find(Kind);
  return It == Counts_.end() ? 0 : It->second;
}

std::vector<std::string> Register::checkDiagPolicy(const std::string &Source, const std::string &Key,
                                                   const std::string &CompileOutput) const {
  std::vector<std::string> Out;
  const std::string Plain = StripAnsi(CompileOutput);
  const auto PlainLines = SplitLines(Plain);

  // The codes the compiler printed, in order, once each.
  std::set<std::string> Emitted;
  {
    const std::regex Code("\\[(E[0-9]{3})");
    for (auto I = std::sregex_iterator(Plain.begin(), Plain.end(), Code); I != std::sregex_iterator(); ++I)
      Emitted.insert((*I)[1].str());
  }
  std::string EmittedList;
  for (const auto &C : Emitted)
    EmittedList += (EmittedList.empty() ? "" : ",") + C;
  const std::string EmittedOrNone = EmittedList.empty() ? "no error code at all" : EmittedList;
  const std::string FirstEmitted = Emitted.empty() ? "" : *Emitted.begin();

  // NO-CODE: a `── error` header with no `[Exxx]` is a plain error, so the
  // beginner has no `--explain` page for it. (A warning has a code too, but is never judged here.)
  int Codeless = 0;
  // GENERIC-CODE: E066 is the TypeChecker's one catch-all class; the line under
  // its header is the real banner, and anything but "Type mismatch!" there is
  // a more specific mistake wearing the catch-all's code.
  std::string CatchAll;
  // CASCADE: the "(N errors)" count the compiler prints under a file name,
  // summed over every header (an error in an imported module prints under its
  // own file's header).
  int ActualErrors = 0;
  bool SawCount = false;
  {
    const std::string ErrHeader = "── error";
    const std::regex HasCodeRe("\\[E[0-9]{3}");
    const std::regex E066Header("^── error.*\\[E066[ \\]]");
    const std::regex CountRe("^[^ ]+ \\(([0-9]+) errors?\\)");
    bool AfterE066 = false;
    for (const auto &L : PlainLines) {
      if (L.compare(0, ErrHeader.size(), ErrHeader) == 0 && !std::regex_search(L, HasCodeRe))
        ++Codeless;
      if (std::regex_search(L, E066Header)) {
        AfterE066 = true;
        continue;
      }
      if (AfterE066 && !Trim(L).empty()) {
        const std::string Banner = Trim(L);
        if (CatchAll.empty() && Banner != "Type mismatch!")
          CatchAll = Banner;
        AfterE066 = false;
      }
      std::smatch M;
      if (std::regex_search(L, M, CountRe)) {
        ActualErrors += std::stoi(M[1].str());
        SawCount = true;
      }
    }
  }

  const std::vector<Entry> *Entries = nullptr;
  if (auto It = ByKey_.find(Key); It != ByKey_.end())
    Entries = &It->second;
  auto Has = [&](const char *Kind) {
    if (Entries)
      for (const auto &E : *Entries)
        if (E.Kind == Kind)
          return true;
    return false;
  };
  auto FirstOf = [&](const char *Kind) -> std::string {
    if (Entries)
      for (const auto &E : *Entries)
        if (E.Kind == Kind)
          return E.Line;
    return "";
  };
  bool CodePin = false;
  for (const auto &L : SplitLines(Source))
    if (std::regex_match(L, std::regex("^[ \t]*//[ \t]*EXPECT-DIAG:[ \t]*E[0-9]{3}[ \t]*$")))
      CodePin = true;

  // Every entry must be a known kind.
  bool Bad = false;
  if (Entries) {
    static const std::set<std::string> Known = {"UNPINNED", "NO-CODE", "WRONG-REASON", "GENERIC-CODE",
                                                "CASCADE"};
    for (const auto &E : *Entries)
      if (!Known.count(FirstToken(E.Line))) {
        Out.push_back("DIAG-POLICY: " + Key + " has an entry of unknown kind '" + FirstToken(E.Line) +
                      "' in the diagnostic register (expected UNPINNED, NO-CODE, WRONG-REASON, "
                      "GENERIC-CODE or CASCADE)");
        Bad = true;
      }
  }
  if (Bad)
    return Out;

  if (Has("UNPINNED")) {
    if (CodePin)
      Out.push_back("DIAG-POLICY stale: " + Key +
                    " pins an error code but is still listed UNPINNED in tests/DIAGNOSTIC_GAPS.txt -- "
                    "delete that line");
    // Baseline: nothing else is checked until the test is pinned.
    return Out;
  }

  if (CodePin && Has("WRONG-REASON")) {
    Out.push_back("DIAG-POLICY stale: " + Key +
                  " pins an error code but is still listed WRONG-REASON -- if it now fires the "
                  "diagnostic it was written for, delete that line");
    Bad = true;
  }
  if (!CodePin && !Entries) {
    Out.push_back("DIAG-POLICY: " + Key +
                  " has no '// EXPECT-DIAG: Exxx' line and is not in tests/DIAGNOSTIC_GAPS.txt.");
    Out.push_back("  The compiler emitted: " + EmittedOrNone + ".");
    Out.push_back("  Pin the code the test is MEANT to produce. If the compiler emits a different one, "
                  "list it as WRONG-REASON; if none, as NO-CODE.");
    Bad = true;
  }

  // --- NO-CODE ---------------------------------------------------------
  if (Codeless > 0 && !Has("NO-CODE") && !Has("WRONG-REASON")) {
    Out.push_back("DIAG-POLICY: " + Key + " makes the compiler print " + std::to_string(Codeless) +
                  " error(s) with no [Exxx] code -- a beginner cannot 'turf --explain' those.");
    Out.push_back("  List it as NO-CODE in tests/DIAGNOSTIC_GAPS.txt (and pin its message text with "
                  "'// EXPECT-DIAG: <text>'), or give the error a code.");
    Bad = true;
  }
  if (Has("NO-CODE")) {
    if (Codeless == 0) {
      Out.push_back("DIAG-POLICY stale: " + Key +
                    " is listed NO-CODE but every error it prints now has a code (" +
                    (EmittedList.empty() ? "none" : EmittedList) + ") -- pin '// EXPECT-DIAG: " +
                    FirstEmitted + "' and delete the line");
      Bad = true;
    } else {
      // The test must also pin the plain error by text: a marker that is not
      // a bare code.
      bool TextPin = false;
      for (const auto &M : MarkerLines(Source, "^[ \t]*//[ \t]*EXPECT-DIAG:"))
        if (!std::regex_match(M, std::regex("^EXPECT-DIAG:[ \t]*E[0-9]{3}[ \t]*$")))
          TextPin = true;
      if (!TextPin) {
        Out.push_back("DIAG-POLICY: " + Key +
                      " is NO-CODE, so it must pin its reason by text: add '// EXPECT-DIAG: <fragment of "
                      "the message>'");
        Bad = true;
      }
    }
  }

  // --- GENERIC-CODE ----------------------------------------------------
  if (!CatchAll.empty() && !Has("GENERIC-CODE") && !Has("WRONG-REASON")) {
    Out.push_back("DIAG-POLICY: " + Key + " reports '" + CatchAll +
                  "' under E066, the type-mismatch catch-all, so a beginner's 'turf --explain E066' "
                  "will not describe it.");
    Out.push_back("  List it as GENERIC-CODE (code=E066 title=\"" + CatchAll +
                  "\") in tests/DIAGNOSTIC_GAPS.txt, or give that mistake its own code.");
    Bad = true;
  }
  if (Has("GENERIC-CODE")) {
    const std::string Line = FirstOf("GENERIC-CODE");
    const std::string Code = FieldValue(Line, "code=(E[0-9]{3})");
    const std::string Title = FieldValue(Line, "title=\"([^\"]*)\"");
    if (Code.empty() || Title.empty()) {
      Out.push_back("DIAG-POLICY: " + Key + "'s GENERIC-CODE entry needs 'code=Exxx' and 'title=\"...\"'");
      Bad = true;
    } else if (!HasCode(Plain, Code) || !Contains(Plain, Title)) {
      Out.push_back("DIAG-POLICY stale: " + Key + " is listed as '" + Title + "' under " + Code +
                    " but the output no longer has both (" + EmittedOrNone +
                    ") -- update or delete the entry");
      Bad = true;
    }
  }

  // --- CASCADE ---------------------------------------------------------
  if (Has("CASCADE")) {
    const std::string Want = FieldValue(FirstOf("CASCADE"), "errors=([0-9]+)");
    if (Want.empty()) {
      Out.push_back("DIAG-POLICY: " + Key +
                    "'s CASCADE entry needs 'errors=N' (how many errors the compiler prints today)");
      Bad = true;
    } else if (!SawCount || std::to_string(ActualErrors) != Want) {
      Out.push_back("DIAG-POLICY stale: " + Key + " is listed as printing " + Want +
                    " errors but now prints " + (SawCount ? std::to_string(ActualErrors) : "?") +
                    " -- update the entry, or delete it if the cascade is fixed (and then pin "
                    "'// EXPECT-NO-DIAG: <the bogus message>')");
      Bad = true;
    }
  }

  // --- WRONG-REASON ----------------------------------------------------
  if (Has("WRONG-REASON")) {
    const std::string Line = FirstOf("WRONG-REASON");
    const std::string Emits = FieldValue(Line, "emits=([A-Za-z0-9,]+)");
    if (Emits.empty() || !Contains(Line, "intended=")) {
      Out.push_back("DIAG-POLICY: " + Key +
                    "'s WRONG-REASON entry needs 'emits=Exxx' (or 'emits=none') and 'intended=...'");
      Bad = true;
    } else if (Emits == "none") {
      if (!Emitted.empty()) {
        Out.push_back("DIAG-POLICY stale: " + Key +
                      " is listed as emitting no code but the output has " + EmittedList +
                      " -- update or delete the entry");
        Bad = true;
      }
    } else {
      std::istringstream Codes(Emits);
      std::string N;
      while (std::getline(Codes, N, ','))
        if (!N.empty() && !HasCode(Plain, N)) {
          Out.push_back("DIAG-POLICY stale: " + Key + " is listed as emitting " + N +
                        " but the output has " + EmittedOrNone + " -- update or delete the entry");
          Bad = true;
        }
    }
  }
  return Out;
}

std::vector<std::string> Register::checkRuntimePolicy(const std::string &Source,
                                                      const std::string &Key) const {
  std::string Kind;
  if (auto It = ByKey_.find(Key); It != ByKey_.end() && !It->second.empty())
    Kind = It->second.front().Kind;
  bool Pinned = false;
  for (const auto &L : SplitLines(Source))
    if (std::regex_search(L, std::regex("^[ \t]*//[ \t]*EXPECT-(RUNTIME|EXIT):[ \t]*[^ \t]")))
      Pinned = true;
  if (Pinned) {
    if (Kind.empty())
      return {};
    return {"DIAG-POLICY stale: " + Key + " has an EXPECT-RUNTIME pin but is still listed " + Kind +
            " in tests/DIAGNOSTIC_GAPS.txt -- delete that line"};
  }
  if (Kind == "UNPINNED")
    return {};
  if (Kind.empty())
    return {"DIAG-POLICY: " + Key +
                " aborts at runtime but has no '// EXPECT-RUNTIME: <text>' line and is not in "
                "tests/DIAGNOSTIC_GAPS.txt.",
            "  Pin the message it is MEANT to abort with, so another trap firing first cannot keep it "
            "green."};
  return {"DIAG-POLICY: " + Key + " is a runtime-abort test; only UNPINNED is a valid register kind for "
          "it, not '" + Kind + "'"};
}

} // namespace TestExpect
