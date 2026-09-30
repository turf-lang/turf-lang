# `json`

A hand-rolled, dependency-free JSON parser and serializer. Not prelude —
requires an explicit `import json;`.

```turflang
import json;
```

> **Why hand-rolled, not an FFI binding to a system JSON library.**
> Binding a C library like `cjson`/`json-c`/`jansson` via `extern from
> "libname" fn` (the way [`math`](./math_and_os.md) binds libm) would make
> `import json;` fail to *link* on any machine that doesn't happen to have
> that specific library installed — a real portability cost for a standard
> library module that's supposed to just work. This parser has no external
> dependency beyond what every other stdlib module already needs.

## `JsonValue`

A tagged-union-shaped struct: a `JsonType` tag (`Null`/`Bool`/`Number`/
`String`/`Array`/`Object`) plus one field per variant. Construct one with
the static constructors below, or get one back from `json::parse`.

### Constructors

`JsonValue::null_value()`, `JsonValue::from_bool(b: bool)`,
`JsonValue::from_number(n: double)`, `JsonValue::from_string(s: string)`,
`JsonValue::new_array()`, `JsonValue::new_object()`.

### Building up a value

- **`.push(item: JsonValue): void`** — appends to an array-typed value.
- **`.set(key: string, value: JsonValue): void`** — sets a field on an
  object-typed value.

```turflang
mut config: json::JsonValue = json::JsonValue::new_object();
config.set("dice_sides", json::JsonValue::from_number(6.0));
config.set("rolls", json::JsonValue::from_number(5.0));
```

### Reading a value

- **`.get(key: string): JsonValue`** — object field access by key (linear
  scan over the object's keys, same tradeoff `Map<K, V>` already accepts —
  see [`collections.md`](./collections.md)).
- **`.has(key: string): bool`**
- **`.get_string(key: string): string`** / **`.get_number(key: string): double`** / **`.get_int(key: string): int`** / **`.get_bool(key: string): bool`** — typed convenience accessors, so a caller doesn't have to unwrap `.get(key)`'s `JsonValue` by hand for the common case.
- **`.at(index: int): JsonValue`** — array element access by index.
- **`.length(): int`** — element/field count of an array or object value.

## Parsing and serializing

### `json::parse(s: string): !JsonValue`

Parses a JSON text into a `JsonValue` tree. Returns
`error JsonError { UnexpectedEnd(offset: int), UnexpectedChar(offset: int),
InvalidNumber(offset: int), InvalidLiteral(offset: int),
UnterminatedString(offset: int), ExpectedColon(offset: int),
ExpectedComma(offset: int) }` on malformed input, rather than panicking —
malformed JSON (a truncated file, a hand-edited config) is an ordinary,
expected failure mode. Every variant carries the byte offset into the text at
which the parser gave up; read it with
`matches json::JsonError::UnexpectedChar(at):` in a `compare` over `err`, or
ignore it and compare `err == json::JsonError::UnexpectedChar` as before.

```turflang
mut v: json::JsonValue = json::parse("{\"a\": 1}") recover run {
    io::printline("malformed JSON");
    return 1;
};
test::assert(v.get_int("a") == 1, "");
```

### `json::stringify(v: JsonValue): string`

Serializes a `JsonValue` back to compact JSON text — the inverse of
`parse`, so `parse(stringify(v))` round-trips.

```turflang
immut text: string = json::stringify(config);
```

## Known gaps

Handles the common real cases correctly (objects, arrays, strings,
integer/decimal numbers, `true`/`false`/`null`, nesting) but is not a full
RFC 8259 implementation — `\uXXXX` Unicode escape sequences in string
literals are not supported.
