/*
 * runtime/turfrt_signal.c -- FDN-006 stage 1
 * (docs/planning/ROADMAP_v2.0.0.md): turns a stack overflow's bare SIGSEGV
 * (audit F-20: exit 139, no message) into the same documented
 * `panic: ...` / abort() (SIGABRT, exit 134) shape every other runtime
 * safety check already produces, and gives every OTHER SIGSEGV (a real
 * memory-safety bug in `unsafe` code, or in the compiler/runtime itself) a
 * message that says so instead of a bare crash.
 *
 * Installed once, from turf_rt_init() (runtime/turfrt_alloc.c), before a
 * single line of Turf-level `main` runs -- same ordering guarantee
 * `turf_gc_record_stack_base()` already relies on (INV-RT-1).
 *
 * ─── Why an alternate signal stack ─────────────────────────────────────
 * The whole point of this handler is to run WHILE the thread's normal stack
 * has just been exhausted -- delivering SIGSEGV on that same, already-full
 * stack would immediately fault again trying to push the handler's own
 * frame, silently reverting to the bare crash this file exists to prevent.
 * `sigaltstack()` gives the signal delivery mechanism a separate, small,
 * always-available region to run the handler on, entirely independent of
 * how exhausted the thread's real stack is.
 *
 * ─── Why the guard-page classification is a bounded WINDOW, not an exact
 * boundary ─────────────────────────────────────────────────────────────
 * `RLIMIT_STACK` gives the thread's configured stack size; combined with
 * `g_stack_base` (turfrt_gc.c, the stack's high end) that gives a
 * *computed* low limit. The real kernel-enforced guard region sits at or
 * near that computed address, but a single stack frame that pushes far
 * past the limit in one instruction (a large local array, deep inlining)
 * can fault more than a few bytes beyond it -- `TURF_STACK_GUARD_WINDOW`
 * below is a generous fixed margin on both sides of the computed limit,
 * chosen the same way this codebase already treats every other
 * classification pass that can't be made exact (EscapePass, FDN-002):
 * never claim "stack overflow" for a fault this can't reasonably attribute
 * to the stack limit (no false positive misdiagnosing a real memory-safety
 * bug as "just recursion"), but be generous enough that a real overflow is
 * not missed and misreported as a generic, unhelpful "segmentation fault"
 * (a documented, honest false-negative-leaning gap, not a bug to "tighten"
 * without re-testing `39_nullable_references`-style deep-recursion cases
 * across the frame sizes this compiler actually generates).
 *
 * ─── Why `getrlimit`/`RLIM_INFINITY` degrades to "never classify as
 * overflow" ─────────────────────────────────────────────────────────────
 * A thread with an unlimited or unqueryable stack limit gives this file no
 * address to compare against at all -- reporting EVERY SIGSEGV as "stack
 * overflow" in that case would misdiagnose a real memory-safety bug just as
 * badly as the reverse. Every SIGSEGV degrades to the generic
 * "segmentation fault" message instead, which is still strictly better than
 * this group's own starting point (a bare, message-less crash).
 *
 * ─── Signal-handler safety, disclosed ───────────────────────────────────
 * Both branches below call functions (`turf_rt_panic`, `fprintf`) that are
 * not on POSIX's async-signal-safe list -- `abort()` itself IS safe to call
 * from a signal handler (POSIX.1-2008), but the stdio calls that build the
 * message first are not, strictly. This is a deliberate, accepted trade-off
 * the same way libturfrt's other panics already are: the process is about
 * to terminate either way, and the alternative (a hand-rolled
 * `write(2)`-only formatter, no floating-point/pointer-to-string via
 * stdio) would need its own integer/pointer-to-hex formatter duplicated
 * from nothing. The one real hazard this accepts: if the fault happened
 * while the crashing thread already held stdio's internal FILE lock (mid
 * `printf`), the handler's own `fprintf`/`turf_rt_panic` call could
 * deadlock rather than print -- in which case the process still terminates
 * (no output, but no worse than the pre-FDN-006 bare SIGSEGV), it simply
 * loses the diagnostic message on that specific interleaving. Recorded here
 * rather than silently assumed away; see FDN-006.md's *Known pitfalls*.
 */

/* <ucontext.h>'s REG_RBP / REG_RSP names are glibc extensions; must precede
 * every include. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "turfrt.h"
#include "turfrt_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * FDN-005 stage 3 (docs/planning/ROADMAP_v2.0.0.md): same detection as
 * turfrt_gc.c's own TURF_GC_ASAN (kept as an independent, duplicated
 * three-line check rather than a shared header symbol -- this is a
 * compiler-provided macro test, not a value either file computes, so
 * there is nothing to keep "in sync" by sharing it). See
 * turf_rt_install_segv_handler()'s own comment below for why this
 * matters here specifically.
 */
#if defined(__SANITIZE_ADDRESS__)
#define TURF_SIGNAL_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define TURF_SIGNAL_ASAN 1
#endif
#endif

#ifndef TURF_SIGNAL_ASAN /* only the fault handlers use the depth count */
/*
 * RT-6 (issue #300): how many calls deep the stack was when it ran out, so
 * "unbounded recursion" is a number a user can compare with what they
 * expected (a recursion meant to go 1000 deep that reports 130000 has lost
 * its base case; one that reports 90000 for a legitimately deep input needs
 * a loop or a collection).
 *
 * Counted by walking the saved-frame-pointer chain from the faulting
 * context, which is why codegen gives every Turf function the
 * `"frame-pointer"="all"` attribute (CodegenFunc.cpp): without it an
 * optimized build keeps no chain to walk, and nothing else in a process
 * with no unwinder can recover a depth after the fact. The count is
 * "about": inlining merges frames, and C code between Turf frames (libc,
 * the runtime) is built without frame pointers and breaks the chain, so it
 * can only undercount, never invent frames. Every link is checked to lie
 * on the stack and move strictly up, so a clobbered frame-pointer register
 * (a libc leaf function is allowed to use it as a scratch register) ends
 * the walk instead of reading outside the stack. Returns 0 when no frame
 * validated, and the caller then prints the message without a depth rather
 * than a wrong one.
 */
static unsigned long long turf_rt_walk_frames(uintptr_t fp, uintptr_t sp,
                                              uintptr_t stack_base) {
  if (stack_base == 0)
    return 0;
  unsigned long long depth = 0;
  while (fp >= sp && fp + 2 * sizeof(uintptr_t) <= stack_base &&
         (fp & (sizeof(uintptr_t) - 1)) == 0) {
    uintptr_t next = ((uintptr_t *)fp)[0];
    uintptr_t ret = ((uintptr_t *)fp)[1];
    if (ret == 0)
      break;
    depth++;
    if (next <= fp)
      break;
    fp = next;
  }
  return depth;
}

/* The text both platforms print; `depth` 0 means unknown. Static: the
 * handler runs on a small alternate stack (POSIX) or on what the thread has
 * left (Windows), neither of which is the place for a 160-byte buffer. */
static const char *turf_rt_overflow_message(unsigned long long depth) {
  static char msg[160];
  if (depth > 0)
    snprintf(msg, sizeof(msg),
             "the call stack exceeded its limit at about %llu nested calls "
             "-- check for unbounded or missing-base-case recursion",
             depth);
  else
    snprintf(msg, sizeof(msg),
             "the call stack exceeded its limit -- check for unbounded or "
             "missing-base-case recursion");
  return msg;
}
#endif /* !TURF_SIGNAL_ASAN */

#if !defined(_WIN32)
#include <signal.h>
#include <sys/resource.h>

/* Well above MINSIGSTKSZ/SIGSTKSZ on every platform this project targets
 * (Tier 1/2, ROADMAP_v2.0.0.md DEC-2) -- a fixed constant rather than the
 * runtime `sysconf`-based SIGSTKSZ some libcs now define, so this is a
 * plain static array, not a malloc'd buffer this file would need to
 * remember to free (it never does; it lives for the process's whole
 * life). */
#ifndef TURF_SIGNAL_ASAN
#define TURF_ALTSTACK_SIZE (64 * 1024)
static unsigned char g_altstack[TURF_ALTSTACK_SIZE];
#endif

#define TURF_STACK_GUARD_WINDOW ((uintptr_t)(256 * 1024))

#if defined(__linux__)
#include <ucontext.h>
#endif

#ifndef TURF_SIGNAL_ASAN /* only the handler below uses these */
/* The frame and stack pointers of the faulting context, or 0 where this
 * platform's register layout is not handled (the message then has no depth). */
static int turf_rt_fault_regs(void *ucontext, uintptr_t *fp, uintptr_t *sp) {
  if (!ucontext)
    return 0;
#if defined(__linux__) && defined(__x86_64__)
  ucontext_t *uc = (ucontext_t *)ucontext;
  *fp = (uintptr_t)uc->uc_mcontext.gregs[REG_RBP];
  *sp = (uintptr_t)uc->uc_mcontext.gregs[REG_RSP];
  return 1;
#elif defined(__linux__) && defined(__aarch64__)
  ucontext_t *uc = (ucontext_t *)ucontext;
  *fp = (uintptr_t)uc->uc_mcontext.regs[29];
  *sp = (uintptr_t)uc->uc_mcontext.sp;
  return 1;
#elif defined(__APPLE__) && defined(__aarch64__)
  ucontext_t *uc = (ucontext_t *)ucontext;
  *fp = (uintptr_t)uc->uc_mcontext->__ss.__fp;
  *sp = (uintptr_t)uc->uc_mcontext->__ss.__sp;
  return 1;
#elif defined(__APPLE__) && defined(__x86_64__)
  ucontext_t *uc = (ucontext_t *)ucontext;
  *fp = (uintptr_t)uc->uc_mcontext->__ss.__rbp;
  *sp = (uintptr_t)uc->uc_mcontext->__ss.__rsp;
  return 1;
#else
  (void)fp;
  (void)sp;
  return 0;
#endif
}

static unsigned long long turf_rt_count_frames(void *ucontext,
                                               uintptr_t stack_base) {
  uintptr_t fp, sp;
  if (!turf_rt_fault_regs(ucontext, &fp, &sp))
    return 0;
  return turf_rt_walk_frames(fp, sp, stack_base);
}
#endif /* !TURF_SIGNAL_ASAN */

/* Never referenced under TURF_SIGNAL_ASAN (see
 * turf_rt_install_segv_handler()'s own comment below) -- guarded out
 * entirely rather than left as dead code a -Wunused-function build would
 * otherwise flag. */
#ifndef TURF_SIGNAL_ASAN
static void turf_rt_segv_handler(int sig, siginfo_t *info, void *ucontext) {
  uintptr_t fault_addr = (uintptr_t)info->si_addr;
  uintptr_t stack_base = turf_gc_get_stack_base();
  int is_stack_overflow = 0;

  if (stack_base != 0 && fault_addr != 0) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY &&
        stack_base > (uintptr_t)rl.rlim_cur) {
      uintptr_t stack_limit = stack_base - (uintptr_t)rl.rlim_cur;
      uintptr_t window_lo = (stack_limit > TURF_STACK_GUARD_WINDOW)
                                 ? stack_limit - TURF_STACK_GUARD_WINDOW
                                 : 0;
      uintptr_t window_hi = stack_limit + TURF_STACK_GUARD_WINDOW;
      if (fault_addr >= window_lo && fault_addr <= window_hi)
        is_stack_overflow = 1;
    }
  }

  if (is_stack_overflow) {
    turf_rt_panic("stack overflow",
                  turf_rt_overflow_message(
                      turf_rt_count_frames(ucontext, stack_base)),
                  NULL, 0);
  } else {
    fprintf(stderr,
            "panic: %s (this is a bug in unsafe code or in TurfLang) at "
            "address 0x%llx\n",
            sig == SIGBUS ? "bus error" : "segmentation fault",
            (unsigned long long)(uintptr_t)info->si_addr);
    fflush(stderr);
    abort();
  }
}
#endif /* !TURF_SIGNAL_ASAN */

void turf_rt_install_segv_handler(void) {
#ifdef TURF_SIGNAL_ASAN
  /*
   * FDN-005 stage 3: under ASan, deliberately install NOTHING here, ever
   * -- ASan's own runtime already installs its own, far more capable
   * SIGSEGV handler (a real, per-fault stack-overflow AND generic
   * invalid-access report, with a full stack trace and shadow-memory
   * dump) from its process-startup constructor, which runs BEFORE
   * turf_rt_init() ever gets a chance to call this function.
   * sigaction(SIGSEGV, ...) unconditionally REPLACES whatever handler was
   * previously installed for that signal -- there is no "chain to the
   * previous handler" here, so installing ours would silently discard
   * ASan's, turning every ASan-detected fault into this file's own
   * generic "segmentation fault at address ..." line with none of ASan's
   * own diagnostic detail. Found live, the first time an ASan-built
   * runtime's SIGSEGV path was ever exercised: a real fault was caught
   * and reported by THIS file's handler instead of ASan's, discarding
   * exactly the extra detail an ASan build exists to provide. An
   * ASan-built binary's stack-overflow/generic-SIGSEGV reporting is
   * strictly a downgrade path relative to non-ASan (INV-RT-5 only
   * "holds" for a non-ASan build -- see that invariant's own row), never
   * an upgrade, so this is an accepted, documented trade-off of choosing
   * to build with TURF_RT_SANITIZE, not a regression to fix.
   */
  return;
#else
  stack_t ss;
  ss.ss_sp = g_altstack;
  ss.ss_size = sizeof(g_altstack);
  ss.ss_flags = 0;
  if (sigaltstack(&ss, NULL) != 0)
    return; /* Best-effort: without an altstack the handler can't safely run
              * on a stack that's already exhausted -- leave SIGSEGV at its
              * default disposition (today's bare-crash behaviour) rather
              * than risk a second fault delivering INSIDE the handler. */

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = turf_rt_segv_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigaction(SIGSEGV, &sa, NULL);
  /* macOS reports many protection faults -- including, depending on the
   * version, a hit on the main thread's stack guard -- as SIGBUS rather
   * than SIGSEGV (Rust's and Go's runtimes handle both for this reason).
   * Without this, a stack overflow there would still be a bare
   * "Bus error: 10" crash, the exact F-20 shape FDN-006 removed on Linux.
   * Linux raises SIGBUS only for things like a truncated mmap'd file,
   * which the generic branch reports as "bus error". */
  sigaction(SIGBUS, &sa, NULL);
#endif
}

#else /* _WIN32 */

/*
 * REL-008 stage 5. A Windows stack overflow is not a signal: it is a structured
 * exception, EXCEPTION_STACK_OVERFLOW, raised on the thread whose stack just ran
 * out. A VECTORED exception handler sees it first, process-wide, with no
 * try/except frame needed in compiled Turf code. It must run on what the thread
 * has left, so SetThreadStackGuarantee() reserves extra stack for exactly this
 * (the Windows counterpart of POSIX's sigaltstack): the guarantee is per thread,
 * and this is installed from the main thread (turf_rt_init, before `main`), which
 * is the only thread a Turf program has.
 *
 * Both outcomes print what the POSIX handler above prints, word for word, so a
 * program's behaviour (and a test's EXPECT-RUNTIME text) is the same on every
 * platform: a stack overflow is the documented `panic: stack overflow`, any
 * other access violation says it is a bug in unsafe code or in TurfLang. Both
 * end in abort() (SIGABRT, status 134 in the test runner's vocabulary).
 *
 * abort() and a crash would otherwise open Windows' error-reporting dialog and
 * wait for a click, which a headless runner never gets; the error mode and the
 * abort behaviour are set so a crash is just a process that ends.
 */
#include <signal.h>
#include <stdlib.h>
#include <windows.h>

static LONG WINAPI turf_rt_veh(EXCEPTION_POINTERS *ep) {
  const DWORD code = ep->ExceptionRecord->ExceptionCode;
  if (code == EXCEPTION_STACK_OVERFLOW) {
    unsigned long long depth = 0;
#if defined(_M_X64) || defined(__x86_64__)
    /* The x64 frame-pointer chain, as on POSIX (RT-6); other Windows
     * targets print the message without a depth. */
    ULONG_PTR lo = 0, hi = 0;
    GetCurrentThreadStackLimits(&lo, &hi);
    depth = turf_rt_walk_frames((uintptr_t)ep->ContextRecord->Rbp,
                                (uintptr_t)ep->ContextRecord->Rsp,
                                (uintptr_t)hi);
#endif
    turf_rt_panic("stack overflow", turf_rt_overflow_message(depth), NULL, 0);
  } else if (code == EXCEPTION_ACCESS_VIOLATION) {
    /* ExceptionInformation[0] is read/write/execute, [1] the faulting address. */
    void *addr = ep->ExceptionRecord->NumberParameters >= 2
                     ? (void *)ep->ExceptionRecord->ExceptionInformation[1]
                     : NULL;
    fprintf(stderr,
            "panic: segmentation fault (this is a bug in unsafe code or in "
            "TurfLang) at address 0x%llx\n",
            (unsigned long long)(uintptr_t)addr);
    fflush(stderr);
    abort();
  }
  return EXCEPTION_CONTINUE_SEARCH; /* not ours: let it take its course */
}

/* The status a Turf program that called abort() ends with on Windows. The C runtime's
 * own abort() exits with 3, which is also what `fn main(): int { return 3; }` exits
 * with, so a test runner could not tell a panic from an ordinary exit code. This is
 * in the NTSTATUS "customer" range (bit 29 set), which no program returns on purpose.
 * src/tooling/TestRunner.cpp (WindowsExitStatus) and src/driver/main.cpp (turfc's own abort handler)
 * spell the same number; change all three together. */
#define TURF_WIN_ABORT_STATUS 0xE0545254u

static void turf_rt_on_sigabrt(int sig) {
  (void)sig;
  /* TerminateProcess, not exit(): abort() means the process state is not trusted, so
   * no atexit handlers or stdio flush. stderr is unbuffered, the message is out. */
  TerminateProcess(GetCurrentProcess(), TURF_WIN_ABORT_STATUS);
}

void turf_rt_install_segv_handler(void) {
  ULONG guarantee = 128 * 1024;
  SetThreadStackGuarantee(&guarantee); /* best effort: a failure only shrinks the room */
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  signal(SIGABRT, turf_rt_on_sigabrt);
  AddVectoredExceptionHandler(1, turf_rt_veh);
}

#endif
