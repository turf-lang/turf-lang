/*
 * runtime/turfrt_win32.c -- REL-008 stage 5 (docs/planning/ROADMAP_v2.0.0.md).
 *
 * Three C functions the standard library binds by their POSIX names, which the
 * Windows C runtime (UCRT) does not have: `posix_memalign`, `malloc_usable_size`
 * and `setenv` (lib/std/mem/mem.tr, lib/std/os/os.tr). A Windows build of any
 * program that imported `mem` or `os` failed to LINK with "undefined symbol"
 * (found by compiling every test for Windows and linking it, 4 of 602 did not).
 *
 * They are defined here under the SAME names, so nothing in lib/std changes and
 * no public API moves: Turf has no conditional compilation, and the library is
 * written against raw libc bindings, so the portable place for "this platform
 * spells it differently" is the one C library that is built per platform.
 *
 * The one subtlety is releasing memory. A block from `_aligned_malloc` must be
 * freed with `_aligned_free`, never `free`, but every manual-heap release goes
 * through turf_rt_checked_free() (mem::free -> gc_checked_free), which is the
 * runtime's own: it asks turf_win32_free_aligned() first. A block whose
 * alignment the ordinary allocator already provides (16 bytes on Win64) is a
 * plain malloc block and is not tracked at all.
 *
 * Limitation, disclosed: `mem::grow` (realloc) on an over-aligned block is not
 * supported on Windows -- `realloc` is bound straight to the C runtime, which
 * would hand an `_aligned_malloc` pointer to a plain realloc. Over-aligned
 * (> 16 byte) allocations are rare in this library's own use.
 */
#ifdef _WIN32

#include <errno.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "turfrt_internal.h"

/* malloc on Win64 returns 16-byte-aligned memory. */
#define TURF_WIN32_MALLOC_ALIGN 16

typedef struct AlignedBlock {
  void *ptr;
  size_t alignment;
  size_t size;
  struct AlignedBlock *next;
} AlignedBlock;

static AlignedBlock *g_aligned = NULL;
static SRWLOCK g_aligned_lock = SRWLOCK_INIT;

int posix_memalign(void **memptr, size_t alignment, size_t size) {
  /* (A NULL `memptr` is undefined in POSIX too, and MinGW's header marks it
   * nonnull, so it is not checked.) POSIX: the alignment is a power of two
   * that is a multiple of sizeof(void *). */
  if (alignment < sizeof(void *) || (alignment & (alignment - 1)) != 0)
    return EINVAL;

  if (alignment <= TURF_WIN32_MALLOC_ALIGN) {
    void *p = malloc(size == 0 ? 1 : size);
    if (p == NULL)
      return ENOMEM;
    *memptr = p;
    return 0;
  }

  void *p = _aligned_malloc(size == 0 ? 1 : size, alignment);
  if (p == NULL)
    return ENOMEM;
  AlignedBlock *b = (AlignedBlock *)malloc(sizeof(AlignedBlock));
  if (b == NULL) {
    _aligned_free(p);
    return ENOMEM;
  }
  b->ptr = p;
  b->alignment = alignment;
  b->size = size;
  AcquireSRWLockExclusive(&g_aligned_lock);
  b->next = g_aligned;
  g_aligned = b;
  ReleaseSRWLockExclusive(&g_aligned_lock);
  *memptr = p;
  return 0;
}

size_t malloc_usable_size(void *p) {
  if (p == NULL)
    return 0; /* glibc's answer; _msize(NULL) would fault */
  AcquireSRWLockShared(&g_aligned_lock);
  for (AlignedBlock *b = g_aligned; b != NULL; b = b->next) {
    if (b->ptr == p) {
      size_t n = _aligned_msize(p, b->alignment, 0);
      ReleaseSRWLockShared(&g_aligned_lock);
      return n;
    }
  }
  ReleaseSRWLockShared(&g_aligned_lock);
  return _msize(p);
}

/* turf_rt_checked_free()'s first question on Windows: was this block from the
 * aligned allocator? If so it has been released here (the caller must not call
 * free()); returns 1. Otherwise 0, and the caller frees it as usual. */
int turf_win32_free_aligned(void *p) {
  if (p == NULL)
    return 0;
  AcquireSRWLockExclusive(&g_aligned_lock);
  AlignedBlock **link = &g_aligned;
  while (*link != NULL) {
    if ((*link)->ptr == p) {
      AlignedBlock *b = *link;
      *link = b->next;
      ReleaseSRWLockExclusive(&g_aligned_lock);
      _aligned_free(p);
      free(b);
      return 1;
    }
    link = &(*link)->next;
  }
  ReleaseSRWLockExclusive(&g_aligned_lock);
  return 0;
}

int setenv(const char *name, const char *value, int overwrite) {
  if (name == NULL || *name == '\0' || strchr(name, '=') != NULL || value == NULL) {
    errno = EINVAL;
    return -1;
  }
  if (!overwrite && getenv(name) != NULL)
    return 0;
  return _putenv_s(name, value) == 0 ? 0 : -1;
}

#else
/* An empty translation unit is not valid ISO C; this file is Windows-only. */
typedef int turfrt_win32_unused_t;
#endif /* _WIN32 */
