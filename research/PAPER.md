# Turflang: Sandbox-Validated, Tiered AI Diagnostics in an AOT Compilation Pipeline

Karan Kumar (23BPS1034) · Aayush Shukla (23BPS1081) · Aarav Tandon (23BPS1134)
Project Guide: Dr. Indira B · Team 551

*Assembled draft — edit the section files, not this one.*

---

## Abstract

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

---

## Keywords

1. Compiler diagnostics
2. Small language models (on-device inference)
3. Model cascades / confidence-gated escalation
4. Sandbox validation (verified program repair)
5. Grammar-constrained decoding

---

## Introduction

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

---

## Methodology

This section is organised as follows. §1 states the design goal and the tiered
framing (**Figure 1**, **Figure 2**). §2 describes the deterministic host
pipeline the diagnostic layer runs beside, including the surface grammar
(**Listing 1**), the operator-precedence table (**Table 1**), the two-pass
name-resolution algorithm (**Algorithm 1**), and the control-flow-graph
dataflow analyses with their equations (**Eq. 1–4**). §3 describes the
diagnostic substrate both tiers depend on. §4 describes Tier 0, including the
"did you mean?" edit-distance machinery (**Eq. 5–6**, **Algorithm 2**). §5
describes the full Tier 1 `--smart` pipeline (**Figure 3**), its
category-sized context window (**Eq. 7**), the grammar that constrains the
model's output (**Listing 2**), the fetch/validate/retry loop
(**Algorithm 3**), the sandbox (**Algorithm 4**, **Figure 4**), and the
two-stage confidence computation (**Eq. 8–11**, **Algorithm 5**, **Figure 5**).
§6 describes the reserved Tier 2 escalation point. §7 gives worked examples
(**Listings 3–5**). §8 covers implementation and reproducibility.

> Mermaid source for every figure is in `research/figures.md` for rendering to
> a vector image.

---

## 1. Design goal and the tiered framing

Turflang's diagnostic layer is built on one principle: **do not pay for, and do
not trust, more than the situation requires.** Most diagnostics do not need a
model at all — the compiler already knows the exact category of the mistake and
often the exact symbol involved. Of those that could benefit from a
natural-language fix, none should be shown to the user on the strength of the
model's own say-so. The layer therefore has three tiers with strictly
increasing cost and strictly increasing scrutiny before anything reaches the
user.

| Tier | Trigger | Cost | What guarantees correctness |
|---|---|---|---|
| **0 — Deterministic** | every compile | none | the compiler's own analysis; hand-written docs |
| **1 — Local SLM** | `--smart`, first error per pass | ~1 GB RAM, seconds, on-device | sandbox recompilation of the candidate fix |
| **2 — Cloud/frontier** | *reserved, not implemented* | API cost + latency | (would reuse Tier 1's sandbox gate) |

**Figure 1. The compiler and its diagnostic layer.** The five compilation
passes are strictly deterministic; the diagnostic layer runs *beside* them,
reading structured diagnostics rather than formatted strings.

```
                         ┌─────────────────────── deterministic compilation ───────────────────────┐
   source.tr  ─▶  Lexer ─▶ Parser ─▶ Resolver(2-pass) ─▶ TypeChecker ─▶ Codegen ─▶ LLVM IR ─▶ binary
                    │        │           │                    │            │
                    │        │           │                    │            └─ CFG static-analysis battery
                    ▼        ▼           ▼                    ▼               (Eq. 1–4), every compile
                 ┌───────────────────────────────────────────────────────┐
                 │  DiagnosticEngine  — single choke point (Algorithm)   │
                 │  every TurfError / TurfWarning / LogErrorAt passes here │
                 └───────────────────────────────┬───────────────────────┘
                                                 ▼
                               ┌───────────────────────────────────┐
                               │   Tiered diagnostic dispatch       │  ◀── Figure 2
                               └───────────────────────────────────┘
```

**Figure 2. Tiered diagnostic dispatch (per raised diagnostic).**

```
 raised diagnostic d  (category c, location ℓ, related locations R, hint h)
        │
        ├─ already an error recorded on ℓ.line ?  ──yes──▶  drop (cascade suppression)
        │
        ▼
   TIER 0  (always):  category c from typeid(d);  "did you mean?" via Eq. 5–6 / Algorithm 2;
                      footer → `turf --explain E0NN`
        │
        ├─ `--smart` not set,  OR  d is a warning,  OR  an AI fix already fired this pass
        │        └────────────────────────────────▶  emit Tier-0 diagnostic (+ cascade note)
        ▼
   TIER 1  (--smart, first error of the pass):
        classify → size context (Eq. 7) → pin R → inject category doc
        → grammar-constrained SLM call (Listing 2)
        → structural prior (Eq. 8) → SANDBOX (Algorithm 4) ⇄ retry ≤3 (Algorithm 3)
        → finalize confidence (Eq. 9)
        │
        ├─ gate:  valid ∧ conf ≥ 0.8 ∧ fix_mode ≠ NONE   (Eq. 10)
        │        ├─ pass ─▶  replace message with [AI Suggestion] block
        │        └─ fail ─▶  emit the compiler's own unmodified diagnostic
        ▼
   TIER 2  (reserved, not implemented): cloud model, would re-enter the same sandbox gate
```

---

## 2. The deterministic compilation pipeline

Turflang is a five-pass, from-scratch AOT compiler; nothing about the
compilation itself is probabilistic.

```
source.tr ─▶ Lexer ─tokens▶ Parser ─AST▶ Resolver(2 passes) ─annotated AST▶
             TypeChecker ─type-annotated AST▶ Codegen ─▶ LLVM IR ─▶ native binary
```

Each pass consumes the concrete structure the previous one built:

| Pass | Input | Output |
|---|---|---|
| Lexer | source bytes | `Token` stream, each with a `SourceLocation` (start + end) |
| Parser | tokens | AST — ~42 concrete node kinds under one abstract base |
| Resolver | AST | same AST, mutated in place: a resolved identity on every name reference |
| TypeChecker | resolved AST | same AST + a side table `NodeTypes_ : ExprAST* → TurfTypePtr` |
| Codegen | type-annotated AST | LLVM IR → object file → linked binary |

**Codegen is only ever allowed to run on an AST that passed both the Resolver
and the TypeChecker cleanly** — the driver returns before constructing the
TypeChecker if resolution failed, and before any `codegen()` call if
type-checking failed. Codegen therefore trusts the annotations it reads without
re-validating them. The reason for splitting name registration from name use is
**forward references**: a name may be used before it is declared (mutually
recursive functions, a field whose type is declared later), so the Resolver
hoists every top-level declaration before checking any use (**Algorithm 1**).

### 2.1 Surface grammar (excerpt)

Turflang has several deliberately non-mainstream constructs — postfix `to`
casts, `mut`/`immut` in place of a bare type keyword, a `give … otherwise`
ternary, `!T` error-union types, and `recover` — so the grammar is given here
explicitly. **Listing 1** is a simplified EBNF excerpt (not the full grammar;
lexical rules and recovery productions omitted).

**Listing 1. Turflang surface grammar (simplified EBNF).**

```ebnf
program        = { top-level-decl } ;
top-level-decl = import-decl | func-def | struct-decl | enum-decl
               | error-decl | extern-decl | operator-decl | export-decl ;

import-decl    = "import" ident ";" ;
export-decl    = "export" "immut" ident ":" type "=" const-expr ";" ;
extern-decl    = "extern" [ "from" string-lit ] "fn" ident
                 "(" [ param { "," param } ] ")" ":" type ";" ;

func-def       = "fn" [ generic-params ] ident
                 "(" [ param { "," param } ] ")" ":" type block ;
generic-params = "<" ident { "," ident } ">" ;
param          = ident ":" type ;

struct-decl    = "struct" [ generic-params ] ident "{" [ field { "," field } ] "}" ;
field          = ident ":" type ;
enum-decl      = "enum" ident "{" ident { "," ident } "}" ;
error-decl     = "error" ident ( "{" ident { "," ident } "}" | ";" ) ;   (* bare form = one DEFAULT variant *)

type           = [ "!" ] core-type { "*" } [ "[" [ int-lit ] "]" ] ;     (* !T = error union *)
core-type      = "int" | "int32" | "double" | "bool" | "char" | "string" | "void"
               | ident [ "<" type { "," type } ">" ]                      (* generic instantiation *)
               | "fn" "(" [ type { "," type } ] ")" ":" type              (* first-class function type *)
               | ident "::" ident ;                                       (* module- / enum-qualified *)

block          = "{" { stmt } "}" ;
stmt           = var-decl | if-stmt | while-stmt | do-while-stmt | for-stmt
               | foreach-stmt | compare-stmt | return-stmt | "break" ";"
               | "continue" ";" | unsafe-block | expr ";" ;
var-decl       = ( "mut" | "mutable" | "immut" | "immutable" ) ident ":" type
                 [ "=" expr ] ";" ;
unsafe-block   = "unsafe" block ;
foreach-stmt   = "for" ( ident | "(" ident "," ident ")" ) "in" expr block ;
compare-stmt   = "compare" "(" expr ")" "{" { "matches" "(" expr ")" ":" { stmt } "done" }
                 [ "other" ":" { stmt } ] "}" ;

expr           = ternary ;
ternary        = bin-expr [ "give" expr "otherwise" expr ] ;
bin-expr       = unary { binop unary } ;            (* resolved by precedence climbing, Table 1 *)
unary          = { "!" | "-" | "&" } postfix ;
postfix        = primary { call-args | "[" expr "]" | "." ident | "::" ident
                          | "to" type                                    (* postfix cast, prec 12 *)
                          | "recover" recover-tail } ;
recover-tail   = "produce" expr | "run" block | "as" ident "run" block ;
call-args      = "(" [ expr { "," expr } ] ")" ;
primary        = int-lit | double-lit | string-lit | char-lit | "true" | "false"
               | ident | "(" expr ")" | struct-lit | array-lit ;
struct-lit     = ident "{" [ ident ":" expr { "," ident ":" expr } ] "}" ;
array-lit      = "[" [ expr { "," expr } ] "]" ;
```

### 2.2 Expression parsing — precedence climbing

Expressions are parsed by **precedence climbing** (a form of
operator-precedence parsing), driven by one integer-keyed table
(`InitializePrecedence()` in `src/ParserCore.cpp`), not by a chain of
per-level productions. The driver `ParseBinOpRHS()` parses one unary operand,
then repeatedly, while the next operator binds at least as tightly as the
current precedence floor, consumes it and recursively parses its right operand
with the floor raised by one — except for right-associative operators
(assignment, `**`), where the floor is **not** raised, so `a = b = c` groups as
`a = (b = c)` and `2 ** 3 ** 2` as `2 ** (3 ** 2)`.

**Table 1. Operator precedence (higher binds tighter).**

| Prec. | Operators | Assoc. |
|---:|---|---|
| 10 | `=` and all compound assignments (`+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `\|=`, `^=`, `<<=`, `>>=`, `**=`) | right |
| 12 | `to` (postfix cast) | left |
| 15 | `\|\|` | left |
| 20 | `&&` | left |
| 25 | `==` `!=` | left |
| 30 | `<` `>` `<=` `>=` | left |
| 33 | `\|` (bitwise or) | left |
| 35 | `^` (bitwise xor) | left |
| 37 | `&` (bitwise and) | left |
| 38 | `<<` `>>` | left |
| 40 | `+` `-` | left |
| 50 | `*` `/` `%` | left |
| 60 | `**` (exponentiation) | right |

`to` at precedence 12 (looser than every real binary operator, tighter than
assignment) is what makes `y = x to int` parse as `y = (x to int)` and
`1 + 2 to double` as `(1 + 2) to double`.

### 2.3 Two-pass name resolution

**Algorithm 1. `Resolver::resolveAll`** (`src/ResolverCore.cpp`). Pass 1
hoists; pass 2 resolves against the fully populated symbol table, so source
order never matters.

```
procedure ResolveAll(Nodes):
    # ── Pass 1: hoist top-level declarations (bodies untouched) ──
    for n in Nodes:
        if n is FuncDef:
            sig ← BuildFunctionSymbol(n)          # substitutes bare NamedType("T")
            RegisterSignature(sig)                 #   → TurfGenericParamType for generic params
        else if n is StructDecl or EnumDecl or ErrorDecl:
            RegisterType(n)
        else if n is Import(m):
            ResolveAll(TopLevelOf(m))              # recursive: hoist the imported module too
    # ── Pass 2: full resolution (walk everything, bodies included) ──
    for n in Nodes:
        Resolve(n)     # visitor walk; writes ResolvedSymbol / ResolvedCallTarget /
                       #   ResolvedKind directly onto each name-reference node
```

`BuildFunctionSymbol` performs **generic-parameter substitution** during
hoisting: `ParseType()` cannot know that `T` in `fn<T> id(x: T): T` is a type
parameter rather than a struct, so it emits a plain `NamedType("T")`;
`substituteTypeParams()` rewrites that to a real `TurfGenericParamType`,
recursing through `*`, `&`, `[]`, and generic-argument wrapping, before the
signature is stored. Every later pass then sees a real generic-parameter type
and treats it as "compatible with everything" during overload resolution.

### 2.4 Control-flow-graph static analyses

Per function, immediately before finishing that function's codegen, Turflang
builds a control-flow graph — a set of basic blocks `V` with edges `E`, one
entry block, and one **virtual exit block** every `return` path connects to —
and runs a fixed battery of analyses over it, **unconditionally on every
compile**, independent of any flag. Reachability is a BFS from the entry block.
The dataflow analyses are textbook iterative fixed-point computations.

**Eq. 1 — Cyclomatic complexity.** Counted over the CFG including the virtual
exit block and its incoming edges, for a single connected component
(`P = 1`):

```
M = |E| − |V| + 2P            with P = 1  ⇒  M = |E| − |V| + 2
```

`CyclomaticComplexityWarning` (non-fatal) fires when `M > 15`.

**Eq. 2 — Definite assignment** (forward dataflow; meet = intersection).
`Def(b)` is the set of body-local variables assigned in block `b`; parameters
are excluded (always pre-assigned).

```
OUT[b] = IN[b] ∪ Def(b)
IN[b]  =  ⋂  OUT[p]           over p ∈ pred(b);      IN[entry] = Params
```

Intersection at merge points is load-bearing: a variable is "definitely
assigned" only if assigned on **every** incoming path. After the fixed point,
a read of a body-local `v` in block `b` with `v ∉ IN[b]` raises
`DefiniteAssignmentError` (**E008**, fatal), with a related-location note at
the declaration.

**Eq. 3 — Liveness** (backward dataflow; meet = union). `Gen[b]` =
upward-exposed reads, `Kill[b]` = writes.

```
LiveOut[b] =  ⋃  LiveIn[s]        over s ∈ succ(b)
LiveIn[b]  = Gen[b] ∪ ( LiveOut[b] \ Kill[b] )
```

A write to `v` at a point where `v` is not in the live-after set is a
`DeadStoreWarning`; a body-local never in the separate `EverRead` set is an
`UnusedVariableWarning` (both non-fatal, parameters excluded).

**Eq. 4 — Compile-time array-bounds check.** A forward constant-propagation
pass tracks a partial map `val : Var ⇀ ℤ` (literals folded through `+ − × /`,
collapsing to ⊥ when predecessors disagree). For an access `a[e]` where `e`
constant-folds to `ĩ` and `a` has a known fixed size `s`:

```
0 ≤ ĩ < s     must hold, else ArrayBoundsError (E001) is raised at compile time
```

Other CFG checks in the same battery: unreachable-code / dead-branch /
missing-return (**E009**), statement-after-terminator (**E012** / **E013**),
unbreakable-loop detection via **Tarjan's strongly-connected-components
algorithm** (a closed SCC with no exit edge ⇒ `InfiniteLoopWarning`),
infinite-recursion detection via DFS from entry to the virtual exit (every path
crosses a self-call ⇒ `RecursionWithoutBaseError`, **E010**), use-after-free /
double-free via a points-to-lite forward dataflow (**E017** / **E018**), and
definite field assignment for constructors (same shape as Eq. 2, checked at the
exit block).

---

## 3. The diagnostic substrate

Both tiers depend on the compiler emitting *structured* diagnostics.

### 3.1 One choke point

Every diagnostic is a `TurfError` (fatal — records, then `longjmp`s to the
nearest recovery point or flushes and exits) or a `TurfWarning` (non-fatal —
records and returns). Both, plus the raw `LogErrorAt()` helper, funnel through
`DiagnosticEngine::add()` (`src/Lexer.cpp`), which is where cascade suppression
happens (once a primary error is recorded on a line, later diagnostics on that
line are dropped) and where the `--smart` pipeline is invoked for the first
error of a pass.

### 3.2 Ground-truth error category — the routing signal

Every `TurfError` subclass carries `SLM::ErrorCategory Category_`
(`include/ErrorCategory.h`), set from the C++ type of the class that raised it
and read back with `typeid(*this)`. The enum has 12 values: `TYPE_MISMATCH`,
`UNDECLARED_VAR`, `CONTROL_FLOW`, `SYNTAX_ERROR`, `ARITHMETIC`,
`STRING_CONVERSION`, `ARRAY_ERROR`, `DECLARATION`, `FUNCTION_SIGNATURE`,
`BUILTIN_FUNCTION`, `MEMORY_ERROR`, `GENERAL`. When `Category_` is non-`GENERAL`
it *is* the category — no text matching. Only a `GENERAL` diagnostic is passed
to a deterministic text-pattern classifier, `classifyError()`, which acts as a
strict refinement: it can turn a `GENERAL` into something specific but never
overrides a real answer.

### 3.3 Related locations — the context signal

A diagnostic that has something to point at attaches a `RelatedLocation`
`{SourceLocation, Description}` at the exact pass where that fact is already
known (declaration site of a mistyped variable, every candidate overload on an
arity mismatch, the array's declaration on an out-of-bounds access), via a
chainable `.withRelated(loc, "declared here")`. Nothing is recovered later by
regex over an error string. These lines are what Tier 1 pins into the prompt.

### 3.4 Error codes and `turf --explain`

Every registered `TurfError` subclass has a stable `E0NN` code (68 codes,
E001–E068), looked up via `typeid(*this)` at raise time. `turf --explain E0NN`
is a separate mode taking no `.tr` input; it resolves an explanation from disk
with a three-tier fallback — per-code file, construct-specific doc, per-category
doc — and prints class name, category, and body. A curated `Important` subset
(guaranteed stack overflow, use-after-free, invalid state-machine transition)
gets a stronger footer nudge.

---

## 4. Tier 0 — deterministic diagnostics

Tier 0 is always active and needs no flag. Besides ground-truth categorisation
(§3.2) and static explanations (§3.4), it provides **"did you mean …?"**
identifier suggestions (`include/SuggestionIndex.h`) for every unknown name —
variable, type, keyword, struct field, struct method, module symbol, enum
variant.

**Eq. 5 — Distance threshold.** For a query string `q`:

```
             ⎧ 1                        if |q| ≤ 4
   θ(q)  =   ⎨ 2                        if 5 ≤ |q| ≤ 7
             ⎩ max( 3, ⌊ |q| / 3 ⌋ )   otherwise
```

**Eq. 6 — Restricted Damerau–Levenshtein (optimal string alignment).** For
candidate `a` and query `b`, `d(i,j)` over `i ∈ [0,|a|]`, `j ∈ [0,|b|]`:

```
d(0,j) = j ,   d(i,0) = i

               ⎧ d(i−1, j) + 1                       (deletion)
d(i,j) = min ⎨ d(i, j−1) + 1                         (insertion)
               ⎨ d(i−1, j−1) + [ a_i ≠ b_j ]         (substitution)
               ⎩ d(i−2, j−2) + 1   if i,j ≥ 2 ∧ a_i = b_{j−1} ∧ a_{i−1} = b_j   (transposition)
```

The transposition case is what makes a single adjacent swap (`itn` → `int`)
cost 1 rather than 2. A match is reported when `a` is a complete candidate and
`d(|a|,|q|) ≤ θ(q)`.

**Algorithm 2. Trie search with the Eq. 6 recurrence.** Candidates are indexed
into a trie so the DP rows are shared across common prefixes; the transposition
term needs the row from **two** trie levels up plus the parent character.

```
function Suggest(q, Candidates, k):
    θ ← θ(q)                                              # Eq. 5
    T ← empty trie
    for w in Candidates with  | |w| − |q| | ≤ θ  and  w ≠ q:
        T.insert(w)
    R ← ∅
    row0 ← [0, 1, 2, …, |q|]
    for child in T.root.children:
        TrieSearch(child, child.ch, child.ch, '\0', row0, row0, θ, R)
    return first k of  sort(R by (distance asc, label asc))

function TrieSearch(node, prefix, ch, prevCh, prevRow, prevPrevRow, θ, R):
    cur[0] ← prevRow[0] + 1
    for j in 1 .. |q|:
        ins ← cur[j−1] + 1
        del ← prevRow[j] + 1
        sub ← prevRow[j−1] + (0 if q[j−1] = ch else 1)
        best ← min(ins, del, sub)
        if j ≥ 2 and prevCh ≠ '\0' and ch = q[j−2] and prevCh = q[j−1]:
            best ← min(best, prevPrevRow[j−2] + 1)        # transposition
        cur[j] ← best
    if node.terminal and cur[|q|] ≤ θ:
        R ← R ∪ { (prefix, cur[|q|]) }
    if min(cur) ≤ θ:                                       # prune: no descendant can do better
        for child in node.children:
            TrieSearch(child, prefix + child.ch, child.ch, ch, cur, prevRow, θ, R)
```

`static` is excluded from keyword-typo candidates because it sits at distance 2
from the very common identifier `state`.

---

## 5. Tier 1 — the `--smart` SLM pipeline

`--smart` runs an AI-assisted fix-suggestion pipeline on the **first** error of
a compilation pass. Later errors in the same pass get only a "may be a
cascading effect" note; warnings never invoke it; a clean compile loads no
model.

**Figure 3. The `--smart` Tier-1 pipeline.**

```
  error raised (first this pass)
        │
   (a)  ├─▶ deterministic category      c ← Category_  else classifyError(msg)
        │
   (b)  ├─▶ context assembly            window  w(c)   lines each side          (Eq. 7)
        │                               + pin every RelatedLocation outside the window
        │                               + inject docs/slm/<c>.txt
        │
   (c)  ├─▶ prompt build                ChatML: grounding/voice preamble ‖ category ‖ doc ‖ rules
        │                               user JSON: {error, hint, code[, previous_attempt_failed]}
        │
   (d)  ├─▶ local inference             Qwen2.5-Coder-1.5B Q4_K_M via llama.cpp, in-process
        │                               greedy + repetition penalty, GBNF-constrained (Listing 2)
        │                               ⇒ AISuggestion{explanation, fix_mode, replacement_line,
        │                                              target_line, end_line, warning, …}
        │
   (e)  ├─▶ structural prior            s ← StructuralPrior(·)                  (Eq. 8, Algorithm 5)
        │
   (f)  ├─▶ SANDBOX                     deep-copy state; splice patch; re-parse+resolve+typecheck+lint;
        │                               confirm original error gone            (Algorithm 4, Figure 4)
        │        │
        │        ├─ fail, attempts left ─▶ previous_attempt_failed ← sandbox diagnostics ──▶ back to (c)
        │        │                          (≤ 3 attempts total — Algorithm 3)
        │        └─ fail, no attempts ───▶ fix_mode ← NONE
        │
   (g)  ├─▶ finalize confidence         conf ← Finalize(s, sandbox_ok)         (Eq. 9)
        │
   (h)  └─▶ display gate                valid ∧ conf ≥ 0.8 ∧ fix_mode ≠ NONE    (Eq. 10)
                 ├─ pass ─▶ [AI Suggestion] block replaces the compiler message
                 └─ fail ─▶ the compiler's own unmodified diagnostic
```

### 5.1 Deterministic classification first (stage a)

The category is resolved before any model call: the raising class's non-`GENERAL`
`Category_` is used directly; otherwise `classifyError()` pattern-matches the
ANSI-stripped message and a keyword extracted from it (struct/enum/module
keywords checked before short control-flow keywords, to avoid substring traps
such as `"for enum"`). Whether the category was resolved deterministically is
recorded — it contributes `+0.4` to the structural prior (Eq. 8).

### 5.2 Context assembly (stage b)

**Eq. 7 — Category-sized context window.** `contextSizeForCategory()` returns
the number of source lines included on each side of the error line:

```
          ⎧ 3   for  c ∈ { SYNTAX_ERROR, BUILTIN_FUNCTION, ARITHMETIC,
          ⎪                 STRING_CONVERSION, CONTROL_FLOW }
  w(c) =  ⎨ 5   for  c ∈ { FUNCTION_SIGNATURE, DECLARATION, UNDECLARED_VAR, MEMORY_ERROR }
          ⎪ 8   for  c ∈ { TYPE_MISMATCH, ARRAY_ERROR, GENERAL }
          ⎩ 5   otherwise
```

On top of the `[ℓ − w(c), ℓ + w(c)]` window, every `RelatedLocation` on the
diagnostic that falls **outside** the window is appended as a labelled
"Relevant definition(s)" line with its real line number and its description,
marked with a distinct prefix so the model reads it as a deliberate anchor.
`getCategoryDocPath()` then maps `c` (and any construct keyword) to a short
reference file under `docs/slm/` (e.g. `type_mismatch.txt`, `structs.txt`)
injected into the system prompt, so the model reasons from Turflang's actual
rules rather than its C/Python/Java bias.

### 5.3 Prompt construction (stage c)

ChatML-framed. The **system prompt** is: a voice/grounding preamble (match the
compiler's warm second-person diagnostic voice; derive *all* Turflang syntax
knowledge from the injected doc and code context; do not "correct" unfamiliar
constructs; say so in `explanation` when unsure, because `replacement_line` is
sandbox-checked but `explanation` is not) ‖ the resolved category name ‖ the
injected doc ‖ a fixed rule list. The **user prompt** is a JSON object
`{error, hint, code}` plus `previous_attempt_failed` on retries, followed by
the exact response schema. `confidence` is deliberately **not** a field the
model produces — small models anchor to nearby example values and echo them.

### 5.4 Local inference engine (stage d)

`src/SLMEngine.cpp` links `llama.cpp` [23] directly into the compiler binary
(it does **not** use Turflang's FFI):

- **Model** — `qwen2.5-coder-1.5b-instruct-q4_k_m.gguf` [22]: a 1.5-B code
  model, 4-bit K-quantised, ≈ 1 GB, resolved relative to the compiler
  executable, `mmap` load.
- **Context** — `n_ctx = 10240`, `n_batch = 4096` (a decode whose token count
  exceeds `n_batch` is a native abort; batches above ≈ 4096 fall off a fast
  kernel path). KV cache is `Q8_0` for K and V.
- **Prompt budget** — **Eq. 11**:

  ```
  |sys_tokens| + |user_tokens|  ≤  n_ctx − GEN_BUDGET ,   GEN_BUDGET = 512
  ```

  and each `llama_decode` batch is clamped to `n_batch` first; an over-budget
  prompt is clipped rather than aborting.
- **System-prompt KV reuse** — the tokenised system prompt's KV cache is
  retained; an identical subsequent system prompt reuses those tokens.
- **Sampling** — a repetition penalty `penalties(last=64, repeat=1.1,
  freq=0.0, present=0.0)` chained **before greedy decoding**. Temperature is
  intentionally not chained: it only reshapes a distribution and has no effect
  on `argmax`; the repetition penalty is what counters small models looping
  under deterministic sampling.
- **Grammar-constrained decoding** — when `grammars/slm_suggestion.gbnf`
  (**Listing 2**) loads, a grammar-masking sampler is chained **first**, so the
  response schema is enforced *during* sampling: a malformed response is not
  merely unlikely, it is not sampleable. Missing/unparseable grammar degrades
  gracefully to unconstrained output.

**Listing 2. GBNF grammar constraining the SLM response**
(`grammars/slm_suggestion.gbnf`).

```gbnf
root ::= "{" ws (
         "\"explanation\":"       ws string   "," ws
         "\"fix_mode\":"          ws fix-mode "," ws
         "\"suggested_comment\":" ws string   "," ws
         "\"code_patch\":"        ws string   "," ws
         "\"replacement_line\":"  ws string   "," ws
         "\"target_line\":"       ws integer  "," ws
         "\"end_line\":"          ws integer  "," ws
         "\"warning\":"           ws string   "," ws
         "\"category\":"          ws category
         ) ws "}"

fix-mode ::= "\"MODIFY\"" | "\"NONE\""
integer  ::= "0" | [1-9] [0-9]*
category ::= ( "\"TYPE_MISMATCH\""      | "\"UNDECLARED_VARIABLE\""
            | "\"CONTROL_FLOW\""        | "\"SYNTAX_ERROR\""
            | "\"ARITHMETIC\""          | "\"STRING_CONVERSION\""
            | "\"ARRAY_ERROR\""         | "\"DECLARATION\""
            | "\"FUNCTION_SIGNATURE\""  | "\"BUILTIN_FUNCTION\""
            | "\"MEMORY_ERROR\""        | "\"GENERAL\"" )

string      ::= "\"" string-char* "\""
string-char ::= [^"\\\x00-\x1f] | "\\" escape
escape      ::= ["\\/bfnrt] | "u" [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F]
ws          ::= [ \t\n\r]*
```

The parsed response fills an `AISuggestion`: `explanation`, `fix_mode`,
`suggested_comment`, `code_patch` (prose description), `replacement_line`
(exact Turflang source), `target_line` / `end_line` (1-based inclusive range
`replacement_line` replaces; `0`/`0` ⇒ the diagnostic's own line), `warning`.

### 5.5 The fetch / validate / retry loop (stages c–g)

**Algorithm 3. `validateAndApplySuggestion`** (`src/SLMIntegration.cpp`),
`kMaxSlmAttempts = 3`.

```
function ValidateAndApplySuggestion(err, ℓ, hint, R, c):
    prev ← ""                                             # empty on attempt 1
    for attempt in 1 .. 3:
        a ← FetchSuggestion(err, ℓ, hint, R, c, prev)      # build prompt, SLM call, parse JSON
        if not a.valid:            return a                # model failed/timed out — retry won't help
        if a.fix_mode = NONE:      return a                # nothing to validate
        (ok, diags) ← VerifySuggestion(a, ℓ)               # Algorithm 4
        a.confidence ← Finalize(a.confidence, ok)          # Eq. 9
        if ok:                     a.valid ← true;  return a
        if attempt = 3:            a.valid ← false; a.fix_mode ← NONE;  return a
        prev ← diags                                       # real compiler errors → next prompt
```

### 5.6 The sandbox (stage f)

**Algorithm 4. `VerifySuggestion` / `ApplyAndValidateFix`**
(`include/Sandbox.h`, `src/Sandbox.cpp`).

```
function VerifySuggestion(a, errorLine):
    S ← DeepCopy( sourceLines, ASTRoot, symbolTable )      # CompilerStateSnapshot; original kept for rollback
    lo ← (a.target_line > 0)  ?  a.target_line  :  errorLine
    hi ← (a.end_line ≥ lo)    ?  a.end_line     :  lo
    S.applyFix(lo, hi, a.replacement_line)                 # splice [lo,hi] out, insert (may be multi-line)
    S.validate()                                           # FULL re-parse + re-resolve + re-typecheck + lint,
                                                           #   capturing every diagnostic produced
    ok ← ( no error-level diagnostic in S.diagnostics )    # a residual benign warning is allowed
          ∧ S.isOriginalErrorResolved(errorLine)           # the specific original diagnostic is gone
    if ok:  S.commitToMain()   else:  S.reject()
    return ( ok, join(S.diagnostics, " | ") )
```

**Figure 4. Sandbox lifecycle.**

```
   main compiler state
        │  DeepCopy(source, AST, symbol table)     ── original snapshot retained ──┐
        ▼                                                                          │
   ┌─────────┐  applyFix(lo,hi,text)   ┌───────────┐  validate()   ┌────────────┐  │
   │ SNAPSHOT │ ────────────────────▶ │  PATCHED   │ ───────────▶ │ VALIDATED? │  │
   └─────────┘                        └───────────┘               └─────┬──────┘  │
                                                                        │         │
                                          ok ∧ original error gone ─────┤         │
                                                                        │         │
                                             ┌──────────────────────────┴───┐     │
                                             ▼                              ▼     │
                                      commitToMain()                    reject() ─┘
                                     (state advances)             (discard; original stands)
```

### 5.7 Two-stage confidence (stages e, g)

**Eq. 8 — Structural prior** (pre-sandbox plausibility). With
`det` = "category resolved deterministically and ≠ GENERAL",
`p` = `replacement_line`, `kw` = the error's construct keyword,
`orig` = the original text of the line range `p` would replace:

```
                              ⎧ 0                              if p ≠ "" ∧ trim(p) = trim(orig)   (echo-back: hard reject)
   s(a) =  min( 1 ,  Σ ) ,   Σ =
             0.4 · [det]  +  0.2 · [fix_mode = MODIFY]
           + ( if p ≠ "" :  0.2  +  0.1 · [ p has no newline ∧ p ∉ {"{","}"} ]
                                 +  0.1 · [ kw ≠ "" ∧ kw occurs in p on a word boundary ] )
```

**Algorithm 5. `computeStructuralPrior`** (the procedural form of Eq. 8).

```
function StructuralPrior(a, c, det, kw, errorLine):
    s ← 0
    if det and c ≠ GENERAL:            s ← s + 0.4
    if a.fix_mode = "MODIFY":          s ← s + 0.2
    p ← a.replacement_line
    if p ≠ "":
        orig ← source[ compareRange(a, errorLine) ]        # target_line..end_line, else errorLine
        if trim(p) = trim(orig):       return 0.0          # model echoed the input — cannot clear the gate
        s ← s + 0.2
        if '\n' ∉ p and p ∉ {"{", "}"}: s ← s + 0.1
        if kw ≠ "" and WordBoundaryContains(p, kw): s ← s + 0.1
    return min(s, 1.0)
```

**Eq. 9 — Confidence finalization** (after the sandbox result is known):

```
                    ⎧ max( 0.9 , s )    if sandbox validated the fix   (proven correct — a low prior can't veto it)
   conf(s, ok) =   ⎨
                    ⎩ min( s , 0.5 )    otherwise                       (unvalidated — can never auto-apply)
```

**Eq. 10 — Display gate** (`DiagnosticEngine::add`):

```
   show_AI( a )  ⇔  a.valid  ∧  a.confidence ≥ 0.8  ∧  a.fix_mode ≠ "NONE"
```

**Corollary.** Composing Eq. 9 into Eq. 10: a validated fix has
`conf ≥ 0.9 ≥ 0.8` and an unvalidated one has `conf ≤ 0.5 < 0.8`, so the gate
reduces to

```
   show_AI( a )  ⇔  a.valid  ∧  sandbox_validated( a )  ∧  a.fix_mode ≠ "NONE"
```

— i.e. the structural prior only orders and logs; it can neither promote an
unvalidated guess nor block a sandbox-proven fix.

**Figure 5. The confidence transform.** A candidate's score is moved onto one
of two disjoint intervals by the sandbox result; the 0.8 gate sits in the gap
between them.

```
   structural prior  s ∈ [0, 1]
        │
        ├── sandbox FAIL / NONE ──▶  conf = min(s, 0.5)   ──┐   [0.0 ─────────── 0.5]      never shown
        │                                                    │                  ╎
        │                                             gate → ╎ 0.8              ╎
        │                                                    │                  ╎
        └── sandbox PASS ─────────▶  conf = max(0.9, s)   ──┘        [0.9 ── 1.0]          shown as [AI Suggestion]
```

### 5.8 Rendering (stage h)

On a pass, the compiler's message is replaced with an `[AI Suggestion]` block
— `suggested_comment`, then `Explanation:` (if any), then `Suggested fix:` (if
`fix_mode = MODIFY` and `code_patch` non-empty), then `Note:` (if `warning`
non-empty) — and `g_slmFiredThisPass` is set so no further model call happens
this pass.

---

## 6. Tier 2 — the reserved escalation point

A cloud/frontier model for the hardest *conceptual* errors is not implemented.
The retry loop (Algorithm 3), the sandbox gate (Algorithm 4), and the
confidence transform (Eq. 9–10) are model-agnostic, so a Tier 2 call would sit
after a Tier 1 `NONE`/low-confidence outcome and reuse them unchanged: even a
far more capable model still could not show the user a fix the compiler had not
recompiled. Leaving it unbuilt is a scope decision, not an architectural gap.

---

## 7. Worked examples

**Listing 3. A complete Turflang program** (the `fib` benchmark of §Results —
recursive, explicit return types, `mut` locals, module import).

```turf
// Recursive Fibonacci. Function-call-overhead heavy, no arrays.
import io;

fn fib(n: int): int {
    if n == 0 { return 0; }
    if n == 1 { return 1; }
    return fib(n - 1) + fib(n - 2);
}

fn main(): int {
    mut result: int = fib(38);
    io::printline(result);
    return 0;
}
```

**Listing 4. `error` types, the `!T` error union, and `recover as err run`**
(`tests/25_errors/07_recover_as_err.tr`). `!int` is an open union — the
signature names no specific error type; `recover` is the only way to consume a
`!T` at a call site; a `return` inside a `recover … run` block returns from the
*enclosing* function.

```turf
import test;

error NetworkError { Timeout, ConnectionRefused }

fn connect(mode: int): !int {
    if mode == 1 { return NetworkError::Timeout; }
    if mode == 2 { return NetworkError::ConnectionRefused; }
    return 200;
}

fn tryTimeout(): int {
    return connect(1) recover as err run {
        if err == NetworkError::Timeout { return 1; }   // variant identity compare (lowers to strcmp)
        return 0;
    };
}

fn main(): int {
    test::assert_eq(tryTimeout(), 1, "err identified as Timeout");
    return 0;
}
```

**Listing 5. End-to-end `--smart` trace** (illustrative; a type-mismatch
error). Stages keyed to Figure 3.

```
$ turf snippet.tr --smart -o snippet

(a) raising class → Category_ = TYPE_MISMATCH            [deterministic ⇒ +0.4 to Eq. 8]
(b) w(TYPE_MISMATCH) = 8 lines each side; RelatedLocation "declared here (line 3)"
    is inside the window; inject docs/slm/type_mismatch.txt
(c) ChatML prompt built  (system KV cache reused from a prior call)
(d) GBNF-constrained JSON:
      { "fix_mode": "MODIFY", "replacement_line": "mut total: double = subtotal + tax;",
        "target_line": 0, "end_line": 0, "category": "TYPE_MISMATCH", ... }
(e) structural prior:  0.4 (det) + 0.2 (MODIFY) + 0.2 (non-empty) + 0.1 (clean line)
                     + 0.0 (no construct keyword)                      = 0.9
(f) SANDBOX: splice line 7; re-parse + re-resolve + re-typecheck + lint → 0 errors;
    isOriginalErrorResolved(7) → true                                   ⇒ ok
(g) finalize:  conf = max(0.9, 0.9) = 0.9
(h) gate:  valid ∧ 0.9 ≥ 0.8 ∧ fix_mode ≠ NONE  → show [AI Suggestion]
```

If the sandbox in (f) had failed, its joined diagnostics would become
`previous_attempt_failed` and stages (c)–(g) would repeat (≤ 3 total); after a
third failure `fix_mode` becomes `NONE`, `conf ≤ 0.5`, and the compiler's own
type-mismatch message is printed unchanged.

---

## 8. Implementation and reproducibility

- **Compiler** — C++20, CMake, LLVM 22; `llama.cpp` is a vendored submodule
  compiled into the `turf` binary. ~50 k lines of C++ across `src/`/`include/`.
  Standard library (~23 modules) written in Turflang itself via `extern fn`
  FFI.
- **Regression suite** — 646 `.tr` test files across 34 numbered topic
  folders; a diagnostic-repair corpus of 241 compile-time diagnostic test
  files spanning 19 error categories is used for the Tier 1 evaluation.
- **SLM evaluation harness** — the compiled binary is invoked exactly as an
  end user would (`turf <file>.tr --smart -o <out>`); acceptance is scored by
  whether the display gate (Eq. 10) fires, which by construction requires a
  passed sandbox recompilation. Traces: `TURF_SLM_DEBUG=1`,
  `TURF_SANDBOX_DEBUG=1`.
- **Determinism** — greedy decoding (§5.4) plus the fixed grammar
  (**Listing 2**) make the model's output reproducible for a given prompt and
  model file; the only nondeterminism in the pipeline is wall-clock timing.

---

## Results

> **Status: placeholder — to be regenerated.** The entire benchmark suite
> (Part A and Part B) will be re-run on the project MacBook and every table in
> this section replaced with the fresh numbers, plus full per-run statistics
> and a per-category chart. The tables below are the digest from the previous
> run (*Turf-Lang Compiler — Benchmark Report* [21], August 2026), kept here only so
> the section structure, metrics, and prose are ready to drop the new figures
> into. Do not cite these numbers as final.

## Test environment

| Parameter | Value |
|---|---|
| Dates measured | 2026-08-19 – 2026-08-21 |
| Hardware | Apple M4 |
| OS | macOS 26.5.2 (Darwin 25.5.0, arm64) |
| Compiler backend | LLVM / Clang 22.1.5 (Homebrew) |
| Small language model | `qwen2.5-coder-1.5b-instruct-q4_k_m.gguf`, run locally via the bundled `llama.cpp` backend, no network calls |

Part B uses the **same** LLVM 22.1.5 backend for both the Turflang compiler and
the C reference compiler, isolating front-end / codegen differences from
LLVM-version differences.

---

## Part A — SLM-assisted diagnostic repair pipeline (`--smart`)

**Sample.** N = 99 cases, fixed-seed random sample (`random.seed(42)`) from a
regression corpus of 241 compile-time diagnostic test files spanning 19 error
categories. One further sampled file is a runtime-only assertion test (compiles
successfully, binary aborts on execution) and is outside `--smart`'s
compile-time scope; it is excluded from the N = 99 figures.

**System under test.** The compiled binary, invoked as an end user would:
`turf <file>.tr --smart -o <out>`.

**Acceptance criterion.** A case is scored as *validated fix surfaced* iff the
confidence gate fires (`confidence ≥ 0.8 ∧ fix_mode ≠ NONE`). This gate
requires the candidate patch to have passed sandboxed re-validation — a full
re-parse, re-resolve, re-type-check of the patched buffer plus a structural
lint pass — with up to three sandbox-retry attempts per case before a fix is
withheld. Re-validation fails only on error-level diagnostics; a residual
benign warning is still accepted.

**Measurement.** Wall-clock time and peak resident set size (RSS) via
`/usr/bin/time -l` around each invocation, 30-second cooldown between cases.

### Headline results (N = 99)

| Metric | Value |
|---|---|
| Validated fix surfaced (sandbox-passed, confidence ≥ 0.8) | **45 / 99 (45.5 %)** |
| Correctly withheld (`NONE`, or failed re-validation) | 54 / 99 (54.5 %) |
| Process crashes | **0 / 100** |
| Peak RSS — mean | 1495 MB |
| Peak RSS — range | 1487–1515 MB |
| Mean wall time, error → verdict | 13.8 s |
| Median wall time, error → verdict | 13.1 s |
| Wall time range | 5.5 s – 36.9 s |
| Mean wall time — accepted cases only (n = 45) | 10.0 s |
| Mean wall time — rejected cases only (n = 54) | 16.9 s |

Peak RSS excludes the single runtime-only test, which never invokes the SLM and
measures 55 MB. Rejected cases consistently take longer than accepted ones,
consistent with the gate exhausting the full three-attempt sandbox-retry loop
before declining rather than failing fast.

### Accuracy by error category (N = 99)

| Category | Validated / Total | Rate |
|---|---|---|
| 10_default_init | 1/1 | 100 % |
| 02_resolver | 3/3 | 100 % |
| 15_enums | 4/5 | 80 % |
| 05_arrays_pointers_ffi | 5/7 | 71 % |
| 14_cfg_analysis | 2/3 | 67 % |
| 04_control_flow | 5/10 | 50 % |
| 17_char_type | 1/2 | 50 % |
| 24_first_class_functions | 1/2 | 50 % |
| 26_state_machines | 1/2 | 50 % |
| 03_typechecker | 6/13 | 46 % |
| 07_functions_modules | 7/16 | 44 % |
| 11_bitwise_ops | 3/7 | 43 % |
| 06_structs | 3/11 | 27 % |
| 25_errors | 2/8 | 25 % |
| 01_syntax | 1/5 | 20 % |
| 13_string_stdlib | 0/1 | 0 % |
| 16_sys_and_test_stdlib | 0/1 | 0 % |
| 18_operator_overload | 0/1 | 0 % |
| 19_int32_type | 0/1 | 0 % |
| **Total** | **45/99** | **45.5 %** |

The four 0 % categories are single-case negative tests where the compiler is
designed to reject the code outright; trace inspection (`TURF_SLM_DEBUG=1`)
confirms the model correctly declines with `fix_mode: NONE` in each, rather
than proposing an incorrect patch — i.e. these are *correct withholds*, not
failures.

### Latency — independent sub-sample (N = 18)

An 18-case fixed-seed sub-sample from the same pool, same 30-second-cooldown
protocol.

| Metric | Value |
|---|---|
| Mean wall time, error → verdict | 12.2 s |
| Median wall time, error → verdict | 10.8 s |
| Wall time range | 6.1 s – 35.8 s |
| Mean wall time — accepted (n = 10) | 9.4 s |
| Mean wall time — rejected (n = 8) | 15.6 s |
| Peak RSS — mean | 1490 MB |
| Peak RSS — range | 1472–1506 MB |
| Crashes | 0 / 18 |

Agrees closely with the full-sample headline (mean 12.2 s vs. 13.8 s),
supporting that both measure the pipeline's steady-state latency.

### Reading of Part A

- **Zero crashes across 100 invocations** of a compiler linking an in-process
  LLM backend, with a bounded, flat memory footprint (~1.5 GB, range < 30 MB).
- The gate's asymmetry is by design: **no incorrect fix was surfaced** in the
  sample. Every 0 %-category case was a correct refusal. The 45.5 % headline is
  a *validated-fix yield*, not an accuracy — the complement is dominated by
  correct withholds, not wrong answers shown to the user.
- Categories where the compiler supplies a precise ground-truth category and a
  related location (resolver, default-init, enums, arrays/FFI) yield the
  highest validated-fix rates; open-ended syntax and struct errors yield the
  lowest, matching the expectation that a 1.5-B local model is strongest when
  the compiler has already narrowed the problem.

---

## Part B — compiled-program performance vs. C (same LLVM backend)

Six programs, each implemented identically in Turflang and C: `fib(38)`
(recursive, call-bound), `loopsum` (3×10⁸-iteration scalar loop), `bubblesort`
(n = 6000, O(n²)), `matrixmul` (250×250, O(n³)), `structs` (2000-element
array-of-structs over 20 000 timesteps), `sieve` (Sieve of Eratosthenes over
200 000, ×60). Output was verified byte-for-byte identical across all 24
Turf-vs-clang comparisons (6 programs × 4 opt levels) before any timing.
Each binary run 15 times after a warm-up; median is the primary metric.

### Median wall-clock time (ms)

| Benchmark | Turflang | clang -O0 | clang -O1 | clang -O2 | clang -O3 |
|---|---|---|---|---|---|
| fib(38) | 79.26 | 136.11 | 54.76 | 80.33 | 64.52 |
| loopsum | 139.89 | 169.58 | 117.73 | 92.26 | 92.20 |
| bubblesort | 16.52 | 24.00 | 14.84 | 8.55 | 8.51 |
| matrixmul | 9.52 | 14.25 | 6.82 | 4.75 | 4.82 |
| structs | 13.43 | 41.82 | 12.36 | 9.10 | 9.07 |
| sieve | 19.20 | 43.58 | 16.61 | 13.65 | 13.60 |

### Turflang median relative to each clang level (ratio = Turf / C; < 1 = Turf faster)

| Benchmark | vs -O0 | vs -O1 | vs -O2 | vs -O3 |
|---|---|---|---|---|
| fib(38) | 0.58× | 1.45× | 0.99× | 1.23× |
| loopsum | 0.82× | 1.19× | 1.52× | 1.52× |
| bubblesort | 0.69× | 1.11× | 1.93× | 1.94× |
| matrixmul | 0.67× | 1.40× | 2.00× | 1.98× |
| structs | 0.32× | 1.09× | 1.48× | 1.48× |
| sieve | 0.44× | 1.16× | 1.41× | 1.41× |

### Reading of Part B

Turflang beats unoptimised C by 18–68 % on every benchmark and is within ±20 %
of `clang -O1` on four of six. It does not match `-O2`/`-O3` on the
numeric-loop benchmarks (`loopsum`, `bubblesort`, `matrixmul`), where clang's
auto-vectoriser and loop unroller — passes absent from Turflang's fixed
five-pass pipeline — give a real structural advantage. The two struct/branchy
benchmarks (`structs`, `sieve`) land in the *best* relative position (9 % and
16 % slower than `-O1`; 68 % and 56 % faster than `-O0`), indicating the
`-O2`/`-O3` gap is specifically a vectorisation/unrolling gap on tight numeric
loops, not a general codegen weakness. This is the expected profile for a
compiler whose optimiser runs one fixed, moderate (`-O1`-class) pass sequence.

**What Part B supports:** Turflang is a genuine AOT compiler emitting native
machine code of the same order as production C through the same LLVM backend,
across six distinct codegen shapes, with byte-identical output.
**What it does not claim:** parity with a mature optimising compiler at its
most aggressive setting — it currently does not on numeric-loop-heavy code, and
the gap is attributable to specific missing passes, not a structural flaw.

---

## Conclusion

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

---

## References

> Verification note (see `README.md` for the full explanation of the
> re-verification gate): entries [1]–[4], [6], [7], [9]–[11], [16], [20],
> [22], [23] were confirmed against ACM DL / arXiv during drafting. Entries
> marked `*(re-verify)*` — [5], [12], [13], [15], [18] — carry 2026 arXiv
> identifiers taken from the project's earlier presentation, did not surface in
> a search, and **must be checked (title, authors, year, venue, ID) or removed
> before submission**. [14], [17], [19] are 2025 works whose final venue should
> be confirmed. [21] is an internal project document.

[1] B. A. Becker, P. Denny, R. Pettit, D. Bouchard, D. J. Bouvier, B.
Harrington, A. Kamil, A. Karkare, C. McDonald, P.-M. Osera, J. L. Pearce, and
J. Prather, "Compiler Error Messages Considered Unhelpful: The Landscape of
Text-Based Programming Error Message Research," in *Proc. Working Group Reports
on Innovation and Technology in Computer Science Education (ITiCSE-WGR '19)*,
2019, pp. 177–210. doi:10.1145/3344429.3372508.

[2] P. Denny, A. Luxton-Reilly, and D. Carpenter, "Enhancing Syntax Error
Messages Appears Ineffectual," in *Proc. 19th ACM Conf. Innovation and
Technology in Computer Science Education (ITiCSE '14)*, Uppsala, Sweden, 2014,
pp. 273–278. doi:10.1145/2591708.2591748.

[3] E. A. Santos and B. A. Becker, "Not the Silver Bullet: LLM-enhanced
Programming Error Messages are Ineffective in Practice," in *Proc. 2024 Conf.
United Kingdom & Ireland Computing Education Research (UKICER '24)*, 2024.
arXiv:2409.18661. doi:10.1145/3689535.3689554.

[4] L. Chen, M. Zaharia, and J. Zou, "FrugalGPT: How to Use Large Language
Models While Reducing Cost and Improving Performance," arXiv:2305.05176, 2023.

[5] D. Nogueira et al., "Unreliable in Practice? A Comprehensive Study of
Errors in LLM-Generated Code," arXiv:2608.00661, 2026. *(re-verify)*

[6] S. Rabanser, N. Rauschmayr, A. Wittmann, P. Kairouz, and others,
"Gatekeeper: Improving Model Cascades Through Confidence Tuning,"
arXiv:2502.19335, 2025. (NeurIPS 2025 poster; ICML 2025.)

[7] X. Chen, M. Lin, N. Schärli, and D. Zhou, "Teaching Large Language Models
to Self-Debug," arXiv:2304.05128, 2023. (ICLR 2024.)

[8] Turflang project, "The Compiler Pipeline: Lexer to Codegen" and "CLI,
Diagnostics & Static Analysis," internal architecture documentation
(`docs/architecture/compiler_pipeline.md`, `docs/reference/cli_and_diagnostics.md`),
2026.

[9] E. A. Santos, P. Prasad, and B. A. Becker, "Always Provide Context: The
Effects of Code Context on Programming Error Message Enhancement," in *Proc.
ACM Conf. Global Computing Education Vol. 1 (CompEd '23)*, Hyderabad, India,
2023, pp. 147–153. doi:10.1145/3576882.3617909.

[10] A. Salmon, R. Hammer, E. A. Santos, and B. A. Becker, "Debugging Without
Error Messages: How LLM Prompting Strategy Affects Programming Error Message
Effectiveness," arXiv:2501.05706, 2025.

[11] Q. Chen, J. Li, C. Deng, J. Yu, M. Chen, and I. Ahmed, "A Deep Dive into
LLM Code Generation Mistakes: What and Why?," arXiv:2411.01414, 2024.

[12] "The New Compiler Stack: A Survey on the Synergy of LLMs and Compilers,"
arXiv:2601.02045, 2026. *(re-verify)*

[13] "An Agentic Harness for Real-World Compilers (llvm-autofix),"
arXiv:2603.20075, 2026. *(re-verify)*

[14] "C3PO: Optimized LLM Cascades with Probabilistic Cost Constraints for
Reasoning," arXiv:2511.07396, 2025. (NeurIPS 2025.)

[15] S. Kotte, "UCCI: Calibrated Uncertainty for Cost-Optimal LLM Cascade
Routing," arXiv:2605.18796, 2026. *(re-verify)*

[16] M. Yue, J. Zhao, M. Zhang, L. Du, and Z. Yao, "Large Language Model
Cascades with Mixture-of-Thoughts Representations for Cost-Efficient
Reasoning," arXiv:2310.03094, 2023. (ICLR 2024.)

[17] C. Fanconi and M. van der Schaar, "Cascaded Language Models for
Cost-Effective Human–AI Decision-Making," arXiv:2506.11887, 2025. (NeurIPS
2025.)

[18] Y. Dong, P. Sharma, B. O'Toole, J. P. Champati, and D. Wu, "Pay for
Hints, Not Answers: LLM Shepherding for Cost-Efficient Inference,"
arXiv:2601.22132, 2026. *(re-verify)*

[19] Google Research, "Speculative Cascades: A Hybrid Approach for Smarter,
Faster LLM Inference," Google Research Blog, Sept. 2025.

[20] L. Chen and G. Varoquaux, "What is the Role of Small Models in the LLM
Era: A Survey," arXiv:2409.06857, 2024.

[21] Turflang project, "Turf-Lang Compiler — Benchmark Report: Supporting Data
for Faculty Benchmark Review," Aug. 2026.

[22] Qwen Team, "Qwen2.5-Coder Technical Report," arXiv:2409.12186, 2024.
(Model used: `qwen2.5-coder-1.5b-instruct`, 4-bit `Q4_K_M` GGUF.)

[23] G. Gerganov and contributors, "llama.cpp: LLM inference in C/C++,"
open-source project, https://github.com/ggml-org/llama.cpp. (Vendored as the
in-process inference backend; GBNF grammar-constrained sampling.)

---

