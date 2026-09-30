#!/bin/bash
#
# git_deps_smoke.sh -- LEC-001 stage 6: git dependencies, the fetch cache and
# turf.lock (docs/development/FREEZE.md 6.16, F-PKG-3: the root's turf.lock
# pins commit + SHA-256, verified on every build; one version per name;
# --offline, --locked; `turf update` is the only thing that moves a ref).
#
# The Go tests (cd cli && go test ./...) cover the tree hash, the lock format,
# the fetcher against real bare repositories and the graph rules in isolation.
# What only a real build shows is the whole path: `turf` -> system git -> the
# cache -> the lock -> the package graph -> `turfc`, on a program whose answer
# comes from the dependency, so a wrong tree would change the exit status.
# Every upstream here is a local bare repository built by this script, so there
# is no network. Every case checks the exit status and the message or lock
# line it is about, never just "it failed" (AGENTS.md: a failure for the wrong
# reason is not a pass).
#
# Usage: scripts/git_deps_smoke.sh   (after building build/turf and build/turfc)
# Exit:  0 = all checks pass, or build/turf is absent and CI is not set (Go is
#            optional for a local build: a SKIP, never a silent pass in CI);
#        1 = a check failed (each failure is printed).

set -u
cd "$(dirname "$0")/.." || exit 1
ROOT=$(pwd)
TURF="$ROOT/build/turf"
TURFC="$ROOT/build/turfc"

if [ ! -x "$TURF" ]; then
  if [ -n "${CI:-}" ]; then
    echo "FAIL: build/turf is missing in CI (was Go installed before the CMake build?)"
    exit 1
  fi
  echo "SKIP (no build/turf: Go not installed?)"
  exit 0
fi
[ -x "$TURFC" ] || { echo "FAIL: $TURFC not built"; exit 1; }
if ! command -v git > /dev/null 2>&1; then
  if [ -n "${CI:-}" ]; then
    echo "FAIL: git is not installed in CI"
    exit 1
  fi
  echo "SKIP (git is not installed)"
  exit 0
fi
# Which compiler and which stdlib run must follow from the binaries' own
# location, not from a developer's shell (TURF_HOME often points at another
# checkout).
unset TURF_HOME TURF_COMPILER

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
FAILED=0
fail() { echo "FAIL: $1"; FAILED=$((FAILED + 1)); }

# The cache is ours, never the developer's, and git must not read their
# configuration (signing, autocrlf, insteadOf) or need their identity.
CACHE=$WORK/cache
export TURF_CACHE_DIR=$CACHE
export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1 GIT_TERMINAL_PROMPT=0
export GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@t GIT_COMMITTER_NAME=t GIT_COMMITTER_EMAIL=t@t

t() { # t <name> <dir> <turf args...>: output and status under $WORK/_<name>.{out,rc}
  local n=$1 d=$2
  shift 2
  (cd "$d" && "$TURF" "$@") > "$WORK/_$n.out" 2>&1
  echo $? > "$WORK/_$n.rc"
}
strip() { sed 's/\x1b\[[0-9;]*m//g' "$1"; }
expect_rc() { [ "$(cat "$WORK/_$2.rc")" = "$3" ] || fail "$1: exit $(cat "$WORK/_$2.rc"), expected $3 (got: $(strip "$WORK/_$2.out" | head -c 400))"; }
expect_out() {
  strip "$WORK/_$2.out" | grep -qF -- "$3" ||
    fail "$1: output lacks '$3' (got: $(strip "$WORK/_$2.out" | head -c 500))"
}
expect_no_out() {
  if strip "$WORK/_$2.out" | grep -qF -- "$3"; then fail "$1: output must not contain '$3'"; fi
}
same() { cmp -s "$2" "$3" || fail "$1: $2 and $3 differ"; }
# lock_has <label> <app dir> <line>: turf.lock holds exactly this line.
lock_has() { grep -qxF -- "$3" "$2/turf.lock" || fail "$1: turf.lock lacks the line: $3 (has: $(head -c 700 "$2/turf.lock"))"; }
lock_lacks() { if grep -qxF -- "$3" "$2/turf.lock"; then fail "$1: turf.lock must not contain: $3"; fi; }

# --- upstreams: local bare repositories, pushed to from a scratch clone ----------
UP=$WORK/up
SC=$WORK/sc
mkdir -p "$UP" "$SC"
mkup() { # mkup <name>
  git init -q --bare -b main "$UP/$1.git"
  git clone -q "$UP/$1.git" "$SC/$1" 2> /dev/null
  git -C "$SC/$1" checkout -q -B main
}
# publish <name> <answer>: a library whose answer() returns <answer>, as a new
# commit on main. Extra files in the working tree are committed with it.
publish() {
  local n=$1
  mkdir -p "$SC/$n/src"
  printf '[package]\nname = "%s"\nversion = "1.0.0"\n' "$n" > "$SC/$n/turf.toml"
  printf 'pub fn answer(): int {\n    return %s;\n}\n' "$2" > "$SC/$n/src/$n.tr"
  git -C "$SC/$n" add -A
  git -C "$SC/$n" commit -q -m "answer $2"
  git -C "$SC/$n" push -q origin main
}
head_of() { git -C "$SC/$1" rev-parse HEAD; }
tagit() { # tagit <name> <tag>: create or force-move a tag, and push it forcibly
  git -C "$SC/$1" tag -f "$2" > /dev/null 2>&1
  git -C "$SC/$1" push -q --force origin "refs/tags/$2"
}
url() { echo "$UP/$1.git"; }

# mkapp <dir> <turf.toml [dependencies] lines> <modules imported and summed>:
# a program whose exit status is the sum of its dependencies' answers.
mkapp() {
  local dir=$1 deps=$2 mods=$3 imports="" sum="" m
  mkdir -p "$dir/src"
  printf '[package]\nname = "app"\nversion = "0.1.0"\n\n[dependencies]\n%s\n' "$deps" > "$dir/turf.toml"
  for m in $mods; do
    imports="${imports}import $m;\n"
    sum="${sum:+$sum + }$m::answer()"
  done
  printf "${imports}\nfn main(): int {\n    return %s;\n}\n" "$sum" > "$dir/src/main.tr"
}
# mklib <dir> <name> <deps lines> <expr>: a path library exposing v() = <expr>.
mklib() {
  local dir=$1 name=$2 imports="" m
  mkdir -p "$dir/src"
  printf '[package]\nname = "%s"\nversion = "0.1.0"\n\n[dependencies]\n%s\n' "$name" "$3" > "$dir/turf.toml"
  for m in $4; do imports="${imports}import $m;\n"; done
  printf "${imports}\npub fn v(): int {\n    return %s;\n}\n" "$5" > "$dir/src/$name.tr"
}
dep_git() { echo "$1 = { git = \"$(url "$2")\", $3 }"; } # dep_git <name> <repo> 'tag = "v1"'

# --- 1: a tag dependency builds, runs, and is pinned ------------------------------
mkup jsonx
publish jsonx 7
C1=$(head_of jsonx)
tagit jsonx v1
[ "${#C1}" = 40 ] || fail "setup: commit id is not 40 hex ($C1)"
A1=$WORK/app1
mkapp "$A1" "$(dep_git jsonx jsonx 'tag = "v1"')" jsonx
t run1 "$A1" run
expect_rc "a tag dependency through turf run" run1 7
expect_out "a tag dependency is fetched once" run1 "turf: fetching jsonx"
lock_has "tag dependency" "$A1" 'version = 1'
lock_has "tag dependency" "$A1" 'name = "jsonx"'
lock_has "tag dependency" "$A1" "source = \"git+$(url jsonx)\""
lock_has "tag dependency" "$A1" 'ref = "tag=v1"'
lock_has "tag dependency records the FULL commit" "$A1" "commit = \"$C1\""
grep -Eq '^hash = "sha256:[0-9a-f]{64}"$' "$A1/turf.lock" || fail "tag dependency: hash is not sha256:<64 hex>"
[ -f "$CACHE/src/jsonx-$C1/src/jsonx.tr" ] || fail "tag dependency: the tree is not at cache/src/jsonx-<commit>"
[ -z "$(find "$CACHE/src" -name .git)" ] || fail "an extracted tree must never contain .git"
[ "$(ls "$CACHE/git" | wc -l)" = 1 ] || fail "one URL should have exactly one mirror"
grep -q "$CACHE/src/jsonx-$C1/src" "$A1/build/package-graph.json" || fail "the package graph should point at the extracted tree"

# --- 16: building twice leaves turf.lock byte-identical ---------------------------
cp "$A1/turf.lock" "$WORK/lock1"
t build1b "$A1" build
expect_rc "a second build" build1b 0
expect_no_out "a second build needs no fetch" build1b "turf: fetching"
same "building twice: turf.lock is byte-identical" "$WORK/lock1" "$A1/turf.lock"
t locked_ok "$A1" build --locked
expect_rc "--locked with a correct lock" locked_ok 0
same "--locked writes nothing" "$WORK/lock1" "$A1/turf.lock"

# --- 2: the upstream is gone; the cache and the lock are enough -------------------
mv "$UP/jsonx.git" "$UP/jsonx.git.away"
t build_away "$A1" build
expect_rc "build with the upstream gone (cache)" build_away 0
t build_away_off "$A1" build --offline
expect_rc "build --offline with the upstream gone" build_away_off 0
t run_away "$A1" run --offline
expect_rc "run --offline" run_away 7
mv "$UP/jsonx.git.away" "$UP/jsonx.git"

# --- hash: content counts, mtimes and permission bits do not ----------------------
find "$CACHE/src/jsonx-$C1" -type f -exec touch -t 200001010000 {} +
chmod 755 "$CACHE/src/jsonx-$C1/turf.toml"
t build_touched "$A1" build
expect_rc "mtimes and permission bits do not change the tree hash" build_touched 0
chmod 644 "$CACHE/src/jsonx-$C1/turf.toml"

# --- 3: an edited cached file is detected on the next build -----------------------
printf '// tampered\n' >> "$CACHE/src/jsonx-$C1/src/jsonx.tr"
t build_tampered "$A1" build
expect_rc "an edited cached file" build_tampered 1
expect_out "an edited cached file" build_tampered "does not match turf.lock"
expect_out "an edited cached file names the cache path" build_tampered "$CACHE/src/jsonx-$C1"
expect_out "an edited cached file says how to recover" build_tampered "delete $CACHE/src/jsonx-$C1 and build again"
t build_tampered_off "$A1" build --offline
expect_rc "an edited cached file, --offline" build_tampered_off 1
expect_out "an edited cached file, --offline" build_tampered_off "does not match turf.lock"
t fetch_tampered "$A1" fetch
expect_rc "an edited cached file, turf fetch" fetch_tampered 1
# The advice in the message works, with no network.
rm -rf "$CACHE/src/jsonx-$C1"
t build_recovered "$A1" build --offline
expect_rc "deleting the tree re-extracts it from the mirror" build_recovered 0
# An added file counts as well as an edit.
: > "$CACHE/src/jsonx-$C1/extra.tr"
t build_extra "$A1" build
expect_rc "an extra file in the cached tree" build_extra 1
expect_out "an extra file in the cached tree" build_extra "does not match turf.lock"
rm -rf "$CACHE/src/jsonx-$C1"
t build_recovered2 "$A1" build
expect_rc "recovery after an extra file" build_recovered2 0

# --- 4: the manifest changed; --locked refuses and writes nothing -----------------
publish jsonx 8
C2=$(head_of jsonx)
tagit jsonx v2
cp "$A1/turf.lock" "$WORK/lock4"
sed 's/tag = "v1"/tag = "v2"/' "$A1/turf.toml" > "$A1/turf.toml.new" && mv "$A1/turf.toml.new" "$A1/turf.toml"
t locked_changed "$A1" build --locked
expect_rc "--locked with a changed manifest" locked_changed 1
expect_out "--locked names the changed entry" locked_changed "changed 'jsonx'"
expect_out "--locked names what changed" locked_changed "ref tag=v1 -> tag=v2"
expect_out "--locked says nothing was written" locked_changed "nothing was written"
same "--locked leaves turf.lock alone" "$WORK/lock4" "$A1/turf.lock"
t build_v2 "$A1" build
expect_rc "the same change without --locked" build_v2 0
lock_has "a changed manifest ref is re-resolved" "$A1" "commit = \"$C2\""
lock_has "a changed manifest ref is re-resolved" "$A1" 'ref = "tag=v2"'
t run_v2 "$A1" run
expect_rc "the new tag's code runs" run_v2 8

# --- 5: --locked needs a lock ------------------------------------------------------
A5=$WORK/app5
mkapp "$A5" "$(dep_git jsonx jsonx 'tag = "v1"')" jsonx
t locked_none "$A5" build --locked
expect_rc "--locked with no turf.lock" locked_none 1
expect_out "--locked with no turf.lock" locked_none "--locked needs turf.lock, and there is none"
[ ! -e "$A5/turf.lock" ] || fail "--locked must not create turf.lock"
t fetch_locked_none "$A5" fetch --locked
expect_rc "turf fetch --locked with no turf.lock" fetch_locked_none 1

# --- 6: a branch dependency: build keeps the commit, only update moves it ---------
mkup brnch
publish brnch 1
B1=$(head_of brnch)
AB=$WORK/appb
mkapp "$AB" "$(dep_git brnch brnch 'branch = "main"')" brnch
t run_b1 "$AB" run
expect_rc "a branch dependency" run_b1 1
lock_has "a branch dependency" "$AB" 'ref = "branch=main"'
lock_has "a branch dependency" "$AB" "commit = \"$B1\""
publish brnch 2
B2=$(head_of brnch)
t run_b1b "$AB" run
expect_rc "a new commit on the branch: build keeps the locked one" run_b1b 1
lock_has "build keeps the old branch commit" "$AB" "commit = \"$B1\""
expect_no_out "build does not touch the network for a locked branch" run_b1b "turf: fetching"
t update_b "$AB" update
expect_rc "turf update on a moved branch" update_b 0
expect_out "turf update prints old -> new" update_b "brnch ${B1:0:12} -> ${B2:0:12}"
lock_has "turf update writes the new commit" "$AB" "commit = \"$B2\""
t run_b2 "$AB" run
expect_rc "after update the new commit runs" run_b2 2
t update_b2 "$AB" update
expect_rc "update with nothing to move" update_b2 0
expect_out "update with nothing to move" update_b2 "brnch unchanged"
t update_b_named "$AB" update brnch
expect_out "update by name" update_b_named "brnch unchanged"

# --- 7: a force-moved tag: build keeps the commit, update moves it ----------------
mkup movtag
publish movtag 10
M1=$(head_of movtag)
tagit movtag v1
AM=$WORK/appm
mkapp "$AM" "$(dep_git movtag movtag 'tag = "v1"')" movtag
t run_m1 "$AM" run
expect_rc "a tag dependency (before the tag moves)" run_m1 10
publish movtag 11
M2=$(head_of movtag)
tagit movtag v1
t run_m1b "$AM" run
expect_rc "a force-moved tag: build keeps the locked commit" run_m1b 10
lock_has "a force-moved tag: build keeps the locked commit" "$AM" "commit = \"$M1\""
t update_m "$AM" update movtag
expect_out "update moves a force-moved tag" update_m "movtag ${M1:0:12} -> ${M2:0:12}"
t run_m2 "$AM" run
expect_rc "the moved tag's code runs after update" run_m2 11

# --- 8: an abbreviated rev is recorded in full ------------------------------------
mkup revrepo
publish revrepo 5
R1=$(head_of revrepo)
AR=$WORK/appr
mkapp "$AR" "$(dep_git revrepo revrepo "rev = \"${R1:0:7}\"")" revrepo
t run_r "$AR" run
expect_rc "an abbreviated rev" run_r 5
lock_has "an abbreviated rev: the lock records the full commit" "$AR" "commit = \"$R1\""
lock_has "an abbreviated rev: the ref is what the manifest asked" "$AR" "ref = \"rev=${R1:0:7}\""
publish revrepo 6
t update_r "$AR" update
expect_out "update of a rev changes nothing" update_r "revrepo unchanged"

# --- an annotated tag resolves to its commit, not the tag object -------------------
mkup annot
publish annot 4
N1=$(head_of annot)
git -C "$SC/annot" tag -a v1 -m "annotated" > /dev/null 2>&1
git -C "$SC/annot" push -q origin refs/tags/v1
AN=$WORK/appn
mkapp "$AN" "$(dep_git annot annot 'tag = "v1"')" annot
t run_n "$AN" run
expect_rc "an annotated tag" run_n 4
lock_has "an annotated tag: the lock holds the commit, not the tag object" "$AN" "commit = \"$N1\""

# --- a dependency's own turf.lock is ignored (F-PKG-3: the root's is the only one)
mkup lockrepo
printf 'this is not a lockfile\n' > "$SC/lockrepo/turf.lock"
publish lockrepo 9
tagit lockrepo v1
AL=$WORK/appl
mkapp "$AL" "$(dep_git lockrepo lockrepo 'tag = "v1"')" lockrepo
t run_l "$AL" run
expect_rc "a dependency with its own (garbage) turf.lock" run_l 9

# --- 9: a diamond on one URL and tag: one tree ------------------------------------
mkup shared
publish shared 3
S1=$(head_of shared)
tagit shared v1
D9=$WORK/d9
mklib "$D9/alpha" alpha "$(dep_git shared shared 'tag = "v1"')" shared "shared::answer()"
mklib "$D9/beta" beta "$(dep_git shared shared 'tag = "v1"')" shared "shared::answer() + 10"
mkdir -p "$D9/app/src"
printf '[package]\nname = "app"\nversion = "0.1.0"\n\n[dependencies]\nalpha = { path = "../alpha" }\nbeta = { path = "../beta" }\n' > "$D9/app/turf.toml"
printf 'import alpha;\nimport beta;\n\nfn main(): int {\n    return alpha::v() + beta::v();\n}\n' > "$D9/app/src/main.tr"
t run_d9 "$D9/app" run
expect_rc "a diamond on one URL and tag" run_d9 16
[ "$(ls -d "$CACHE"/src/shared-* | wc -l)" = 1 ] || fail "a diamond must extract one tree"
[ "$(grep -c '^name = "shared"$' "$D9/app/turf.lock")" = 1 ] || fail "a diamond must be one lock entry"
[ "$(strip "$WORK/_run_d9.out" | grep -c 'turf: fetching shared')" = 1 ] || fail "a diamond must fetch once"
lock_has "path packages are in the lock too" "$D9/app" 'source = "path+../alpha"'
lock_has "path packages are in the lock too" "$D9/app" 'source = "path+../beta"'

# --- 10: the same URL at two tags with different commits --------------------------
publish shared 4
S2=$(head_of shared)
tagit shared v2
D10=$WORK/d10
mklib "$D10/alpha" alpha "$(dep_git shared shared 'tag = "v1"')" shared "shared::answer()"
mklib "$D10/beta" beta "$(dep_git shared shared 'tag = "v2"')" shared "shared::answer()"
mkdir -p "$D10/app/src"
printf '[package]\nname = "app"\nversion = "0.1.0"\n\n[dependencies]\nalpha = { path = "../alpha" }\nbeta = { path = "../beta" }\n' > "$D10/app/turf.toml"
printf 'import alpha;\nimport beta;\n\nfn main(): int {\n    return alpha::v() + beta::v();\n}\n' > "$D10/app/src/main.tr"
t build_d10 "$D10/app" build
expect_rc "a diamond conflict on two tags" build_d10 1
expect_out "a diamond conflict" build_d10 "package 'shared' is required from two different directories"
expect_out "a diamond conflict names the first requirer" build_d10 "package 'alpha'"
expect_out "a diamond conflict names the second requirer" build_d10 "package 'beta'"
expect_out "a diamond conflict names the first ref" build_d10 "tag=v1"
expect_out "a diamond conflict names the second ref" build_d10 "tag=v2"
expect_out "a diamond conflict names both commits" build_d10 "commit ${S1:0:12}"
expect_out "a diamond conflict names both commits" build_d10 "commit ${S2:0:12}"
[ ! -e "$D10/app/turf.lock" ] || fail "a failed resolution must not write turf.lock"

# Two spellings of one commit are not a conflict.
D10b=$WORK/d10b
mklib "$D10b/alpha" alpha "$(dep_git shared shared 'tag = "v1"')" shared "shared::answer()"
mklib "$D10b/beta" beta "$(dep_git shared shared "rev = \"${S1:0:10}\"")" shared "shared::answer()"
mkdir -p "$D10b/app/src"
cp "$D10/app/turf.toml" "$D10b/app/turf.toml"
cp "$D10/app/src/main.tr" "$D10b/app/src/main.tr"
t run_d10b "$D10b/app" run
expect_rc "a tag and a rev naming one commit agree" run_d10b 6

# --- 11: one name as a path dependency and as a git dependency --------------------
D11=$WORK/d11
mklib "$D11/alpha" alpha "$(dep_git shared shared 'tag = "v1"')" shared "shared::answer()"
mklib "$D11/beta" beta 'shared = { path = "../shared" }' shared "shared::answer()"
mkdir -p "$D11/shared/src"
printf '[package]\nname = "shared"\nversion = "0.1.0"\n' > "$D11/shared/turf.toml"
printf 'pub fn answer(): int {\n    return 99;\n}\n' > "$D11/shared/src/shared.tr"
mkdir -p "$D11/app/src"
printf '[package]\nname = "app"\nversion = "0.1.0"\n\n[dependencies]\nalpha = { path = "../alpha" }\nbeta = { path = "../beta" }\n' > "$D11/app/turf.toml"
printf 'import alpha;\nimport beta;\n\nfn main(): int {\n    return alpha::v() + beta::v();\n}\n' > "$D11/app/src/main.tr"
t build_d11 "$D11/app" build
expect_rc "one name as a git and a path dependency" build_d11 1
expect_out "one name as a git and a path dependency" build_d11 "package 'shared' is required from two different directories"
expect_out "one name as a git and a path dependency names the git source" build_d11 "git $(url shared) (tag=v1"
expect_out "one name as a git and a path dependency names the directory" build_d11 "$D11/shared"

# --- 12: --offline with an empty cache --------------------------------------------
TURF_CACHE_DIR=$WORK/emptycache t off_empty2 "$A1" build --offline
expect_rc "--offline with an empty cache" off_empty2 1
expect_out "--offline with an empty cache" off_empty2 "needs the network; run without --offline"
expect_out "--offline names the dependency" off_empty2 "'jsonx'"
[ ! -d "$WORK/emptycache/git" ] || fail "--offline must not clone anything"
TURF_CACHE_DIR=$WORK/emptycache t off_empty3 "$A1" fetch --offline
expect_rc "turf fetch --offline with an empty cache" off_empty3 1
expect_out "turf fetch --offline with an empty cache" off_empty3 "needs the network; run without --offline"
# A dependency that is not locked is refused offline even though the cache holds a mirror of its URL.
A12=$WORK/app12
mkapp "$A12" "$(dep_git jsonx jsonx 'branch = "main"')" jsonx
t off_unlocked "$A12" build --offline
expect_rc "--offline with a dependency that is not locked" off_unlocked 1
expect_out "--offline with a dependency that is not locked" off_unlocked "is not in turf.lock"
expect_out "--offline with a dependency that is not locked" off_unlocked "needs the network; run without --offline"
[ ! -e "$A12/turf.lock" ] || fail "a refused --offline build must not write turf.lock"

# --- 13: git not on PATH ----------------------------------------------------------
NOGIT=$WORK/nogit-bin
mkdir -p "$NOGIT"
for tool in cc gcc ld as; do
  p=$(command -v "$tool" 2> /dev/null) && ln -sf "$p" "$NOGIT/$tool"
done
PATH=$NOGIT t nogit "$A1" build
expect_rc "git not on PATH" nogit 1
expect_out "git not on PATH" nogit "git dependencies need the 'git' program, which is not on PATH ('jsonx' in "
expect_out "git not on PATH names the manifest" nogit "$A1/turf.toml"
# A project with no git dependency does not care.
A13=$WORK/app13
mkdir -p "$A13/src"
printf '[package]\nname = "app"\nversion = "0.1.0"\n' > "$A13/turf.toml"
printf 'fn main(): int {\n    return 3;\n}\n' > "$A13/src/main.tr"
PATH=$NOGIT t nogit_pathonly "$A13" run
expect_rc "a project without git dependencies needs no git" nogit_pathonly 3

# --- 14: a git package's path dependencies must stay inside its repository --------
mkup mono
mkdir -p "$SC/mono/src"
publish mono 1
printf '[package]\nname = "mono"\nversion = "1.0.0"\n\n[dependencies]\nsib = { path = "../sib" }\n' > "$SC/mono/turf.toml"
git -C "$SC/mono" add -A
git -C "$SC/mono" commit -q -m "depends outside"
git -C "$SC/mono" push -q origin main
tagit mono v1
AE=$WORK/appe
mkapp "$AE" "$(dep_git mono mono 'tag = "v1"')" mono
t build_escape "$AE" build
expect_rc "a git package's path dependency that escapes its repository" build_escape 1
expect_out "a git package's path dependency that escapes its repository" build_escape "a git dependency's path dependencies must stay inside its repository"
expect_out "the escape names the dependency" build_escape "dependency 'sib'"
[ ! -e "$AE/turf.lock" ] || fail "a failed resolution must not write turf.lock"
# ...and one that stays inside works, and is covered by its parent's tree hash.
mkup mono2
mkdir -p "$SC/mono2/packages/inner/src"
printf '[package]\nname = "inner"\nversion = "1.0.0"\n' > "$SC/mono2/packages/inner/turf.toml"
printf 'pub fn answer(): int {\n    return 12;\n}\n' > "$SC/mono2/packages/inner/src/inner.tr"
mkdir -p "$SC/mono2/src"
printf '[package]\nname = "mono2"\nversion = "1.0.0"\n\n[dependencies]\ninner = { path = "packages/inner" }\n' > "$SC/mono2/turf.toml"
printf 'import inner;\n\npub fn answer(): int {\n    return inner::answer();\n}\n' > "$SC/mono2/src/mono2.tr"
git -C "$SC/mono2" add -A
git -C "$SC/mono2" commit -q -m "monorepo"
git -C "$SC/mono2" push -q origin main
tagit mono2 v1
AI=$WORK/appi
mkapp "$AI" "$(dep_git mono2 mono2 'tag = "v1"')" mono2
t run_inner "$AI" run
expect_rc "a path dependency inside a git repository" run_inner 12
lock_has "the git package is in the lock" "$AI" 'name = "mono2"'
lock_lacks "a path dependency inside a git tree has no lock entry of its own" "$AI" 'name = "inner"'

# --- 15: turf add --git ------------------------------------------------------------
mkup addrepo
publish addrepo 21
tagit addrepo v1
AA=$WORK/appa
mkapp "$AA" "" ""
printf 'fn main(): int {\n    return 0;\n}\n' > "$AA/src/main.tr"
cp "$AA/turf.toml" "$WORK/toml_a0"
t add_noref "$AA" add addrepo --git "$(url addrepo)"
expect_rc "turf add --git with no ref" add_noref 2
expect_out "turf add --git with no ref" add_noref "needs exactly one of --tag, --branch or --rev"
same "a refused add writes nothing" "$WORK/toml_a0" "$AA/turf.toml"
t add_tworefs "$AA" add addrepo --git "$(url addrepo)" --tag v1 --branch main
expect_rc "turf add --git with two refs" add_tworefs 2
t add_refnogit "$AA" add addrepo --tag v1
expect_rc "turf add --tag without --git" add_refnogit 2
expect_out "turf add --tag without --git" add_refnogit "only apply with --git"
t add_offline "$AA" add addrepo --git "$(url addrepo)" --tag v1 --offline
expect_rc "turf add --git --offline" add_offline 2
t add_badtag "$AA" add addrepo --git "$(url addrepo)" --tag nope
expect_rc "turf add --git of a tag that does not exist" add_badtag 1
expect_out "turf add --git of a tag that does not exist" add_badtag "has no tag named 'nope'"
same "a failed add writes nothing" "$WORK/toml_a0" "$AA/turf.toml"
t add_wrongname "$AA" add other --git "$(url addrepo)" --tag v1
expect_rc "turf add --git with a key that is not the package's name" add_wrongname 1
expect_out "turf add --git with a key that is not the package's name" add_wrongname "the key must be its package name"
same "a refused add writes nothing (wrong name)" "$WORK/toml_a0" "$AA/turf.toml"
t add_ok "$AA" add addrepo --git "$(url addrepo)" --tag v1
expect_rc "turf add --git --tag" add_ok 0
grep -qxF "addrepo = { git = \"$(url addrepo)\", tag = \"v1\" }" "$AA/turf.toml" || fail "turf add --git: the dependency line was not written"
[ "$(($(wc -l < "$AA/turf.toml") - $(wc -l < "$WORK/toml_a0")))" = 1 ] || fail "turf add --git changed more than one line"
printf 'import addrepo;\n\nfn main(): int {\n    return addrepo::answer();\n}\n' > "$AA/src/main.tr"
t run_added "$AA" run
expect_rc "the build works after turf add --git" run_added 21
lock_has "the build after add pins the dependency" "$AA" "commit = \"$(head_of addrepo)\""
# A repository that is not a package is refused before anything is written.
mkdir -p "$SC/plain"
mkup plain
printf 'hello\n' > "$SC/plain/README"
git -C "$SC/plain" add -A
git -C "$SC/plain" commit -q -m "no manifest"
git -C "$SC/plain" push -q origin main
cp "$AA/turf.toml" "$WORK/toml_a1"
t add_plain "$AA" add plain --git "$(url plain)" --branch main
expect_rc "turf add --git of a repository with no turf.toml" add_plain 1
expect_out "turf add --git of a repository with no turf.toml" add_plain "has no turf.toml at its root"
same "a refused add writes nothing (not a package)" "$WORK/toml_a1" "$AA/turf.toml"

# --- 17: removing a dependency drops its lock entry --------------------------------
AD=$WORK/appd
mkapp "$AD" "$(dep_git jsonx jsonx 'tag = "v1"')
$(dep_git brnch brnch 'branch = "main"')" "jsonx brnch"
t build_d "$AD" build
expect_rc "two git dependencies" build_d 0
lock_has "two git dependencies" "$AD" 'name = "brnch"'
lock_has "two git dependencies" "$AD" 'name = "jsonx"'
t remove_d "$AD" remove brnch
expect_rc "turf remove of a git dependency" remove_d 0
printf 'import jsonx;\n\nfn main(): int {\n    return jsonx::answer();\n}\n' > "$AD/src/main.tr"
t build_d2 "$AD" build
expect_rc "build after removing a dependency" build_d2 0
lock_lacks "removing a dependency drops its lock entry" "$AD" 'name = "brnch"'
lock_has "the other dependency stays" "$AD" 'name = "jsonx"'

# --- 18: turf fetch ----------------------------------------------------------------
AF=$WORK/appf
mkapp "$AF" "$(dep_git jsonx jsonx 'tag = "v1"')" jsonx
TURF_COMPILER=/nonexistent/turfc t fetch_app "$AF" fetch
expect_rc "turf fetch (it needs no compiler)" fetch_app 0
expect_out "turf fetch prints one line per git dependency" fetch_app "jsonx ${C1:0:12} (tag=v1)"
lock_has "turf fetch writes the lock" "$AF" "commit = \"$C1\""
[ ! -e "$AF/build" ] || fail "turf fetch must compile nothing (no build/ directory)"
t fetch_locked "$AF" fetch --locked
expect_rc "turf fetch --locked with a correct lock" fetch_locked 0
t fetch_args "$AF" fetch jsonx
expect_rc "turf fetch takes no names" fetch_args 2
t build_af "$AF" build
expect_rc "build after fetch" build_af 0
[ -x "$AF/build/app" ] || fail "build after fetch: no executable"

# --- a project with no git dependency still gets a lock ------------------------------
A19=$WORK/app19
mkdir -p "$A19"
t new19 "$A19" new plainapp
t build19 "$A19/plainapp" build
expect_rc "a path-only project builds" build19 0
lock_has "a path-only project's lock" "$A19/plainapp" 'version = 1'
[ "$(grep -c '^\[\[package\]\]' "$A19/plainapp/turf.lock")" = 0 ] || fail "a project with no dependencies has an empty lock"
lock_has "the lock says it is generated" "$A19/plainapp" '# This file is generated by turf. Commit it; do not edit it.'

# --- unknown ref, malformed lock, update's refusals --------------------------------
AU=$WORK/appu
mkapp "$AU" "$(dep_git jsonx jsonx 'tag = "nope"')" jsonx
t build_unknown "$AU" build
expect_rc "a tag that does not exist" build_unknown 1
expect_out "a tag that does not exist names the URL" build_unknown "$(url jsonx) has no tag named 'nope'"
expect_out "a tag that does not exist names the dependency" build_unknown "dependency 'jsonx'"
AV=$WORK/appv
mkapp "$AV" "$(dep_git jsonx jsonx 'tag = "v1"')" jsonx
printf 'version = 9\n' > "$AV/turf.lock"
t build_badlock "$AV" build
expect_rc "a lock with an unknown version" build_badlock 1
expect_out "a lock with an unknown version names the file" build_badlock "turf.lock"
expect_out "a lock with an unknown version" build_badlock "version 9 is not supported"
expect_out "a lock with an unknown version says how to fix it" build_badlock "delete it and run \`turf build\` to regenerate it"
printf 'version = 1\nfrobnicate = true\n' > "$AV/turf.lock"
t build_badlock2 "$AV" build
expect_rc "a lock with an unknown key" build_badlock2 1
expect_out "a lock with an unknown key" build_badlock2 "unknown key \`frobnicate\`"
rm -f "$AV/turf.lock"

t update_path "$D9/app" update alpha
expect_rc "turf update of a path dependency" update_path 1
expect_out "turf update of a path dependency" update_path "'alpha' is a path dependency; it is always used as it is on disk"
t update_unknown "$AB" update nosuchpkg
expect_rc "turf update of an unknown name" update_unknown 1
expect_out "turf update of an unknown name" update_unknown "'nosuchpkg' is not a package in this project's dependency graph"
t update_offline "$AB" update --offline
expect_rc "turf update --offline" update_offline 2
t update_none "$A19/plainapp" update
expect_rc "turf update in a project with no git dependencies" update_none 0
expect_out "turf update in a project with no git dependencies" update_none "no git dependencies to update"

if [ "$FAILED" -ne 0 ]; then
  echo "git_deps_smoke: $FAILED check(s) failed"
  exit 1
fi
echo "git_deps_smoke: all checks passed"
