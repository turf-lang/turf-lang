#!/bin/bash
#
# fmt_smoke.sh -- REL-002 stage 2: `turf fmt` / `turfc --format`
# (src/tooling/Formatter.cpp, src/driver/main.cpp runFormat(), turf-cli's cmd/turf/cmd_fmt.go;
# docs/development/FREEZE.md 6.23).
#
# What is promised: the formatter moves whitespace and nothing else, the lexer
# reads the same tokens from the result, formatting twice changes nothing, and
# a file it cannot vouch for is left alone. tests/api/analysis_test.cpp proves
# the verifier refuses each kind of corrupt result; this proves the command that
# uses it: the three modes and their statuses, that one bad file does not stop
# the rest, that a file already formatted is not touched, and -- on the
# repository's own ~1200 files, the only input that is not made up to fit --
# that a second pass finds nothing left to do and that no result was refused as
# a formatter bug.
#
# Usage: scripts/smoke/fmt_smoke.sh  (after building build/turfc and build/turf)
# Exit:  0 = all checks pass (the `turf fmt` half is a SKIP, not a pass, when
#            build/turf is absent and CI is not set: Go is optional locally);
#        1 = a check failed (each failure is printed).

set -u
cd "$(dirname "$0")/../.." || exit 1
ROOT=$(pwd)
TURF="$ROOT/build/turf"
TURFC="$ROOT/build/turfc"

[ -x "$TURFC" ] || { echo "FAIL: $TURFC not built"; exit 1; }
unset TURF_HOME TURF_COMPILER

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
FAILED=0
fail() { echo "FAIL: $1"; FAILED=$((FAILED + 1)); }

strip() { sed 's/\x1b\[[0-9;]*m//g' "$1"; }
# c <name> <dir> <turfc args...> / t <name> <dir> <turf args...>: output and
# status are kept under $WORK/_<name>.{out,err,rc}; stdout and stderr apart,
# since --format's stdout IS the formatted text.
c() {
  local n=$1 d=$2
  shift 2
  (cd "$d" && "$TURFC" "$@") > "$WORK/_$n.out" 2> "$WORK/_$n.err" < /dev/null
  echo $? > "$WORK/_$n.rc"
}
t() {
  local n=$1 d=$2
  shift 2
  (cd "$d" && "$TURF" "$@") > "$WORK/_$n.out" 2> "$WORK/_$n.err" < /dev/null
  echo $? > "$WORK/_$n.rc"
}
expect_rc() { [ "$(cat "$WORK/_$2.rc")" = "$3" ] || fail "$1: exit $(cat "$WORK/_$2.rc"), expected $3 (stdout: $(head -c 300 "$WORK/_$2.out"); stderr: $(strip "$WORK/_$2.err" | head -c 300))"; }
expect_out() { grep -qF -- "$3" "$WORK/_$2.out" || fail "$1: stdout lacks '$3' (got: $(head -c 400 "$WORK/_$2.out"))"; }
expect_err() { strip "$WORK/_$2.err" | grep -qF -- "$3" || fail "$1: stderr lacks '$3' (got: $(strip "$WORK/_$2.err" | head -c 400))"; }
expect_no_out() { if grep -qF -- "$3" "$WORK/_$2.out"; then fail "$1: stdout must not contain '$3'"; fi; }
same() { cmp -s "$2" "$3" || fail "$1: $2 and $3 differ"; }

messy() {
  cat > "$1" <<'EOF'
fn main(): int {
        mut x: int = 1;
    if (x == 1) {
  x = 2;
    }


    return x;
}
EOF
}
clean() {
  cat > "$1" <<'EOF'
fn main(): int {
    mut x: int = 1;
    if (x == 1) {
        x = 2;
    }

    return x;
}
EOF
}

# --- turfc --format: the three modes ----------------------------------------------
D="$WORK/modes"
mkdir -p "$D"
messy "$D/messy.tr"
clean "$D/want.tr"
clean "$D/clean.tr"
c stdout "$D" --format messy.tr
expect_rc "--format prints the formatted text" stdout 0
same "--format prints the formatted text" "$WORK/_stdout.out" "$D/want.tr"
cp "$D/messy.tr" "$D/messy.orig"
same "--format without --write leaves the file alone" "$D/messy.tr" "$D/messy.orig"
# The output is a fixed point: formatting it again prints it unchanged.
cp "$WORK/_stdout.out" "$D/again.tr"
c stdout2 "$D" --format again.tr
same "formatting the output again changes nothing" "$WORK/_stdout2.out" "$D/again.tr"

c check_messy "$D" --format --check messy.tr clean.tr
expect_rc "--check names a file that would change" check_messy 1
expect_out "--check names a file that would change" check_messy "would reformat messy.tr"
expect_no_out "--check does not name a formatted file" check_messy "would reformat clean.tr"
expect_out "--check summarises" check_messy "Would reformat 1 file; 1 already formatted."
same "--check writes nothing" "$D/messy.tr" "$D/messy.orig"
c check_clean "$D" --format --check clean.tr
expect_rc "--check on a formatted file" check_clean 0
expect_out "--check on a formatted file" check_clean "Would reformat 0 files; 1 already formatted."

# --write rewrites what differs and does not touch the rest: a file that is
# already formatted keeps its modification time, so a build system sees no change.
touch -d '2001-01-01 00:00:00' "$D/clean.tr"
c write "$D" --format --write messy.tr clean.tr
expect_rc "--write" write 0
expect_out "--write summarises" write "Reformatted 1 file; 1 already formatted."
same "--write produces the formatted text" "$D/messy.tr" "$D/want.tr"
[ "$(stat -c %Y "$D/clean.tr" 2>/dev/null || stat -f %m "$D/clean.tr")" = "$(date -d '2001-01-01 00:00:00' +%s 2>/dev/null || date -j -f '%Y-%m-%d %H:%M:%S' '2001-01-01 00:00:00' +%s)" ] ||
  fail "--write touched a file that was already formatted"
c check_after_write "$D" --format --check messy.tr clean.tr
expect_rc "--check right after --write is clean" check_after_write 0

# --- usage errors (status 2): nothing is touched -----------------------------------
messy "$D/m2.tr"
cp "$D/m2.tr" "$D/m2.orig"
c both "$D" --format --write --check m2.tr
expect_rc "--write with --check" both 2
expect_err "--write with --check" both "not both"
c two "$D" --format m2.tr clean.tr
expect_rc "two files, no mode" two 2
expect_err "two files, no mode" two "exactly one file"
printf 'x\n' > "$D/notes.txt"
c notr "$D" --format --check notes.txt
expect_rc "a non-.tr file" notr 2
expect_err "a non-.tr file" notr "'.tr' extension"
c nofile "$D" --format --check
expect_rc "no file at all" nofile 2
same "a usage error touches nothing" "$D/m2.tr" "$D/m2.orig"

# --- one bad file does not stop the rest -------------------------------------------
B="$WORK/batch"
mkdir -p "$B"
messy "$B/a.tr"
printf 'fn main(): int {\n    return 3.14.15;\n}\n' > "$B/lexbad.tr"
printf 'fn main(): int {\n    mut s: string = "open;\n    return 0;\n}\n' > "$B/unterminated.tr"
messy "$B/z.tr"
cp "$B/lexbad.tr" "$B/lexbad.orig"
c batch "$B" --format --write a.tr lexbad.tr missing.tr unterminated.tr z.tr
expect_rc "a batch with bad files" batch 1
expect_err "a lexical error is reported, not a crash" batch "Could not format lexbad.tr: it has a lexical error (line 2:"
expect_err "an unterminated string is reported too" batch "Could not format unterminated.tr: it has a lexical error"
expect_err "a missing file is reported" batch "Could not open file missing.tr"
expect_out "the good files were still formatted" batch "Reformatted 2 files"
same "a, formatted despite the failure before it" "$B/a.tr" "$D/want.tr"
same "z, formatted despite the failures before it" "$B/z.tr" "$D/want.tr"
same "a file that cannot be formatted is left byte for byte" "$B/lexbad.tr" "$B/lexbad.orig"
expect_no_out "no compiler diagnostics are printed for a lexical error" batch "── error"
if strip "$WORK/_batch.err" | grep -q "── error"; then fail "a lexical error must not print the compiler's diagnostic, only the formatter's message"; fi
c single_bad "$B" --format lexbad.tr
expect_rc "--format on a file with a lexical error" single_bad 1
[ ! -s "$WORK/_single_bad.out" ] || fail "--format printed text for a file it could not format"

# --skip-unlexable (what `turf fmt` passes for files it found under a
# directory): a lexical error is named as skipped and is not a failure. Nothing
# else is: a missing file still fails, and a usage error stays one.
messy "$B/a.tr"
messy "$B/z.tr"
c skip "$B" --format --write --skip-unlexable a.tr lexbad.tr unterminated.tr z.tr
expect_rc "--skip-unlexable: lexical errors are not failures" skip 0
expect_out "--skip-unlexable names what it skipped" skip "skipped lexbad.tr: it has a lexical error (line 2:"
expect_out "--skip-unlexable names an unterminated string too" skip "skipped unterminated.tr: it has a lexical error"
expect_out "--skip-unlexable counts them" skip "Reformatted 2 files; 0 already formatted; 2 skipped (lexical error)."
same "--skip-unlexable still formats the rest" "$B/a.tr" "$D/want.tr"
same "--skip-unlexable leaves a skipped file byte for byte" "$B/lexbad.tr" "$B/lexbad.orig"
c skip_chk "$B" --format --check --skip-unlexable a.tr lexbad.tr
expect_rc "--check --skip-unlexable on formatted files" skip_chk 0
expect_out "--check --skip-unlexable" skip_chk "Would reformat 0 files; 1 already formatted; 1 skipped (lexical error)."
c skip_missing "$B" --format --check --skip-unlexable a.tr missing.tr
expect_rc "--skip-unlexable does not excuse a missing file" skip_missing 1
expect_err "--skip-unlexable does not excuse a missing file" skip_missing "Could not open file missing.tr"
c skip_nomode "$B" --format --skip-unlexable a.tr
expect_rc "--skip-unlexable without --write or --check" skip_nomode 2
expect_err "--skip-unlexable without --write or --check" skip_nomode "goes with --write or --check"

# --- the repository's own files -----------------------------------------------------
# Formatting is idempotent and never refused as a formatter bug across ~1200 real
# files. Run on a copy, since this repository's files are not rewritten here. The
# files that cannot be formatted are the tests that are invalid on purpose (a
# malformed number, an unterminated string); the count is the same both times.
CORPUS="$WORK/corpus"
mkdir -p "$CORPUS"
cp -r tests lib examples showcase "$CORPUS/"
rm -rf "$CORPUS/lib/usr"
(cd "$CORPUS" && find tests lib examples showcase -name '*.tr' | sort > files.txt)
N=$(wc -l < "$CORPUS/files.txt")
[ "$N" -gt 1000 ] || fail "corpus: expected the repository's .tr files, found $N"
# The files go to turfc in batches of 200: one command line holding ~1200 paths is
# over Windows' 32 KB limit (`turf fmt` batches the same way, cmd_fmt.go). Each
# batch prints its own "Would reformat N files; ..." summary, so the checks below
# read every summary line instead of one.
(cd "$CORPUS" && split -l 200 files.txt batch_)
# corpus_run <name> <turfc flags...>: every batch, stdout and stderr appended to
# $WORK/_<name>.out / .err, and <name>.rc is 1 if any batch exited non-zero.
corpus_run() {
  local name=$1 b rc=0
  shift
  : > "$WORK/_$name.out"; : > "$WORK/_$name.err"
  for b in "$CORPUS"/batch_*; do
    (cd "$CORPUS" && "$TURFC" --format "$@" $(cat "$b")) >> "$WORK/_$name.out" 2>> "$WORK/_$name.err" < /dev/null || rc=1
  done
  echo $rc > "$WORK/_$name.rc"
}
corpus_run cw --write
if strip "$WORK/_cw.err" | grep -q "formatter bug"; then
  fail "corpus: a result was refused as a formatter bug: $(strip "$WORK/_cw.err" | grep 'formatter bug' | head -2)"
fi
corpus_run cc --check
# Every batch must say it has nothing left to reformat.
if [ -z "$(grep '^Would reformat' "$WORK/_cc.out")" ] || grep '^Would reformat' "$WORK/_cc.out" | grep -qv '^Would reformat 0 files;'; then
  fail "corpus: a second pass still wants to reformat: $(grep -m3 'would reformat' "$WORK/_cc.out")"
fi
W1=$(strip "$WORK/_cw.err" | grep -c "Could not format")
W2=$(strip "$WORK/_cc.err" | grep -c "Could not format")
[ "$W1" = "$W2" ] || fail "corpus: $W1 files could not be formatted, then $W2"
# With --skip-unlexable the same set is skipped and the run is clean: that is the
# state `turf fmt --check` has to reach for CI. (--check on the already
# formatted copy; the number skipped is the number that could not be formatted.)
corpus_run cs --check --skip-unlexable
expect_rc "corpus: --check --skip-unlexable is clean" cs 0
SKIPPED=$(sed -n 's/.*; \([0-9][0-9]*\) skipped (lexical error)\.$/\1/p' "$WORK/_cs.out" | awk '{ n += $1 } END { print n + 0 }')
[ "$SKIPPED" = "$W1" ] || fail "corpus: expected $W1 skipped, got $SKIPPED"
# Everything that could not be formatted is a deliberately invalid test.
if strip "$WORK/_cw.err" | grep "Could not format" | grep -v "lexical error" | grep -q .; then
  fail "corpus: a file failed for a reason other than a lexical error: $(strip "$WORK/_cw.err" | grep 'Could not format' | grep -v 'lexical error' | head -2)"
fi
if strip "$WORK/_cw.err" | grep "Could not format" | grep -v "/fail_" | grep -q .; then
  fail "corpus: a file that is not a fail_ test has a lexical error: $(strip "$WORK/_cw.err" | grep 'Could not format' | grep -v '/fail_' | head -2)"
fi

# --- turf fmt: which files -------------------------------------------------------------
if [ ! -x "$TURF" ]; then
  if [ -n "${CI:-}" ]; then
    echo "FAIL: build/turf is missing in CI (was Go installed before the CMake build?)"
    exit 1
  fi
  echo "SKIP the turf fmt half (no build/turf: Go not installed?)"
else
  P="$WORK/proj"
  mkdir -p "$P"
  t new "$P" new app
  expect_rc "turf new app" new 0
  A="$P/app"
  messy "$A/src/main.tr"
  mkdir -p "$A/src/util" "$A/tests/known" "$A/build"
  messy "$A/src/util/text.tr"
  messy "$A/tests/basic.tr"
  messy "$A/tests/known/invalid.tr"
  messy "$A/build/generated.tr"
  clean "$A/tests/ok.tr"

  t proj_bare "$A" fmt
  expect_rc "turf fmt in a project with no mode" proj_bare 2
  expect_err "turf fmt in a project with no mode" proj_bare "say which: --write"
  t outside "$WORK" fmt --check
  expect_rc "turf fmt --check outside a project" outside 2
  expect_err "turf fmt --check outside a project" outside "not inside a TurfLang project"

  t proj_check "$A" fmt --check
  expect_rc "turf fmt --check over a project" proj_check 1
  expect_out "src/ is searched" proj_check "would reformat src/main.tr"
  expect_out "a nested module is found" proj_check "would reformat src/util/text.tr"
  expect_out "tests/ is searched" proj_check "would reformat tests/basic.tr"
  expect_no_out "a formatted file is not named" proj_check "would reformat tests/ok.tr"
  expect_no_out "build/ is not searched" proj_check "generated.tr"
  expect_out "the summary counts the files" proj_check "Would reformat 4 files; 1 already formatted."
  t proj_check_sub "$A/src/util" fmt --check
  expect_rc "turf fmt --check from a subdirectory finds the project" proj_check_sub 1
  expect_out "paths are spelled from the project root" proj_check_sub "would reformat src/main.tr"

  t proj_excl "$A" fmt --check --exclude tests/known
  expect_out "--exclude skips a directory" proj_excl "Would reformat 3 files; 1 already formatted."
  t proj_write "$A" fmt --write --exclude tests/known
  expect_rc "turf fmt --write over a project" proj_write 0
  same "src/main.tr is formatted" "$A/src/main.tr" "$D/want.tr"
  same "tests/basic.tr is formatted" "$A/tests/basic.tr" "$D/want.tr"
  same "an excluded file is untouched" "$A/tests/known/invalid.tr" "$D/messy.orig"
  t proj_after "$A" fmt --check --exclude tests/known
  expect_rc "turf fmt --check after --write" proj_after 0

  # A file the lexer rejects: skipped when found under a directory (a tree can
  # hold sources that are invalid on purpose), an error when named.
  printf 'fn main(): int {\n    return 3.14.15;\n}\n' > "$A/tests/lexbad.tr"
  cp "$A/tests/lexbad.tr" "$A/tests/lexbad.orig"
  t skip_dir "$A" fmt --check --exclude tests/known
  expect_rc "turf fmt --check skips an unlexable file found in a directory" skip_dir 0
  expect_out "the skip is named" skip_dir "skipped tests/lexbad.tr: it has a lexical error"
  expect_out "the skip is counted" skip_dir "1 skipped (lexical error)."
  t skip_write "$A" fmt --write --exclude tests/known
  expect_rc "turf fmt --write skips it too" skip_write 0
  same "a skipped file is left byte for byte" "$A/tests/lexbad.tr" "$A/tests/lexbad.orig"
  t skip_named "$A" fmt --check tests/lexbad.tr
  expect_rc "a named file with a lexical error is an error" skip_named 1
  expect_err "a named file with a lexical error is an error" skip_named "Could not format tests/lexbad.tr: it has a lexical error"
  t skip_both "$A" fmt --check --exclude tests/known tests tests/lexbad.tr
  expect_rc "named wins over found: the directory must not excuse it" skip_both 1
  expect_err "named wins over found" skip_both "Could not format tests/lexbad.tr"
  rm -f "$A/tests/lexbad.tr" "$A/tests/lexbad.orig"

  # Named paths: files, directories, a typo.
  t path_file "$A" fmt tests/ok.tr
  expect_rc "turf fmt <file> prints it" path_file 0
  same "turf fmt <file> prints it" "$WORK/_path_file.out" "$A/tests/ok.tr"
  t path_dir "$A" fmt --check tests
  expect_rc "turf fmt --check <dir>" path_dir 1
  expect_out "turf fmt --check <dir>" path_dir "would reformat tests/known/invalid.tr"
  t path_many "$A" fmt tests/ok.tr tests/basic.tr
  expect_rc "two files to stdout" path_many 2
  t path_dir_stdout "$A" fmt tests
  expect_rc "a directory to stdout" path_dir_stdout 2
  t path_typo "$A" fmt --check srcc
  expect_rc "a path that does not exist" path_typo 1
  expect_err "a path that does not exist" path_typo "srcc: no such file or directory"
  t path_none "$A" fmt --check tests/known --exclude tests/known
  expect_rc "everything excluded" path_none 1
  expect_err "everything excluded" path_none "no .tr files found"
  t flag_typo "$A" fmt --wirte
  expect_rc "an unknown flag" flag_typo 2
fi

if [ "$FAILED" -ne 0 ]; then
  echo "fmt_smoke: $FAILED check(s) failed"
  exit 1
fi
echo "fmt_smoke: all checks passed"
