# DONE.md — handbook edits completed

Rows moved here from `PENDING.md`, each with the commit that did the work.
The handbook itself is still paused; rows land here as non-handbook items
in the queue are done.

| Feature | Chapter / file | What was done | Commit |
|---|---|---|---|
| LEC-001 | `docs/reference/cli_and_diagnostics.md` and the new `docs/reference/projects_and_packages.md` (the reference half of the LEC-001 `cli_and_diagnostics.md` row) | Command-line sections rewritten around `turf` and `turfc` (commands, passthrough, handshake, discovery, single-file commands, a "for tool authors" section); error-code count (68 → 91) and Important list corrected, E090/E091 added; new reference page for projects, packages, dependencies, `turf.lock` and the cache; both linked from `docs/reference/README.md`. The OPT-001 and TOOL-001 rows for this file stay open: the `-O*`, `-g`, `--target` and `-mcpu` flags are now in the table, but the F-OPT-1 contract paragraph and the `gdb` walkthrough are not written. | `docs(reference): projects_and_packages.md (@a2ys)` and `docs(reference): cli_and_diagnostics.md around turf and turfc (@a2ys)` (LEC-001 stage 8, 2026-09-30) |
| FDN-005 | `docs/development/INVARIANTS.md` `INV-RT-4` | Added the SysV64 execution evidence (`TURF_GC_VALIDATE`'s register cross-check and full-stack mark-set comparison, zero misses) and recorded the no-`/proc` fallback that did silently under-scan, now fixed. | `docs(development): record the FDN-005 follow-up` (the FDN-005 follow-up, 2026-09-29) |
| ARCH-001 | `AGENTS.md` | Nothing was owed; closed by REL-006's review of the queue. | (none) (REL-006, 2026-10-13) |
| TYPE-001 | `docs/reference/structs_and_enums.md` §"Self-referential structs" | Covered by the 2.0 rewrite of the target, verified against the compiler. | 0d35b10 (REL-006, 2026-10-13) |
| SEM-001 | `docs/reference/variables_and_types.md` §"Literals" | Covered by the 2.0 rewrite of the target, verified against the compiler. | 413c2c9 (REL-006, 2026-10-13) |
| OPT-001 | `docs/reference/cli_and_diagnostics.md` | Covered by the 2.0 rewrite of the target, verified against the compiler. | 908466d (REL-006, 2026-10-13) |
| TOOL-001 | `docs/reference/cli_and_diagnostics.md` | Covered by the 2.0 rewrite of the target, verified against the compiler. | 908466d (REL-006, 2026-10-13) |
| LANG-004 | `docs/reference/control_flow.md` §"`for` — range form" | Covered by the 2.0 rewrite of the target, verified against the compiler. | a0b5ce5 (REL-006, 2026-10-13) |
| LANG-004 | `docs/reference/ffi_and_unsafe.md` §1 `extern fn` | Covered by the 2.0 rewrite of the target, verified against the compiler. | 61b7eef (REL-006, 2026-10-13) |
| SEM-002 | `docs/reference/variables_and_types.md` §"Literals" | Covered by the 2.0 rewrite of the target, verified against the compiler. | 413c2c9 (REL-006, 2026-10-13) |
| SEM-002 | `docs/reference/variables_and_types.md` §"Casting with `to`" | Covered by the 2.0 rewrite of the target, verified against the compiler. | 413c2c9 (REL-006, 2026-10-13) |
| SEM-002 | `docs/reference/operators.md` §"Arithmetic: `+ - * / %`" | Covered by the 2.0 rewrite of the target, verified against the compiler. | 413c2c9 (REL-006, 2026-10-13) |
| SEM-003 | `docs/reference/variables_and_types.md` §"Variable declarations" | Covered by the 2.0 rewrite of the target, verified against the compiler. | 413c2c9 (REL-006, 2026-10-13) |
| SEM-004 | `docs/reference/variables_and_types.md` §"Default initialization" (lines 57–146) | Covered by the 2.0 rewrite of the target, verified against the compiler. | 413c2c9 (REL-006, 2026-10-13) |
| SEM-004 | `docs/reference/arrays.md` §"Fixed-size syntax and literals" (lines 16–32) | Covered by the 2.0 rewrite of the target, verified against the compiler. | 2eb97c5 (REL-006, 2026-10-13) |
| TYPE-002 | `docs/architecture/compiler_pipeline.md` §7 "Generic type erasure" (lines 884–944) | Covered by the 2.0 rewrite of the target, verified against the compiler. | c2fe1e7 (REL-006, 2026-10-13) |
| TYPE-002 | `docs/architecture/memory_model.md` §2.7 "Generics and type erasure" (lines 572–667, subsections 2.7.1–2.7.3) | Covered by the 2.0 rewrite of the target, verified against the compiler. | 09ab1aa (REL-006, 2026-10-13) |
| TYPE-002 | `docs/reference/generics_and_operators.md` §"Type-erasure model" (line 105) and §"The `List<T*>` workaround for large element types" (line 119) | Covered by the 2.0 rewrite of the target, verified against the compiler. | 4bf2265 (REL-006, 2026-10-13) |
| TYPE-002 | `docs/reference/stdlib/collections.md` §"Shared design: type erasure and growth policy" | Covered by the 2.0 rewrite of the target, verified against the compiler. | 7038ed1 (REL-006, 2026-10-13) |
| TYPE-002 | `docs/archive/LIMITATIONS.md` items 1–3 | Nothing was owed; closed by REL-006's review of the queue. | (none) (REL-006, 2026-10-13) |
| LANG-001 | `docs/reference/generics_and_operators.md` §"Operator Overloading" (the "Syntax"/"The overloadable set"/"Both operands already builtin"/"Overload selection"/"Visibility"/"Gotcha: compound assignment isn't routed through operators" subsections, lines 164–337) | Covered by the 2.0 rewrite of the target, verified against the compiler. | 4bf2265 (REL-006, 2026-10-13) |
| LANG-001 | `docs/reference/operators.md` §"Logical: `&& \ | Covered by the 2.0 rewrite of the target, verified against the compiler. | 413c2c9 (REL-006, 2026-10-13) |
| LANG-002 | `docs/architecture/pointers_and_references.md`, `docs/reference/structs_and_enums.md` §"Self-referential structs", `docs/reference/ffi_and_unsafe.md` §3/§4 | Covered by the 2.0 rewrite of the target, verified against the compiler. | 0d35b10, 61b7eef, ff4b287 (REL-006, 2026-10-13) |
| LANG-002 | `README.md` §"FFI" and §"`unsafe { }` and safe references (`&T`)" | Covered by the 2.0 rewrite of the target, verified against the compiler. | ed532dd (REL-006, 2026-10-13) |
| LANG-003 | `docs/reference/control_flow.md` (new `## defer` section) and `docs/reference/README.md`'s chapter-3 one-line summary | Covered by the 2.0 rewrite of the target, verified against the compiler. | a0b5ce5, 74d2443 (REL-006, 2026-10-13) |
| RT-001 | `docs/architecture/runtime.md` | Covered by the 2.0 rewrite of the target, verified against the compiler. | 09ab1aa (REL-006, 2026-10-13) |
| MEM-001 | `docs/architecture/memory_model.md` §1.4, §1.7, §2.4, §2.6 | Covered by the 2.0 rewrite of the target, verified against the compiler. | 09ab1aa (REL-006, 2026-10-13) |
| MEM-002 | `docs/architecture/memory_model.md` §1.1, §1.4, §1.7, §2.6 | Covered by the 2.0 rewrite of the target, verified against the compiler. | 09ab1aa (REL-006, 2026-10-13) |
| MEM-002 | `docs/reference/stdlib/mem.md` | Covered by the 2.0 rewrite of the target, verified against the compiler. | 7038ed1 (REL-006, 2026-10-13) |
| MEM-002 | `docs/reference/stdlib/collections.md` | Covered by the 2.0 rewrite of the target, verified against the compiler. | 7038ed1 (REL-006, 2026-10-13) |
| STD-001 | `docs/architecture/boundary.md`, `docs/architecture/pointers_and_references.md` §2.5.1 | Covered by the 2.0 rewrite of the target, verified against the compiler. | ff4b287 (REL-006, 2026-10-13) |
| STD-003 | `docs/reference/stdlib/collections.md`, `docs/reference/stdlib/http.md`, `docs/reference/stdlib/mem.md` | Covered by the 2.0 rewrite of the target, verified against the compiler. | 7038ed1, e35e504 (REL-006, 2026-10-13) |
| STD-003 | `docs/reference/generics_and_operators.md` §"Type-erasure model"/§"The `List<T*>` workaround for large element types" | Covered by the 2.0 rewrite of the target, verified against the compiler. | 4bf2265 (REL-006, 2026-10-13) |
| OPT-002 | `docs/reference/cli_and_diagnostics.md` §"Optimization levels" (still pending from OPT-001's own row above — this row is additive to that one, not independent of it) | Covered by the 2.0 rewrite of the target, verified against the compiler. | 908466d (REL-006, 2026-10-13) |
| OVH-002 | `docs/architecture/boundary.md` §5/§6, `docs/architecture/errors_and_recover.md` §2.3/§2.8/Part 3 | Covered by the 2.0 rewrite of the target, verified against the compiler. | ff4b287 (REL-006, 2026-10-13) |
| OVH-002 | `docs/reference/generics_and_operators.md` §"Type-erasure model"/§"The `List<T*>` workaround for large element types" | Covered by the 2.0 rewrite of the target, verified against the compiler. | 4bf2265 (REL-006, 2026-10-13) |
| OVH-001 | `docs/architecture/runtime.md` §4, `docs/architecture/memory_model.md` §2.6, `INVARIANTS.md` (INV-RT-3, new INV-MEM-2) | Covered by the 2.0 rewrite of the target, verified against the compiler. | 09ab1aa (REL-006, 2026-10-13) |
| OVH-003 | `docs/architecture/pointers_and_references.md` §2.7 | Covered by the 2.0 rewrite of the target, verified against the compiler. | ff4b287 (REL-006, 2026-10-13) |
| OVH-003 | `docs/reference/stdlib/collections.md` | Covered by the 2.0 rewrite of the target, verified against the compiler. | 7038ed1 (REL-006, 2026-10-13) |
| OVH-004 | `docs/reference/structs_and_enums.md` §"Self-referential structs" | Covered by the 2.0 rewrite of the target, verified against the compiler. | 0d35b10 (REL-006, 2026-10-13) |
| OVH-009 | `docs/reference/generics_and_operators.md` | Covered by the 2.0 rewrite of the target, verified against the compiler. | 4bf2265 (REL-006, 2026-10-13) |
| OVH-010 | `docs/reference/stdlib/mem.md` | Covered by the 2.0 rewrite of the target, verified against the compiler. | 7038ed1 (REL-006, 2026-10-13) |
| OVH-005 | `docs/reference/variables_and_types.md` §`int32` (lines 168–188) | Covered by the 2.0 rewrite of the target, verified against the compiler. | 413c2c9 (REL-006, 2026-10-13) |
| OVH-008 | (none) | Nothing was owed; closed by REL-006's review of the queue. | (none) (REL-006, 2026-10-13) |
| OVH-011 | (none) | Nothing was owed; closed by REL-006's review of the queue. | (none) (REL-006, 2026-10-13) |
| OVH-007 | (none) | Nothing was owed; closed by REL-006's review of the queue. | (none) (REL-006, 2026-10-13) |
| OVH-006 | (none) | Nothing was owed; closed by REL-006's review of the queue. | (none) (REL-006, 2026-10-13) |
| STD-002 | `docs/reference/error_handling.md` | Covered by the 2.0 rewrite of the target, verified against the compiler. | b2b3b48 (REL-006, 2026-10-13) |
| OVH-012 | `docs/architecture/errors_and_recover.md` §2.6 | Covered by the 2.0 rewrite of the target, verified against the compiler. | ff4b287 (REL-006, 2026-10-13) |
| OVH-012 | `docs/reference/cli_and_diagnostics.md` | Covered by the 2.0 rewrite of the target, verified against the compiler. | 908466d (REL-006, 2026-10-13) |
| OVH-013 | (none) | Nothing was owed; closed by REL-006's review of the queue. | (none) (REL-006, 2026-10-13) |
| OVH-014 | (none) | Nothing was owed; closed by REL-006's review of the queue. | (none) (REL-006, 2026-10-13) |
| OVH-015 | (none) | Nothing was owed; closed by REL-006's review of the queue. | (none) (REL-006, 2026-10-13) |
| OVH-CLOSE | (none) | Nothing was owed; closed by REL-006's review of the queue. | (none) (REL-006, 2026-10-13) |
| SMP-002 | (none) | Nothing was owed; closed by REL-006's review of the queue. | (none) (REL-006, 2026-10-13) |
| FDN-003 | `docs/reference/functions_and_modules.md` §4 (new §4.8 "Types belong to their module"), plus the "Common errors" table | Covered by the 2.0 rewrite of the target, verified against the compiler. | affa17f (REL-006, 2026-10-13) |
| FDN-003 | `docs/reference/structs_and_enums.md` §"Declaring a struct" rules list | Covered by the 2.0 rewrite of the target, verified against the compiler. | 0d35b10 (REL-006, 2026-10-13) |
| FDN-003 | `docs/reference/variables_and_types.md` §"`typeof(expr)`" | Covered by the 2.0 rewrite of the target, verified against the compiler. | 413c2c9 (REL-006, 2026-10-13) |
| FDN-003 | `docs/reference/cli_and_diagnostics.md` (`-g` / debugging, once TOOL-001's row lands) | Covered by the 2.0 rewrite of the target, verified against the compiler. | 908466d (REL-006, 2026-10-13) |
| FDN-003 (follow-ups) | `docs/reference/structs_and_enums.md` (methods on generic structs; local type declarations) | Covered by the 2.0 rewrite of the target, verified against the compiler. | 0d35b10 (REL-006, 2026-10-13) |
| FDN-001 | `docs/reference/ffi_and_unsafe.md` §"`&T` references" | Covered by the 2.0 rewrite of the target, verified against the compiler. | 61b7eef (REL-006, 2026-10-13) |
| FDN-001 | `docs/reference/stdlib/mem.md` or a new "Memory" reference page | Covered by the 2.0 rewrite of the target, verified against the compiler. | 7038ed1 (REL-006, 2026-10-13) |
| FDN-002 | `docs/reference/ffi_and_unsafe.md` §"`&T` references" (right beside FDN-001's own `heap()` addition above) | Covered by the 2.0 rewrite of the target, verified against the compiler. | 61b7eef (REL-006, 2026-10-13) |
| FDN-002 | wherever E0NN codes end up catalogued for readers (no such reference chapter exists yet) | Covered by the 2.0 rewrite of the target, verified against the compiler. | 908466d (REL-006, 2026-10-13) |
| FDN-002 | `docs/MIGRATING_TO_2.md` (REL-006 writes it, per ROADMAP Part V's compatibility table) | Covered by the 2.0 rewrite of the target, verified against the compiler. | 0f6a8d7 (REL-006, 2026-10-13) |
| FDN-004 | `docs/reference/variables_and_types.md` §"`char`" (and/or a new "Indexing a string" subsection near the `string` section) | Covered by the 2.0 rewrite of the target, verified against the compiler. | 413c2c9 (REL-006, 2026-10-13) |
| FDN-004 | `docs/reference/stdlib/core.md` (`strings` module section) | Covered by the 2.0 rewrite of the target, verified against the compiler. | badc7c7 (REL-006, 2026-10-13) |
| FDN-004 | `docs/MIGRATING_TO_2.md` (REL-006 writes it, per ROADMAP Part V's compatibility table) | Covered by the 2.0 rewrite of the target, verified against the compiler. | 0f6a8d7 (REL-006, 2026-10-13) |
| FDN-006 | `docs/architecture/runtime.md` §7 | Covered by the 2.0 rewrite of the target, verified against the compiler. | 09ab1aa (REL-006, 2026-10-13) |
| FDN-006 | `docs/reference/cli_and_diagnostics.md` | Covered by the 2.0 rewrite of the target, verified against the compiler. | 908466d (REL-006, 2026-10-13) |
| SMP-001 | (none) | Nothing was owed; closed by REL-006's review of the queue. | (none) (REL-006, 2026-10-13) |
| FDN-005 | `docs/architecture/memory_model.md` §2.6 | Covered by the 2.0 rewrite of the target, verified against the compiler. | 09ab1aa (REL-006, 2026-10-13) |
| LEC-002 | `docs/reference/functions_and_modules.md` (new subsection on visibility) and `docs/architecture/modules_and_scope_resolution.md` §1.3/§1.7 | Covered by the 2.0 rewrite of the target, verified against the compiler. | affa17f, c2fe1e7 (REL-006, 2026-10-13) |
| LEC-003 | `docs/MIGRATING_TO_2.md` (REL-006 writes it) | Covered by the 2.0 rewrite of the target, verified against the compiler. | 0f6a8d7 (REL-006, 2026-10-13) |
| LEC-004 | `docs/MIGRATING_TO_2.md` (REL-006 writes it) | Covered by the 2.0 rewrite of the target, verified against the compiler. | 0f6a8d7 (REL-006, 2026-10-13) |
| LEC-007 | `docs/MIGRATING_TO_2.md` (REL-006 writes it) | Covered by the 2.0 rewrite of the target, verified against the compiler. | 0f6a8d7 (REL-006, 2026-10-13) |
| LEC-008 | `docs/MIGRATING_TO_2.md` (REL-006 writes it) | Covered by the 2.0 rewrite of the target, verified against the compiler. | 0f6a8d7 (REL-006, 2026-10-13) |
| REL-004 | Nothing | Nothing was owed; closed by REL-006's review of the queue. | (none) (REL-006, 2026-10-13) |
| Annotations (CONSTITUTION.md Art. V) | `docs/reference/functions_and_modules.md` (new "Annotations" section) and `docs/reference/ffi_and_unsafe.md` §1 `extern fn` | Covered by the 2.0 rewrite of the target, verified against the compiler. | affa17f, 61b7eef (REL-006, 2026-10-13) |
