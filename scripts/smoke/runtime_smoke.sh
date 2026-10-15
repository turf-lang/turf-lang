#!/bin/bash
#
# runtime_smoke.sh -- RT-001's own regression coverage
# (docs/archive/ARCHITECTURE_OVERHAUL.md §8 RT-001, "Testing scope").
# `test_runner.sh` cannot exercise any of this: libturfrt is a separate C
# static library, not a `.tr` file, and none of what's checked here (link
# wiring, a descriptor's exact bitmap, a panic's exact message/exit code)
# is something a `.tr` program can observe about itself.
#
# What this proves:
#   1. libturfrt actually links into a compiled Turf binary (nm shows the
#      turf_rt_* symbols; the process runs and exits 0).
#   2. turf_rt_init()/turf_rt_shutdown() are genuinely on the call path
#      around the user's `main` body (checked at -O0, where the wrapper
#      call sequence survives un-inlined -- see WrapMainWithRuntimeBootstrap()'s
#      own doc comment, src/codegen/Codegen.h).
#   3. A panic (division by zero) aborts with the documented message shape
#      ("panic: <kind>: <msg> (<file>:<line>)") and a non-zero, non-timeout
#      exit code -- the exact contract test_runner.sh's own
#      RUNTIME_ABORT_EXPECTED convention checks for `fail_*.tr` tests.
#   4. A handful of representative struct types get a TurfTypeDesc with the
#      correct size/word_count and pointer-words bitmap array, read back out
#      of --emit-llvm output (the compiler's own recommended verification
#      method for this group) -- including a type with more than 64 words
#      (OVH-001: the original single-uint64_t bitmap silently truncated
#      anything past word 63/byte 512, which MEM-002's collector then
#      depended on for correctness -- a live use-after-free for any struct
#      that size or larger).
#   5. libturfrt's own C API (turf_rt_alloc/turf_rt_panic/
#      turf_rt_typedesc_is_pointer_word), called directly from a small C
#      program linked straight against lib/libturfrt.a -- no Turf compiler
#      involved at all, the most direct test this library gets.
#   6. FDN-006 (docs/development/ROADMAP_v2.0.0.md): deep, non-tail recursion
#      prints "panic: stack overflow" (not a bare SIGSEGV, exit 139) and
#      aborts (134); a direct huge allocation prints "panic: out of memory"
#      (already existed since RT-001 -- this is its first real test, per
#      FDN-006 stage 2's own instruction to test it against a direct large
#      allocation rather than the Overhaul closure audit's quadratic-loop
#      attempt, which never actually reached this path); linking with
#      TURF_HOME pointing at a runtime-less tree gives a TurfLang
#      diagnostic, not cc's own generic linker error.
#
# Usage: scripts/smoke/runtime_smoke.sh
# Exit:  0 = every check passed; 1 = at least one failed.

set -u
cd "$(dirname "$0")/../.." || exit 1

BOLD="\033[1m"; GREEN="\033[0;32m"; RED="\033[0;31m"; YELLOW="\033[0;33m"
RESET="\033[0m"

TURF="./build/turfc"
LIBTURFRT="lib/libturfrt.a"
FAIL=0
WORKDIR=$(mktemp -d)
trap 'rm -rf "$WORKDIR"' EXIT

pass() { echo -e "  ${GREEN}✓${RESET} $1"; }
fail() { echo -e "  ${RED}✗${RESET} $1"; FAIL=1; }

if [ ! -x "$TURF" ]; then
  echo -e "${RED}build/turfc missing -- build before running this smoke test.${RESET}"
  exit 1
fi
if [ ! -f "$LIBTURFRT" ]; then
  echo -e "${RED}$LIBTURFRT missing -- build before running this smoke test.${RESET}"
  exit 1
fi

echo -e "${BOLD}1. libturfrt links into a compiled program${RESET}"
cat > "$WORKDIR/hello.tr" <<'EOF'
import io;
fn main(): int {
    io::printline("runtime_smoke ok");
    return 0;
}
EOF
if "$TURF" "$WORKDIR/hello.tr" -o "$WORKDIR/hello" > "$WORKDIR/hello.compile.log" 2>&1; then
  pass "compiled"
else
  fail "compile failed"; cat "$WORKDIR/hello.compile.log"
fi
if nm "$WORKDIR/hello" 2>/dev/null | grep -qE "T _?turf_rt_init"; then
  pass "turf_rt_init linked in (nm)"
else
  fail "turf_rt_init not found in linked binary"
fi
OUT=$("$WORKDIR/hello" 2>&1); STATUS=$?
if [ "$STATUS" -eq 0 ] && [ "$OUT" = "runtime_smoke ok" ]; then
  pass "ran, exit 0, correct output"
else
  fail "unexpected run result (status=$STATUS, output='$OUT')"
fi

echo -e "${BOLD}2. turf_rt_init/turf_rt_shutdown on the call path around main (-O0 IR)${RESET}"
if "$TURF" "$WORKDIR/hello.tr" --emit-llvm -O0 -o "$WORKDIR/hello_o0" > /dev/null 2>&1; then
  IR="$WORKDIR/hello_o0.ll"
  if grep -q "@turf_rt_init()" "$IR" && grep -q "@turf_rt_shutdown()" "$IR" \
     && grep -q "@__turf_user_main" "$IR"; then
    pass "wrapper calls both + delegates to __turf_user_main"
  else
    fail "wrapper shape missing from -O0 IR"
  fi
else
  fail "-O0 --emit-llvm compile failed"
fi

echo -e "${BOLD}3. a panic aborts with the documented message shape${RESET}"
cat > "$WORKDIR/panic.tr" <<'EOF'
fn main(): int {
    mut z: int = 0;
    mut x: int = 10 / z;
    return x;
}
EOF
if "$TURF" "$WORKDIR/panic.tr" -o "$WORKDIR/panic" > /dev/null 2>&1; then
  "$WORKDIR/panic" > "$WORKDIR/panic.out" 2>&1
  STATUS=$?
  if [ "$STATUS" -ne 0 ] && [ "$STATUS" -ne 137 ] && [ "$STATUS" -ne 143 ]; then
    pass "non-zero, non-timeout exit ($STATUS)"
  else
    fail "expected a runtime abort, got exit $STATUS"
  fi
  if grep -qE "^panic: runtime check: Division by zero \(.*panic\.tr:[0-9]+\)$" "$WORKDIR/panic.out"; then
    pass "message matches 'panic: <kind>: <msg> (<file>:<line>)'"
  else
    fail "unexpected panic message:"; cat "$WORKDIR/panic.out"
  fi
else
  fail "panic.tr failed to compile"
fi

echo -e "${BOLD}4. TurfTypeDesc bitmaps for representative types${RESET}"
BIG70_FIELDS=""
for i in $(seq 0 69); do BIG70_FIELDS="${BIG70_FIELDS}f${i}: int, "; done
cat > "$WORKDIR/types.tr" <<EOF
import list::List;

struct Plain {
    a: int,
    b: double
}

struct WithPointer {
    p: Plain,
    tag: string
}

// OVH-001: 70 int fields (words 0-69) then a pointer field at word 70 --
// past the old fixed 64-word bitmap cap.
struct Big70 {
    ${BIG70_FIELDS}name: string
}

fn main(): int {
    mut pl: Plain = Plain::default();
    mut wp: WithPointer = WithPointer::default();
    mut l: List<int> = List::default();
    l.push(1);
    mut big: Big70 = Big70::default();
    return pl.a + wp.p.a + l.length() + big.f0;
}
EOF
if "$TURF" "$WORKDIR/types.tr" --emit-llvm -o "$WORKDIR/types" > /dev/null 2>&1; then
  IR="$WORKDIR/types.ll"
  # `want_bitmap_words` is a comma-separated list of the expected [N x i64]
  # bitmap array's own elements, e.g. "0,64" for a 2-word array.
  check_desc() {
    local name="$1" want_size="$2" want_word_count="$3" want_bitmap_words="$4"
    local desc_line bitmap_line n
    desc_line=$(grep "@__turf_typedesc_${name} = " "$IR")
    if echo "$desc_line" | grep -qE "i64 ${want_size}, ptr @__turf_typedesc_name\.${name}, i64 ${want_word_count}, ptr @__turf_typedesc_bitmap\.${name} }"; then
      pass "$name: size=$want_size word_count=$want_word_count"
    else
      fail "$name: expected size=$want_size word_count=$want_word_count, got: $desc_line"
    fi
    n=$(( $(echo "$want_bitmap_words" | tr -cd ',' | wc -c) + 1 ))
    bitmap_line=$(grep "@__turf_typedesc_bitmap\.${name} = " "$IR")
    # LLVM's own printer collapses an all-zero constant array to the
    # `zeroinitializer` sigil rather than spelling out every `i64 0`.
    local want_array_lit
    if echo "$want_bitmap_words" | grep -qE "^0(,0)*$"; then
      want_array_lit="[${n} x i64] zeroinitializer"
    else
      want_array_lit="[${n} x i64] [$(echo "$want_bitmap_words" | sed 's/^/i64 /; s/,/, i64 /g')]"
    fi
    if echo "$bitmap_line" | grep -qF "$want_array_lit"; then
      pass "$name: bitmap=[$want_bitmap_words] ($n word(s))"
    else
      fail "$name: expected bitmap literal '$want_array_lit', got: $bitmap_line"
    fi
  }
  # Plain { a: int (8B @0), b: double (8B @8) } -- no pointer words.
  check_desc "Plain" 16 2 "0"
  # WithPointer { p: Plain (16B @0, no pointers), tag: string (8B @16, a
  # pointer) } -- bit 2 (word 16/8) set => 4.
  check_desc "WithPointer" 24 3 "4"
  # ListHeader<int> { data: T* (@0, a pointer), len: int (@8), cap: int
  # (@16) } -- bit 0 set => 1, and List<int> is one `h: &ListHeader<int>`
  # pointer (LEC-009). Named `list.…_gen1_int`: a module-owned type's LLVM
  # struct (and so its descriptor) is `module.Name` (FDN-003).
  check_desc "list.ListHeader_gen1_int" 24 3 "1"
  check_desc "list.List_gen1_int" 8 1 "1"
  # OVH-001: Big70 { f0..f69: int (70 * 8B = 560B), name: string (8B @560,
  # a pointer) } -- word_count = ceil(568/8) = 71, so the bitmap array has
  # ceil(71/64) = 2 words. `name`'s word index is 560/8 = 70, which falls
  # in the SECOND array word (70/64 = 1) at bit 70%64 = 6 => value 64. The
  # old fixed-64-word bitmap could not represent this bit at all.
  check_desc "Big70" 568 71 "0,64"
else
  fail "types.tr failed to compile"
fi

echo -e "${BOLD}5. libturfrt's own C API, called directly (no Turf compiler involved)${RESET}"
cat > "$WORKDIR/api_test.c" <<'EOF'
#include "turfrt.h"
#include <assert.h>
#include <string.h>

int main(void) {
    turf_rt_init();

    void *p = turf_rt_alloc(64, NULL);
    assert(p != NULL);

    uint64_t bits[1] = {4}; /* bit 2 set */
    TurfTypeDesc desc = {24, "WithPointer", 3, bits};
    assert(turf_rt_typedesc_is_pointer_word(&desc, 0) == 0);
    assert(turf_rt_typedesc_is_pointer_word(&desc, 1) == 0);
    assert(turf_rt_typedesc_is_pointer_word(&desc, 2) == 1);
    assert(turf_rt_typedesc_is_pointer_word(&desc, 3) == 0); /* out of range (word_count=3) */
    assert(turf_rt_typedesc_is_pointer_word(NULL, 0) == 0);

    /* OVH-001: a descriptor wider than the old fixed 64-word cap -- word
     * 128 (array index 128/64=2, bit 0) must still be traced correctly. */
    uint64_t big_bits[3] = {0, 0, 1};
    TurfTypeDesc big_desc = {1040, "Big", 130, big_bits};
    assert(turf_rt_typedesc_is_pointer_word(&big_desc, 63) == 0);
    assert(turf_rt_typedesc_is_pointer_word(&big_desc, 64) == 0);
    assert(turf_rt_typedesc_is_pointer_word(&big_desc, 128) == 1);
    assert(turf_rt_typedesc_is_pointer_word(&big_desc, 129) == 0);
    assert(turf_rt_typedesc_is_pointer_word(&big_desc, 130) == 0); /* out of range */

    turf_rt_shutdown();
    return 0;
}
EOF
if cc -std=c11 -I runtime "$WORKDIR/api_test.c" "$LIBTURFRT" -o "$WORKDIR/api_test" > "$WORKDIR/api_test.compile.log" 2>&1; then
  pass "compiled against libturfrt.a directly"
  if "$WORKDIR/api_test"; then
    pass "ran, all assertions held"
  else
    fail "assertion failed at runtime"
  fi
else
  fail "failed to compile against libturfrt.a"; cat "$WORKDIR/api_test.compile.log"
fi

echo -e "${BOLD}6. FDN-006: stack overflow, OOM, and missing-runtime diagnostics${RESET}"

# 6a. Deep, non-tail recursion -> "panic: stack overflow", exit 134 (not a
# bare SIGSEGV/139). The recursive call feeds `+ buf[0]` after it returns,
# so it is not in tail position -- no optimization level can turn this into
# a loop that never grows the stack. `n > 2000000000` only satisfies the
# compiler's own unconditional-infinite-recursion check (E010); the stack
# overflows long before `n` could reach it.
cat > "$WORKDIR/stack_overflow.tr" <<'EOF'
fn recurse(n: int): int {
    mut buf: int[8] = [1, 2, 3, 4, 5, 6, 7, 8];
    if n > 2000000000 {
        return buf[0];
    }
    return recurse(n + 1) + buf[0];
}
fn main(): int {
    return recurse(0);
}
EOF
if "$TURF" "$WORKDIR/stack_overflow.tr" -o "$WORKDIR/stack_overflow" > /dev/null 2>&1; then
  "$WORKDIR/stack_overflow" > "$WORKDIR/stack_overflow.out" 2>&1
  STATUS=$?
  if [ "$STATUS" -eq 134 ]; then
    pass "deep recursion aborts with exit 134 (not a bare SIGSEGV/139)"
  else
    fail "expected exit 134, got $STATUS"; cat "$WORKDIR/stack_overflow.out"
  fi
  if grep -q "^panic: stack overflow" "$WORKDIR/stack_overflow.out"; then
    pass "message starts 'panic: stack overflow'"
  else
    fail "unexpected message:"; cat "$WORKDIR/stack_overflow.out"
  fi
else
  fail "stack_overflow.tr failed to compile"
fi

# 6b. A direct huge allocation -> "panic: out of memory", exit 134. Goes
# straight at gc_alloc() with a size no allocator can satisfy, rather than
# the audit's own quadratic-loop attempt (strings::repeat), which spun
# forever without ever reaching this path.
cat > "$WORKDIR/oom.tr" <<'EOF'
fn main(): int {
    unsafe {
        mut p: void* = gc_alloc(9223372036854775807);
    }
    return 0;
}
EOF
if "$TURF" "$WORKDIR/oom.tr" -o "$WORKDIR/oom" > /dev/null 2>&1; then
  "$WORKDIR/oom" > "$WORKDIR/oom.out" 2>&1
  STATUS=$?
  if [ "$STATUS" -eq 134 ]; then
    pass "huge direct allocation aborts with exit 134"
  else
    fail "expected exit 134, got $STATUS"; cat "$WORKDIR/oom.out"
  fi
  if grep -q "^panic: out of memory" "$WORKDIR/oom.out"; then
    pass "message starts 'panic: out of memory'"
  else
    fail "unexpected message:"; cat "$WORKDIR/oom.out"
  fi
else
  fail "oom.tr failed to compile"
fi

# 6c. TURF_HOME pointing at a runtime-less tree -> a TurfLang diagnostic
# naming the missing libturfrt.a, reported BEFORE cc/gcc is ever invoked --
# not cc's own generic "-lturfrt: No such file" linker error. `lib/std`
# itself is COPIED in (real, complete), not symlinked: a symlink resolves
# to the real repo's own path via real_path(), which would make
# IsPathUnderInstallRoot()'s canonical-path check see the file as living
# under the REAL install root instead of $FAKE_HOME, silently changing which
# capability gates apply and breaking this check for an unrelated reason.
FAKE_HOME=$(mktemp -d)
mkdir -p "$FAKE_HOME/lib"
cp -r lib/std "$FAKE_HOME/lib/std"
if TURF_HOME="$FAKE_HOME" "$TURF" "$WORKDIR/hello.tr" -o "$WORKDIR/hello_fakehome" \
     > "$WORKDIR/fakehome.out" 2>&1; then
  fail "expected compilation to fail against a runtime-less TURF_HOME, it succeeded"
else
  if grep -q "TurfLang runtime library not found" "$WORKDIR/fakehome.out" \
     && grep -qF "$FAKE_HOME/lib/libturfrt.a" "$WORKDIR/fakehome.out"; then
    pass "missing libturfrt.a reported as a TurfLang diagnostic naming the path"
  else
    fail "expected a TurfLang diagnostic naming the missing path, got:"
    cat "$WORKDIR/fakehome.out"
  fi
fi
rm -rf "$FAKE_HOME"

# 6d. A program that names a library the C runtime does not provide goes to the
# system toolchain. With no compiler on PATH that is a TurfLang diagnostic naming the
# library, not a bare "could not exec"; with one, a library the linker cannot find
# gets the hint that follows the linker's own line.
EMPTY_PATH=$(mktemp -d)
if PATH="$EMPTY_PATH" "$TURF" "$WORKDIR/hello.tr" -o "$WORKDIR/nolib_nocc" --link nosuchlib \
     > "$WORKDIR/nocc.out" 2>&1; then
  fail "expected the link to fail with no C compiler on PATH, it succeeded"
elif grep -q "linking needs a C compiler" "$WORKDIR/nocc.out" \
     && grep -qF '"nosuchlib"' "$WORKDIR/nocc.out"; then
  pass "no C compiler: a diagnostic that names the library the program links"
else
  fail "expected 'linking needs a C compiler' naming nosuchlib, got:"; cat "$WORKDIR/nocc.out"
fi
rm -rf "$EMPTY_PATH"
if "$TURF" "$WORKDIR/hello.tr" -o "$WORKDIR/nolib_nofile" --link nosuchlib \
     > "$WORKDIR/nolib.out" 2>&1; then
  fail "expected the link to fail for a library that does not exist, it succeeded"
elif grep -q 'links against "nosuchlib", which Turf does not bundle' "$WORKDIR/nolib.out"; then
  pass "a library the linker cannot find: the hint names it and says what to install"
else
  fail "expected the 'does not bundle' hint, got:"; cat "$WORKDIR/nolib.out"
fi

# SIGBUS gets the same handler as SIGSEGV: macOS reports protection faults,
# stack-guard hits among them, as SIGBUS, so a SIGSEGV-only handler left
# them as bare crashes. Linux raises a real SIGBUS on touching an mmap'd page
# past a file's (truncated) end -- that is what this does.
echo -e "${BOLD}7. a SIGBUS is reported like a SIGSEGV, not a bare crash${RESET}"
cat > "$WORKDIR/bus.c" <<'EOF'
#include "turfrt.h"
#include <fcntl.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
int main(void) {
  turf_rt_init();
  char path[] = "/tmp/turfrt_busXXXXXX";
  int fd = mkstemp(path);
  if (fd < 0 || ftruncate(fd, 4096) != 0) return 2;
  volatile char *p = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED) return 3;
  if (ftruncate(fd, 0) != 0) return 4;
  unlink(path);
  return p[0];
}
EOF
if cc -std=gnu11 -I runtime "$WORKDIR/bus.c" "$LIBTURFRT" -o "$WORKDIR/bus" > "$WORKDIR/bus.compile.log" 2>&1; then
  "$WORKDIR/bus" > "$WORKDIR/bus.out" 2>&1
  STATUS=$?
  if [ "$STATUS" -eq 134 ] && grep -q "^panic: bus error" "$WORKDIR/bus.out"; then
    pass "SIGBUS -> 'panic: bus error ...', exit 134"
  else
    fail "expected 'panic: bus error' and exit 134, got exit $STATUS:"; cat "$WORKDIR/bus.out"
  fi
else
  fail "bus.c failed to compile:"; cat "$WORKDIR/bus.compile.log"
fi

echo
if [ "$FAIL" -eq 0 ]; then
  echo -e "${GREEN}${BOLD}All runtime smoke checks passed.${RESET}"
  exit 0
else
  echo -e "${RED}${BOLD}At least one runtime smoke check failed.${RESET}"
  exit 1
fi
