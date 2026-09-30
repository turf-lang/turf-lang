# Turflang Language Reference

This is the complete, verified reference for the Turflang language surface — what you can write and what it means. Every page here is grounded directly in the current compiler source and the real regression test suite (`tests/`), not carried forward from older drafts, so it should be trusted as accurate against the tree it ships alongside.

For internals — how the compiler actually implements any of this (stack vs. heap layout, the pass structure, type checking, static analysis) — see [`docs/architecture/`](../architecture/) instead. Each page below cross-references its architecture counterpart where one exists, rather than duplicating internals content.

## Suggested reading order

**Core language**
1. [Variables & Types](variables_and_types.md) — declarations, primitive types, literals, casting
2. [Operators](operators.md) — arithmetic, comparison, logical, bitwise, the `give`/`otherwise` ternary
3. [Control Flow](control_flow.md) — `if`, `while`, `do`/`while`, `for`, `defer`, `compare`
4. [Error Handling](error_handling.md) — `error` types, `!T` unions, `recover`, `assert`/`fail`

**Structuring programs**
5. [Functions & Modules](functions_and_modules.md) — `fn`, first-class functions, UFCS, `import`, `::`
6. [Structs & Enums](structs_and_enums.md) — declarations, methods, construction, `enum`
7. [State Machines](state_machines.md) — the `machine` construct: compile-time-checked state transitions
8. [Generics & Operator Overloading](generics_and_operators.md) — `List<T>`-style generics, `operator`
9. [Arrays](arrays.md) — fixed-size arrays, bounds checking

**Systems programming**
10. [FFI & `unsafe`](ffi_and_unsafe.md) — `extern fn`, `unsafe { }`, `&T` references, raw pointers

**Standard library** (in [`stdlib/`](stdlib/))
11. [Core](stdlib/core.md) — `int`/`double`/`bool`/`char`/`array`/`strings` (the prelude)
12. [Collections](stdlib/collections.md) — `List`/`Stack`/`Queue`/`Map`/`Set`
13. [I/O & Networking](stdlib/io_and_net.md) — `io` (incl. file I/O), `net` (incl. DNS resolution), `sys`
14. [Memory](stdlib/mem.md) — `mem`, `Pool<T>`
15. [Math & OS](stdlib/math_and_os.md) — `math`, `os`
16. [Time](stdlib/time.md) — wall clock, CPU-time measurement, sleeping
17. [Random](stdlib/random.md) — `random_int`/`random_double`/`random_bool` (not cryptographic)
18. [Filesystem](stdlib/fs.md) — path manipulation, directory operations
19. [JSON](stdlib/json.md) — a dependency-free `JsonValue` parser/serializer
20. [HTTP](stdlib/http.md) — `Request`/`Response` parsing/building over `net`, a callback-based `Server`
21. [Testing](stdlib/testing.md) — `test`

**Tooling**
22. [Projects & Packages](projects_and_packages.md) — `turf.toml`, dependencies (`path`/`git`), `turf.lock`, how imports resolve across packages, the `turf new`/`build`/`run`/`add`/`update` commands
23. [CLI & Diagnostics](cli_and_diagnostics.md) — the `turf` command, error codes, `--explain`, compile-time static analysis

## Scope note

SLM-assisted tooling (`--smart`, `turf --explain`'s per-code writeups in `docs/slm/`) is documented only at the surface level here — its own dedicated documentation pass is still to come.

`Map<K, V>`/`Set<T>` have one documented, unresolved limitation: lookups compare `K`/`T` by raw identity rather than content when `K`/`T = string` (see [`stdlib/collections.md`](stdlib/collections.md) for the full mechanism and a working non-generic-method pattern that sidesteps it). Nothing else in the standard library has a known live bug as of this writing.
