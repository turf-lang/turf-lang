# Conformance items: one row per frozen decision

`docs/development/FREEZE.md`'s appendix is the specification of the 2.0 language:
76 numbered decisions, `F-GEN-1` to `F-REFARR-2`. This file says, for each one,
which tests and scripts show it holds. `tests/CONFORMANCE.md` maps *areas* to
folders; this maps *rules* to evidence, which is the question a conformance
suite has to answer: "this sentence of the spec: where is it checked?"

`scripts/conformance_check.py` (CI job `doc_links`, and `session_postflight.sh`)
fails when:

- a decision in `FREEZE.md` has no row here, or a row names no such decision,
  so **a new frozen decision cannot land without its evidence being named**;
- a path matches nothing (a test was renamed or deleted);
- the evidence does not match **Needs** (below), or a `.tr` file sits in the
  wrong column (a `fail_*` test is negative evidence, anything else positive);
- a script is named as evidence that no workflow runs.

## Needs

| Needs | Meaning |
|---|---|
| `PN` | the rule has a valid form and an invalid one: a passing test or script **and** a `fail_*` test or script |
| `P` | only the valid form is observable from a program (a representation, a guarantee, an absence) |
| `N` | only the rejection is observable |
| `S` | a rule about the toolchain (exit statuses, archive layout, a file format) that only a script can observe |
| `-` | no program or script can observe it (a process rule); the Note says why and what enforces it instead |

What this proves: that evidence exists, is of the right kind, and runs. What it
cannot prove: that the evidence is *enough* for the sentence it is attached to.
Read the Note, then the test.

## Items

| Decision | Needs | Positive | Negative | Note |
|---|---|---|---|---|
| **F-GEN-1** | P | `tests/38_monomorphization/01_free_function_multi_instantiation.tr`, `tests/38_monomorphization/05_struct_multi_instantiation.tr`, `tests/38_monomorphization/06_generic_struct_over_8_bytes.tr` | — | Every instantiation gets its own code, including types over 8 bytes that erasure could not hold. |
| **F-GEN-2** | P | `tests/38_monomorphization/31_generic_walker_type_shapes.tr` | — | Enforced by an assertion in Codegen; what a program can show is that every shape of generic type compiles without reaching it. |
| **F-GEN-3** | PN | `tests/38_monomorphization/many_independent_instantiations.tr` | `tests/38_monomorphization/fail_generic_requires_operator.tr` | No constraint syntax: a missing operator is reported where the generic is instantiated. |
| **F-NUM-1** | PN | `tests/36_numeric_semantics/01_int_literal_bounds_round_trip.tr`, `tests/35_numeric_literals/08_round_trip_exact_value.tr` | `tests/36_numeric_semantics/fail_int_literal_not_exact_as_double.tr` | A literal takes the type of its destination, and is checked against it. |
| **F-NUM-2** | N | `tests/36_numeric_semantics/01_int_literal_bounds_round_trip.tr` | `tests/36_numeric_semantics/fail_int_literal_too_large.tr`, `tests/36_numeric_semantics/fail_int_literal_too_small.tr`, `tests/36_numeric_semantics/fail_int32_literal_out_of_range.tr` | Never a clamp. |
| **F-NUM-3** | PN | `tests/36_numeric_semantics/03_no_false_positive_overflow_traps.tr`, `tests/36_numeric_semantics/09_double_to_int_range_and_nan_text.tr` | `tests/36_numeric_semantics/fail_runtime_overflow_add.tr`, `tests/36_numeric_semantics/fail_runtime_overflow_mul.tr`, `tests/36_numeric_semantics/fail_runtime_overflow_div_intmin_negone.tr`, `tests/36_numeric_semantics/fail_runtime_double_to_int_nan.tr`, `tests/36_numeric_semantics/fail_runtime_double_to_int_out_of_range.tr`, `tests/36_numeric_semantics/fail_runtime_double_to_int32_out_of_range.tr`, `tests/36_numeric_semantics/fail_runtime_overflow_pow.tr` | Traps at run time on every operator that can overflow (`+ - * / unary- << += ++ **`), and on a `double → int`/`int32` conversion with no integer result (NaN, ±inf, out of range; 2026-10-12). |
| **F-NUM-4** | PN | `tests/36_numeric_semantics/05_float_overflow_is_infinity_not_a_trap.tr` | `tests/36_numeric_semantics/fail_float_literal_overflow.tr` | Run-time overflow is `inf`; an unrepresentable literal is an error. |
| **F-NUM-5** | PN | `tests/19_int32_type/` | `tests/36_numeric_semantics/fail_unsigned_integer_type_does_not_exist.tr` | `int` and `int32` only. |
| **F-NUM-6** | S | `scripts/opt_levels_smoke.sh` | — | The whole suite also runs at `-O0/-O1/-O2/-O3/-Os` in `test.yml`; this script compares outputs across levels. |
| **F-GUESS-1** | PN | `tests/37_guess_inference/01_primitive_defaults_and_reassignment.tr` | `tests/37_guess_inference/fail_guess_reassign_wrong_type.tr` | The type is fixed at the declaration. |
| **F-GUESS-2** | N | `tests/37_guess_inference/01_primitive_defaults_and_reassignment.tr` | `tests/37_guess_inference/fail_guess_no_init.tr` | No declare-then-infer. |
| **F-GUESS-3** | PN | `tests/37_guess_inference/04_function_return_and_int32.tr` | `tests/37_guess_inference/fail_guess_parameter.tr`, `tests/37_guess_inference/fail_guess_return_type.tr`, `tests/37_guess_inference/fail_guess_struct_field.tr` | Added by this map: all three positions used to be accepted or rejected with the wrong message. |
| **F-INIT-1** | PN | `tests/10_default_init/01_mut_primitive_assign_then_use.tr`, `tests/10_default_init/13_mut_branch_both_assign.tr` | `tests/10_default_init/fail_mut_use_before_init.tr`, `tests/10_default_init/fail_struct_flat_use_before_init.tr` | Definite assignment for every type. |
| **F-INIT-2** | P | `tests/10_default_init/02_struct_flat_default.tr`, `tests/10_default_init/12_struct_independence.tr` | — | Storage is zero-filled; `T::default()` is observable. |
| **F-INIT-3** | PN | `tests/38_monomorphization/29_generic_instantiation_cycle_via_reference.tr` | `tests/06_structs/fail_self_containing_struct.tr`, `tests/06_structs/fail_mutual_struct_cycle.tr`, `tests/06_structs/fail_struct_array_cycle.tr` | By-value cycles are rejected; a cycle through a reference is fine. |
| **F-MEM-1** | P | `tests/41_gc/01_retention_across_collection.tr`, `tests/41_gc/02_precise_element_tracing.tr`, `tests/41_gc/03_interior_pointer_retention.tr`, `scripts/gc_smoke.sh` | — | Also run under `TURF_GC_STRESS=1`, `TURF_GC_VALIDATE=1` and an ASan runtime in CI. |
| **F-MEM-2** | P | `tests/40_defer/01_ordering.tr` | — | There is no finalizer to test the absence of; cleanup is `defer`, and this shows it. |
| **F-MEM-3** | PN | `tests/05_arrays_pointers_ffi/14_mem_stdlib.tr`, `tests/41_gc/11_mem_root_pins_manual_heap_reference.tr` | `tests/41_gc/fail_free_gc_pointer.tr` | The two heaps stay apart: freeing a GC pointer is a panic. |
| **F-MEM-4** | S | `scripts/runtime_smoke.sh` | — | `libturfrt` is linked into every binary, and its absence is a driver diagnostic. |
| **F-MEM-5** | PN | `tests/39_nullable_references/escape_return_heap_local_ok.tr`, `tests/39_nullable_references/escape_return_param_field_ok.tr` | `tests/39_nullable_references/fail_return_address_of_local.tr`, `tests/39_nullable_references/fail_push_local_ref_into_list.tr`, `tests/39_nullable_references/fail_escape_return_after_loop.tr`, `tests/39_nullable_references/fail_store_local_ref_into_param_field.tr` | A reference into the frame may be returned only via `heap()` or a parameter's own field; the three negatives are the three sinks (return, collection, field of a parameter). |
| **F-HEAP-1** | PN | `tests/39_nullable_references/05_dynamic_linked_list.tr`, `tests/39_nullable_references/06_dynamic_binary_tree.tr` | `tests/39_nullable_references/fail_heap_of_void.tr` | `heap(x)` allocates with the real type descriptor. |
| **F-PTR-1** | PN | `tests/39_nullable_references/03_narrowing_shapes.tr` | `tests/39_nullable_references/fail_deref_without_check.tr`, `tests/39_nullable_references/fail_method_call_without_check.tr`, `tests/39_nullable_references/fail_deref_field_chain_without_variable.tr`, `tests/39_nullable_references/fail_cast_nullable_expression.tr`, `tests/39_nullable_references/fail_null_to_safe_reference.tr`, `tests/05_arrays_pointers_ffi/fail_ptr_outside_unsafe.tr` | `&T` cannot be null or dereferenced unchecked, `&?T` must be narrowed first, and a raw pointer outside `unsafe` is rejected. |
| **F-OP-1** | PN | `tests/18_operator_overload/04_operator_overload_selection.tr` | `tests/18_operator_overload/fail_operator_exact_match_no_coercion.tr`, `tests/18_operator_overload/fail_generic_operator_exact_match_no_coercion.tr` | The operand types must match the declaration exactly; no int-to-double or other widening picks an overload. |
| **F-OP-2** | PN | `tests/18_operator_overload/11_postfix_and_compound_assign.tr`, `tests/18_operator_overload/09_string_compound_assign.tr` | `tests/18_operator_overload/fail_operator_compound_assign_not_declarable.tr` | `a += b` uses `operator +`; there is no `operator +=`. |
| **F-OP-3** | P | `tests/18_operator_overload/04_operator_overload_selection.tr` | — | Overloads of an operator are chosen like overloads of a function. |
| **F-OP-4** | N | `tests/18_operator_overload/01_struct_operator_add.tr` | `tests/18_operator_overload/fail_operator_logical_and_not_overloadable.tr`, `tests/18_operator_overload/fail_operator_assignment_not_overloadable.tr`, `tests/18_operator_overload/fail_operator_member_access_not_overloadable.tr`, `tests/18_operator_overload/fail_operator_call_not_overloadable.tr` | Four of the five spellings the decision lists; conversion operators have no syntax to try. |
| **F-OP-5** | PN | `tests/18_operator_overload/17_generic_operator_list_any_element.tr`, `tests/18_operator_overload/19_generic_operator_user_struct_forms.tr` | `tests/18_operator_overload/fail_generic_operator_param_not_in_operands.tr`, `tests/18_operator_overload/fail_generic_operator_no_owned_operand.tr`, `tests/18_operator_overload/fail_generic_operator_orphan_rule.tr` | A generic operator infers its parameters from its operands; each type parameter must appear in one, and one operand must be owned by the file. |
| **F-STD-1** | PN | `tests/05_arrays_pointers_ffi/21_unsafe_module_capability_and_multi_extern.tr` | `tests/39_nullable_references/fail_raw_pointer_signature_no_capability.tr` | A privilege is a declared `unsafe module;`, not a path. |
| **F-STD-2** | PN | `tests/50_intrinsic_shadowing/03_intrinsics_still_work_when_not_shadowed.tr` | `scripts/check_boundary.sh` | The script fails if the list of intrinsics has a second home. |
| **F-STD-3** | P | `tests/12_string_ops/04_string_char_access.tr`, `tests/05_arrays_pointers_ffi/19_ffi_string_to_ptr_coercion.tr` | — | A `string` passes to C as the bytes it is. |
| **F-PRE-1** | PN | `tests/22_prelude/01_zero_import_ufcs.tr` | `tests/16_sys_and_test_stdlib/fail_bare_assert_no_import.tr` | The nine prelude modules need no import; `test` is not one of them. |
| **F-PRE-2** | PN | `tests/22_prelude/05_bare_io_math_sys_no_imports.tr` | `tests/22_prelude/fail_bare_call_of_unflattened_prelude_function.tr` | Added by this map. |
| **F-PRE-3** | PN | `tests/22_prelude/06_user_declaration_shadows_prelude_name.tr`, `tests/22_prelude/07_explicit_import_shadows_prelude_name.tr` | `tests/22_prelude/fail_user_declaration_hides_other_overloads.tr` | A file's own function, or an explicit import, hides the prelude's bare name instead of colliding with it. |
| **F-PRE-4** | PN | `tests/22_prelude/09_ufcs_receivers_own_module_breaks_a_tie.tr` | `tests/20_ufcs/fail_ufcs_ambiguous_call.tr` | In a UFCS tie the receiver's own prelude module wins; two non-prelude candidates are still ambiguous. |
| **F-DIAG-1** | S | `scripts/slm_replay_smoke.sh`, `scripts/slm_deterministic_fix_smoke.sh` | — | Every displayed suggestion was validated in the sandbox this run. |
| **F-DIAG-2** | S | `scripts/slm_replay_smoke.sh`, `scripts/slm_cache_smoke.sh` | — | A cached or replayed suggestion is re-validated, never trusted. |
| **F-DIAG-3** | - | — | — | A process rule (no new `SLM::ErrorCategory` without a request); nothing a program can show. `scripts/error_codes_check.sh` keeps the code registry consistent, which is the part a script can see. |
| **F-OPT-1** | S | `scripts/opt_levels_smoke.sh` | — | Same evidence as F-NUM-6, which is the same rule seen from the numeric side. |
| **F-OPT-2** | PN | `tests/43_check_elision/01_bounds_and_overflow_elided_still_correct.tr` | `tests/43_check_elision/fail_overflow_still_traps_when_unprovable.tr`, `tests/43_check_elision/fail_bounds_still_caught_after_elision.tr` | An index proven in range loses its check; one the compiler cannot prove still traps. |
| **F-TRACK-1** | - | — | — | A process rule: an implementation session updates a change record. `scripts/session_postflight.sh` checks it, by hand per SESSION_PROTOCOL.md; CI cannot know a session happened. |
| **F-TYPE-1** | PN | `tests/07_functions_modules/type_identity_same_name_in_two_modules.tr` | `tests/07_functions_modules/fail_type_identity_ambiguous_symbol_import_vs_local.tr`, `tests/07_functions_modules/fail_type_identity_wildcard_imports_both_declare_node.tr` | Two modules may each declare `Node`; a bare spelling that could mean either is an error, never "local wins". |
| **F-STR-1** | PN | `tests/12_string_ops/13_string_index_bounds_and_char_type.tr`, `tests/12_string_ops/14_string_negative_index.tr` | `tests/12_string_ops/fail_string_index_oob_read_runtime.tr`, `tests/12_string_ops/fail_string_index_negative_read_runtime.tr` | `s[i]` is a `char`; a negative `i` counts from the end; a read outside `-strlen(s)..strlen(s)` panics (amended 2026-10-12 for #256). |
| **F-VIS-1** | PN | `tests/07_functions_modules/visibility/visibility_pub_access.tr`, `tests/07_functions_modules/visibility/visibility_wildcard_skips_private.tr` | `tests/07_functions_modules/visibility/fail_private_field_read.tr`, `tests/07_functions_modules/visibility/fail_private_function_qualified.tr`, `tests/07_functions_modules/visibility/fail_private_in_public_return.tr` | Private by default; `pub` opens a declaration, a field, a method; a wildcard import skips private names. |
| **F-PKG-1** | S | `scripts/cli_smoke.sh` | — | The `turf`/`turfc` version handshake and passthrough. |
| **F-PKG-2** | PN | `scripts/project_smoke.sh` | `tests/07_functions_modules/stdlib_shadow/fail_import_local_module_named_like_stdlib.tr` | The single-file half is a `.tr` test; the package half is the script. |
| **F-PKG-3** | S | `scripts/git_deps_smoke.sh` | — | Lock file, `--offline`, `--locked`. |
| **F-PKG-4** | S | `scripts/project_smoke.sh` | — | `[unsafe] allow`, not transitive. |
| **F-IMP-1** | PN | `tests/07_functions_modules/import_scope_own_imports_work.tr`, `tests/07_functions_modules/import_scope_prelude_qualifier.tr` | `tests/07_functions_modules/fail_import_scope_transitive_call.tr`, `tests/07_functions_modules/fail_import_scope_transitive_type.tr` | `m::` is allowed only for a module the file imports itself; another file's import never counts. |
| **F-IMP-2** | PN | `tests/07_functions_modules/import_symbol_single.tr`, `tests/07_functions_modules/import_symbol_list.tr` | `tests/07_functions_modules/fail_import_scope_symbol_import_call.tr` | `import m::name;` brings `name` in, not the `m::` qualifier. |
| **F-IMP-3** | PN | `tests/07_functions_modules/import_scope_value_held_not_spelled.tr` | `tests/07_functions_modules/fail_import_scope_symbol_import_type.tr` | Only the spelling of `m::T` is gated; a value of an unimported type can be held and used. |
| **F-IMP-4** | P | `tests/07_functions_modules/scope_instance_method_via_symbol_import.tr`, `tests/18_operator_overload/03_operator_import_visibility.tr` | — | Methods, UFCS and operators follow the type, not the import. |
| **F-IMP-5** | PN | `tests/07_functions_modules/import_scope_own_imports_work.tr` | `tests/07_functions_modules/fail_import_scope_transitive_call.tr` | The diagnostic is pinned to its code (E145) by the test. |
| **F-PAT-1** | PN | `tests/45_pattern_matching/17_top_level_identifier_is_not_a_binding.tr`, `tests/45_pattern_matching/01_struct_destructuring_basic.tr` | `tests/45_pattern_matching/fail_pattern_old_struct_literal_still_rejected.tr`, `tests/45_pattern_matching/fail_guarded_arm_does_not_cover_enum.tr` | A top-level `matches x:` still means equality; destructuring forms bind; the old struct-literal pattern is rejected. |
| **F-ERR-1** | PN | `tests/25_errors/17_payload_declare_construct_destructure.tr`, `tests/25_errors/19_payload_pattern_forms.tr` | `tests/25_errors/fail_payload_construct_too_few_args.tr`, `tests/25_errors/fail_payload_bare_reference_with_fields.tr` | Typed error fields are built positionally and read only by patterns; a bare reference to a variant that has fields is an error. |
| **F-API-1** | S | `scripts/stdlib_surface_check.sh` | — | Plus `tests/46_stdlib_surface/`, which uses every public item. |
| **F-API-2** | S | `scripts/stdlib_surface_check.sh` | — | The script fails on a public name that is not `snake_case`. |
| **F-API-3** | PN | `tests/25_errors/12_stdlib_map_try_get.tr` | `tests/42_caller_location/fail_stdlib_list_pop_names_caller.tr`, `tests/42_caller_location/fail_stdlib_map_get_names_caller.tr` | Failure shapes: `!T`, a panic that names the caller, a predicate. |
| **F-API-4** | - | — | — | A review rule about what to make public; `scripts/stdlib_surface_check.sh` locks the result, not the judgement. |
| **F-CLI-1** | S | `scripts/cli_smoke.sh` | — | Exit statuses 0 / 1 / 2. |
| **F-CLI-2** | S | `scripts/cli_smoke.sh` | — | An unknown option is a usage error with a suggestion. |
| **F-CLI-3** | S | `scripts/cli_smoke.sh` | — | `turfc --help` is deliberately unknown; help is `turfc help`. |
| **F-CLI-4** | S | `scripts/cli_smoke.sh` | — | Version lines 1 and 2 are the contract. |
| **F-REL-1** | S | `scripts/release_smoke.sh` | — | Archive layout. |
| **F-REL-2** | S | `scripts/cross_target_smoke.sh` | — | A foreign `--target` emits its object and stops with a Turf diagnostic. |
| **F-REL-3** | S | `scripts/release_smoke.sh` | — | Checksum, PATH block, uninstall. |
| **F-REL-4** | S | `scripts/release_smoke.sh` | — | The archive version is `VERSION`. |
| **F-TEST-1** | S | `scripts/test_cmd_smoke.sh` | — | A project without tests is an error. |
| **F-TEST-2** | S | `scripts/test_cmd_smoke.sh`, `scripts/test_runner.sh` | — | Each convention is broken on purpose and the run must fail for that reason. |
| **F-TEST-3** | S | `scripts/test_cmd_smoke.sh` | — | A project's tests import its modules, dependencies and helpers. |
| **F-FMT-1** | S | `scripts/fmt_smoke.sh` | — | Every result is verified before it is written. |
| **F-FMT-2** | S | `scripts/fmt_smoke.sh` | — | A lexical error is reported, left unchanged, and the batch continues. |
| **F-FMT-3** | S | `scripts/fmt_smoke.sh` | — | stdout / `--write` / `--check`. |
| **F-COLL-1** | PN | `tests/51_shared_collections/01_list_shared_and_copy.tr`, `tests/51_shared_collections/02_stack_queue_shared_and_copy.tr`, `tests/51_shared_collections/03_set_map_shared_and_copy.tr`, `tests/51_shared_collections/04_collection_in_struct.tr`, `tests/41_gc/14_shared_collection_header_survives_collection.tr` | `tests/07_functions_modules/visibility/fail_private_stdlib_list_data.tr` | `=`, passing and storing share one header; `.copy()` is independent; the header is private. |
| **F-COLL-2** | PN | `tests/10_default_init/18_reference_field_default.tr`, `tests/38_monomorphization/15_generic_struct_field_only.tr` | `tests/10_default_init/fail_default_reference_cycle.tr`, `tests/10_default_init/fail_default_reference_cycle_through_value.tr` | A `&U` field defaults to a fresh `U::default()`; a cycle through `&` is E147. |
| **F-REFARR-1** | PN | `tests/52_arrays_of_references/01_array_of_references.tr`, `tests/52_arrays_of_references/03_nullable_array_literals.tr` | `tests/52_arrays_of_references/fail_parentheses_around_non_reference.tr`, `tests/52_arrays_of_references/fail_parentheses_without_array.tr`, `tests/52_arrays_of_references/fail_escape_into_slice_parameter.tr` | `(&N)[k]` is an array of references and `&N[k]` a reference to an array; parentheses around any other type, or without `[`, are E109; a frame reference stored into a `(&N)[]` parameter's element is E086. |
| **F-REFARR-2** | PN | `tests/52_arrays_of_references/04_arrays_of_structs_with_references_initialised.tr`, `tests/05_arrays_pointers_ffi/zero_filled_string_array_elements_are_empty.tr` | `tests/52_arrays_of_references/fail_array_of_references_without_initializer.tr`, `tests/52_arrays_of_references/fail_array_of_structs_holding_reference_without_initializer.tr` | A `(&N)[k]`, or an array whose element carries a `&U`, needs an initializer (E128); `(&?N)[k]` starts all-null; an unwritten string element reads `""`. |
