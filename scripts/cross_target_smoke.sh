#!/bin/bash
#
# cross_target_smoke.sh -- REL-003 stage 2 (D20, docs/development/FREEZE.md
# 6.26): `--target=<triple>` for a machine other than the host still emits a
# real object file, and linking it stops with a TurfLang diagnostic instead of
# the host linker's "file in wrong format". `test_runner.sh` cannot see any of
# this: it never passes `--target`, and it only reads the exit status.
#
# Every case checks the exit status AND the text it is about, never just
# "it failed" (AGENTS.md: a failure for the wrong reason is not a pass). The
# cases that must NOT trip the diagnostic matter as much as the ones that do:
# a spelling of the host triple that differs only in its vendor field has to
# keep linking, or the check refuses native builds.
#
# Usage: scripts/cross_target_smoke.sh     (after building build/turfc)
# Exit:  0 = all checks pass; 1 = a check failed (each failure is printed).

set -u
cd "$(dirname "$0")/.." || exit 1
ROOT=$(pwd)
TURFC="${TURFC:-$ROOT/build/turfc}"
[ -x "$TURFC" ] || { echo "FAIL: $TURFC not built"; exit 1; }

# The build tree's own lib/ and lib/std, never a developer's TURF_HOME.
unset TURF_HOME

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cd "$WORK" || exit 1

FAILED=0
pass() { echo "   ok   $1"; }
fail() { echo "   FAIL $1"; FAILED=$((FAILED + 1)); }

cat > hello.tr <<'EOF'
fn main(): int {
    printline("hello from a cross target smoke");
    return 0;
}
EOF

# ELF e_machine (offset 18, little endian): 183 = AArch64, 62 = x86-64.
elf_machine() {
  od -An -tu2 -j18 -N2 "$1" | tr -d ' '
}

echo "== a foreign target gets a TurfLang diagnostic and keeps its object"
rm -f arm arm.o
"$TURFC" hello.tr --target=aarch64-unknown-linux-gnu -o arm > arm.out 2> arm.err
STATUS=$?
if [ "$STATUS" = 1 ]; then pass "exit status 1"; else fail "exit status was $STATUS, want 1"; fi
if grep -q "cross-linking is not supported in 2.0" arm.err; then
  pass "names the limitation"
else
  fail "no 'cross-linking is not supported in 2.0' in: $(cat arm.err)"
fi
if grep -q "aarch64-unknown-linux-gnu" arm.err && grep -q "libturfrt" arm.err; then
  pass "names the target and the runtime"
else
  fail "diagnostic does not name the target and libturfrt: $(cat arm.err)"
fi
if grep -qE "Linking failed|/usr/bin/ld|collect2|wrong format" arm.err; then
  fail "the host linker was still run: $(cat arm.err)"
else
  pass "the host linker was never run"
fi
if [ -f arm.o ]; then
  pass "object file kept"
  if [ "$(elf_machine arm.o)" = 183 ]; then pass "object is real AArch64 code"; else fail "arm.o e_machine is $(elf_machine arm.o), want 183"; fi
else
  fail "arm.o was deleted"
fi
[ ! -e arm ] && pass "no executable was produced" || fail "an executable 'arm' exists"

echo "== a spelling of the host that differs only in vendor still links and runs"
HOST=$(cc -dumpmachine 2>/dev/null)
case "$HOST" in
  *linux-gnu*)
    # arch-linux-gnu or arch-vendor-linux-gnu, whichever this cc prints.
    ARCH=${HOST%%-*}
    for spelling in "$HOST" "$ARCH-linux-gnu" "$ARCH-unknown-linux-gnu" "$ARCH-pc-linux-gnu"; do
      rm -f native native.o
      if "$TURFC" hello.tr --target="$spelling" -o native > native.out 2> native.err && ./native | grep -q "hello from a cross target smoke"; then
        pass "--target=$spelling"
      else
        fail "--target=$spelling should be the host: $(cat native.err)"
      fi
    done

    # Same CPU and OS, different libc: libturfrt is built against the host's.
    MUSL="$ARCH-linux-musl"
    rm -f musl musl.o
    "$TURFC" hello.tr --target="$MUSL" -o musl > musl.out 2> musl.err
    STATUS=$?
    if [ "$STATUS" = 1 ] && grep -q "cross-linking is not supported in 2.0" musl.err; then
      pass "--target=$MUSL is foreign (different ABI environment)"
    else
      fail "--target=$MUSL: status $STATUS, stderr: $(cat musl.err)"
    fi
    ;;
  *)
    echo "   skip (host '$HOST' is not a linux-gnu triple; only that spelling set is checked here)"
    ;;
esac

echo "== @ffi reads the --target platform, not the host's"
# Only the macOS key is written: a darwin target forwards text_length to
# strlen, any linux target binds text_length itself. -O0, or the call to
# strlen("abc") is inlined and folded to 3.
cat > ffi.tr <<'EOF'
@ffi(macos: strlen)
extern fn text_length(s: string): int;

fn main(): int {
    return text_length("abc");
}
EOF
forwards_to_strlen() {
  grep -A4 '^define.*@text_length(' "$1" | grep -q 'call .*@strlen('
}
rm -f ffi_mac.ll
"$TURFC" ffi.tr -O0 --target=x86_64-apple-darwin --emit-llvm -o ffi_mac > ffi_mac.out 2> ffi_mac.err
if [ -f ffi_mac.ll ] && forwards_to_strlen ffi_mac.ll; then
  pass "--target=x86_64-apple-darwin forwards text_length to strlen"
else
  fail "darwin target did not forward: $(cat ffi_mac.err; ls)"
fi
rm -f ffi_linux.ll
"$TURFC" ffi.tr -O0 --target=aarch64-unknown-linux-gnu --emit-llvm -o ffi_linux > ffi_linux.out 2> ffi_linux.err
if [ -f ffi_linux.ll ] && ! grep -q '^define.*@text_length(' ffi_linux.ll && grep -q '^declare.*@text_length(' ffi_linux.ll; then
  pass "--target=aarch64-unknown-linux-gnu binds text_length itself"
else
  fail "linux target should declare text_length: $(cat ffi_linux.err; ls)"
fi

echo "== no --target is the host, unchanged"
rm -f plain
if "$TURFC" hello.tr -o plain > plain.out 2> plain.err && ./plain | grep -q "hello from a cross target smoke"; then
  pass "plain build links and runs"
else
  fail "plain build failed: $(cat plain.err)"
fi

echo "== --emit-obj writes the object and never links"
# Driver interface 1.6: turf-fuzz compiles thousands of programs it never
# runs, and the system linker was about half of each compile.
rm -f only only.o
if "$TURFC" hello.tr --emit-obj -o only > only.out 2> only.err && [ -f only.o ] && [ ! -e only ]; then
  pass "host object only.o, no binary"
  if [ "$(od -An -tu2 -j16 -N2 only.o | tr -d ' ')" = 1 ]; then pass "only.o is a relocatable object"; else fail "only.o e_type is not ET_REL"; fi
  if grep -q "Compiled to only.o" only.out; then pass "says where the object went"; else fail "no 'Compiled to only.o': $(cat only.out)"; fi
else
  fail "--emit-obj: rc or files wrong: $(cat only.err; ls)"
fi
rm -f armonly.o
# For another machine it is the object the user asked for, so no D20 error.
if "$TURFC" hello.tr --emit-obj --target=aarch64-unknown-linux-gnu -o armonly > armonly.out 2> armonly.err \
    && [ "$(elf_machine armonly.o)" = 183 ] && ! grep -q "cannot link" armonly.err; then
  pass "--emit-obj --target=aarch64 exits 0 with an AArch64 object"
else
  fail "--emit-obj for a foreign target: $(cat armonly.err)"
fi
"$TURFC" hello.tr --emit-obj --emit-llvm -o both > both.out 2> both.err
rc=$?
if [ "$rc" = 2 ] && grep -q "pass one of them" both.err; then
  pass "--emit-obj with --emit-llvm is a usage error (exit 2)"
else
  fail "--emit-obj --emit-llvm: rc $rc, $(cat both.err)"
fi

echo
if [ "$FAILED" -eq 0 ]; then
  echo "cross_target_smoke: all checks passed"
  exit 0
fi
echo "cross_target_smoke: $FAILED check(s) failed"
exit 1
