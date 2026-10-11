#!/bin/sh
#
# install.sh -- install TurfLang from a GitHub release archive (Linux, macOS).
#
#   curl -fsSL https://raw.githubusercontent.com/turf-lang/turf-lang/master/install.sh | sh
#   sh install.sh --version v2.0.0 --prefix ~/tools/turf --no-modify-path
#   sh install.sh --uninstall
#
# What it does: finds the archive for this machine in the release's
# checksums.txt, downloads it, VERIFIES its SHA-256 (it stops, and never
# installs, if the check cannot run or fails), unpacks it into the prefix
# (default ~/.turf, so `turf` is ~/.turf/bin/turf), and adds that bin
# directory to your PATH with one clearly marked block in your shell's
# start-up file, so `turf` works from any directory in a NEW terminal.
#
# The checksum comes from the same release as the archive. It catches a
# damaged or truncated download, not a compromised release; archive signing is
# separate (ROADMAP_v2.0.0.md REL-008 stage 7).
#
# Options:
#   --version V        install release V (v2.0.0 or 2.0.0); default: the latest
#   --prefix DIR       install here instead of ~/.turf  (env: TURF_INSTALL_DIR)
#   --no-modify-path   edit no start-up file; print the line to add instead
#   --uninstall        remove what this script installed and its PATH block
#   -h, --help         this text
#
# Environment: TURF_INSTALL_BASE_URL points at a mirror, or a directory of
# `download/<tag>/...` files (file:// works), instead of
# https://github.com/turf-lang/turf-lang/releases. TURF_INSTALL_PLATFORM
# overrides platform detection (linux-x86_64, macos-arm64).
#
# Uninstall removes exactly what is listed in <prefix>/.turf-install and the
# marked PATH blocks; it never touches your projects or the package cache
# (~/.cache/turf, or $TURF_CACHE_DIR), which is disposable and not ours.

set -eu

BASE_URL="${TURF_INSTALL_BASE_URL:-https://github.com/turf-lang/turf-lang/releases}"
PREFIX="${TURF_INSTALL_DIR:-$HOME/.turf}"
VERSION=""
MODIFY_PATH=1
UNINSTALL=0

MARK_BEGIN="# >>> turf >>>"
MARK_END="# <<< turf <<<"
# What a release archive puts at its top level; install and uninstall touch
# these names and nothing else under the prefix.
OWNED="bin lib docs share LICENSE README.md"

say() { printf '%s\n' "$*"; }
die() { printf 'install.sh: %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
  case "$1" in
    --version) [ $# -ge 2 ] || die "--version needs a value"; VERSION="$2"; shift 2 ;;
    --prefix) [ $# -ge 2 ] || die "--prefix needs a directory"; PREFIX="$2"; shift 2 ;;
    --no-modify-path) MODIFY_PATH=0; shift ;;
    --uninstall) UNINSTALL=1; shift ;;
    -h|--help) sed -n '3,34p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) printf 'install.sh: unknown option %s (try --help)\n' "$1" >&2; exit 2 ;;
  esac
done

# ---- the shell start-up files we may edit -----------------------------------

# The file the PATH block goes in, for this user's login shell. Printed with
# the shell it was chosen for; the caller says so when it is the fallback.
rc_file_for_shell() {
  os=$(uname -s)
  case "$(basename "${SHELL:-}")" in
    zsh) printf '%s\n' "${ZDOTDIR:-$HOME}/.zshrc" ;;
    bash)
      # macOS starts bash as a login shell, which reads ~/.bash_profile and
      # not ~/.bashrc; Linux terminals start an interactive non-login shell.
      if [ "$os" = Darwin ]; then printf '%s\n' "$HOME/.bash_profile"
      else printf '%s\n' "$HOME/.bashrc"; fi ;;
    fish) printf '%s\n' "${XDG_CONFIG_HOME:-$HOME/.config}/fish/conf.d/turf.fish" ;;
    *) printf '%s\n' "$HOME/.profile" ;;
  esac
}

# Every file a previous install may have edited, so uninstall finds the block
# even if the user has changed shells since.
all_rc_files() {
  printf '%s\n' "${ZDOTDIR:-$HOME}/.zshrc" "$HOME/.bashrc" "$HOME/.bash_profile" \
    "$HOME/.profile" "${XDG_CONFIG_HOME:-$HOME/.config}/fish/conf.d/turf.fish"
}

# Drops the marked block (if any) from $1, and the one blank line add_block put
# in front of it, so install followed by uninstall gives the file back as it was.
# Every other line stays as it was.
remove_block() {
  f="$1"
  [ -f "$f" ] || return 0
  grep -qF "$MARK_BEGIN" "$f" 2>/dev/null || return 0
  tmp=$(mktemp)
  awk -v b="$MARK_BEGIN" -v e="$MARK_END" '
    skip { if ($0 == e) skip = 0; next }
    $0 == b { held = 0; skip = 1; next }
    held { print ""; held = 0 }
    $0 == "" { held = 1; next }
    { print }
    END { if (held) print "" }' "$f" > "$tmp"
  # cat into place, not mv: keeps the file's mode, owner and any symlink
  # (dotfile managers link ~/.zshrc elsewhere).
  cat "$tmp" > "$f"
  rm -f "$tmp"
}

# bin as it should be written in a start-up file: $HOME-relative when it is
# under the home directory, so the file survives a renamed home.
bin_for_rc() {
  case "$PREFIX/bin" in
    "$HOME"/*) printf '%s\n' "\$HOME/${PREFIX#"$HOME"/}/bin" ;;
    *) printf '%s\n' "$PREFIX/bin" ;;
  esac
}

add_block() {
  f="$1"
  mkdir -p "$(dirname "$f")"
  remove_block "$f"
  b=$(bin_for_rc)
  case "$f" in
    */turf.fish)
      # Not ours to share: conf.d/turf.fish is a file of its own, so a plain
      # overwrite is the whole of its uninstall too.
      { printf '%s\n' "$MARK_BEGIN"
        printf 'fish_add_path -g "%s"\n' "$b"
        printf '%s\n' "$MARK_END"; } > "$f" ;;
    *)
      # A file whose last line has no newline gets one first; that, unlike
      # the blank line after it, is not undone by uninstall.
      { if [ -s "$f" ]; then
          [ -n "$(tail -c 1 "$f")" ] && printf '\n'
          printf '\n'
        fi
        printf '%s\n' "$MARK_BEGIN"
        printf 'export PATH="%s:$PATH"\n' "$b"
        printf '%s\n' "$MARK_END"; } >> "$f" ;;
  esac
}

# ---- uninstall --------------------------------------------------------------

if [ "$UNINSTALL" = 1 ]; then
  manifest="$PREFIX/.turf-install"
  [ -f "$manifest" ] || die "nothing to uninstall: $manifest does not exist (this script did not install into $PREFIX)."
  for name in $OWNED; do
    [ -e "$PREFIX/$name" ] && rm -rf "${PREFIX:?}/$name" && say "removed $PREFIX/$name"
  done
  rm -f "$manifest"
  rmdir "$PREFIX" 2>/dev/null || true
  for f in $(all_rc_files); do
    if [ -f "$f" ] && grep -qF "$MARK_BEGIN" "$f" 2>/dev/null; then
      case "$f" in
        */turf.fish) rm -f "$f" ;;
        *) remove_block "$f" ;;
      esac
      say "removed the turf PATH block from $f"
    fi
  done
  say "TurfLang is uninstalled. Your projects and the package cache were not touched."
  exit 0
fi

# ---- platform and download --------------------------------------------------

if [ -n "${TURF_INSTALL_PLATFORM:-}" ]; then
  PLATFORM="$TURF_INSTALL_PLATFORM"
else
  os=$(uname -s); arch=$(uname -m)
  case "$os/$arch" in
    Linux/x86_64|Linux/amd64) PLATFORM=linux-x86_64 ;;
    Darwin/arm64|Darwin/aarch64) PLATFORM=macos-arm64 ;;
    MINGW*|MSYS*|CYGWIN*) die "this is Windows; use install.ps1 from PowerShell instead (irm https://raw.githubusercontent.com/turf-lang/turf-lang/master/install.ps1 | iex), or download the .zip from $BASE_URL." ;;
    *) die "there is no TurfLang release for $os on $arch. Supported: Linux x86-64, macOS arm64 (Windows x86-64 from the .zip)." ;;
  esac
fi

if [ -n "$VERSION" ]; then
  case "$VERSION" in v*) TAG="$VERSION" ;; *) TAG="v$VERSION" ;; esac
  DL="$BASE_URL/download/$TAG"
else
  DL="$BASE_URL/latest/download"
fi

# fetch URL DEST [progress]: quiet by default; with `progress` and a terminal on
# stderr, the tool's own progress bar. The archive is tens of MB, and a silent
# download looks exactly like a hang. Not a terminal (a CI log, a pipe to a
# file): no bar, since a bar redrawn a hundred times is noise there, and the
# step lines around it still say what is happening.
if command -v curl >/dev/null 2>&1; then
  fetch() {
    if [ "${3:-}" = progress ] && [ -t 2 ]; then curl -fL --retry 3 -# -o "$2" "$1"
    else curl -fsSL --retry 3 -o "$2" "$1"; fi
  }
elif command -v wget >/dev/null 2>&1; then
  fetch() {
    # --show-progress needs wget 1.16 (2014); older ones just stay quiet.
    if [ "${3:-}" = progress ] && [ -t 2 ] && wget --help 2>&1 | grep -q -- --show-progress; then
      wget -q --show-progress -O "$2" "$1"
    else wget -q -O "$2" "$1"; fi
  }
else
  die "need curl or wget to download the release."
fi

if command -v sha256sum >/dev/null 2>&1; then
  sha256_of() { sha256sum "$1" | awk '{print $1}'; }
elif command -v shasum >/dev/null 2>&1; then
  sha256_of() { shasum -a 256 "$1" | awk '{print $1}'; }
else
  die "need sha256sum or shasum to verify the download; refusing to install an unverified archive."
fi
command -v tar >/dev/null 2>&1 || die "need tar to unpack the release."

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM

say "[1/4] Looking up the release ($DL)"
fetch "$DL/checksums.txt" "$WORK/checksums.txt" || die "could not download $DL/checksums.txt (is there a release${VERSION:+ $VERSION}?)"

# The one line for this platform. `-linux-x86_64.tar.gz` cannot match
# `-linux-arch-x86_64.tar.gz`, so the Arch build is never picked by accident.
LINE=$(awk -v p="$PLATFORM" '{ f = $2; sub(/^\*/, "", f) } f ~ ("^turf-v.*-" p "\\.tar\\.gz$") { print $1, f }' "$WORK/checksums.txt")
[ -n "$LINE" ] || die "this release has no $PLATFORM archive. checksums.txt lists: $(awk '{print $2}' "$WORK/checksums.txt" | tr '\n' ' ')"
[ "$(printf '%s\n' "$LINE" | wc -l | tr -d ' ')" = 1 ] || die "this release lists more than one $PLATFORM archive; refusing to guess."
WANT=$(printf '%s\n' "$LINE" | awk '{print $1}')
ARCHIVE=$(printf '%s\n' "$LINE" | awk '{print $2}')

say "[2/4] Downloading $ARCHIVE"
fetch "$DL/$ARCHIVE" "$WORK/$ARCHIVE" progress || die "could not download $DL/$ARCHIVE"
GOT=$(sha256_of "$WORK/$ARCHIVE")
if [ "$GOT" != "$WANT" ]; then
  die "SHA-256 mismatch for $ARCHIVE: the release says $WANT, the download is $GOT. Nothing was installed."
fi
say "[3/4] SHA-256 verified."

say "[4/4] Unpacking into $PREFIX"
mkdir "$WORK/unpacked"
tar -xzf "$WORK/$ARCHIVE" -C "$WORK/unpacked"
SRC=$(ls -d "$WORK"/unpacked/turf-v* 2>/dev/null | head -n 1)
[ -n "$SRC" ] && [ -x "$SRC/bin/turf" ] && [ -x "$SRC/bin/turfc" ] || die "$ARCHIVE does not have the expected layout (bin/turf, bin/turfc); nothing was installed."

# ---- install ----------------------------------------------------------------

mkdir -p "$PREFIX"
# Move the old install aside and put it back if the new one cannot be moved in,
# so a failed upgrade leaves the previous working install, not half of each.
mkdir "$WORK/old"
for name in $OWNED; do
  [ -e "$PREFIX/$name" ] && mv "$PREFIX/$name" "$WORK/old/$name"
done
restore() {
  for name in $OWNED; do rm -rf "${PREFIX:?}/$name"; done
  for name in $OWNED; do [ -e "$WORK/old/$name" ] && mv "$WORK/old/$name" "$PREFIX/$name"; done
  true
}
for name in $OWNED; do
  if [ -e "$SRC/$name" ] && ! mv "$SRC/$name" "$PREFIX/$name"; then
    restore
    die "could not move $name into $PREFIX; the previous install (if any) was restored."
  fi
done

INSTALLED=$("$PREFIX/bin/turfc" --version 2>/dev/null | head -n 1 || true)
{
  printf 'archive=%s\n' "$ARCHIVE"
  printf 'compiler=%s\n' "$INSTALLED"
} > "$PREFIX/.turf-install"

say "Installed $INSTALLED into $PREFIX"

# ---- PATH -------------------------------------------------------------------

if [ "$MODIFY_PATH" = 1 ]; then
  RC=$(rc_file_for_shell)
  add_block "$RC"
  case "$(basename "${SHELL:-}")" in
    zsh|bash|fish) say "Added $PREFIX/bin to your PATH in $RC." ;;
    *) say "Your shell (${SHELL:-unknown}) is not one this installer knows, so the PATH line went in $RC; if your shell does not read it, add $PREFIX/bin to your PATH yourself." ;;
  esac
  say "Open a new terminal, or run this to use it right now:"
  say "  export PATH=\"$PREFIX/bin:\$PATH\""
else
  say "Not editing any start-up file (--no-modify-path). Add this to your PATH:"
  say "  export PATH=\"$PREFIX/bin:\$PATH\""
fi
say
say "Try it:  turf new hello && cd hello && turf run"
