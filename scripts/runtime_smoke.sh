#!/bin/bash
#
# runtime_smoke.sh -- RT-001's own regression coverage
# (docs/planning/ARCHITECTURE_OVERHAUL.md §8 RT-001, "Testing scope").
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
#      own doc comment, include/Codegen.h).
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
#
# Usage: scripts/runtime_smoke.sh
# Exit:  0 = every check passed; 1 = at least one failed.

set -u
cd "$(dirname "$0")/.." || exit 1

BOLD="\033[1m"; GREEN="\033[0;32m"; RED="\033[0;31m"; YELLOW="\033[0;33m"
RESET="\033[0m"

TURF="./build/turf"
LIBTURFRT="lib/libturfrt.a"
FAIL=0
WORKDIR=$(mktemp -d)
trap 'rm -rf "$WORKDIR"' EXIT

pass() { echo -e "  ${GREEN}✓${RESET} $1"; }
fail() { echo -e "  ${RED}✗${RESET} $1"; FAIL=1; }

if [ ! -x "$TURF" ]; then
  echo -e "${RED}build/turf missing -- build before running this smoke test.${RESET}"
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
if nm "$WORKDIR/hello" 2>/dev/null | grep -q "T turf_rt_init"; then
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
  # List<int> { data: T* (@0, a pointer), len: int (@8), cap: int (@16) }
  # -- bit 0 set => 1.
  check_desc "List_gen1_int" 24 3 "1"
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

echo
if [ "$FAIL" -eq 0 ]; then
  echo -e "${GREEN}${BOLD}All runtime smoke checks passed.${RESET}"
  exit 0
else
  echo -e "${RED}${BOLD}At least one runtime smoke check failed.${RESET}"
  exit 1
fi
