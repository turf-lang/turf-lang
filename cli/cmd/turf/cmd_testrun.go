package main

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"

	"github.com/turf-lang/turf-lang/cli/internal/driver"
	"github.com/turf-lang/turf-lang/cli/internal/project"
	"github.com/turf-lang/turf-lang/cli/internal/version"
)

// testsDirName is the one place a project's tests live (FREEZE.md 6.16's
// layout); there is no manifest key to move it.
const testsDirName = "tests"

// cmdTest runs the project's tests: `turfc --test tests` over the package
// graph, from the project root.
//
// How a test is judged is the compiler's, not turf's (TestRunner.h): turf never
// reads TurfLang source, so the conventions (fail_*, RUNTIME_ABORT_EXPECTED, the
// EXPECT-* markers) live in exactly one place, and `scripts/test_runner.sh` runs
// the same code for the compiler's own suite. What turf adds is the project: it
// finds tests/, resolves the dependencies, and hands the compiler a graph in
// which a test file belongs to the root package, so a test can import the
// package's own modules and its dependencies.
//
// A project without tests/ is an error, never a silent pass: "0 tests, all
// green" is exactly the answer that hides a project nobody ever tested.
func cmdTest(args []string) int {
	nf, flags := splitNetFlags(args)
	if code := checkProjectFlags(flags); code != 0 {
		return code
	}

	// Checked before anything that costs time or the network (the compiler
	// handshake, resolving or fetching dependencies), like "no turf.toml".
	cwd, err := os.Getwd()
	if err != nil {
		return failf("cannot determine the current directory: %v", err)
	}
	root, err := project.FindRoot(cwd)
	if errors.Is(err, project.ErrNoProject) {
		return usagef("%s", noProjectMsg)
	} else if err != nil {
		return failf("%v", err)
	}
	dir := filepath.Join(root, testsDirName)
	if st, err := os.Stat(dir); err != nil || !st.IsDir() {
		return failf("this project has no %s/ directory (looked for %s)\n"+
			"hint: create it and put test files in it, for example %s/basic.tr:\n\n%s",
			testsDirName, dir, testsDirName, indent(testTemplate, "        "))
	}

	return withProjectGraph(nf, true, func(p *projectBuild) int {
		info, err := driver.Probe(p.compiler)
		if err != nil {
			return failf("%v", err)
		}
		if err := testInterfaceError(info, p.compiler); err != nil {
			return failf("%v", err)
		}
		defer catchInterrupt()()
		runArgs := append([]string{"--test", testsDirName, "--package-graph", p.graphArg()}, flags...)
		return driver.RunIn(p.dir, p.compiler, runArgs)
	})
}

// testInterfaceError is nil when the compiler has `--test`. A compiler that
// predates it would answer `turfc --test` with "Unknown option", which says
// nothing about the real problem (an old turfc), so it is asked up front.
func testInterfaceError(info driver.CompilerInfo, compiler string) error {
	need := version.RequiredForTest
	if info.Interface.Major == need.Major && info.Interface.Minor >= need.Minor {
		return nil
	}
	return fmt.Errorf("`turf test` needs a compiler that speaks driver interface %d.%d or a later %d.x, "+
		"but turfc %s (%s) speaks %d.%d\nhint: upgrade turfc, or point TURF_COMPILER at a newer one",
		need.Major, need.Minor, need.Major, info.Release, compiler,
		info.Interface.Major, info.Interface.Minor)
}

// testTemplate is what the "no tests/" hint shows: the smallest test that
// passes, using the stdlib's `test` module (a bare `assert` is not in the
// prelude).
const testTemplate = `import test;

fn main(): int {
    test::assert(1 + 1 == 2, "arithmetic still works");
    return 0;
}
`

const helpTest = `turf test - run the project's tests

Usage:
  turf test [--offline] [--locked] [test flags...] [compiler flags...]

Run anywhere inside a project. Every .tr file under the project's tests/
directory (at any depth) is compiled and run by 'turfc --test', in parallel,
with its own directory as the working directory. A project with no tests/
directory, or with no tests in it, is an error.

How a test is judged is by its file name:

  name.tr                   must compile, then exit with status 0
  fail_name.tr              must fail to compile (a compiler crash does not count)
  fail_name.tr containing   must compile, then abort (exit non-zero)
    RUNTIME_ABORT_EXPECTED
  name_module.tr            a companion to import from a test; never run

and comments in the file can pin why, not only whether:

  // EXPECT-DIAG: E011          the compile output has this error code
  // EXPECT-DIAG: some text     ... or this text (use it for a warning)
  // EXPECT-NO-DIAG: some text  ... and must not have this text
  // EXPECT-RUNTIME: some text  the aborting program printed this text
  // EXPECT-EXIT: 134           the aborting program exited with this status

A test imports what the package itself can: its own modules under src/, its
direct dependencies and the standard library, plus helper modules (*_module.tr)
beside it in tests/. The project is resolved first, so --offline and --locked
apply as for 'turf build'.

Test flags, handed to the compiler's runner unchanged:

  --jobs <n>          tests running at once (default: every processor, or
                      TURF_TEST_JOBS)
  --timeout <sec>     how long one test program may run (default 5, or
                      TURF_TEST_TIMEOUT_SEC)
  --exclude <path>    skip this file or directory, relative to tests/ (repeatable)

Compiler flags such as -O2, -g and --link <lib> are applied to every test's
compile. -o and --package-graph are refused: turf owns them. The exit status is
0 when every test passed and 1 when one failed or nothing could run.
`
