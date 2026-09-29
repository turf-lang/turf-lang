#!/bin/bash
#
# migrate_pub_smoke.sh -- LEC-002 stage 6: `turfc --migrate=pub` round-trips a
# 1.x-style project.
#
# Before 2.0 everything a module declared was visible to every importer; from
# 2.0 it is private unless it says `pub`. tests/custom/migrate_pub/ is a small
# project written the 1.x way (a module with no `pub` anywhere, and an importer
# that uses all of it). This proves, end to end:
#   1. as written, it is REJECTED (E088) -- so the migration is actually needed;
#   2. `--migrate=pub` without `--write` only reports, and changes nothing;
#   3. `--migrate=pub --write` on the module makes the project compile and run
#      with the same result it had in 1.x (exit 0);
#   4. running it again changes nothing (idempotent);
#   5. `fn`/`struct` inside a comment or string, a function-local struct, and a
#      function TYPE `fn(int, int): int` are not mistaken for declarations.
# The bigger check -- migrating the whole pre-LEC-002 test suite and running it
# green -- needs the old tree (`git archive f62ed6a`) and is recorded in
# docs/development/changes/LEC-002.md; this script is the part that can run
# anywhere, in CI.
#
# Usage: scripts/migrate_pub_smoke.sh        (after building build/turfc)
# Exit:  0 = all checks pass; 1 = one failed (which one is printed).

set -u
cd "$(dirname "$0")/.." || exit 1

TURF="${TURF_COMPILER:-./build/turfc}"
[ -x "$TURF" ] || { echo "FAIL: $TURF not built"; exit 1; }
# Absolute, because the script cd's into a scratch directory below.
TURF="$(cd "$(dirname "$TURF")" && pwd)/$(basename "$TURF")"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cp tests/custom/migrate_pub/*.tr "$WORK/"
cd "$WORK" || exit 1
strip() { sed 's/\x1b\[[0-9;]*m//g'; }
fail() { echo "FAIL: $1"; exit 1; }

# 1. As written (1.x style) the project must not compile: nothing is exported.
out=$("$TURF" main.tr -o main.out 2>&1 | strip)
echo "$out" | grep -q '\[E088\]' || fail "the un-migrated project should be rejected with E088"

# 2. Report only: says what it would do, touches nothing.
cp geometry.tr geometry.orig
report=$("$TURF" --migrate=pub geometry.tr 2>&1)
cmp -s geometry.tr geometry.orig || fail "--migrate=pub without --write modified the file"
echo "$report" | grep -q 'would migrate geometry.tr' || fail "report missing 'would migrate'"
echo "$report" | grep -q '+ pub fn add(' || fail "report should show 'pub fn add('"
echo "$report" | grep -q 'Re-run with --write' || fail "report should say how to apply"

# 3. Apply, then the project builds and behaves as it did in 1.x.
"$TURF" --migrate=pub --write geometry.tr > /dev/null 2>&1 || fail "--write failed"
"$TURF" main.tr -o main.out > compile.log 2>&1 || { strip < compile.log | head -20; fail "migrated project does not compile"; }
./main.out
rc=$?
[ $rc -eq 0 ] || fail "migrated project exited $rc (expected 0)"

# 4. Idempotent: a second run has nothing to add and says so.
again=$("$TURF" --migrate=pub --write geometry.tr 2>&1)
echo "$again" | grep -q 'Migrated 0 declarations/fields in 0 files' || fail "second run was not a no-op: $again"

# 5. The shapes that must NOT be touched.
grep -q '^    struct Scratch'   geometry.tr || fail "a function-local struct was given pub"
grep -q 'fn struct enum'      geometry.tr || fail "comment text was altered"
grep -q '^pub struct Point<T>' geometry.tr || fail "top-level struct was not migrated"
grep -q '^    pub pick: fn(int, int): int' geometry.tr || fail "a function-typed field should get pub, and its type be left alone"
[ "$(grep -c 'pub fn(' geometry.tr)" -eq 0 ] || fail "a function TYPE was given pub"
grep -q '^export immut'       geometry.tr || fail "export immut was altered"
grep -q '^@caller_location'   geometry.tr && grep -q '^pub fn where_' geometry.tr || fail "decorated fn: pub must follow the decorator"

echo "migrate_pub_smoke: all checks passed"
