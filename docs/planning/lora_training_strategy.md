# LoRA Training Strategy for Turf-Lang

Training a Small Language Model (SLM) on a completely custom language requires a specialized approach. Because Turf-Lang does not exist in the model's pre-training data (like Python or C++ do), you cannot rely on its innate knowledge. 

You want to train two distinct LoRA (Low-Rank Adaptation) adapters. This is an excellent architecture choice because `llama.cpp` can dynamically swap or stack LoRA adapters onto the same base model in memory without reloading the multi-gigabyte base weights.

Here is the end-to-end strategy to achieve this.

---

## Adapter 1: Syntax & Instant Fixes

**Goal:** Teach the model Turf-Lang's specific syntax rules so it stops hallucinating C/Java/Python syntax when generating `code_patch` suggestions.

### 1. Dataset Generation (The Hardest Part)
Since you can't scrape GitHub for Turf-Lang code, you must procedurally generate synthetic data.
- **Write a Code Mutator:** Create a script (or a C++ tool in the compiler) that takes valid, passing Turf-Lang test files and intentionally breaks them. 
  - *Mutations:* Delete a semicolon, change `int` to `string`, remove a closing brace, swap function arguments, etc.
- **Run the Compiler:** Pass the broken code through the Turf-Lang compiler to capture the exact `Error` and `Code Context` (the exact lines your diagnostic engine outputs).
- **Format:** Construct JSONL files that exactly match the ChatML prompt structure you are already using in `SLMIntegration.cpp`.

**Example Training Row:**
```json
{
  "messages": [
    {"role": "system", "content": "You are a conservative compiler assistant for Turf-Lang... [Your TYPE_MISMATCH rules]"},
    {"role": "user", "content": "{\"error\": \"Cannot assign string to int\", \"code\": \"12 | mut count: int = \\\"hello\\\";\"}"},
    {"role": "assistant", "content": "{\"explanation\": \"Type mismatch in assignment.\", \"fix_mode\": \"MODIFY\", \"code_patch\": \"count: int = 0;\", ...}"}
  ]
}
```

### 2. Training
Use a fast training framework like **Unsloth** or **Axolotl** (Python-based) rather than raw `llama.cpp` for the actual training phase, as they are heavily optimized for GPU memory.
- **Rank (r):** 16 to 32 (Syntax fixing is mostly local pattern matching).
- **Epochs:** 2-3 (Be careful not to overfit, or it will repeat the same fix for everything).

### 3. Integration
Load `syntax_fixer.lora` into your `llama_context` specifically when `fetchSuggestion()` is called. 

---

## Adapter 2: AST Generation (Neural Parser)

**Goal:** Teach the model to read raw Turf-Lang text and emit a serialized Abstract Syntax Tree (AST), effectively acting as a neural parser.

### 1. Dataset Generation
This requires high-fidelity, complex data mappings.
- **AST Serializer:** You first need to write a function in the compiler (e.g., in `ASTPrinter.h` or a new `ASTSerializer.h`) that dumps the C++ AST into a structured JSON format.
- **Fuzzer / Corpus:** Take a massive corpus of valid Turf-Lang code (your tests + procedurally generated valid code).
- **Generation:** Compile them, dump the JSON AST.

**Example Training Row:**
```json
{
  "messages": [
    {"role": "system", "content": "You are a Turf-Lang parser. Convert the code into a JSON AST."},
    {"role": "user", "content": "fn add(a: int, b: int) -> int { return a + b; }"},
    {"role": "assistant", "content": "{\"type\": \"FunctionDef\", \"name\": \"add\", \"params\": [...], \"body\": [...] }"}
  ]
}
```

### 2. Training
AST generation is a much harder reasoning task than syntax fixing because it requires tracking deep, nested, hierarchical relationships.
- **Rank (r):** 64 to 128 (Higher rank needed for complex structural reasoning).
- **Target Modules:** You must target `q_proj`, `k_proj`, `v_proj`, `o_proj`, `gate_proj`, `up_proj`, `down_proj` (target ALL linear layers in the transformer to give it maximum capacity to learn structure).

### 3. Integration & Guardrails
A neural network is probabilistic; it *will* occasionally generate invalid AST structures. 
- **GBNF Grammar:** You **must** generate a highly strict GBNF grammar for your AST JSON schema and apply it via `llama_sampler_init_grammar` during inference. This mathematically guarantees the SLM cannot output a malformed JSON string, ensuring your C++ deserializer won't crash when reading the model's AST output.

---

## Tooling Pipeline

1. **Data Prep:** Procedural C++ generator -> JSONL.
2. **Train:** Unsloth (Python) on Google Colab or a local GPU.
3. **Export:** Export the trained LoRA to GGUF format (`export_gguf` in Unsloth).
4. **C++ Loading:** In `SLMEngine.cpp`, use `llama_model_lora_adapter_set(model, "syntax_fixer.gguf", 1.0f)` to hot-swap the adapter.

By using separate LoRAs, your single base model can instantly switch between being a "Syntax Fixer" and an "AST Builder" without polluting the weights of either task!
