#!/bin/bash
#
# dev_install.sh -- put this checkout's `turf` on your PATH.
#
# Symlinks <prefix>/bin/turf -> build/turf (prefix default: ~/.local). After
# that, `turf file.tr`, `turf run file.tr` and `turf check file.tr` work from
# any directory with no environment variables at all, and every rebuild is
# picked up immediately, since the link points at the build tree rather than
# at a copy. `turf` finds build/turfc by itself.
#
# `turfc` is the tooling interface, not the user's command (FREEZE.md 6.16,
# F-PKG-1), so it is only linked with --with-turfc: for tool authors who want
# to call the compiler directly from any directory.
#
# Why symlinks are enough, and why TURF_HOME is not needed: `turfc` finds its
# install root (lib/std, lib/libturfrt.a) from its own REAL path, <bin>/..
# (TurfInstallRoot(), src/frontend/ParserModule.cpp), and `turf` finds `turfc` beside
# its own real path with symlinks resolved (FREEZE.md 6.16, F-PKG-1). Both
# land back in this checkout. Exporting TURF_HOME on top is worse than
# redundant: it wins over the binary's own location, so any OTHER build (a
# worktree, a scratch build directory) silently uses this checkout's stdlib
# and runtime instead of its own.
#
# This is a developer convenience, not the release install. A real install
# is a release archive installed with scripts/install/install.sh (REL-003), which
# also edits your shell start-up file so `turf` is on PATH in new terminals;
# this script never edits one.
#
# Usage: scripts/install/dev_install.sh [--prefix DIR] [--with-turfc] [--uninstall]

set -u
cd "$(dirname "$0")/../.." || exit 1
REPO=$(pwd -P)

PREFIX="$HOME/.local"
UNINSTALL=0
NAMES="turf"
while [ $# -gt 0 ]; do
  case "$1" in
    --prefix) PREFIX="${2:?--prefix needs a directory}"; shift 2 ;;
    --with-turfc) NAMES="turf turfc"; shift ;;
    --uninstall) UNINSTALL=1; shift ;;
    -h|--help) sed -n '3,28p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
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
for name in $NAMES; do
  target="$REPO/build/$name"
  link="$BIN/$name"
  if [ ! -x "$target" ]; then
    # build/turf only exists when Go was found at configure time. Without it
    # there is nothing user-facing to link; --with-turfc still links the
    # compiler for tool authors.
    echo "skipped $name: $target not built (Go not installed? then use --with-turfc)"
    continue
  fi
  if [ -e "$link" ] && [ ! -L "$link" ]; then
    echo "dev_install.sh: $link exists and is not a symlink; not replacing it" >&2
    exit 1
  fi
  ln -sfn "$target" "$link" && echo "$link -> $target"
done

# Print the exact command that puts $BIN on PATH for the user's shell, not
# just "add it to your rc file", which assumes the reader knows which file and
# what to write.
#
# FOR NOW ONLY. This is a developer script, and printing a command is all it
# can safely do: editing someone's shell start-up files unasked is not.
# Proper PATH management for every OS belongs to the release installers
# (ROADMAP_v2.0.0.md REL-008 stage 7: install.sh / install.ps1, covering
# bash/zsh/fish, macOS's login shells, and the Windows user PATH). Revisit
# this, and the matching note in `turf version` (turf-cli's cmd/turf/pathhint.go),
# when those land.
case ":$PATH:" in
  *":$BIN:"*) ;;
  *)
    echo "note: $BIN is not on your PATH yet, so 'turf' will not be found."
    case "$(basename "${SHELL:-sh}")" in
      zsh)
        echo "      Add it (zsh):   echo 'export PATH=\"$BIN:\$PATH\"' >> ~/.zshrc" ;;
      bash)
        # macOS starts bash as a login shell, which reads ~/.bash_profile, not ~/.bashrc.
        if [ "$(uname -s)" = "Darwin" ]; then RC="~/.bash_profile"; else RC="~/.bashrc"; fi
        echo "      Add it (bash):  echo 'export PATH=\"$BIN:\$PATH\"' >> $RC" ;;
      fish)
        echo "      Add it (fish):  fish_add_path $BIN" ;;
      *)
        echo "      Add this line to your shell's start-up file:  export PATH=\"$BIN:\$PATH\"" ;;
    esac
    echo "      Then open a new terminal (or run the same export line in this one)."
    ;;
esac
if [ -n "${TURF_HOME:-}" ]; then
  echo "note: TURF_HOME is set ($TURF_HOME). It is not needed with these links,"
  echo "      and it makes every other build use that checkout's stdlib; consider"
  echo "      removing it from your shell's rc file."
fi
