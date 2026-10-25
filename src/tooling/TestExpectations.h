#ifndef TURF_TEST_EXPECTATIONS_H
#define TURF_TEST_EXPECTATIONS_H

// REL-002: the pure half of `turfc --test` -- reading what a test file says it
// expects (the `// EXPECT-*` marker comments, LEC-005) and judging compiler or
// program output against it. Nothing here starts a process or touches the
// file system, so the rules are one small, separately readable unit; the
// runner (TestRunner.cpp) decides what to run and feeds the results in.
//
// Every check returns the problems it found, one human-readable line each, and
// an empty vector means the expectation holds. The wording is the wording
// scripts/test_runner.sh printed before it became a wrapper over `--test`.

#include <map>
#include <string>
#include <vector>

namespace TestExpect {

/// Removes ANSI colour sequences (`ESC [ ... m`), so a marker can be matched
/// against prose without knowing whether the compiler coloured its output.
std::string StripAnsi(const std::string &Text);

/// Whether @p Source has any `// RUNTIME_ABORT_EXPECTED` marker (a plain
/// substring test, as the runner always did).
bool IsRuntimeAbortTest(const std::string &Source);

/// `// EXPECT-DIAG: E011` / `// EXPECT-DIAG: some text` /
/// `// EXPECT-NO-DIAG: some text`, matched against the compile output.
std::vector<std::string> CheckDiag(const std::string &Source, const std::string &CompileOutput);

/// `// EXPECT-FIX: <description>` (TDP-2-001): each marker names a `Fix:` line
/// of the compile output exactly. With @p RequireAllPinned (the compiler's own
/// suite, which runs with its register) each `Fix:` line the compiler printed
/// must also be named by a marker: a fix is a promise, so none goes unpinned.
/// A project's own tests opt in by writing the marker.
std::vector<std::string> CheckFix(const std::string &Source, const std::string &CompileOutput,
                                  bool RequireAllPinned);

/// Whether @p Source has any `// EXPECT-FIX:` marker.
bool HasFixMarker(const std::string &Source);

/// How many `Fix:` lines the compile output has.
size_t CountFixes(const std::string &CompileOutput);

/// `// EXPECT-RUNTIME: text`, matched against the program's own output.
std::vector<std::string> CheckRuntime(const std::string &Source, const std::string &ProgramOutput);

/// `// EXPECT-EXIT: <status>`, for an abort that is silent by design.
std::vector<std::string> CheckExit(const std::string &Source, int Status);

/// TDP-2-006: whether @p Source declares a test function, a line that starts
/// with `@test` (after blanks). The runner builds such a file's tests with
/// `--test-build`; a comment or string that mentions `@test` mid-line does not
/// count.
bool HasTestFunctions(const std::string &Source);

/// One `@test` function as a test build's manifest describes it
/// (src/frontend/TestRegistry.cpp has the format).
struct TestCase {
  struct Diag {
    std::string Code, Message, Fix;
    int Line = 0;
  };
  std::string Name;
  int Line = 0;
  /// "returns", or the outcome word: compile_error, warning, no_warnings,
  /// panic, exit, error.
  std::string Outcome;
  std::string Code, Text, Error;
  bool HasFix = false;
  std::string Fix;
  int Status = 0;
  /// The number the test program runs it by, -1 when it is not run.
  int Run = -1;
  std::vector<Diag> Errors, Warnings;
};

/// Reads a manifest. On failure returns false and sets @p Error.
bool ParseManifest(const std::string &Text, std::vector<TestCase> &Cases, bool &HasMain,
                   std::string &Error);

/// A compile_error test: the errors inside its body against its outcome.
std::vector<std::string> JudgeCompileError(const TestCase &C);

/// A warning or no_warnings test, before it runs: the warnings inside its
/// body against its outcome. Empty for any other outcome.
std::vector<std::string> JudgeWarnings(const TestCase &C);

/// How test @p C's own process ended against its outcome. @p Output is what
/// it printed, with the line the test program reports through.
std::vector<std::string> JudgeRun(const TestCase &C, int Status, bool TimedOut,
                                  const std::string &Output);

/// @p Output without the lines the test program reports through.
std::string ProgramOutput(const std::string &Output);

/// The register of tests whose diagnostic is not pinned by an error code
/// (tests/DIAGNOSTIC_GAPS.txt, issue #263). It is a policy of the compiler's
/// own suite, so `--test` applies it only when handed one; an ordinary
/// project's `fail_*` tests need no register.
class Register {
public:
  /// Reads @p Path. On failure returns false and sets @p Error.
  bool load(const std::string &Path, std::string &Error);

  /// How many entries of @p Kind (UNPINNED, NO-CODE, ...) the file holds.
  size_t count(const std::string &Kind) const;

  /// The policy for a compile-time `fail_*` test: it pins an error code or has
  /// a register entry, and every entry still tells the truth about the real
  /// output. @p Key is the register's spelling of the test's path.
  std::vector<std::string> checkDiagPolicy(const std::string &Source, const std::string &Key,
                                           const std::string &CompileOutput) const;

  /// The same for a `RUNTIME_ABORT_EXPECTED` test: it must say what it aborts
  /// with (`EXPECT-RUNTIME` or `EXPECT-EXIT`), or be a listed UNPINNED.
  std::vector<std::string> checkRuntimePolicy(const std::string &Source,
                                              const std::string &Key) const;

private:
  struct Entry {
    std::string Kind;
    std::string Line;
  };
  std::map<std::string, std::vector<Entry>> ByKey_;
  std::map<std::string, size_t> Counts_;
};

} // namespace TestExpect

#endif
