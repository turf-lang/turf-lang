#pragma once

// TestAnnotation.h -- the meaning of `@test`, owned by `turf test`
// (src/tooling/TestRunner.cpp) and the compile that builds a file's tests
// (src/frontend/TestRegistry.h). See src/frontend/Annotation.h.
//
// TDP-2-006 (roadmap F-11d, issue #299): a function marked `@test` is a test
// written beside the code it checks. `turf test` runs it; a normal build
// checks it and leaves it out. The optional first argument is the test's
// OUTCOME, how it is expected to end:
//
//   @test                                      it returns (`: void`, or a
//                                              `: !void` without an error)
//   @test(compile_error, E041, "text")         its body is rejected with E041
//   @test(compile_error, E041, "t", fix: "f")  ... and the compiler prints a fix
//   @test(warning, E172, "text")               it compiles with that warning
//   @test(no_warnings)                         it compiles with no warning
//   @test(panic, "text")                       it panics with that message
//   @test(exit, 3)                             it ends through sys::exit(3)
//   @test(error, ParseError::Empty)            its `!void` body returns that error
//
// The outcome words mean this only as the FIRST argument, so they reserve
// nothing: `compile_error` and not `Error::E041`, because `Error::E041` is
// already how a program names a variant of its own `error Error`.
//
// TDP-2-026 (roadmap F-11f, issue #340): OPTIONS, how the test is run, follow
// the outcome in any order, each at most once:
//
//   skip: "reason"           compiled and checked, not run; listed with the reason
//   timeout: 2000            fails if it has not ended within 2000 ms
//   levels: all | [O0, O2]   built and run at each level (all: O0 to O3)
//   stdout: "text"           what it prints to stdout is exactly this
//   tag: "net" | ["a", "b"]  `turf test --tag net` / `--skip-tag net` select by it
//   serial                   never runs beside another test (accepted, no effect yet)
//
// Values are literals. The names mean this only inside `@test(...)`.

#include "diagnostics/ErrorCodes.h"
#include "frontend/Annotation.h"
#include "sema/Types.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace TestAnnotation {

inline constexpr const char *Name = "test";

enum class Outcome { Returns, CompileError, Warning, NoWarnings, Panic, Exit, Error };

/// What one `@test(...)` expects, read from its arguments.
struct Spec {
  Outcome Kind = Outcome::Returns;
  std::string Code; ///< compile_error, warning: "E041"
  std::string Text; ///< compile_error, warning, panic: a message contains this
  bool HasFix = false;
  std::string Fix; ///< compile_error's `fix:`: a printed `Fix:` line contains this
  int Status = 0;  ///< exit
  /// error: the type as written (`ParseError`, `fs::FsError`) and the variant.
  std::string ErrorType, ErrorVariant;

  // Options (TDP-2-026).
  bool Skip = false;
  std::string SkipReason;
  int TimeoutMs = 0; ///< 0: the runner's own limit
  /// The optimization levels to build and run at (`O0`, ..., `Os`); empty: the
  /// level `turf test` was given.
  std::vector<std::string> Levels;
  bool HasStdout = false;
  std::string Stdout;
  std::vector<std::string> Tags;
  bool Serial = false;
};

inline const char *OptionList =
    "`skip:`, `timeout:`, `levels:`, `stdout:`, `tag:` and `serial`, and `fix:` "
    "after a compile_error outcome";

/// The optimization levels `levels:` names; `all` is O0 to O3.
inline bool isLevelWord(const std::string &W) {
  return W == "O0" || W == "O1" || W == "O2" || W == "O3" || W == "Os";
}

/// The word that names @p K in source, "" for Returns (which has none).
inline const char *outcomeWord(Outcome K) {
  switch (K) {
  case Outcome::Returns: return "";
  case Outcome::CompileError: return "compile_error";
  case Outcome::Warning: return "warning";
  case Outcome::NoWarnings: return "no_warnings";
  case Outcome::Panic: return "panic";
  case Outcome::Exit: return "exit";
  case Outcome::Error: return "error";
  }
  return "";
}

inline bool outcomeOf(const std::string &Word, Outcome &Out) {
  for (Outcome K : {Outcome::CompileError, Outcome::Warning, Outcome::NoWarnings,
                    Outcome::Panic, Outcome::Exit, Outcome::Error})
    if (Word == outcomeWord(K)) {
      Out = K;
      return true;
    }
  return false;
}

inline const char *OutcomeList =
    "compile_error, warning, no_warnings, panic, exit and error";

/// Reads @p Ann into @p Out. Returns what is wrong with its arguments, or ""
/// when they are well formed. Shape only: whether the function under it can
/// be a test, and whether an `error` outcome's variant exists, are checked
/// where that is known (validate() below, the Resolver).
inline std::string parse(const Annotation &Ann, Spec &Out) {
  Out = Spec();
  std::vector<const AnnotationArg *> Positional;
  std::vector<std::string> Seen;
  auto Once = [&](const std::string &Name) -> std::string {
    if (std::find(Seen.begin(), Seen.end(), Name) != Seen.end())
      return "`@test` has `" + Name + "` twice. Write each option once.";
    Seen.push_back(Name);
    return "";
  };
  auto IsWordArg = [](const AnnotationArg &A) {
    return !A.IsString && !A.IsNumber && !A.IsList;
  };
  for (const auto &A : Ann.Args) {
    if (A.Key.empty()) {
      // `serial`, the one option with no value, is a bare word anywhere
      // after the outcome.
      if (IsWordArg(A) && A.Value == "serial") {
        if (std::string Why = Once("serial"); !Why.empty())
          return Why;
        Out.Serial = true;
        continue;
      }
      Positional.push_back(&A);
      continue;
    }
    const std::string &K = A.Key;
    if (K != "fix" && K != "skip" && K != "timeout" && K != "levels" && K != "stdout" &&
        K != "tag")
      return "`@test` has no option `" + K + ":`. Its options are " + OptionList + ".";
    if (std::string Why = Once(K + ":"); !Why.empty())
      return Why;
    if (K == "skip") {
      if (!A.IsString || A.Value.empty())
        return "`skip:` takes the reason the test is set aside, a string that is not "
               "empty: `@test(skip: \"waits for #123\")`.";
      Out.Skip = true;
      Out.SkipReason = A.Value;
    } else if (K == "timeout") {
      const bool Digits = A.IsNumber && !A.Value.empty() && A.Value.size() <= 9 &&
                          A.Value.find_first_not_of("0123456789") == std::string::npos;
      if (!Digits || std::stoi(A.Value) <= 0)
        return "`timeout:` takes a whole number of milliseconds greater than 0: "
               "`@test(timeout: 2000)`.";
      Out.TimeoutMs = std::stoi(A.Value);
    } else if (K == "levels") {
      if (IsWordArg(A) && A.Value == "all") {
        Out.Levels = {"O0", "O1", "O2", "O3"};
      } else if (A.IsList && !A.Items.empty()) {
        for (const auto &I : A.Items) {
          if (!IsWordArg(I) || !isLevelWord(I.Value))
            return "`levels:` takes `all` or a list of levels from `O0`, `O1`, `O2`, "
                   "`O3` and `Os`: `@test(levels: [O0, O2])`.";
          if (std::find(Out.Levels.begin(), Out.Levels.end(), I.Value) == Out.Levels.end())
            Out.Levels.push_back(I.Value);
        }
      } else {
        return "`levels:` takes `all` or a list of levels from `O0`, `O1`, `O2`, "
               "`O3` and `Os`: `@test(levels: [O0, O2])`.";
      }
    } else if (K == "stdout") {
      if (!A.IsString)
        return "`stdout:` takes the exact text the test prints, a string: "
               "`@test(stdout: \"3\\n\")`.";
      Out.HasStdout = true;
      Out.Stdout = A.Value;
    } else if (K == "tag") {
      auto Bad = [] {
        return std::string("`tag:` takes a string or a list of strings, none of them "
                           "empty: `@test(tag: \"net\")` or `@test(tag: [\"net\", \"slow\"])`.");
      };
      if (A.IsString && !A.Value.empty()) {
        Out.Tags.push_back(A.Value);
      } else if (A.IsList && !A.Items.empty()) {
        for (const auto &I : A.Items) {
          if (!I.IsString || I.Value.empty())
            return Bad();
          Out.Tags.push_back(I.Value);
        }
      } else {
        return Bad();
      }
    }
  }
  for (size_t I = 1; I < Positional.size(); ++I) {
    Outcome K;
    if (!Positional[I]->IsString && !Positional[I]->IsNumber &&
        outcomeOf(Positional[I]->Value, K))
      return "`" + Positional[I]->Value +
             "` is an outcome, and an outcome is the first argument: `@test(" +
             Positional[I]->Value + ", ...)`.";
  }
  if (Positional.empty()) {
    if (Ann.findKeyed("fix"))
      return "`@test(fix: ...)` needs a compile_error outcome first: "
             "`@test(compile_error, E041, \"text\", fix: \"text\")`.";
    return "";
  }

  const AnnotationArg &First = *Positional[0];
  if (First.IsString || First.IsNumber || !outcomeOf(First.Value, Out.Kind))
    return "`" + First.Value + "` is not an outcome of `@test`. The outcomes are " +
           OutcomeList + ", e.g. `@test(panic, \"Division by zero\")`.";
  const std::string Word = outcomeWord(Out.Kind);
  std::vector<const AnnotationArg *> Rest(Positional.begin() + 1, Positional.end());
  const AnnotationArg *Fix = Ann.findKeyed("fix");
  if (Fix && Out.Kind != Outcome::CompileError)
    return "`fix:` belongs to a compile_error outcome: "
           "`@test(compile_error, E041, \"text\", fix: \"text\")`.";
  // A compile_error test never runs, so it has no time to limit and no output.
  if (Out.Kind == Outcome::CompileError && (Out.TimeoutMs > 0 || Out.HasStdout))
    return std::string("`") + (Out.TimeoutMs > 0 ? "timeout:" : "stdout:") +
           "` needs a test that runs, and a compile_error test is only compiled. "
           "Remove it, or give the test an outcome that runs.";

  // `E041`, which the registry must know, as an error (@p WantWarning false)
  // or a warning.
  auto ReadCode = [&](const AnnotationArg &A, bool WantWarning) -> std::string {
    const std::string &C = A.Value;
    bool Shaped = !A.IsString && !A.IsNumber && C.size() == 4 && C[0] == 'E';
    for (size_t I = 1; Shaped && I < C.size(); ++I)
      Shaped = std::isdigit(static_cast<unsigned char>(C[I])) != 0;
    if (!Shaped)
      return "`" + C + "` is not a diagnostic code: write one such as `E041`.";
    const ErrorCodeEntry *Row = ErrorCodes::lookupByCode(C);
    if (!Row)
      return "`" + C + "` is not a code the compiler has: `turf --explain " + C +
             "` lists none.";
    const bool IsWarning = ErrorCodes::isWarningCode(*Row);
    if (WantWarning && !IsWarning)
      return "`" + C + "` is an error's code, not a warning's: write `@test(compile_error, " + C +
             ", \"...\")`.";
    if (!WantWarning && IsWarning)
      return "`" + C + "` is a warning's code, not an error's: write `@test(warning, " + C +
             ", \"...\")`.";
    Out.Code = C;
    return "";
  };

  switch (Out.Kind) {
  case Outcome::Returns:
    break;
  case Outcome::CompileError:
  case Outcome::Warning: {
    const std::string Shape = "`@test(" + Word + ", E041, \"text\")`";
    if (Rest.size() != 2 || !Rest[1]->IsString)
      return "`" + Word + "` takes a code and a message: " + Shape +
             " (\"\" matches any message).";
    if (std::string Why = ReadCode(*Rest[0], Out.Kind == Outcome::Warning); !Why.empty())
      return Why;
    Out.Text = Rest[1]->Value;
    if (Fix) {
      if (!Fix->IsString)
        return "`fix:` takes a string: the text of the `Fix:` line, e.g. `fix: \"add ';'\"`.";
      Out.HasFix = true;
      Out.Fix = Fix->Value;
    }
    break;
  }
  case Outcome::NoWarnings:
    if (!Rest.empty())
      return "`no_warnings` takes nothing more: `@test(no_warnings)`.";
    break;
  case Outcome::Panic:
    if (Rest.size() != 1 || !Rest[0]->IsString)
      return "`panic` takes the panic's message: `@test(panic, \"Division by zero\")` "
             "(\"\" matches any message).";
    Out.Text = Rest[0]->Value;
    break;
  case Outcome::Exit: {
    if (Rest.size() != 1 || !Rest[0]->IsNumber)
      return "`exit` takes the exit status: `@test(exit, 3)`.";
    const std::string &N = Rest[0]->Value;
    if (N.size() > 3 || N.find_first_not_of("0123456789") != std::string::npos ||
        std::stoi(N) > 255)
      return "`" + N + "` is not an exit status: a status is a whole number from 0 to 255.";
    Out.Status = std::stoi(N);
    break;
  }
  case Outcome::Error: {
    // `ParseError::Empty`, or `fs::FsError::RemoveFileFailed` for a module's.
    const size_t Sep = Rest.size() == 1 ? Rest[0]->Value.rfind("::") : std::string::npos;
    if (Rest.size() != 1 || Rest[0]->IsString || Rest[0]->IsNumber || Sep == std::string::npos)
      return "`error` takes the error the test returns: `@test(error, ParseError::Empty)`.";
    Out.ErrorType = Rest[0]->Value.substr(0, Sep);
    Out.ErrorVariant = Rest[0]->Value.substr(Sep + 2);
    break;
  }
  }
  return "";
}

/// The `@test` annotation of @p Decl, or null.
inline const Annotation *find(const AnnotatedDecl &Decl) {
  const auto All = Decl.getAnnotations(Name);
  return All.empty() ? nullptr : All.front();
}

/// True if @p Decl is a test whose outcome is compile_error: its body is
/// expected to be rejected, so it is never emitted and its own errors are
/// the test's to judge (TestRegistry.h).
inline bool isCompileErrorTest(const AnnotatedDecl &Decl) {
  const Annotation *Ann = find(Decl);
  Spec S;
  return Ann && parse(*Ann, S).empty() && S.Kind == Outcome::CompileError;
}

inline std::string validate(const Annotation &Ann,
                            const std::vector<std::pair<std::string, TurfTypePtr>> &Params,
                            const TurfTypePtr &ReturnType, bool IsExtern,
                            const std::vector<Annotation> &All) {
  if (IsExtern)
    return "`@test` marks a function written in Turf, not an `extern fn`.";
  size_t Count = 0;
  for (const auto &A : All)
    Count += A.Name == Name;
  if (Count > 1)
    return "A function has one `@test`; write its outcome in that one.";
  Spec S;
  if (std::string Why = parse(Ann, S); !Why.empty())
    return Why;
  if (!Params.empty())
    return "A test takes no parameters: `turf test` calls it with none.";
  const bool ReturnsVoid = !ReturnType || ReturnType->isVoid();
  bool ReturnsFallibleVoid = false;
  if (ReturnType && ReturnType->isErrorUnion())
    if (const auto *EU = dynamic_cast<const TurfErrorUnionType *>(ReturnType.get()))
      ReturnsFallibleVoid = EU->getSuccessType() && EU->getSuccessType()->isVoid();
  if (!ReturnsVoid && !ReturnsFallibleVoid)
    return "A test returns `void` or `!void`, not `" + ReturnType->getName() + "`.";
  if (S.Kind == Outcome::Error && !ReturnsFallibleVoid)
    return "A test with an `error` outcome returns `!void`, so it can return the error.";
  return "";
}

} // namespace TestAnnotation
