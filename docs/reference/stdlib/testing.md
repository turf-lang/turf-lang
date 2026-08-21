# Testing: `test`

Minimal assertion-based test helpers, built on `sys::panic`. Not prelude —
requires `import test;`.

```turflang
import test;
```

`test.tr` also internally does `import strings;` for its own string
`==`/`!=`/`+` usage when formatting failure messages. Because virtually
every test file already does `import test;`, this incidentally makes string
operators feel globally available across the test suite — but that's just a
side effect of `test.tr`'s own import, not a separate prelude mechanism;
`strings` is already prelude on its own regardless (see
[`core.md`](./core.md)).

> **Note: `test::assert`/`test::fail` are not prelude.** Bare
> `assert(...)`/`fail(...)` without `import test;` fails to compile — unlike
> `int`/`double`/`bool`/`char`/`strings`/`array`, which need no import at
> all. This is confirmed by dedicated compile-failure tests
> (`tests/16_sys_and_test_stdlib/fail_bare_assert_no_import.tr`,
> `fail_bare_fail_no_import.tr`).

---

## `fail(msg: string): void`

Thin wrapper over `sys::panic(msg)` — prints `msg` to stderr and aborts
immediately. Never returns.

---

## `assert(cond: bool): void`

Calls the two-argument overload with a canned default message:

```
Oops! An assertion failed here.
Hint: Assertions are checks that make sure your code is running exactly as
you expect. This one didn't pass.
```

---

## `assert(cond: bool, msg: string): void`

If `!cond`, calls `fail(msg)`.

```turflang
test::assert(1 + 1 == 2, "math should work");
```

---

## `assert_eq(left, right): void` / `assert_eq(left, right, msg: string): void`

Overloaded for `int`, `double`, `bool`, `string` — four independent overload
sets, not a single generic `assert_eq<T>`. Equality on an arbitrary generic
`T` isn't guaranteed to mean anything, since operator overloading in Turf is
per-type, so this was deliberately kept as real per-type overloads.

If `left != right`, formats `"assert_eq failed: <left> != <right>"`
(optionally appending `" (<msg>)"` when `msg` is non-empty) and fails.

```turflang
test::assert_eq(x, 5);
test::assert_eq(x, 5, "x should be 5");

// used throughout essentially every regression test in the repo:
test::assert_eq(twelve.gcd(18), 6, "gcd(12, 18) == 6");
```

---

## `assert_neq(left, right): void` / `assert_neq(left, right, msg: string): void`

Same four-type overload set as `assert_eq`. If `left == right`, fails with
`"assert_neq failed: both sides equal <left>"` (plus the optional `"
(<msg>)"`).

```turflang
test::assert_neq(5, 6);
test::assert_neq(5, 6, "five should not equal six");
```

---

## Full example

```turflang
import test;

fn main(): int {
    mut m: Map<string, int>;
    m.put("a", 1);

    test::assert(m.contains("a"), "map should contain key 'a'");
    test::assert_eq(m.get("a"), 1, "value for 'a' should be 1");
    test::assert_neq(m.get("a"), 2, "value for 'a' should not be 2");

    return 0;
}
```
