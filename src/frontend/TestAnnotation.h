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

#include "diagnostics/ErrorCodes.h"
#include "frontend/Annotation.h"
#include "sema/Types.h"

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
};

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
  for (const auto &A : Ann.Args) {
    if (!A.Key.empty()) {
      if (A.Key != "fix")
        return "'@test' has no option '" + A.Key +
               ":'. Its one option is 'fix:', after a compile_error outcome.";
      continue;
    }
    Positional.push_back(&A);
  }
  for (size_t I = 1; I < Positional.size(); ++I) {
    Outcome K;
    if (!Positional[I]->IsString && !Positional[I]->IsNumber &&
        outcomeOf(Positional[I]->Value, K))
      return "'" + Positional[I]->Value +
             "' is an outcome, and an outcome is the first argument: '@test(" +
             Positional[I]->Value + ", ...)'.";
  }
  if (Positional.empty()) {
    if (!Ann.Args.empty())
      return "'@test(fix: ...)' needs a compile_error outcome first: "
             "'@test(compile_error, E041, \"text\", fix: \"text\")'.";
    return "";
  }

  const AnnotationArg &First = *Positional[0];
  if (First.IsString || First.IsNumber || !outcomeOf(First.Value, Out.Kind))
    return "'" + First.Value + "' is not an outcome of '@test'. The outcomes are " +
           OutcomeList + ", e.g. '@test(panic, \"Division by zero\")'.";
  const std::string Word = outcomeWord(Out.Kind);
  std::vector<const AnnotationArg *> Rest(Positional.begin() + 1, Positional.end());
  const AnnotationArg *Fix = Ann.findKeyed("fix");
  if (Fix && Out.Kind != Outcome::CompileError)
    return "'fix:' belongs to a compile_error outcome: "
           "'@test(compile_error, E041, \"text\", fix: \"text\")'.";

  // `E041`, which the registry must know, as an error (@p WantWarning false)
  // or a warning.
  auto ReadCode = [&](const AnnotationArg &A, bool WantWarning) -> std::string {
    const std::string &C = A.Value;
    bool Shaped = !A.IsString && !A.IsNumber && C.size() == 4 && C[0] == 'E';
    for (size_t I = 1; Shaped && I < C.size(); ++I)
      Shaped = std::isdigit(static_cast<unsigned char>(C[I])) != 0;
    if (!Shaped)
      return "'" + C + "' is not a diagnostic code: write one such as 'E041'.";
    const ErrorCodeEntry *Row = ErrorCodes::lookupByCode(C);
    if (!Row)
      return "'" + C + "' is not a code the compiler has: `turf --explain " + C +
             "` lists none.";
    const bool IsWarning = ErrorCodes::isWarningCode(*Row);
    if (WantWarning && !IsWarning)
      return "'" + C + "' is an error's code, not a warning's: write '@test(compile_error, " + C +
             ", \"...\")'.";
    if (!WantWarning && IsWarning)
      return "'" + C + "' is a warning's code, not an error's: write '@test(warning, " + C +
             ", \"...\")'.";
    Out.Code = C;
    return "";
  };

  switch (Out.Kind) {
  case Outcome::Returns:
    break;
  case Outcome::CompileError:
  case Outcome::Warning: {
    const std::string Shape = "'@test(" + Word + ", E041, \"text\")'";
    if (Rest.size() != 2 || !Rest[1]->IsString)
      return "'" + Word + "' takes a code and a message: " + Shape +
             " (\"\" matches any message).";
    if (std::string Why = ReadCode(*Rest[0], Out.Kind == Outcome::Warning); !Why.empty())
      return Why;
    Out.Text = Rest[1]->Value;
    if (Fix) {
      if (!Fix->IsString)
        return "'fix:' takes a string: the text of the 'Fix:' line, e.g. 'fix: \"add ';'\"'.";
      Out.HasFix = true;
      Out.Fix = Fix->Value;
    }
    break;
  }
  case Outcome::NoWarnings:
    if (!Rest.empty())
      return "'no_warnings' takes nothing more: '@test(no_warnings)'.";
    break;
  case Outcome::Panic:
    if (Rest.size() != 1 || !Rest[0]->IsString)
      return "'panic' takes the panic's message: '@test(panic, \"Division by zero\")' "
             "(\"\" matches any message).";
    Out.Text = Rest[0]->Value;
    break;
  case Outcome::Exit: {
    if (Rest.size() != 1 || !Rest[0]->IsNumber)
      return "'exit' takes the exit status: '@test(exit, 3)'.";
    const std::string &N = Rest[0]->Value;
    if (N.size() > 3 || N.find_first_not_of("0123456789") != std::string::npos ||
        std::stoi(N) > 255)
      return "'" + N + "' is not an exit status: a status is a whole number from 0 to 255.";
    Out.Status = std::stoi(N);
    break;
  }
  case Outcome::Error: {
    // `ParseError::Empty`, or `fs::FsError::RemoveFileFailed` for a module's.
    const size_t Sep = Rest.size() == 1 ? Rest[0]->Value.rfind("::") : std::string::npos;
    if (Rest.size() != 1 || Rest[0]->IsString || Rest[0]->IsNumber || Sep == std::string::npos)
      return "'error' takes the error the test returns: '@test(error, ParseError::Empty)'.";
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
    return "'@test' marks a function written in Turf, not an 'extern fn'.";
  size_t Count = 0;
  for (const auto &A : All)
    Count += A.Name == Name;
  if (Count > 1)
    return "A function has one '@test'; write its outcome in that one.";
  Spec S;
  if (std::string Why = parse(Ann, S); !Why.empty())
    return Why;
  if (!Params.empty())
    return "A test takes no parameters: 'turf test' calls it with none.";
  const bool ReturnsVoid = !ReturnType || ReturnType->isVoid();
  bool ReturnsFallibleVoid = false;
  if (ReturnType && ReturnType->isErrorUnion())
    if (const auto *EU = dynamic_cast<const TurfErrorUnionType *>(ReturnType.get()))
      ReturnsFallibleVoid = EU->getSuccessType() && EU->getSuccessType()->isVoid();
  if (!ReturnsVoid && !ReturnsFallibleVoid)
    return "A test returns 'void' or '!void', not '" + ReturnType->getName() + "'.";
  if (S.Kind == Outcome::Error && !ReturnsFallibleVoid)
    return "A test with an 'error' outcome returns '!void', so it can return the error.";
  return "";
}

} // namespace TestAnnotation
