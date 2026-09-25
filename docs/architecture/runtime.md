# The Runtime: `libturfrt`

Internals reference for `libturfrt` — the small C static library every
compiled Turf binary links, introduced by RT-001
(`docs/planning/ARCHITECTURE_OVERHAUL.md` §8). Like the other docs in this
directory, this is grounded directly in the current source, not a planning
document — `runtime/turfrt.h`'s own doc comments are the closer, line-level
reference; this document is the "why does this exist and how does it fit
into the rest of the compiler" companion to it.

Related docs:
- `docs/architecture/compiler_pipeline.md` — the five-pass pipeline this
  library's two hooks (the `main` wrapper, panic lowering) attach to.
- `docs/architecture/memory_model.md` — what Turf's memory model looks like
  *today*; `libturfrt` is the beginning of changing that (§4 below), not a
  change to it yet. `runtime/turfrt.h`'s `TurfTypeDesc` doc comment is the
  authoritative reference for what MEM-002 will eventually consume.
- `include/Intrinsics.h` — the closed, enumerated list this library's own
  public symbols are part of (F-STD-2).

---

## 1. Why a runtime exists at all

Before RT-001, TurfLang had none (`docs/architecture/memory_model.md` §1.7,
"no automatic memory management of any kind"; the old §1.10 of this
directory's sibling doc described it plainly as "no `libturfrt`, no `.c`/
`.o` runtime sources, no linked support library"). Every panic was an inline
`fprintf`+`abort()` built directly into whatever function's Codegen needed
one; every allocation was libc `malloc` reached through an ordinary
`extern fn`.

Rule B2 (`docs/development/FREEZE.md` §6.8, `ARCHITECTURE_OVERHAUL.md`
§3.3): **the runtime owns anything that must be true across the whole
process and cannot be re-derived per translation unit** — the heap, the
collector, the root set, panic behaviour, the OOM policy. TurfLang never
separately compiles (every `import` is re-parsed and re-codegenned into the
same module, §27 D7 in the blueprint), so until MEM-002's collector exists
this rule has had nothing to attach to. RT-001 is the scaffolding: a real C
library, versioned lock-step with the compiler, linked into every binary,
with exactly two things in it today (a fail-fast allocator wrapper, and
consistent panic formatting) plus the type-descriptor infrastructure
MEM-002's precise heap tracing will need. Nothing here is a place to move
standard-library functionality into (F-STD-3, Rule B3) — `lib/std/mem/mem.tr`'s
`malloc`/`free` bindings are untouched; the manual heap stays manual.

## 2. What's in it

`runtime/turfrt.h` is the complete public API — four functions and one
struct:

| Symbol | What it does today | What it's for later |
|---|---|---|
| `turf_rt_init()` | No-op | MEM-002's heap/block-table setup |
| `turf_rt_shutdown()` | No-op | MEM-002's final-collection/stats hook |
| `turf_rt_alloc(bytes, desc)` | Fail-fast `malloc` wrapper | MEM-002's real GC-heap allocation entrypoint |
| `turf_rt_panic(kind, msg, file, line)` | Formats a message to stderr, `abort()`s | Unchanged — this is already its final form |
| `TurfTypeDesc` (not a function) | Emitted per aggregate type, read by nothing yet | MEM-002's mark-phase input |

Three `.c` files, split by concern the same way the compiler's own Codegen
split is: `runtime/turfrt_alloc.c` (bootstrap + allocation),
`runtime/turfrt_panic.c` (panic formatting), `runtime/turfrt_types.c` (the
one `TurfTypeDesc` accessor, `turf_rt_typedesc_is_pointer_word()`, whose
only caller today is `scripts/runtime_smoke.sh`'s own C-level check — see
that script for the currently-most-direct exercise of this library's API).

Built as `CMakeLists.txt`'s `turfrt` target: a **static** library, built
from plain C11, with **no dependency on `turfcore`/LLVM at all** — it is not
linked into the `turf` compiler binary itself. It is linked into every
*compiled Turf program*, the same relationship `-lm` already has to a
compiled binary. Static rather than shared: every compiled Turf program is
meant to be a standalone executable (this is also why `libturfrt.a`'s build
output is pointed at `<repo-root>/lib/` — see §5 below — rather than a
`lib/` subdirectory the linker would need an rpath to find at runtime).

## 3. The two compiler-side hooks

### 3.1 Panic lowering — `CodegenPanic()`, `src/Builtins.cpp`

Every one of the compiler's own runtime safety guards — division/modulo by
zero, integer overflow (SEM-002's checked arithmetic), array bounds, a bad
string→int/enum cast — funnels through one function, `CodegenPanic(Value
*MsgVal, SourceLocation Loc)`. Before RT-001 this built the
`fprintf(stderr, "%s\n", MsgVal); abort();` sequence directly, per call
site, in raw LLVM IR. It now declares (once per module) and calls
`turf_rt_panic`, passing a fixed `"runtime check"` kind literal, the
existing message string unchanged, and the Turf-level `SourceLocation`'s
file/line. `kind` is not distinct per call site — see `CodegenPanic()`'s own
doc comment for why widening its signature to carry one wasn't worth doing
in this group (all 9 call sites would need touching for a diagnostic-text
change only, no behavioural difference).

This is **not** the same path `lib/std/io/sys.tr`'s `panic()`/`assert`/`fail`
use — those are ordinary Turf functions, built on a separate `write(2)`-based
mechanism, deliberately untouched by RT-001. `CodegenPanic()` is reachable
only from the compiler's own inline safety checks, never from Turf source
under any name.

Both paths still terminate the same way: `abort()` (SIGABRT, exit 134),
which is why `scripts/test_runner.sh`'s `RUNTIME_ABORT_EXPECTED` convention
(any non-zero, non-timeout exit) needed no change at all.

### 3.2 The `main` wrapper — `WrapMainWithRuntimeBootstrap()`, `src/CodegenFunc.cpp`

Turf's `main` has always been compiled *as* the literal C-ABI `main` (no
wrapper) — `CodegenFunc.cpp`'s own long-standing comment on the
argc/argv-stashing machinery this relies on (`ArgcStorageGV`/`ArgvStorageGV`,
the `TurfRuntimeGetArgc`/`TurfRuntimeGetArgv` synthesized accessors os.tr
binds to) explains that setup in full; RT-001 does not touch any of it.

`turf_rt_init()`/`turf_rt_shutdown()` need to run before/after the user's
`main` body on every path, which means something has to sit *around* it.
Rather than thread two new calls into `FuncDefExprAST::codegen()`'s existing,
delicate `Name == "main"` branches, `WrapMainWithRuntimeBootstrap()` runs as
a **pure post-processing step** on the already-fully-built LLVM module:

1. Look up the `Function` literally named `"main"` (guaranteed to exist and
   have a body — `main.cpp`'s entry-point check already ran).
2. Rename it to `__turf_user_main` and demote it to internal linkage.
3. Declare (or reuse) `turf_rt_init`/`turf_rt_shutdown` prototypes.
4. Build a **new** function, literally named `"main"`, with the identical
   `FunctionType` the old one had (`(i32, ptr) -> ReturnType` — argc/argv
   forwarded through unchanged, opaque to this function), whose body is
   exactly: call `turf_rt_init()`; call `__turf_user_main(argc, argv)`,
   keeping its result; call `turf_rt_shutdown()`; return that result (or
   `ret void`, for the — currently untested but not rejected by the
   Resolver — case of a declared `fn main(): void`).

Called from `main.cpp`'s driver immediately after the entry-point check and
**before** `runOptimizationPipeline()`. Ordering matters: OPT-001's
Internalize pass treats `{"main"}` as the *entire* must-preserve set
(`main.cpp`'s own comment on `MustPreserveGV` — nothing else in the module
is externally reachable by name, since TurfLang never separately compiles).
Running the wrapper before optimization means the symbol actually named
`"main"` at Internalize time is the wrapper, and `__turf_user_main` keeps
its own liveness for free through the wrapper's ordinary in-module call —
no second preserve-set entry needed. At `-O0` the two functions stay
distinct (verified live, `scripts/runtime_smoke.sh` checks exactly this in
the emitted IR); at higher levels the trivial wrapper is routinely inlined
away entirely by LLVM's own inliner, which is correct and expected — the
*call sequence*, not the function boundary, is the actual contract, and
`scripts/opt_levels_smoke.sh` (exact-output-match across every optimization
level) is what proves inlining never changes observable behaviour here.

## 4. Type descriptors

For every real, fully-built LLVM aggregate type in the module, the compiler
emits a constant global mirroring `TurfTypeDesc { size, name,
pointer_word_bitmap }` (`runtime/turfrt.h`'s own doc comment has the exact
field semantics, including the 64-word/512-byte bitmap coverage limit).
This is forward infrastructure for MEM-002's precise heap tracing
(F-MEM-1: "precise on the heap" — a per-type pointer bitmap the compiler
emits is exactly what that requires) landed now, while TYPE-002's
per-instantiation struct layouts are fresh, rather than retrofitted later.
**Nothing reads a descriptor's bitmap yet** — the only consumer as of
RT-001 is `scripts/runtime_smoke.sh`'s own `--emit-llvm`-and-grep check,
exactly the verification method `ARCHITECTURE_OVERHAUL.md` §8 RT-001
prescribes for this group.

### 4.1 Why emission is a separate, late pass — `EmitAllTurfTypeDescs()`

The obvious place to build a struct's descriptor is inline, in
`StructDeclExprAST::codegen()`, right after `ST->setBody(...)` gives it a
final layout. That was the first implementation, and it crashed on a real,
reproducible case: `struct Holder { items: List<int> }`.

`main.cpp`'s struct pre-pass (sub-pass 2c) codegens every top-level
`StructDeclExprAST` in `PrePassASTs` order. A monomorphized generic
instantiation's own clone (`List_gen1_int`, from TYPE-002's monomorphizer)
is *appended* to that list, so it codegens **after** an ordinary struct
declared earlier in the source file that embeds it by value. At the moment
`Holder`'s own `StructDeclExprAST::codegen()` reaches "compute my own
size," `List<int>`'s LLVM `StructType` is still an **opaque forward stub**
(registered in `LLVMTypeCache`, no body yet) — this is completely normal
and by design (`main.cpp`'s own comment on the "lingering opaque
monomorphized stub" scan describes exactly this state as expected mid-pass).

The problem is what querying a struct's `DataLayout` size does internally:
`DataLayout::getTypeAllocSize()`/`getStructLayout()` on `Holder` needs the
size of *every field*, including the still-opaque `List<int>` — LLVM
computes (and **permanently caches**, keyed by `StructType*`, with **no
invalidation hook**) a zero-element `StructLayout` for it as a side effect.
Moments later, when `List_gen1_int`'s own clone is codegenned and given its
real body, `DataLayout::getStructLayout()` on that *exact* `StructType*`
returns the same stale, zero-element cached object —
`StructLayout::getElementOffset(0)` then asserts (`Idx < NumElements`
failed), a genuine compiler crash, confirmed live against
`tests/38_monomorphization/04_method_on_generic_struct.tr` and four sibling
tests in that folder (every one embeds a generic instantiation by value).

The fix is `EmitAllTurfTypeDescs()` (`src/CodegenStruct.cpp`, declared in
`include/Codegen.h`): a single pass over `TheModule->getIdentifiedStructTypes()`,
run exactly once, from `main.cpp`, **immediately before `verifyModule()`** —
after the struct pre-pass, after every import-triggered struct build
(sub-passes 3a/3b), after the main pass, after the existing
lingering-opaque-stub diagnostic scan. By this point every struct that will
ever exist in the module does, so `DataLayout::getStructLayout()` is never
called on any `StructType` before its own `setBody()` has run — the
ordering hazard doesn't just get worked around for the generics case, it
stops being reachable at all, for any struct-embeds-struct shape.

### 4.2 Keeping descriptors alive across optimization

A `TurfTypeDesc` global has no in-module reference — nothing calls
`turf_rt_alloc` with one yet (that's MEM-001), so at any optimization level
above `-O0`, `Internalize`+`GlobalDCE` (OPT-001) would otherwise delete
every one of them as dead. `EmitAllTurfTypeDescs()` appends each descriptor
to `@llvm.compiler.used` (`llvm::appendToCompilerUsed()`,
`llvm/Transforms/Utils/ModuleUtils.h`) — the standard LLVM idiom for "keep
this alive through the optimizer without giving it real external linkage."
Verified live (`scripts/opt_levels_smoke.sh` and a direct `--emit-llvm`
diff): every descriptor is present, byte-identical, at every optimization
level.

## 5. Build and distribution

`CMakeLists.txt`'s `turfrt` target output is pointed at `<repo-root>/lib/`
(`ARCHIVE_OUTPUT_DIRECTORY`) — the same directory `lib/std/` already lives
under. This is deliberate, not incidental: `TurfInstallRoot()`
(`src/ParserModule.cpp`, exported for this purpose — previously file-local)
is the one function both `import` and `linkObject()` (`src/main.cpp`) use to
find "this install's root" — `TURF_HOME` if set, else
`<compiler-binary-dir>/..`. A plain dev build (`build/turf`, no `TURF_HOME`
set) resolves that to the repo root, so `lib/libturfrt.a` is found with the
same zero-setup property `lib/std/` already has. `linkObject()` passes
`-L<TurfInstallRoot()>/lib -lturfrt` ahead of every other link flag.

`*.a` is already `.gitignore`d, so this build artifact living inside the
otherwise source-controlled `lib/` tree is never accidentally committed.
The release packaging action
(`.github/actions/package-release/action.yml`) needed **no changes**: it
already does a blanket `cp -r lib` into the release archive, which picks up
`lib/libturfrt.a` automatically once the build produces it there —
verified live by inspecting a local build's `lib/` directory post-build.

## 6. What this group deliberately does not do

Per RT-001's own "Must NOT reinterpret" (`ARCHITECTURE_OVERHAUL.md` §8):
the runtime is C, not TurfLang, and it is not a place to move standard-library
functionality into. Concretely, still true after this group:

- `lib/std/mem/mem.tr`'s `malloc`/`free` bindings are unchanged — the manual
  heap stays manual (F-MEM-3). Nothing in `lib/std` calls `turf_rt_alloc`.
- No GC heap, no collector, no root set — `turf_rt_init`/`turf_rt_shutdown`
  are no-ops today. That's MEM-002.
- `TurfTypeDesc.pointer_word_bitmap` is emitted, but read by nothing except
  a smoke-test script. It is a **sound over-approximation** (every
  pointer-shaped word, not proven-GC-only words — nothing before
  MEM-001/MEM-002 tags a pointer field with which heap it points into); the
  collector consuming it precisely is future work, not a correctness gap in
  what's emitted today.
