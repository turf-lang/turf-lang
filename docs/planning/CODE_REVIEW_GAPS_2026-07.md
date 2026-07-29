# Code Review Gaps — 2026-07 Session

Originally compiled during a full file-by-file code review pass (Lexer →
Parser → Resolver → TypeChecker → Codegen) plus a targeted enum/`compare`-
`matches` probing pass. Re-verified against the current codebase during a
later cleanup pass: every numbered gap this file originally tracked (#0,
#0b, #1b, #2, #3, #4, #5, #6, #7, #8, #9, #10, #11, #12, #13, #14, #15, plus
"OTHER BUGS FOUND ALONG THE WAY" A and B) was confirmed fixed and has a
regression test — see git history (`git log --oneline -- src/`) for the
individual fixes and `tests/*/` for coverage. Removed from this file rather
than kept as a growing "done" list.

Two items don't fit that pattern — a genuinely still-open architectural gap,
and a deliberately accepted, documented limitation. Both re-verified live
against the current compiler; kept below.

---

## Open: `BlockExprAST::codegen()` never checks for a null statement result

**Where:** `src/Codegen.cpp`, `BlockExprAST::codegen()`.

```cpp
Value *LastVal = nullptr;
for (auto &Expr : Expressions) {
    LastVal = Expr->codegen();
}
```

No check for `Expr->codegen()` returning `nullptr` — a failing statement
(any codegen path that returns null without raising its own diagnostic
first) is silently dropped from the compiled program instead of stopping
compilation. This was the root cause behind several now-fixed bugs in this
file's original history (a failing statement's absence looked like "nothing
happened" rather than a compile error) — those specific call sites are
fixed, but this general propagation gap itself remains.

**Why still unfixed:** tried hardening it (`if (!LastVal) { ExitScope();
return nullptr; }` immediately after the codegen call) — this broke the test
suite with link errors ("undefined reference to `turf_math_power_int_int`"
etc.), meaning the current silently-continue behavior is load-bearing,
almost certainly tied to the pre-pass/full-pass codegen architecture (a
function body can apparently be legitimately partially/unsuccessfully
pre-pass-codegen'd before its callees' prototypes exist yet, relying on a
later full pass to retry and succeed). Reverted the hardening change.

**To pick this up:** fully map `g_protoOnlyPass`'s call graph (`main.cpp`)
before touching `BlockExprAST::codegen()` again — need to distinguish "a
mid-block null return here is expected, the full pass will retry" from
"something broke silently and should stop compilation," which the pre-pass/
full-pass split currently conflates.

---

## Accepted limitation: exhaustive `compare` as a function's ENTIRE body still false-flags "missing return"

**Where:** `src/CFG.cpp` (definite-return analysis).

A function whose entire body is a single, provably-exhaustive `compare`
with no trailing statement still gets a false "missing return" diagnostic.
`compare` is deliberately never treated as a hard CFG terminator by itself
(this preserves the widely-used "exhaustive compare + trailing belt-and-
suspenders return" idiom throughout the test suite — an earlier, more
"precise" version of the CFG pass broke 7+ existing tests by treating
`compare` as terminating). The narrower case of a `compare` with literally
nothing after it inherits that same choice as a side effect.

**Workaround:** add a trailing `return` (or any statement) after the
`compare`, even though it's unreachable in the exhaustive case — matches
the idiom already used everywhere else.

Regression test documenting this (still present, not excluded from the
suite as a "real" bug — it's asserting the *current*, accepted behavior):
`tests/99_known_bugs/fail_compare_only_body_missing_return.tr`.

---

## Cross-references

- `docs/planning/generics_and_operators_roadmap.md` — the aspirational "real
  monomorphization" design; current generics are type-erasure-based (see
  `Resolver::substituteTypeParams()` in `src/Resolver.cpp` for how a bare
  generic type parameter is threaded through today). Still not implemented.
- Memory: `project_generics_namedtype_gap` (persistent Claude memory note)
  has the deep technical trace of how `NamedType` vs `TurfGenericParamType`
  interact across passes, useful background for anyone touching generics.
