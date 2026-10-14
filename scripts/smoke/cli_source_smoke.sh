#!/bin/bash
#
# cli_source_smoke.sh -- the `turf` front end is built from the turf-cli
# repository, not from a copy inside turf-lang.
#
# turf-lang holds no Go source for `turf`. CMake builds build/turf from
# TURF_CLI_DIR (by default a turf-cli checkout next to this one), and Go stamps
# the binary with its module path and the git revision it was built from. This
# checks, from that stamp:
#   - turf-lang has no cli/ (no second copy to drift from turf-cli);
#   - TURF_CLI_DIR is the turf-cli module (github.com/turf-lang/turf-cli);
#   - the build directory's CMake cache points at that same checkout;
#   - turf's main package and module are turf-cli's;
#   - turf's vcs.revision is the turf-cli checkout's HEAD, so the binary
#     is current with the checkout and was not built inside turf-lang.
#   - the checkout is at the commit TURF_CLI_REVISION pins: required in CI
#     (every build of one turf-lang commit ships the same turf), a NOTE
#     locally, where ../turf-cli may be ahead while turf is being changed.
#
# Usage: scripts/smoke/cli_source_smoke.sh [--build-dir=DIR]
#            (after building DIR/turf; DIR defaults to build)
# Exit:  0 = all checks pass, or DIR/turf or `go` is absent and CI is not
#            set (Go is optional for a local build: a SKIP, never a silent pass
#            in CI); 1 = a check failed (each failure is printed).

set -u
cd "$(dirname "$0")/../.." || exit 1
ROOT=$(pwd)
BUILD=build
for arg in "$@"; do
  case $arg in
    --build-dir=*) BUILD=${arg#--build-dir=} ;;
    *) echo "usage: $0 [--build-dir=DIR]"; exit 1 ;;
  esac
done
TURF="$BUILD/turf"
[ -x "$TURF.exe" ] && TURF="$TURF.exe"
# Windows (MSYS2): TURF_CLI_DIR and the CMake cache hold D:\ or D:/ paths.
unixpath() { if command -v cygpath >/dev/null 2>&1; then cygpath -u "$1"; else printf '%s\n' "$1"; fi; }
MODULE=github.com/turf-lang/turf-cli

skip_or_fail() {
  if [ -n "${CI:-}" ]; then
    echo "FAIL: $1"
    exit 1
  fi
  echo "SKIP: $1"
  exit 0
}

[ -x "$TURF" ] || skip_or_fail "$TURF is missing (was Go installed before the CMake build?)"
command -v go >/dev/null 2>&1 || skip_or_fail "go is not on PATH, so $TURF's build stamp cannot be read"

FAILS=0
pass() { echo "PASS: $1"; }
fail() { echo "FAIL: $1"; FAILS=$((FAILS + 1)); }

# --- turf-lang carries no copy -------------------------------------------------
if [ -e cli/go.mod ] || [ -n "$(git ls-files cli 2>/dev/null)" ]; then
  fail "turf-lang has a cli/ directory; turf's source lives only in turf-cli"
else
  pass "turf-lang has no cli/"
fi

# --- TURF_CLI_DIR is turf-cli ----------------------------------------------------
CLI_DIR=$(unixpath "${TURF_CLI_DIR:-../turf-cli}")
if [ ! -f "$CLI_DIR/go.mod" ]; then
  fail "no turf-cli checkout at $CLI_DIR (set TURF_CLI_DIR)"
  echo "$FAILS check(s) failed"
  exit 1
fi
CLI_DIR=$(cd "$CLI_DIR" && pwd -P)
GOMOD_MODULE=$(sed -n 's/^module[[:space:]]*//p' "$CLI_DIR/go.mod")
if [ "$GOMOD_MODULE" = "$MODULE" ]; then
  pass "$CLI_DIR is module $MODULE"
else
  fail "$CLI_DIR/go.mod declares module '$GOMOD_MODULE', not $MODULE"
fi

# --- the build directory was configured against it -------------------------------
CACHED=$(sed -n 's/^TURF_CLI_DIR:PATH=//p' "$BUILD/CMakeCache.txt" 2>/dev/null | tr -d '\r')
[ -n "$CACHED" ] && CACHED=$(unixpath "$CACHED")
if [ -z "$CACHED" ]; then
  fail "$BUILD/CMakeCache.txt has no TURF_CLI_DIR"
elif [ ! -d "$CACHED" ] || [ "$(cd "$CACHED" && pwd -P)" != "$CLI_DIR" ]; then
  fail "the CMake cache builds turf from '$CACHED', not $CLI_DIR (re-run cmake)"
else
  pass "the CMake cache builds turf from $CLI_DIR"
fi

# --- turf's own stamp ----------------------------------------------------------
INFO=$(go version -m "$TURF" 2>&1) || { fail "go version -m cannot read $TURF: $INFO"; echo "$FAILS check(s) failed"; exit 1; }
field() { printf '%s\n' "$INFO" | awk -v k="$1" '$1 == k { print $2; exit }'; }
setting() { printf '%s\n' "$INFO" | awk -v k="$1" '$1 == "build" { split($2, kv, "="); if (kv[1] == k) { print substr($2, length(k) + 2); exit } }'; }

PKG=$(field path)
MOD=$(field mod)
[ "$PKG" = "$MODULE/cmd/turf" ] && pass "$TURF's main package is $PKG" \
  || fail "$TURF's main package is '$PKG', not $MODULE/cmd/turf"
[ "$MOD" = "$MODULE" ] && pass "$TURF's module is $MOD" \
  || fail "$TURF's module is '$MOD', not $MODULE"

REV=$(setting vcs.revision)
CLI_HEAD=$(git -C "$CLI_DIR" rev-parse HEAD 2>/dev/null)
if [ -z "$CLI_HEAD" ]; then
  fail "$CLI_DIR is not a git checkout, so $TURF's revision cannot be checked against it"
elif [ -z "$REV" ]; then
  fail "$TURF carries no vcs.revision (built with -buildvcs=false, or outside a git checkout)"
elif [ "$REV" = "$CLI_HEAD" ]; then
  pass "$TURF was built from turf-cli at ${REV:0:12}"
  [ "$(setting vcs.modified)" = "true" ] && echo "NOTE: that turf-cli checkout had uncommitted changes"
elif git cat-file -e "$REV^{commit}" 2>/dev/null && ! git -C "$CLI_DIR" cat-file -e "$REV^{commit}" 2>/dev/null; then
  fail "$TURF was built inside turf-lang (revision ${REV:0:12} is a turf-lang commit; re-run cmake and rebuild)"
else
  fail "$TURF was built from revision ${REV:0:12}, but $CLI_DIR is at ${CLI_HEAD:0:12} (rebuild)"
fi

# --- the pinned revision ------------------------------------------------------------
PINNED=$(tr -d '[:space:]' < TURF_CLI_REVISION 2>/dev/null)
if [ -z "$PINNED" ]; then
  fail "TURF_CLI_REVISION is missing or empty"
elif [ -z "$CLI_HEAD" ]; then
  : # reported above
elif [ "$CLI_HEAD" = "$PINNED" ]; then
  pass "turf-cli is at the pinned revision ${PINNED:0:12}"
elif [ -n "${CI:-}" ]; then
  fail "turf-cli is at ${CLI_HEAD:0:12}, but TURF_CLI_REVISION pins ${PINNED:0:12}"
else
  echo "NOTE: turf-cli is at ${CLI_HEAD:0:12}, but TURF_CLI_REVISION pins ${PINNED:0:12} (update the pin before turf-lang commits that need it)"
fi

if [ "$FAILS" -ne 0 ]; then
  echo "$FAILS check(s) failed"
  exit 1
fi
echo "All checks passed"
