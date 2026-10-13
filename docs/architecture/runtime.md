# The Runtime: `libturfrt`

Internals reference for `libturfrt`, the small C static library every compiled
Turf binary links. It is grounded in the current source: `runtime/turfrt.h`'s
doc comments are the line-level reference, and this document explains what the
library is for and how it fits into the compiler.

Related docs:
- `docs/architecture/compiler_pipeline.md` for the pass order the two
  compiler-side hooks (the `main` wrapper, panic lowering) attach to.
- `docs/architecture/memory_model.md` for the storage model, the two heaps and
  the collector's root discovery (§2.6). This document covers the library
  around the collector, not the collector's algorithm.
- `docs/architecture/boundary.md` for the compiler / standard library / runtime
  boundary rules and the closed list of intrinsics.
- `src/sema/Intrinsics.h`, the closed list of bare intrinsics; every runtime
  symbol Turf source can reach is named there (F-STD-2).

---

## 1. What the runtime owns

Rule B2 (`docs/development/FREEZE.md` §6.8): the runtime owns anything that
must be true across the whole process and cannot be re-derived per translation
unit. Turf never compiles modules separately (every `import` is parsed and
code-generated into the same LLVM module), so "per translation unit" is not a
useful place to keep process-wide state. The runtime therefore owns:

- the GC heap and its collector,
- the root set and the stack bounds the collector scans,
- panic formatting and the process-wide fault handlers (stack overflow,
  segmentation fault),
- the out-of-memory policy for the GC heap,
- a few platform primitives whose C spelling differs per OS (§2).

It is C, not Turf, and it is not a place to move standard-library functionality
into (F-STD-3, Rule B3). `lib/std/mem/mem.tr`'s `malloc`/`calloc`/`realloc`
bindings stay what they are: the manual heap is libc's heap (F-MEM-3).

## 2. What is in it

The library is built from plain C11 (`CMakeLists.txt`, target `turfrt`), has no
dependency on `turfcore` or LLVM, and is not linked into `turfc`. It is linked
into every compiled Turf program, the same relationship `-lm` has.

| File | Role |
|---|---|
| `runtime/turfrt.h` | The public C ABI: the entry points below and `TurfTypeDesc`. The compiler never includes it; Codegen builds the matching LLVM types by hand (`getOrCreateTurfTypeDescType()` in `src/codegen/CodegenStruct.cpp`). |
| `runtime/turfrt_internal.h` | Glue between the runtime's own `.c` files. Not part of the ABI. |
| `runtime/turfrt_alloc.c` | `turf_rt_init()`/`turf_rt_shutdown()`, the three allocation entry points, `turf_rt_checked_free()`, `turf_rt_stats()`. |
| `runtime/turfrt_gc.c` | The collector: block table, root discovery, mark, sweep, pins, the testing aids. |
| `runtime/turfrt_panic.c` | `turf_rt_panic()`. |
| `runtime/turfrt_types.c` | `turf_rt_typedesc_is_pointer_word()`, the one reader of a descriptor's bitmap. |
| `runtime/turfrt_abi.c` | The ABI stamp (§7.3). |
| `runtime/turfrt_signal.c` | The fault handlers (§7.1). |
| `runtime/turfrt_sys.c` | `turfrt_clock_us()` (CPU time in microseconds) and the process-wide random generator (`turfrt_random_u31()`, `turfrt_random_reseed()`), bound by `lib/std/time/time.tr` and `lib/std/random/random.tr`. |
| `runtime/turfrt_net.c` | Socket helpers whose behaviour differs per platform (`turfrt_net_startup`, `_socket`, `_close`, `_reuseaddr`, `_sockaddr`), bound by `lib/std/io/net.tr`. |
| `runtime/turfrt_win32.c` | On Windows only: `posix_memalign`, `malloc_usable_size`, `setenv`, defined under their POSIX names so `lib/std/mem/mem.tr` and `lib/std/os/os.tr` link unchanged, plus `turf_win32_free_aligned()`. |

Naming rule for a runtime function that Turf source binds with `extern fn`: use
the prefix `turfrt_`, never `turf_<module>_<name>`. The compiler mangles a
stdlib function `random::reseed` to `turf_random_reseed`, and an `extern fn`
keeps its C name verbatim, so the two spellings would collide.

The entry points in `runtime/turfrt.h`:

| Symbol | What it does |
|---|---|
| `turf_rt_init()` | Records the stack base (`turf_gc_record_stack_base()`), then installs the fault handlers (`turf_rt_install_segv_handler()`). |
| `turf_rt_shutdown()` | Prints the allocation statistics when `TURF_RT_STATS_DUMP` is set; otherwise nothing. There is no final collection: process exit reclaims everything. |
| `turf_rt_alloc(bytes, desc)`, `turf_rt_alloc_zeroed(bytes, desc)`, `turf_rt_realloc(p, bytes, desc)` | Collected GC-heap allocation. Each panics (`out of memory`) when the system allocator fails, so a caller never sees `NULL`. Each registers the block with the collector before the allocation-pressure check can run. |
| `turf_rt_collect()` | One stop-the-world mark-sweep cycle. |
| `turf_rt_add_global_root(slot)`, `turf_rt_remove_global_root(slot)` | Register a `void**` storage location as a precise root. Turf has no mutable global storage, so Codegen has no caller for these; they exist as tested runtime API (`scripts/smoke/gc_smoke.sh`). |
| `turf_rt_pin(p)`, `turf_rt_unpin(p)` | Pin the GC object containing `p`; pins nest. Reached from `mem::root()`/`mem::unroot()`. |
| `turf_rt_is_gc_pointer(p)` | Exact block-start test. Reached from the `gc_is_managed` intrinsic. |
| `turf_rt_checked_free(p)` | libc `free()` for the manual heap, after panicking if `p` is a live GC block (`cross-heap free`). Reached from `mem::free()` through the `gc_checked_free` intrinsic. |
| `turf_rt_stats()` | Allocation counters plus the collector's point-in-time numbers; read from C by the smoke scripts. |
| `turf_rt_panic(kind, msg, file, line)` | Formats one line to stderr and calls `abort()`. |
| `turf_rt_typedesc_is_pointer_word(desc, i)` | Bit accessor for a descriptor's pointer bitmap. |
| `turf_rt_abi_2_0()` | The ABI stamp. |

`TurfTypeDesc` is the one struct shared with the compiler (§4).

## 3. The two compiler-side hooks

### 3.1 Panic lowering: `CodegenPanic()`, `src/sema/Builtins.cpp`

Every one of the compiler's own runtime safety checks (division and modulo by
zero, checked-arithmetic overflow, array and slice bounds, a bad string to int
or enum cast) goes through `CodegenPanic(Value *MsgVal, SourceLocation Loc)`.
It declares `turf_rt_panic` once per module and calls it with the fixed kind
`"runtime check"`, the message, and the Turf source file and line. The kind is
not distinct per call site; the message carries the distinction (INV-RT-2).

This is not the path `lib/std/core/sys.tr`'s `panic()` uses. That is an
ordinary Turf function that writes to stderr with `write(2)` and calls
`abort()`. `CodegenPanic()` is reachable only from the compiler's own inline
checks, never from Turf source.

### 3.2 The `main` wrapper: `WrapMainWithRuntimeBootstrap()`, `src/codegen/CodegenFunc.cpp`

The user's `main` is compiled as the C-ABI `main`, including the
argc/argv-stashing machinery (`ArgcStorageGV`/`ArgvStorageGV` and the
`TurfRuntimeGetArgc`/`TurfRuntimeGetArgv` accessors `os.tr` binds to).
`WrapMainWithRuntimeBootstrap()` runs on the finished module, so none of that
is touched:

1. Find the function named `main`.
2. Rename it `__turf_user_main` and give it internal linkage.
3. Declare (or reuse) `turf_rt_init`, `turf_rt_shutdown` and the ABI stamp
   symbol (`RuntimeAbiSymbol`, `src/codegen/Codegen.h`).
4. Create a new `main` with the original function type whose body calls the ABI
   stamp, `turf_rt_init()`, then `__turf_user_main(argc, argv)`, then
   `turf_rt_shutdown()`, and returns the user's result. `fn main(): void` is
   accepted, so for a void `main` the wrapper is `i32 main(...)` returning 0.

The driver (`src/driver/main.cpp`) calls it after the entry-point check, after
every function is generated, and before `runOptimizationPipeline()`. Order
matters: the optimizer's Internalize pass treats `{"main"}` as the whole
must-preserve set, so the symbol named `main` at that point has to be the
wrapper; `__turf_user_main` stays alive through the wrapper's ordinary call.
At `-O0` the two functions stay distinct (`scripts/smoke/runtime_smoke.sh`
checks the call sequence in the emitted IR); at higher levels the inliner
usually folds the wrapper away. The call sequence, not the function boundary,
is the contract, and `scripts/smoke/opt_levels_smoke.sh` shows optimization
changes no observable behaviour (INV-OPT-1). INV-RT-1 states the contract:
every binary links `libturfrt` and runs `turf_rt_init()`/`turf_rt_shutdown()`
around `main`.

## 4. Type descriptors

For every fully built LLVM struct type in the module the compiler emits a
constant global mirroring the runtime's struct:

```c
typedef struct TurfTypeDesc {
  uint64_t size;                  /* allocation size in bytes */
  const char *name;               /* for diagnostics only */
  uint64_t word_count;            /* ceil(size / 8) */
  const uint64_t *pointer_words;  /* ceil(word_count / 64) words of bitmap */
} TurfTypeDesc;
```

Bit `i % 64` of `pointer_words[i / 64]` set means "bytes `[8i, 8i+8)` may hold
a pointer". The bitmap covers the type's entire layout at any size (INV-MEM-2);
`turf_rt_typedesc_is_pointer_word()` bounds-checks against `word_count`.
It is a sound over-approximation: every pointer-shaped word is marked, whether
it points into the GC heap or the manual heap, because the type system does not
tag a pointer field with its heap.

The collector reuses one descriptor for an array of elements: it walks a block
in `desc->size`-byte strides and re-applies the bitmap at each stride. A
single-struct allocation is the case where the block is exactly one stride.
`desc == NULL` means opaque (no pointers inside), which is correct for a string
buffer. Collection backing buffers get an element descriptor from
`getOrCreateElementTurfTypeDesc()` (`src/codegen/CodegenStruct.cpp`), injected
by `CastExprAST::codegen()` at a `gc_alloc(...) to T*` site.

`buildTurfTypeDescInit()` (`src/codegen/CodegenStruct.cpp`, shared by
`emitTurfTypeDesc()` and `getOrCreateElementTurfTypeDesc()`) builds the
initializer: `word_count`, a private `[N x i64]` bitmap global
(`N = ceil(word_count / 64)`) and a private name-string global. The name
globals are `@__turf_typedesc_name.<llvm struct name>` and
`@__turf_typedesc_bitmap.<llvm struct name>`; an element descriptor is
`@__turf_elemdesc`, with its own `_name`/`_bitmap` companions. An assertion at
emission (`word_count * 8 >= size`, bitmap length equals `N`) makes a
reintroduced fixed cap fail loudly in a debug build. For example, a struct with
one pointer in its third word:

```llvm
@__turf_typedesc_bitmap.WithPointer = private constant [1 x i64] [i64 4]
@__turf_typedesc_WithPointer = internal constant %TurfTypeDesc
    { i64 24, ptr @__turf_typedesc_name.WithPointer, i64 3,
      ptr @__turf_typedesc_bitmap.WithPointer }
```

A type with 70 `int` fields and then a `string` has `word_count` 71, so its
bitmap is `[2 x i64] [i64 0, i64 64]`: word 70 lands in the second `i64` at bit
6. `scripts/smoke/runtime_smoke.sh` checks both shapes with `--emit-llvm`, and
exercises a hand-built descriptor with `word_count` 130 directly in C.

### 4.1 Why emission is a late pass: `EmitAllTurfTypeDescs()`

`EmitAllTurfTypeDescs()` (`src/codegen/CodegenStruct.cpp`, declared in
`src/codegen/Codegen.h`) iterates `TheModule->getIdentifiedStructTypes()` once,
from the driver, immediately before `verifyModule()`. It cannot run inline in
`StructDeclExprAST::codegen()`. A monomorphized clone (`List_gen1_int`) is
appended to the pre-pass list, so it is generated after an ordinary struct
declared earlier that embeds it by value (`struct Holder { items: List<int> }`).
While generating `Holder`, asking LLVM's `DataLayout` for its size also computes
a `StructLayout` for the still-opaque `List_gen1_int`, and `DataLayout` caches
that zero-element layout against the `StructType*` with no invalidation hook.
When the clone later gets its real body, `getStructLayout()` returns the stale
object and `getElementOffset(0)` asserts. Emitting every descriptor after all
struct-building sub-passes (the struct pre-pass, the import-triggered struct
builds, the main pass, and the driver's scan for a lingering opaque
monomorphized stub) means `getStructLayout()` is never called on a struct
before its own `setBody()`. INV-RT-3 records the ordering guarantee.

A function body that needs a named struct's descriptor before that pass (to
pass to `turf_rt_alloc`) calls `getOrForwardDeclareTurfTypeDescGlobal()`, which
creates a forward declaration that `emitTurfTypeDesc()` later fills in, so both
orders produce the same single global.

### 4.2 Surviving optimization

Nothing in the module references a descriptor global unless a call site passes
it, and at any level above `-O0` Internalize plus GlobalDCE would delete the
unreferenced ones. `emitTurfTypeDesc()` and `getOrCreateElementTurfTypeDesc()`
append each descriptor to `@llvm.compiler.used` (`llvm::appendToCompilerUsed()`,
`llvm/Transforms/Utils/ModuleUtils.h`), the standard way to keep a global alive
through the optimizer without external linkage.

### 4.3 Descriptor names carry the owning module

A descriptor is named after its LLVM struct, and a module-owned type's LLVM
struct is named `module.Name` (`TypeIdentity::llvmName()`, `src/sema/Types.h`).
`list`'s `List<int>` is `@__turf_typedesc_list.List_gen1_int` and its `name`
string is `"list.List_gen1_int"`. A type declared in the main program keeps its
bare name. Two modules may each declare a `Node` without their descriptors
colliding. Nothing in the runtime parses the name.

## 5. Build and distribution

`CMakeLists.txt` points the `turfrt` archive's output at `<repo-root>/lib/`,
next to `lib/std/`. `TurfInstallRoot()` (`src/frontend/ParserModule.cpp`,
declared in `src/frontend/Parser.h`) is the single function both `import` and
`linkObject()` (`src/driver/main.cpp`) use to find the install root:
`TURF_HOME` if set, else `<compiler-binary-dir>/..`. A dev build therefore
finds `lib/libturfrt.a` with no setup. `*.a` is git-ignored, and the release
packaging copies `lib/` wholesale, which picks the archive up.

`linkObject()` links in one of two ways:

- With the system toolchain: `cc` (`gcc` on Windows) with
  `-L<root>/lib -lturfrt -lm`, ahead of any library the program names with
  `extern from` or `--link`. On Windows it adds `-Wl,--stack,8388608` (an 8 MB
  main stack, as on Linux) and `-lws2_32`, since an archive member's own
  library needs are not recorded in it.
- With the linker built into `turfc`, when the program names no system library
  beyond the C runtime, the release archive ships `lib/rt/<platform>/`, and
  `TURF_LINKER=system` is not set. It links statically against that bundled C
  runtime and `libturfrt.a`.

Either way the archive must exist: if `<root>/lib/libturfrt.a` is missing,
`linkObject()` reports `TurfLang runtime library not found at '<path>'` and a
`TURF_HOME` hint before invoking any linker (INV-RT-7). `turfc --version`
prints the runtime ABI stamp (`runtime turf_rt_abi_2_0`), the install root and
which linker this install uses.

A `turfc` built with `-DTURF_RT_SANITIZE=ON` links every program it compiles
against `libturfrt_asan.a`, an ASan+UBSan build of the same sources (target
`turfrt_asan`), and always uses the system linker. Under ASan the runtime
installs no fault handler (§7.1) and the conservative scan skips ASan-poisoned
words (`memory_model.md` §2.6.3).

## 6. What the runtime deliberately does not do

- It does not manage the manual heap. `mem::alloc` and friends are libc, and
  nothing in `lib/std` calls `turf_rt_alloc` except through the `gc_*`
  intrinsics (F-MEM-3).
- It does not trace into manual-heap memory. A GC pointer stored only in a
  `mem::alloc` block is not a root unless the program pins it with
  `mem::root()` (`memory_model.md` §1.7).
- It is single-threaded and stop-the-world, with no finalizers (F-MEM-2) and no
  final collection at shutdown.

## 7. The runtime contract

What a user sees when something goes wrong, and what stops a binary from
running against the wrong runtime.

### 7.1 Stack overflow and other faults

`turf_rt_install_segv_handler()` (`runtime/turfrt_signal.c`) is called from
`turf_rt_init()` right after the stack base is recorded.

**POSIX (Linux, macOS).** It installs a `sigaltstack`-backed handler for
`SIGSEGV` and `SIGBUS` (macOS reports many protection faults, the stack guard
among them, as `SIGBUS`). The alternate stack is a fixed 64 KiB static buffer:
the handler must run after the thread's own stack is exhausted, and delivering
the signal on it would fault again. If `sigaltstack()` fails, the handler is not
installed and the process keeps its default disposition.

The handler classifies the fault. `getrlimit(RLIMIT_STACK)` and
`turf_gc_get_stack_base()` (the stack's high end, shared with the collector, not
a second copy) give a computed low limit. A fault address within a fixed
256 KiB window on either side of it is a stack overflow; anything else, and any
case where `RLIMIT_STACK` is unlimited or unqueryable, is a generic fault. The
window errs toward "generic": the handler never calls a real memory-safety bug
"just recursion".

A classified overflow goes through `turf_rt_panic("stack overflow", ...)`, so it
ends exactly like every other panic. Any other fault prints
`panic: segmentation fault (this is a bug in unsafe code or in TurfLang) at address <addr>`
(`bus error` for `SIGBUS`) and calls `abort()`.

The handler calls `fprintf`, which is not async-signal-safe (only `abort()`
is). That is accepted deliberately, since the process is terminating either way.
The one hazard is a fault that arrives while the crashing thread holds stdio's
lock: the message can be lost, and the process still terminates.

**Under ASan** `turf_rt_install_segv_handler()` installs nothing. `sigaction`
replaces rather than chains, and ASan's own handler, installed from its startup
constructor, reports far more. INV-RT-5 therefore holds for non-ASan builds
only, which is why the ASan CI job skips
`tests/41_gc/fail_stack_overflow_deep_recursion.tr`.

**Windows.** A stack overflow is a structured exception, not a signal.
`turf_rt_install_segv_handler()` reserves 128 KiB of guaranteed stack with
`SetThreadStackGuarantee()` (the counterpart of `sigaltstack`), installs a
vectored exception handler (`turf_rt_veh`) that prints the same two messages as
the POSIX handler, and sets the error mode and abort behaviour so a crash does
not open the error-reporting dialog. It also hooks `SIGABRT` to
`TerminateProcess` with status `0xE0545254`, because the C runtime's own
`abort()` exits with 3, which an ordinary `return 3` also produces. The test
runner (`src/tooling/TestRunner.cpp`) and `turfc` itself spell the same number;
the three places change together. The runner reports that status as 134.

### 7.2 Out of memory

`turf_rt_alloc`, `turf_rt_alloc_zeroed` and `turf_rt_realloc`
(`runtime/turfrt_alloc.c`) call `turf_rt_panic("out of memory", ...)` the moment
the system allocator returns `NULL`, with the descriptor's name (or `<raw>`) as
the message. The collector's own bookkeeping arrays do the same. No caller ever
sees a silent `NULL` from the GC heap. The manual heap has its own, separately
implemented policy: `mem::alloc` and friends call `sys::panic()` on `NULL`
(`lib/std/mem/mem.tr`). Both fail loudly, but they are different code (F-MEM-3).
`tests/41_gc/fail_oom_huge_direct_allocation.tr` and
`scripts/smoke/runtime_smoke.sh` exercise the path with one `gc_alloc()` call
sized at the largest `int`, which no allocator can satisfy.

### 7.3 The compiler/runtime ABI stamp

`runtime/turfrt_abi.c` defines `turf_rt_abi_2_0(void)`, which does nothing.
`WrapMainWithRuntimeBootstrap()` calls it from every binary's `main` wrapper
(the name is held in `RuntimeAbiSymbol`, `src/codegen/Codegen.h`). The effect is
entirely at link time: an object compiled by this compiler linked against a
`libturfrt.a` that lacks the symbol fails with
`undefined reference to 'turf_rt_abi_2_0'`, a name that says what is wrong,
instead of linking and misbehaving against a runtime whose `TurfTypeDesc`
layout or entry-point signatures it no longer agrees with. A change to that ABI
bumps the suffix (`_2_0` to `_2_1`, ...) in both places together (INV-RT-6).

### 7.4 Panic message shapes and exit codes

| Termination | Message | Exit code |
|---|---|---|
| A compiler-inserted runtime check (division by zero, overflow, array bounds, ...) | `panic: runtime check: <msg> (<file>:<line>)` | 134 (`SIGABRT`) |
| Stack overflow | `panic: stack overflow: <msg> (<runtime>:0)` | 134 |
| Any other `SIGSEGV` (and `SIGBUS` on POSIX) | `panic: segmentation fault (this is a bug in unsafe code or in TurfLang) at address <addr>` | 134 |
| Out of memory, GC heap | `panic: out of memory: <desc or <raw>> (<site>:0)` | 134 |
| Out of memory, manual heap | `sys::panic()`'s own format | 134 |
| A cross-heap `mem::free()`, an unbalanced `mem::unroot()`, an undiscoverable stack | `panic: <kind>: <msg> (<site>:0)` | 134 |
| `TURF_GC_VALIDATE` finding a missed root | `panic: GC root missed: ...` | 134 |
| `lib/std/core/sys.tr`'s `panic()`/`assert`/`fail` | that function's own message | 134 |
| `sys::exit(n)` | none | `n` |

Every panic path ends in `abort()`. On Windows the same termination surfaces as
status `0xE0545254` (see §7.1), which the test runner reports as 134. Only
`sys::exit(n)` is a deliberate, non-panic exit with a caller-chosen code.
