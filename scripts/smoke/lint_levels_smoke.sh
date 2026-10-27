#!/bin/bash
#
# lint_levels_smoke.sh -- V-1 (TDP-2-003): every warning has a level, and the
# level is chosen on the command line (`turf.toml [lints]` reaches the compiler
# as these same flags). test_runner.sh cannot see any of it: it judges a test by
# whether it compiles and what the output says, never by a flag.
#
#   1. the default is warn: the warning prints with its [E163] code, the build
#      succeeds;
#   2. `--allow E163` records nothing; the other warnings are untouched;
#   3. `--deny E163` prints it as an ERROR (and says why), counts it as one, fails
#      the build and writes no executable;
#   4. the last level for a code wins, in either order, and `--warn` undoes a deny;
#   5. a warning raised while PARSING (E154, E153) is denied once, not twice;
#   6. a level that names nothing real is a usage error (status 2), never a
#      silent no-op: an unknown code, a malformed one, `--allow`/`--warn` of an
#      error's code, a lint set that is newer than the compiler or does not
#      exist, a flag with no value. `--deny` of an error's code is what it
#      already is, so it passes;
#   7. a denied warning inside lib/std never fails a build over code the user
#      cannot edit (E159 is in lib/std/http); --stdlib-warnings opts back in;
#   8. `turfc --test` hands the levels to every test's compile, and a bad one
#      stops the run before any test starts;
#   9. `--lint-set 2.0` (a set the compiler has) is accepted and every 2.0
#      warning still shows.
#
# Usage: scripts/smoke/lint_levels_smoke.sh     (after building build/turfc)
# Exit:  0 = all checks pass; 1 = one failed (which one is printed).

set -u
cd "$(dirname "$0")/../.." || exit 1

TURF="${TURF_COMPILER:-./build/turfc}"
[ -x "$TURF" ] || { echo "FAIL: $TURF not built"; exit 1; }
TURF="$(cd "$(dirname "$TURF")" && pwd)/$(basename "$TURF")"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cd "$WORK" || exit 1
strip() { sed 's/\x1b\[[0-9;]*m//g'; }
fail() { echo "FAIL: $1"; exit 1; }

# Compile with the given flags; the output is the text, the status is $?.
run() { local f=$1; shift; "$TURF" "$f" -o "$WORK/out.bin" "$@" 2>&1 | strip; return "${PIPESTATUS[0]}"; }

cat > w.tr <<'EOF'
fn main(): int {
    mut unused: int = 5;
    mut x: int = 1;
    x = x;
    return 0;
}
EOF

# 1. default
out=$(run w.tr); st=$?
[ "$st" = 0 ] || fail "1: a program with only warnings must build (status $st). Got: $out"
echo "$out" | grep -q "warning at line 2, col 5 \[E163" || fail "1: E163 should print as a warning with its code. Got: $out"
echo "$out" | grep -q "\[E156" || fail "1: E156 should print. Got: $out"

# 2. allow
out=$(run w.tr --allow E163); st=$?
[ "$st" = 0 ] || fail "2: --allow E163 must build (status $st). Got: $out"
echo "$out" | grep -q "E163" && fail "2: an allowed warning was still reported. Got: $out"
echo "$out" | grep -q "\[E156" || fail "2: --allow E163 must not hide E156. Got: $out"
echo "$out" | grep -q "w.tr (2 warnings)" || fail "2: the file header should count 2 warnings. Got: $out"

# 3. deny
rm -f out.bin
out=$(run w.tr --deny E163); st=$?
[ "$st" = 1 ] || fail "3: --deny E163 must fail the build with status 1 (got $st). Got: $out"
echo "$out" | grep -q "error at line 2, col 5 \[E163" || fail "3: a denied warning must print as an error. Got: $out"
echo "$out" | grep -q "E163 is a warning by default; its lint level here is deny" || fail "3: a denied warning must say why. Got: $out"
echo "$out" | grep -q "w.tr (1 error, 2 warnings)" || fail "3: the file header should count 1 error and 2 warnings. Got: $out"
[ ! -e out.bin ] || fail "3: a failed build must write no executable"

# 4. the last level wins
run w.tr --deny E163 --allow E163 >/dev/null || fail "4: --deny then --allow E163 should build"
run w.tr --allow E163 --deny E163 >/dev/null && fail "4: --allow then --deny E163 should fail"
run w.tr --deny E163 --warn E163 >/dev/null || fail "4: --warn after --deny E163 should build"
out=$(run w.tr --deny e163); [ "$?" = 1 ] || fail "4: a code is read in either case. Got: $out"

# 5. a parse-time warning is denied once
cat > empty_compare.tr <<'EOF'
fn main(): int {
    mut n: int = 0;
    compare n {
    }
    return 0;
}
EOF
out=$(run empty_compare.tr); [ "$?" = 0 ] || fail "5: an empty compare is only a warning. Got: $out"
echo "$out" | grep -q "\[E154" || fail "5: the empty-compare warning should carry E154. Got: $out"
out=$(run empty_compare.tr --deny E154); [ "$?" = 1 ] || fail "5: --deny E154 should fail. Got: $out"
n=$(echo "$out" | grep -c "error at line")
[ "$n" = 1 ] || fail "5: a denied parse-time warning must be reported once, not $n times. Got: $out"
echo "$out" | grep -q "(1 error)" || fail "5: the header should count one error. Got: $out"

# 6. usage errors
usage() { # description, then the flags
  local what=$1; shift
  out=$(run w.tr "$@"); st=$?
  [ "$st" = 2 ] || fail "6: $what should be a usage error (status 2), got $st. Got: $out"
  echo "$out" | grep -q "^Error: " || fail "6: $what should say what is wrong. Got: $out"
}
usage "an unknown code" --allow E999
usage "a malformed code" --allow E16
usage "a word that is not a code" --deny unused
usage "--warn of an error's code" --warn E102
usage "--allow of an error's code" --allow E102
usage "a lint set newer than the compiler" --lint-set 2.99
usage "a lint set that does not exist" --lint-set 3.0
usage "a malformed lint set" --lint-set 2
usage "a flag with no value" --allow
run w.tr --deny E102 >/dev/null || fail "6: --deny of an error's code is what it already is and must be accepted"

# 7. lib/std
cat > std.tr <<'EOF'
import http;
fn main(): int { return 0; }
EOF
out=$(run std.tr); [ "$?" = 0 ] || fail "7: baseline: importing http builds. Got: $out"
out=$(run std.tr --deny E159); [ "$?" = 0 ] || fail "7: --deny E159 must not fail a build over lib/std's own warning. Got: $out"
out=$(run std.tr --deny E159 --stdlib-warnings); [ "$?" = 1 ] || fail "7: with --stdlib-warnings the user has opted in, so --deny E159 applies. Got: $out"

# 8. --test
mkdir -p proj/tests
cat > proj/tests/unused_var.tr <<'EOF'
fn main(): int {
    mut unused: int = 5;
    return 0;
}
EOF
runtests() { "$TURF" --test proj/tests "$@" > t.out 2>&1; local st=$?; strip < t.out; return $st; }
out=$(runtests); [ "$?" = 0 ] || fail "8: baseline test run should pass. Got: $out"
out=$(runtests --deny E163); [ "$?" = 1 ] || fail "8: --test --deny E163 should fail the test that has the warning. Got: $out"
out=$(runtests --deny E163 --allow E163); [ "$?" = 0 ] || fail "8: the last level wins under --test too. Got: $out"
out=$(runtests --deny E999); [ "$?" = 2 ] || fail "8: a bad level stops --test with a usage error before any test runs. Got: $out"

# 9. a lint set the compiler has
out=$(run w.tr --lint-set 2.0); [ "$?" = 0 ] || fail "9: --lint-set 2.0 should be accepted. Got: $out"
echo "$out" | grep -q "\[E163" || fail "9: a warning that shipped in 2.0 shows under --lint-set 2.0. Got: $out"

echo "PASS: lint levels"
