#ifndef TURF_TEST_RUNNER_H
#define TURF_TEST_RUNNER_H

// REL-002 (docs/development/ROADMAP_v2.0.0.md): `turfc --test`, the one
// implementation of how a TurfLang test is judged. `turf test` drives it for a
// project, and scripts/test_runner.sh drives it for the compiler's own suite,
// so the conventions below have exactly one home.
//
// A test is a `.tr` file under the tests directory. Each is compiled with this
// same `turfc`, in a child process (the compiler keeps its state in globals, so
// two compiles in one process would see each other), and judged by its name:
//   - a plain file must compile and then exit 0;
//   - `fail_*.tr` must fail to compile (a crash of the compiler is not a
//     rejection);
//   - `fail_*.tr` containing `RUNTIME_ABORT_EXPECTED` must compile, then abort;
//   - `// EXPECT-DIAG`, `// EXPECT-NO-DIAG`, `// EXPECT-RUNTIME` and
//     `// EXPECT-EXIT` markers pin *why* (TestExpectations.h);
//   - `*_module.tr` files are companions to be imported, never run.
// Every test runs with its own directory as the working directory.

#include <string>
#include <vector>

struct TestRunOptions {
  /// This compiler's own path: what a test is compiled with.
  std::string Compiler;
  /// The tests directory. The caller has decided what the default is; it must
  /// exist, or the run is an error (a missing tests/ is never a silent pass).
  std::string Dir = "tests";
  /// `--package-graph` file handed to every test compile (a project's tests),
  /// or empty for single-file mode (the compiler's own suite).
  std::string PackageGraph;
  /// Flags forwarded to every test compile: -O*, -g, --link <lib>,
  /// --allow-unsafe-modules, --stdlib-warnings.
  std::vector<std::string> CompileFlags;
  /// Paths relative to Dir that are skipped (a directory skips everything
  /// under it).
  std::vector<std::string> Excludes;
  /// Paths relative to Dir to run, each a test file or a directory of tests;
  /// empty runs every test. One that names no test is an error, so a
  /// misspelled path never passes as "nothing to run".
  std::vector<std::string> Only;
  /// The diagnostic register to enforce (tests/DIAGNOSTIC_GAPS.txt), or empty.
  /// Register keys spell a test `<Dir>/<path under Dir>`.
  std::string DiagRegister;
  /// Tests run at once. 0 means the TURF_TEST_JOBS environment variable if it
  /// is set, else every processor.
  int Jobs = 0;
  /// Seconds a test program may run. 0 means TURF_TEST_TIMEOUT_SEC if set,
  /// else 5.
  int TimeoutSec = 0;
};

/// Runs every test under Opts.Dir and prints one line per test and a summary.
/// Returns 0 when every test passed, 1 when one failed or the run could not
/// start (no tests directory, no tests, an unreadable register).
int RunTests(const TestRunOptions &Opts);

#endif
