# JSON: `json`

Parse JSON text into a tree of values, build a tree by hand, and write it back
out as text. The module is written in Turf, with no C library behind it, so it
works the same on every platform. Not prelude: it needs `import json;`.

```turflang
import json;
```

A JSON document is one `JsonValue`. A `JsonValue` has a **kind** (null, bool,
number, string, array or object) and holds the data for that kind. `parse` makes
one from text, `stringify` turns one into text, and in between you read it with
`get_string`, `get_int` and friends, or build one with `new_object`, `set`,
`new_array` and `push`.

## Example: a round trip

This reads a settings document, changes it, adds a field, and writes it back.
It follows the library's [failure rule](README.md#2-failure-three-kinds-three-shapes):
text that might be malformed is parsed with `recover`, and `has` asks before a
`get` that would panic.

```turflang
import json;

fn main(): int {
    immut text: string = "{\"name\":\"Ada\",\"visits\":3,\"tags\":[\"admin\",\"dev\"]}";

    mut doc: json::JsonValue = json::parse(text) recover as err run {
        eprintline("not valid JSON");
        return 1;
    };

    printline(doc.get_string("name"));         // Ada
    printline(doc.get_int("visits"));          // 3
    printline(doc.get("tags").length());       // 2
    printline(doc.get("tags").at(1).str_val);  // dev

    doc.set("visits", json::JsonValue::from_number(doc.get_number("visits") + 1.0));
    if !doc.has("active") {
        doc.set("active", json::JsonValue::from_bool(true));
    }
    doc.get("tags").push(json::JsonValue::from_string("ops"));

    printline(json::stringify(doc));
    return 0;
}
```

```text
Ada
3
2
dev
{"name":"Ada","visits":4,"tags":["admin","dev","ops"],"active":true}
```

---

## `JsonType` and `JsonValue`

```text
enum JsonType { Null, Bool, Number, String, Array, Object }

struct JsonValue {
    pub kind: JsonType,
    pub bool_val: bool,
    pub num_val: double,
    pub str_val: string,
    pub arr_val: List<JsonValue>,
    pub obj_keys: List<string>,
    pub obj_vals: List<JsonValue>,
}
```

Only the fields that belong to `kind` mean anything; the others hold their
default (`false`, `0`, `""`, empty lists). A number is always a `double`. An
object is two parallel lists, keys and values, **in insertion order**: the first
key set is the first written out, and setting an existing key replaces its value
in place.

A `JsonValue` is shared the way a collection is
([Collections: shared by default](collections.md#shared-by-default-copied-on-demand)):
copying one, or putting it inside another, does not copy the arrays and objects
it holds. A later `set` or `push` on the original shows through every place it
was put.

```turflang
import json;

fn main(): int {
    mut inner: json::JsonValue = json::JsonValue::new_object();
    mut outer: json::JsonValue = json::JsonValue::new_array();
    outer.push(inner);
    inner.set("late", json::JsonValue::from_bool(true));
    printline(json::stringify(outer));   // [{"late":true}]
    return 0;
}
```

---

## Making values

All of these are static: `json::JsonValue::from_string("x")`.

| Constructor | Result |
|---|---|
| `JsonValue::null_value(): JsonValue` | `null` |
| `JsonValue::from_bool(b: bool): JsonValue` | `true` or `false` |
| `JsonValue::from_number(n: double): JsonValue` | a number |
| `JsonValue::from_string(s: string): JsonValue` | a string |
| `JsonValue::new_array(): JsonValue` | `[]` |
| `JsonValue::new_object(): JsonValue` | `{}` |

### `JsonValue.push(item: JsonValue): void`

Appends `item` to an array.

### `JsonValue.set(key: string, value: JsonValue): void`

On an object, sets `key` to `value`, replacing the value if the key is already
there. Lookup is a linear scan, so an object with thousands of keys is slow to
build; JSON objects are meant to be small.

`push` and `set` do not check the value's kind. Calling `push` on a number
stores the item in its array field, which `stringify` ignores because the kind
is still `Number`. Make the container with `new_array` or `new_object` first.

---

## Reading values

### By key: `get_string`, `get_number`, `get_int`, `get_bool`

```text
JsonValue.get_string(key: string): string
JsonValue.get_number(key: string): double
JsonValue.get_int(key: string): int
JsonValue.get_bool(key: string): bool
```

Read a member of an object. A **missing key panics** (below). A member of the
wrong kind is not an error: it reads as the default, `""`, `0`, `0.0` or `false`
(`get_string` on a number is `""`). `get_int` truncates toward zero
(`36.9` reads as `36`). Check `kind` when the kind matters.

### `JsonValue.get(key: string): JsonValue`

The member as a `JsonValue`, to read further into it
(`doc.get("user").get_string("name")`). It **panics** if the key is not there,
with a message that starts with your call's `file:line:` and names the key:
`app.tr:12: JsonValue.get: no such key 'zz'`. It also panics on a value that is not
an object, which has no keys.

### `JsonValue.has(key: string): bool`

Whether an object has `key`. Ask with this before `get` when a key is optional.
It is `false` on anything that is not an object.

### `JsonValue.at(index: int): JsonValue`

The element of an array. It **panics** with `JsonValue.at: index out of bounds`
(prefixed by your call's `file:line:`) if `index` is negative or not less than the
length, and on any value that is not an array.

### `JsonValue.length(): int`

The number of elements of an array or keys of an object; `0` for every other
kind. It does not panic.

### The fields

`kind`, `bool_val`, `num_val`, `str_val`, `arr_val`, `obj_keys` and `obj_vals`
are public. Use `kind` to branch on what a value is, and the field for that kind
to read it:

```turflang
import json;

fn describe(v: json::JsonValue): string {
    compare v.kind {
        matches json::JsonType::Null:
        return "null";
        done
        matches json::JsonType::Bool:
        return "bool " + (v.bool_val to string);
        done
        matches json::JsonType::Number:
        return "number " + (v.num_val to string);
        done
        matches json::JsonType::String:
        return "string " + v.str_val;
        done
        matches json::JsonType::Array:
        return "array of " + (v.length() to string);
        done
        otherwise:
        return "object with " + (v.length() to string) + " keys";
    }
}

fn main(): int {
    immut doc: json::JsonValue = json::parse("[1, \"two\", null, {\"k\": true}]") recover run {
        return 1;
    };
    mut i: int = 0;
    while i < doc.length() {
        printline(describe(doc.at(i)));
        i = i + 1;
    }
    return 0;
}
```

```text
number 1
string two
null
object with 1 keys
```

---

## `stringify(v: JsonValue): string`

Writes `v` as compact JSON text, with no whitespace between tokens. Object keys
come out in insertion order. Strings are escaped with `escape_string`.

A **number is written so that it reads back as exactly the same `double`**.
An integral value within ±2^53 is written as an integer (`1700000000`,
`-3`); any other value with the fewest significant digits that round-trip,
at most 17 (`0.5`, `0.30000000000000004` for `0.1 + 0.2`, `1e+21`). Ids,
timestamps and counts therefore survive `stringify` and `parse` unchanged.
A number that is infinite or NaN is written as `inf` or `nan`, which is not valid
JSON; the parser itself produces infinity for an out-of-range number such as
`1e400`.

## `escape_string(s: string): string`

The body of a JSON string literal for `s`, **without** the surrounding quotes.
Use it when you assemble JSON text by hand (an HTTP handler putting a name into
a response) so that the text can never break out of its string. It writes `\"`
and `\\`, the short forms `\n \r \t \b \f`, and `\u00XX` for every other control
character below space. Bytes of non-ASCII (UTF-8) text pass through unchanged,
and `/` is not escaped. `stringify` uses it for every string and key it writes.

```turflang
import json;

fn main(): int {
    printline(json::escape_string("say \"hi\"\n"));   // say \"hi\"\n
    return 0;
}
```

---

## `parse(s: string): !JsonValue`

Parses `s` as one JSON value (an object, array, string, number, `true`, `false`
or `null`), with any whitespace around it and any nesting. It returns
`!JsonValue`: malformed text is input, not a bug, so it fails with an error
rather than panicking.

### `error JsonError`

```text
error JsonError {
    UnexpectedEnd(offset: int),
    UnexpectedChar(offset: int),
    InvalidNumber(offset: int),
    InvalidLiteral(offset: int),
    UnterminatedString(offset: int),
    ExpectedColon(offset: int),
    ExpectedComma(offset: int),
}
```

Every variant carries the **byte offset** into the text where the parser gave
up (counting from 0). Compare with `err == json::JsonError::ExpectedComma` to ask
only which error, or read the offset with a pattern inside a `compare`:

| Variant | Raised when | Example text | Offset |
|---|---|---|---|
| `UnexpectedEnd` | the text ends where a value should start | `""`, `"[1,"` | `0`, `3` |
| `UnexpectedChar` | a character cannot start a value, or an object key is not a string | `"@"`, `"{a:1}"`, `"[1,]"` | `0`, `1`, `3` |
| `InvalidNumber` | `-` or a number with no digits, or an exponent with none | `"-"`, `"-x"`, `"1.5e"` | where the number starts |
| `InvalidLiteral` | `true`, `false` or `null` is misspelled or cut short | `"tru"` | where the parser stopped |
| `UnterminatedString` | the closing quote is missing | `"\"abc"` | end of the text |
| `ExpectedColon` | no `:` after an object key | `{"a" 1}` | `5` |
| `ExpectedComma` | no `,` or closing bracket after an element | `[1 2]`, `{"a":1 "b":2}` | `3`, `7` |

```turflang
import json;

fn report(text: string): void {
    immut v: json::JsonValue = json::parse(text) recover as err run {
        compare err {
            matches json::JsonError::UnexpectedEnd(at):
            printline("the text ended early, at byte " + (at to string));
            done
            matches json::JsonError::UnexpectedChar(at):
            printline("unexpected character at byte " + (at to string));
            done
            matches json::JsonError::ExpectedComma(at):
            printline("expected a comma at byte " + (at to string));
            done
            otherwise:
            printline("malformed JSON");
        }
        return;
    };
    printline("parsed " + json::stringify(v));
}

fn main(): int {
    report("[1, 2, 3]");
    report("[1,");
    report("[1 2]");
    report("{a: 1}");
    return 0;
}
```

```text
parsed [1,2,3]
the text ended early, at byte 3
expected a comma at byte 3
unexpected character at byte 1
```

### What the parser accepts

The parser is lenient in a few places, because its job is to read the one value
in a request or response body, not to validate a document:

- **Text after the value is ignored.** `{"a":1} trailing` parses as `{"a":1}`,
  and `1 2` as `1`.
- A number may have a leading zero (`01` is `1`) or end in a dot (`1.` is `1`).
- A duplicate object key keeps the last value.
- Numbers use `e` or `E` exponents (`1e10`, `2.5E-3`) and are read as `double`.

### Known limitation: `\uXXXX`

A `\uXXXX` escape in a string is **not decoded**: the parser drops the backslash
and keeps the rest as plain characters, so `"A"` reads as `u0041` instead of
`A`. The other escapes (`\" \\ \/ \n \t \r`) are decoded. A `\b` or `\f` escape
is read as the letter `b` or `f`, so a string containing those control
characters does not survive a `stringify` and `parse` round trip. UTF-8 text
written directly in the document (`"é"`) is read correctly.

## Failure summary

| Call | On a problem |
|---|---|
| `parse` | returns `JsonError` with the byte offset |
| `JsonValue.get`, `get_string`, `get_number`, `get_int`, `get_bool` | **panic** if the key is missing (ask with `has`) |
| `JsonValue.at` | **panics** if the index is out of range |
| `stringify`, `escape_string`, `length`, `has`, the constructors | never fail |
