#!/usr/bin/env python3
"""Build fixture/{train,test}.jsonl from the SLM benchmark corpus (smoke tests)."""
import difflib, json, os, re, subprocess, sys, tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import rag_lib as L

REPO = L.REPO_ROOT
TURFC = str(REPO / "build/turfc")
# Only benchmark cases whose source_test is a passing program with a one-line diff.
TEST = ["type_mismatch_int_to_string", "typo_undeclared_variable"]
# Same source_test as a test case on purpose: they must never be retrieved for it.
TRAIN = ["semicolon_var_decl", "type_mismatch_double_to_int"]
# Extra train entries: delete the first `;` of a statement line in other passing tests.
EXTRA_TRAIN_SOURCES = ["tests/01_syntax/03_pratt_postfix.tr", "tests/01_syntax/06_pratt_chaining.tr",
                       "tests/03_typechecker/02_logical_relational.tr", "tests/04_control_flow/02_if_elif_else.tr"]


def comp(src, tmp):
    f = Path(tmp) / "x.tr"; f.write_text(src)
    p = subprocess.run([TURFC, str(f), "-o", str(Path(tmp) / "x.bin")], capture_output=True, text=True, cwd=REPO)
    return p.returncode, p.stdout + p.stderr, Path(tmp) / "x.bin"


def main():
    os.environ.pop("TURF_HOME", None)
    corpus = {c["id"]: c for c in json.load(open(REPO / "scripts/slm/slm_benchmark/corpus.json"))}
    out = {"train": [], "test": []}
    with tempfile.TemporaryDirectory() as tmp:
        for split, ids in (("train", TRAIN), ("test", TEST)):
            items = [corpus[i] for i in ids]
            if split == "train":
                for src in EXTRA_TRAIN_SOURCES:
                    p = REPO / src
                    if not p.exists():
                        continue
                    ls = p.read_text().split("\n")
                    k = next((j for j, l in enumerate(ls) if l.startswith("    ") and l.rstrip().endswith(";") and "return" not in l), None)
                    if k is None:
                        continue
                    ls[k] = ls[k].rstrip()[:-1]
                    items.append({"id": "x_" + p.stem, "source_test": src, "kind": "delete_semicolon",
                                  "mutated_source": "\n".join(ls)})
            for c in items:
                cid = c["id"]
                orig = (REPO / c["source_test"]).read_text()
                ol, ml = orig.split("\n"), c["mutated_source"].split("\n")
                assert len(ol) == len(ml), cid
                diffs = [i for i in range(len(ol)) if ol[i] != ml[i]]
                assert len(diffs) == 1, (cid, diffs)
                i = diffs[0]
                rc, diag, _ = comp(c["mutated_source"], tmp)
                assert rc != 0, cid
                clean = L.strip_ansi(diag)
                m = re.search(r"at line (\d+), col \d+\s+\[(E\d+)", clean)
                msg = re.search(r"\]\s*\n\s*\n\s*(.+)", clean)
                rc0, _, exe = comp(orig, tmp)
                assert rc0 == 0, cid
                r = subprocess.run([str(exe)], capture_output=True, timeout=10)
                out[split].append({
                    "id": cid, "split": split, "source_test": c["source_test"], "kind": c["kind"],
                    "line": i + 1, "original_line": ol[i], "mutated_line": ml[i],
                    "mutated_source": c["mutated_source"], "diagnostic": diag,
                    "error_code": m.group(2) if m else "", "error_message": msg.group(1).strip() if msg else "",
                    "diag_line": int(m.group(1)) if m else i + 1,
                    "expected_stdout": r.stdout.decode(), "expected_exit": r.returncode})
    d = Path(__file__).resolve().parent / "fixture"
    for s, rows in out.items():
        (d / f"{s}.jsonl").write_text("".join(json.dumps(r) + "\n" for r in rows))
        print(s, len(rows))

main()
