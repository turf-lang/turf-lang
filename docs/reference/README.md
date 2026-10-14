# TurfLang Language Reference

The reference for TurfLang 2.0: what you can write and what it means. Every TurfLang example in these pages is compiled by CI (`scripts/check/doc_examples.py`): an example either compiles, or is marked with the error code it must be rejected with.

Coming from 1.x? Start with the [migration guide](../MIGRATING_TO_2.md). For step-by-step walkthroughs (a project, a library, a test suite) see the [guides](../guides/README.md). For how the compiler implements the language, see [`docs/architecture/`](../architecture/); each page here links to its counterpart where one exists.

## Suggested reading order

**Core language**
1. [Variables & Types](variables_and_types.md): declarations, `guess` inference, definite assignment, the primitive types, literals, overflow, `to` conversions, `sizeof`/`typeof`
2. [Operators](operators.md): the precedence table, arithmetic and its runtime checks, comparison, logical, bitwise, `give`/`otherwise`
3. [Control Flow](control_flow.md): `if`, `while`, `do`/`while`, `for`, `break`/`continue`, `defer`, `compare` and the pattern language
4. [Error Handling](error_handling.md): `error` types and payloads, `!T`, `recover`, `try`, caller-located panics

**Structuring programs**
5. [Functions & Modules](functions_and_modules.md): `fn`, first-class functions, UFCS, annotations, `import`, the prelude, `pub` visibility
6. [Structs & Enums](structs_and_enums.md): declaration, construction, methods, layout cycles, self-referential types, `enum`
7. [State Machines](state_machines.md): the `machine` construct and its checked transitions
8. [Generics & Operator Overloading](generics_and_operators.md): monomorphized generics, the overloadable operators, indexing operators
9. [Arrays](arrays.md): fixed-size arrays, `T[]` parameters and slicing, bounds checking

**Memory and systems programming**
10. [Memory & Safety](memory_and_safety.md): the two heaps, `heap(expr)`, `&T`/`&?T`, escape rules, runtime checks, what safe code guarantees
11. [FFI & `unsafe`](ffi_and_unsafe.md): `extern fn`, linking C libraries, raw pointers, `unsafe` blocks and modules, capabilities

**Standard library** (in [`stdlib/`](stdlib/)). Start with the [conventions and the 2.x surface](stdlib/README.md): naming, the failure rule, `defer` for resources, what is public.
12. [Core](stdlib/core.md): `int`, `double`, `bool`, `char`, `array`, `strings` (the prelude)
13. [Collections](stdlib/collections.md): `List`, `Stack`, `Queue`, `Map`, `Set`
14. [I/O & Networking](stdlib/io_and_net.md): `io` (including files), `sys`, `net`
15. [Memory](stdlib/mem.md): `mem`, `Pool`
16. [Math & OS](stdlib/math_and_os.md): `math`, `os`
17. [Time](stdlib/time.md): wall clock, CPU time, sleeping
18. [Random](stdlib/random.md): pseudo-random numbers (not cryptographic)
19. [Files & Directories](stdlib/fs.md): paths and directory operations
20. [JSON](stdlib/json.md): parsing and serializing `JsonValue`
21. [HTTP](stdlib/http.md): `Request`/`Response` and a callback-based `Server`
22. [Testing](stdlib/testing.md): the `test` module and `turf test`

**Tooling**
23. [Projects & Packages](projects_and_packages.md): `turf.toml`, `path` and `git` dependencies, `turf.lock`, how imports resolve across packages
24. [CLI & Diagnostics](cli_and_diagnostics.md): the `turf` commands and flags, the diagnostic format and every error code, static analysis, runtime failures, `--smart`
