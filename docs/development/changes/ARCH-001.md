# ARCH-001 — Development change-tracking and semantic freeze

| | |
|---|---|
| **Status** | landed |
| **Dates** | started 2026-09-17 · landed 2026-09-17 |
| **Commits** | *(fill in on commit — the record predates the sha by design)* |
| **Sessions** | 1 of 1 |
| **Depends on** | none — this is the first group |
| **Frozen decisions relied on** | F-TRACK-1 (every session updates a change record). No semantic decision is relied on or touched. |

## Why

Every later feature group produces architectural knowledge that would
otherwise live only in a commit message and a diff, and this repository has
already demonstrated exactly how that knowledge rots:

**43 source comments across 22 files cited `docs/planning/Errors.md` as the
design authority for the `error`/`!T`/`recover` feature. That file was deleted
on 2026-08-04** (commit `b689fa8`, "retire completed Phase 2/3/RFC docs") and
nothing noticed for over a year. Its replacement,
`docs/architecture/errors_and_recover.md`, was written and even says in its
own opening paragraph that it supersedes the deleted brief — but not one of
the 43 citations was updated. A reader following any of them found nothing.
Six other `docs/` paths were dangling the same way.

That is not a documentation-tidiness problem. A citation to a file that does
not exist reads as authoritative, so it sends a reader — human or agent —
looking for a specification that they then cannot find, and the usual
conclusion is that the rule it describes is not real.

The second, subtler failure this group addresses: the session protocol in
`ARCHITECTURE_OVERHAUL.md` §22 was written down, but nothing made it
*checkable*. A session could skip `FREEZE.md`, misattribute a pre-existing
test failure to its own change, or leave a stub change record, and the only
thing standing in the way was diligence.

## Language semantics

### Old behaviour
### New behaviour
### Is this breaking?

**None, none, and no.** This group changes no `.tr` syntax, no type rule, no
diagnostic, and no generated code. `src/` and `include/` were touched in
**comments only** — verified by rebuilding and re-running the full suite
(606/606, unchanged from the baseline).

## Compiler architecture

No pass changed. The only source edits are doc-comment citations:

- 43 `docs/planning/Errors.md` citations → `docs/architecture/errors_and_recover.md`
  across 22 files (Lexer/Parser/Resolver/TypeChecker/Codegen headers and
  bodies, `Types.h`, the AST headers, `ErrorsFlow.h`).
- 34 `docs/planning/COMPILER_MODULE_BREAKDOWN.md` citations →
  `docs/architecture/compiler_pipeline.md`. The plan doc for the 2026-08
  module split was retired once the split landed (`b689fa8`); the document
  that now describes the per-pass file layout is `compiler_pipeline.md`, and
  every split file's header doc comment points there.
- 12 bare `Errors.md` prose mentions → `errors_and_recover.md`, with the six
  that referred to "Errors.md's own note on why payload fields are deferred"
  re-pointed at the section that actually contains it,
  `errors_and_recover.md` §5.1.
- Five one-off citations, each handled on its own merits (see *Removed
  assumptions*).

Comment reflow: the new paths are up to 16 characters longer, so affected
comment blocks were re-wrapped to the file's 80-column norm rather than left
overhanging. Four box-drawing section headers had the citation moved to the
line below instead, so the rules keep their original width.

## Runtime changes

None.

## Standard-library changes

None. `lib/std/**` was not edited; it was compile-checked (see *Tests*).

## New invariants

None added to the *language*. `INVARIANTS.md` was created and seeded with the
twelve invariants `ARCHITECTURE_OVERHAUL.md` §19 derives from the handbook's
pitfall blocks and the audit — every one of them with an honest status, not an
aspirational one. Two were spot-checked live against this build rather than
transcribed:

- **INV-LAYOUT-1** (finite struct layout) does **not** hold. `struct A { me: A }`
  on its own compiles clean; adding `mutable x: A;` crashes **the compiler
  itself** with SIGSEGV (exit 139) — and does so already at `turf --resolve`,
  not in Codegen. `ARCHITECTURE_OVERHAUL.md` Appendix A finding 7 describes
  this as the compiled program segfaulting; it is the compiler. TYPE-001 owns
  the fix, and the Resolver is the right place for it.
- **INV-NUM-2** (no literal value lost) does **not** hold:
  `mutable x: int = 999999999999999999999999999999;` compiles with no
  diagnostic at all and prints `9223372036854775807`.

## Tests

No `.tr` tests added — this group adds no language behaviour, and
`ARCHITECTURE_OVERHAUL.md` §8 says so explicitly ("Not a `.tr` test — it does
not belong in `tests/`").

What was run and looked at:

| Check | Result |
|---|---|
| `./scripts/test_runner.sh` (baseline, before any edit) | 606/606 pass |
| `./scripts/test_runner.sh` (after) | 606/606 pass |
| `./scripts/cmake_build.sh --release` | succeeds, before and after |
| Every `lib/std/**/*.tr` via `turf --resolve` | resolves (two known exceptions below) |
| All six prelude modules compiled directly | no ambiguity diagnostic |
| All 7 `showcase/*.tr` compiled | exit 0 |
| `scripts/check_doc_links.sh` on a clean tree | passes |
| `scripts/check_doc_links.sh` with a deliberately broken link | fails, naming file, line and path |
| `scripts/session_preflight.sh ARCH-001` on a dirty tree | fails, listing the dirty files |
| `scripts/session_postflight.sh ARCH-001` against a stub record | fails on *Removed assumptions* / *Known pitfalls* |
| `session_postflight.sh` with a deliberately misnamed new test (`…_module.tr` **with** a `main()`) | fails, naming the file |
| `session_postflight.sh` with a deliberately misnamed new companion (no `main()`, not `fail_*`) | fails, naming the file |
| `check_doc_links.sh` with a **source** comment citing the allowlisted `Errors.md` | still fails — proving the allowlist is referrer-scoped, not a blanket exemption |
| `git diff -U0 src include`, every changed line | comment-only; the sole non-comment-looking hunk is `TOK_ERROR`'s trailing `///<` text, with the enum entry itself byte-identical |

The self-tests for the two new scripts are the ones the group's completion
criteria call for: a check nobody has ever seen fail is not a check.

## Known pitfalls

**1. `check_doc_links.sh`'s allowlist is referrer-scoped, and that is load-bearing.**
`scripts/doc_links_allowlist.txt` entries are `path :: referrer-glob :: why`.
The temptation when the check fails is to add the path alone. Don't: the whole
value of the check is that a *new* source comment citing a retired document
still fails, while the one document that narrates that retirement does not.
A bare path exemption silently restores the original bug.

**2. A stale allowlist entry fails the check, on purpose, and the fix is a deletion.**
When a session lands, say, `TYPE-001`, that feature's record starts existing
under `changes/` and its allowlist line becomes stale — CI then fails with
`STALE ALLOWLIST … now exists -- delete its line`. That is the intended
maintenance, not a bug in the checker. The same applies if a citation is
reworded away: an entry nobody uses any more is an exemption quietly widening
its own scope.

**3. Two `lib/std` modules genuinely cannot be compiled directly, and it is not their fault.**
`lib/std/collections/list.tr` and `lib/std/mem/mem.tr` fail a direct
`turf --resolve` with `E034 A struct named 'List'/'Pool' is already declared`.
Cause: the prelude module `strings` does `import list::List`, and the preludes
are auto-imported into *every* top-level file — so compiling `list.tr` as the
entry point imports its own `List` back over itself (and `list.tr` imports
`mem`, which is why `mem.tr` fails the same way). This is the same bug class
`AGENTS.md` documents for the six prelude modules, where it was fixed only for
a prelude module against *itself*. `session_postflight.sh` therefore checks
those two files for "E034 and nothing else" rather than skipping them, so a
*new* breakage in them still fails. **Do not "fix" this while doing something
else** — it is a module-system change and belongs to whoever owns that work.

**4. The postflight's stdlib sweep uses `--resolve`, not a full compile.**
Every stdlib module ends in `Error: No entry point found` because none has a
`main()`, so the exit code is 1 for all of them and is useless as a signal.
The script filters that one line out and fails on anything else. If you add a
check here, filter by diagnostic, never by exit code.

**5. `session_postflight.sh`'s "is the record a stub" test counts lines, and
line-counting is fooled by a sentence.** Two lines of prose under *Removed
assumptions* passes. It is a floor against an empty heading, not a judge of
whether you wrote anything worth reading — that part is still on you.

**6. The `_module.tr` sweep is scoped to what the session touched, and it has to be.**
`SESSION_PROTOCOL.md` item 3 says "if any test file was added or renamed",
and `session_postflight.sh` implements exactly that: a `.tr` file this session
added or renamed under `tests/` is a hard failure if misnamed; the whole-tree
sweep is printed for the record but does not fail the run. The reason is
concrete — the whole-tree sweep is **not clean today**, and a check that is red
on arrival is a check people learn to ignore (see *Removed assumptions* 8).

**7. The record for a feature is named by its ID and nothing else.**
`session_postflight.sh ARCH-001` looks for `docs/development/changes/ARCH-001.md`,
greps `INDEX.md` for `| ARCH-001 |` and `PENDING.md` for the bare ID. A record
filed under a prettier name is, to every tool here, a missing record.

## Removed assumptions

**1. "`docs/planning/Errors.md` is where the `error`/`!T`/`recover` design lives" — gone, and it was already false.**
Anything that learned the error-system design from a source comment was
following a dead link. The live document is
`docs/architecture/errors_and_recover.md`; its Part 1 is the language
semantics, Part 2 the pass-by-pass implementation with file:line references,
and §5.1 specifically is the "why no payload fields" note that six comments
were vaguely gesturing at.

**2. "`docs/planning/COMPILER_MODULE_BREAKDOWN.md` explains why the compiler is split into per-category files" — gone.**
That plan document was retired when the split it planned landed. The per-pass
file layout is now described in `docs/architecture/compiler_pipeline.md`
(§"Related docs" and each pass's own section), which is where all 34 file
headers now point. Its own reference to the retired plan is the one place the
retirement is recorded.

**3. "A `docs/…md` path in a comment can be trusted to exist" — no longer an assumption; it is now enforced.**
`scripts/check_doc_links.sh` runs in CI as its own job (needing neither LLVM
nor a build). Adding a citation to a file you have not created will fail the
build. Conversely: **removing a doc is now a breaking change to everything
that cites it**, and the checker tells you what those are before you push.

**4. "The `docs/planning/` docs describe the language as it is" — not for the two places ARCH-001 marked.**
`ROADMAP_v1.0.0_COMPLETE.md` §3's "Final Syntax (locked in)" block still shows
`Point { x: 10, y: 20 }`, which the compiler rejects outright. The snippet is
deliberately left as written — it is a record of what Phase 2 decided — and
issue #14 beneath it now carries the superseded note. Treat that whole section
as history, not specification. `docs/reference/structs_and_enums.md` is
authoritative for how structs are constructed.

**5. "`docs/patent/PATENT_BENCHMARK_DATA.md` is in this repository" — it never was.**
`src/SLMIntegration.cpp` cited it for the benchmark that motivated the
multi-line-replacement fix. `git log --all` has no record of that path ever
existing here; the comment now says the writeup is kept outside the repository
rather than pointing at a path in it.

**6. "Issue #192's status note explains the unsized-`T[]` `for`-each restriction" — that note did not survive.**
`PHASE_3_ISSUES.md` was retired into `ROADMAP_v1.0.0_COMPLETE.md`, which
absorbed the Phase 3 checklist but not #192's reasoning. The live explanation
is `ArrayType::getSize()`'s own doc comment, which is what
`TypeCheckerStmt.cpp` now points at.

**7. "A session can be trusted to have read `FREEZE.md` and started from a green tree."**
`scripts/session_preflight.sh <ID>` now prints the reading list (including the
line number of that feature's §8 section and the `docs/architecture/*.md`
files that section actually names, parsed out of the blueprint rather than
guessed) and verifies clean-tree/build/suite/doc-links before any edit. A
session that skips it and later hits a failure has no way to tell whose
failure it is.

**8. "Everything under `tests/` is covered by the suite" — false, by one file, since 2026-08.**
`tests/07_functions_modules/export_qualified_cross_module.tr` has a `main()`
and is a genuine standalone test, but its name contains the substring
`_module.tr`, so `scripts/test_runner.sh` excludes it: 0 results, no failure,
no output. It compiles and passes when run directly (`7`, `hi from a module`,
exit 0), so nothing is broken — it simply is not being checked, and has not
been since commit `847128c` introduced it. This is precisely the trap
`AGENTS.md` calls "the one trap everyone hits repeatedly on this project".
Renaming it is a `tests/` change with no connection to change-tracking, so
ARCH-001 does not make it: it is recorded here, and named in
`session_postflight.sh`'s own comment, so the next session that touches that
folder inherits the knowledge rather than rediscovering it.

## Handbook impact

| File / chapter | What is now wrong | Suggested replacement |
|---|---|---|
| — | Nothing. No language behaviour changed, so no chapter's claims moved. | — |

### Examples that became invalid
None.

### Examples that should be added
None.

### Sections that must NOT change
All of them, as far as this group is concerned. A handbook writer resuming
after ARCH-001 should find their chapters exactly as they left them; the only
thing that changed underneath is that the internal documents their chapters
cite now resolve.

## Unresolved questions

1. **Should `check_doc_links.sh` follow relative Markdown links too?** It
   currently checks `docs/…md` paths, per §18.5's wording. Relative links
   inside `docs/reference/` (`](./collections.md)`) are not checked. None were
   found dangling in a manual sweep, so this is a widening, not a fix —
   deliberately left for whoever wants it, with the cost being false positives
   on anchors and on relative paths that resolve differently per directory.
2. **`.github/workflows/test.yml` was edited without a local YAML parser
   available** (no `pyyaml`, no `ruby`, no `yq` in this environment). The new
   `doc_links` job mirrors the existing job's structure exactly and the script
   it runs was executed directly many times, but the workflow file itself is
   verified by eye, not by a parser. First CI run confirms it.

## Related records

None yet — this is the first. Every later record links back here for the
system it created.
