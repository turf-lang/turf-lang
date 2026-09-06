# Turflang — Research Paper (working draft)

Working title: **Turflang: Sandbox-Validated, Tiered AI Diagnostics in an AOT Compilation Pipeline**

Authors: Karan Kumar (23BPS1034), Aayush Shukla (23BPS1081), Aarav Tandon (23BPS1134)
Project Guide: Dr. Indira B · Team 551

## Purpose of this folder

Each numbered file is one section of the paper, written to be concatenated in
order. Prose is final-draft quality. The Literature Survey was dropped (it is
optional for this format); its essential citations were folded into the
Introduction.

| File | Paper section | Status |
|---|---|---|
| `00_abstract.md` | Abstract | draft complete |
| `01_keywords.md` | Keywords | complete |
| `02_introduction.md` | Introduction (overview, brief description, contributions) — cited | draft complete |
| `03_methodology.md` | Methodology (architecture, grammar, equations, algorithms, figures) | draft complete |
| `04_results.md` | Results | **placeholder — re-run benchmarks on the MacBook and replace every table** |
| `05_conclusion.md` | Conclusion | draft complete |
| `06_references.md` | References | draft complete |
| `PAPER.md` | all sections concatenated (regenerate after edits) | generated |
| `figures.md` | Mermaid source for the 5 methodology figures (not part of PAPER.md) | supplement |

**Methodology now contains** (all grounded in the codebase): the tiered
architecture + 5 figures (ASCII inline, Mermaid in `figures.md`); a simplified
EBNF of the Turflang surface grammar (Listing 1) + operator-precedence table
(Table 1); the SLM-response GBNF grammar (Listing 2); numbered equations for
cyclomatic complexity, definite-assignment and liveness dataflow, compile-time
bounds checking, the "did you mean?" edit-distance recurrence + threshold, the
context-window function, the structural-prior score, confidence finalization,
and the display gate (Eq. 1–11); numbered pseudocode for two-pass resolution,
the trie edit-distance search, the fetch/validate/retry loop, the sandbox, and
the structural prior (Algorithm 1–5); and worked Turflang examples + an
end-to-end `--smart` trace (Listings 3–5).

Regenerate `PAPER.md` after editing any section:

```bash
cd research && { printf '# Turflang: Sandbox-Validated, Tiered AI Diagnostics in an AOT Compilation Pipeline\n\nKaran Kumar (23BPS1034) · Aayush Shukla (23BPS1081) · Aarav Tandon (23BPS1134)\nProject Guide: Dr. Indira B · Team 551\n\n*Assembled draft — edit the section files, not this one.*\n\n---\n\n'; for f in 0[0-6]_*.md; do sed 's/^# /## /' "$f"; printf '\n---\n\n'; done; } > PAPER.md
```

## Source-of-truth pointers (for fact-checking during revision)

- Pipeline & CFG analyses: `docs/architecture/compiler_pipeline.md`
- Error/`recover` system: `docs/architecture/errors_and_recover.md`
- CLI / diagnostics / `--explain`: `docs/reference/cli_and_diagnostics.md`
- Feature completeness: `docs/planning/ROADMAP_v1.0.0_COMPLETE.md`
- SLM pipeline: `src/SLMIntegration.cpp`, `include/SLMIntegration.h`,
  `src/SLMEngine.cpp`, `include/Sandbox.h`, `src/Sandbox.cpp`,
  `include/ErrorCategory.h`, `grammars/slm_suggestion.gbnf`
- Display gate: `src/Lexer.cpp` `DiagnosticEngine::add()` (~line 333)
- Deterministic "did you mean": `include/SuggestionIndex.h`
- Benchmark numbers: `Turf-Lang Compiler — Benchmark Report` (Aug 2026), reproduced in `04_results.md`

## Open items before submission

1. **Re-run the full benchmark suite on the MacBook** and replace every table
   in `04_results.md`; add a per-category chart.
2. Confirm final author order / institutional affiliation block.
3. Reference check — see below.

### What "re-verify references" means

`06_references.md` mixes two kinds of entry:

- **Confirmed** — checked against the ACM Digital Library / arXiv while
  drafting (title, authors, venue, ID all match): [1]–[4], [6], [7], [9]–[11],
  [16], [20], plus [22]/[23].
- **Not yet confirmed** — entries carried over from the earlier project
  presentation whose arXiv identifiers are dated 2026 (e.g. `arXiv:2601.02045`,
  `2603.20075`, `2608.00661`, `2605.18796`, `2601.22132`). These did not turn
  up in a search and may be placeholder/approximate IDs from the slide deck.
  They are marked `*(re-verify)*` in the file.

Before submitting, open each `*(re-verify)*` entry's arXiv page and confirm the
paper exists and that its **title, author list, year, venue, and identifier**
all match what the reference says — correcting or removing any that don't. A
wrong or invented citation in a submitted paper is a real problem, so this is a
hard gate, not a nicety. The confirmed entries only need a light second pass
for formatting consistency.
