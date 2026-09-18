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

## Why this file is not a changelog

A changelog says what changed. This says **what someone else must now go and
rewrite**, keyed by where they will be sitting when they do it.
