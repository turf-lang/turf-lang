// TDP-2-006: the `@test` functions one compile has seen. See TestRegistry.h.

#include "frontend/TestRegistry.h"

#include "frontend/AST.h"

#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cctype>
#include <fstream>

namespace TestRegistry {

namespace {

State g_State;

bool before(const SourceLocation &A, const SourceLocation &B) {
  return A.Line < B.Line || (A.Line == B.Line && A.Col < B.Col);
}

bool inside(const Test &T, const DiagnosticEngine::Diag &D) {
  return T.BodyEnd.Line > 0 && D.Filename == T.File && !before(D.Loc, T.DeclLoc) &&
         before(D.Loc, T.BodyEnd);
}

Test *testAround(const DiagnosticEngine::Diag &D) {
  for (auto &T : g_State.Tests)
    if (inside(T, D))
      return &T;
  return nullptr;
}

/// The same diagnostic again: the main file is parsed and checked twice
/// (main.cpp's pre-pass and main pass), and both report.
void keepOnce(std::vector<DiagnosticEngine::Diag> &List, const DiagnosticEngine::Diag &D) {
  for (const auto &K : List)
    if (K.Loc.Line == D.Loc.Line && K.Loc.Col == D.Loc.Col && K.Code == D.Code &&
        K.Msg == D.Msg)
      return;
  List.push_back(D);
}

std::string plain(const std::string &S) {
  std::string Out;
  for (size_t I = 0; I < S.size(); ++I) {
    if (S[I] == '\x1b' && I + 1 < S.size() && S[I + 1] == '[') {
      I += 2;
      while (I < S.size() && !std::isalpha(static_cast<unsigned char>(S[I])))
        ++I;
      continue;
    }
    Out += S[I];
  }
  return Out;
}

} // namespace

Mode mode() { return g_State.M; }
void setMode(Mode M) { g_State.M = M; }

void noteTest(const FuncDefExprAST &F) {
  const Annotation *Ann = TestAnnotation::find(F);
  if (!Ann)
    return;
  const std::string &File = F.getLoc().Filename;
  for (const auto &T : g_State.Tests)
    if (T.File == File && T.DeclLoc.Line == F.getLoc().Line && T.DeclLoc.Col == F.getLoc().Col)
      return;
  Test T;
  T.File = File;
  T.Name = F.getName();
  T.DeclLoc = F.getLoc();
  T.BodyEnd = F.BodyEnd;
  T.SpecOk = TestAnnotation::parse(*Ann, T.Spec).empty();
  T.ReturnsError = F.getReturnType() && F.getReturnType()->isErrorUnion();
  g_State.Tests.push_back(std::move(T));
}

bool captureError(const DiagnosticEngine::Diag &D) {
  g_State.LastErrorCaptured = false;
  if (g_State.M == Mode::Raw)
    return false;
  Test *T = testAround(D);
  if (!T || !T->SpecOk || T->Spec.Kind != TestAnnotation::Outcome::CompileError)
    return false;
  const bool TakeAll = g_State.M == Mode::TestBuild && D.Filename == MainSourceFilename;
  if (!TakeAll && D.Code != T->Spec.Code)
    return false;
  keepOnce(T->Errors, D);
  g_State.LastErrorCaptured = true;
  return true;
}

bool lastErrorCaptured() { return g_State.LastErrorCaptured; }

bool ownsWarning(const DiagnosticEngine::Diag &D) {
  if (g_State.M == Mode::Raw)
    return false;
  const Test *T = testAround(D);
  return T && T->SpecOk && T->Spec.Kind == TestAnnotation::Outcome::Warning &&
         D.Code == T->Spec.Code;
}

bool swallowWarning(const DiagnosticEngine::Diag &D) {
  if (g_State.M == Mode::Raw)
    return false;
  const Test *T = testAround(D);
  if (!T || !T->SpecOk)
    return false;
  if (T->Spec.Kind == TestAnnotation::Outcome::CompileError)
    return true;
  // A warning test's own warning is shown only where it is judged: in a test
  // build, inside a main-file test (noteWarning()).
  return ownsWarning(D) &&
         !(g_State.M == Mode::TestBuild && D.Filename == MainSourceFilename);
}

void noteWarning(const DiagnosticEngine::Diag &D) {
  if (g_State.M != Mode::TestBuild || D.Filename != MainSourceFilename)
    return;
  if (Test *T = testAround(D))
    keepOnce(T->Warnings, D);
}

void noteErrorTag(const FuncDefExprAST &F, const std::string &Tag) {
  for (auto &T : g_State.Tests)
    if (T.File == F.getLoc().Filename && T.DeclLoc.Line == F.getLoc().Line &&
        T.DeclLoc.Col == F.getLoc().Col)
      T.ErrorTag = Tag;
}

void noteEmitted(const FuncDefExprAST &F, const std::string &Symbol) {
  for (auto &T : g_State.Tests)
    if (T.File == F.getLoc().Filename && T.DeclLoc.Line == F.getLoc().Line &&
        T.DeclLoc.Col == F.getLoc().Col)
      T.Symbol = Symbol;
}

std::vector<Test> &tests() { return g_State.Tests; }

std::vector<const Test *> mainFileTests() {
  std::vector<const Test *> Out;
  for (const auto &T : g_State.Tests)
    if (T.File == MainSourceFilename)
      Out.push_back(&T);
  std::stable_sort(Out.begin(), Out.end(), [](const Test *A, const Test *B) {
    return before(A->DeclLoc, B->DeclLoc);
  });
  return Out;
}

// The manifest, one JSON object:
//
//   {"format": 1, "has_main": true, "tests": [
//     {"name": "parses_a_port", "line": 12, "outcome": "returns",
//      "returns_error": false, "run": 0,
//      "code": "E041", "text": "...", "fix": "...", "status": 3,
//      "error": "ParseError::Empty",
//      "errors": [{"code": "E041", "line": 14, "message": "...", "fix": "..."}],
//      "warnings": [{"code": "E172", "line": 13, "message": "..."}]}]}
//
// `outcome` is "returns" or the outcome word; `code`, `text`, `fix`, `status`
// and `error` appear when the outcome has them. `run` is the number the test
// program runs the test by (`./prog 0`), or -1 for a test it does not run (a
// compile_error test). `errors` (compile_error only) and `warnings` are what
// was reported inside the test; an error's `fix` is the description of the
// fix the compiler checked for it, "" when it has none.
bool writeManifest(const std::string &Path, bool HasMain,
                   const std::vector<std::string> &RunnableOrder) {
  using TestAnnotation::Outcome;
  // A copy: checking a fix runs an analysis, which sets this registry aside
  // and puts it back (save()/restore()), moving every test it holds.
  std::vector<Test> Mine;
  for (const Test *T : mainFileTests())
    Mine.push_back(*T);
  llvm::json::Array Tests;
  for (const Test &Each : Mine) {
    const Test *T = &Each;
    llvm::json::Object O;
    O["name"] = T->Name;
    O["line"] = T->DeclLoc.Line;
    const char *Word = TestAnnotation::outcomeWord(T->Spec.Kind);
    O["outcome"] = *Word ? Word : "returns";
    O["returns_error"] = T->ReturnsError;
    int Run = -1;
    for (size_t I = 0; I < RunnableOrder.size(); ++I)
      if (!T->Symbol.empty() && RunnableOrder[I] == T->Symbol)
        Run = static_cast<int>(I);
    O["run"] = Run;
    switch (T->Spec.Kind) {
    case Outcome::CompileError:
    case Outcome::Warning:
      O["code"] = T->Spec.Code;
      O["text"] = T->Spec.Text;
      if (T->Spec.HasFix)
        O["fix"] = T->Spec.Fix;
      break;
    case Outcome::Panic:
      O["text"] = T->Spec.Text;
      break;
    case Outcome::Exit:
      O["status"] = T->Spec.Status;
      break;
    case Outcome::Error:
      // As the program reports the error value (Test::ErrorTag).
      O["error"] = !T->ErrorTag.empty() ? T->ErrorTag
                                        : T->Spec.ErrorType + "::" + T->Spec.ErrorVariant;
      break;
    case Outcome::Returns:
    case Outcome::NoWarnings:
      break;
    }
    llvm::json::Array Errors;
    for (DiagnosticEngine::Diag D : T->Errors) {
      // Only a test that names a fix pays for checking one (each check
      // analyses the program again, and a compile checks at most a few).
      if (T->Spec.HasFix)
        DiagnosticEngine::checkFix(D);
      Errors.push_back(llvm::json::Object{{"code", D.Code},
                                          {"line", D.Loc.Line},
                                          {"message", plain(D.Msg)},
                                          {"fix", D.Fix.Description}});
    }
    O["errors"] = std::move(Errors);
    llvm::json::Array Warnings;
    for (const auto &D : T->Warnings)
      Warnings.push_back(llvm::json::Object{
          {"code", D.Code}, {"line", D.Loc.Line}, {"message", plain(D.Msg)}});
    O["warnings"] = std::move(Warnings);
    Tests.push_back(std::move(O));
  }
  llvm::json::Object Manifest{{"format", 1}, {"has_main", HasMain}, {"tests", std::move(Tests)}};
  std::ofstream Out(Path, std::ios::trunc | std::ios::binary);
  if (!Out)
    return false;
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  OS << llvm::json::Value(std::move(Manifest)) << "\n";
  OS.flush();
  Out << Text;
  return static_cast<bool>(Out);
}

State save() { return g_State; }
void restore(State S) { g_State = std::move(S); }

} // namespace TestRegistry
