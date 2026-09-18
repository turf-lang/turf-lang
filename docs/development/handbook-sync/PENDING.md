# PENDING.md — handbook edits owed

The handbook (`book/`) and the user-facing reference (`docs/reference/`) are
paused while the compiler changes underneath them. Every feature group appends
here rather than editing those files mid-flight, so that when writing resumes
there is one queue to work from instead of a 36,900-line diff to re-read.

One row per affected chapter or reference file. Move a row to `DONE.md` with
the commit that did it — do not delete it.

| Feature | Chapter / file | What is owed | Source record |
|---|---|---|---|
| ARCH-001 | `AGENTS.md` | Nothing owed — ARCH-001 added the one pointer line itself, and `AGENTS.md` is not part of the handbook. Listed so the next session sees the queue is genuinely empty, not merely unstarted. | [ARCH-001](../changes/ARCH-001.md) |
| TYPE-001 | `docs/reference/structs_and_enums.md` §"Self-referential structs" | Add the by-value rule next to the existing pointer-field guidance (lines 46–83): a struct field, or an array-of-struct field, that holds another struct **by value** (not `T*`/`&T`/`&?T`) may not form a cycle with any other by-value field, directly or through a chain — rejected at compile time as `E069`/`StructLayoutCycleError`, naming the full cycle. The existing `next: Node*` example (and its `unsafe { }`/no-raw-pointer-parameter notes) is unaffected and stays exactly as written — pointer fields were never in scope for this rule. Suggested new example: `struct A { b: B } struct B { a: A }` rejected, contrasted with the same pair rewritten with `b: &B`/`a: A` (or either field made a pointer) to show the fix. | [TYPE-001](../changes/TYPE-001.md) |
| SEM-001 | `docs/reference/variables_and_types.md` §"Literals" | Rewrite entirely. New grammar: plain decimal (`123`, `1_000_000`), hex (`0xFF`, `0x1_000`), binary (`0b1010`), octal (`0o17`) — all four case-insensitive on the prefix letter, `_` a separator anywhere between two digits, never leading/trailing. `0755` is decimal `755`, NOT octal (document this explicitly — it is the one place a reader coming from C will guess wrong). Float literals: dotted (`1.5`, mandatory digit on both sides of `.`) or dotless-exponent (`1e6`) — both forms optionally take an exponent (`1.5e-3`, `1E+10`); a bare trailing dot (`5.`) is NOT a valid float and is not even lexed as part of the number. Document the four new lexical errors by name (E070–E073, one worked example each — `docs/errors/codes/E070.txt` through `E073.txt` already have full prose to draw from) and the representability note: an integer literal's value is now held exactly regardless of magnitude, but an out-of-range value is not YET a compile error (SEM-002 territory) — say so plainly rather than let a reader assume the reference already describes SEM-002's behavior. | [SEM-001](../changes/SEM-001.md) |
| SEM-001 | Handbook ch03 (lexical analysis) | The chapter's own description of number-token scanning is now stale (radix forms, `_` separators, and the dotless-exponent form did not exist when it was written). Rewrite the numeric-literal section to match the new reference material once ch03 resumes; do not touch anything else in the chapter. | [SEM-001](../changes/SEM-001.md) |
| SEM-001 | Handbook ch12 (`bool`/`char`/numeric coercion pitfalls) | No prose change owed — ch12's own numeric-coercion discussion (INV-NUM-1, `bool` isolation) is untouched by this group. Only its CODE EXAMPLES using integer literals should be spot-checked once ch12 resumes, in case any relied on the old silent-clamp behavior for an out-of-range literal (none currently do, per this group's own `lib/std`/`showcase` sweep, but ch12 itself was not swept as part of this session since the handbook is paused). | [SEM-001](../changes/SEM-001.md) |

## Why this file is not a changelog

A changelog says what changed. This says **what someone else must now go and
rewrite**, keyed by where they will be sitting when they do it.
