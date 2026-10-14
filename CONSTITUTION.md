# The Turf Design Philosophy

## Preamble

Turf is a compiled, statically typed systems language for people who want native speed and memory safety without having to learn an ownership calculus first. It is built on the belief that a language earns trust through guarantees it can demonstrate, not through features it can list.

This document describes what Turf is for and the principles that hold when features, syntax, and implementation change. Details live in the language specification. Principles live here.

## The Principles

### I. Safe by default, unsafe by declaration

Safe Turf code cannot corrupt memory. Programmers do not write lifetimes or borrow annotations to get this; the language and its runtime carry that responsibility. When a program needs to step outside the guarantees, for raw pointers or foreign code, it says so explicitly, and the unsafe region is visible at the point of use. Unsafe is a tool, never a default.

### II. Native performance, with costs you can read

Turf compiles ahead of time to native code through LLVM. The cost of an operation should be predictable from the code that spells it. Where Turf makes a performance claim, the claim is backed by published measurements, and a regression against them is treated as a bug.

### III. The compiler is the authority

Whether a program is valid is decided by the compiler, deterministically, with no network, no model, and no hidden state. The same source produces the same diagnostics every time. Diagnostics are designed, not incidental: stable error codes, precise source locations, an explanation available for each code, and a test suite that checks they fire for the right reason. Optional tooling, including AI assistance on request, may help a programmer understand or fix a problem. It never decides one.

### IV. Nothing crosses a boundary implicitly

Visibility, imports, errors, and foreign calls are explicit where they happen. Declarations are private unless made public. A module sees only what it imports itself, not what its imports imported. Failure is a value the programmer handles, not an invisible exit path. When code reaches across a boundary, the reader can see it.

### V. A few general mechanisms, not many special cases

The standard library gets no privileges that user code lacks. Strings, collections, and operators are built from the same mechanisms available to everyone. When the compiler special-cases a type, that is a defect to be removed. The language grows by adding general tools, not exceptions.

The compiler may know a fact about a function only because the function's declaration says so, never because of its name. A user's allocator is understood by the compiler exactly as the standard library's is, because both say what they are in the same way. We never know in advance where the compiler will need to be told something, so the way of telling it is a language mechanism open to everyone, not a private list of names.

The one exception is the language's own vocabulary. Its keywords, and a closed, enumerated list of intrinsics such as `sizeof`, `typeof`, `heap` and the `gc_*` family, are part of the language itself, not library functions, and there is no declaration for them to carry a fact. That list is written in one place and changed only as a recorded decision. Even there, a name never beats a declaration: a program's own function that happens to share an intrinsic's name is the function that gets called.

### VI. It works when you install it

A single toolchain provides the compiler, formatter, test runner, language server, and documentation tooling. A new project builds without configuration and a release is self-contained. The first hour with Turf should be spent writing Turf, not setting up an environment.

### VII. Stability is a promise

A public interface, once released, is not broken casually. When a change must break something, it comes with a deliberate decision and a migration path. People can build on Turf because Turf commits to staying where it is put.

### VIII. A claim is not true until it is demonstrated

A guarantee that is documented but not enforced is a bug. Safety is shown by tests, sanitizers, and real programs, not asserted. Turf is developed in the open about what it does not yet guarantee, and that candor is part of the design.

## Non-Goals

These are deliberate. They protect Turf from becoming something it is not.

- **Turf will not add a borrow checker.** Memory safety comes from the runtime and from explicit unsafe boundaries, not from ownership-and-lifetime analysis. If that trade-off is wrong for a program, Rust is the better tool, and that is acceptable.
- **Turf will not target no-runtime embedded systems.** Turf has a runtime. Environments that cannot host one are not Turf's territory.
- **Turf will not put AI in the compilation path.** No model is consulted to accept, reject, or compile a program. AI exists only as optional, on-demand assistance with diagnostics, guided by what the compiler has already decided.

## Where Turf Sits

Rust gets its safety from its type system and pays in programmer effort. C gets its speed from trusting the programmer and pays in safety. Go gets its simplicity from a runtime and a small feature set. Turf aims to offer native speed and enforced safety with the programmer's effort spent on the problem, not on the memory model, with diagnostics clear enough to teach the language as you use it.

## Amendment

The principles above are changed rarely and openly, with the reasoning published. Everything else in Turf, including syntax, standard library, and implementation, is expected to evolve.
