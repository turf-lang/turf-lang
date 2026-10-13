#!/bin/bash
#
# cli_smoke.sh -- LEC-001 stage 2: the `turf` front end (cli/) drives `turfc`
# without changing anything about it.
#
# The Go unit tests (cd cli && go test ./...) cover argv classification,
# discovery order and the handshake's parsing in isolation. What only a real
# build can show is the end-to-end contract of docs/development/FREEZE.md 6.16
# (F-PKG-1): `turf <args>` is `turfc <args>` byte for byte (stdout, stderr and
# exit status), every compiler mode is reachable as a subcommand, the
# driver-interface handshake refuses a compiler that does not fit (and accepts
# one whose release version merely differs), and the CLI writes no file the
# compiler would not. Each case checks the exact exit code, and byte-identity
# against running turfc directly wherever the spec says so.
#
# Usage: scripts/smoke/cli_smoke.sh       (after building build/turf and build/turfc)
# Exit:  0 = all checks pass, or build/turf is absent and CI is not set (Go is
#            optional for a local build, so it is a SKIP, never a silent pass in
#            CI); 1 = a check failed (each failure is printed).

set -u
cd "$(dirname "$0")/../.." || exit 1
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

# The cases below are about discovery and about which compiler runs, so an
# ambient override or install root (a developer's shell often has TURF_HOME
# pointing at another checkout) must not decide the result.
unset TURF_COMPILER TURF_HOME

# The two release versions are independent (FREEZE.md 6.16, F-PKG-1): the
# compiler's is the root VERSION file, turf's is a constant in its own source.
V=$(head -1 VERSION)
CLI_V=$(sed -n 's/^const Version = "\(.*\)"$/\1/p' cli/internal/version/version.go)
[ -n "$CLI_V" ] || { echo "FAIL: cannot read turf's version from cli/internal/version/version.go"; exit 1; }
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/tmp"

FAILED=0
fail() { echo "FAIL: $1"; FAILED=$((FAILED + 1)); }
strip() { sed 's/\x1b\[[0-9;]*m//g'; }

# capture <name> <cmd...>: run it, keeping stdout, stderr and the exit status
# under $WORK/<name>.{out,err,rc}.
capture() {
  local n=$1
  shift
  "$@" > "$WORK/$n.out" 2> "$WORK/$n.err"
  echo $? > "$WORK/$n.rc"
}
# capture_in <dir> <name> <cmd...>: the same, run from inside <dir>.
capture_in() {
  local d=$1
  shift
  (cd "$d" && capture "$@")
}
rc_of() { cat "$WORK/$1.rc"; }

# identical <label> <a> <b>: two captures agree on stdout, stderr and status.
identical() {
  local label=$1 a=$2 b=$3
  cmp -s "$WORK/$a.out" "$WORK/$b.out" || fail "$label: stdout differs"
  cmp -s "$WORK/$a.err" "$WORK/$b.err" || fail "$label: stderr differs"
  [ "$(rc_of "$a")" = "$(rc_of "$b")" ] || fail "$label: exit status differs ($(rc_of "$a") vs $(rc_of "$b"))"
}

# expect_rc <label> <capture> <code>
expect_rc() {
  [ "$(rc_of "$2")" = "$3" ] || fail "$1: exit status $(rc_of "$2"), expected $3"
}

# expect_err <label> <capture> <text>
expect_err() {
  grep -qF -- "$3" "$WORK/$2.err" || fail "$1: stderr lacks '$3' (got: $(head -c 300 "$WORK/$2.err"))"
}

# Inputs. Written here rather than borrowed from tests/ so the script depends
# on nothing but the two binaries and the standard library.
cd "$WORK" || exit 1
cat > pass.tr <<'EOF'
import io;
fn main(): int {
    io::printline("hello from pass.tr");
    return 0;
}
EOF
cat > fail_x.tr <<'EOF'
import io;
fn main(): int {
    mut a: string = 10;
    return 0;
}
EOF
cat > ret3.tr <<'EOF'
import io;
fn main(): int {
    io::printline("hello from ret3.tr");
    return 3;
}
EOF
cat > m.tr <<'EOF'
fn helper(): int {
    return 1;
}
EOF

# A fresh directory holding only the given inputs, so "no file was created" is
# a plain listing comparison.
fresh() {
  local d
  d=$(mktemp -d "$WORK/case.XXXXXX")
  cp "$@" "$d/"
  echo "$d"
}
listing() { (cd "$1" && ls -A | sort | tr '\n' ' '); }

# --- passthrough: `turf t.tr -o a` is `turfc t.tr -o a` ----------------------
# Both run from the same directory with the same relative path (the fail_*
# diagnostics contain the path) and the SAME -o name, because a successful
# compile prints "Compiled to <name>": with different names the stdout could
# never be byte-identical, and that is not the difference under test. The
# second run simply overwrites the first's output.
for t in pass fail_x; do
  capture "pt_$t.turf"  "$TURF"  "$t.tr" -o "out_$t"
  capture "pt_$t.turfc" "$TURFC" "$t.tr" -o "out_$t"
  identical "passthrough $t.tr" "pt_$t.turf" "pt_$t.turfc"
done
expect_rc "passthrough pass.tr" pt_pass.turf 0
expect_rc "passthrough fail_x.tr" pt_fail_x.turf 1
[ -s "$WORK/pt_fail_x.turf.err" ] || fail "passthrough fail_x.tr: the failing case printed no diagnostics, so the comparison proves nothing"
grep -q 'Successfully compiled' "$WORK/pt_pass.turf.out" || fail "passthrough pass.tr: expected the compiler's success message on stdout"
[ -x out_pass ] || fail "passthrough pass.tr produced no executable"

# --- `turf compile` is the same thing spelled out ----------------------------
capture c1.turf  "$TURF"  compile pass.tr -o c1_out
capture c1.turfc "$TURFC" pass.tr -o c1_out
identical "compile pass.tr" c1.turf c1.turfc
capture c2.turf  "$TURF"  compile fail_x.tr -o c2_out
capture c2.turfc "$TURFC" fail_x.tr -o c2_out
identical "compile fail_x.tr" c2.turf c2.turfc

# An unknown flag is forwarded and rejected by turfc in turfc's own words: the
# proof that the CLI validates nothing.
capture o9.turf  "$TURF"  compile pass.tr -O9
capture o9.turfc "$TURFC" pass.tr -O9
identical "compile -O9" o9.turf o9.turfc
expect_rc "compile -O9" o9.turf 2
expect_err "compile -O9" o9.turf "unknown optimization level"

# --- check: every diagnostic, no file ----------------------------------------
d=$(fresh pass.tr fail_x.tr)
before=$(listing "$d")
capture_in "$d" ck_pass  "$TURF"  check pass.tr
capture_in "$d" ck_fail  "$TURF"  check fail_x.tr
capture_in "$d" ck_failc "$TURFC" --check fail_x.tr
expect_rc "check pass.tr" ck_pass 0
expect_rc "check fail_x.tr" ck_fail 1
identical "check fail_x.tr vs turfc --check" ck_fail ck_failc
[ "$(listing "$d")" = "$before" ] || fail "check created a file: before='$before' after='$(listing "$d")'"

# A file with no main is checked as a program by default -- the same verdict
# `turf compile` gives -- and as an importable module only with --module.
# Before this, `turf check` passed a file that `turf compile` rejects.
capture ck_nomain  "$TURF" check m.tr
expect_rc "check m.tr (no main)" ck_nomain 1
expect_err "check m.tr (no main)" ck_nomain "No entry point found"
expect_err "check m.tr (no main)" ck_nomain "--module"
capture ck_module  "$TURF" check --module m.tr
expect_rc "check --module m.tr" ck_module 0
capture ck_modprog "$TURF" check --module pass.tr
expect_rc "check --module pass.tr (a program is a fine module too)" ck_modprog 0
capture mod_nock   "$TURFC" --module pass.tr
expect_rc "turfc --module without --check" mod_nock 2
expect_err "turfc --module without --check" mod_nock "only applies with --check"

# --- dump: turfc --check --module --dump-<what>, no file ---------------------
d=$(fresh pass.tr)
before=$(listing "$d")
capture_in "$d" dd.turf  "$TURF"  dump ast pass.tr
capture_in "$d" dd.turfc "$TURFC" --check --module --dump-ast pass.tr
identical "dump ast" dd.turf dd.turfc
expect_rc "dump ast" dd.turf 0
[ -s "$WORK/dd.turf.out" ] || fail "dump ast printed nothing"
[ "$(listing "$d")" = "$before" ] || fail "dump created a file: '$(listing "$d")'"
# A dump is an inspection, not a verdict: a module with no main dumps cleanly.
capture dm.turf "$TURF" dump ast m.tr
expect_rc "dump ast m.tr (no main)" dm.turf 0
[ -s "$WORK/dm.turf.out" ] || fail "dump ast m.tr printed nothing"

capture db.turf "$TURF" dump bogus pass.tr
expect_rc "dump bogus" db.turf 2
expect_err "dump bogus" db.turf "instantiations"
capture dn.turf "$TURF" dump
expect_rc "dump (no <what>)" dn.turf 2

# --- explain ------------------------------------------------------------------
capture ex.turf  "$TURF"  explain E001
capture ex.turfc "$TURFC" --explain E001
identical "explain E001" ex.turf ex.turfc
expect_rc "explain E001" ex.turf 0
# This script runs from a temp directory, not the checkout, which is exactly
# where `turf explain` used to degrade to a one-line description: its docs
# were opened relative to the cwd. The full write-up (a per-code page, and a
# category page) must appear from anywhere.
grep -qF "A module name matches more than one file" <("$TURF" explain E090 | strip) ||
  fail "explain E090 outside the checkout: the per-code page was not found"
grep -qF "reach past the end of an array" <("$TURF" explain E001 | strip) ||
  fail "explain E001 outside the checkout: the category page was not found"
capture exn.turf "$TURF" explain
expect_rc "explain (no code)" exn.turf 2

# --- fmt ----------------------------------------------------------------------
capture f1.turf  "$TURF"  fmt pass.tr
capture f1.turfc "$TURFC" --format pass.tr
identical "fmt pass.tr" f1.turf f1.turfc
expect_rc "fmt pass.tr" f1.turf 0
capture f2.turf "$TURF" fmt pass.tr ret3.tr
expect_rc "fmt two files without --write" f2.turf 2

# --- migrate ------------------------------------------------------------------
capture mg.turf  "$TURF"  migrate pub m.tr
capture mg.turfc "$TURFC" --migrate=pub m.tr
identical "migrate pub" mg.turf mg.turfc
expect_rc "migrate pub" mg.turf 0
grep -q 'would migrate' "$WORK/mg.turf.out" || fail "migrate pub: report lacks 'would migrate'"
capture mgn.turf "$TURF" migrate
expect_rc "migrate (no name)" mgn.turf 2
expect_err "migrate (no name)" mgn.turf "pub"

# --- run: the program's status, its stdout, and the temp directory gone -------
TMPDIR="$WORK/tmp" capture r.turf "$TURF" run ret3.tr
expect_rc "run ret3.tr" r.turf 3
grep -q 'hello from ret3.tr' "$WORK/r.turf.out" || fail "run: the program's stdout did not pass through"
[ -z "$(ls -A "$WORK/tmp")" ] || fail "run left a temporary directory behind: $(ls -A "$WORK/tmp")"

# A failed compile: turfc's status, turfc's diagnostics, and still no leftovers.
TMPDIR="$WORK/tmp" capture rf.turf "$TURF" run fail_x.tr
capture rf.turfc "$TURFC" fail_x.tr -o "$WORK/rf_out"
expect_rc "run fail_x.tr" rf.turf "$(rc_of rf.turfc)"
[ -z "$(ls -A "$WORK/tmp")" ] || fail "a failed run left a temporary directory behind"

capture ro.turf "$TURF" run ret3.tr -o x
expect_rc "run -o" ro.turf 2
expect_err "run -o" ro.turf "temporary file"
capture rn.turf "$TURF" run
expect_rc "run (no file)" rn.turf 2
expect_err "run (no file)" rn.turf "not inside a TurfLang project"

# --- version and the handshake ------------------------------------------------
capture v.turf "$TURF" version
expect_rc "version" v.turf 0
head -1 "$WORK/v.turf.out" | grep -q "^turf $CLI_V " || fail "version: line 1 is '$(head -1 "$WORK/v.turf.out")', expected 'turf $CLI_V ...'"
grep -qF "turfc $V" "$WORK/v.turf.out" || fail "version: output lacks 'turfc $V'"
grep -qF "interface" "$WORK/v.turf.out" || fail "version: output does not show the driver interface"
capture v2.turf "$TURF" --version
identical "--version vs version" v.turf v2.turf
"$TURFC" --version | sed -n 2p | grep -qE '^interface [0-9]+\.[0-9]+$' || fail "turfc --version line 2 is not 'interface M.m'"

# --- REL-001: what --version reports, and the frozen exit statuses ------------
# Lines 3+ of `turfc --version` are `<key> <value>`: the LLVM it was built
# against, the runtime ABI stamp its programs demand, and its install root.
capture tv "$TURFC" --version
expect_rc "turfc --version" tv 0
grep -qE '^llvm [0-9]+\.[0-9]+' "$WORK/tv.out" || fail "turfc --version has no 'llvm <version>' line"
grep -qE '^root /' "$WORK/tv.out" || fail "turfc --version has no absolute 'root <path>' line"
ABI=$(sed -n 's/^runtime //p' "$WORK/tv.out")
[ -n "$ABI" ] || fail "turfc --version has no 'runtime <abi symbol>' line"
# The symbol is defined in C (runtime/turfrt_abi.c) and named in C++
# (RuntimeAbiSymbol), so only a check against the real archive keeps them
# from drifting apart.
if [ -f "$ROOT/lib/libturfrt.a" ] && command -v nm > /dev/null 2>&1; then
  nm "$ROOT/lib/libturfrt.a" 2> /dev/null | grep -qE " T _?$ABI\$" || fail "lib/libturfrt.a does not define the ABI symbol '$ABI' that turfc --version reports"
fi
# turf version shows turfc's extra lines too.
grep -qE '^  llvm ' "$WORK/v.turf.out" || fail "turf version does not show turfc's llvm line"
grep -qE '^  runtime ' "$WORK/v.turf.out" || fail "turf version does not show turfc's runtime line"
grep -qE '^  root ' "$WORK/v.turf.out" || fail "turf version does not show turfc's root line"

# `turfc help` is the spelling (no --help); both front ends say what the
# statuses mean.
capture th "$TURFC" help
expect_rc "turfc help" th 0
grep -q 'Usage: turfc' "$WORK/th.out" || fail "turfc help does not print the usage"
grep -q 'Exit status' "$WORK/th.out" || fail "turfc help does not document the exit statuses"
capture thf "$TURFC" --help
expect_rc "turfc --help" thf 2
expect_err "turfc --help" thf "turfc help"

# An unknown flag is a usage error (2), with a suggestion when one is near, and
# `turf` passes it through byte for byte. Nothing is compiled, so no file appears.
d=$(fresh pass.tr)
before=$(listing "$d")
capture_in "$d" uf.turfc "$TURFC" pass.tr --chek
capture_in "$d" uf.turf  "$TURF"  pass.tr --chek
identical "unknown flag --chek" uf.turf uf.turfc
expect_rc "unknown flag --chek" uf.turfc 2
expect_err "unknown flag --chek" uf.turfc "Unknown option: --chek"
expect_err "unknown flag --chek" uf.turfc "did you mean '--check'"
[ "$(listing "$d")" = "$before" ] || fail "an unknown flag still produced a file: before='$before' after='$(listing "$d")'"
capture uf2 "$TURFC" --verison
expect_rc "unknown flag --verison" uf2 2
expect_err "unknown flag --verison" uf2 "did you mean '--version'"
capture uf3 "$TURFC" pass.tr --targt=x86_64-linux-gnu
expect_rc "unknown flag --targt=" uf3 2
expect_err "unknown flag --targt=" uf3 "did you mean '--target='"
capture uf4 "$TURF" --bogus
expect_rc "turf --bogus" uf4 2
expect_err "turf --bogus" uf4 "Unknown option: --bogus"
# Nothing near: no guess is better than a wrong one.
capture uf5 "$TURFC" pass.tr --zzzzzzzz
expect_rc "unknown flag with no near match" uf5 2
grep -q 'did you mean' "$WORK/uf5.err" && fail "a far-off flag got a 'did you mean' guess"

# The other usage errors are 2 as well, and a missing value says so.
capture uv "$TURFC" pass.tr -o
expect_rc "-o without a value" uv 2
expect_err "-o without a value" uv "needs a value"
capture un1 "$TURFC"
expect_rc "turfc with no input" un1 2
capture un2 "$TURFC" notes.txt
expect_rc "turfc non-.tr input" un2 2
capture un3 "$TURFC" --explain
expect_rc "turfc --explain (no code)" un3 2
capture un4 "$TURFC" --explain E99999
expect_rc "turfc --explain unknown code" un4 2
capture un5 "$TURFC" --migrate=bogus pass.tr
expect_rc "turfc --migrate=bogus" un5 2
# ...while a program with a mistake, and a file that is not there, are 1.
capture er1 "$TURFC" --check fail_x.tr
expect_rc "compile error" er1 1
capture er2 "$TURFC" --check no_such_file.tr
expect_rc "missing input file" er2 1

# A different RELEASE version is fine when the interface fits: the pairing
# F-PKG-1's amendment exists for (TurfLang 2.0.0 driven by CLI 1.0.0). The fake
# forwards everything but --version to the real compiler, so the compile must
# actually succeed through it.
IFACE=$("$TURFC" --version | sed -n 2p)
cat > "$WORK/fake_other_release" <<EOF
#!/bin/sh
if [ "\$1" = "--version" ]; then printf 'turfc 9.9.9\n$IFACE\n'; exit 0; fi
exec "$TURFC" "\$@"
EOF
chmod +x "$WORK/fake_other_release"
d=$(fresh pass.tr)
TURF_COMPILER="$WORK/fake_other_release" capture_in "$d" or.turf "$TURF" check pass.tr
expect_rc "compiler with another release version, same interface" or.turf 0

# An interface this turf does not speak is refused, naming both sides.
printf '#!/bin/sh\nprintf "turfc 0.0.0\\ninterface 99.0\\n"\n' > "$WORK/fake_turfc"
chmod +x "$WORK/fake_turfc"
TURF_COMPILER="$WORK/fake_turfc" capture mm.turf "$TURF" pass.tr
expect_rc "incompatible compiler" mm.turf 1
expect_err "incompatible compiler" mm.turf "does not speak the interface"
expect_err "incompatible compiler" mm.turf "$CLI_V"
expect_err "incompatible compiler" mm.turf "0.0.0"
expect_err "incompatible compiler" mm.turf "interface 99.0"
expect_err "incompatible compiler" mm.turf "upgrade turf"

# A compiler from before the interface existed prints only line 1.
printf '#!/bin/sh\necho "turfc 0.0.0"\n' > "$WORK/fake_old_turfc"
chmod +x "$WORK/fake_old_turfc"
TURF_COMPILER="$WORK/fake_old_turfc" capture old.turf "$TURF" pass.tr
expect_rc "pre-interface compiler" old.turf 1
expect_err "pre-interface compiler" old.turf "reports no driver interface"

TURF_COMPILER=/nonexistent capture nc.turf "$TURF" pass.tr
expect_rc "TURF_COMPILER=/nonexistent" nc.turf 1
expect_err "TURF_COMPILER=/nonexistent" nc.turf "TURF_COMPILER"

# Discovery follows symlinks: a `turf` symlinked from elsewhere still finds the
# turfc beside the real binary (the installed layout, reached through ~/bin).
mkdir -p "$WORK/linkdir"
ln -s "$TURF" "$WORK/linkdir/turf"
capture sl.turf "$WORK/linkdir/turf" version
expect_rc "symlinked turf" sl.turf 0
grep -qF "turfc $V" "$WORK/sl.turf.out" || fail "symlinked turf did not find build/turfc"

# --- the command surface ------------------------------------------------------
capture ub.turf "$TURF" biuld
expect_rc "unknown command" ub.turf 2
expect_err "unknown command" ub.turf "did you mean 'build'"
capture ui.turf "$TURF" install
expect_rc "turf install" ui.turf 2
expect_err "turf install" ui.turf "turf fetch"
capture up.turf "$TURF" publish
expect_rc "turf publish" up.turf 1
expect_err "turf publish" up.turf "F-7c"
capture ubd.turf "$TURF" build
expect_rc "turf build" ubd.turf 2
expect_err "turf build" ubd.turf "not inside a TurfLang project"
capture un.turf "$TURF"
expect_rc "no arguments" un.turf 2
capture h.turf "$TURF" compile --help
expect_rc "compile --help" h.turf 0
grep -q -- '--check' "$WORK/h.turf.out" || fail "compile --help does not list --check"
capture h2.turf "$TURF" --help
expect_rc "--help" h2.turf 0
grep -q 'Project' "$WORK/h2.turf.out" || fail "--help does not list the project commands"
grep -q 'Exit status: 0 success, 1 .*2 the command line is wrong' <(tr '\n' ' ' < "$WORK/h2.turf.out" | sed 's/  */ /g') || fail "--help does not document the exit statuses"
capture bh.turf "$TURF" build --help
capture h3.turf "$TURF" help build
expect_rc "help build" h3.turf 0
identical "help build vs build --help" h3.turf bh.turf
capture h4.turf "$TURF" help biuld
expect_rc "help biuld" h4.turf 2

if [ $FAILED -ne 0 ]; then
  echo "cli_smoke: $FAILED check(s) failed"
  exit 1
fi
echo "cli_smoke: all checks passed"
