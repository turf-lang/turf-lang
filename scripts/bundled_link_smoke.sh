#!/bin/bash
#
# bundled_link_smoke.sh -- REL-008 stage 3: the linker built into turfc.
#
# turfc links a program in-process with lld, against the C runtime under
# <root>/lib/rt/<platform>/, when the program names no system C library beyond
# the C runtime itself (the stdlib's `math` names "m"); otherwise, and whenever
# there is no such runtime, it runs the system `cc` as it always did
# (include/BundledLink.h has the contract and the rule). This checks each side
# of that rule against the real compiler, with a DECOY `cc` first on PATH that
# records that it was run and fails, so "the system toolchain was not used" is a
# fact the test observes, not an inference.
#
# It needs a turfc that embeds lld (REL-008 builds: `cmake_build.sh
# --release-static --embed-lld`; `turfc --version` says `linker embedded-lld`).
# Any other turfc, and any host without a C runtime to stand in for the bundled
# one, SKIPs (exit 0), unless CI is set, where a skip is a failure. The stand-in
# runtime is built from this machine's own *static glibc* and gcc runtime
# (crt1.o, libc.a, libgcc.a, ...), laid out as the contract says. That proves the
# mechanism. It is NOT the shipped runtime (musl, REL-008 stage 4); in particular
# static glibc cannot resolve host names at run time, so the net tests are not
# part of this check.
#
# Usage: TURFC=path/to/turfc scripts/bundled_link_smoke.sh   (default build/turfc)
# Exit:  0 = all checks pass (or skipped); 1 = a check failed.

set -u
cd "$(dirname "$0")/.." || exit 1
ROOT=$(pwd)
TURFC="${TURFC:-$ROOT/build/turfc}"

skip() {
  echo "bundled_link_smoke: SKIP ($1)"
  if [ -n "${CI:-}" ]; then echo "bundled_link_smoke: but CI is set, so a skip is a failure"; exit 1; fi
  exit 0
}
[ -x "$TURFC" ] || skip "$TURFC not built"
unset TURF_HOME TURF_LINKER
"$TURFC" --version 2>/dev/null | grep -q '^linker embedded-lld' || skip "this turfc has no embedded linker"
case "$(uname -s)/$(uname -m)" in
  Linux/x86_64) PLATFORM=linux-x86_64 ;;
  *) skip "no bundled link for $(uname -s)/$(uname -m) yet" ;;
esac
command -v cc >/dev/null 2>&1 || skip "no cc here to take a stand-in C runtime from"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
FAILED=0
pass() { echo "   ok   $1"; }
fail() { echo "   FAIL $1"; FAILED=$((FAILED + 1)); }

# An install tree: bin/turfc beside lib/ (copied, not symlinked: the compiler
# decides what is the standard library by real path).
INST="$WORK/inst"
RT="$INST/lib/rt/$PLATFORM"
mkdir -p "$INST/bin" "$RT"
cp "$TURFC" "$INST/bin/turfc"
cp "$ROOT/lib/libturfrt.a" "$INST/lib/" || skip "lib/libturfrt.a not built"
cp -r "$ROOT/lib/std" "$INST/lib/"
for f in crt1.o crti.o crtn.o libc.a libgcc.a libgcc_eh.a crtbeginT.o crtend.o; do
  p=$(cc -print-file-name="$f")
  [ -f "$p" ] || { [ "$f" = crtbeginT.o ] || [ "$f" = crtend.o ] || [ "$f" = libgcc_eh.a ] || skip "this host has no static $f"; continue; }
  cp "$p" "$RT/$f"
done
# glibc's libm.a is a linker script naming files by absolute path; use the real archive.
M=$(cc -print-file-name=libm.a)
if head -c 20 "$M" | grep -q '^!<arch>'; then cp "$M" "$RT/libm.a"
else real=$(sed -n 's/.*GROUP *( *\([^ ]*libm-[^ ]*\.a\).*/\1/p' "$M"); [ -f "$real" ] && cp "$real" "$RT/libm.a"; fi

# The decoy cc.
DECOY="$WORK/decoy"; mkdir -p "$DECOY"
printf '#!/bin/sh\necho "$@" >> "%s/cc.log"\nexit 99\n' "$WORK" > "$DECOY/cc"
chmod +x "$DECOY/cc"
ran_cc() { [ -s "$WORK/cc.log" ]; }
reset_cc() { rm -f "$WORK/cc.log"; }

cd "$WORK" || exit 1
cat > hello.tr <<'TR'
import math;

fn main(): int {
    printline("hello from the bundled link");
    printline(math::sqrt(16.0));
    return 0;
}
TR
cat > usesz.tr <<'TR'
extern from "z" fn zlibVersion(): string;

fn main(): int {
    printline(zlibVersion());
    return 0;
}
TR
compile() { PATH="$DECOY:$PATH" "$INST/bin/turfc" "$@" > out.log 2>&1; }
is_static() { ! readelf -lW "$1" 2>/dev/null | grep -q INTERP; }

echo "== --version reports the embedded linker and the usable runtime"
V=$("$INST/bin/turfc" --version)
echo "$V" | grep -q '^linker embedded-lld$' && pass "linker embedded-lld" || fail "no 'linker embedded-lld' line"
echo "$V" | grep -q "^bundled-runtime .*/lib/rt/$PLATFORM$" && pass "bundled-runtime line" || fail "no 'bundled-runtime' line"

echo "== a program naming only the C runtime's own libraries links in-process"
reset_cc
if compile hello.tr -o hello; then pass "compiled"; else fail "compile failed: $(tail -3 out.log)"; fi
ran_cc && fail "the system cc was run" || pass "the system cc was not run"
if [ -x hello ] && is_static hello; then pass "the executable is static"; else fail "the executable is not static"; fi
OUT=$(./hello 2>&1)
echo "$OUT" | grep -q "hello from the bundled link" && echo "$OUT" | grep -q "^4" && pass "it runs and prints" || fail "wrong output: $OUT"

echo "== TURF_LINKER=system uses the system toolchain"
reset_cc; rm -f hello
TURF_LINKER=system compile hello.tr -o hello && fail "expected the decoy cc to fail the link" || pass "the link failed (decoy cc exit 99)"
ran_cc && pass "the decoy cc was run" || fail "the system cc was not run"

echo "== a program that names another system library goes to the system toolchain"
reset_cc
compile usesz.tr -o usesz && fail "expected the decoy cc to fail the link" || pass "the link failed (decoy cc exit 99)"
ran_cc && grep -q -- '-lz' "$WORK/cc.log" && pass "cc was run with -lz" || fail "cc was not run with -lz"
reset_cc
compile hello.tr -o hello2 --link z && fail "--link z: expected the decoy cc to fail the link" || pass "--link z: the link failed (decoy cc exit 99)"
ran_cc && pass "--link z: cc was run" || fail "--link z: cc was not run"

echo "== a runtime directory missing a required file is not used"
mv "$RT/crt1.o" "$RT/crt1.o.off"; reset_cc
compile hello.tr -o hello3 && fail "expected the decoy cc to fail the link" || pass "the link failed (decoy cc exit 99)"
ran_cc && pass "the decoy cc was run (fell back)" || fail "no fallback to cc"
"$INST/bin/turfc" --version | grep -q '^bundled-runtime' && fail "--version still lists a runtime" || pass "--version no longer lists a bundled-runtime"
mv "$RT/crt1.o.off" "$RT/crt1.o"

echo "== a runtime that is present but broken fails clearly, without falling back"
cp "$RT/libc.a" "$RT/libc.a.good"; : > "$RT/libc.a"; reset_cc
if compile hello.tr -o hello4; then fail "expected the link to fail"; else pass "the link failed"; fi
grep -q "the linker built into turfc" out.log && pass "the message names the embedded linker" || fail "message does not name the embedded linker: $(tail -3 out.log)"
grep -q "TURF_LINKER=system" out.log && pass "the message names the escape hatch" || fail "message does not name TURF_LINKER=system"
ran_cc && fail "it fell back to cc after a bundled failure" || pass "no silent fallback to cc"
mv "$RT/libc.a.good" "$RT/libc.a"

echo
if [ "$FAILED" -ne 0 ]; then echo "bundled_link_smoke: $FAILED check(s) failed"; exit 1; fi
echo "bundled_link_smoke: all checks passed"
