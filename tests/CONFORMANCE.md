# Conformance map

REL-004 stage 1. `docs/planning/ROADMAP_v2.0.0.md` Part VII lists the areas a
2.0 compiler must be tested on, "positive **and** negative". This file says
which tests do that for each, and `scripts/conformance_check.sh` (run by CI
and by `session_postflight.sh`) fails when the map and the tree disagree.

`tests/` stays the regression suite, organised by topic folder; nothing here
moves a test. The map only points at them.

**Two maps, two questions.** This file answers *"which tests cover this area?"*
(21 areas, rows are folders). `tests/CONFORMANCE_ITEMS.md` answers *"where is
this rule checked?"*: one row for each of the 72 frozen decisions in
`docs/development/FREEZE.md` (`F-GEN-1` ... `F-FMT-3`), naming the tests and
scripts that show it holds, and the check fails if a decision has no row. The
second is the conformance suite in the sense of the word; this one is the
inventory it sits in.

## How to read it

- **Positive** is evidence that the valid form compiles and behaves; **Negative**
  is evidence that the invalid form is rejected (a `fail_*.tr` pinned with
  `// EXPECT-DIAG:` / `// EXPECT-RUNTIME:`, so it fails for the reason the
  area names) or, for the areas that are processes rather than language
  (targets, packages, distribution, debug info), the failure-case checks of the
  smoke script named.
- A cell is a list of repository paths in backticks. A path may be a file, a
  directory (everything below it) or a glob. The checker requires that every
  path exists, that every `.tr` file in a **Negative** cell is a `fail_*` test,
  and that no area has an empty side.
- **Adding an area, or a feature that belongs to one:** add its tests to the
  row in the same change. **Moving or renaming a test** that a row names makes
  the check fail until the row is updated, which is the point.
- What the check cannot judge is whether the evidence is *enough*. That is
  the reviewer's job, and the "Known thin spots" section below is where a
  reviewer who finds a hole writes it down until a test closes it.

## Areas

| Area | Positive | Negative |
|---|---|---|
| Lexer/parser | `tests/01_syntax/`, `tests/35_numeric_literals/` | `tests/01_syntax/fail_*.tr`, `tests/35_numeric_literals/fail_*.tr` |
| Type system | `tests/03_typechecker/`, `tests/17_char_type/`, `tests/19_int32_type/` | `tests/03_typechecker/fail_*.tr`, `tests/17_char_type/fail_*.tr`, `tests/19_int32_type/fail_*.tr` |
| Generics & monomorphization | `tests/38_monomorphization/` | `tests/38_monomorphization/fail_*.tr` |
| Numeric semantics | `tests/36_numeric_semantics/`, `tests/11_bitwise_ops/` | `tests/36_numeric_semantics/fail_*.tr`, `tests/11_bitwise_ops/fail_*.tr` |
| Initialization | `tests/10_default_init/` | `tests/10_default_init/fail_*.tr` |
| Operators | `tests/18_operator_overload/` | `tests/18_operator_overload/fail_*.tr` |
| References & pointers | `tests/39_nullable_references/`, `tests/05_arrays_pointers_ffi/raw_pointer_index_inside_unsafe_ok.tr`, `tests/05_arrays_pointers_ffi/18_unsafe_block_and_safe_reference.tr` | `tests/39_nullable_references/fail_*.tr`, `tests/05_arrays_pointers_ffi/fail_raw_pointer_index_outside_unsafe.tr`, `tests/05_arrays_pointers_ffi/fail_ptr_outside_unsafe.tr`, `tests/05_arrays_pointers_ffi/fail_address_of_non_addressable.tr` |
| Error handling | `tests/25_errors/` | `tests/25_errors/fail_*.tr` |
| Pattern matching | `tests/45_pattern_matching/` | `tests/45_pattern_matching/fail_*.tr` |
| Visibility & modules | `tests/07_functions_modules/visibility/`, `tests/02_resolver/`, `tests/20_ufcs/` | `tests/07_functions_modules/visibility/fail_*.tr`, `tests/02_resolver/fail_*.tr`, `tests/20_ufcs/fail_*.tr` |
| Runtime | `tests/36_numeric_semantics/03_no_false_positive_overflow_traps.tr`, `scripts/runtime_smoke.sh` | `tests/41_gc/fail_stack_overflow_deep_recursion.tr`, `tests/41_gc/fail_oom_huge_direct_allocation.tr`, `tests/22_prelude/fail_bare_panic_aborts.tr`, `tests/36_numeric_semantics/fail_runtime_overflow_add.tr` |
| GC | `tests/41_gc/`, `tests/08_e2e_stress/06_gc_bounded_allocation.tr`, `scripts/gc_smoke.sh` | `tests/41_gc/fail_*.tr` |
| Manual memory | `tests/05_arrays_pointers_ffi/14_mem_stdlib.tr`, `tests/05_arrays_pointers_ffi/17_mem_pool_and_aligned_alloc.tr`, `tests/41_gc/11_mem_root_pins_manual_heap_reference.tr`, `scripts/mem_smoke.sh` | `tests/05_arrays_pointers_ffi/fail_double_free.tr`, `tests/05_arrays_pointers_ffi/fail_use_after_free.tr`, `tests/41_gc/fail_free_gc_pointer.tr`, `tests/41_gc/fail_mem_unroot_unpinned.tr` |
| `defer` | `tests/40_defer/` | `tests/40_defer/fail_*.tr` |
| FFI | `tests/05_arrays_pointers_ffi/02_ffi_ptr_verify.tr`, `tests/05_arrays_pointers_ffi/19_ffi_string_to_ptr_coercion.tr`, `tests/05_arrays_pointers_ffi/20_extern_fn_function_local.tr`, `tests/05_arrays_pointers_ffi/21_unsafe_module_capability_and_multi_extern.tr`, `tests/21_extern_conflict/`, `tests/49_annotations/ffi_forwards_to_named_symbol.tr` | `tests/05_arrays_pointers_ffi/fail_void_extern_parameter.tr`, `tests/05_arrays_pointers_ffi/fail_unknown_type_extern_param.tr`, `tests/05_arrays_pointers_ffi/fail_fn_signature_raw_pointer_disallowed.tr`, `tests/21_extern_conflict/fail_*.tr` |
| Standard library | `tests/13_string_stdlib/`, `tests/16_sys_and_test_stdlib/`, `tests/23_stdlib_primitives/`, `tests/27_os_and_math_stdlib/`, `tests/28_io_file_io/`, `tests/29_time_stdlib/`, `tests/30_random_stdlib/`, `tests/31_fs_and_path_stdlib/`, `tests/32_set_and_sort_stdlib/`, `tests/33_net_dns/`, `tests/34_http_and_json/`, `tests/46_stdlib_surface/`, `tests/51_shared_collections/` | `tests/13_string_stdlib/fail_*.tr`, `tests/16_sys_and_test_stdlib/fail_*.tr`, `tests/28_io_file_io/fail_*.tr`, `tests/30_random_stdlib/fail_*.tr`, `tests/32_set_and_sort_stdlib/fail_*.tr`, `tests/34_http_and_json/fail_*.tr` |
| Diagnostics | `tests/44_slm_diagnostics/`, `tests/47_diagnostic_file_headers/`, `tests/14_cfg_analysis/`, `tests/49_annotations/`, `scripts/error_codes_check.sh` | `tests/44_slm_diagnostics/fail_*.tr`, `tests/47_diagnostic_file_headers/fail_*.tr`, `tests/14_cfg_analysis/fail_*.tr`, `tests/49_annotations/fail_*.tr` |
| Optimizer | `tests/43_check_elision/`, `scripts/opt_levels_smoke.sh` | `tests/43_check_elision/fail_*.tr` |
| Debug info | `scripts/debuginfo_smoke.sh`, `scripts/tree_programs_smoke.sh` | `scripts/debuginfo_smoke.sh`, `scripts/tree_programs_smoke.sh` |
| Targets | `scripts/cross_target_smoke.sh` | `scripts/cross_target_smoke.sh` |
| Packages & distribution | `scripts/project_smoke.sh`, `scripts/package_graph_smoke.sh`, `scripts/examples_smoke.sh`, `scripts/release_smoke.sh` | `scripts/project_smoke.sh`, `scripts/package_graph_smoke.sh`, `scripts/release_smoke.sh` |

## Properties checked across the whole suite rather than per area

| Property | Where it is checked |
|---|---|
| Every `-O` level gives the same behaviour (INV-OPT-1) | `.github/workflows/test.yml`: the suite at `-O0/-O1/-O2/-O3/-Os`, plus `scripts/opt_levels_smoke.sh` |
| `-g` changes nothing observable | the suite at `-O0 -g` and `-g` (`test.yml`); `llvm-dwarfdump --verify` on every showcase (`scripts/tree_programs_smoke.sh`) |
| The collector finds every root | the suite under `TURF_GC_STRESS=1`, `TURF_GC_VALIDATE=1`, with an ASan runtime (`test.yml`); the other `-O` levels nightly (`.github/workflows/nightly.yml`) |
| No sanitizer report in the compiler or the runtime | the suite with `TURF_SANITIZE_COMPILER` (job `compiler_sanitizers`) and with `TURF_RT_SANITIZE` (job `gc_hardening_asan`) |
| Invalid input produces diagnostics, never a crash | `scripts/fuzz/crash_fuzz.py`: a short pass per push (job `compiler_sanitizers`); the long run is turf-fuzz's weekly workflow (sanitized compiler, master); a finding's minimized program is added to `tests/` with its fix |
| `lib/std` and `showcase/` build | `scripts/tree_programs_smoke.sh` (the test runner only covers `tests/`) |
| The reference docs' examples compile | `scripts/doc_examples.py` (CI step "Reference-doc examples compile"): 289 `turflang` blocks (2026-10-12), 151 compile or fail as marked, 5 are marked `skip`, 133 are in `docs/reference/DOC_EXAMPLES_GAPS.txt` for REL-006 to drain, labelled `NOT-SELF-CONTAINED` (72), `NEEDS-REVIEW` (26), `SHOWS-ERROR` (22), `PLACEHOLDER` (13). `NEEDS-REVIEW` is where the real drift is: stdlib examples that use raw pointers (E130) and private fields (E088) the compiler now rejects |
| Every `fail_*` test fails for the reason it names | `// EXPECT-DIAG:` on every one, enforced by the runner; `tests/DIAGNOSTIC_GAPS.txt` is the register of the exceptions |

## What the map does not prove (measured 2026-10-11)

The check in `scripts/conformance_check.py` says every area has *some* positive
and *some* negative test. These numbers say how far that is from "enough":

| Measure | Value |
|---|---|
| Source lines of `src/` executed by `test_runner.sh` (gcov, `-DTURF_COVERAGE=ON`, 1200 tests) | **70.8%** (15 691 of 22 153) |
| The same, leaving out the five files other binaries cover (`AnalysisFacade`, `Formatter`, `PackageGraph`, `Sandbox`, `SLMIntegration`: 0% here, by `turf-api-tests`, `fmt_smoke.sh`, `project_smoke.sh`, the SLM replay scripts) | about **79%** |
| Weakest core files by line | `SymbolTable.cpp` 52%, `ParserModule.cpp` 64%, `ResolverModuleTable.cpp` 66%, `Lint.cpp` 71%, `ParserStmt.cpp` 73%, `CodegenCore.cpp` 73%. **These overstate the gaps**: gcov credits a header's inline functions to every file that includes it, and most of the uncalled "functions" are STL instantiations, `--dump-*` printers, and code only the facade or the package-graph scripts reach. Reading the uncalled functions found two real ones: `getDebugType` (118 lines; reached only by `-g`, which is why the `-g` suite runs exist) and `IdenticalBranchesWarning` (see below) |
| Live error codes with no `// EXPECT-DIAG: Exxx` test (6 more are retired) | **2**, down from 8: E091 (a package-graph error, pinned by `scripts/project_smoke.sh`) and E121 (the internal-compiler-error code, which no valid test should be able to raise). E009, E038, E043, E074, E075 and E094 got tests |
| `FREEZE.md` decisions (`F-*`) with named evidence | 72 of 72 since `CONFORMANCE_ITEMS.md` (it was 16 of 72 named in any test). Anchoring them found one rule broken (F-GUESS-3) and five with no test at all |
| `INVARIANTS.md` entries (`INV-*`) named by any test | 9 of 68 |

Line coverage counts a line run, not a condition decided, so 79% is an upper
bound on what the suite checks. `CONFORMANCE_ITEMS.md` ties tests to the 72 `F-*` decisions only. The
`INV-*` invariants and the longer prose of `FREEZE.md` §6 are not anchored yet,
which is why the `INV-*` row is low even where the behaviour is tested. Reproduce the coverage figure with:

    cmake -S . -B build_cov -DCMAKE_BUILD_TYPE=Debug -DTURF_SMART=OFF -DTURF_BUILD_CLI=OFF -DTURF_COVERAGE=ON
    cmake --build build_cov -j8
    ulimit -s 65536; TURF_COMPILER=$(pwd)/build_cov/turfc ./scripts/test_runner.sh
    cd build_cov && for g in $(find CMakeFiles -name '*.gcda'); do gcov -n $g; done

Use it to find code no test reaches, not as a gate.

## Known thin spots

Written down by whoever found them, deleted by whoever closes them. An entry
here is not a licence to leave an area without a negative test: the checker
rejects that regardless.

- **Runtime: bootstrap and the ABI stamp** are exercised by `scripts/runtime_smoke.sh`
  alone, not by a `.tr` test, because a test program cannot observe a runtime
  that failed to start.
- **Parser nesting** is bounded two ways: a recursion counter during parsing
  (E111), and a depth measurement of each finished top-level declaration
  (`AstDepthProbe`). Only the second sees a tree built from finished pieces
  (`((x--…--)--…--)`). Its limit, 2000, is the parser's, not a measurement of any
  pass's stack use; the sanitizer job raises the stack limit
  (`ulimit -s 65536`) for the same reason.
- **The identical-branches lint almost never fires.** `IdenticalBranchesWarning`
  compares branches by a fingerprint that covers only literals, variables and
  binary operations (`Lint.cpp`, `Fingerprint()`); an assignment, a call or a
  `return` gets a unique fingerprint, so `if c { y = 0; } else { y = 0; }` is not
  reported. `tests/14_cfg_analysis/42_identical_branches_warn.tr` covers the shape
  it does see. Widening it is a design change (false positives), not a test.
