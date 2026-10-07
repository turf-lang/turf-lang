# Functions and Modules

This page covers how to declare and call functions in Turflang, how to pass
functions around as values, uniform function call syntax (UFCS), and the
module/import system, including the `::` scope-resolution operator and the
auto-imported prelude.

See also: `docs/architecture/modules_and_scope_resolution.md` for the
compiler-internals view of the module system (hoisting, resolver passes,
file:line references) if you need to go deeper than "how do I write this."

---

## 1. Function declarations

### Basic syntax

```turflang
fn add(a: int, b: int): int {
    return a + b;
}

fn greet(): void {
}
```

- `fn <name>(<param>: <type>, ...): <returnType> { <body> }`.
- The return type is **always required**, even when a function returns
  nothing — use `void`:

  ```turflang
  fn greet(): void {
  }

  greet(); // a void function can be called as a bare statement
  ```

- A parameter's type can never be `void` — `fn f(x: void)` is rejected at
  parse time.
- Ordinary numeric widening applies to arguments and return values, e.g. an
  `int` literal passed where a `double` is expected:

  ```turflang
  fn toDouble(x: double): double {
      return x;
  }

  mut result: double = toDouble(42); // int -> double widening
  ```

### Struct methods

Methods are declared with a `.` between the struct name and the method
name, right after `fn`:

```turflang
struct MathUtil {}

fn MathUtil.square(x: int) static: int {
    return x * x;
}

fn MathUtil.describe(): string {
    // A legitimate instance method: `self` is available even though this
    // particular struct has no fields to read from it.
    return "a MathUtil instance";
}

fn main(): int {
    MathUtil::square(5);           // static methods: called via `::`, no instance
    mut mu: MathUtil = MathUtil::default();
    mu.describe();                 // instance methods: called via `.`, on a value
    return 0;
}
```

- **Instance methods** (`fn StructName.method(...): RetType`) implicitly
  receive `self` as their first parameter and are called with `.` on a
  value: `mu.describe()`.
- **Static methods** add the `static` keyword right after the parameter
  list, before the return type: `fn StructName.method(...) static: RetType`.
  They receive no `self` and are only ever callable as
  `StructName::method(args)` — **never** through an instance, and never
  with `.` on the bare type name (see [§4.3](#43--vs--the-hard-rule)).
- `default` can't be hand-declared as a method name, and `new` can never be
  declared `static` — both names are reserved for construction
  (`StructName::new(...)`, `StructName::default()`).

> **Note:** Struct construction only uses `StructName::new(args)` /
> `StructName::default()`. The old brace-literal form
> (`StructName { field: val }`) does not exist in the language.

### File-level pointer

`src/ParserDecl.cpp` (`ParseFuncDef`) — declaration parsing, `self`
injection, `static` handling.

---

## 2. First-class functions

Functions can be passed as arguments, stored in variables, and returned
from other functions using the function-type spelling `fn(T1, T2, ...): R`.

### The `fn(...):R` type

```turflang
fn apply(f: fn(int): int, x: int): int {
    return f(x);
}
fn double_it(n: int): int { return n * 2; }

apply(double_it, 5); // == 10, pass as argument
```

```turflang
mutable f: fn(int): int = double_it; // store in a variable
f = triple_it;                       // reassign to a different function
f(3);
```

```turflang
fn add(a: int, b: int): int { return a + b; }
fn sub(a: int, b: int): int { return a - b; }

fn chooseOp(useAdd: bool): fn(int, int): int { // return a function
    if useAdd {
        return add;
    }
    return sub;
}

mutable op: fn(int, int): int = chooseOp(true);
op(3, 4); // == 7
```

The function-type parameter list is **types only** — no parameter names —
followed by a mandatory `: <returnType>`.

### A realistic example — `reduce()` over an array

```turflang
fn reduce(arr: int[5], f: fn(int, int): int, init: int): int {
    mutable acc: int = init;
    for i in 0..5 step i += 1 {
        acc = f(acc, arr[i]);
    }
    return acc;
}

fn add(a: int, b: int): int { return a + b; }
fn mul(a: int, b: int): int { return a * b; }

mutable nums: int[5] = [1, 2, 3, 4, 5];
reduce(nums, add, 0);   // 15
reduce(nums, mul, 1);   // 120
```

### Rules and gotchas

- **A bare function name used as a value** (not called) resolves under the
  same visibility rules as an ordinary bare call: it has to be declared in
  the current file, be a self-reference within its own module, or have been
  brought in by an on-demand import (see [§5](#5-import-forms)). "Every
  function in the program" is not visible this way.

- **Overloaded names cannot be used as first-class values.** If more than
  one overload matches the bare name, this is a hard compile error:

  ```turflang
  fn combine(a: int, b: int): int { return a + b; }
  fn combine(a: double, b: double): double { return a - b; }

  mutable f: fn(int, int): int = combine; // ERROR:
  // 'combine' is overloaded — it can't be used as a first-class function
  // value without an unambiguous single signature.
  ```

- **`Module::func` and `StructName::method` cannot be used bare as
  first-class values** — only an unqualified free-function name can be.
  `::`-qualified names are for *calling*, not for taking a reference:

  ```turflang
  mutable f = io::printline;      // ERROR — not a first-class function
  mutable g = MathUtil::square;   // ERROR — not a first-class function

  mutable f2 = printline;         // OK, if `printline` was on-demand imported
  mutable g2 = square;            // OK, if `square` is an unqualified free function
  ```

  Two different function types are considered compatible when their
  rendered signatures match exactly (e.g. both are `fn(int, int): int`) —
  effectively structural equality on the parameter/return types.

---

## 3. UFCS (uniform function call syntax)

UFCS lets you call an ordinary free function using method-call (`.`)
syntax: `receiver.freeFunctionName(remainingArgs...)` calls
`freeFunctionName(receiverType, ...remainingParams)` as if it were a method
on `receiverType`. This is the **only** way dot-call syntax ever works on
primitives (`int`, `double`, `char`, `string`, arrays) — they have no bound
methods of their own.

```turflang
fn double_it(x: int): int { return x * 2; }
mut n: int = 21;
n.double_it();   // == 42, UFCS on an int receiver

fn is_long(s: string): bool { return strings::length(s) > 3; }
mut s: string = "hello";
s.is_long();      // true, UFCS on a string receiver
```

### Resolution order

1. **Bound method first.** If the receiver is a struct with an actual
   `.`-declared instance method of that name at a matching arity, it wins.
   UFCS is never even considered.
2. **UFCS fallback**, only when no bound method exists or none matches the
   call's arity. The candidate pool is every free function in the current
   file *plus* every free function in every module known anywhere in the
   whole compile — the same "known anywhere" visibility rule used for
   `operator` overload resolution. This is broader than the rule for
   ordinary bare calls, which only sees on-demand imports into the current
   file.

```turflang
struct User { tag: int }

fn User.print(): int { return 1; }              // bound method, 0 args
fn print(u: User, verbose: bool): int { return 2; } // free function, UFCS candidate

mut u: User = User::default();
u.print();      // == 1, resolves to the bound method (tier 1)
u.print(true);  // == 2, tier 1 has no 1-arg match, falls through to UFCS
```

### Argument matching

- **The receiver position is an exact type match — no coercion.** A `char`
  receiver will not match a UFCS candidate declared for `int`, even though
  an explicit `char → int` widening cast exists elsewhere in the language:

  ```turflang
  fn describe(x: int): string { return "int"; }

  mut c: char = 'a';
  c.describe(); // ERROR — no exact-match UFCS candidate for a char receiver
  ```

  Arrays are the one exception: an unsized array parameter (`arr: int[]`)
  matches a receiver of any concrete size, same as ordinary calls.

- **Everything after the receiver uses normal coercion.** An `int` literal
  argument still widens to a declared `double` parameter:

  ```turflang
  fn scale(x: int, factor: double): double { return x * factor; }

  mut n: int = 10;
  n.scale(2); // == 20.0 — the receiver `n` matches exactly; `2` widens to double
  ```

- **Ambiguity is a hard error.** If more than one visible candidate matches
  the call site exactly, compilation fails with "Ambiguous UFCS call to
  '<name>': more than one visible free-function overload matches this call
  site exactly."

> **Note:** Because UFCS candidate visibility is "known anywhere in the
> compile," a UFCS call can succeed purely because *some* file, anywhere in
> the program, imported the module that defines the candidate function —
> not necessarily the file making the call.

---

## 4. Modules

### 4.1 Import forms

```turflang
import io;                    // bare — qualified access only (io::printline(...))
import io::printline;         // single symbol — bare-callable after this
import io::[print, printline]; // symbol list — both become bare-callable
import math::[*];             // wildcard — every export of math becomes bare-callable
```

```turflang
import math::[*];

fn main(): int {
    abs(-5); // bare-callable thanks to the wildcard import
    max(3, 7);
    return 0;
}
```

Only `import module::symbol;`, `import module::[a, b, c];`, and
`import module::[*];` add names to the current file's flat (unqualified)
namespace. A plain `import module;` only ever grants `::`-qualified access.

The two are independent: `import module::symbol;` does **not** let you write
`module::other` (or `module::symbol`), and `import module;` does not make
anything bare. A file that wants both writes both lines:

```turflang
import list;           // list::List<int>, list::List::default()
import list::List;     // bare List<int>, List::default()
```

**A file may use `module::` only for a module it imports itself.** Importing
`json` (which itself imports `list`) does not let you write `list::List<int>`:
that is error `E145`, and its hint is the line to add, `import list;`. The
prelude modules (4.5) are the exception: they need no import. Methods,
UFCS calls and operators are not affected (they follow the value's type, see
the UFCS note in section 3), and neither is holding a value whose type comes
from a module you did not import; only *writing* `io::Something` needs the import.

### 4.2 `::` scope resolution

`::` is Turflang's general namespace-resolution operator. It is used for:

```turflang
Status::Failed;              // enum variant
io::printline("hi");         // module function
MathUtil::square(5);         // struct static method
StructName::new(args);       // construction
StructName::default();       // construction
Module::StructName::method(); // 3-part chain: module, then struct, then method
```

### 4.3 `.` vs `::` — the hard rule

`.` is for accessing something on an instance you actually hold. `::` is
for everything else (modules, static methods, enum variants, construction).
These are **enforced by the compiler**, not just a style convention:

```turflang
import io;

io.printline("x");  // ERROR — '.' is instance-only; use io::printline(...)
```

```turflang
struct MathUtil {}
fn MathUtil.square(x: int) static: int { return x * x; }

MathUtil.square(5); // ERROR — '.' on a bare struct type name; use MathUtil::square(5)
```

```turflang
struct P { x: int }
fn P.get(): int { return self.x; }

mut p: P = P::default();
p::get(); // ERROR — '::' on a real instance; use p.get()
```

> **Note:** A local variable shadows a module of the same name, using
> ordinary lexical scoping:
>
> ```turflang
> import io;
>
> struct Thing { val: int }
> fn Thing.show(): void { io::printline(self.val); }
>
> fn main(): int {
>     mut io: Thing = Thing::new(42);
>     io.show(); // resolves to the local `io` variable's method, not the module
>     return 0;
> }
> ```

### 4.4 Where `import <name>;` looks

`import <name>;` must name **exactly one** file. The compiler looks everywhere
it is allowed to and never picks a "first" match. Two different files
answering to the name is error **E090**, listing both, even when one of them is
the standard library's. (Until 2.0 the first match silently won, so a local
`json.tr` was quietly replaced by the stdlib's.)

**In a project** (a directory with a `turf.toml`, built with `turf build`,
`turf run` or `turf check`), a module of the package can import one of:
- one of the package's own modules: a `<name>.tr` anywhere under its `src/`;
- a package listed under `[dependencies]` in its `turf.toml`, by package name;
- a standard-library module.

Nothing else is searched: not the file's directory, the current directory or
`lib/usr`. A module that exists in the build but is out of reach (a dependency
of a dependency, or another package's internal module) is error **E091**. The
full rules are in [Projects and packages](projects_and_packages.md#4-how-imports-resolve).

**For a single file** (`turf file.tr`, `turf run file.tr`), the places are:
1. the standard library, `lib/std/**` under the installation
   (`$TURF_HOME`, or the directory above the compiler binary);
2. `lib/usr/**` under the installation;
3. the directory of the file doing the import;
4. the current directory.

The same file reached twice (compiling from the file's own directory makes
3 and 4 the same place) is one match, not two.

Either way, an `import` written *inside* the standard library, and the
automatic prelude imports (4.5), look only in the standard library. So a
`list.tr` of your own never interferes with the stdlib's internal use of its
`list`; it only matters if you write `import list;` yourself.

The standard library's subfolders (`lib/std/core/`, `lib/std/io/`, …) are for
browsing only. A module name is never a path, and moving a file between
subfolders never breaks an `import`.

### 4.5 The prelude — modules auto-imported for you

Nine standard-library modules are automatically available in every file —
the program and every module — with no `import` statement needed:

```
int, double, bool, char, strings, array     // reached by UFCS, operators, strings::length(s)
io, math, sys                               // ALSO callable by their bare names
```

**The console, math and process modules are bare-callable.** `printline("hi")`,
`print(3)`, `input()`, `abs(-5)`, `max(a, b)`, `sqrt(2.0)`, `exit(1)` and
`panic("message")` all work with no import and no `io::` — every public free
function of `io`, `math` and `sys` (the constants stay qualified:
`math::PI`). The qualified forms (`io::printline`, `math::abs`) work too, with
no import. The other six modules are not flattened into bare names: their
functions read as methods (`c.is_digit()`, `s.length()`).

A bare name is the **last resort**, so the prelude can never make a program
ambiguous. In order, a bare call `f(...)` looks at: (1) the functions this file
declares, (2) for code inside a module, that module's own functions, (3) names
you explicitly imported (`import m::f;`), and only if none of those has any
function called `f`, (4) the prelude. So declaring your own `max(a: int, b:
int)` simply hides math's `max`, and `math::max(...)` still reaches it. The
price is that once your file declares *any* `max`, a bare `max(1.5, 0.5)` is
checked against yours alone and reports a type mismatch naming your function;
write `math::max` for the prelude's.

> **Note — two different things.** *Being known* to the compile (every prelude
> module is) gives UFCS and operator candidacy and the `::` qualifier. *Being
> bare-callable* is the extra step only `io`, `math` and `sys` take. A module
> you add to your program with `import list;` is neither automatic nor bare.

What the prelude buys you with zero imports:

- `::`-qualified calls: `strings::length(s)`, `io::printline(x)`, `math::PI`.
  (`int`, `double`, `bool` and `char` are also type keywords, so their module
  functions are reached through UFCS, `n.is_even()`, never `int::is_even(n)`.)
- UFCS and operator-overload candidacy, since that visibility rule is
  "known anywhere in the compile" and the prelude modules are always known:

  ```turflang
  // No imports anywhere in this file.
  fn main(): int {
      mut c: char = '7';
      c.is_digit();      // UFCS on char, from the char prelude module
      mut n: int = 8;
      n.is_even();        // UFCS on int
      mut a: string = "foo";
      mut b: string = "bar";
      mut s: string = a + b; // operator+ on string, from the strings prelude module
      return 0;
  }
  ```

What it does **not** buy you:

```turflang
// No imports.
fn main(): int {
    mut c: char = '7';
    is_digit(c); // ERROR — bare call, not UFCS; only io, math and sys are bare-callable
    return 0;
}
```

Other prelude behavior worth knowing:

- Explicitly re-importing a prelude module (`import char;`) is harmless —
  it does not conflict with the automatic import.
- `int`, `double`, `bool`, and `char` are simultaneously primitive-type
  keywords and prelude module names; `import int;`, `import char;`, etc.
  work as ordinary explicit imports if you want them.

### 4.6 Circular imports

A→B→A (or any longer cycle) is rejected as a circular import. A **diamond**
— two modules that both import a third, unrelated, common module — is not
a cycle and compiles fine.

### 4.7 `export immut` — module-level constants

```turflang
// lib/std/core/math.tr
export immut PI: double = 3.14159265358979323846;
```

- Only valid at top level (not inside a function body).
- Only `immut` — `export mut` is rejected.
- The initializer must be a compile-time-foldable literal. There's no
  runtime storage behind it: every reference is replaced by the literal
  value at compile time.
- Same visibility rules as functions: bare self-reference from within its
  own file/module, `module::CONST` once imported, or bare access after an
  on-demand import.

```turflang
import io;
import some_module; // declares `export immut FACTOR: int = 10;` and
                     // `fn scaleUp(x: int): int { return x * FACTOR; }`

fn main(): int {
    io::printline(some_module::scaleUp(5));
    return 0;
}
```

---

## Common errors you'll run into

| Situation | What happens |
|---|---|
| `io.printline(...)` | rejected — `.` on a module; use `::` |
| `MathUtil.square(5)` (bare type, static method) | rejected — `.` on a struct type; use `::` |
| `p::get()` where `p` is a variable | rejected — `::` on an instance; use `.` |
| Calling an unknown bare name | `UnknownFunctionError` |
| `import`ing a module that doesn't exist | `ModuleNotFoundError` |
| A→B→A import cycle | `CircularImportError` |
| Two overloads both match a first-class function reference | "overloaded — can't be used as a first-class function value" |
| Two UFCS candidates match a call site exactly | "Ambiguous UFCS call" |
