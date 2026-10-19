"""Shared pieces of the RAG experiment: the production error-category router,
BM25, documentation chunking, retrieval for each arm, and prompt building."""
from __future__ import annotations

import json
import math
import random
import re
import urllib.request
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
DOCS_DIR = REPO_ROOT / "docs" / "slm"
ERROR_CODES_H = REPO_ROOT / "src" / "diagnostics" / "ErrorCodes.h"

ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")


def strip_ansi(s: str) -> str:
    return ANSI_RE.sub("", s)


# --------------------------------------------------------------------------
# A1: the production category classifier (src/diagnostics/ErrorCategory.h)
# --------------------------------------------------------------------------

def extract_context_keyword(msg: str) -> str:
    """Port of SLM::extractContextKeyword (message text only, same order)."""
    if "Struct" in msg or "struct" in msg or "has no method named" in msg:
        return "struct"
    if "Enum" in msg or "enum" in msg:
        return "enum"
    if any(w in msg for w in ("module", "Module", "import", "Import", "export", "Export")):
        return "module"
    for kw in ("elseif", "while", "for", "if", "return", "break", "continue"):
        if kw in msg:
            return kw
    return ""


CONSTRUCT_DOCS = {
    "typeof": "docs/slm/builtin_typeof.txt",
    "struct": "docs/slm/structs.txt",
    "enum": "docs/slm/enums.txt",
    "module": "docs/slm/modules.txt",
}
CATEGORY_DOCS = {
    "TYPE_MISMATCH": "docs/slm/type_mismatch.txt",
    "UNDECLARED_VAR": "docs/slm/undeclared_var.txt",
    "CONTROL_FLOW": "docs/slm/control_flow.txt",
    "SYNTAX_ERROR": "docs/slm/syntax_error.txt",
    "ARITHMETIC": "docs/slm/arithmetic.txt",
    "STRING_CONVERSION": "docs/slm/string_conversion.txt",
    "ARRAY_ERROR": "docs/slm/array_error.txt",
    "DECLARATION": "docs/slm/declaration.txt",
    "FUNCTION_SIGNATURE": "docs/slm/function_signature.txt",
    "BUILTIN_FUNCTION": "docs/slm/general.txt",
    "MEMORY_ERROR": "docs/slm/general.txt",
    "GENERAL": "docs/slm/general.txt",
}

_code_category: dict[str, str] | None = None


def code_categories() -> dict[str, str]:
    """error code -> ErrorCategory name, read from the AllErrorCodes table in
    src/diagnostics/ErrorCodes.h (production gets the category from the raising
    error class; that table is the per-code record of the same value)."""
    global _code_category
    if _code_category is not None:
        return _code_category
    text = ERROR_CODES_H.read_text()
    consts = dict(re.findall(r'ErrCode::(\w+)\s*=\s*"(E\d+)"', text))
    # `inline constexpr const char *Name = "Exxx";`
    consts.update(re.findall(r'const char \*(\w+)\s*=\s*"(E\d+)"', text))
    str_lit = r'"(?:[^"\\]|\\.)*"'
    entry = re.compile(
        r'\{\s*("E\d+"|ErrCode::\w+)\s*,\s*' + str_lit + r'\s*,\s*' + str_lit +
        r'\s*,\s*SLM::ErrorCategory::(\w+)')
    out: dict[str, str] = {}
    for key, cat in entry.findall(text):
        if key.startswith('"'):
            code = key.strip('"')
        else:
            code = consts.get(key.split("::", 1)[1], "")
        if code:
            out[code] = cat
    _code_category = out
    return out


def category_doc_path(error_code: str, error_message: str) -> tuple[str, str, str]:
    """-> (category, keyword, doc path relative to the repo root)."""
    cat = code_categories().get(error_code, "GENERAL")
    kw = extract_context_keyword(error_message)
    if kw in CONSTRUCT_DOCS:
        return cat, kw, CONSTRUCT_DOCS[kw]
    return cat, kw, CATEGORY_DOCS.get(cat, "docs/slm/general.txt")


# --------------------------------------------------------------------------
# BM25
# --------------------------------------------------------------------------

_TOK_RE = re.compile(r"[A-Za-z_]\w*|\d+|->|\+\+|--|::|==|!=|&&|\|\||\+=|-=|=>")


def tokenize_text(s: str) -> list[str]:
    return [t.lower() for t in _TOK_RE.findall(s)]


class BM25:
    def __init__(self, docs: list[list[str]], k1: float = 1.2, b: float = 0.75):
        self.k1, self.b = k1, b
        self.docs = docs
        self.N = len(docs)
        self.avg = (sum(len(d) for d in docs) / self.N) if self.N else 0.0
        self.tf = []
        self.df: dict[str, int] = {}
        for d in docs:
            tf: dict[str, int] = {}
            for t in d:
                tf[t] = tf.get(t, 0) + 1
            self.tf.append(tf)
            for t in tf:
                self.df[t] = self.df.get(t, 0) + 1

    def idf(self, t: str) -> float:
        n = self.df.get(t, 0)
        return math.log(1 + (self.N - n + 0.5) / (n + 0.5))

    def score(self, query: list[str], i: int) -> float:
        tf, dl = self.tf[i], len(self.docs[i])
        s = 0.0
        for t in query:
            f = tf.get(t, 0)
            if f:
                s += self.idf(t) * f * (self.k1 + 1) / (
                    f + self.k1 * (1 - self.b + self.b * dl / (self.avg or 1)))
        return s

    def rank(self, query: list[str]) -> list[tuple[float, int]]:
        scored = [(self.score(query, i), i) for i in range(self.N)]
        scored.sort(key=lambda x: (-x[0], x[1]))
        return scored


# --------------------------------------------------------------------------
# Documentation chunks (A2, A4)
# --------------------------------------------------------------------------

# Files under docs/slm/ that are not retrieval material: error_categories.txt
# is the classifier reference (never injected into a fix prompt in production).
# docs/slm/codes/ (per-code pages) is not part of docs/slm/*.txt.
EXCLUDED_DOCS = {"error_categories.txt"}
MIN_CHARS, MAX_CHARS = 350, 900  # ~100-250 tokens at ~3.6 chars/token
_HEADING_RE = re.compile(r"^[A-Z][A-Z0-9 ,/()'`._\-]{3,}:?\s*$|^[A-Z][A-Z0-9 ,/()'`._\-]{3,}:")


def _units(text: str) -> list[str]:
    units, cur = [], []
    for line in text.split("\n"):
        if not line.strip():
            if cur:
                units.append("\n".join(cur))
                cur = []
            continue
        if _HEADING_RE.match(line) and cur:
            units.append("\n".join(cur))
            cur = []
        cur.append(line)
    if cur:
        units.append("\n".join(cur))
    return units


def _split_big(unit: str) -> list[str]:
    if len(unit) <= MAX_CHARS:
        return [unit]
    out, cur = [], ""
    for line in unit.split("\n"):
        if cur and len(cur) + len(line) + 1 > MAX_CHARS:
            out.append(cur)
            cur = ""
        cur += ("\n" if cur else "") + line
    if cur:
        out.append(cur)
    return out


def build_doc_chunks(docs_dir: Path = DOCS_DIR) -> list[dict]:
    chunks = []
    for f in sorted(docs_dir.glob("*.txt")):
        if f.name in EXCLUDED_DOCS:
            continue
        pieces = [p for u in _units(f.read_text()) for p in _split_big(u)]
        merged, cur = [], ""
        for p in pieces:
            if cur and len(cur) + len(p) + 2 > MAX_CHARS:
                merged.append(cur)
                cur = ""
            cur = (cur + "\n\n" + p) if cur else p
            if len(cur) >= MIN_CHARS:
                merged.append(cur)
                cur = ""
        if cur:
            if merged and len(cur) < MIN_CHARS // 2 and len(merged[-1]) + len(cur) + 2 <= MAX_CHARS + 300:
                merged[-1] += "\n\n" + cur
            else:
                merged.append(cur)
        for i, m in enumerate(merged):
            chunks.append({"id": f"{f.stem}#{i}", "file": f.name, "text": m})
    return chunks


# --------------------------------------------------------------------------
# Server client
# --------------------------------------------------------------------------

class Server:
    def __init__(self, url: str):
        self.url = url.rstrip("/")
        self._tok_cache: dict[str, int] = {}

    def _post(self, path: str, body: dict, timeout: float = 900) -> dict:
        req = urllib.request.Request(self.url + path, json.dumps(body).encode(),
                                     {"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read())

    def count(self, text: str) -> int:
        if text not in self._tok_cache:
            r = self._post("/tokenize", {"content": text, "add_special": False}, 60)
            self._tok_cache[text] = len(r["tokens"])
        return self._tok_cache[text]

    def truncate_to(self, text: str, budget: int) -> str:
        """Longest line-aligned prefix of `text` within `budget` tokens."""
        if self.count(text) <= budget:
            return text
        lines = text.split("\n")
        lo, hi = 0, len(lines)
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if self.count("\n".join(lines[:mid])) <= budget:
                lo = mid
            else:
                hi = mid - 1
        return "\n".join(lines[:lo])

    def chat(self, system: str, user: str, schema: dict, max_tokens: int = 200,
             cache_prompt: bool = False) -> dict:
        body = {
            "messages": [{"role": "system", "content": system},
                         {"role": "user", "content": user}],
            "temperature": 0, "top_k": 1, "seed": 0, "max_tokens": max_tokens,
            "cache_prompt": cache_prompt,
            "response_format": {"type": "json_schema",
                                "json_schema": {"name": "fix", "strict": True, "schema": schema}},
        }
        return self._post("/v1/chat/completions", body)


FIX_SCHEMA = {
    "type": "object",
    "properties": {"line": {"type": "integer"}, "new_code": {"type": "string"}},
    "required": ["line", "new_code"],
    "additionalProperties": False,
}

# --------------------------------------------------------------------------
# Retrieval
# --------------------------------------------------------------------------

DEFAULT_BUDGET = 1200


def diag_for_prompt(diagnostic: str) -> str:
    """Compiler output as the model sees it: no ANSI, no `Fix:` block (a
    compiler fix would hand over the answer, and production only asks the model
    when there is none), no `--explain` pointer."""
    lines = strip_ansi(diagnostic).split("\n")
    out, skipping = [], False
    for ln in lines:
        if re.match(r"^\s*Fix:\s", ln):
            skipping = True
            continue
        if skipping:
            if not ln.strip():
                skipping = False
            else:
                continue
        if "--explain" in ln:
            continue
        out.append(ln)
    return re.sub(r"\n{3,}", "\n\n", "\n".join(out)).strip()


def diag_line_text(case: dict) -> str:
    n = case.get("diag_line") or case.get("line") or 0
    src = case["mutated_source"].split("\n")
    return src[n - 1] if 1 <= n <= len(src) else ""


def assert_no_leak(case: dict, examples: list[dict]) -> None:
    for e in examples:
        assert e.get("split") == "train", f"leak: {e['id']} is not from the train split"
        assert e["source_test"] != case["source_test"], (
            f"leak: {e['id']} shares source_test {e['source_test']} with {case['id']}")
        assert e["id"] != case["id"], f"leak: {e['id']} is the query itself"


def example_block(examples: list[dict]) -> str:
    parts = []
    for e in examples:
        code = f"{e['error_code']}: " if e.get("error_code") else ""
        parts.append("Example\n"
                     f"Broken line: {e['mutated_line'].rstrip()}\n"
                     f"Compiler said: {code}{e['error_message']}\n"
                     f"Fixed line: {e['original_line'].rstrip()}")
    return "\n\n".join(parts)


class Retriever:
    def __init__(self, server: Server, train: list[dict], budget: int = DEFAULT_BUDGET,
                 docs_dir: Path = DOCS_DIR):
        for t in train:
            assert t["split"] == "train", f"train file holds a {t['split']} entry: {t['id']}"
        self.server, self.train, self.budget = server, train, budget
        self.chunks = build_doc_chunks(docs_dir)
        self.chunk_bm25 = BM25([tokenize_text(c["text"]) for c in self.chunks])
        self.ex_bm25 = BM25([tokenize_text(e["error_message"] + " " + e["mutated_line"]) for e in train])
        self._doc_cache: dict[str, str] = {}

    # --- A1
    def category(self, case: dict) -> tuple[str, dict]:
        cat, kw, path = category_doc_path(case.get("error_code", ""), case.get("error_message", ""))
        p = REPO_ROOT / path
        if path not in self._doc_cache:
            if not p.exists():
                p = REPO_ROOT / "docs/slm/general.txt"
            self._doc_cache[path] = p.read_text()
        block = self.server.truncate_to(self._doc_cache[path].rstrip(), self.budget)
        return block, {"category": cat, "keyword": kw, "doc": path}

    # --- A1p: what production --smart assembles (SLMIntegration.cpp): the syntax
    # card, then "THIS ERROR (Exxx):" + docs/slm/codes/Exxx.txt, then
    # "GENERAL REFERENCE:" + the category doc; cut from the tail to the budget.
    def production_text(self, case: dict) -> tuple[str, dict]:
        cat_block, info = self.category(case)
        cat_text = self._doc_cache.get(info["doc"], cat_block).rstrip()
        text = cat_text
        code = case.get("error_code", "")
        code_path = REPO_ROOT / "docs/slm/codes" / f"{code}.txt"
        has_code = bool(code) and code_path.exists()
        if has_code:
            text = f"THIS ERROR ({code}):\n{code_path.read_text()}\nGENERAL REFERENCE:\n{text}"
        card = REPO_ROOT / "docs/slm/syntax_card.txt"
        if card.exists():
            text = card.read_text() + "\n" + text
        return text, {**info, "code_doc": has_code}

    def production(self, case: dict, budget: int | None = None) -> tuple[str, dict]:
        budget = self.budget if budget is None else budget
        text, info = self.production_text(case)
        return self.server.truncate_to(text, max(budget, 0)), info

    # --- A6: production docs + retrieved examples, sharing the budget
    def production_examples(self, case: dict) -> tuple[str, dict]:
        ex = self.examples(case)
        exb = example_block(ex)
        rest = self.budget - self.server.count(exb) - 4
        pblock, info = self.production(case, rest)
        block = exb + ("\n\n" + pblock if pblock else "")
        return block, {"examples": [e["id"] for e in ex], **info}

    # --- A2
    def docs(self, case: dict, budget: int | None = None) -> tuple[str, dict]:
        budget = self.budget if budget is None else budget
        line = diag_line_text(case)
        q = tokenize_text(case.get("error_message", "")) + tokenize_text(line) + \
            re.findall(r"[A-Za-z_]\w*", line)
        q = [t.lower() if not t.islower() else t for t in q]
        picked, used = [], 0
        for s, i in self.chunk_bm25.rank(q):
            if s <= 0:
                break
            c = self.chunks[i]
            text = f"[{c['file']}]\n{c['text']}"
            n = self.server.count(text) + 2
            if used + n <= budget:
                picked.append((c, text))
                used += n
        block = "\n\n".join(t for _, t in picked)
        return block, {"chunks": [c["id"] for c, _ in picked]}

    # --- A3
    def _pool(self, case: dict) -> list[int]:
        return [i for i, e in enumerate(self.train)
                if e["source_test"] != case["source_test"] and e["id"] != case["id"]]

    def examples(self, case: dict, k: int = 3) -> list[dict]:
        pool = self._pool(case)
        q = tokenize_text(case.get("error_message", "") + " " + case["mutated_line"])
        scored = [(self.ex_bm25.score(q, i), i) for i in pool]
        code = case.get("error_code", "")
        scored.sort(key=lambda x: (0 if (code and self.train[x[1]]["error_code"] == code) else 1,
                                   -x[0], x[1]))
        out, seen = [], set()
        for _, i in scored:
            e = self.train[i]
            key = (e["mutated_line"].strip(), e["original_line"].strip())
            if key in seen:
                continue
            seen.add(key)
            out.append(e)
            if len(out) == k:
                break
        assert_no_leak(case, out)
        return out

    def examples_block(self, case: dict) -> tuple[str, dict]:
        ex = self.examples(case)
        block = self.server.truncate_to(example_block(ex), self.budget)
        return block, {"examples": [e["id"] for e in ex]}

    # --- A4
    def examples_docs(self, case: dict) -> tuple[str, dict]:
        ex = self.examples(case)
        exb = example_block(ex)
        rest = self.budget - self.server.count(exb) - 4
        dblock, info = self.docs(case, max(rest, 0))
        block = exb + ("\n\n" + dblock if dblock else "")
        return block, {"examples": [e["id"] for e in ex], **info}

    # --- A5
    def random_examples(self, case: dict, seed: int = 0, k: int = 3) -> tuple[str, dict]:
        pool = self._pool(case)
        rng = random.Random(f"{seed}:{case['id']}")
        ex = [self.train[i] for i in rng.sample(pool, min(k, len(pool)))]
        assert_no_leak(case, ex)
        block = self.server.truncate_to(example_block(ex), self.budget)
        return block, {"examples": [e["id"] for e in ex]}

    def context(self, arm: str, case: dict) -> tuple[str, dict]:
        if arm == "A0_none":
            return "", {}
        if arm == "A1_category":
            return self.category(case)
        if arm == "A1p_production":
            return self.production(case)
        if arm == "A2_bm25_docs":
            return self.docs(case)
        if arm == "A3_examples":
            return self.examples_block(case)
        if arm == "A4_examples_docs":
            return self.examples_docs(case)
        if arm == "A5_random_examples":
            return self.random_examples(case)
        if arm == "A6_production_examples":
            return self.production_examples(case)
        raise ValueError(arm)


ARMS = ["A0_none", "A1_category", "A1p_production", "A2_bm25_docs", "A3_examples",
        "A4_examples_docs", "A5_random_examples", "A6_production_examples"]

# --------------------------------------------------------------------------
# Prompt
# --------------------------------------------------------------------------

SYSTEM_PROMPT = (
    "You are a compiler assistant for Turf-Lang, a statically-typed language you "
    "have not seen before.\n\n"
    "GROUNDING: everything you know about Turf-Lang syntax comes ONLY from the "
    "context given in the message, NOT from C, Python, Java, Rust, or any other "
    "language. Turf-Lang has unusual choices (postfix casts like `x to int`, "
    "`mut`/`immut` instead of a bare type keyword, `give ... otherwise ...` for a "
    "ternary) that do not exist in mainstream languages -- do not silently "
    "'correct' these into something more familiar. If a construct is not shown, "
    "do not invent it.\n\n"
    "TASK: the program does not compile. Fix it by replacing exactly ONE line.\n"
    "RULES:\n"
    "1. \"line\" is the 1-based number of the line you replace. It may differ from "
    "the line the compiler points at.\n"
    "2. \"new_code\" is the complete replacement line in Turf-Lang, with its "
    "leading indentation: raw code only, one line, no explanation, no markdown "
    "fence, no line number.\n"
    "3. If you cannot produce a valid one-line fix, answer {\"line\": 0, "
    "\"new_code\": \"\"}. That is the right answer whenever you are not sure; it is "
    "better than a wrong patch.\n"
    "4. Respond ONLY with the JSON object."
)

PROGRAM_BUDGET = 1300


def numbered_program(server: Server, case: dict) -> tuple[str, bool]:
    """The mutated program with line numbers. Programs over PROGRAM_BUDGET tokens
    are cut to a window around the diagnostic's line (same for every arm)."""
    lines = case["mutated_source"].rstrip("\n").split("\n")
    fmt = lambda a, b: "\n".join(f"{n:4d} | {lines[n - 1]}" for n in range(a, b + 1))
    full = fmt(1, len(lines))
    if server.count(full) <= PROGRAM_BUDGET:
        return full, False
    center = case.get("diag_line") or case.get("line") or 1
    half = len(lines)
    while half > 4:
        half = int(half * 0.75)
        a, b = max(1, center - half), min(len(lines), center + half)
        text = fmt(a, b)
        if server.count(text) <= PROGRAM_BUDGET:
            return (f"(lines {a}-{b} of {len(lines)} shown)\n" + text), True
    a, b = max(1, center - 4), min(len(lines), center + 4)
    return fmt(a, b), True


def build_user_prompt(server: Server, case: dict, context: str) -> tuple[str, bool]:
    prog, trunc = numbered_program(server, case)
    return (
        "PROGRAM (line numbers are added for you, they are not part of the code):\n"
        f"{prog}\n\n"
        "COMPILER OUTPUT:\n"
        f"{diag_for_prompt(case['diagnostic'])}\n\n"
        "CONTEXT:\n"
        f"{context if context else '(none)'}\n\n"
        "Reply with ONLY the JSON object {\"line\": <int>, \"new_code\": <string>} "
        "for the one line to replace, or {\"line\": 0, \"new_code\": \"\"} if you cannot fix it."
    ), trunc
