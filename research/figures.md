# Figures — Mermaid source

Renderable source for the five methodology figures. Paste a block into
<https://mermaid.live> (or any Mermaid renderer) and export SVG/PNG for the
paper. ASCII equivalents are inline in `03_methodology.md`.

---

## Figure 1 — The compiler and its diagnostic layer

```mermaid
flowchart LR
    SRC[source.tr] --> LEX[Lexer]
    LEX -->|tokens| PAR[Parser]
    PAR -->|AST| RES[Resolver<br/>2-pass]
    RES -->|annotated AST| TC[TypeChecker]
    TC -->|type-annotated AST| CG[Codegen]
    CG --> IR[LLVM IR] --> BIN[native binary]
    CG -.per function.-> CFG[CFG static-analysis battery<br/>Eq. 1-4, every compile]

    LEX -.->|TurfError / TurfWarning / LogErrorAt| DE
    PAR -.-> DE
    RES -.-> DE
    TC  -.-> DE
    CG  -.-> DE
    CFG -.-> DE

    subgraph DIAG [diagnostic layer -- runs beside the pipeline]
        DE[DiagnosticEngine<br/>single choke point] --> DISP[Tiered diagnostic dispatch<br/>see Figure 2]
    end
```

---

## Figure 2 — Tiered diagnostic dispatch (per raised diagnostic)

```mermaid
flowchart TD
    D[diagnostic d<br/>category c, location L, related R, hint h] --> CAS{error already<br/>on L.line?}
    CAS -->|yes| DROP[drop -- cascade suppression]
    CAS -->|no| T0

    subgraph T0G [Tier 0 -- always, zero cost]
        T0[category c from typeid d<br/>did-you-mean via Eq. 5-6 / Algorithm 2<br/>footer to turf --explain E0NN]
    end

    T0 --> GATE1{--smart set<br/>AND d is an error<br/>AND no AI fix fired this pass?}
    GATE1 -->|no| EMIT0[emit Tier-0 diagnostic<br/>+ cascade note if applicable]
    GATE1 -->|yes| T1

    subgraph T1G [Tier 1 -- local SLM]
        T1[classify -> size context Eq. 7 -> pin R -> inject doc] --> T1B[grammar-constrained SLM call<br/>Listing 4]
        T1B --> T1C[structural prior Eq. 8]
        T1C --> T1D[SANDBOX Algorithm 4]
        T1D -->|fail, attempts left| T1B
        T1D --> T1E[finalize confidence Eq. 9]
    end

    T1E --> GATE2{valid AND conf >= 0.8<br/>AND fix_mode != NONE<br/>Eq. 10}
    GATE2 -->|pass| AIB["replace message with [AI Suggestion] block"]
    GATE2 -->|fail| EMIT1[emit the compiler's own unmodified diagnostic]

    EMIT1 -.-> T2
    T2[Tier 2 -- reserved, not implemented<br/>would re-enter the same sandbox gate]:::ghost
    classDef ghost stroke-dasharray: 5 5,fill:#f6f6f6,color:#888
```

---

## Figure 3 — The `--smart` Tier-1 pipeline

```mermaid
flowchart TD
    E[error raised -- first this pass] --> A["(a) deterministic category<br/>c = Category_ else classifyError(msg)"]
    A --> B["(b) context assembly<br/>window w(c) lines each side -- Eq. 7<br/>pin RelatedLocations outside the window<br/>inject docs/slm/&lt;c&gt;.txt"]
    B --> C["(c) prompt build<br/>ChatML: grounding/voice | category | doc | rules<br/>user JSON: error, hint, code [, previous_attempt_failed]"]
    C --> Dn["(d) local inference<br/>Qwen2.5-Coder-1.5B Q4_K_M via llama.cpp, in-process<br/>greedy + repetition penalty, GBNF-constrained (Listing 4)"]
    Dn --> F1["(e) structural prior s -- Eq. 8 / Algorithm 5"]
    F1 --> SB["(f) SANDBOX -- Algorithm 4<br/>deep-copy state; splice patch;<br/>re-parse + resolve + typecheck + lint;<br/>confirm original error gone"]
    SB -->|fail, attempt &lt; 3| RB["previous_attempt_failed = sandbox diagnostics"]
    RB --> C
    SB -->|fail, attempt = 3| NONE["fix_mode = NONE"]
    SB -->|pass| G
    NONE --> G["(g) finalize confidence<br/>conf = Finalize(s, sandbox_ok) -- Eq. 9"]
    G --> H{"(h) display gate<br/>valid AND conf >= 0.8 AND fix_mode != NONE -- Eq. 10"}
    H -->|pass| SHOW["[AI Suggestion] block replaces the compiler message"]
    H -->|fail| PLAIN["the compiler's own unmodified diagnostic"]
```

---

## Figure 4 — Sandbox lifecycle

```mermaid
stateDiagram-v2
    [*] --> Snapshot : DeepCopy(source, AST, symbol table)
    note right of Snapshot
        original snapshot retained
        for rollback
    end note
    Snapshot --> Patched : applyFix(lo, hi, replacement_line)
    Patched --> Validating : validate()  (full re-parse + resolve + typecheck + lint)
    Validating --> Decision : capture all diagnostics
    Decision --> Committed : no error-level diag AND isOriginalErrorResolved(errorLine)
    Decision --> Rejected : otherwise
    Committed --> [*] : commitToMain()  (main state advances)
    Rejected --> [*] : reject()  (discard; original diagnostic stands)
```

---

## Figure 5 — The confidence transform

```mermaid
flowchart LR
    S["structural prior s in [0, 1]<br/>Eq. 8"] --> Q{sandbox result}
    Q -->|FAIL / NONE| L["conf = min(s, 0.5)<br/>interval [0.0, 0.5]"]
    Q -->|PASS| Hi["conf = max(0.9, s)<br/>interval [0.9, 1.0]"]
    L --> GATE["display gate at 0.8<br/>(Eq. 10)"]
    Hi --> GATE
    GATE -->|conf &lt; 0.8| NEVER[never shown]
    GATE -->|conf &gt;= 0.8| SHOWN["shown as [AI Suggestion]"]
```
