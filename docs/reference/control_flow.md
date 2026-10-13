# Control Flow

Reference for TurfLang's control-flow constructs:

- choosing: [`if` / `elseif` / `else`](#if--elseif--else), the [`give` / `otherwise` ternary](#the-give--otherwise-ternary), and [`compare` / `matches`](#compare--matches) with its [pattern language](#patterns);
- repeating: [`while`](#while), [`do` / `while`](#do--while), and [`for`](#for-range-form) over a [range](#for-range-form) or an [array](#for-each-form);
- leaving early: [`break` / `continue`](#break-and-continue) and [`defer`](#defer);
- and how all of these meet [error values](#control-flow-and-error-values).

Conditions are bare expressions: `if x > 0 { ... }`. Parentheses are accepted for grouping (`while (x > 0)`) but never required. A condition must be a `bool`: there is no truthiness, so `if n { ... }` with an `int` `n` is a type mismatch (E066); write `if n != 0 { ... }`. The body of every `if`, `elseif`, `else`, `while`, `do` and `for` is a braced block; a `compare` arm has its own `:` ... `done` body.

---

## `if` / `elseif` / `else`

```turflang
import io;

fn sign(x: int): int {
    if x > 10 {
        return 3;
    } elseif x > 0 {
        return 2;
    } elseif x == 0 {
        return 1;
    } else {
        return -1;
    }
}

fn main(): int {
    io::printline(sign(50));
    io::printline(sign(5));
    io::printline(sign(0));
    io::printline(sign(-4));
    return 0;
}
```

```text
3
2
1
-1
```

- Braces around each branch body are **mandatory**. There is no brace-less `if`; forgetting the `{` is error E107 (`Expected '{' after if-condition`), and the hint points you at `give` / `otherwise` for the single-expression case.
- `elseif` (one word) chains further conditions. `else if` is accepted as well, but `elseif` is the spelling to use.
- `else` is optional. An empty branch is legal (`if cond {} else { ... }`).
- Branches are evaluated top to bottom; the first true condition wins and the rest are not evaluated.
- An `if` is a statement, not an expression. For a conditional value use the ternary below.

<!-- doc-example: fails E107 -->
```turflang
fn main(): int {
    mut n: int = 0;
    if n == 0 n = 1;
    return n;
}
```

---

## The `give` / `otherwise` ternary

`condition give a otherwise b` is an expression: it evaluates to `a` when the condition holds and to `b` otherwise. Only the chosen side is evaluated.

```turflang
import io;

fn main(): int {
    mut a: int = 7;
    mut b: int = 3;
    mut bigger: int = a > b give a otherwise b;
    io::printline(bigger);

    // chains read left to right
    mut label: string = a > 10 give "big" otherwise a > 5 give "medium" otherwise "small";
    io::printline(label);
    return 0;
}
```

```text
7
medium
```

The ternary has the lowest precedence of any expression, lower even than assignment. It works directly as an initializer (above), but in a plain assignment wrap it in parentheses, or the assignment is taken as part of the condition (`n = n == 0 give 5 otherwise 6;` is a type mismatch, E066):

```turflang
fn main(): int {
    mut n: int = 0;
    n = (n == 0 give 5 otherwise 6);
    return n;
}
```

> `otherwise` has two unrelated meanings: the ternary's second half (here) and the catch-all arm of [`compare`](#compare--matches). They share a spelling and nothing else.

---

## `while`

```turflang
import io;

fn main(): int {
    mut i: int = 0;
    mut sum: int = 0;
    while i < 5 {
        sum = sum + i;
        i = i + 1;
    }
    io::printline(sum);
    return 0;
}
```

```text
10
```

A pre-test loop: the condition is checked before every iteration, so the body runs zero or more times.

---

## `do` / `while`

```turflang
import io;

fn main(): int {
    mut x: int = 5;
    mut iterations: int = 0;
    do {
        x = x - 1;
        iterations = iterations + 1;
    } while (x > 0);
    io::printline(iterations);

    // The body runs once even though the condition is false from the start.
    mut y: int = 0;
    do {
        y = y + 100;
    } while y < 10;
    io::printline(y);
    return 0;
}
```

```text
5
100
```

A post-test loop: the body always runs at least once, and the condition is checked after each pass. Parentheses around the condition are optional.

- The body must be braced. `do x -= 1; while x > 0;` is error E107 (`Expected '{' to open the do-block body`).
- The `while` after the closing brace is required (error E110, `Expected 'while' after do-block`).
- A `do` / `while` is the one loop that ends in a **`;`** after its condition, since it does not close with a `}`. A missing one is error E102 (`Expected ';' after do-while loop`); a second, stray `;` is error E103 (`Unexpected ';'`).
- The condition is outside the body's scope: a variable declared inside the body is not visible to it.

<!-- doc-example: fails E040 -->
```turflang
fn main(): int {
    do {
        mut finished: bool = true;
    } while finished;
    return 0;
}
```

`break` and `continue` inside a `do` / `while` act on that loop, whatever loops surround or sit inside it.

---

## `for`: range form

```turflang
import io;

fn main(): int {
    mut sum: int = 0;
    for i in 1..5 {
        sum = sum + i;
    }
    io::printline(sum);
    return 0;
}
```

```text
10
```

`for i in a..b { ... }` runs the body with `i` taking the values `a`, `a + 1`, ... up to **but not including** `b`. The range is always half-open; there is no inclusive form (`a..=b` is a syntax error), so write `a..b + 1` for "up to and including `b`".

### `step`

The optional `step` clause is an expression that runs after each pass of the body and advances the loop. When it is left out, it is `step i++` (for a `double` loop variable, `i++` adds `1.0`). Any expression is allowed:

```turflang
import io;

fn main(): int {
    for i in 0..10 step i += 3 {
        io::print(i);
        io::print(" ");
    }
    io::printline("");

    for x in 0.0..1.0 step x += 0.25 {
        io::print(x);
        io::print(" ");
    }
    io::printline("");
    return 0;
}
```

```text
0 3 6 9 
0 0.25 0.5 0.75 
```

If the `step` keyword is present it must be followed by an expression; `for i in 0..5 step { ... }` is a syntax error (E103).

### Counting down

The direction is taken from the bounds. If the start is below the end, the loop runs while `i < end`; if the start is above the end, it runs while `i > end`. The end is excluded either way, and an empty range (`0..0`) runs the body zero times. A descending range therefore needs a `step` that decreases:

```turflang
import io;

fn main(): int {
    for i in 5..1 step i-- {
        io::print(i);
    }
    io::printline("");

    for i in 10..0 step i -= 3 {
        io::print(i);
        io::print(" ");
    }
    io::printline("");
    return 0;
}
```

```text
5432
10 7 4 1 
```

The default step counts up, so `for i in 5..1 { ... }` never reaches its end and runs forever, as does any `step` that moves away from the end. The compiler does not catch this in general.

### The loop variable and the bounds

- Both bounds are evaluated once, before the first pass. Changing the variable a bound came from inside the body does not move the end.
- The loop variable takes its type from the bounds: `int` for integer bounds, `double` for `double` bounds. The bounds must be numbers.
- The variable exists only inside the loop; it is not visible after the closing brace.
- The body may assign to it (`i = i + 1`); the `step` then continues from the new value.
- `continue` still runs the `step` clause before the next test. A range `for` behaves like a `while` loop with the step folded in after the body.

### A `step` that never touches the loop variable

A `step` need not mention the loop variable, but one that never changes it, in a body that does not change it either, is almost certainly a mistake. The compiler warns:

```text
Warning: The loop variable 'i' is never changed by this loop's 'step' or its body, so it will loop forever: 'i' starts below the end value and never changes.
```

If a different variable controls when the loop ends, use `while`.

```turflang
fn main(): int {
    mut n: int = 0;
    // warns: `step` advances n, not i
    for i in 0..10 step n += 10 {
        n += 1;
        if n > 50 {
            break;
        }
    }
    return n;
}
```

The `(index, value)` pair form belongs to [for-each](#for-each-form); on a range it is error E120 (`The '(index, value)' form isn't supported for a range-based for-loop`).

<!-- doc-example: fails E120 -->
```turflang
fn main(): int {
    for (index, value) in 0..10 {
    }
    return 0;
}
```

---

## `for`: for-each form

`for value in array { ... }` visits the elements of a sized array in order. `for (index, value) in array { ... }` also binds the element's position: `index` is an `int` counting from 0, `value` has the array's element type.

```turflang
import io;

enum Suit { Hearts, Spades }

struct Card {
    rank: int,
    suit: Suit,
}

fn main(): int {
    immut numbers: int[4] = [10, 20, 30, 40];

    mut sum: int = 0;
    for value in numbers {
        sum = sum + value;
    }
    io::printline(sum);

    mut weighted: int = 0;
    for (index, value) in numbers {
        weighted = weighted + index * value;
    }
    io::printline(weighted);

    // elements can be structs, enums, or arrays
    mut hand: Card[2] = [Card::default(), Card::default()];
    hand[1].rank = 12;
    hand[1].suit = Suit::Spades;
    for card in hand {
        io::print(card.rank);
        io::print(" ");
    }
    io::printline("");

    immut grid: int[2][3] = [[1, 2, 3], [4, 5, 6]];
    mut total: int = 0;
    for row in grid {       // row: int[3]
        for cell in row {
            total = total + cell;
        }
    }
    io::printline(total);
    return 0;
}
```

```text
100
200
0 12 
21
```

- The loop variable is a **copy** of the element. Assigning to it does not change the array.
- Both variables exist only inside the loop. Giving them the same name (`for (x, x) in a`) is error E033, like any duplicate declaration.
- An [unsized parameter](arrays.md#unsized-parameters-t) (`arr: int[]`) can be iterated; it carries the caller's length, so the loop visits every element of whatever array was passed. So can a [slice](arrays.md#range-slicing-arrab) (`for v in a[1..3]`).

```turflang
import io;

fn sumAll(arr: int[]): int {
    mut total: int = 0;
    for value in arr {
        total += value;
    }
    return total;
}

fn main(): int {
    immut a: int[5] = [1, 2, 3, 4, 5];
    io::printline(sumAll(a));
    io::printline(sumAll(a[1..3]));
    return 0;
}
```

```text
15
5
```

Only built-in arrays can be iterated. A scalar, a `string`, a struct, and the collection types `List`, `Map` and `Set` are rejected with error E005 (`for-each requires an array!`). To walk a collection, loop over its indices:

```turflang
import io;
import list::List;

fn main(): int {
    mut l: List<int> = List::default();
    l.push(4);
    l.push(5);
    l.push(6);
    mut total: int = 0;
    for i in 0..l.length() {
        total = total + l.get(i);
    }
    io::printline(total);
    return 0;
}
```

```text
15
```

<!-- doc-example: fails E005 -->
```turflang
import list::List;

fn main(): int {
    mut l: List<int> = List::default();
    for value in l {
    }
    return 0;
}
```

---

## `break` and `continue`

Both are bare keywords. There are no labels, so neither can name an outer loop.

- `break` leaves the innermost enclosing loop.
- `continue` jumps to that loop's next iteration. In a range `for` the `step` clause runs first.
- Either one outside any loop is error E007 (`'break' used outside of a loop`, `'continue' used outside of a loop`).
- They work the same way in `while`, `do` / `while`, range `for` and for-each, in any nesting, and inside a [`compare`](#compare--matches) arm (where they act on the loop around the `compare`).

```turflang
import io;

fn main(): int {
    mut sum: int = 0;
    for i in 0..10 {
        if i == 3 {
            continue;
        }
        if i == 7 {
            break;
        }
        sum = sum + i;
    }
    io::printline(sum);    // 0 + 1 + 2 + 4 + 5 + 6

    for i in 0..3 {
        for j in 0..3 {
            if j == 1 {
                continue;   // skips only the inner iteration
            }
            io::print(i * 10 + j);
            io::print(" ");
        }
    }
    io::printline("");
    return 0;
}
```

```text
18
0 2 10 12 20 22 
```

<!-- doc-example: fails E007 -->
```turflang
fn main(): int {
    break;
    return 0;
}
```

---

## `defer`

`defer <statement>;` schedules a statement to run when the **enclosing block** exits, not when the function does. It runs on every way out of that block: reaching the closing brace, `return`, `break`, `continue`, and an error returned by [`try`](error_handling.md#try-passing-an-error-on) or a [`recover`](error_handling.md#recover--resolving-a-fallible-value) block.

It is the language's mechanism for deterministic cleanup (closing a file, releasing a lock, freeing a manual allocation) because there are no finalizers.

```turflang
import io;

fn work(n: int): int {
    defer io::printline("cleanup 1");
    defer io::printline("cleanup 2");
    if n < 0 {
        io::printline("early return");
        return -1;
    }
    io::printline("end of body");
    return n;
}

fn main(): int {
    io::printline(work(1));
    io::printline(work(-1));
    return 0;
}
```

```text
end of body
cleanup 2
cleanup 1
1
early return
cleanup 2
cleanup 1
-1
```

### Ordering

Deferred statements in one block run in **reverse** order of registration (last in, first out), as `cleanup 2` before `cleanup 1` above. This makes cleanup mirror acquisition: what was opened last is closed first.

When several blocks are left at once (a `return` three blocks deep), the pending defers of every block run, innermost block first.

```turflang
import io;

fn deep(): int {
    defer io::printline("d1");
    if true {
        defer io::printline("d2");
        while true {
            defer io::printline("d3");
            return 7;
        }
    }
    return 0;
}

fn main(): int {
    io::printline(deep());
    return 0;
}
```

```text
d3
d2
d1
7
```

### Scope: the block, not the function

A `defer` inside a loop body or an `if` branch belongs to that block. In a loop it runs once per iteration, however the iteration ends:

```turflang
import io;

fn main(): int {
    for i in 0..4 {
        defer io::printline("  end of iteration");
        if i == 1 {
            continue;
        }
        if i == 2 {
            break;
        }
        io::printline("  body");
    }
    io::printline("after loop");
    return 0;
}
```

```text
  body
  end of iteration
  end of iteration
  end of iteration
after loop
```

### When the statement is evaluated

The deferred statement is evaluated when it **runs**, not when it is written. It sees the variables' values at block exit. A `return` expression, however, is evaluated *before* the deferred statements run, so a deferred statement cannot change the value being returned:

```turflang
import io;

fn returned(): int {
    mut x: int = 1;
    defer x = 100;      // runs after `return x` has taken its value
    return x;
}

fn late(): void {
    mut n: int = 1;
    defer io::printline(n);   // prints 2, the value at block exit
    n = 2;
}

fn main(): int {
    io::printline(returned());
    late();
    return 0;
}
```

```text
1
2
```

### What may be deferred

A deferred statement is a single statement: a call, an assignment, a loop. A loop or other statement that ends in a `}` takes a `;` after it. A block (`defer { ... }`) or a declaration (`defer mut q: int = 3;`) is a syntax error.

A deferred statement **cannot jump out of itself**. `return`, `break` and `continue` aimed at something outside the `defer` are error E083 (`A deferred statement can't 'return'`), and `try` is error E095, because a defer runs while the function is already leaving and has nowhere to go:

<!-- doc-example: fails E083 -->
```turflang
fn main(): int {
    defer return 5;
    return 0;
}
```

A `break` or `continue` that belongs to a loop *inside* the deferred statement is fine; it never escapes:

```turflang
import io;

fn main(): int {
    mut n: int = 0;
    defer while n < 5 {
        n += 1;
        if n == 3 {
            break;
        }
    };
    return 0;
}
```

A fallible call inside a defer is resolved there with `recover`:

```turflang
import io;

error CloseError { Failed }

fn close(code: int): !void {
    if code != 0 {
        return CloseError::Failed;
    }
    return;
}

fn main(): int {
    defer close(1) recover run {
        io::printline("close failed");
    };
    return 0;
}
```

```text
close failed
```

### Defers and failures

- **Returned errors.** A `return` of an error value, a [`try`](error_handling.md#try-passing-an-error-on) that propagates one, and a `return` inside a `recover` block all run the pending defers of the blocks they leave.
- **Panics.** A panic (an out-of-bounds index, an integer-overflow trap, division by zero, a failed `test::assert`) aborts the process immediately. There is no unwinding, so **deferred statements do not run**. Do not rely on `defer` for cleanup that must happen on a crash.

```turflang
import io;

error ParseError { Empty, Bad }

fn stage(k: int): !int {
    if k == 1 { return ParseError::Empty; }
    return k;
}

fn pipeline(k: int): !int {
    defer io::printline("pipeline: cleanup");
    mut v: int = try stage(k);
    io::printline("pipeline: stage ok");
    return v + 1;
}

fn main(): int {
    io::printline(pipeline(0) recover produce -1);
    io::printline(pipeline(1) recover produce -1);
    return 0;
}
```

```text
pipeline: stage ok
pipeline: cleanup
1
pipeline: cleanup
-1
```

---

## `compare` / `matches`

`compare` selects one of several arms by matching a **subject** against patterns. It is a statement, not an expression. Each `matches` arm ends in `done`; the optional `otherwise` arm comes last and needs no `done`.

```turflang
import io;

fn word(n: int): string {
    mut s: string = "";
    compare n {
        matches 0:
            s = "none";
        done
        matches 1:
        matches 2:
            s = "few";
        done
        otherwise:
            s = "many";
    }
    return s;
}

fn main(): int {
    io::printline(word(0));
    io::printline(word(2));
    io::printline(word(40));
    return 0;
}
```

```text
none
few
many
```

- The subject and each `matches` value accept optional parentheses: `compare (x)`, `matches (1):`.
- **Stacked headers.** Several `matches` headers in a row, with no statements between them, share the body that follows (`matches 1:` and `matches 2:` above). The body runs when any of them matches. It is not "fall through into the next arm": each arm is separate.
- Every `matches` arm ends in `done`; leaving it out is error E058 (`Expected 'done' after matches arm body`). An arm needs at least one statement.
- `otherwise` must be the last arm and may appear only once; a second one, or one that is not last, is error E058. It ends at the closing `}` of the `compare`, so it takes no `done`.
- Any statement may appear in an arm, including `return`, `break` and `continue` (the last two act on the loop around the `compare`).
- A subject can be an `int`, `char`, `string`, `bool`, an `enum` or [`machine`](state_machines.md), a struct, or an error value. A pattern whose type does not fit the subject is error E059 (`Subject has type 'string' but a matches arm has type 'int'`).
- If no arm matches and there is no `otherwise`, the `compare` does nothing, except where the subject must be covered ([Exhaustiveness](#exhaustiveness)).
- A `compare` with no arms and no `otherwise` compiles and warns that it is a no-op.
- Matching a value that was already matched earlier in the same `compare` is error E133 (`Duplicate pattern`): the second arm could never run.

<!-- doc-example: fails E133 -->
```turflang
fn main(): int {
    mut n: int = 1;
    compare n {
        matches 1:
            n = 10;
        done
        matches 1:
            n = 20;
        done
    }
    return n;
}
```

A string subject compares by content, and a `char` subject (such as `s[0]`) by value:

```turflang
import io;

fn kind(s: string): string {
    mut r: string = "other";
    compare s {
        matches "yes":
        matches "y":
            r = "affirmative";
        done
        matches "no":
            r = "negative";
        done
    }
    return r;
}

fn leading(s: string): int {
    mut r: int = 0;
    compare s[0] {
        matches 'a':
            r = 1;
        done
        matches _ if s[0] >= '0':
            r = 2;
        done
        matches _:
            r = 3;
        done
    }
    return r;
}

fn main(): int {
    io::printline(kind("y"));
    io::printline(kind("maybe"));
    io::printline(leading("abc"));
    io::printline(leading("5"));
    io::printline(leading("!"));
    return 0;
}
```

```text
affirmative
other
1
2
3
```

### Patterns

A `matches` header is a **value** (the subject is compared with it by equality) or one of the forms that look inside the subject. A bare identifier at the top of a header is an ordinary variable to compare against; it never binds.

| Header | Meaning |
|---|---|
| `matches 3:`, `matches "x":`, `matches 'c':`, `matches true:` | equals this literal |
| `matches Color::Red:` | equals this enum or machine variant |
| `matches limit:` | equals the value of the variable or expression `limit` |
| `matches _:` | matches anything, binds nothing |
| `matches Point { x: 0, y }:` | destructures a struct (see below) |
| `matches IOError::Denied(path, ..):` | destructures an error variant's payload |
| `matches <pattern> if <condition>:` | the pattern matches **and** the condition holds |

`_` is the wildcard and is reserved: it cannot be a variable, parameter or function name (error E092). `_name` (an underscore followed by anything) is an ordinary identifier, and, like any name that starts with `_`, is never reported as unused.

#### Struct patterns

`Name { field: pattern, field, .. }` matches a struct and picks it apart. It may also be written `module::Name { ... }`.

```turflang
import io;

struct Point {
    x: int,
    y: int,
}

struct Line {
    a: Point,
    b: Point,
}

fn classify(p: Point): int {
    compare p {
        matches Point { x: 0, y: 0 }:      // test both fields
            return 1;
        done
        matches Point { x, y } if x == y:  // bind both, then a guard
            return 2;
        done
        matches Point { x: 0, .. }:        // test x, ignore every other field
            return 3;
        done
        matches _:                         // anything at all
            return -1;
        done
    }
}

fn horizontal(l: Line): bool {
    compare l {
        matches Line { a: Point { y: ay, .. }, b: Point { y: by, .. } } if ay == by:
            return true;
        done
        matches _:
            return false;
        done
    }
}

fn pt(x: int, y: int): Point {
    mut p: Point = Point::default();
    p.x = x;
    p.y = y;
    return p;
}

fn main(): int {
    io::printline(classify(pt(0, 0)));
    io::printline(classify(pt(3, 3)));
    io::printline(classify(pt(0, 9)));
    io::printline(classify(pt(1, 2)));

    mut l: Line = Line::default();
    l.a = pt(0, 4);
    l.b = pt(9, 4);
    io::printline(horizontal(l) give "level" otherwise "sloped");
    return 0;
}
```

```text
1
2
3
-1
level
```

What goes in a field's position:

| Field pattern | Meaning |
|---|---|
| an integer literal (`5`, `-5`), a `char`, `string` or `bool` literal | the field equals it |
| an enum or machine variant (`Color::Red`) | the field equals it |
| `_` | any value, not bound |
| a name (`y`, or `field: name`) | **binds** the field's value to that name |
| another struct pattern | destructures a nested struct, to any depth |

Rules for struct patterns:

- **A bare name always binds.** Inside a destructuring pattern, `Point { x: limit }` binds the field to a new name `limit`; it does not compare with an existing variable `limit`. To compare a field with a variable or any other expression, bind it and use a guard: `matches Point { x, .. } if x == limit:`.
- **`double` literals are not patterns** (exact float equality is rarely what is meant; error E092). Bind the field and test it in a guard.
- **Every field is listed, or the pattern ends in `..`.** `Point { x }` on a two-field struct is error E092 (`this pattern doesn't mention the field 'y' of 'P'`), so adding a field to a struct later cannot silently change what an existing pattern means.
- **Fields are read as `p.field` reads them**, so another module's non-`pub` field cannot be named (error E088); write `..` to ignore it.
- **What can be matched:** a struct value, a `&T` reference, and `self` inside a method. A `&?T` (which may be null) and a raw `T*` are refused (E092); test a `&?T` for null first.
- A field named twice, an unknown field, a literal of the wrong type, and a struct pattern for a different struct than the subject are all E092 (or E068 for an unknown field), each with a message that names the field.

#### Bindings

Bindings are **immutable copies** of the matched values. They are visible in the header's guard and in that arm's body, nowhere else, and they shadow an outer variable of the same name for that arm only:

```turflang
import io;

struct Point {
    x: int,
    y: int,
}

fn main(): int {
    mut x: int = 100;
    mut p: Point = Point::default();
    p.x = 5;
    compare p {
        matches Point { x, .. }:
            io::printline(x);       // this x is p.x
        done
    }
    io::printline(x);               // the outer x again
    return 0;
}
```

```text
5
100
```

Assigning to a binding is error E060 (`You can't change the value of 'x' because it's immutable`).

#### Guards

`matches <pattern> if <condition>:` runs the arm only when the pattern matches and the condition is `true`. The pattern's bindings are visible in the condition. A guard that fails does not leave the `compare`: matching continues with the next header.

#### Order and evaluation

Headers are tried **top to bottom**, and the first one that matches (pattern and guard) runs. Put the specific headers first and the general ones after them. The subject is evaluated **once**; every header and guard tests that one value.

#### Alternatives

Stacked headers are the way to write "this or that". They share one body, so every stacked header must bind exactly the same names with the same types (or none), else error E092. Each header keeps its own guard.

```turflang
import io;

struct Point {
    x: int,
    y: int,
}

fn main(): int {
    mut p: Point = Point::default();
    p.x = 1;
    p.y = 7;
    compare p {
        matches Point { x: 0, y }:
        matches Point { x: 1, y }:      // either header runs the body, with y bound
            io::printline(y * 10);
        done
        otherwise:
            io::printline("no");
    }
    return 0;
}
```

```text
70
```

### Exhaustiveness

Without an `otherwise`:

| Subject | Requirement | Otherwise |
|---|---|---|
| `enum` or `machine` | every variant has an **unguarded** header | error E093 (`Non-exhaustive enum comparison`), listing the missing variants |
| `bool` | both `true` and `false` have an unguarded header | error E093 (`Non-exhaustive bool comparison`) |
| a declared `error` type | every variant has an unguarded variant pattern whose fields all match anything | error E093 (`Non-exhaustive error comparison`) |
| an error value from `recover as err run` | cannot be covered by variants; always needs `otherwise` or an unguarded `_` | error E093 |
| `int`, `char`, `string`, or a struct with a refutable pattern | not required; if nothing matches, the `compare` does nothing | none |

A **guarded** arm never counts toward exhaustiveness, because its guard may be false. `matches _:` (unguarded) covers everything and is the way to say "and everything else" without `otherwise`.

<!-- doc-example: fails E093 -->
```turflang
enum Color { Red, Green, Blue }

fn main(): int {
    mut c: Color = Color::Red;
    mut n: int = 0;
    compare c {
        matches Color::Red:
            n = 1;
        done
        matches Color::Green:
            n = 2;
        done
    }
    return n;
}
```

When every possibility is covered and every arm returns, no `return` is needed after the `compare`:

```turflang
import io;

enum Color { Red, Green, Blue }

fn name(c: Color): string {
    compare c {
        matches Color::Red:
            return "red";
        done
        matches Color::Green:
            return "green";
        done
        matches Color::Blue:
            return "blue";
        done
    }
}

fn main(): int {
    io::printline(name(Color::Blue));
    return 0;
}
```

```text
blue
```

### Unreachable arms

An arm, or the `otherwise`, that follows an unguarded header which already matches everything (`_`, or a struct pattern whose fields all bind or ignore) can never run. This is the warning E094 (`This arm can never run`, or `The 'otherwise' arm can never run`). The program still compiles; delete the arm or move it above the catch-all. A duplicate *value* pattern is the error E133 described above.

```turflang
import io;

fn main(): int {
    mut n: int = 3;
    compare n {
        matches _:
            io::printline("anything");
        done
        matches 3:                     // warning E094: can never run
            io::printline("three");
        done
    }
    return 0;
}
```

---

## Control flow and error values

Error values take part in control flow in three ways. The full treatment is in [Error Handling](error_handling.md).

**Propagating.** `try e` returns `e`'s error unchanged from the enclosing `!V` function, exactly as a `return` would: the pending [`defer`s](#defers-and-failures) of the blocks it leaves run first. See [`try`](error_handling.md#try-passing-an-error-on).

**Resolving.** [`recover`](error_handling.md#recover--resolving-a-fallible-value) turns a `!T` into a `T`. Inside a `recover run` or `recover as err run` block, `return`, `break` and `continue` act on the **enclosing function or loop**, not on the `recover` expression. A `break` in a recover block therefore ends the surrounding loop.

**Matching.** `err` in `recover as err run` can be any error a callee returns, so a `compare` on it needs an `otherwise` (or an unguarded `_`). Variant patterns (`Type::Variant`, with `(...)` when the variant has fields) pick out one error, positionally destructure its payload with the same sub-patterns as a struct pattern, and accept guards. A variant pattern never counts as a catch-all.

`continue` and `break` below are therefore ordinary loop control, written from inside the error handler:

```turflang
import io;

error NetError {
    Timeout,
    Refused(port: int),
}

fn connect(port: int): !int {
    if port == 0 {
        return NetError::Timeout;
    }
    if port < 1024 {
        return NetError::Refused(port);
    }
    return port;
}

fn first_open(ports: int[4]): int {
    for p in ports {
        mut port: int = connect(p) recover as err run {
            compare err {
                matches NetError::Timeout:
                    continue;                        // next port
                done
                matches NetError::Refused(q) if q == 80:
                    break;                           // give up entirely
                done
                otherwise:
                    continue;
            }
        };
        return port;
    }
    return -1;
}

fn main(): int {
    immut ports: int[4] = [0, 22, 8080, 9000];
    io::printline(first_open(ports));

    immut blocked: int[4] = [0, 80, 8080, 9000];
    io::printline(first_open(blocked));
    return 0;
}
```

```text
8080
-1
```

A variant that carries fields must say what to do with them: `matches IOError::Denied(path, 0):` tests the second field and binds the first; `matches IOError::Denied(..):` ignores both; a bare `matches IOError::Denied:` is error E092 for a variant that has fields. A variant with no fields is written bare (`matches IOError::Timeout:`). The payload ([Payloads](error_handling.md#payloads-data-on-a-variant)) is the only way to read a variant's fields; the message is read with `err.message`.

For a value of a *declared* error type (not an open `err`), the compare is checked for [exhaustiveness](#exhaustiveness) variant by variant:

```turflang
import io;

error IOError {
    Denied(path: string, mode: int),
    Timeout,
}

fn describe(e: IOError): string {
    compare e {
        matches IOError::Denied(path, 0):
            return path + ": read-only";
        done
        matches IOError::Denied(path, ..):
            return path + ": denied";
        done
        matches IOError::Timeout:
            return "timed out";
        done
    }
}

fn main(): int {
    io::printline(describe(IOError::Timeout));
    return 0;
}
```

```text
timed out
```
