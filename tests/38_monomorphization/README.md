# 38_monomorphization

Regression tests for TYPE-002 (`docs/development/changes/TYPE-002.md`),
landing incrementally across sessions S11b through S14b. Empty as of S11b
(the session standing up the monomorphizer's skeleton) — real instantiation
doesn't start until S12a; this folder exists ahead of any test in it so the
change record's *Test landing schedule* has somewhere real to point at.

**INV-TEST-1** (`docs/development/INVARIANTS.md`): a test in this folder
that keys a collection by `string` (`Map<string, V>`, `Set<string>`) must
build that key at **runtime**, never from two identical string literals.
The compiler's linker can merge identical string literals into one
`.rodata` object, which makes two literal keys the same *pointer* even
when the bug this folder exists to catch (§9.2's *"No per-`T` operator
dispatch"* — `Map<K,V>`'s `K==K` comparison silently falling back to a raw
pointer compare instead of `operator==(string,string)`) is still fully
present. A pointer-identical pair of literals passes either way, which is
exactly how `tests/07_functions_modules/09_stdlib_map.tr` let this bug
through undetected before (see `tests/99_known_bugs/set_string_dedup_broken.tr`,
which S14b moves into this folder once it actually passes). Build the key
with something the linker cannot fold — string concatenation of two
runtime values, a value read from an argument, `strings::substring()` on a
longer runtime string, etc. — never write the same string literal twice
and compare the results.
