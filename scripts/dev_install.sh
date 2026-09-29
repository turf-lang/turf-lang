#!/bin/bash
#
# dev_install.sh -- put this checkout's `turf` and `turfc` on your PATH.
#
# Symlinks <prefix>/bin/turf -> build/turf and <prefix>/bin/turfc ->
# build/turfc (prefix default: ~/.local). After that, `turf file.tr`,
# `turf run file.tr` and `turfc file.tr` work from any directory with no
# environment variables at all, and every rebuild is picked up immediately,
# since the links point at the build tree rather than at a copy.
#
# Why symlinks are enough, and why TURF_HOME is not needed: `turfc` finds its
# install root (lib/std, lib/libturfrt.a) from its own REAL path, <bin>/..
# (TurfInstallRoot(), src/ParserModule.cpp), and `turf` finds `turfc` beside
# its own real path with symlinks resolved (FREEZE.md 6.16, F-PKG-1). Both
# land back in this checkout. Exporting TURF_HOME on top is worse than
# redundant: it wins over the binary's own location, so any OTHER build (a
# worktree, a scratch build directory) silently uses this checkout's stdlib
# and runtime instead of its own.
#
# This is a developer convenience, not the release install. A real install
# is an unpacked release archive (REL-003).
#
# Usage: scripts/dev_install.sh [--prefix DIR] [--uninstall]

set -u
cd "$(dirname "$0")/.." || exit 1
REPO=$(pwd -P)

PREFIX="$HOME/.local"
UNINSTALL=0
while [ $# -gt 0 ]; do
  case "$1" in
    --prefix) PREFIX="${2:?--prefix needs a directory}"; shift 2 ;;
    --uninstall) UNINSTALL=1; shift ;;
    -h|--help) sed -n '3,24p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "dev_install.sh: unknown option '$1' (try --help)" >&2; exit 2 ;;
  esac
done
BIN="$PREFIX/bin"

if [ "$UNINSTALL" = 1 ]; then
  for name in turf turfc; do
    link="$BIN/$name"
    # Only remove a link that points into THIS checkout: a real installed
    # turf, or another checkout's link, is not ours to delete.
    if [ -L "$link" ] && [ "$(readlink "$link")" = "$REPO/build/$name" ]; then
      rm "$link" && echo "removed $link"
    fi
  done
  exit 0
fi

if [ ! -x "$REPO/build/turfc" ]; then
  echo "dev_install.sh: $REPO/build/turfc not found; build first (./scripts/cmake_build.sh)" >&2
  exit 1
fi

mkdir -p "$BIN" || exit 1
for name in turf turfc; do
  target="$REPO/build/$name"
  link="$BIN/$name"
  if [ ! -x "$target" ]; then
    # build/turf is optional: it only exists when Go was found at configure
    # time. turfc alone is a complete compiler.
    echo "skipped $name: $target not built (Go not installed?)"
    continue
  fi
  if [ -e "$link" ] && [ ! -L "$link" ]; then
    echo "dev_install.sh: $link exists and is not a symlink; not replacing it" >&2
    exit 1
  fi
  ln -sfn "$target" "$link" && echo "$link -> $target"
done

case ":$PATH:" in
  *":$BIN:"*) ;;
  *) echo "note: $BIN is not on your PATH; add it in your shell's rc file." ;;
esac
if [ -n "${TURF_HOME:-}" ]; then
  echo "note: TURF_HOME is set ($TURF_HOME). It is not needed with these links,"
  echo "      and it makes every other build use that checkout's stdlib; consider"
  echo "      removing it from your shell's rc file."
fi
