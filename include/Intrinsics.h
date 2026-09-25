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
//   gc_realloc(p, n) -- MEM-001. `p` any pointer-shaped value, `n: int`
//                  new byte count, returns `void*`. Calls
//                  turf_rt_realloc(p, n, NULL) -- exists specifically so
//                  MEM-001's collection-growth call sites keep realloc's
//                  in-place-extend behavior instead of degrading to
//                  always-allocate-fresh-and-copy. Arity 2.
//
// Reserved, NOT yet implemented -- do not add a matching enum/keyword
// anywhere else in the compiler as a side effect of an unrelated change
// (F-STD-2's "explicit, change-recorded act"). Listed here only so this
// registry is the first place a future session checks before wiring it up:
//
//   caller_location -- named in RT-001's own spec text
//                  (ARCHITECTURE_OVERHAUL.md §8) as a symbol Intrinsics.h
//                  should eventually enumerate. Verified live as of RT-001:
//                  no TOK_CALLER_LOCATION, no Lexer keyword-table entry, no
//                  Resolver/TypeChecker/Codegen handling exists anywhere in
//                  the tree. STD-002 (§8) is the group that actually builds
//                  it (`@caller_location`, error payloads); this line is a
//                  placeholder for that session, not a claim it works today.

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
//                           accessor; MEM-002's mark phase is its intended
//                           long-term caller, scripts/runtime_smoke.sh its
//                           only caller in RT-001.
//
// TurfTypeDesc (the {size, name, pointer_word_bitmap} struct these
// functions traffic in) is NOT itself a callable intrinsic, but every
// monomorphized aggregate gets one emitted as a compiler-generated LLVM
// constant global (src/CodegenStruct.cpp, right after StructDeclExprAST's
// ST->setBody() call) -- listed here because it's the other half of the
// closed vocabulary this header exists to name.

#endif // TURF_INTRINSICS_H
