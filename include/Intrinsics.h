/**
 * @file Intrinsics.h
 * @brief The closed, enumerated list of every symbol the compiler treats
 *        specially -- F-STD-2 (FREEZE.md §6.8): "the intrinsic/builtin list
 *        is closed and enumerated in one place. Adding an intrinsic is an
 *        explicit, change-recorded act." Introduced by RT-001
 *        (docs/planning/ARCHITECTURE_OVERHAUL.md §8), which needed a single
 *        place to name the new `turf_rt_*` runtime entrypoints alongside
 *        the compiler-level intrinsics that already existed with no such
 *        registry.
 *
 * @details This header is documentation-as-code, not a dispatch table --
 *          nothing #includes it to drive a switch statement. Its job is to
 *          be the one place grep'd (or read) before adding a new
 *          special-cased symbol name anywhere in the compiler, so that act
 *          is never silent. Three genuinely different kinds of "intrinsic"
 *          are listed separately below because they're recognized through
 *          three different mechanisms -- conflating them would misstate how
 *          each is actually implemented:
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
 *
 * @copyright Copyright (c) 2026 TurfLang contributors. Licensed under the
 * Apache License.
 */

#ifndef TURF_INTRINSICS_H
#define TURF_INTRINSICS_H

// ─── 1. Language-level bare intrinsics ─────────────────────────────────────
// Real TOK_* keywords (include/Lexer.h's Keywords table, src/Lexer.cpp) or
// entries in Builtins.cpp's g_builtins registry -- callable from Turf source
// under their own bare name, with no `import`.
//
//   sizeof(T)   -- TOK_SIZEOF (src/Lexer.cpp). Parsed by its own grammar
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
//   caller_location -- STD-002. Unlike every other bare intrinsic above, a
//                  real reserved LEXER KEYWORD (TOK_CALLER_LOCATION,
//                  include/Lexer.h), not a g_builtins entry -- it's a
//                  zero-argument, non-callable VALUE read (mirroring `self`,
//                  TOK_SELF), not a call. Legal only inside a function
//                  declared `@caller_location` (the one legal use of `@` in
//                  this language -- ParsePrimaryBase()'s TOK_AT case,
//                  src/ParserCore.cpp); reads that function's own hidden,
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
// src/CodegenCore.cpp) is the only current user:
//
//   llvm::Intrinsic::sadd_with_overflow  -- checked `+` on `int`
//   llvm::Intrinsic::ssub_with_overflow  -- checked `-` on `int`
//   llvm::Intrinsic::smul_with_overflow  -- checked `*` on `int`
//
// (Unary `-`, `<<`, and signed `/` overflow are checked WITHOUT an LLVM
// intrinsic -- see EmitCheckedIntNegate()/EmitCheckedShl()/
// EmitSignedDivOverflowGuard() in src/CodegenCore.cpp, each hand-rolled
// from ordinary icmp/branch IR. Listed here for completeness, not because
// they're "intrinsics" in LLVM's own sense.)

// ─── 3. Runtime entrypoints (libturfrt, runtime/turfrt.h) ──────────────────
// Real C functions, linked into every compiled Turf binary via `-lturfrt`
// (src/main.cpp's linkObject()). Declared as ordinary LLVM Function
// prototypes at the call site, exactly like an `extern fn`-declared libc
// binding, but never reachable BY that mechanism -- no lib/std file
// declares any of these as `extern fn`, on purpose (Rule B2/B3).
//
//   turf_rt_init()      -- process bootstrap; called from the
//                           compiler-emitted wrapper around `main`
//                           (src/CodegenFunc.cpp,
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
//                           scripts/mem_smoke.sh (linked straight against
//                           lib/libturfrt.a) or turf_rt_shutdown()'s
//                           TURF_RT_STATS_DUMP-gated stderr dump.
//   turf_rt_panic(kind, msg, file, line) -- replaces CodegenPanic()'s old
//                           inline fprintf+abort (src/Builtins.cpp). Every
//                           one of the compiler's own runtime safety guards
//                           (division/modulo by zero, integer overflow,
//                           array bounds, bad string->int/enum casts) calls
//                           this, never reachable from Turf source under
//                           any name.
//   turf_rt_typedesc_is_pointer_word(desc, word_index) -- descriptor
//                           accessor; MEM-002's mark phase (turfrt_gc.c) is
//                           its real, intended caller as of this group
//                           (scripts/runtime_smoke.sh was its only caller
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
//                           (scripts/gc_smoke.sh) with no reachable-from-
//                           Turf-source surface yet; see
//                           docs/development/changes/MEM-002.md.
//   turf_rt_is_gc_pointer(p) -- MEM-002. Reached via `gc_is_managed(p)`
//                           above.
//   turf_rt_checked_free(p) -- MEM-002. Reached via `gc_checked_free(p)`
//                           above.
//   turf_rt_pin(p) / turf_rt_unpin(p) -- OVH-010. Reached via `gc_pin(p)`/
//                           `gc_unpin(p)` above.
//
// TurfTypeDesc (the {size, name, pointer_word_bitmap} struct these
// functions traffic in) is NOT itself a callable intrinsic, but every
// monomorphized aggregate gets one emitted as a compiler-generated LLVM
// constant global (src/CodegenStruct.cpp, right after StructDeclExprAST's
// ST->setBody() call) -- listed here because it's the other half of the
// closed vocabulary this header exists to name.

// ─── 4. The prelude module list (STD-001) ──────────────────────────────────
// int, double, bool, char, strings, array -- auto-imported into every
// top-level compile (as if the user's file began with `import int; import
// double; ...`), by ParsePreludeImports() (src/ParserModule.cpp), reading
// the array PreludeModuleNames right above it in the same file.
//
// This is a SPECIFICATION, not an arbitrary hardcode: these six names are
// exactly the `.tr` files under lib/std/core/ -- see
// docs/architecture/boundary.md's "Prelude module specification" section
// for why that correspondence is deliberate (core/ is what the target
// boundary diagram, ARCHITECTURE_OVERHAUL.md §3.2, labels "core
// (prelude)"), not incidental. scripts/check_boundary.sh's check 2 enforces
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

#endif // TURF_INTRINSICS_H
