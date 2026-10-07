#!/bin/bash
#
# package_release.sh -- build one release archive in the REL-003 layout
# (docs/development/FREEZE.md 6.26):
#
#   turf-v<ver>-<platform>/
#     bin/turf  bin/turfc           (+ .exe on Windows, and any --bin-extra DLLs)
#     lib/libturfrt.a  lib/std/**
#     lib/rt/<platform>/**          (the bundled C runtime, only when built:
#                                    REL-008 stage 4; turfc's embedded linker uses it)
#     docs/errors/**  docs/slm/**   (`turf explain` and `--smart` read these)
#     share/turf/examples/**
#     LICENSE  README.md
#
# plus `<archive>.sha256` beside it. The layout is what makes an unpacked
# archive work with no setup: `turfc` derives its install root from its own
# real path, <bin>/.., and both `turf` (finds `turfc` beside itself) and the
# compiler (finds lib/ above itself) are satisfied by exactly this shape. The
# old archives put the binary at the top level, one directory too high for that
# rule, and relied on TURF_HOME or a lucky working directory.
#
# This is a script, not steps inside the workflow, so the packaging that CI
# publishes is the packaging a developer can run and open (scripts/
# release_smoke.sh does exactly that). The version comes from the root VERSION
# file; a tag that disagrees with it is an error (REL-001's deferred
# single-sourcing item), because an archive named v2.0.1 that contains a
# compiler reporting 2.0.0 is the bug this prevents.
#
# Usage: scripts/package_release.sh --platform <name> [--build-dir DIR]
#            [--out DIR] [--format tar.gz|zip] [--bin-extra FILE]...
#            [--expect-tag vX.Y.Z]
#   --platform     linux-x86_64, linux-arch-x86_64, macos-arm64, windows-x86_64
#   --build-dir    where turf/turfc were built (default: <repo>/build)
#   --out          where the archive and its checksum go (default: the current
#                  directory); relative paths are relative to where you run it
#   --format       tar.gz (default; zip when the platform is windows-*)
#   --bin-extra    an extra file for bin/ (a DLL the compiler needs); repeatable
#   --expect-tag   fail unless this equals v<VERSION> (the release workflow
#                  passes the pushed tag)
# Prints the archive path on the last line of stdout.

set -eu
# Relative paths on the command line mean relative to where the caller is, not
# to the repository root this script moves into.
CALLER=$(pwd)
abs() { case "$1" in /*|[A-Za-z]:*) printf '%s\n' "$1" ;; *) printf '%s\n' "$CALLER/$1" ;; esac; }
cd "$(dirname "$0")/.."

PLATFORM=""
BUILD=""
OUT="$CALLER"
FORMAT=""
EXPECT_TAG=""
EXTRAS=()
while [ $# -gt 0 ]; do
  case "$1" in
    --platform) PLATFORM="${2:?--platform needs a name}"; shift 2 ;;
    --build-dir) BUILD=$(abs "${2:?--build-dir needs a directory}"); shift 2 ;;
    --out) OUT=$(abs "${2:?--out needs a directory}"); shift 2 ;;
    --format) FORMAT="${2:?--format needs tar.gz or zip}"; shift 2 ;;
    --bin-extra) EXTRAS+=("$(abs "${2:?--bin-extra needs a file}")"); shift 2 ;;
    --expect-tag) EXPECT_TAG="${2:?--expect-tag needs a tag}"; shift 2 ;;
    -h|--help) sed -n '3,40p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "package_release.sh: unknown option '$1' (try --help)" >&2; exit 2 ;;
  esac
done
[ -n "$PLATFORM" ] || { echo "package_release.sh: --platform is required" >&2; exit 2; }
if [ -z "$FORMAT" ]; then
  case "$PLATFORM" in windows-*) FORMAT=zip ;; *) FORMAT=tar.gz ;; esac
fi
case "$FORMAT" in tar.gz|zip) ;; *) echo "package_release.sh: --format must be tar.gz or zip" >&2; exit 2 ;; esac

[ -n "$BUILD" ] || BUILD="$(pwd)/build"
VERSION=$(tr -d '[:space:]' < VERSION)
if [ -n "$EXPECT_TAG" ]; then
  # A -dev VERSION is the development line, never a release; an rc suffix is
  # fine (v1.0.0-rc1 was a real dry run) as long as the tag spells it too.
  case "$VERSION" in
    *-dev) echo "package_release.sh: VERSION is '$VERSION', a development version; set it to the release version (for example 2.0.0) before tagging." >&2; exit 1 ;;
  esac
  if [ "$EXPECT_TAG" != "v$VERSION" ]; then
    echo "package_release.sh: the tag is '$EXPECT_TAG' but VERSION says '$VERSION' (so the tag should be 'v$VERSION')." >&2
    echo "  Fix VERSION and the README's version line, or tag again; an archive must not carry a different version than its name." >&2
    exit 1
  fi
fi

EXE=""
case "$PLATFORM" in windows-*) EXE=".exe" ;; esac
for f in "$BUILD/turfc$EXE" "$BUILD/turf$EXE" lib/libturfrt.a; do
  [ -f "$f" ] || { echo "package_release.sh: $f not found; build first (./scripts/cmake_build.sh --release)" >&2; exit 1; }
done

NAME="turf-v$VERSION-$PLATFORM"
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
DIR="$STAGE/$NAME"
mkdir -p "$DIR/bin" "$DIR/lib" "$DIR/docs" "$DIR/share/turf"

cp "$BUILD/turfc$EXE" "$BUILD/turf$EXE" "$DIR/bin/"
for f in ${EXTRAS[@]+"${EXTRAS[@]}"}; do
  [ -f "$f" ] || { echo "package_release.sh: --bin-extra $f not found" >&2; exit 1; }
  cp "$f" "$DIR/bin/"
done
# libturfrt.a only: lib/ in a checkout also holds libturfrt_asan.a, which is a
# sanitizer build for developers and is not what a program should link.
cp lib/libturfrt.a "$DIR/lib/"
cp -r lib/std "$DIR/lib/"
# The bundled C runtime the embedded linker links programs against. Absent until
# REL-008 stage 4 builds it; an archive without it still works (turfc then runs
# the system cc), and with it, a program that names no other C library links
# without one.
if [ -d lib/rt ]; then
  cp -r lib/rt "$DIR/lib/"
fi
cp -r docs/errors docs/slm "$DIR/docs/"
cp -r examples "$DIR/share/turf/examples"
# A project built inside the checkout leaves build/ and .turf/ behind.
find "$DIR/share/turf/examples" \( -name build -o -name .turf \) -prune -exec rm -rf {} + 2>/dev/null || true
cp LICENSE README.md "$DIR/"

mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
if [ "$FORMAT" = zip ]; then
  ARCHIVE="$OUT/$NAME.zip"
  rm -f "$ARCHIVE"
  if command -v 7z >/dev/null 2>&1; then
    (cd "$STAGE" && 7z a "$ARCHIVE" "$NAME" >/dev/null)
  elif command -v zip >/dev/null 2>&1; then
    (cd "$STAGE" && zip -qr "$ARCHIVE" "$NAME")
  else
    echo "package_release.sh: need 7z or zip to make a .zip" >&2; exit 1
  fi
else
  ARCHIVE="$OUT/$NAME.tar.gz"
  tar -czf "$ARCHIVE" -C "$STAGE" "$NAME"
fi

# `<hash>  <file name>`, the form `sha256sum -c` / `shasum -a 256 -c` read,
# with the bare file name so it verifies from the directory it was downloaded to.
(cd "$OUT" && { sha256sum "$(basename "$ARCHIVE")" 2>/dev/null || shasum -a 256 "$(basename "$ARCHIVE")"; } > "$(basename "$ARCHIVE").sha256")

echo "$ARCHIVE"
