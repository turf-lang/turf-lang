#!/bin/bash
#
# release_smoke.sh -- REL-003: the release archive and install.sh work as a
# user would meet them, not as a developer in a checkout would. Everything
# else in scripts/ runs build/turfc from the repository, where lib/std, the
# runtime and the front end are all one `..` away by accident; this builds the
# archive the release workflow publishes (scripts/package_release.sh), serves
# it from a local "release" directory, installs it with install.sh into a
# throwaway HOME, and then uses only what that install put on PATH.
#
# Proves, each against the real files:
#   * the archive has the documented layout, and a tag that disagrees with
#     VERSION is refused;
#   * install.sh verifies the SHA-256 (a corrupted archive installs nothing);
#   * a NEW shell resolves `turf` through the PATH block, the block is added
#     once however often you install, and the rest of the start-up file is kept;
#   * `turf new hello`, `turf run`, `turf build` with a path dependency (the
#     bundled examples) and `turf explain` work with TURF_HOME unset and the
#     checkout absent from PATH;
#   * uninstall puts the start-up file back byte for byte.
#
# What it does NOT prove: that nothing but the archive is needed. `cc` is still
# on PATH, because turfc still links through it and links LLVM dynamically
# (REL-008 stages 2-4 remove both; its clean-machine smoke, stage 8, is the
# test of that promise and does not exist yet). Windows and macOS runs happen
# only on their CI runners.
#
# Usage: scripts/release_smoke.sh [--archive FILE] [--keep]
#   --archive FILE  test this archive (with its .sha256 beside it) instead of
#                   packaging build/ now
#   --keep          leave the temporary directory for inspection
# Exit:  0 = all checks pass; 1 = a check failed.

set -u
cd "$(dirname "$0")/.." || exit 1
ROOT=$(pwd)

ARCHIVE=""
KEEP=0
while [ $# -gt 0 ]; do
  case "$1" in
    --archive) ARCHIVE="${2:?--archive needs a file}"; shift 2 ;;
    --keep) KEEP=1; shift ;;
    *) echo "release_smoke.sh: unknown option '$1'" >&2; exit 2 ;;
  esac
done

case "$(uname -s)/$(uname -m)" in
  Linux/x86_64) PLATFORM=linux-x86_64 ;;
  Darwin/arm64) PLATFORM=macos-arm64 ;;
  *) echo "SKIP (no install.sh archive for $(uname -s)/$(uname -m))"; exit 0 ;;
esac

WORK=$(mktemp -d)
if [ "$KEEP" = 1 ]; then echo "keeping $WORK"; else trap 'rm -rf "$WORK"' EXIT; fi

FAILED=0
pass() { echo "   ok   $1"; }
fail() { echo "   FAIL $1"; FAILED=$((FAILED + 1)); }
# BSD sed (macOS) has no \x1b escape, so the ESC byte is spelled out.
ESC=$(printf '\033')
strip() { sed "s/${ESC}\[[0-9;]*m//g"; }
# Which file install.sh edits for bash, and how a new bash reads it: macOS
# starts every bash as a login shell (~/.bash_profile), Linux as an
# interactive one (~/.bashrc).
if [ "$(uname -s)" = Darwin ]; then BASH_RC=.bash_profile; BASH_FLAGS=-li; else BASH_RC=.bashrc; BASH_FLAGS=-i; fi

# ---- the archive ------------------------------------------------------------
echo "== the archive"
VERSION=$(tr -d '[:space:]' < VERSION)
TAG="v$VERSION"
REL="$WORK/release/download/$TAG"
mkdir -p "$REL"
if [ -n "$ARCHIVE" ]; then
  cp "$ARCHIVE" "$ARCHIVE.sha256" "$REL/" || { echo "FAIL: cannot read $ARCHIVE and its .sha256"; exit 1; }
else
  [ -x build/turf ] && [ -x build/turfc ] || { echo "FAIL: build/turf and build/turfc are needed (./scripts/cmake_build.sh)"; exit 1; }
  ./scripts/package_release.sh --platform "$PLATFORM" --out "$REL" > "$WORK/package.out" 2> "$WORK/package.err" \
    || { echo "FAIL: packaging: $(cat "$WORK/package.err")"; exit 1; }
fi
ARCHIVE_FILE=$(cd "$REL" && ls turf-v*.tar.gz)
cat "$REL"/*.sha256 > "$REL/checksums.txt"
(cd "$REL" && { sha256sum -c "$ARCHIVE_FILE.sha256" >/dev/null 2>&1 || shasum -a 256 -c "$ARCHIVE_FILE.sha256" >/dev/null 2>&1; }) \
  && pass "the .sha256 file verifies the archive" || fail "the .sha256 file does not verify the archive"

LISTING=$(tar -tzf "$REL/$ARCHIVE_FILE")
TOP=$(printf '%s\n' "$LISTING" | head -n 1 | cut -d/ -f1)
for want in bin/turf bin/turfc lib/libturfrt.a lib/std/core/char.tr docs/errors/codes/E001.txt \
            docs/slm/syntax_card.txt share/turf/examples/plotter/turf.toml LICENSE README.md; do
  printf '%s\n' "$LISTING" | grep -qx "$TOP/$want" && pass "contains $want" || fail "archive lacks $want"
done
printf '%s\n' "$LISTING" | grep -q "libturfrt_asan" && fail "the sanitizer runtime is in the archive" || pass "no sanitizer runtime"
printf '%s\n' "$LISTING" | grep -qE "^$TOP/(turf|turfc)$" && fail "a binary sits at the archive top level (the pre-REL-003 layout)" || pass "binaries only in bin/"

# A tag that is not the VERSION must not be packaged.
if ./scripts/package_release.sh --platform "$PLATFORM" --out "$WORK/neg" --expect-tag v0.0.0-wrong >/dev/null 2> "$WORK/tag.err"; then
  fail "packaging accepted a tag that is not v<VERSION>"
elif grep -qE "development version|VERSION says" "$WORK/tag.err"; then
  pass "a tag that disagrees with VERSION is refused"
else
  fail "tag check failed for the wrong reason: $(cat "$WORK/tag.err")"
fi

# Package recipes are generated from the same checksums install.sh reads.
./scripts/gen_recipes.sh --checksums "$REL/checksums.txt" --version "$TAG" --out "$WORK/recipes" > "$WORK/recipes.out" 2>&1 \
  && grep -q "$(awk '{print $1}' "$REL/$ARCHIVE_FILE.sha256")" "$WORK/recipes"/homebrew/turf.rb \
  && pass "gen_recipes.sh writes a formula carrying the archive's SHA-256" \
  || fail "gen_recipes.sh: $(cat "$WORK/recipes.out")"

# ---- install ----------------------------------------------------------------
echo "== install.sh"
export TURF_INSTALL_BASE_URL="file://$WORK/release"
export TURF_INSTALL_PLATFORM="$PLATFORM"
HOME_DIR="$WORK/home"
mkdir -p "$HOME_DIR"
PREFIX="$HOME_DIR/.turf"
# Never the developer's own environment: not their TURF_HOME, their compiler
# override, their cache, or the shell configuration of the machine.
unset TURF_HOME TURF_COMPILER ZDOTDIR XDG_CONFIG_HOME
export TURF_CACHE_DIR="$WORK/cache"
mkdir -p "$TURF_CACHE_DIR" && echo keep > "$TURF_CACHE_DIR/marker"
run_installer() { HOME="$HOME_DIR" SHELL=/bin/bash sh "$ROOT/install.sh" "$@"; }

# A corrupted archive must install nothing.
BAD="$WORK/badrelease"
mkdir -p "$BAD/download/$TAG"
cp "$REL"/checksums.txt "$BAD/download/$TAG/"
head -c 100000 "$REL/$ARCHIVE_FILE" > "$BAD/download/$TAG/$ARCHIVE_FILE"
TURF_INSTALL_BASE_URL="file://$BAD" run_installer --version "$TAG" > "$WORK/bad.out" 2>&1
STATUS=$?
if [ "$STATUS" != 0 ] && grep -q "SHA-256 mismatch" "$WORK/bad.out" && [ ! -e "$PREFIX/bin" ] && [ ! -e "$HOME_DIR/$BASH_RC" ]; then
  pass "a corrupted archive is refused, and nothing is installed or edited"
else
  fail "corrupted archive: status $STATUS, $(tail -n 2 "$WORK/bad.out")"
fi

# The user's existing start-up file, with a blank line and no PATH of ours.
printf 'alias ll="ls -l"\n\nexport EDITOR=vi\n' > "$HOME_DIR/$BASH_RC"
cp "$HOME_DIR/$BASH_RC" "$WORK/bashrc.before"

run_installer --version "$TAG" > "$WORK/install1.out" 2>&1 || { fail "install failed: $(tail -n 5 "$WORK/install1.out")"; cat "$WORK/install1.out"; exit 1; }
grep -q "SHA-256 verified" "$WORK/install1.out" && pass "checksum verified before installing" || fail "no checksum verification line"
[ -x "$PREFIX/bin/turf" ] && [ -x "$PREFIX/bin/turfc" ] && [ -f "$PREFIX/lib/libturfrt.a" ] && pass "installed into ~/.turf" || fail "~/.turf is missing bin/lib files"
BLOCKS=$(grep -c '^# >>> turf >>>$' "$HOME_DIR/$BASH_RC")
[ "$BLOCKS" = 1 ] && pass "one PATH block in ~/$BASH_RC" || fail "$BLOCKS PATH blocks in ~/$BASH_RC"
head -n 3 "$HOME_DIR/$BASH_RC" | cmp -s - "$WORK/bashrc.before" && pass "the user's own lines are kept" || fail "the user's .bashrc lines changed"

run_installer --version "$TAG" > "$WORK/install2.out" 2>&1 || fail "a second install failed: $(tail -n 3 "$WORK/install2.out")"
BLOCKS=$(grep -c '^# >>> turf >>>$' "$HOME_DIR/$BASH_RC")
[ "$BLOCKS" = 1 ] && pass "installing again leaves one block" || fail "$BLOCKS PATH blocks after a second install"

# The latest-release lookup, by way of the directory install.sh builds it from.
mkdir -p "$WORK/release/latest"
ln -sfn "$REL" "$WORK/release/latest/download"
HOME="$WORK/h2" SHELL=/bin/bash sh "$ROOT/install.sh" --prefix "$WORK/h2/t" --no-modify-path > "$WORK/latest.out" 2>&1 \
  && [ -x "$WORK/h2/t/bin/turf" ] && [ ! -e "$WORK/h2/$BASH_RC" ] \
  && pass "default (latest) lookup, and --no-modify-path edits nothing" \
  || fail "latest/--no-modify-path install: $(tail -n 3 "$WORK/latest.out")"

# ---- a new shell finds turf, with nothing of the checkout in sight ---------------
echo "== a new shell"
# An interactive (macOS: login) bash reads its start-up file. The PATH is the system's only: no checkout,
# no ~/.local/bin (where dev_install.sh puts a link).
SYSPATH=/usr/local/bin:/usr/bin:/bin
FOUND=$(HOME="$HOME_DIR" PATH="$SYSPATH" SHELL=/bin/bash bash $BASH_FLAGS -c 'command -v turf' 2>/dev/null)
[ "$FOUND" = "$PREFIX/bin/turf" ] && pass "a new bash resolves turf to the installed one" || fail "a new bash resolved turf to '$FOUND'"
if command -v zsh >/dev/null 2>&1; then
  ZH="$WORK/zhome"; mkdir -p "$ZH"
  HOME="$ZH" SHELL=/bin/zsh sh "$ROOT/install.sh" --version "$TAG" > "$WORK/zsh.out" 2>&1
  FOUND=$(HOME="$ZH" PATH="$SYSPATH" ZDOTDIR="$ZH" zsh -i -c 'command -v turf' 2>/dev/null)
  [ "$FOUND" = "$ZH/.turf/bin/turf" ] && pass "a new zsh resolves turf (~/.zshrc)" || fail "a new zsh resolved turf to '$FOUND'"
fi

# ---- use it -----------------------------------------------------------------
echo "== turf, from the installed archive only"
USERENV() { env -i HOME="$HOME_DIR" PATH="$PREFIX/bin:$SYSPATH" TURF_CACHE_DIR="$WORK/cache" TERM=dumb "$@"; }

VER=$(USERENV turf version 2>&1 | strip)
printf '%s\n' "$VER" | grep -q "root $PREFIX\$" && pass "turfc's install root is the prefix" || fail "turf version: $VER"
printf '%s\n' "$VER" | grep -q "is not on your PATH\|not this one" && fail "turf version says it is not the one on PATH" || pass "turf version raises no PATH note"

mkdir "$WORK/proj" && cd "$WORK/proj" || exit 1
USERENV turf new hello > "$WORK/new.out" 2>&1 && pass "turf new hello" || fail "turf new: $(cat "$WORK/new.out")"
cd hello 2>/dev/null || { fail "turf new made no hello/"; exit 1; }
OUT=$(USERENV turf run 2>&1 | strip)
printf '%s\n' "$OUT" | grep -qi "hello" && pass "turf run prints hello" || fail "turf run: $OUT"
USERENV turf build > "$WORK/build.out" 2>&1 && [ -x build/hello ] && pass "turf build leaves build/hello" || fail "turf build: $(cat "$WORK/build.out")"

# A path dependency: the examples the archive ships, copied out the way a user
# would, since the install directory is not for building in.
cp -R "$PREFIX/share/turf/examples" "$WORK/examples"
cd "$WORK/examples/plotter" || exit 1
OUT=$(USERENV turf run 2>&1 | strip)
printf '%s\n' "$OUT" | grep -qF "distance squared: 25" && pass "a project with a path dependency (examples/plotter) runs" || fail "plotter: $OUT"

# An error explanation reads docs/ from the install root.
OUT=$(USERENV turf explain E001 2>&1 | strip)
[ "$(printf '%s\n' "$OUT" | wc -l)" -gt 3 ] && pass "turf explain reads the installed docs/errors" || fail "turf explain E001 was one line: $OUT"

# ---- uninstall --------------------------------------------------------------
echo "== uninstall"
cd "$WORK" || exit 1
run_installer --uninstall > "$WORK/uninstall.out" 2>&1 || fail "uninstall failed: $(cat "$WORK/uninstall.out")"
[ ! -e "$PREFIX" ] && pass "~/.turf is gone" || fail "~/.turf is still there: $(ls -A "$PREFIX")"
cmp -s "$HOME_DIR/$BASH_RC" "$WORK/bashrc.before" && pass "~/$BASH_RC is back byte for byte" || fail "~/$BASH_RC differs from before the install"
[ "$(cat "$WORK/cache/marker" 2>/dev/null)" = keep ] && pass "the package cache was not touched" || fail "the cache marker file vanished"
run_installer --uninstall > "$WORK/uninstall2.out" 2>&1 && fail "uninstalling twice did not complain" || pass "uninstalling when nothing is installed is an error, not a silent pass"

echo
if [ "$FAILED" -eq 0 ]; then
  echo "release_smoke: all checks passed"
  exit 0
fi
echo "release_smoke: $FAILED check(s) failed"
exit 1
