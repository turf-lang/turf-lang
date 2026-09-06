# Abstract

**General description.** Turflang is a statically-typed, ahead-of-time (AOT)
programming language that compiles to native machine code through an LLVM
backend, with a conventional deterministic front end (lexer, parser, two-pass
resolver, type checker, code generator) augmented by an integrated,
locally-run language-model diagnostic layer.

**Existing problem.** Compiler error messages have been a documented barrier to
programmer productivity for decades, and controlled studies show that naive
message enhancement — including flat, ungated augmentation with large language
models — yields little or no measurable benefit while adding cost, latency, and
the risk of confidently-wrong advice.

**Proposed solution.** Turflang treats diagnostics as a tiered decision
problem: resolve deterministically wherever the compiler already knows the
answer, consult a small on-device model only when it does not, and never
surface a model-authored fix that has not been re-compiled. A candidate patch
is applied to a deep-copied snapshot of compiler state and fully
re-parsed, re-resolved, and re-type-checked; only a patch that provably
eliminates the original diagnostic without introducing new errors passes a
confidence gate and reaches the user. Confidence is computed from measurable
properties of the response and finalized by the sandbox outcome rather than by
model self-report, and a bounded retry loop feeds the sandbox's real compiler
diagnostics back into the prompt so a failed attempt course-corrects instead of
re-guessing.

**Comparison.** Prior cascade-routing work (e.g. FrugalGPT, GATEKEEPER)
reduces inference cost for generic question-answering and reasoning by
deferring from a small model to a large one on a difficulty proxy. Turflang
applies the same confidence-gated escalation idea to compiler diagnostics,
using compiler-internal signals — a ground-truth error category carried by the
raising diagnostic class and the pass/fail result of an actual sandbox
recompilation — as both the routing and the verification signal. The
underlying compiler is a genuine AOT native compiler: across six benchmark
programs spanning recursion, scalar loops, 1-D and 2-D arrays, structs, and
branchy access, Turflang-compiled binaries beat unoptimized C (`clang -O0`) by
18–68 % and land within roughly ±20 % of `clang -O1` on four of the six, using
the same LLVM version for both toolchains.
