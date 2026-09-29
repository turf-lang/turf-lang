# SESSION_PROTOCOL.md — how an overhaul implementation session runs

> **Authoritative.** `docs/planning/ARCHITECTURE_OVERHAUL.md` §22.0–§22.2 is
> the snapshot this was lifted from; this file wins where they disagree.
> It applies to any session implementing one of the feature groups in that
> document's §8 (`ARCH-001`, `TYPE-001`, `SEM-001`, …). Ordinary bug-fix and
> feature work that is not an overhaul group follows `AGENTS.md` alone.

Two scripts make this mechanical rather than aspirational:

```bash
scripts/session_preflight.sh  <FEATURE-ID>   # before touching anything
scripts/session_postflight.sh <FEATURE-ID>   # before declaring done
```

Neither replaces reading. `session_preflight.sh` prints the reading list and
proves the starting state is clean; `session_postflight.sh` mechanically
checks §22.2's list and exits non-zero on any miss.

---

## Step 1 — Read `AGENTS.md`, in full, first

Before anything else, and **before any repository exploration at all**. It is
the source of truth for build commands, the test runner's conventions,
commit-message format, and this repository's own engineering rules. Neither
this file nor `ARCHITECTURE_OVERHAUL.md` restates any of it, so a session that
skips `AGENTS.md` will get the workflow wrong.

Three things in it bite hardest and are worth re-reading even if you think
you know them:

- `scripts/test_runner.sh` **silently excludes** any filename containing
  `_module.tr` as a bare substring. A real test named that way is skipped
  with no failure and no output.
- The runner covers **only** `tests/`. `lib/std/**` and `showcase/**` are not
  exercised by it and can sit broken indefinitely with the suite green.
  `AGENTS.md` records a confirmed instance of exactly that.
- Compiling one of the six prelude modules directly
  (`turfc --resolve lib/std/core/char.tr`) is the **only** regression coverage
  for the prelude-self-import bug class.

## Step 2 — Read `docs/development/FREEZE.md`

Every decision in it is settled. If you believe one is wrong, **stop**: write
the argument into `docs/development/changes/<ID>.md` under *Unresolved
questions* and ask. Do not implement a different interpretation and explain it
afterwards.

## Step 3 — Read your feature group's section in `ARCHITECTURE_OVERHAUL.md` §8, and every section it names

Each group ends with a **"Must NOT reinterpret"** list. That list is the short
version of everything above. `ARCHITECTURE_OVERHAUL.md` §25 (*Things We Should
NOT Do*) is the repository-wide version of the same list — the entries there
are the changes that look tempting from inside a neighbouring feature group
and are wrong.

## Step 4 — Read the relevant `docs/architecture/*.md`

`AGENTS.md`'s own instruction: *"Read the relevant one before making
non-trivial changes to that subsystem — don't re-derive from scratch."*

| Subsystem | Document |
|---|---|
| Pass structure, CFG analysis | `docs/architecture/compiler_pipeline.md` |
| Storage, lifetime, layout | `docs/architecture/memory_model.md` |
| `::`, imports, module search | `docs/architecture/modules_and_scope_resolution.md` |
| `unsafe`, `&T`, raw pointers | `docs/architecture/pointers_and_references.md` |
| `error`, `!T`, `recover` | `docs/architecture/errors_and_recover.md` |

## Step 5 — Run `scripts/session_preflight.sh <FEATURE-ID>`

Confirms a clean tree, a successful build, and a green suite **before** you
change anything. A session must never mistake a pre-existing failure for one
it caused.

---

## While working

Follow the repository's stated conventions, which `AGENTS.md` owns: doc
comments explain **why** (a hidden invariant, a past bug this prevents) and
never restate the code; every fix gets a regression test in
`tests/<NN_topic>/` covering both directions where it makes sense; small
scoped commits, one logical change each; `type(scope): summary (@username)`
titles; **no `Co-Authored-By` or other AI-attribution trailer**.

And the rule this project holds hardest: **verify claims live.** If you are
about to write "X is enforced" in a doc comment or a change record, first
compile the specific input that would prove it wrong.

---

## Before declaring done — the completion checklist

`scripts/session_postflight.sh <FEATURE-ID>` checks all of these:

1. `./scripts/test_runner.sh` green.
2. `lib/std/**` and `showcase/**` compiled **directly** (the runner covers
   neither).
3. The `*_module.tr` naming sweep run in **both** directions, if any test file
   was added or renamed.
4. `docs/development/changes/<ID>.md` filled in — **not a stub**. The
   *Removed assumptions* and *Known pitfalls* sections in particular, since
   those are what a future reader cannot reconstruct from a diff.
5. `INDEX.md` row added; `handbook-sync/PENDING.md` entry per affected chapter
   or reference file; `INVARIANTS.md` updated with any new INV-*.
6. `scripts/check_doc_links.sh` passes.
7. The group's own **Completion criteria** from §8 are each individually
   demonstrated, not asserted.

Item 7 is the one no script can check for you. "Demonstrated" means a command
whose output you actually looked at — not a belief that the code you wrote
does what you meant.
