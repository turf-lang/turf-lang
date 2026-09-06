# Introduction

## Overview

Diagnostic messages produced by compilers and interpreters have been studied
for more than half a century, and the consensus is that they remain difficult
to act on, especially for less experienced programmers [1]. The natural
response — rewriting messages to be longer, friendlier, and example-rich — has
been evaluated repeatedly and has largely failed to move the needle: a
controlled study of enhanced syntax-error messages found no measurable learning
or debugging benefit over stock compiler output [2]. Large language models
(LLMs) reopened the question, because a capable model can, in principle,
explain a diagnostic in context and propose a concrete fix. Yet when
LLM-enhanced programming error messages are evaluated with real users in
realistic conditions rather than by experts reading transcripts, the advantage
mostly evaporates: GPT-4-generated messages beat conventional compiler messages
on only one of six debugging tasks, with hand-written explanations still
winning overall [3]. Follow-up work has established that *how* the model is
prompted matters more than the fact that a model is used at all: supplying the
surrounding code context measurably improves the specificity and quality of
LLM-produced fixes [9], and prompting strategy strongly affects explanation
usefulness [10]. Two structural problems remain. First, calling a frontier
model on every error of every compile imposes per-invocation monetary cost and
multi-second latency that no interactive compiler can absorb [4]. Second, an
LLM suggestion shown to the user without being checked is a liability: a wrong
patch or a fluent-but-false explanation is presented with the same authority as
a correct one, and LLM-generated code is known to contain a substantial rate of
compiler- and runtime-level errors [5], with recurring, catalogued
non-syntactic failure modes [11].

A separate line of work — model cascades — addresses the cost problem in
generic settings. FrugalGPT routes each query to the cheapest model that can
answer it and escalates only when a scoring function judges the cheap answer
inadequate, matching a strong single model's accuracy at a fraction of the cost
[4]. GATEKEEPER and related work refine the escalation decision itself,
calibrating a small model so that its confidence reflects when it should defer
rather than its raw self-reported certainty [6]; a broader literature explores
probabilistic cost bounds, calibrated routing uncertainty, answer-consistency
as a difficulty signal, three-tier model→model→human escalation, "hint" rather
than full-answer delegation, speculative-decoding/cascade hybrids, and the role
of small models generally [14]–[20]. These systems target question-answering
and reasoning benchmarks and use a generic difficulty or agreement proxy as the
routing signal; none is specialised to compiler diagnostics, and none verifies
the small model's output against a ground-truth oracle before acting on it.
Surveys of LLMs in the compiler pipeline note the same gap, and agentic
harnesses that let LLMs fix real compiler bugs report that even frontier models
remain weak on compiler-specific tasks [12], [13] — a further reason to keep
the model strictly advisory and behind a verifier. Independently,
execution-feedback repair loops such as SELF-DEBUGGING show that letting a
model observe the concrete result of running its own code and try again
markedly improves code generation [7] — but again in a general coding context,
not wired into a compiler's own analysis passes.

## Brief description

Turflang is a from-scratch, statically-typed, AOT-compiled language with an
LLVM backend. Its compilation pipeline is entirely deterministic —
`Lexer → Parser → Resolver (two-pass) → TypeChecker → Codegen` — and is
described in CS terms (recursive-descent parsing, hoisting, visitor-pattern
type inference, control-flow-graph dataflow analysis) rather than
LLVM-specific ones [8]. What Turflang adds is a diagnostics layer that runs
*alongside* the pipeline and treats the compiler's own internal state, not the
formatted text of an error string, as the source of truth for AI-assisted
suggestions.

Concretely, the layer is organised as three tiers. **Tier 0** is deterministic
and always on at zero cost: every raised diagnostic carries a ground-truth
error category derived from the C++ type of the class that raised it; a
trie-backed index answers "did you mean …?" identifier suggestions using
restricted Damerau–Levenshtein distance; and every coded error points at a
static, hand-written `turf --explain E0NN` write-up. **Tier 1**, enabled by the
opt-in `--smart` flag, consults a small language model
(Qwen2.5-Coder-1.5B, 4-bit quantised, roughly 1 GB of RAM) run locally and
in-process through `llama.cpp` with no network access. The model's prompt is
assembled from the diagnostic's structural context — a category-sized window of
surrounding source plus the exact declaration/candidate lines the compiler pass
already identified as relevant — and its output is constrained at the token
level by a GBNF grammar that makes malformed JSON unsampleable. Crucially, a
candidate fix is never trusted: it is spliced into a deep copy of the compiler's
source, AST, and symbol table and speculatively re-parsed, re-resolved, and
re-type-checked; only a fix that demonstrably removes the original diagnostic
without adding new errors clears the confidence gate. Confidence is *computed*
from response properties and then *finalised* by the empirical sandbox result,
never asked of the model. A failed sandbox attempt's real compiler diagnostics
are fed back into the next prompt, up to three attempts total, after which the
compiler's own unmodified message is shown. **Tier 2** — a cloud/frontier model
for the hardest conceptual errors — is not implemented; the architecture
deliberately leaves the escalation point for it.

## Contribution of the proposed work

1. **A tiered, compiler-internal diagnostic architecture.** We show how to
   organise compiler diagnostics as a cascade whose routing signal is not a
   generic difficulty proxy but the compiler's own ground-truth error category
   (carried on the raising exception class, read back by type), and whose
   verification signal is an actual recompilation of the candidate fix.

2. **A sandbox-validated escalation gate.** No model-authored fix reaches the
   user unless a deep-copied snapshot of compiler state re-parses,
   re-resolves, and re-type-checks the patched buffer cleanly *and* confirms
   the original diagnostic is gone. This converts "the model was confident"
   into "the compiler agrees," and makes a wrong patch a silent non-event
   rather than misleading user-facing output.

3. **Two-stage computed confidence instead of self-report.** A structural
   prior is accumulated from measurable signals (was the category resolved
   deterministically, is there a concrete single-line replacement, is it
   on-topic for the symbol involved, did the model echo the original line
   unchanged), then overridden by the sandbox outcome — proven-correct fixes
   are clamped high, unvalidated ones are capped below the display gate.

4. **A compiler-in-the-loop retry loop.** The sandbox's own rejection
   diagnostics are threaded back into the model prompt as explicit feedback,
   giving the retry the real reason the previous patch failed to compile
   rather than a bare "try again," bounded at three attempts.

5. **Grammar-constrained, KV-cache-reusing local inference in a real
   compiler.** The model runs in-process with a fixed system prompt whose
   tokens are cached across calls, greedy decoding with a repetition penalty
   tuned for small models, and a GBNF grammar that fixes the response schema
   during sampling rather than validating it afterward.

6. **Evidence that the host is a real AOT compiler, not a research toy.**
   Benchmarking against semantically identical C compiled with the same LLVM
   version places Turflang consistently ahead of `clang -O0` and within
   roughly ±20 % of `clang -O1` on most workloads, so the diagnostic
   contributions sit on top of a genuine native-code toolchain.
