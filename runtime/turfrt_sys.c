/*
 * runtime/turfrt_sys.c -- REL-008 stage 5 (docs/planning/ROADMAP_v2.0.0.md).
 *
 * Two small system primitives whose C-library spelling differs across platforms
 * in ways that change their BEHAVIOUR, not just their name, found by running the
 * test suite on Windows (lib/std/time/time.tr and lib/std/random/random.tr were
 * written against glibc and said so):
 *
 *   turfrt_clock_us()    the process's CPU time, in microseconds.
 *                      time.tr hard-codes CLOCKS_PER_SEC = 1_000_000 and calls
 *                      libc's clock(). That is true of glibc, where clock() is
 *                      CPU time in microseconds. On Windows clock() is
 *                      WALL-CLOCK time in milliseconds (CLOCKS_PER_SEC is 1000),
 *                      so elapsed_ms() came out a thousand times too small, and
 *                      wall time, not CPU time, was being measured. This returns
 *                      CPU time in microseconds everywhere, so the constant and
 *                      the documented meaning are true on every platform.
 *
 *   turfrt_random_u31()  a pseudo-random value in [0, 2^31 - 1].
 *   turfrt_random_reseed()
 *                      random.tr was built on libc's rand(), reseeding it from
 *                      the clock on every call because the language has no
 *                      mutable module-level state to hold "already seeded". That
 *                      worked only where clock() advanced between two calls a few
 *                      microseconds apart; with a millisecond clock a loop of
 *                      1000 calls got one seed and one value 1000 times. And
 *                      rand()'s range is RAND_MAX, which is 2^31 - 1 on glibc but
 *                      32767 on Windows, while random.tr exported it as the
 *                      former. The runtime CAN hold state (it owns the
 *                      process-wide things, FREEZE.md 6.8 B2), so the generator
 *                      lives here: seeded once from the operating system's
 *                      entropy, then advanced, with a range that is the same
 *                      everywhere. NOT cryptographically secure, as random.tr says.
 *
 * NAMING RULE for any runtime function that Turf source binds with `extern fn`:
 * use the prefix `turfrt_`, never `turf_<module>_<name>`. The compiler mangles a
 * stdlib function `random::reseed` to `turf_random_reseed`, and an `extern fn`
 * keeps its C name verbatim, so an extern called `turf_random_reseed` IS that
 * function as far as LLVM is concerned: the first version of this file used that
 * name and the compiler (correctly, by its own rule) reported `reseed` as
 * "infinite recursion", because its only statement called what mangled to itself.
 *
 * Single-threaded use (a Turf program has one thread), so the state has no lock.
 */
#ifdef _WIN32
#define _CRT_RAND_S /* rand_s(): the C runtime's OS-entropy source; before <stdlib.h> */
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

int64_t turfrt_clock_us(void) {
#ifdef _WIN32
  /* GetProcessTimes: user + kernel, in 100 ns units. Its resolution is the
   * scheduler tick (about 15.6 ms), fine for the "how long did this CPU-bound
   * work take" use elapsed_ms() documents. */
  FILETIME created, exited, kernel, user;
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
    return 0;
  const uint64_t k = ((uint64_t)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime;
  const uint64_t u = ((uint64_t)user.dwHighDateTime << 32) | user.dwLowDateTime;
  return (int64_t)((k + u) / 10);
#else
  struct timespec ts;
  if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0)
    return (int64_t)((double)clock() * 1000000.0 / (double)CLOCKS_PER_SEC);
  return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
#endif
}

static uint64_t g_rng_state = 0; /* 0 = not seeded yet; xorshift64* never reaches 0 */

/* Eight bytes from the operating system, or a time-and-address mix if it will
 * not give any. Never returns 0. */
static uint64_t turf_entropy64(void) {
  uint64_t v = 0;
  int ok = 0;
#ifdef _WIN32
  unsigned int a = 0, b = 0;
  if (rand_s(&a) == 0 && rand_s(&b) == 0) {
    v = ((uint64_t)a << 32) | b;
    ok = 1;
  }
#else
  FILE *f = fopen("/dev/urandom", "rb");
  if (f != NULL) {
    ok = fread(&v, sizeof v, 1, f) == 1;
    fclose(f);
  }
#endif
  if (!ok) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    v = ((uint64_t)ts.tv_sec << 32) ^ (uint64_t)ts.tv_nsec ^ (uint64_t)(uintptr_t)&v ^ (uint64_t)clock();
  }
  return v != 0 ? v : 0x9E3779B97F4A7C15ull;
}

void turfrt_random_reseed(void) { g_rng_state = turf_entropy64(); }

/* xorshift64* (Vigna): small, fast and well distributed for a non-security PRNG. */
int64_t turfrt_random_u31(void) {
  if (g_rng_state == 0)
    g_rng_state = turf_entropy64();
  uint64_t x = g_rng_state;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  g_rng_state = x;
  return (int64_t)((x * 0x2545F4914F6CDD1Dull) >> 33); /* 64 - 33 = the top 31 bits */
}
