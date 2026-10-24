/**
 * @file Intrinsics.h
 * @brief The closed, enumerated list of every symbol the compiler treats
 *        specially -- F-STD-2 (FREEZE.md §6.8): "the intrinsic/builtin list
 *        is closed and enumerated in one place. Adding an intrinsic is an
 *        explicit, change-recorded act." Introduced by RT-001
 *        (docs/archive/ARCHITECTURE_OVERHAUL.md §8), which needed a single
 *        place to name the new `turf_rt_*` runtime entrypoints alongside
 *        the compiler-level intrinsics that already existed with no such
 *        registry.
 *
 * @details This header is documentation-as-code, with ONE piece of code in
 *          it: `BareIntrinsic` / `LookupIntrinsic()` just below, the single
 *          place a bare intrinsic's NAME is compared. The Resolver calls it
 *          only for a bare call that no declared function matched, and
 *          records the answer on the call node (`FuncCallExprAST::
 *          ResolvedIntrinsic`); every later pass reads that tag and never
 *          compares a name, so a program that declares its own `heap` or
 *          `typeof` gets its own function, not the built-in. Its job is to
 *          be the one place grep'd (or read) before adding a new
 *          special-cased symbol name anywhere in the compiler, so that act
 *          is never silent. The kinds are listed separately below because
 *          each is recognized through a different mechanism -- conflating
 *          them would misstate how each is actually implemented:
 *
 *          1. Language-level bare intrinsics: real lexer keywords/tokens,
 *             callable from Turf source with no module qualifier and no
 *             `import`. Recognized in the Lexer's keyword table.
 *          2. LLVM-level intrinsics: not Turf syntax at all -- IDs from
 *             `llvm::Intrinsic::*` that Codegen requests via
 *             `getOrInsertDeclaration()`/`getDeclaration()`, invisible to
 *             the Resolver/TypeChecker entirely.
 *          3. Runtime entrypoints: real C functions in `libturfrt`
 *             (`runtime/turfrt.h`), called by name from Codegen-emitted IR.
 *             Never reachable from Turf source directly (no `extern fn`
 *             declares them in any lib/std file) -- Rule B2
 *             (ARCHITECTURE_OVERHAUL.md §3.3): the runtime is not a
 *             capability a Turf program requests, it's infrastructure every
 *             compiled binary already links.
 *          4. The prelude module list: not a callable symbol at all, but
 *             the other half of "what does the compiler know about by
 *             name" (STD-001, ARCHITECTURE_OVERHAUL.md §8/§15.3) -- which
 *             module names get auto-imported into every top-level compile
 *             with no `import` statement.
 *          5. Standard-library functions the compiler lowers itself: real
 *             `pub fn` declarations in lib/std whose calls Codegen replaces
 *             with one instruction (`math::wrapping_add`, NM-3). Recognized
 *             by the (module, function) of the resolved declaration.
 *
 * @copyright Copyright (c) 2026 TurfLang contributors. Licensed under the
 * Apache License.
 */

#ifndef TURF_INTRINSICS_H
#define TURF_INTRINSICS_H

#include <string>

/// The closed set of category-1 bare intrinsics (see below): language
/// vocabulary with no declaration anywhere. `None` is every other call.
///
/// Named `BareIntrinsic` because `Intrinsic` alone collides with
/// `llvm::Intrinsic` in every file that says `using namespace llvm`.
enum class BareIntrinsic {
  None,
  TypeOf,
  GcAlloc,
  GcAllocZeroed,
  GcRealloc,
  GcCollect,
  GcIsManaged,
  GcCheckedFree,
  GcPin,
  GcUnpin,
  Heap,
  HashOf,
};

/// The intrinsic spelled `Name`, or `BareIntrinsic::None`. The one place an
/// intrinsic's name is compared -- and it is only asked when no declared
/// function of that name exists (ResolverCall.cpp), so a user's own `heap`,
/// `typeof` or `gc_alloc` always wins over the built-in.
inline BareIntrinsic LookupIntrinsic(const std::string &Name) {
  if (Name == "typeof")          return BareIntrinsic::TypeOf;
  if (Name == "gc_alloc")        return BareIntrinsic::GcAlloc;
  if (Name == "gc_alloc_zeroed") return BareIntrinsic::GcAllocZeroed;
  if (Name == "gc_realloc")      return BareIntrinsic::GcRealloc;
  if (Name == "gc_collect")      return BareIntrinsic::GcCollect;
  if (Name == "gc_is_managed")   return BareIntrinsic::GcIsManaged;
  if (Name == "gc_checked_free") return BareIntrinsic::GcCheckedFree;
  if (Name == "gc_pin")          return BareIntrinsic::GcPin;
  if (Name == "gc_unpin")        return BareIntrinsic::GcUnpin;
  if (Name == "heap")            return BareIntrinsic::Heap;
  if (Name == "hash_of")         return BareIntrinsic::HashOf;
  return BareIntrinsic::None;
}

/// The spelling of `I` -- the key of its entry in the builtin registry
/// (Builtins.cpp), which holds its arity. The other direction of
/// LookupIntrinsic(); `None` has no spelling.
inline const char *IntrinsicName(BareIntrinsic I) {
  switch (I) {
  case BareIntrinsic::TypeOf:        return "typeof";
  case BareIntrinsic::GcAlloc:       return "gc_alloc";
  case BareIntrinsic::GcAllocZeroed: return "gc_alloc_zeroed";
  case BareIntrinsic::GcRealloc:     return "gc_realloc";
  case BareIntrinsic::GcCollect:     return "gc_collect";
  case BareIntrinsic::GcIsManaged:   return "gc_is_managed";
  case BareIntrinsic::GcCheckedFree: return "gc_checked_free";
  case BareIntrinsic::GcPin:         return "gc_pin";
  case BareIntrinsic::GcUnpin:       return "gc_unpin";
  case BareIntrinsic::Heap:          return "heap";
  case BareIntrinsic::HashOf:        return "hash_of";
  case BareIntrinsic::None:          break;
  }
  return "";
}

// ─── 1. Language-level bare intrinsics ─────────────────────────────────────
// Real TOK_* keywords (src/frontend/Lexer.h's Keywords table, src/frontend/Lexer.cpp) or
// entries in Builtins.cpp's g_builtins registry -- callable from Turf source
// under their own bare name, with no `import`.
//
//   sizeof(T)   -- TOK_SIZEOF (src/frontend/Lexer.cpp). Parsed by its own grammar
//                  production (ParserCore.cpp), not through the ordinary
//                  call-expression path. Requires a monomorphized concrete
//                  T (TYPE-002); asserts if handed a TurfGenericParamType
//                  (F-GEN-2).
//   typeof(expr) -- registered in Builtins.cpp's g_builtins (Arity 1);
//                  the one remaining entry there after `lengthof` was
//                  deleted (STRING_UFCS_OPERATOR_CHAR_RFC.md §7 step 7).
//                  Codegen handles it inline at its one call site
//                  (FuncCallExprAST::codegen(), CodegenCore.cpp) rather
//                  than through a shared dispatcher.
//   gc_alloc(n)  -- MEM-001. `n: int` byte count, returns `void*`. Calls
//                  turf_rt_alloc(n, NULL) -- always an untyped/raw
//                  allocation (see turf_rt_alloc's own doc comment in
//                  runtime/turfrt.h for why no TurfTypeDesc is passed).
//                  Registered in Builtins.cpp's g_builtins (Arity 1);
//                  dispatched the same way as typeof, immediately
//                  alongside it in FuncCallExprAST::codegen().
//   gc_alloc_zeroed(n) -- MEM-001. Like gc_alloc(n), but the block is
//                  zero-filled (turf_rt_alloc_zeroed). For the handful of
//                  GC-heap call sites that rely on zero-initialization,
//                  not just convenience -- see turf_rt_alloc_zeroed's own
//                  doc comment.
//   gc_realloc(p, n) -- MEM-001/MEM-002. `p` any pointer-shaped value,
//                  `n: int` new byte count, returns `void*`. Calls
//                  turf_rt_realloc(p, n, desc) -- `desc` is a real
//                  per-element TurfTypeDesc when CastExprAST::codegen()
//                  recognizes a `gc_realloc(...) to T*`-shaped call site
//                  (MEM-002), NULL otherwise. Exists specifically so
//                  MEM-001's collection-growth call sites keep realloc's
//                  in-place-extend behavior instead of degrading to
//                  always-allocate-fresh-and-copy. Arity 2.
//   gc_collect() -- MEM-002. Forces one turf_rt_collect() mark-sweep
//                  cycle (also run automatically on allocation pressure).
//                  Registered in Builtins.cpp's g_builtins (Arity 0);
//                  dispatched alongside gc_alloc.
//   gc_is_managed(p) -- MEM-002. `bool`: true iff `p` is exactly a live
//                  GC-heap block's own start address (turf_rt_is_gc_pointer).
//                  Registered in Builtins.cpp's g_builtins (Arity 1).
//   gc_checked_free(p) -- MEM-002. Frees a manual-heap `p`
//                  (turf_rt_checked_free) -- panics instead if `p` is a
//                  GC-heap pointer. `lib/std/mem/mem.tr`'s own `free()` is
//                  its one caller: Turf's `extern fn` binds a declared name
//                  directly to an identically-named C symbol with no alias
//                  syntax, so mem.tr's checked wrapper could not itself be
//                  named `free` while an `extern fn free` still existed in
//                  the same file -- this bare intrinsic is what replaced
//                  that extern binding. Registered in Builtins.cpp's
//                  g_builtins (Arity 1).
//   gc_pin(p) / gc_unpin(p) -- OVH-010
//                  (docs/development/changes/OVH-010.md). Pins/unpins
//                  the GC object containing `p` against collection
//                  (turf_rt_pin/turf_rt_unpin) -- an interior pointer is
//                  fine, pins nest, and `p` need not be a GC pointer at all
//                  (a documented no-op). `lib/std/mem/mem.tr`'s
//                  `mem::root()`/`mem::unroot()` are their one caller each,
//                  the same one-bare-intrinsic-one-stdlib-wrapper shape
//                  `gc_checked_free`/`mem::free` already use. Both Arity 1,
//                  registered in Builtins.cpp's g_builtins.
//   heap(expr) -- FDN-001 (docs/development/ROADMAP_v2.0.0.md, DEC-5). `expr: T`
//                  for any sized T (struct, fixed array, primitive), returns
//                  `&T`. Copies expr's evaluated value into a fresh
//                  turf_rt_alloc(sizeof(T), desc_T) block and returns its
//                  address -- `desc_T` is the SAME per-type TurfTypeDesc
//                  global emitTurfTypeDesc()/getOrForwardDeclareTurfTypeDescGlobal()
//                  already builds for T when T is a named struct (OVH-002),
//                  or getOrCreateElementTurfTypeDesc()'s general per-LLVM-type
//                  descriptor otherwise -- so the collector traces a heap()ed
//                  value's own pointer fields exactly as it would a
//                  stack-resident T. Rejects `void` (nothing to copy).
//                  Registered in Builtins.cpp's g_builtins (Arity 1);
//                  dispatched in FuncCallExprAST::codegen() via the shared
//                  HeapAllocateValue() helper (CodegenCore.cpp).
//   hash_of(expr) -- REL-005 follow-up (owner decision 2026-10-13). `int`:
//                  a hash of `expr`'s VALUE that agrees with field-by-field
//                  `==`, generated from its static type
//                  (EmitStructuralHash(), CodegenCore.cpp): a string by its
//                  bytes, a double with -0.0 taken as 0.0, a struct field by
//                  field (never its padding), a fixed array element by
//                  element, a reference or pointer by address. What
//                  hashing::hash() (and so every Map/Set key) uses; the old
//                  byte hash read padding and missed equal keys (1999 of 2000
//                  struct keys at -O0). Registered in Builtins.cpp's
//                  g_builtins (Arity 1).
//   caller_location -- STD-002. Unlike every other bare intrinsic above, a
//                  real reserved LEXER KEYWORD (TOK_CALLER_LOCATION,
//                  src/frontend/Lexer.h), not a g_builtins entry -- it's a
//                  zero-argument, non-callable VALUE read (mirroring `self`,
//                  TOK_SELF), not a call. Legal only inside a function
//                  declared `@caller_location` (the one legal use of `@` in
//                  this language -- ParsePrimaryBase()'s TOK_AT case,
//                  src/frontend/ParserCore.cpp); reads that function's own hidden,
//                  Codegen-only trailing parameter (never a real `Params`
//                  entry -- see docs/architecture/caller_location.md §3.3
//                  for why keeping it out of the arity-checked signature is
//                  the whole design). Full contract, worked examples, and
//                  the transitive forward-vs-stamp propagation rule:
//                  docs/architecture/caller_location.md.

// ─── 2. LLVM-level intrinsics ──────────────────────────────────────────────
// Requested via llvm::Intrinsic::getOrInsertDeclaration() (LLVM >= 19) /
// getDeclaration() (LLVM < 19), never visible to the Resolver or
// TypeChecker -- SEM-002's checked-arithmetic codegen (EmitCheckedIntArith(),
// src/codegen/CodegenCore.cpp) is the only current user:
//
//   llvm::Intrinsic::sadd_with_overflow  -- checked `+` on `int`
//   llvm::Intrinsic::ssub_with_overflow  -- checked `-` on `int`
//   llvm::Intrinsic::smul_with_overflow  -- checked `*` on `int`
//
// (Unary `-`, `<<`, and signed `/` overflow are checked WITHOUT an LLVM
// intrinsic -- see EmitCheckedIntNegate()/EmitCheckedShl()/
// EmitSignedDivOverflowGuard() in src/codegen/CodegenCore.cpp, each hand-rolled
// from ordinary icmp/branch IR. Listed here for completeness, not because
// they're "intrinsics" in LLVM's own sense.)

// ─── 3. Runtime entrypoints (libturfrt, runtime/turfrt.h) ──────────────────
// Real C functions, linked into every compiled Turf binary via `-lturfrt`
// (src/driver/main.cpp's linkObject()). Declared as ordinary LLVM Function
// prototypes at the call site, exactly like an `extern fn`-declared libc
// binding, but never reachable BY that mechanism -- no lib/std file
// declares any of these as `extern fn`, on purpose (Rule B2/B3).
//
//   turf_rt_init()      -- process bootstrap; called from the
//                           compiler-emitted wrapper around `main`
//                           (src/codegen/CodegenFunc.cpp,
//                           WrapMainWithRuntimeBootstrap()).
//   turf_rt_shutdown()  -- process teardown; called from the same wrapper,
//                           after the user's `main` body returns.
//   turf_rt_alloc(bytes, desc) -- the typed allocation entrypoint. As of
//                           MEM-001, reached from Codegen via the
//                           `gc_alloc(n)` bare intrinsic above -- every
//                           GC-heap call site classified in
//                           docs/development/changes/MEM-001.md reroutes
//                           here, always with desc == NULL (see its own
//                           doc comment in runtime/turfrt.h).
//   turf_rt_alloc_zeroed(bytes, desc) -- MEM-001. Zero-filling counterpart
//                           to turf_rt_alloc, reached via `gc_alloc_zeroed`.
//   turf_rt_realloc(p, new_size, desc) -- MEM-001. Reached via
//                           `gc_realloc`, used by every rerouted
//                           collection-growth call site.
//   turf_rt_stats() -- MEM-001's allocation-accounting hook. Never reached
//                           from Codegen/Turf source at all (no intrinsic
//                           calls it) -- read directly from C, either
//                           scripts/smoke/mem_smoke.sh (linked straight against
//                           lib/libturfrt.a) or turf_rt_shutdown()'s
//                           TURF_RT_STATS_DUMP-gated stderr dump.
//   turf_rt_panic(kind, msg, file, line) -- replaces CodegenPanic()'s old
//                           inline fprintf+abort (src/sema/Builtins.cpp). Every
//                           one of the compiler's own runtime safety guards
//                           (division/modulo by zero, integer overflow,
//                           array bounds, bad string->int/enum casts) calls
//                           this, never reachable from Turf source under
//                           any name.
//   turf_rt_typedesc_is_pointer_word(desc, word_index) -- descriptor
//                           accessor; MEM-002's mark phase (turfrt_gc.c) is
//                           its real, intended caller as of this group
//                           (scripts/smoke/runtime_smoke.sh was its only caller
//                           in RT-001).
//   turf_rt_collect() -- MEM-002. Runs one mark-sweep cycle. Reached from
//                           Codegen via the `gc_collect()` bare intrinsic
//                           above; also called internally (turf_gc_note_alloc,
//                           turfrt_gc.c) on allocation pressure.
//   turf_rt_add_global_root(slot) / turf_rt_remove_global_root(slot) --
//                           MEM-002. Registers/unregisters a `void**`
//                           precise root. No current Codegen call site --
//                           TurfLang has no mutable global/static storage
//                           today -- so this is genuine, tested runtime API
//                           (scripts/smoke/gc_smoke.sh) with no reachable-from-
//                           Turf-source surface yet; see
//                           docs/development/changes/MEM-002.md.
//   turf_rt_is_gc_pointer(p) -- MEM-002. Reached via `gc_is_managed(p)`
//                           above.
//   turf_rt_checked_free(p) -- MEM-002. Reached via `gc_checked_free(p)`
//                           above.
//   turf_rt_pin(p) / turf_rt_unpin(p) -- OVH-010. Reached via `gc_pin(p)`/
//                           `gc_unpin(p)` above.
//   turf_rt_abi_2_0() -- FDN-006 stage 3, the compiler/runtime ABI stamp
//                           (runtime/turfrt_abi.c). Does nothing; called
//                           once from the compiler-emitted `main` wrapper
//                           (WrapMainWithRuntimeBootstrap(),
//                           src/codegen/CodegenFunc.cpp) purely so a stale or
//                           mismatched `libturfrt.a` fails to LINK, never
//                           reachable from Turf source under any name.
//
// TurfTypeDesc (the {size, name, pointer_word_bitmap} struct these
// functions traffic in) is NOT itself a callable intrinsic, but every
// monomorphized aggregate gets one emitted as a compiler-generated LLVM
// constant global (src/codegen/CodegenStruct.cpp, right after StructDeclExprAST's
// ST->setBody() call) -- listed here because it's the other half of the
// closed vocabulary this header exists to name.

// ─── 4. The prelude module list (STD-001) ──────────────────────────────────
// int, double, bool, char, strings, array, io, math, sys -- auto-imported into every
// top-level compile (as if the user's file began with `import int; import
// double; ...`), by ParsePreludeImports() (src/frontend/ParserModule.cpp), reading
// the array PreludeModuleNames right above it in the same file.
//
// This is a SPECIFICATION, not an arbitrary hardcode: these nine names are
// exactly the `.tr` files under lib/std/core/ -- see
// docs/architecture/boundary.md's "Prelude module specification" section
// for why that correspondence is deliberate (core/ is what the target
// boundary diagram, ARCHITECTURE_OVERHAUL.md §3.2, labels "core
// (prelude)"), not incidental. scripts/check/check_boundary.sh's check 2 enforces
// the correspondence mechanically -- a future session adding a 7th core/
// module (or removing one) must update PreludeModuleNames in the same
// change, or the check fails.
//
// Deliberately NOT derived from a directory listing at compile time: the
// rest of lib/std's subfolder structure (collections/, io/, mem/, math/,
// ...) is explicitly documented (AGENTS.md, modules_and_scope_resolution.md
// Part 2.4) as invisible to `import` -- purely organizational. Making
// core/ additionally drive real compiler behavior (auto-import) at
// runtime would make that one subfolder secretly load-bearing in a way
// none of the others are, and would let a purely-organizational file move
// silently change what gets auto-imported into every compile. Keeping
// PreludeModuleNames an explicit, named list -- checked against core/'s
// contents rather than generated from them -- keeps that change visible
// and deliberate (the same "explicit, change-recorded act" F-STD-2 asks of
// the intrinsic list above).

// ─── 5. Standard-library functions the compiler lowers itself (NM-3) ───────
// Ordinary `pub fn` declarations in lib/std with a real signature, a doc
// comment and a body, which Codegen replaces with one LLVM instruction both
// at every direct call and in the function's own definition (so a function
// value of one behaves the same). Keyed by the (module, function) of the
// RESOLVED DECLARATION, never by the name at a call: the Resolver asks
// LookupLoweredFunction() with the declaring module's key
// (Resolver::buildFunctionSymbol()) and records the answer on the symbol
// (ResolverFunctionSymbol::Lowering), so a program's own `wrapping_add`, in
// its main file or its own module, is its own function. Change record:
// docs/development/changes/TDP-2-002.md.
//
//   math::wrapping_add(a: int, b: int): int -- `add i64`, no `nsw`
//   math::wrapping_sub(a: int, b: int): int -- `sub i64`, no `nsw`
//   math::wrapping_mul(a: int, b: int): int -- `mul i64`, no `nsw`
//   math::wrapping_shl(a: int, n: int): int -- `shl i64` by `n & 63`
//
// Each is the result modulo 2^64 in two's complement and never traps. The
// rest of the family (wrapping_neg, _abs, _div, _rem, _shr, _pow) is ordinary
// Turf built on these four in lib/std/core/math.tr. The
// lowering applies at every optimization level, `-O0` included: a call that
// stayed a call would give the same result but lose the speed the functions
// exist for.

/// The closed set of kind-5 functions. `None` is every other declaration.
enum class LoweredFunction {
  None,
  WrappingAdd,
  WrappingSub,
  WrappingMul,
  WrappingShl,
};

/// The kind-5 function declared as `Name` in the module keyed `Module`, or
/// `LoweredFunction::None`.
inline LoweredFunction LookupLoweredFunction(const std::string &Module,
                                             const std::string &Name) {
  if (Module != "math")
    return LoweredFunction::None;
  if (Name == "wrapping_add") return LoweredFunction::WrappingAdd;
  if (Name == "wrapping_sub") return LoweredFunction::WrappingSub;
  if (Name == "wrapping_mul") return LoweredFunction::WrappingMul;
  if (Name == "wrapping_shl") return LoweredFunction::WrappingShl;
  return LoweredFunction::None;
}

#endif // TURF_INTRINSICS_H
