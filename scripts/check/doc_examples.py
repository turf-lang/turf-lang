#!/usr/bin/env python3
"""doc_examples.py -- REL-004 / REL-006: the reference docs as conformance tests.

Every ```turflang block in docs/reference/**/*.md and docs/guides/**/*.md is
compiled with the real compiler. The reference is the user-facing statement of the language, so an
example that no longer compiles is a doc that has drifted from it (the
2026-08-21 rewrite was done against 1.0 and has not kept up: for one, it still
says there is no type inference).

How a block is judged
  * It is a complete program if it has `fn main`; otherwise it is wrapped:
      - a block that starts with a declaration (fn, struct, enum, error,
        machine, import, operator, extern, export, pub, @annotation, immut/mut
        at top level) gets `fn main(): int { return 0; }` appended;
      - anything else is a statement fragment and is wrapped in main().
  * The compile must succeed (warnings are fine).
  * The pages build their examples up, so a block that fails ON ITS OWN is tried
    again with two kinds of context the prose supplies: an `import m;` for every
    stdlib module it names as `m::`, and the earlier declaration blocks of the
    same page (struct, enum, fn, ...) that themselves compile, kept in order.
    A block that needs more than that is not self-contained, and is registered.

Marking a block (an HTML comment on the line directly above the fence):
  <!-- doc-example: fails E066 -->   the block shows a mistake: it must be
                                     REJECTED, and with that code
  <!-- doc-example: skip <reason> -->  not compiled (output, pseudo-syntax)

Blocks that fail and carry no marker are listed in docs/reference/
DOC_EXAMPLES_GAPS.txt, keyed by file, ordinal and a hash of the block, so
that (like tests/DIAGNOSTIC_GAPS.txt) the register can only shrink:
  * a block that fails and is not in the register fails the check (new drift);
  * a register line whose block now compiles, was marked, or changed fails the
    check (stale: delete it).
REL-006 drains the register by rewriting the docs; it is a to-do list, not an
excuse.

Usage: scripts/check/doc_examples.py [--update-register] [--compiler PATH] [-v]
Exit:  0 = consistent; 1 = new drift or a stale register line.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import re
import subprocess
import sys
import tempfile
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DOCS = REPO / "docs" / "reference"
GUIDES = REPO / "docs" / "guides"
REGISTER = DOCS / "DOC_EXAMPLES_GAPS.txt"
ANSI = re.compile(r"\x1b\[[0-9;]*m")
FENCE = re.compile(r"^(?P<marker><!--\s*doc-example:\s*(?P<kind>fails|skip)\s*(?P<arg>[^>]*?)\s*-->\s*\n)?```turflang[ \t]*\n(?P<body>.*?)^```[ \t]*$",
                   re.S | re.M)
DECL_START = re.compile(r"^\s*(?:@\w+|fn\b|struct\b|enum\b|error\b|machine\b|import\b|operator\b|extern\b|export\b|pub\b|public\b|unsafe\s+module\b|immut\b|immutable\b|mut\b|mutable\b)")


class Block:
    def __init__(self, path: Path, ordinal: int, body: str, kind: str | None, arg: str):
        self.path, self.ordinal, self.body, self.kind, self.arg = path, ordinal, body, kind, arg
        self.hash = hashlib.sha1(body.encode()).hexdigest()[:8]

    @property
    def key(self) -> str:
        return f"{self.path.relative_to(REPO).as_posix()}#{self.ordinal} {self.hash}"

    def program(self, context: str = "", imports: tuple[str, ...] = ()) -> str:
        body = self.body
        head = "".join(f"import {m};\n" for m in imports)
        if re.search(r"\bfn\s+(?:int\s+)?main\s*\(", body):
            return head + body
        first = next((l for l in body.splitlines() if l.strip() and not l.strip().startswith("//")), "")
        # `mut x: int = 1;` is a statement in a function and a top-level
        # constant outside one; the docs mean the former far more often.
        top_level = DECL_START.match(first) and not re.match(r"\s*(?:immut|immutable|mut|mutable)\b", first)
        if top_level:
            return head + context + body.rstrip("\n") + "\n\nfn main(): int {\n    return 0;\n}\n"
        lines = body.splitlines()
        own_imports = [l for l in lines if re.match(r"\s*import\b", l)]
        rest = [l for l in lines if l not in own_imports]
        return (head + "\n".join(own_imports) + "\n" + context + "fn main(): int {\n"
                + "\n".join("    " + l for l in rest) + "\n    return 0;\n}\n")

    def is_declaration(self) -> bool:
        first = next((l for l in self.body.splitlines() if l.strip() and not l.strip().startswith("//")), "")
        return bool(DECL_START.match(first)) and not re.match(r"\s*(?:immut|immutable|mut|mutable)\b", first) \
            and not re.search(r"\bfn\s+(?:int\s+)?main\s*\(", self.body)


def extract() -> list[Block]:
    blocks = []
    pages = sorted(DOCS.rglob("*.md")) + (sorted(GUIDES.rglob("*.md")) if GUIDES.is_dir() else [])
    for md in pages:
        text = md.read_text()
        for n, m in enumerate(FENCE.finditer(text), 1):
            blocks.append(Block(md, n, m["body"], m["kind"], (m["arg"] or "").strip()))
    return blocks


STDLIB = sorted({p.stem for p in (REPO / "lib" / "std").rglob("*.tr")})
PRELUDE = {"int", "double", "bool", "char", "strings", "array", "io", "math", "sys"}


def stdlib_imports(text: str) -> tuple[str, ...]:
    """`import m;` for each stdlib module `m` the text spells as `m::` and does not import."""
    return tuple(m for m in STDLIB if m not in PRELUDE and re.search(rf"\b{m}::", text)
                 and not re.search(rf"\bimport\s+{m}\b", text))


def compile_text(compiler: str, program: str) -> tuple[bool, str]:
    """(compiled, first error code or '')."""
    with tempfile.TemporaryDirectory(prefix="turf_doc_") as d:
        src = Path(d) / "example.tr"
        src.write_text(program)
        try:
            p = subprocess.run([compiler, str(src), "-O0", "-o", str(Path(d) / "a.out")],
                               capture_output=True, text=True, errors="replace", timeout=60, cwd=d)
        except subprocess.TimeoutExpired:
            return False, "TIMEOUT"
        out = ANSI.sub("", p.stdout + p.stderr)
        m = re.search(r"^── error[^\n]*\[(E\d+)", out, re.M)
        return p.returncode == 0, (m[1] if m else ("CRASH" if p.returncode >= 128 else ""))


def judge_file(compiler: str, blocks: list[Block]) -> list[tuple[Block, bool, str]]:
    """Judge one page's blocks in order, growing the context of declarations."""
    context, out = "", []
    for b in blocks:
        if b.kind == "skip":
            continue
        ok, code = compile_text(compiler, b.program(imports=stdlib_imports(b.body)))
        used_context = ""
        if not ok and b.kind != "fails":
            ok2, code2 = compile_text(compiler, b.program(context, stdlib_imports(b.body + context)))
            if ok2:
                ok, code, used_context = True, "", context
            elif code in ("", "CRASH", "TIMEOUT"):
                pass
        out.append((b, ok, code))
        if ok and b.kind != "fails" and b.is_declaration():
            # Keep it as page context only if it still compiles after the context before it.
            body = b.body.rstrip("\n") + "\n\n"
            good, _ = compile_text(compiler, context + body + "fn main(): int {\n    return 0;\n}\n")
            if good:
                context += body
    return out


NAME_CODES = {"E040", "E041", "E042", "E043", "E008", "E009", "E032", "E031", "E044", "E056", "E066", "E067"}


def label(b: Block, code: str) -> str:
    """A guess at WHY a block does not compile, to order REL-006's work. The
    first match wins; it is advice, not a verdict."""
    body = b.body
    if re.search(r"//.*\b(ERROR|error|rejected|not allowed|illegal|invalid|does not compile|won't compile|fails?)\b", body):
        return "SHOWS-ERROR"
    if re.search(r"^\s*(\.\.\.|/\*\s*\.\.\.\s*\*/)\s*$|\{\s*\.\.\.\s*\}|<[a-z][a-z _-]*>", body, re.M):
        return "PLACEHOLDER"
    if code in NAME_CODES:
        return "NOT-SELF-CONTAINED"
    return "NEEDS-REVIEW"


def read_register() -> dict[str, str]:
    reg = {}
    if REGISTER.exists():
        for line in REGISTER.read_text().splitlines():
            if line.strip() and not line.startswith("#"):
                key, _, why = line.partition("  ")
                reg[key.strip()] = why.strip()
    return reg


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--compiler", default=os.environ.get("TURF_COMPILER", str(REPO / "build" / "turfc")))
    ap.add_argument("--update-register", action="store_true", help="rewrite the register from the current failures")
    ap.add_argument("-v", action="store_true")
    args = ap.parse_args()
    if not os.access(args.compiler, os.X_OK):
        print(f"doc_examples: compiler {args.compiler} not built", file=sys.stderr)
        return 1

    blocks = extract()
    by_file: dict[Path, list[Block]] = {}
    for b in blocks:
        by_file.setdefault(b.path, []).append(b)
    with ThreadPoolExecutor(max_workers=max(1, (os.cpu_count() or 2) // 2)) as ex:
        judged = [r for part in ex.map(lambda bs: judge_file(args.compiler, bs), by_file.values()) for r in part]
    todo = [b for b, _, _ in judged]
    results = [(ok, code) for _, ok, code in judged]

    register = read_register()
    problems, failing, ok = [], {}, 0
    for b, (compiled, code) in zip(todo, results):
        if b.kind == "fails":
            if compiled:
                problems.append(f"{b.key}: marked `fails {b.arg}` but it compiles")
            elif b.arg and code != b.arg:
                problems.append(f"{b.key}: marked `fails {b.arg}` but fails with {code or 'no code'}")
            else:
                ok += 1
            continue
        if compiled:
            ok += 1
            if b.key in register:
                problems.append(f"{b.key}: in the register but now compiles -- delete the line")
            continue
        failing[b.key] = code or "no-code"
        if b.key not in register and not args.update_register:
            problems.append(f"{b.key}: does not compile ({failing[b.key]}) and is not in the register "
                            f"-- fix the doc, or mark it `fails`/`skip`")

    if args.update_register:
        lines = ["# Reference-doc examples that do not compile (scripts/check/doc_examples.py). A to-do list for",
                 "# REL-006's rewrite of docs/reference/: it can only shrink. Key = file#ordinal hash, then",
                 "# the first error code the example produces. A changed block gets a new hash, so its line",
                 "# goes stale and must be re-triaged: fix the doc or mark the block."]
        by_key = {b.key: b for b in blocks}
        lines += [f"{k}  {v} {label(by_key[k], v)}" for k, v in sorted(failing.items())]
        REGISTER.write_text("\n".join(lines) + "\n")
        print(f"doc_examples: register rewritten ({len(failing)} failing blocks)")
        return 0

    for key in register:
        if key not in failing and not any(key in p for p in problems):
            ok_keys = {b.key for b in blocks}
            if key not in ok_keys:
                problems.append(f"{key}: in the register but no such block (doc edited) -- delete the line")

    skipped = sum(1 for b in blocks if b.kind == "skip")
    by_code = Counter(failing.values())
    print(f"doc_examples: {len(blocks)} blocks: {ok} compile or fail as marked, "
          f"{len(failing)} in the register, {skipped} skipped")
    if args.v and failing:
        print("  failing by first error code: " + ", ".join(f"{c} x{n}" for c, n in by_code.most_common(8)))
        labels = Counter(register.get(k, "").split(" ")[-1] for k in failing)
        print("  by label: " + ", ".join(f"{c} x{n}" for c, n in labels.most_common()))
    if problems:
        print("doc_examples: FAIL")
        for p in problems[:40]:
            print("  " + p)
        if len(problems) > 40:
            print(f"  ... and {len(problems) - 40} more")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
