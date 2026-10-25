#pragma once

// TestRegistry.h -- the `@test` functions one compile has seen (TDP-2-006,
// roadmap F-11d, issue #299; the annotation itself is TestAnnotation.h).
//
// Two things need to know where each test is:
//
//  - The diagnostic engine. A compile_error test's body is SUPPOSED to be
//    rejected, so an error inside the test (from its `fn` to the end of its
//    body: a missing return is reported at the `fn`) is the test's, not the
//    program's: it is
//    taken here (captureError()) instead of being printed and failing the
//    build. In a normal build only an error with the expected code is taken;
//    any other is an ordinary error. In a test build (`--test-build`) every
//    error inside a main-file compile_error test is taken, so a test that
//    fails by reporting the wrong error is reported as that test's failure,
//    with the file's other tests still run.
//
//  - The end of the compile. A normal build drops every test function from
//    the program; a test build makes the main file's tests its entry point
//    (EmitTestHarness(), Codegen.h) and writes what each one expects, and the
//    errors and warnings found inside each, for `turfc --test` to judge
//    (writeManifest()).
//
// The passes that take an error stop checking that test, as a compile stops
// after the pass that rejected a program: the Resolver and the TypeChecker
// recover around a compile_error test's own declaration and mark it
// (FuncDefExprAST::TestBodyFailed), and code generation never emits one.

#include "frontend/Lexer.h"
#include "frontend/TestAnnotation.h"

#include <string>
#include <vector>

class FuncDefExprAST;

namespace TestRegistry {

enum class Mode {
  /// Every error is reported as it is: an analysis that must see the
  /// program's real errors (the compiler-fix check, CompilerFix.cpp).
  Raw,
  /// A normal build: a compile_error test's expected errors are its own.
  Build,
  /// `--test-build`: every error inside a main-file compile_error test is
  /// that test's, and the warnings inside each main-file test are recorded.
  TestBuild,
};

struct Test {
  std::string File;
  std::string Name;
  /// The 'fn', and the first token after the body's '}': a diagnostic from
  /// one up to the other is inside the test. Its annotation is not, so a
  /// malformed `@test` is always an ordinary error.
  SourceLocation DeclLoc{0, 0, ""};
  SourceLocation BodyEnd{0, 0, ""};
  /// False when the annotation's arguments are malformed: the Resolver
  /// reports that (E146), and nothing is taken for such a test.
  bool SpecOk = false;
  TestAnnotation::Spec Spec;
  bool ReturnsError = false; ///< declared `!void`
  /// error: the tag the program reports that error by, `<type identity>::
  /// <variant>` (`fs::FsError::RemoveFileFailed`, `ParseError::Empty`), set by
  /// the Resolver once it knows which type the test named.
  std::string ErrorTag;
  /// compile_error: the errors reported inside the test.
  std::vector<DiagnosticEngine::Diag> Errors;
  /// Test build, main file: the warnings reported inside the test.
  std::vector<DiagnosticEngine::Diag> Warnings;
  /// The emitted function's symbol; empty for a compile_error test, which is
  /// never emitted.
  std::string Symbol;
};

struct State {
  Mode M = Mode::Build;
  std::vector<Test> Tests;
  bool LastErrorCaptured = false;
};

Mode mode();
void setMode(Mode M);

/// Called by the Parser for every function carrying `@test`, in every file it
/// parses. A file parsed twice registers once.
void noteTest(const FuncDefExprAST &F);

/// Whether error @p D, about to be recorded, belongs to a compile_error test
/// (see the file comment); if so it is kept with that test and the caller
/// records nothing. Every call also sets lastErrorCaptured().
bool captureError(const DiagnosticEngine::Diag &D);

/// True if the last error passed to captureError() was taken. A pass that
/// recovers around a compile_error test's declaration rethrows when it was
/// not: an ordinary error unwinds as it always does.
bool lastErrorCaptured();

/// Whether warning @p D lies inside a compile_error test, where it is part
/// of the mistake the test demonstrates and is not shown.
bool swallowWarning(const DiagnosticEngine::Diag &D);

/// A warning that is being recorded: in a test build, kept with the
/// main-file test it lies inside.
void noteWarning(const DiagnosticEngine::Diag &D);

/// The Resolver's answer for an `error` outcome: see Test::ErrorTag.
void noteErrorTag(const FuncDefExprAST &F, const std::string &Tag);

/// The symbol code generation gave test @p F.
void noteEmitted(const FuncDefExprAST &F, const std::string &Symbol);

/// Every test registered, in the order the Parser met them.
std::vector<Test> &tests();

/// The main file's tests, in source order.
std::vector<const Test *> mainFileTests();

/// Writes the test build's manifest, the JSON `turfc --test` reads (shape in
/// TestRegistry.cpp). @p RunnableOrder lists the symbols in the order the
/// test program numbers them. Returns false if the file cannot be written.
bool writeManifest(const std::string &Path, bool HasMain,
                   const std::vector<std::string> &RunnableOrder);

/// The query facade analyses in a scope of its own (AnalysisFacade.cpp): it
/// saves this state, starts an empty registry in the same mode, and puts the
/// saved state back afterwards.
State save();
void restore(State S);

} // namespace TestRegistry
