#!/bin/bash
#
# gc_smoke.sh -- MEM-002's own regression coverage
# (docs/archive/ARCHITECTURE_OVERHAUL.md §8 MEM-002). Mirrors
# runtime_smoke.sh/mem_smoke.sh's shape (RT-001/MEM-001): covers exactly the
# two things this group's own Testing scope names that CANNOT be expressed
# as an ordinary `.tr` test (tests/41_gc/ holds the rest -- retention,
# interior pointers, FFI-crossing, the cross-heap mem::free() diagnostic):
#
#   1. "turf_rt_stats()-based tests: allocate N, drop all references, force
#      a collection, assert live count." turf_rt_stats() is deliberately
#      NOT reachable from Turf source (Rule B2/B3) -- only a C-linked
#      caller can read live_count/live_bytes/freed_count at all.
#   2. turf_rt_add_global_root()/turf_rt_remove_global_root(). TurfLang has
#      no mutable global/static storage today (verified live -- no
#      `GlobalVariable` write site exists anywhere in Codegen except the
#      compiler's own argv/argc plumbing and RT-001's constant TurfTypeDesc
#      globals), so there is no Codegen call site to exercise this through;
#      it is genuine, tested runtime API reached directly, the same
#      precedent mem_smoke.sh's check 1 already sets for
#      Turf-source-unreachable libturfrt entrypoints.
#   3. OVH-010 (docs/development/changes/OVH-010.md): turf_rt_pin()/
#      turf_rt_unpin() -- specifically the "unroot() actually lets the
#      collector reclaim it" direction, which `tests/41_gc/
#      11_mem_root_pins_manual_heap_reference.tr`'s own top comment
#      explains is NOT reliably provable as a compiled-Turf `.tr` test:
#      that file only proves the "survives while pinned" direction (which
#      a conservative collector can never get wrong by accident), the same
#      way check 2's own comment below explains for global-root removal.
#      This check compiles with no `-O` flag at all (this script's own
#      `cc` invocations below), so nothing keeps the checked address alive
#      in a register across the collection the way an optimizing build of
#      a single Turf function legitimately might.
#
# Usage: scripts/smoke/gc_smoke.sh
# Exit:  0 = every check passed; 1 = at least one failed.

set -u
cd "$(dirname "$0")/../.." || exit 1

BOLD="\033[1m"; GREEN="\033[0;32m"; RED="\033[0;31m"; RESET="\033[0m"

LIBTURFRT="lib/libturfrt.a"
FAIL=0
WORKDIR=$(mktemp -d)
trap 'rm -rf "$WORKDIR"' EXIT

pass() { echo -e "  ${GREEN}✓${RESET} $1"; }
fail() { echo -e "  ${RED}✗${RESET} $1"; FAIL=1; }

if [ ! -f "$LIBTURFRT" ]; then
  echo -e "${RED}$LIBTURFRT missing -- build before running this smoke test.${RESET}"
  exit 1
fi

echo -e "${BOLD}1. Allocate N, drop all references, force a collection, assert most were reclaimed${RESET}"
cat > "$WORKDIR/drop_test.c" <<'EOF'
#include "turfrt.h"
#include <assert.h>
#include <stdio.h>

/* Touches (writes then reads) a stack buffer so any stale copy of a
 * previous iteration's now-dead pointer left behind in unused stack memory
 * gets overwritten -- without this, a conservative scanner could pass this
 * check "by accident" (stale garbage that happens to still be a valid
 * address), which would defeat the point of the test. */
static void churn(int depth) {
    volatile long buf[64];
    for (int i = 0; i < 64; i++) buf[i] = i;
    long sum = 0;
    for (int i = 0; i < 64; i++) sum += buf[i];
    assert(sum == 2016);
    if (depth > 0) churn(depth - 1);
}

int main(void) {
    turf_rt_init();

    const int N = 2000;
    for (int i = 0; i < N; i++) {
        /* Every iteration's block is reachable ONLY through this one local,
         * which gets overwritten (not accumulated anywhere) next
         * iteration -- by the time the loop ends, at most the LAST
         * iteration's block is genuinely live. */
        void *p = turf_rt_alloc(64, NULL);
        (void)p;
        churn(6);
    }
    churn(32);
    turf_rt_collect();

    TurfRtStats s = turf_rt_stats();
    printf("after collect: live_count=%llu (of %d allocated), collect_count=%llu\n",
           (unsigned long long)s.live_count, N, (unsigned long long)s.collect_count);

    /* Correctness bound, not a tight one -- a conservative collector may
     * over-retain (§14's own accepted tradeoff), never under-retain, so
     * this only asserts that the OVERWHELMING majority of 2000 short-lived
     * blocks were reclaimed, not that live_count is exactly 0 or 1 (which a
     * conservative collector can never promise in general). */
    assert(s.collect_count >= 1);
    assert(s.live_count < (uint64_t)(N / 4));

    turf_rt_shutdown();
    return 0;
}
EOF
if cc -std=c11 -I runtime "$WORKDIR/drop_test.c" "$LIBTURFRT" -o "$WORKDIR/drop_test" > "$WORKDIR/drop_test.compile.log" 2>&1; then
  pass "compiled against libturfrt.a directly"
  OUT=$("$WORKDIR/drop_test" 2>&1)
  RC=$?
  if [ $RC -eq 0 ]; then
    pass "ran, all assertions held -- $OUT"
  else
    fail "assertion failed at runtime: $OUT"
  fi
else
  fail "failed to compile against libturfrt.a"; cat "$WORKDIR/drop_test.compile.log"
fi

echo -e "${BOLD}2. turf_rt_add_global_root/turf_rt_remove_global_root${RESET}"
cat > "$WORKDIR/root_test.c" <<'EOF'
#include "turfrt.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

static void churn(int depth) {
    volatile long buf[64];
    for (int i = 0; i < 64; i++) buf[i] = i * 3;
    if (depth > 0) churn(depth - 1);
}

int main(void) {
    turf_rt_init();

    void *root_slot = turf_rt_alloc(32, NULL);
    memcpy(root_slot, "hello-from-a-global-root-slot!!", 32);

    /* Registered as a global root -- nothing else in this program keeps
     * root_slot's C local alive across the churn/collect below (a real
     * compiled Turf program's stack would look identical to a
     * conservative scanner: this local COULD coincidentally still keep it
     * alive too, which is fine -- what this test actually proves is that
     * REMOVING the root, and having nothing else reference it, lets it be
     * reclaimed, which only the DEREGISTRATION half can demonstrate). */
    turf_rt_add_global_root(&root_slot);

    churn(24);
    turf_rt_collect();
    assert(memcmp(root_slot, "hello-from-a-global-root-slot!!", 32) == 0);
    printf("registered root survived a collection with correct content\n");

    turf_rt_remove_global_root(&root_slot);
    root_slot = NULL; /* the only C-level reference to the old block is gone */
    churn(64);        /* overwrite whatever stale copy churn()'s own frames left */
    turf_rt_collect();

    TurfRtStats s = turf_rt_stats();
    printf("after removing the root and collecting: live_count=%llu\n",
           (unsigned long long)s.live_count);
    /* Same conservative-scanning caveat as check 1 -- this only asserts
     * the removed root's block is no longer counted as definitely live via
     * the (now nonexistent) root path; incidental stack retention is a
     * possible, accepted false positive, not something this assertion
     * depends on being absent. */
    assert(s.live_count == 0);

    turf_rt_shutdown();
    return 0;
}
EOF
if cc -std=c11 -I runtime "$WORKDIR/root_test.c" "$LIBTURFRT" -o "$WORKDIR/root_test" > "$WORKDIR/root_test.compile.log" 2>&1; then
  pass "compiled against libturfrt.a directly"
  OUT=$("$WORKDIR/root_test" 2>&1)
  RC=$?
  if [ $RC -eq 0 ]; then
    pass "ran, all assertions held"
    echo "$OUT" | sed 's/^/    /'
  else
    fail "assertion failed at runtime: $OUT"
  fi
else
  fail "failed to compile against libturfrt.a"; cat "$WORKDIR/root_test.compile.log"
fi

echo -e "${BOLD}3. turf_rt_pin/turf_rt_unpin${RESET}"
cat > "$WORKDIR/pin_test.c" <<'EOF'
#include "turfrt.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static void churn(int depth) {
    volatile long buf[64];
    for (int i = 0; i < 64; i++) buf[i] = i * 3;
    if (depth > 0) churn(depth - 1);
}

int main(void) {
    turf_rt_init();

    /* `manual_slot` stands in for "a GC pointer reachable only through
     * manual-heap memory" (a C library's own struct, a mem::alloc buffer)
     * -- mem::root()'s own motivating shape (lib/std/mem/mem.tr). An
     * INTERIOR pointer (`p + 4`, not the block's own start address) is
     * pinned deliberately, exercising the "interior pointers allowed"
     * half of the contract the same way stack/register roots already do. */
    void *manual_slot = malloc(sizeof(void *));
    void *p = turf_rt_alloc(64, NULL);
    memcpy(p, "pinned-object-content-check!!!!", 32);
    memcpy(manual_slot, &p, sizeof(void *));
    p = NULL;

    turf_rt_pin((char *)*(void **)manual_slot + 4);

    churn(24);
    turf_rt_collect();

    void *pinned = *(void **)manual_slot;
    assert(turf_rt_is_gc_pointer(pinned));
    assert(memcmp(pinned, "pinned-object-content-check!!!!", 32) == 0);
    printf("pinned object survived a collection with correct content\n");

    turf_rt_unpin((char *)pinned + 4);
    pinned = NULL; /* the only remaining C-level reference to the old block */
    memset(manual_slot, 0, sizeof(void *)); /* drop the manual-heap reference too */
    churn(64);
    turf_rt_collect();

    TurfRtStats s = turf_rt_stats();
    printf("after unpin (nothing else references it) + collect: live_count=%llu\n",
           (unsigned long long)s.live_count);
    /* Same conservative-scanning caveat as check 2's own removal
     * assertion: this only asserts the now-unpinned, now-unreferenced
     * block is gone, not that pinning could never coincide with
     * incidental stack retention in general. */
    assert(s.live_count == 0);

    free(manual_slot);
    turf_rt_shutdown();
    return 0;
}
EOF
if cc -std=c11 -I runtime "$WORKDIR/pin_test.c" "$LIBTURFRT" -o "$WORKDIR/pin_test" > "$WORKDIR/pin_test.compile.log" 2>&1; then
  pass "compiled against libturfrt.a directly"
  OUT=$("$WORKDIR/pin_test" 2>&1)
  RC=$?
  if [ $RC -eq 0 ]; then
    pass "ran, all assertions held"
    echo "$OUT" | sed 's/^/    /'
  else
    fail "assertion failed at runtime: $OUT"
  fi
else
  fail "failed to compile against libturfrt.a"; cat "$WORKDIR/pin_test.compile.log"
fi

echo
if [ "$FAIL" -eq 0 ]; then
  echo -e "${GREEN}${BOLD}All gc smoke checks passed.${RESET}"
  exit 0
else
  echo -e "${RED}${BOLD}At least one gc smoke check failed.${RESET}"
  exit 1
fi
