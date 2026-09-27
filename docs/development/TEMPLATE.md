# TEMPLATE.md — the change-record template

Copy this file to `docs/development/changes/<FEATURE-ID>.md` at the **start**
of a session, not the end — the *Removed assumptions* and *Known pitfalls*
sections are written best while the surprise is fresh.

A section with nothing to say gets the word `None`, not deletion: an empty
*Runtime changes* heading tells a future reader the question was asked. A
stub record — headings with nothing under them — fails
`scripts/session_postflight.sh`, deliberately: a stub is what produced the
`Errors.md` problem this whole system exists to prevent
(`docs/planning/ARCHITECTURE_OVERHAUL.md` §5 C-8).

Why a git log is not enough: a log answers *"what changed in this file"*. A
handbook writer, or the next session, needs *"what does the language mean
now, what did it mean before, which chapter is now wrong, and which examples
no longer compile."*

---

```markdown
# <FEATURE-ID> — <name>

| | |
|---|---|
| **Status** | planned / in-progress / landed / abandoned |
| **Dates** | started YYYY-MM-DD · landed YYYY-MM-DD |
| **Commits** | <sha> … |
| **Sessions** | 1 of 3 |
| **Depends on** | <FEATURE-ID>, … |
| **Frozen decisions relied on** | F-GEN-1, F-NUM-3, … |

## Why
The problem, in the user's terms. What was broken or missing, with a
reproduction if there was one.

## Language semantics
### Old behaviour
Exact, with an example that compiled and what it did.
### New behaviour
Exact, with the same example.
### Is this breaking?
yes/no · who it breaks · the mechanical migration

## Compiler architecture
Pass-by-pass. Only the passes that changed.
- **Lexer** ·  **Parser** ·  **AST** ·  **Types** ·  **Resolver** ·
  **Symbol tables** ·  **TypeChecker** ·  **Monomorphizer** ·  **Codegen** ·
  **CFG/analysis** ·  **Diagnostics** ·  **Optimization**

## Runtime changes
## Standard-library changes
## New invariants
Added to INVARIANTS.md, with what enforces each one.
## Removed assumptions
Things that used to be safe to assume and no longer are. **The most
valuable section for a future reader.**

## Tests
Added · changed · deleted, with paths and what each proves.

## Known pitfalls
Anything a future implementer could plausibly get wrong here. Written the way
the handbook's `pitfall` blocks are written — concrete, with the real failure.

## Handbook impact
| File / chapter | What is now wrong | Suggested replacement |
|---|---|---|
### Examples that became invalid
### Examples that should be added
### Sections that must NOT change
(Guarding against over-correction — e.g. a chapter's *reasoning* staying
correct while only its code listing changed.)

## Unresolved questions
## Related records
```
