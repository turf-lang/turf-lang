# Conclusion

Turflang shows that a compiler can offer AI-assisted diagnostics without
inheriting the two failure modes that have limited the idea in practice —
per-compile frontier-model cost and latency, and unverified suggestions shown
as fact. The mechanism is a tiered decision: resolve deterministically wherever
the compiler already knows the answer (Tier 0), consult a small on-device model
only for the first error of a pass when `--smart` is set (Tier 1), and — the
load-bearing part — never surface a model-authored fix that has not been
re-compiled. A candidate patch is spliced into a deep copy of the compiler's
source, AST, and symbol table and fully re-parsed, re-resolved, and
re-type-checked; only a patch that provably removes the original diagnostic
without introducing new errors clears a confidence gate. Confidence is computed
from measurable response properties and then overridden by the sandbox outcome,
so "the model was confident" is replaced by "the compiler agrees," and a wrong
patch becomes a silent non-event. A bounded retry loop feeds the sandbox's real
diagnostics back into the prompt so a failed attempt course-corrects on the
actual compile error rather than re-guessing.

The empirical picture matches the design intent. Across a fixed-seed sample of
99 compile-time diagnostic cases, the pipeline surfaced a sandbox-validated fix
in 45.5 % of cases, correctly withheld in the rest, surfaced **no** incorrect
fix, and ran with zero crashes and a flat ~1.5 GB memory footprint across 100
in-process LLM invocations. Validated-fix yield is highest exactly where the
compiler hands the model a precise ground-truth category and a related location,
and lowest on open-ended syntax and struct errors — the expected profile for a
1.5-B local model. Separately, benchmarking against semantically identical C
compiled with the same LLVM version places Turflang ahead of `clang -O0` by
18–68 % and within roughly ±20 % of `clang -O1` on most workloads, confirming
that the diagnostic layer sits on top of a real native-code AOT compiler.

Relative to the cascade-routing literature, the specialisation is to make the
routing signal the compiler's own error category and the acceptance signal an
actual recompilation, rather than a generic difficulty proxy and a model's
self-report. The clearest next step is Tier 2: a cloud/frontier model for the
hardest conceptual errors, slotted in after a Tier 1 low-confidence outcome and
reusing the same sandbox gate and two-stage confidence unchanged, so that even
a far more capable model still cannot show the user a fix the compiler has not
verified. Other directions include multi-token source spans for tighter
context, a calibration pass on the structural-prior weights against a larger
labelled corpus, and lifting the "first error per pass" restriction once
per-pass inference cost is amortised.
