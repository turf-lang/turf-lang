#!/bin/bash
#
# pattern_warnings_smoke.sh -- LEC-003: the warnings a `compare` with patterns
# produces, which test_runner.sh cannot see (it only checks that a file
# compiles) and the turf::analyze() facade does not run (they come from the
# CFG passes, which belong to code generation):
#
#   1. a binding nothing reads is "declared but never used", pointing at the
#      binding;
#   2. the same binding spelled `_name` is silent -- the convention the warning's
#      own hint recommends, which only became true once `_name` could be lexed;
#   3. an arm behind an unguarded catch-all is reported as E094, exactly ONCE
#      (the TypeChecker checks every body twice, so an undeduplicated warning
#      would print twice);
#   4. a binding that IS read produces no unused warning, and a struct-pattern
#      binding is never ALSO reported as a dead store.
#
# Usage: scripts/smoke/pattern_warnings_smoke.sh     (after building build/turfc)
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

compile() { "$TURF" "$1" -o "$WORK/out.bin" 2>&1 | strip; }

cat > unused.tr <<'EOF'
struct P { x: int, y: int }
fn main(): int {
    mut p: P = P::default();
    mut r: int;
    compare p {
        matches P { x, y: why }:
            r = x;
        done
        matches _:
            r = 0;
        done
    }
    return r;
}
EOF
out=$(compile unused.tr)
echo "$out" | grep -q "Variable 'why' is declared but never used" \
  || fail "1: an unread binding should be reported unused. Got: $out"
echo "$out" | grep -q "Variable 'x' is declared but never used" \
  && fail "4: a binding that is read was reported unused"
echo "$out" | grep -q "'why' is assigned but the value is never read" \
  && fail "4: an unused binding was ALSO reported as a dead store"

sed 's/why/_why/' unused.tr > quiet.tr
out=$(compile quiet.tr)
echo "$out" | grep -q "_why" && fail "2: a \`_name\` binding should be silent. Got: $out"

cat > unreachable.tr <<'EOF'
struct P { x: int }
fn main(): int {
    mut p: P = P::default();
    mut r: int;
    compare p {
        matches _:
            r = 1;
        done
        matches P { x: 3 }:
            r = 2;
        done
        otherwise:
            r = 3;
    }
    return r;
}
EOF
out=$(compile unreachable.tr)
n=$(echo "$out" | grep -c "This arm can never run")
[ "$n" = "1" ] || fail "3: expected exactly one 'This arm can never run', got $n. Output: $out"
n=$(echo "$out" | grep -c "The 'otherwise' arm can never run")
[ "$n" = "1" ] || fail "3: expected exactly one otherwise warning, got $n. Output: $out"

echo "pattern_warnings_smoke: all checks passed"
