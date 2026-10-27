#!/bin/bash
#
# test_cmd_smoke.sh -- REL-002: `turfc --test` and `turf test`, the one
# implementation of how a TurfLang test is judged (src/tooling/TestRunner.cpp,
# src/tooling/TestExpectations.cpp; docs/development/FREEZE.md 6.22).
#
# The compiler's own suite (scripts/test_runner.sh, now a wrapper over
# `turfc --test`) proves the runner agrees with the conventions on 1100+ real
# tests, but only on files that PASS: a runner that accepted everything would
# keep it green. This script is the other direction. Every convention is
# broken on purpose, in a fixture of its own, and the run must fail with the
# message about THAT convention (AGENTS.md: a failure for the wrong reason is
# not a pass). It also covers what the suite cannot: the missing/empty tests
# directory rule, option errors and exit statuses, and `turf test` on a real
# project (a test importing the package's own module, a dependency and a helper
# module, `tests/` required).
#
# Usage: scripts/smoke/test_cmd_smoke.sh  (after building build/turfc and build/turf)
# Exit:  0 = all checks pass (the `turf test` half is a SKIP, not a pass, when
#            build/turf is absent and CI is not set: Go is optional locally);
#        1 = a check failed (each failure is printed).

set -u
cd "$(dirname "$0")/../.." || exit 1
ROOT=$(pwd)
TURF="$ROOT/build/turf"
TURFC="$ROOT/build/turfc"

[ -x "$TURFC" ] || { echo "FAIL: $TURFC not built"; exit 1; }
# Which compiler and stdlib run must follow from the binaries' own location,
# not from a developer's shell.
unset TURF_HOME TURF_COMPILER TURF_TEST_JOBS TURF_TEST_TIMEOUT_SEC

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
export TURF_CACHE_DIR="$WORK/cache"
FAILED=0
fail() { echo "FAIL: $1"; FAILED=$((FAILED + 1)); }

strip() { sed 's/\x1b\[[0-9;]*m//g' "$1"; }
# c <name> <dir> <turfc args...>: run turfc in <dir>; output and status are
# kept under $WORK/_<name>.{out,rc}. Never reads the terminal.
c() {
  local n=$1 d=$2
  shift 2
  (cd "$d" && "$TURFC" "$@") > "$WORK/_$n.out" 2>&1 < /dev/null
  echo $? > "$WORK/_$n.rc"
}
t() {
  local n=$1 d=$2
  shift 2
  (cd "$d" && "$TURF" "$@") > "$WORK/_$n.out" 2>&1 < /dev/null
  echo $? > "$WORK/_$n.rc"
}
expect_rc() { [ "$(cat "$WORK/_$2.rc")" = "$3" ] || fail "$1: exit $(cat "$WORK/_$2.rc"), expected $3 (got: $(strip "$WORK/_$2.out" | head -c 500))"; }
expect_out() {
  strip "$WORK/_$2.out" | grep -qF -- "$3" ||
    fail "$1: output lacks '$3' (got: $(strip "$WORK/_$2.out" | head -c 500))"
}
expect_no_out() {
  if strip "$WORK/_$2.out" | grep -qF -- "$3"; then fail "$1: output must not contain '$3'"; fi
}

# fixture <dir> <file under dir> <<source on stdin>>
fixture() {
  mkdir -p "$1/$(dirname "$2")"
  cat > "$1/$2"
}

# --- the tests directory is required ---------------------------------------------
mkdir -p "$WORK/nodir" "$WORK/empty" "$WORK/onlycompanion"
c missing "$WORK/nodir" --test
expect_rc "no tests/ at all" missing 1
expect_out "no tests/ at all" missing "no tests directory: 'tests' does not exist"
c missing_named "$WORK/nodir" --test somewhere/else
expect_rc "a named directory that does not exist" missing_named 1
expect_out "a named directory that does not exist" missing_named "'somewhere/else' does not exist"
c empty "$WORK/nodir" --test "$WORK/empty"
expect_rc "an empty tests directory" empty 1
expect_out "an empty tests directory" empty "no tests found"
fixture "$WORK/onlycompanion" "tests/helper_module.tr" <<'EOF'
pub fn one(): int { return 1; }
EOF
c companion "$WORK/onlycompanion" --test
expect_rc "a directory with only a companion module" companion 1
expect_out "a directory with only a companion module" companion "no tests found"

# --- command-line errors are usage errors (status 2), nothing runs -----------------
c file_arg "$WORK/nodir" --test foo.tr
expect_rc "--test with a file" file_arg 2
expect_out "--test with a file" file_arg "takes a directory of tests, not a file"
c two_dirs "$WORK/nodir" --test a b
expect_rc "--test with two directories" two_dirs 2
c typo "$WORK/nodir" --test --jbos 2
expect_rc "a misspelled runner option" typo 2
expect_out "a misspelled runner option" typo "did you mean '--jobs'?"
c jobs0 "$WORK/nodir" --test --jobs 0
expect_rc "--jobs 0" jobs0 2
c jobs_missing "$WORK/nodir" --test --jobs
expect_rc "--jobs without a value" jobs_missing 2
c bad_o "$WORK/nodir" --test -O4
expect_rc "an unknown -O level" bad_o 2
c not_first "$WORK/nodir" x.tr --test
expect_rc "--test after a file" not_first 2
expect_out "--test after a file" not_first "--test must be the first argument"

# --- a suite that satisfies every convention passes --------------------------------
G="$WORK/good"
fixture "$G" tests/plain.tr <<'EOF'
import test;

fn main(): int {
    test::assert(1 + 1 == 2, "arithmetic");
    return 0;
}
EOF
fixture "$G" tests/fail_type_mismatch.tr <<'EOF'
// EXPECT-DIAG: E064
fn main(): int { return "x"; }
EOF
fixture "$G" tests/fail_aborts.tr <<'EOF'
// RUNTIME_ABORT_EXPECTED
// EXPECT-RUNTIME: boom
import test;

fn main(): int {
    test::fail("boom");
    return 0;
}
EOF
fixture "$G" tests/fail_silent_abort.tr <<'EOF'
// RUNTIME_ABORT_EXPECTED
// EXPECT-EXIT: 134
import sys;

fn main(): int {
    sys::abort();
    return 0;
}
EOF
# A companion is imported, never run -- it has no main, so running it would fail.
fixture "$G" tests/sub/helper_module.tr <<'EOF'
pub fn seven(): int { return 7; }
EOF
fixture "$G" tests/sub/uses_helper.tr <<'EOF'
import test;
import helper_module;

fn main(): int {
    test::assert(helper_module::seven() == 7, "helper");
    return 0;
}
EOF
# Every test runs in its own directory: this one finds the file beside it.
fixture "$G" tests/sub/cwd.tr <<'EOF'
import io;
import test;

fn main(): int {
    test::assert(io::exists("marker.txt"), "the test's own directory is the working directory");
    return 0;
}
EOF
echo marker > "$G/tests/sub/marker.txt"
c good "$G" --test
expect_rc "a suite satisfying every convention" good 0
expect_out "a suite satisfying every convention" good "Total Tests: 6"
expect_out "a suite satisfying every convention" good "Failed:      0"
expect_out "fail_ rejected" good "fail_type_mismatch.tr ... PASS (Rejected properly)"
expect_out "abort expected" good "fail_aborts.tr ... PASS (Aborted at runtime as expected)"
expect_out "silent abort" good "fail_silent_abort.tr ... PASS (Aborted at runtime as expected)"
expect_out "the companion is skipped, the test that imports it runs" good "uses_helper.tr ... PASS"
expect_no_out "the companion is skipped" good "helper_module.tr"
c good_o0 "$G" --test --jobs 1 -O0
expect_rc "-O0 and --jobs 1 are accepted" good_o0 0
expect_out "the level is named in the header" good_o0 "(-O0)"
c good_dot "$WORK" --test good/tests/
expect_rc "a directory spelled with a trailing slash, from elsewhere" good_dot 0

# --- each convention, broken on purpose, fails for ITS reason ----------------------
# breaks <name> <expected exit-line text> <detail text> : one fixture per case.
broken() {
  local name=$1 headline=$2 detail=$3 file=$4
  shift 4
  local d="$WORK/broken_$name"
  fixture "$d" "tests/$file"
  c "$name" "$d" --test --timeout 2 "$@"
  expect_rc "$name" "$name" 1
  expect_out "$name" "$name" "$headline"
  [ -z "$detail" ] || expect_out "$name (the detail block)" "$name" "$detail"
  expect_out "$name (the summary)" "$name" "Failed:      1"
}

broken exit3 "RUNTIME FAILED (Exit code 3)" "" plain.tr <<'EOF'
fn main(): int { return 3; }
EOF
broken compile_error "COMPILE FAILED" "[E064" plain.tr <<'EOF'
fn main(): int { return "x"; }
EOF
broken fail_compiles "FAILED (Should have failed compilation)" "Compiled successfully but expected to fail" fail_x.tr <<'EOF'
fn main(): int { return 0; }
EOF
broken wrong_diag "FAILED (Rejected, but EXPECT-DIAG not met)" "EXPECT-DIAG missed: no [E999] in the compiler output" fail_x.tr <<'EOF'
// EXPECT-DIAG: E999
fn main(): int { return "x"; }
EOF
broken wrong_diag_text "FAILED (Rejected, but EXPECT-DIAG not met)" "does not contain 'no such words'" fail_x.tr <<'EOF'
// EXPECT-DIAG: no such words
fn main(): int { return "x"; }
EOF
broken no_diag_violated "FAILED (Rejected, but EXPECT-DIAG not met)" "EXPECT-NO-DIAG violated: the compiler output contains 'Wrong return type'" fail_x.tr <<'EOF'
// EXPECT-NO-DIAG: Wrong return type
fn main(): int { return "x"; }
EOF
broken plain_diag_pin "FAILED (EXPECT-DIAG)" "EXPECT-DIAG missed" plain.tr <<'EOF'
// EXPECT-DIAG: dead store
fn main(): int { return 0; }
EOF
broken abort_never_aborts "FAILED (Expected a runtime abort, got exit 0)" "" fail_x.tr <<'EOF'
// RUNTIME_ABORT_EXPECTED
fn main(): int { return 0; }
EOF
broken abort_wont_compile "FAILED (Expected to compile successfully, but got a compile error)" "[E064" fail_x.tr <<'EOF'
// RUNTIME_ABORT_EXPECTED
fn main(): int { return "x"; }
EOF
broken wrong_runtime "FAILED (Aborted, but EXPECT-RUNTIME not met)" "EXPECT-RUNTIME missed: the program's output does not contain 'bang'" fail_x.tr <<'EOF'
// RUNTIME_ABORT_EXPECTED
// EXPECT-RUNTIME: bang
import test;

fn main(): int {
    test::fail("boom");
    return 0;
}
EOF
broken wrong_exit "FAILED (Aborted, but EXPECT-EXIT not met)" "EXPECT-EXIT missed: the program exited with 134, not 1" fail_x.tr <<'EOF'
// RUNTIME_ABORT_EXPECTED
// EXPECT-EXIT: 1
import sys;

fn main(): int {
    sys::abort();
    return 0;
}
EOF
broken hang "TIMEOUT (Likely infinite loop)" "Timed out after 1s" plain.tr --timeout 1 <<'EOF'
fn main(): int {
    while (true) { }
    return 0;
}
EOF

# --- @test functions (TDP-2-006), broken on purpose ---------------------------------
# One file, one failing test per way each outcome can fail, and one passing
# test: every failure must name its reason, and the other tests must still run.
F="$WORK/testfns"
fixture "$F" tests/outcomes.tr <<'EOF'
import test;

error ParseError {
    Empty,
    TooBig,
}

fn parse(s: string): !int {
    if s.length() == 0 {
        return ParseError::Empty;
    }
    return 1;
}

fn square(n: int): int {
    return n * n;
}

@deprecated("use square")
fn old(n: int): int {
    return n;
}

@test
fn returns_assertion(): void {
    test::assert(1 == 2, "one is not two");
}

@test
fn returns_with_error(): !void {
    try parse("");
    return;
}

@test(compile_error, E041, "Unknown function")
fn ce_no_error(): void {
    io::printline(1);
}

@test(compile_error, E041, "Unknown function")
fn ce_another_code(): void {
    immut x: int = "text";
}

@test(compile_error, E041, "nothing like this")
fn ce_another_message(): void {
    undeclared(1);
}

@test(compile_error, E041, "sqaure", fix: "replace 'sqaure' with 'cube'")
fn ce_another_fix(): void {
    io::printline(sqaure(2));
}

@test(compile_error, E041, "undeclared", fix: "anything")
fn ce_no_fix(): void {
    io::printline(undeclared(2));
}

@test(warning, E172, "use square")
fn warning_none(): void {
    io::printline(1);
}

@test(warning, E172, "another text")
fn warning_another(): void {
    io::printline(old(1));
}

@test(no_warnings)
fn no_warnings_has_one(): void {
    io::printline(old(1));
}

@test(panic, "Division by zero")
fn panic_returns(): void {
    io::printline(1);
}

@test(panic, "Division by zero")
fn panic_another_message(): void {
    test::assert(false, "other");
}

@test(exit, 3)
fn exit_returns(): void {
    io::printline(1);
}

@test(exit, 3)
fn exit_another_status(): void {
    sys::exit(4);
}

@test(error, ParseError::TooBig)
fn error_another(): !void {
    try parse("");
    return;
}

@test(error, ParseError::Empty)
fn error_none(): !void {
    try parse("x");
    return;
}

@test
fn still_runs(): void {
    test::assert(true, "fine");
}
EOF
c testfns "$F" --test --timeout 5
expect_rc "@test failures" testfns 1
expect_out "@test failures: the passing test still runs" testfns "outcomes.tr: still_runs ... PASS"
expect_out "@test failures: the count" testfns "Failed:      16"
tf() { expect_out "@test failure $1" testfns "$2"; }
tf returns_assertion "it panicked; expected it to return"
tf returns_with_error "it returned the error ParseError::Empty; expected it to return"
tf ce_no_error "its body compiled without an error; expected E041 \"Unknown function\""
tf ce_another_code "its body has another error, E066 at line 42"
tf ce_another_message "no E041 message contains \"nothing like this\""
tf ce_another_fix "E041 at line 52: Fix: replace 'sqaure' with 'square'"
tf ce_no_fix "E041 at line 57: no fix"
tf warning_none "its body compiled without warning E172 \"use square\""
tf warning_another "its body compiled without warning E172 \"another text\"; its warnings:"
tf no_warnings_has_one "its body compiled with a warning, E172 at line 72"
tf panic_returns "it returned; expected a panic with \"Division by zero\""
tf panic_another_message "it panicked, but its output does not contain \"Division by zero\""
tf exit_returns "it returned; expected it to end through sys::exit(3)"
tf exit_another_status "it ended with exit status 4; expected it to end through sys::exit(3)"
tf error_another "it returned the error ParseError::Empty; expected it to return the error ParseError::TooBig"
tf error_none "it returned; expected it to return the error ParseError::Empty"

# A file whose tests do not build is one failure, with the compiler's reason.
fixture "$F/b" tests/broken.tr <<'EOF'
@test
fn a(): void {
    immut n: int = "text";
}
EOF
c testfns_build "$F/b" --test
expect_rc "@test file that does not build" testfns_build 1
expect_out "@test file that does not build" testfns_build "broken.tr ... COMPILE FAILED (its tests did not run)"
expect_out "@test file that does not build (the reason)" testfns_build "[E066"

# A normal build leaves every test out: the object is byte for byte the
# program's without them (same file name, so the module is named the same).
mkdir -p "$F/with" "$F/without"
cat > "$F/with/prog.tr" <<'EOF'
fn twice(n: int): int { return n * 2; }

@test
fn doubles(): void {
    io::printline(twice(2));
}

@test(compile_error, E041, "")
fn undeclared_call(): void {
    undeclared();
}

fn main(): int { return twice(0); }
EOF
cat > "$F/without/prog.tr" <<'EOF'
fn twice(n: int): int { return n * 2; }

fn main(): int { return twice(0); }
EOF
c obj_with "$F/with" prog.tr --emit-obj -o prog
c obj_without "$F/without" prog.tr --emit-obj -o prog
expect_rc "a build with tests" obj_with 0
cmp -s "$F/with/prog.o" "$F/without/prog.o" ||
  fail "a build with @test functions differs from the same program without them"

# A package's own src/ files with tests run beside tests/, found through the
# graph; a program's `main` there is the program, not a test.
P="$WORK/pkgtests"
fixture "$P" src/util.tr <<'EOF'
import test;

pub fn half(n: int): int { return n / 2; }

@test
fn halves(): void {
    test::assert(half(8) == 4, "8 halved");
}
EOF
fixture "$P" src/main.tr <<'EOF'
import util;

@test
fn calls_its_module(): void {
    io::printline(util::half(4));
}

fn main(): int { return util::half(0); }
EOF
fixture "$P" tests/plain.tr <<'EOF'
fn main(): int { return 0; }
EOF
printf '{"format": 1, "root": "app", "packages": {"app": {"src": "src", "deps": [], "tests": "tests"}}}\n' > "$P/graph.json"
c pkgtests "$P" --test tests --package-graph graph.json
expect_rc "@test functions in src/" pkgtests 0
expect_out "@test functions in src/" pkgtests "util.tr: halves ... PASS"
expect_out "@test functions in src/ (a program's own tests)" pkgtests "main.tr: calls_its_module ... PASS"
expect_out "@test functions in src/ (its main is not a test)" pkgtests "Total Tests: 3"
c pkgtests_only "$P" --test tests --package-graph graph.json --only plain.tr
expect_out "--only runs what it names, not src/'s tests" pkgtests_only "Total Tests: 1"
# A package whose tests are all in src/ needs no tests/ directory; one with no
# test anywhere is still an error, never a silent pass.
Q="$WORK/srconly"
fixture "$Q" src/main.tr <<'EOF'
import test;

fn half(n: int): int { return n / 2; }

@test
fn halves(): void {
    test::assert(half(8) == 4, "8 halved");
}

fn main(): int { return half(0); }
EOF
printf '{"format": 1, "root": "app", "packages": {"app": {"src": "src", "deps": []}}}\n' > "$Q/graph.json"
c srconly "$Q" --test tests --package-graph graph.json
expect_rc "@test functions in src/ with no tests/" srconly 0
expect_out "@test functions in src/ with no tests/" srconly "main.tr: halves ... PASS"
sed -i.bak 's/^@test$/\/\/ not a test/' "$Q/src/main.tr"
c srconly_none "$Q" --test tests --package-graph graph.json
expect_rc "no tests/ and no @test in src/" srconly_none 1
expect_out "no tests/ and no @test in src/" srconly_none "no file under src/ declares a \`@test\` function"

# --- a warning test owns its warning: no lint level applies to it ----------------
# Denied, the expected warning would fail the build the test demonstrates it in;
# allowed, the test could never see it. Any other warning keeps its level.
L="$WORK/warnown"
fixture "$L" tests/deprecated.tr <<'EOF'
@deprecated("use square")
fn old(n: int): int {
    return n * n;
}

@test(warning, E172, "use square")
fn warns(): void {
    io::printline(old(2));
}
EOF
fixture "$L" prog.tr <<'EOF'
@deprecated("use square")
fn old(n: int): int {
    return n * n;
}

fn main(): int {
    return 0;
}

@test(warning, E172, "use square")
fn warns(): void {
    io::printline(old(2));
}
EOF
for lv in deny allow; do
  c "warnown_$lv" "$L" --test "--$lv" E172
  expect_rc "a warning test under --$lv E172" "warnown_$lv" 0
  expect_out "a warning test under --$lv E172" "warnown_$lv" "deprecated.tr: warns ... PASS"
done
c warnown_build "$L" prog.tr -o prog --deny E172
expect_rc "a normal build with a warning test, --deny E172" warnown_build 0
expect_no_out "a normal build does not show a warning test's own warning" warnown_build "E172"
sed -i.bak 's/^    return 0;$/    return old(0);/' "$L/prog.tr"
c warnown_main "$L" prog.tr -o prog --deny E172
expect_rc "the same warning outside the test is still denied" warnown_main 1
expect_out "the same warning outside the test is still denied" warnown_main "[E172"

# --- --exclude, and the diagnostic register is opt-in ------------------------------
X="$WORK/exclude"
fixture "$X" tests/plain.tr <<'EOF'
fn main(): int { return 0; }
EOF
fixture "$X" tests/known/plain_bad.tr <<'EOF'
fn main(): int { return 9; }
EOF
c exclude_off "$X" --test
expect_rc "a failing test in a subdirectory fails the run" exclude_off 1
c exclude_on "$X" --test --exclude known/
expect_rc "--exclude skips a directory" exclude_on 0
expect_out "--exclude skips a directory" exclude_on "Total Tests: 1"
# --only is the other way round: only what it names runs, and a name that
# matches no test is an error, never "nothing to run, all green".
c only_dir "$X" --test --only known/
expect_rc "--only runs a directory" only_dir 1
expect_out "--only runs a directory" only_dir "Total Tests: 1"
expect_out "--only runs a directory" only_dir "plain_bad.tr"
c only_file "$X" --test --only ./plain.tr
expect_rc "--only runs one file" only_file 0
expect_out "--only runs one file" only_file "Total Tests: 1"
c only_missing "$X" --test --only nope.tr
expect_rc "--only naming no test" only_missing 1
expect_out "--only naming no test" only_missing "no test matches 'tests/nope.tr'"
c only_file_as_dir "$X" --test tests/plain.tr
expect_rc "a file in place of the directory" only_file_as_dir 2
expect_out "a file in place of the directory" only_file_as_dir "--only"

R="$WORK/register"
fixture "$R" tests/fail_unpinned.tr <<'EOF'
fn main(): int { return "x"; }
EOF
printf '# a register with no entry for the test above\n' > "$R/register.txt"
c register_off "$R" --test
expect_rc "without a register an unpinned fail_ test is judged by name alone" register_off 0
c register_on "$R" --test --diag-register register.txt
expect_rc "with a register an unpinned fail_ test fails" register_on 1
expect_out "with a register an unpinned fail_ test fails" register_on "has no '// EXPECT-DIAG: Exxx' line and is not in tests/DIAGNOSTIC_GAPS.txt"
expect_out "the register names the code to pin" register_on "The compiler emitted: E064."
printf 'UNPINNED tests/fail_unpinned.tr\n' > "$R/register.txt"
c register_listed "$R" --test --diag-register register.txt
expect_rc "an UNPINNED entry is the baseline" register_listed 0
expect_out "the register's size is reported" register_listed "1 unpinned"
c register_missing "$R" --test --diag-register nowhere.txt
expect_rc "an unreadable register" register_missing 1
expect_out "an unreadable register" register_missing "cannot read the diagnostic register"

# --- turf test: a project ----------------------------------------------------------
if [ ! -x "$TURF" ]; then
  if [ -n "${CI:-}" ]; then
    echo "FAIL: build/turf is missing in CI (was Go installed before the CMake build?)"
    exit 1
  fi
  echo "SKIP the turf test half (no build/turf: Go not installed?)"
else
  PRJ="$WORK/proj"
  mkdir -p "$PRJ"
  t new_geo "$PRJ" new geometry --lib
  expect_rc "turf new geometry --lib" new_geo 0
  fixture "$PRJ/geometry" src/geometry.tr <<'EOF'
import vecmath;

pub fn answer(): int { return vecmath::twice(21); }
EOF
  fixture "$PRJ/geometry" src/vecmath.tr <<'EOF'
pub fn twice(x: int): int { return x * 2; }
EOF
  t new_app "$PRJ" new app
  expect_rc "turf new app" new_app 0
  # `turf add` writes the path dependency the way a user would.
  t add_dep "$PRJ/app" add geometry --path ../geometry
  expect_rc "turf add geometry" add_dep 0

  t no_tests "$PRJ/app" test
  expect_rc "turf test with no tests/ directory" no_tests 1
  # turf's own message, or (a turf that leaves it to the compiler, which
  # also looks for @test functions in src/) turfc's.
  expect_out "turf test with no tests/ directory" no_tests "no tests"
  expect_out "the error shows how to start" no_tests "put test files"

  mkdir -p "$PRJ/app/tests"
  t empty_tests "$PRJ/app" test
  expect_rc "turf test with an empty tests/ directory" empty_tests 1
  expect_out "turf test with an empty tests/ directory" empty_tests "no tests found"

  t outside "$WORK" test
  expect_rc "turf test outside a project" outside 2
  expect_out "turf test outside a project" outside "not inside a TurfLang project"

  fixture "$PRJ/app" src/util.tr <<'EOF'
pub fn square(x: int): int { return x * x; }
EOF
  fixture "$PRJ/app" tests/own_src.tr <<'EOF'
import test;
import util;

fn main(): int {
    test::assert(util::square(5) == 25, "a test imports the package's own module");
    return 0;
}
EOF
  # A test named like the module it tests: `import util;` is src/util.tr, and
  # the test file itself is not a second candidate (no E090).
  fixture "$PRJ/app" tests/util.tr <<'EOF'
import test;
import util;

fn main(): int {
    test::assert(util::square(3) == 9, "a test shares its module's name");
    return 0;
}
EOF
  fixture "$PRJ/app" tests/dependency.tr <<'EOF'
import test;
import geometry;

fn main(): int {
    test::assert(geometry::answer() == 42, "a test imports a dependency");
    return 0;
}
EOF
  fixture "$PRJ/app" tests/nested/with_helper.tr <<'EOF'
import test;
import support_module;

fn main(): int {
    test::assert(support_module::three() == 3, "a test imports a helper beside it");
    return 0;
}
EOF
  fixture "$PRJ/app" tests/nested/support_module.tr <<'EOF'
pub fn three(): int { return 3; }
EOF
  # An internal module of a dependency is not importable (E091), from a test
  # exactly as from src/: a test is the root package's code, not a loophole.
  fixture "$PRJ/app" tests/fail_dependency_internals.tr <<'EOF'
// EXPECT-DIAG: E091
import vecmath;

fn main(): int { return 0; }
EOF
  t pass "$PRJ/app" test
  expect_rc "turf test over a project" pass 0
  expect_out "turf test over a project" pass "Total Tests: 5"
  expect_out "a test imports the package's own module" pass "own_src.tr ... PASS"
  expect_out "a test shares its module's name" pass "util.tr ... PASS"
  expect_out "a test imports a dependency" pass "dependency.tr ... PASS"
  expect_out "a test imports a helper beside it" pass "with_helper.tr ... PASS"
  expect_out "a dependency's internal module stays private to tests too" pass "fail_dependency_internals.tr ... PASS (Rejected properly)"
  t pass_sub "$PRJ/app/src" test --offline --locked --jobs 2
  expect_rc "turf test from a subdirectory, with --offline --locked --jobs" pass_sub 0
  # The graph for a test run names tests/; the graph every other command writes
  # must not (an older turfc rejects the key).
  grep -q '"tests"' "$PRJ/app/build/package-graph.json" || fail "turf test should write tests/ into the package graph"
  t build_after "$PRJ/app" build
  expect_rc "turf build after turf test" build_after 0
  if grep -q '"tests"' "$PRJ/app/build/package-graph.json"; then fail "turf build must write the package graph without tests/"; fi

  t refuse_o "$PRJ/app" test -o x
  expect_rc "turf test -o" refuse_o 2
  t refuse_graph "$PRJ/app" test --package-graph g.json
  expect_rc "turf test --package-graph" refuse_graph 2

  fixture "$PRJ/app" tests/fails.tr <<'EOF'
import test;

fn main(): int {
    test::assert(1 == 2, "this test fails on purpose");
    return 0;
}
EOF
  t fails "$PRJ/app" test
  expect_rc "a failing test fails turf test" fails 1
  expect_out "a failing test fails turf test" fails "Failed:      1"
  expect_out "the failure is named" fails "fails.tr ... RUNTIME FAILED"
  expect_out "the failing test's own message is shown" fails "this test fails on purpose"
  t fails_excluded "$PRJ/app" test --exclude fails.tr
  expect_rc "--exclude is handed through" fails_excluded 0
  # Paths select tests, written from where turf runs.
  t one_path "$PRJ/app" test tests/fails.tr
  expect_rc "turf test <file> runs that test" one_path 1
  expect_out "turf test <file> runs that test" one_path "Total Tests: 1"
  t one_path_inside "$PRJ/app/tests" test util.tr --jobs 1
  expect_rc "a path from inside tests/, next to a flag's value" one_path_inside 0
  expect_out "a path from inside tests/, next to a flag's value" one_path_inside "util.tr ... PASS"
  expect_out "a path from inside tests/, next to a flag's value" one_path_inside "Total Tests: 1"
  t dir_path "$PRJ/app" test tests/nested
  expect_rc "turf test <dir>" dir_path 0
  expect_out "turf test <dir>" dir_path "with_helper.tr ... PASS"
  # `turf run` compiles a test on its own, without the project: the test's
  # `import util;` is not the test file itself, and the hint names `turf test`.
  t run_test_file "$PRJ/app" run tests/util.tr
  expect_rc "turf run on a test file" run_test_file 1
  expect_out "turf run on a test file" run_test_file "[E027"
  expect_out "turf run on a test file" run_test_file "Run it with \`turf test tests/util.tr\`"
  t outside_path "$PRJ/app" test src/util.tr
  expect_rc "a path outside tests/" outside_path 2
  expect_out "a path outside tests/" outside_path "is not under the project's tests/ directory"
  t missing_path "$PRJ/app" test tests/nope.tr
  expect_rc "a path that does not exist" missing_path 2
  t helper_path "$PRJ/app" test tests/nested/support_module.tr
  expect_rc "a companion is not a test" helper_path 1
  expect_out "a companion is not a test" helper_path "no test matches"
fi

if [ "$FAILED" -ne 0 ]; then
  echo "test_cmd_smoke: $FAILED check(s) failed"
  exit 1
fi
echo "test_cmd_smoke: all checks passed"
