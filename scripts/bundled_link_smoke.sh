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
# Any other turfc, and any host without a C runtime to take, SKIPs (exit 0),
# unless CI is set, where a skip is a failure.
#
# Two modes, the same checks:
#   default      a turfc (TURFC=, default build/turfc) with a STAND-IN runtime
#                taken from this machine's own static glibc and gcc runtime
#                (crt1.o, libc.a, libgcc.a, ...), laid out as the contract says.
#                That proves the mechanism. It is not the shipped runtime, and
#                static glibc cannot resolve host names, so the DNS check is
#                skipped.
#   --archive F  a release archive (the one CI is about to publish): unpacked,
#                and its OWN turfc and lib/rt/<platform>/ (musl on Linux, REL-008
#                stage 4) are tested, including a host-name lookup, which only a
#                runtime with a static resolver passes.
#
# Usage: scripts/bundled_link_smoke.sh [--archive FILE]    (TURFC= in default mode)
# Exit:  0 = all checks pass (or skipped); 1 = a check failed.

set -u
cd "$(dirname "$0")/.." || exit 1
ROOT=$(pwd)
ARCHIVE=""
while [ $# -gt 0 ]; do
  case "$1" in
    --archive) ARCHIVE="${2:?--archive needs a file}"; shift 2 ;;
    -h|--help) sed -n '3,40p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "bundled_link_smoke: unknown option '$1'" >&2; exit 2 ;;
  esac
done
case "$ARCHIVE" in ""|/*|[A-Za-z]:*) ;; *) ARCHIVE="$ROOT/$ARCHIVE" ;; esac
TURFC="${TURFC:-$ROOT/build/turfc}"

skip() {
  echo "bundled_link_smoke: SKIP ($1)"
  if [ -n "${CI:-}" ]; then echo "bundled_link_smoke: but CI is set, so a skip is a failure"; exit 1; fi
  exit 0
}
# Per-platform: the runtime directory name, a file the contract requires (moved
# away to prove an incomplete directory is not used), and a file whose being
# empty makes the link fail (to prove a broken runtime is reported, not hidden).
case "$(uname -s)/$(uname -m)" in
  Linux/x86_64) PLATFORM=linux-x86_64; REQUIRED_FILE=crt1.o; BREAK_FILE=libc.a ;;
  Darwin/arm64) PLATFORM=macos-arm64; REQUIRED_FILE=libSystem.tbd; BREAK_FILE=libSystem.tbd ;;
  *) skip "no bundled link for $(uname -s)/$(uname -m) yet" ;;
esac
unset TURF_HOME TURF_LINKER

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
FAILED=0
pass() { echo "   ok   $1"; }
fail() { echo "   FAIL $1"; FAILED=$((FAILED + 1)); }

INST="$WORK/inst"
if [ -n "$ARCHIVE" ]; then
  # The archive's own tree. Not a copy of anything: this is what a user gets.
  [ -f "$ARCHIVE" ] || { echo "bundled_link_smoke: $ARCHIVE not found"; exit 1; }
  mkdir -p "$WORK/unpacked"
  tar -xzf "$ARCHIVE" -C "$WORK/unpacked"
  INST=$(echo "$WORK"/unpacked/turf-v*)
  [ -x "$INST/bin/turfc" ] || { echo "bundled_link_smoke: no bin/turfc in $ARCHIVE"; exit 1; }
  RT="$INST/lib/rt/$PLATFORM"
  REAL_RUNTIME=1
else
  [ "$PLATFORM" = linux-x86_64 ] || skip "the stand-in runtime is Linux-only; pass --archive on this platform"
  [ -x "$TURFC" ] || skip "$TURFC not built"
  command -v cc >/dev/null 2>&1 || skip "no cc here to take a stand-in C runtime from"
  # An install tree: bin/turfc beside lib/ (copied, not symlinked: the compiler
  # decides what is the standard library by real path).
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
  REAL_RUNTIME=0
fi
"$INST/bin/turfc" --version 2>/dev/null | grep -q '^linker embedded-lld' || skip "this turfc has no embedded linker"
if [ -n "$ARCHIVE" ] && [ ! -f "$RT/$REQUIRED_FILE" ]; then
  echo "bundled_link_smoke: the archive has no $RT (REL-008 stage 4 builds it: scripts/build_bundled_runtime.sh)"; exit 1
fi

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
# "Needs nothing installed": a Linux executable is static (no interpreter); a
# macOS one can only load the system's libSystem.
is_self_contained() {
  case "$PLATFORM" in
    linux-x86_64) ! readelf -lW "$1" 2>/dev/null | grep -q INTERP ;;
    macos-arm64) [ "$(otool -L "$1" | sed 1d | grep -vc 'libSystem')" = 0 ] ;;
  esac
}

echo "== --version reports the embedded linker and the usable runtime"
V=$("$INST/bin/turfc" --version)
echo "$V" | grep -q '^linker embedded-lld$' && pass "linker embedded-lld" || fail "no 'linker embedded-lld' line"
echo "$V" | grep -q "^bundled-runtime .*/lib/rt/$PLATFORM$" && pass "bundled-runtime line" || fail "no 'bundled-runtime' line"

echo "== a program naming only the C runtime's own libraries links in-process"
reset_cc
if compile hello.tr -o hello; then pass "compiled"; else fail "compile failed: $(tail -3 out.log)"; fi
ran_cc && fail "the system cc was run" || pass "the system cc was not run"
if [ -x hello ] && is_self_contained hello; then pass "the executable needs nothing installed"; else fail "the executable depends on more than the system"; fi
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
mv "$RT/$REQUIRED_FILE" "$RT/$REQUIRED_FILE.off"; reset_cc
compile hello.tr -o hello3 && fail "expected the decoy cc to fail the link" || pass "the link failed (decoy cc exit 99)"
ran_cc && pass "the decoy cc was run (fell back)" || fail "no fallback to cc"
"$INST/bin/turfc" --version | grep -q '^bundled-runtime' && fail "--version still lists a runtime" || pass "--version no longer lists a bundled-runtime"
mv "$RT/$REQUIRED_FILE.off" "$RT/$REQUIRED_FILE"

echo "== a runtime that is present but broken fails clearly, without falling back"
cp "$RT/$BREAK_FILE" "$RT/$BREAK_FILE.good"; : > "$RT/$BREAK_FILE"; reset_cc
if compile hello.tr -o hello4; then fail "expected the link to fail"; else pass "the link failed"; fi
grep -q "the linker built into turfc" out.log && pass "the message names the embedded linker" || fail "message does not name the embedded linker: $(tail -3 out.log)"
grep -q "TURF_LINKER=system" out.log && pass "the message names the escape hatch" || fail "message does not name TURF_LINKER=system"
ran_cc && fail "it fell back to cc after a bundled failure" || pass "no silent fallback to cc"
mv "$RT/$BREAK_FILE.good" "$RT/$BREAK_FILE"

if [ "$REAL_RUNTIME" = 1 ]; then
  echo "== the shipped runtime resolves host names (static glibc cannot)"
  cat > dns.tr <<'TR'
import io;
import net;

fn main(): int {
    immut ip: string = net::resolve_hostname("localhost") recover produce "RESOLUTION_FAILED";
    io::printline(ip);
    return 0;
}
TR
  reset_cc
  if compile dns.tr -o dns; then pass "compiled"; else fail "compile failed: $(tail -3 out.log)"; fi
  ran_cc && fail "the system cc was run" || pass "the system cc was not run"
  [ "$(./dns 2>&1)" = "127.0.0.1" ] && pass "localhost resolves to 127.0.0.1" || fail "net::resolve_hostname(\"localhost\") gave: $(./dns 2>&1 | tail -2)"
fi

echo
if [ "$FAILED" -ne 0 ]; then echo "bundled_link_smoke: $FAILED check(s) failed"; exit 1; fi
echo "bundled_link_smoke: all checks passed"
