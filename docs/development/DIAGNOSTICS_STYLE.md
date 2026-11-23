# Diagnostics style guide

How every compiler message, hint and label is worded (DX-9, TDP-2-028,
decision D-28). A reviewer checks a new or changed diagnostic against this
page; the snapshot test of every code (`scripts/check/diagnostic_snapshots.py`,
DX-10) shows the change as a diff.

A diagnostic is read by a beginner who has just been stopped. It says what
the compiler found, in words the reader already has, and what they can do
about it.

## The five rules

### 1. First person

The compiler speaks as "I". It says what it found or could not do, never what
the reader did wrong.

| Before | After |
|---|---|
| You tried to store an int in a string variable! | I cannot store an `int` in `s`, which is a `string`. |
| The function 'f' promises to give back a result, but sometimes it doesn't! | I found a path through `f` that ends without returning an `int`. |

### 2. One idea per sentence

A message is one or two short sentences. A second fact goes in its own
sentence, a related place goes in a note, and what to do goes in the hint.

| Before | After |
|---|---|
| Cannot cast between string and non-pointer types. SrcType: int, DestType: string, Context Name: argcast | I cannot convert an `int` to a `string` with `to`. |

### 3. Name the types, never "value"

Say which type was found and which was expected. "Value", "thing", "this"
and "something" tell the reader nothing they can look up.

| Before | After |
|---|---|
| Operator applied to the wrong kind of value | I cannot use `+` on a `bool` and an `int`. |
| A void value can't be used here | `log` returns `void`, so I have nothing to store in `n`. |

### 4. No exclamation marks

A diagnostic is not an alarm. Banners such as "Type mismatch!" or "Oops!" are
not used; the heading already says what kind of problem it is.

| Before | After |
|---|---|
| Type mismatch! | (no banner; the message starts with the fact) |
| Oops! Something doesn't add up: | (no banner) |

### 5. Every hint is something the reader can do

A hint starts with a verb the reader can act on: write, add, remove, rename,
wrap, call, pass, import, declare, move. A hint that restates the message, or
explains without suggesting anything, is cut or moved into the message.

| Before | After |
|---|---|
| Hint: Array indices start at 0. | Hint: Use an index from 0 to 3, or `-1` to `-4` to count from the end. |
| Hint: This is not allowed. | Hint: Wrap the call in `unsafe { }`. |

## Mechanics

- **Code in backticks.** A name, a type, a keyword or a piece of source is
  written in backticks: `` `s` ``, `` `int` ``, `` `unsafe { }` ``. Single
  quotes are not used for code.
- **Lowercase span labels.** A label under a span is a lowercase phrase with
  no full stop: `this is an `int``, `` `s` is a `string` because of this ``.
- **Heading = registered name.** The heading prints the code and its name
  from the registry (`src/diagnostics/ErrorCodes.h`, the V-5 registry):
  `── error at line 4, col 9 [E068 · unknown struct field]`. The name is
  lowercase there, and `scripts/check/error_codes_check.sh` fails when a row
  has no name. The `--smart` category is not printed.
- **Names are short noun phrases** that say the kind of mistake, without a
  verdict: `unknown struct field`, `missing return`, `wrong argument type`.
- **Full sentences end with a full stop**; a heading or a label does not.
- **Numbers as digits**, units spelled: `4 elements`, `2 type arguments`.
- **No internal words.** No C++ class names, no LLVM terms, no "SrcType",
  "Context Name", "codegen" or "resolver" in what a user reads. An internal
  error says that it is a compiler bug and how to report it, nothing more.
- **`turf`, not `turfc`**, wherever a command is suggested.

## Warnings

A warning follows the same rules. Its hint says how to silence it on purpose
when that is a legitimate choice (`--allow E163`, `_name`).

## When rules collide

Clarity wins. A message that would need three sentences to follow rule 2 is
probably two diagnostics, or a message plus a note.
