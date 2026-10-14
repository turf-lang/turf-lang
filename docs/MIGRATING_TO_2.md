# Migrating from TurfLang 1.x to 2.0

This guide is for someone with a working 1.x program. It lists every change that
can make that program stop compiling or behave differently, in the order you
will meet them: first the compile errors, then the changes that compile but
behave differently at run time, then the tools, install and command-line
changes. Each entry gives the old behaviour, what 2.0 does, the error code or
behaviour you will see, the fix, and whether a tool does the fix for you.

Two tools help:

- `turf migrate pub --write <files>` adds `pub` to every declaration, which is
  the one change a tool can make for you at scale ([Visibility](#1-everything-is-private-unless-pub)).
- `turf fmt` is a whitespace formatter. It does not migrate anything; run it
  after an edit session if you want consistent indentation.

2.0 adds no compatibility switches. Where the old behaviour was a silent wrong
answer (wrapping arithmetic, clamped literals, zero-filled variables), it is
gone rather than hidden behind a flag; see [What is not kept](#what-is-not-kept-as-a-shim).

Examples in this guide were compiled with the 2.0 compiler. A "before" example
that shows an error is the 2.0 compiler rejecting it; how 1.x behaved is
described in the text, since 1.x is not available to run.

## Migration checklist

Work through this in order. Each line links to its section.

1. If your program has more than one file, run `turf migrate pub --write` on every
   module file (not the main file), then remove `pub` from what should stay
   private. [Section 1](#1-everything-is-private-unless-pub)
2. Compile, and add `import m;` for every module you use as `m::`.
   [Section 2](#2-a-file-may-use-m-only-for-a-module-it-imports)
3. Rename any module of yours that shares a name with a standard-library module.
   [Section 3](#3-a-module-may-not-share-a-standard-library-module-name)
4. Rename variables, functions and fields called `guess`, `defer`,
   `caller_location`, `pub`, `public`, `try` or `_`.
   [Section 4](#4-new-reserved-words)
5. Give every declared-but-unassigned variable a value, normally
   `T::default()`. [Section 5](#5-no-implicit-default-initialization)
6. Fix integer literals and constant expressions that do not fit their type.
   [Section 6](#6-integer-literals-and-constants-must-fit-their-type)
7. Wherever you use `s[i]` as an `int`, write `s[i] to int`.
   [Section 7](#7-si-is-a-char-and-is-bounds-checked)
8. Replace `return &local;` with `return heap(local);`.
   [Section 8](#8-returning-a-reference-to-a-local-is-an-error)
9. Break by-value struct cycles with `&?T`. [Section 9](#9-a-struct-may-not-contain-itself-by-value)
10. Move raw-pointer code behind `unsafe` or an `unsafe module;`, and use
    `l.get(i)` / `l[i]` on collections. [Section 10](#10-raw-pointers-are-gated)
11. Rewrite `int32` increments as `n = (n + 1) to int32;`.
    [Section 11](#11-int32-literals-work-increment-does-not)
12. Add the missing arm to every `bool` compare and an `otherwise` to every
    compare over a caught error. [Section 12](#12-compare-must-cover-its-subject)
13. Apply the standard-library renames and the `!void` changes.
    [Section 13](#13-standard-library-renames-and-changed-shapes)
14. Check error-value construction and `return f();` uses.
    [Section 14](#14-error-types-payloads-and-try)
15. Re-run your tests and look for the runtime changes: integer overflow panics,
    out-of-range string index panics, `typeof` strings, `a += b` on types with
    `operator +`, `mem::free` of GC memory. [Runtime changes](#runtime-behaviour-changes)
16. Update scripts that look at `turfc` exit status or call `turfc --help`.
    [Tooling](#tooling-install-and-command-line-changes)

---

## Compile errors you will see

### 1. Everything is private unless `pub`

**Old.** Every top-level function, struct, enum, error, machine, method and
struct field was visible to any file that imported the module.

**New.** A declaration is visible outside its file only if it is marked `pub`
(`public` is the same word). Using a private one from another module is error
**E088** (`Type 'Point' is private to module 'util'`). A single-file program is
not affected.

A module `util.tr`:

```text
struct Point {
    x: int,
    y: int
}

fn Point.new(x: int, y: int): void {
    self.x = x;
    self.y = y;
}

fn Point.norm1(): int {
    return self.x + self.y;
}

fn helper(n: int): int {
    return n * 2;
}

fn double_norm(p: Point): int {
    return helper(p.norm1());
}
```

and a main program `app.tr`:

```text
import io;
import util;

fn main(): int {
    immut p: util::Point = util::Point::new(3, 4);
    io::printline(util::double_norm(p));
    io::printline(p.x);
    return 0;
}
```

compile in 1.x. In 2.0 the second file is rejected:

```text
── error at line 5, col 5 [E088 · declaration]

  Type 'Point' is private to module 'util'.
```

**Fix.** Mark the public interface of `util.tr` with `pub`: `pub struct Point`,
`pub x: int` for each field a caller reads, `pub fn Point.new(...)` and so on.
Leave `helper` private; `double_norm` can still call it.

**Tool.** `turf migrate pub` adds `pub` to every top-level declaration and every
struct field, the exact 1.x surface, so the program compiles again. Without
`--write` it only reports what it would change. A second run changes nothing.

```sh
turf migrate pub util.tr           # show the before/after lines
turf migrate pub --write util.tr   # rewrite the file
```

Run it on module files, not on the main program (a `pub` in a main file is
accepted but does nothing). Then go through the result and take `pub` off what
was never meant to be public; the tool cannot know that. After migrating the
two files above, `app.tr` compiles and prints `14` and `3`.

See [Functions and modules](reference/functions_and_modules.md) for the full
visibility rules, including private types in public signatures.

### 2. A file may use `m::` only for a module it imports

**Old.** If any file in the program had loaded a module, every file could write
`m::name` for it. A program with only `import json;` could call `list::List`
or a non-prelude module's functions because `json` imported them itself.
Writing `import list::List;` also let you spell `list::List<int>`.

**New.** A file may write `m::` only for a module it imports with a plain
`import m;`. Anything else is error **E145** (`Module 'list' is not imported in
this file`), and the hint is the exact line to add. `import list::List;` brings
in the name `List` only, not the qualifier `list::`. The prelude modules (`io`,
`math`, `sys`, `strings`, `int`, `double`, `bool`, `char`, `array`) need no
import.

Methods, calls through the dot (`x.f()`), and operators are unaffected: they
follow the value's type, not the import list.

<!-- doc-example: fails E145 -->
```turflang
import json;

fn main(): int {
    mut l: list::List<int> = list::List::default();
    return l.length();
}
```

**Fix.** Add one `import m;` line per module:

```turflang
import json;
import list;

fn main(): int {
    mut l: list::List<int> = list::List::default();
    return l.length();
}
```

**Tool.** None; the error message prints the line to add.

### 3. A module may not share a standard-library module name

**Old.** A module of yours named `list.tr` next to your main file silently
shadowed the standard library's `list`.

**New.** `import list;` with both present is error **E090** (`Module 'list' is
ambiguous: 2 different files answer to that name`). The message names both
files.

**Fix.** Rename your module (`my_list.tr`) and import it by the new name. This
is a naming choice, so no tool does it.

### 4. New reserved words

**Old.** These were ordinary identifiers.

**New.** `guess`, `defer`, `caller_location`, `try`, `pub` / `public` and `_`
are reserved. Using one as a variable, function, field or type name is an
error:

| Word | Error |
|---|---|
| `guess`, `defer`, `caller_location`, `try` | **E108** (`Expected a variable name after 'mutable' or 'immutable'`) |
| `pub`, `public` | **E087** (`'pub' is a reserved word and can't be used as a name`) |
| `_` | **E092** (`'_' is the wildcard pattern, so it can't be used as a name`) |

The bare `_` could not be lexed as a name before, so no real 1.x program is
affected by that one. In 2.0 `_` is the wildcard pattern in `compare`, and a name
that starts with an underscore (`_unused`) is still fine.

<!-- doc-example: fails E108 -->
```turflang
fn main(): int {
    mut guess: int = 1;
    return guess;
}
```

**Fix.** Rename the identifier.

```turflang
import io;

fn main(): int {
    mut attempt: int = 1;
    io::printline(attempt);
    return 0;
}
```

**Tool.** None; it is a rename. `turf check` lists every site.

### 5. No implicit default initialization

**Old.** A variable declared without a value, `mut c: Config;`, was filled with
zeros and was readable straight away.

**New.** A declared variable has no value until you assign one, and reading it
before every path has assigned it is error **E008** (`You're using 'c' ... before
it has a value`). The message names the type and suggests the fix.

<!-- doc-example: fails E008 -->
```turflang
import io;

struct Config {
    port: int,
    name: string
}

fn main(): int {
    mut c: Config;
    io::printline(c.port);
    return 0;
}
```

**Fix.** Give the variable a value where it is declared. `T::default()` gives
the zeroed value the old code relied on.

```turflang
import io;

struct Config {
    port: int,
    name: string
}

fn main(): int {
    mut c: Config = Config::default();
    c.port = 80;
    io::printline(c.port);
    return 0;
}
```

For a generic collection write the bare name: `mut l: List<int> = List::default();`.
See [Variables and types](reference/variables_and_types.md).

**Tool.** None applies it automatically, but every site is an E008 whose message
gives the exact replacement.

### 6. Integer literals and constants must fit their type

**Old.** A literal too big for `int` was silently clamped to the largest
`int` (`99999999999999999999` became `9223372036854775807`), and a constant
expression such as `9223372036854775807 + 1` wrapped.

**New.** Both are error **E076** (`Integer literal is not representable in type
'int'`), and the message states the type's range.

<!-- doc-example: fails E076 -->
```turflang
import io;

fn main(): int {
    immut big: int = 99999999999999999999;
    io::printline(big);
    return 0;
}
```

**Fix.** Write the value you meant, or a type that holds it. `int` is the only
general-purpose integer; for larger magnitudes use `double` or a library type.
`int32` literals are accepted if the value fits ([Section 11](#11-int32-literals-work-increment-does-not)).

**Tool.** None; the error points at the literal. The run-time companion of this
change, overflow of a computed `int`, is in
[Runtime changes](#signed-int-overflow-panics).

### 7. `s[i]` is a `char` and is bounds-checked

**Old.** Indexing a string gave an `int` (the byte), with no check on `i`.

**New.** `s[i]` is a `char`. Using it where an `int` is needed is a type error:
**E066** when assigned to an `int`, **E124** when compared with an integer
literal, **E057** in arithmetic with an `int`. A read with `i` out of range is a
run-time panic ([Runtime changes](#string-indexing-panics-when-out-of-range)).

<!-- doc-example: fails E066 -->
```turflang
import io;

fn main(): int {
    immut s: string = "hello";
    immut c: int = s[1];
    io::printline(c);
    return 0;
}
```

**Fix.** Convert with `to int` where you need the number; compare against a
`char` literal where you need the letter.

```turflang
import io;

fn main(): int {
    immut s: string = "7";
    immut d: int = (s[0] to int) - ('0' to int);
    io::printline(d);
    io::printline(s[0] == '7');
    return 0;
}
```

This prints `7` and then `1`. A `char` also works as a `compare` subject, so
`compare s[0] { matches 'a': ... done }` needs no change.

**Tool.** None applies it, but every site is a type error at the exact
expression, and the fix is mechanical: add `to int`, or change the integer
literal to a char literal. See [Variables and types](reference/variables_and_types.md).

### 8. Returning a reference to a local is an error

**Old.** `return &local;` compiled and returned a reference to storage that no
longer existed when the function returned.

**New.** Error **E086**. The same applies to returning or storing a reference
into a local's field, or a local collection.

<!-- doc-example: fails E086 -->
```turflang
struct Box {
    v: int
}

fn make(): &Box {
    mut b: Box = Box::default();
    return &b;
}

fn main(): int {
    immut r: &Box = make();
    return r.v;
}
```

**Fix.** `heap(value)` copies the value onto the garbage-collected heap and
gives a reference that outlives the function. Alternatively return a reference
you received as a parameter.

```turflang
import io;

struct Box {
    v: int
}

fn make(): &Box {
    mut b: Box = Box::default();
    b.v = 5;
    return heap(b);
}

fn main(): int {
    immut r: &Box = make();
    io::printline(r.v);
    return 0;
}
```

**Tool.** None applies it; the E086 hint suggests `heap()`. Every program that
hit this was already returning a dangling reference. See
[Memory and safety](reference/memory_and_safety.md).

### 9. A struct may not contain itself by value

**Old.** `struct Node { next: Node }` compiled, then crashed when used.

**New.** Error **E069** (`struct 'Node' cannot contain itself by value`), even
if the struct is never instantiated; the message prints the cycle.

<!-- doc-example: fails E069 -->
```turflang
struct Node {
    value: int,
    next: Node
}

fn main(): int {
    return 0;
}
```

**Fix.** Make one field a nullable safe reference, `&?Node`:

```turflang
import io;

struct Node {
    value: int,
    next: &?Node
}

fn Node.new(v: int): void {
    self.value = v;
    self.next = null;
}

fn main(): int {
    immut n: Node = Node::new(7);
    io::printline(n.value);
    io::printline(n.next == null);
    return 0;
}
```

This is a design choice, so no tool does it. See
[Structs and enums](reference/structs_and_enums.md#self-referential-structs).

### 10. Raw pointers are gated

Three separate changes, all about raw pointers (`T*`, `ptr`).

**Raw pointers in function signatures.** In 1.x a function whose parameter is a
raw pointer could not be written at all; in 2.0 it is error **E130** (`Raw
pointer in function signature`), except in an `extern fn` and in a file that
begins with `unsafe module;`. That declaration is honored for files under the
compiler's install root, and elsewhere only when the compile passes
`--allow-unsafe-modules` (for a dependency in a project, the root manifest's
`[unsafe] allow` list decides).

<!-- doc-example: fails E130 -->
```turflang
import io;

fn first(p: int*): int {
    return 1;
}

fn main(): int {
    io::printline(first(null));
    return 0;
}
```

The fix is to take a safe reference (`&int`), or to begin the file with
`unsafe module;` and compile with `--allow-unsafe-modules`.

**Raw pointers outside `unsafe`.** Declaring or using a raw-pointer value
outside an `unsafe { }` block (or an `unsafe module;`) is error **E130**
(`Unsafe pointer use`). Wrap the code in `unsafe { }`.

**Indexing a collection's buffer.** 1.x code could reach into a collection with
`l.data[i]`, unchecked. The buffer is private in 2.0 and the compiler rejects the
access (**E068**, `Struct 'list::List' has no field named 'data'`). Use the
checked accessors:

```turflang
import io;
import list::List;

fn main(): int {
    mut l: List<int> = List::default();
    l.push(4);
    io::printline(l[0]);
    io::printline(l.get(0));
    return 0;
}
```

Use `l.set(i, v)` for writes. See [FFI and unsafe](reference/ffi_and_unsafe.md).

**Tool.** None; wrapping in `unsafe` is a decision about what you trust.

### 11. `int32` literals work, increment does not

**Old.** `mut n: int32 = 5;` was rejected, and `n++` on an `int32` crashed the
compiler.

**New.** An integer literal is accepted for an `int32` when its value is
representable. `++`, `--` and compound assignment (`+=` and so on) on an `int32`
are a clear error, **E126** (`postfix '++' is not supported on 'int32'`).

<!-- doc-example: fails E126 -->
```turflang
fn main(): int {
    mut n: int32 = 5;
    n++;
    return 0;
}
```

**Fix.** Do the arithmetic in `int` and convert back, as the hint says:

```turflang
import io;

fn main(): int {
    mut n: int32 = 5;
    n = (n + 1) to int32;
    io::printline(n);
    return 0;
}
```

**Tool.** None applies it; the error gives the replacement.

### 12. `compare` must cover its subject

**Old.** A `compare` over a `bool` with one arm and no `otherwise` was accepted,
and a compare over a caught error needed no catch-all. (Also, in a `bool`
compare, a `matches true:` arm could silently never match, so a compare that
"worked" by falling through may now take a different path.)

**New.** Without an `otherwise`:

- a `bool` compare must have both `matches true:` and `matches false:`, or it is
  error **E093** (`Non-exhaustive bool comparison`);
- a compare over the error bound by `recover as err run` must end with
  `otherwise` or `matches _:`, or it is **E093** (`Non-exhaustive comparison on
  '<error>'`), because such a value can be any error a callee returns.

<!-- doc-example: fails E093 -->
```turflang
import io;

fn main(): int {
    immut b: bool = true;
    compare b {
        matches true:
            io::printline("yes");
        done
    }
    return 0;
}
```

**Fix.** Add the missing arm:

```turflang
import io;

fn main(): int {
    immut b: bool = false;
    compare b {
        matches true:
            io::printline("yes");
        done
        matches false:
            io::printline("no");
        done
    }
    return 0;
}
```

For a caught error, add an `otherwise` arm (it comes last and takes no `done`):

```turflang
import io;

error E {
    A,
    B
}

fn f(): !int {
    return E::A;
}

fn main(): int {
    immut r: int = f() recover as err run {
        compare err {
            matches E::A:
                return 1;
            done
            otherwise:
                return 2;
        }
    };
    io::printline(r);
    return 0;
}
```

See [Control flow](reference/control_flow.md#exhaustiveness).

**Tool.** None; the error names the missing values.

### 13. Standard-library renames and changed shapes

The standard library's public surface is fixed for the 2.x series, and 2.0
tidied it. Every item here is a compile error in an unmigrated program.

| 1.x | 2.0 | What you see |
|---|---|---|
| `c.isDigit()` `isUpper` `isLower` `isAlpha` `isAlphaNumeric` `isPunctuation` `isWhitespace` `toUpper` `toLower` on a `char` | `is_digit` `is_upper` `is_lower` `is_alpha` `is_alphanumeric` `is_punctuation` `is_whitespace` `to_upper` `to_lower` | **E131** (`No such method`) |
| `Map.indexOf`, `Set.indexOf` | `index_of` | **E054** (`has no method named 'indexOf'`), with the new name as the hint |
| `Map.tryGet` | `try_get` | **E054** |
| `import list::list_sort;` and `list_binary_search`, `list_find` | `l.sort(cmp)`, `l.binary_search(x, cmp)`, `l.find(pred)` | **E030** (`Module 'list' has nothing named 'list_sort'`) |
| `io::write_string(path, s): bool` | `: !void`, fails with `FileError::WriteFailed(path)` | **E066** / **E124** |
| `fs::mkdir`, `fs::remove_file`, `fs::remove_dir`: `bool` | `!void`, fail with `FsError::MkdirFailed` / `RemoveFileFailed` / `RemoveDirFailed` (each carries the path) | **E124** / **E066** |
| `os::set_env(...): bool` | `!void`, fails with `OsError::SetEnvFailed(name)` | type error where the `bool` was used |
| `http::Server.listener`, `.routes` | private | **E088** |

Also changed, without an error: `strings::repeat`, `replace` and `join` are
linear time (they were quadratic), and there is a new `strings::StringBuilder`.

The renames are a find-and-replace. The call to a former list wrapper becomes a
method call:

```turflang
import io;
import list;

fn cmp_int(a: int, b: int): int {
    return a - b;
}

fn is_two(x: int): bool {
    return x == 2;
}

fn main(): int {
    mut l: list::List<int> = list::List::default();
    l.push(2);
    l.push(1);
    l.sort(cmp_int);
    io::printline(l.get(0));
    io::printline(l.binary_search(2, cmp_int));
    io::printline(l.find(is_two));
    return 0;
}
```

This prints `1`, `1` and `1`. The methods work for every element type; the old
wrappers were for `List<int>` only.

**A `bool` that became `!void`.** A function that used to report failure with
`false` now fails as an error value, and the caller handles it with `recover`. The
old test:

<!-- doc-example: fails E124 -->
```turflang
import io;
import fs;

fn main(): int {
    if !fs::mkdir("/nonexistent_dir/sub") {
        io::printline("mkdir failed");
    }
    return 0;
}
```

becomes:

```turflang
import io;
import fs;

fn main(): int {
    fs::mkdir("/nonexistent_dir/sub") recover run {
        io::printline("mkdir failed");
    };
    io::printline("after");
    return 0;
}
```

This prints `mkdir failed` and then `after`. To tell failures apart, bind the
error with `recover as err run { ... }` and compare it against a variant
(`err == fs::FsError::MkdirFailed`). To stop on failure, use
`try fs::mkdir(path);` in a function that itself returns `!T`. See
[the standard library](reference/stdlib/README.md) and
[Error handling](reference/error_handling.md).

**Tool.** None; these are renames and a rewrite of each failure check.

### 14. Error types, payloads and `try`

Several small changes to error handling:

- `try` is a reserved word ([Section 4](#4-new-reserved-words)).
- `io::FileError` variants now carry the path, and `json::JsonError` variants
  carry the byte offset. Constructing one by hand needs the argument, and naming
  a payload variant without it is error **E096** (`declares payload fields ...
  so it can't be used as a value on its own`). Comparing with `==` is
  unchanged: `e == io::FileError::NotFound` compares the variant only.
- `return f();` is now legal when `f` returns a matching `!T`. It was an error
  before; code that worked around it with a `recover` that re-returned can be
  simplified.
- `recover as err run { return err; }` can now be written `try f()`.

<!-- doc-example: fails E096 -->
```turflang
import io;

fn f(): !void {
    return io::FileError::NotFound;
}

fn main(): int {
    f() recover run {
        io::printline("failed");
    };
    return 0;
}
```

Pass the payload:

```turflang
import io;

fn main(): int {
    immut e: io::FileError = io::FileError::NotFound("a.txt");
    io::printline(e == io::FileError::NotFound);
    return 0;
}
```

Passing the error up unchanged is shorter with `try`:

```turflang
import io;

error E {
    A
}

fn f(): !int {
    return E::A;
}

fn g(): !int {
    return f();
}

fn h(): !int {
    immut v: int = try f();
    return v;
}

fn main(): int {
    io::printline(h() recover produce 7);
    io::printline(g() recover produce 8);
    return 0;
}
```

This prints `7` and `8`. See [Error handling](reference/error_handling.md).

**Tool.** None; none of these needs a mechanical rewrite.

---

## Runtime behaviour changes

These compile unchanged and behave differently. Run your tests.

### Signed `int` overflow panics

**Old.** Arithmetic on `int` wrapped around silently.

**New.** An overflowing `+`, `-` or `*` on `int` stops the program with a
run-time panic, `panic: runtime check: Integer overflow (file.tr:LINE)`, and
exit status 134. A constant expression that overflows is a compile error
([Section 6](#6-integer-literals-and-constants-must-fit-their-type)).

```turflang
import io;

fn main(): int {
    mut x: int = 9223372036854775807;
    mut i: int = 0;
    while i < 3 {
        x = x * 2;
        i++;
    }
    io::printline(x);
    return 0;
}
```

This program panics on the first multiplication. **Fix.** Check before you
overflow, or do the arithmetic in `double`. There is no wrapping operator in 2.0
([What is not kept](#what-is-not-kept-as-a-shim)).

```turflang
import io;

fn main(): int {
    mut x: int = 9223372036854775807;
    if x > 4611686018427387903 {
        io::printline("would overflow, clamp instead");
    } else {
        x = x * 2;
    }
    io::printline(x);
    return 0;
}
```

**Tool.** None. See [Variables and types](reference/variables_and_types.md).

### String indexing panics when out of range

**Old.** `s[i]` with `i` outside the string read whatever was in memory.

**New.** It panics: `panic: runtime check: String index out of bounds
(file.tr:LINE)`, exit status 134.

```turflang
import io;

fn main(): int {
    immut s: string = "hi";
    mut i: int = 5;
    io::printline(s[i] to int);
    return 0;
}
```

**Fix.** Check `i < length` first. **Tool.** None.

### `typeof` of an imported type is qualified

**Old.** All struct names shared one global namespace, and `typeof(x)` gave the
bare name.

**New.** Types belong to their module (two modules may each declare a `Node`).
`typeof` returns the **qualified** name for a type declared in an imported
module, `"list::List<string>"`, and the bare name for a type declared in the
main program file, `"Node"`. Code comparing `typeof(x)` with a bare imported
type name now silently takes the other branch.

```turflang
import io;
import list::List;

fn main(): int {
    mut l: List<string> = List::default();
    if typeof(l) == "List<string>" {
        io::printline("old name matched");
    } else {
        io::printline("old name no longer matches");
    }
    if typeof(l) == "list::List<string>" {
        io::printline("qualified matches");
    }
    return 0;
}
```

This prints `old name no longer matches` and `qualified matches`. **Fix.**
Compare against the qualified name, or avoid string comparison of types.
**Tool.** None.

### Compound assignment uses your operators

**Old.** `a op= b` ignored a user-defined `operator op`.

**New.** `a += b` calls `operator +` when the types define it. A type that
defined `+` now also gets `+=`, and the full operator set (not just ten) can be
overloaded.

```turflang
import io;

struct V {
    x: int
}

operator +(a: V, b: V): V {
    mut r: V = V::default();
    r.x = a.x + b.x;
    return r;
}

fn main(): int {
    mut a: V = V::default();
    a.x = 1;
    mut b: V = V::default();
    b.x = 2;
    a += b;
    io::printline(a.x);
    return 0;
}
```

This prints `3`. Nothing to fix unless you relied on `+=` ignoring the
overload. See [Generics and operators](reference/generics_and_operators.md).

### `mem::free` on garbage-collected memory panics

**Old.** Freeing a `string` or a collection's buffer through `mem::free`
freed the wrong heap silently, then corrupted memory or double-freed.

**New.** It panics: `panic: cross-heap free: mem::free() was called on a pointer
that belongs to the GC heap ...`. Only free memory you got from `mem::malloc` or
the other manual allocators; never free strings or collection buffers.

```turflang
import io;
import mem;
import strings;

fn main(): int {
    mut s: string = strings::repeat("z", 10);
    io::printline(s);
    unsafe {
        mem::free(s to void*);
    }
    io::printline("not reached");
    return 0;
}
```

**Fix.** Delete the `free`; the collector reclaims strings and collections.
**Tool.** None. See [Memory and safety](reference/memory_and_safety.md).

### Smaller standard-library behaviour changes

- Panic messages from standard-library functions (`list.pop`, `map.get`,
  `substring` and similar) now start with the caller's `file:line:`. The old
  text is still in the message; a test that compared the whole string needs the
  prefix.
- `json::escape_string`, and so `json::stringify`, now escapes every control
  character below `0x20` as `\b`, `\f` or `\u00XX`. The old output contained raw
  control bytes, which is invalid JSON.
- `close()` on a network connection or listener is safe to call twice.
- Every program now links the TurfLang runtime library. See
  [Tooling](#the-runtime-library-is-part-of-every-install).

---

## Tooling, install and command-line changes

### `turf` and `turfc`, exit status and help

2.0 has two commands: `turf`, which you use, and `turfc`, the compiler it
drives. Both now exit **0** on success, **1** when the program or its input has an
error (a compile error, a file that cannot be read or written), and **2** when the
command line is wrong and nothing was compiled.

**Old.** `turfc` exited 1 for a usage error too. A script that treated every
nonzero status as "the program does not compile" now sees 2 for a bad command
line, and can tell the two apart.

Help is `turf help [command]` and `turfc help`. `turfc --help` is deliberately not
accepted: it prints `Unknown option: --help`, points at `turfc help` and exits 2.
An unknown flag is always a usage error with a nearest-spelling suggestion; it is
never ignored or taken as a file name. `turf version` prints the `turf` and
`turfc` versions, then the compiler's interface, LLVM, runtime, root and linker.
See [CLI and diagnostics](reference/cli_and_diagnostics.md).

**Tool.** None; change the script.

### Installing

Nothing breaks for a 1.x program. Install with the `install.sh` script
(`curl ... | sh`; the archive's SHA-256 is verified before anything is
installed). It unpacks into `~/.turf`, adds one marked `PATH` block to your shell
start-up file, and `install.sh --uninstall` removes exactly what it added. You can
also unpack a release archive (`bin/ lib/ docs/ share/`) anywhere; it needs no
`TURF_HOME`, because both programs find `lib/` from their own location. The
installation steps are in the [README](../README.md#installation); LLVM and a
system C compiler are still needed to build and link programs.

You no longer need `TURF_HOME`. If you set one for 1.x, either unset it or make
sure it points at the current install: the compiler takes the standard library
from the tree it names, so a stale value makes imports fail (E027).

### `--target` compiles but does not link

`turfc --target=<triple>` writes the object file for another machine and then
stops with `cross-linking is not supported in 2.0`, keeping the `.o` for you to
link with a toolchain and a runtime built for that target. Programs for the
machine you compile on are unaffected. See
[CLI and diagnostics](reference/cli_and_diagnostics.md).

### The runtime library is part of every install

**Old.** Compiled programs linked only libc and libm.

**New.** Every program also links the TurfLang runtime library, `libturfrt.a`
(runtime support such as the garbage collector lives there). The
install layout contains `lib/libturfrt.a`. A release archive or `install.sh`
install has it; if you copied only `turf` and `turfc` somewhere by hand, copy
`lib/` with them.

### `turf migrate` and `turf fmt`

`turf migrate pub` is the only migration the toolchain performs
([Section 1](#1-everything-is-private-unless-pub)). `turf fmt [--write|--check]
<paths>` re-indents and removes trailing whitespace and repeated blank lines; it
changes nothing else, and it is not part of migrating code. See
[CLI and diagnostics](reference/cli_and_diagnostics.md).

---

## What is not kept as a shim

These 1.x behaviours were removed on purpose and have no switch to bring them back:

- **Wrapping arithmetic.** `int` overflow panics. Wrapping helpers may come in a
  later 2.x release if there is a measured need for them.
- **Eager default initialization.** A declared variable is never silently
  zero-filled ([Section 5](#5-no-implicit-default-initialization)).
- **Path-based privilege for the standard library.** Raw pointers in signatures
  are allowed only with an explicit `unsafe module;`, never because a file lives
  in a particular folder ([Section 10](#10-raw-pointers-are-gated)).
- **Everything-public modules**, **silently shadowing a standard-library module**,
  and **qualifying with a module you did not import** are errors with a
  mechanical fix ([Sections 1, 2 and 3](#1-everything-is-private-unless-pub)).
