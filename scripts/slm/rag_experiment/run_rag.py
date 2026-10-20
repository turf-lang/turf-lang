#!/usr/bin/env python3
"""Run the RAG experiment: for each (case, arm) ask the local model for a
one-line fix, apply it, compile, run, score, append to results/<run>.jsonl.
Resumable; arms are interleaved per case. See README.md."""
from __future__ import annotations

import argparse
import atexit
import hashlib
import json
import os
import platform
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rag_lib as L  # noqa: E402

HERE = Path(__file__).resolve().parent
REPO = L.REPO_ROOT


# --split -> file in the corpus dir. test_fresh is the held-out set built after the
# experiment design was fixed; it is only opened when a run asks for it.
SPLIT_FILES = {"test": "test.jsonl", "test_fresh": "test_fresh.jsonl"}


def load_jsonl(p: Path) -> list[dict]:
    return [json.loads(x) for x in p.read_text().splitlines() if x.strip()]


def sha256(p: Path) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 22), b""):
            h.update(b)
    return h.hexdigest()


def start_server(a) -> tuple[subprocess.Popen | None, str]:
    if a.server_url:
        return None, a.server_url
    port = a.port
    cmd = [a.llama_server, "-m", a.model, "-t", str(a.threads), "-c", "4096", "-np", "1",
           "--temp", "0", "--seed", "0", "--port", str(port), "--host", "127.0.0.1",
           "-ngl", str(a.gpu_layers)]
    log = open(Path(a.scratch) / "llama-server.log", "w")
    proc = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT)
    atexit.register(lambda: proc.poll() is None and (proc.terminate(), proc.wait(30)))
    url = f"http://127.0.0.1:{port}"
    for _ in range(600):
        if proc.poll() is not None:
            sys.exit(f"llama-server exited early, see {log.name}")
        try:
            with urllib.request.urlopen(url + "/health", timeout=2) as r:
                if json.loads(r.read()).get("status") == "ok":
                    return proc, url
        except Exception:
            pass
        time.sleep(1)
    sys.exit("llama-server did not become healthy")


def run(cmd, timeout, cwd=None):
    try:
        p = subprocess.run(cmd, capture_output=True, timeout=timeout, cwd=cwd)
        return (p.returncode,
                p.stderr.decode("utf-8", "replace") + p.stdout.decode("utf-8", "replace"), False)
    except subprocess.TimeoutExpired:
        return -1, "", True


def score(a, case: dict, out: dict, scratch: Path) -> dict:
    res = dict(parsed=False, abstained=False, right_line=False, applied=False,
               compiles=False, correct=False, exact=False, line=None, new_code=None)
    # retry bookkeeping, popped by the caller: why the patch was (not) rejected and what to tell the model
    res["_reason"], res["_feedback"] = "unparsed", None
    try:
        obj = json.loads(out["raw"])
        line, nc = obj["line"], obj["new_code"]
        assert isinstance(line, int) and not isinstance(line, bool) and isinstance(nc, str)
    except Exception:
        return res
    res.update(parsed=True, line=line, new_code=nc)
    if line == 0:
        res["abstained"] = True
        res["_reason"] = "abstained"
        return res
    res["right_line"] = line == case["line"]
    res["exact"] = nc.strip() == case["original_line"].strip()
    src = case["mutated_source"].split("\n")
    if "\n" in nc or "\r" in nc or not (1 <= line <= len(src)):
        res["_reason"] = "not_applied"
        res["_feedback"] = L.msg_bad_line("new_code must be a single line" if ("\n" in nc or "\r" in nc)
                                          else f"line {line} is not a line of the program")
        return res
    before = list(src)
    src[line - 1] = nc
    res["applied"] = True
    f = scratch / f"{case['id']}__{out['arm']}.tr"
    exe = f.with_suffix(".bin")
    f.write_text("\n".join(src))
    rc, cout, _ = run([a.turfc, str(f), "-o", str(exe)], 120, cwd=REPO)
    res["compiles"] = rc == 0 and exe.exists()
    if not res["compiles"]:
        res["_reason"] = "compile_error"
        res["_diag"] = cout
        if not nc.strip():
            res["_reason"], res["_feedback"] = "empty_new_code", L.MSG_EMPTY_NEW_CODE
        elif L.same_code(nc, before[line - 1]):
            res["_reason"], res["_feedback"] = "unchanged", L.MSG_UNCHANGED_LINE
        else:
            res["_feedback"] = L.compile_feedback(case, cout, str(f), src, line, line, 1, False, before)
    else:
        res["_reason"] = "compiles_wrong"
    if res["compiles"]:
        try:
            p = subprocess.run([str(exe)], capture_output=True, timeout=10)
            res["correct"] = (p.returncode == case["expected_exit"] and
                              p.stdout.decode("utf-8", "replace") == case["expected_stdout"])
        except subprocess.TimeoutExpired:
            pass
        if res["correct"]:
            res["_reason"] = "correct"
    for x in (f, exe):
        try:
            x.unlink()
        except OSError:
            pass
    return res


def score_text(a, case: dict, out: dict, scratch: Path) -> dict:
    """Text-matching format (production's): find `original_code` in the program,
    splice `new_code` over it. Candidate places are tried best first and the
    first patch that compiles is the one scored, as the --smart sandbox accepts
    the first candidate that clears the error. `correct_top` scores only the
    best-ranked place."""
    res = dict(parsed=False, abstained=False, located=False, right_line=False, applied=False,
               compiles=False, correct=False, correct_top=False, exact=False, unchanged=False, n_cands=0,
               loc_how=None, original_code=None, new_code=None)
    res["_reason"], res["_feedback"] = "unparsed", None
    try:
        obj = json.loads(out["raw"])
        oc, nc = obj["original_code"], obj["new_code"]
        assert isinstance(oc, str) and isinstance(nc, str)
    except Exception:
        return res
    res.update(parsed=True, original_code=oc, new_code=nc)
    oc, nc = L.trim_blank_edge_lines(oc), L.trim_blank_edge_lines(nc)
    if not oc.strip() and not nc.strip():
        res["abstained"] = True
        res["_reason"] = "abstained"
        return res
    res["exact"] = nc.strip() == case["original_line"].strip()
    if not nc.strip() or (oc.strip() and L.same_code(oc, nc)):
        res["unchanged"] = True  # production rejects an empty new_code and an unchanged one
        if not nc.strip():
            res["_reason"], res["_feedback"] = "empty_new_code", L.MSG_EMPTY_NEW_CODE
        else:
            res["_reason"], res["_feedback"] = "unchanged", L.MSG_UNCHANGED
        return res
    src = case["mutated_source"].rstrip("\n").split("\n")
    err = case.get("diag_line") or case.get("line")
    cands = L.locate_original(case["mutated_source"], oc, err)
    res["n_cands"] = len(cands)
    if not cands:
        ls = src[err - 1] if err and 1 <= err <= len(src) else ""
        res["_reason"], res["_feedback"] = "not_found", L.msg_not_found(ls)
        return res
    res["located"] = True
    res["loc_how"] = cands[0]["how"]
    res["right_line"] = cands[0]["start"] <= case["line"] <= cands[0]["end"]
    exe_keep = []
    for ci, c in enumerate(cands):
        s0, e0 = c["start"], c["end"]
        if c["how"] == "insertion":
            patched = src[:s0] + L.reindent_new_code(nc, oc, src[s0 - 1]).split("\n") + src[s0:]
        else:
            patched = src[: s0 - 1] + L.reindent_new_code(nc, oc, src[s0 - 1]).split("\n") + src[e0:]
        res["applied"] = True
        f = scratch / f"{case['id']}__{out['arm']}__{ci}.tr"
        exe = f.with_suffix(".bin")
        f.write_text("\n".join(patched) + "\n")
        rc, cout, _ = run([a.turfc, str(f), "-o", str(exe)], 120, cwd=REPO)
        ok = rc == 0 and exe.exists()
        good = False
        if ci == 0 and not ok:  # production feeds back the best-placed candidate's diagnostics
            n_new = len(patched) - (len(src) - (e0 - s0 + 1)) if c["how"] != "insertion" else len(patched) - len(src) + 1
            res["_diag"] = cout
            res["_feedback"] = L.compile_feedback(case, cout, str(f), patched, s0, e0, n_new,
                                                  c["how"] == "fuzzy", src)
        if ok:
            try:
                p = subprocess.run([str(exe)], capture_output=True, timeout=10)
                good = (p.returncode == case["expected_exit"] and
                        p.stdout.decode("utf-8", "replace") == case["expected_stdout"])
            except subprocess.TimeoutExpired:
                pass
        for x in (f, exe):
            try:
                x.unlink()
            except OSError:
                pass
        if ci == 0:
            res["correct_top"] = good
        if ok:
            res["compiles"], res["correct"] = True, good
            res["right_line"] = c["start"] <= case["line"] <= c["end"]
            break
    res["_reason"] = "correct" if res["correct"] else ("compiles_wrong" if res["compiles"] else "compile_error")
    if res["compiles"]:
        res["_feedback"] = None
    return res


RETRY_REASONS = ("compile_error", "empty_new_code", "unchanged", "not_found", "not_applied")


def solve(a, server, retr, case: dict, arm: str, scratch: Path) -> dict | None:
    """One (case, arm): up to `--attempts` model calls. Attempt k+1 happens only
    when attempt k produced a patch production's sandbox would reject (see
    RETRY_REASONS); abstaining, unparseable output and a patch that compiles
    (right or wrong) end the case. The top-level fields are the final attempt's."""
    text_fmt = a.patch_format == "text"
    build = L.build_text_user_prompt if text_fmt else L.build_user_prompt
    scorer = score_text if text_fmt else score
    attempts: list[dict] = []
    feedback = None
    ctx = None
    for k in range(1, a.attempts + 1):
        if k == 1:
            ctx, rinfo = retr.context(arm, case)
        elif arm == "A7_examples_reretrieve":
            new = L.parse_diag(prev_diag) if prev_diag else None
            code, msg = new if new else (case.get("error_code", ""), case.get("error_message", ""))
            ctx, rinfo = retr.examples_reretrieve(case, code, msg)
            rinfo = {**rinfo, "query_error_code": code, "query_error_message": msg}
        ctx_tokens = server.count(ctx) if ctx else 0
        assert ctx_tokens <= a.budget, (arm, ctx_tokens)
        user, trunc = build(server, case, ctx, feedback)
        if a.dump:
            print(f"=== {case['id']} / {arm} attempt {k} ({ctx_tokens} ctx tokens) {rinfo}\n{ctx}\n")
            if not feedback:
                return None
        t0 = time.monotonic()
        resp = server.chat(L.TEXT_SYSTEM_PROMPT if text_fmt else L.SYSTEM_PROMPT, user,
                           L.TEXT_FIX_SCHEMA if text_fmt else L.FIX_SCHEMA,
                           max_tokens=300 if text_fmt else 200, cache_prompt=a.cache_prompt)
        dt = time.monotonic() - t0
        raw = resp["choices"][0]["message"]["content"]
        rec = {"id": case["id"], "arm": arm, "kind": case.get("kind"),
               "error_code": case.get("error_code"), "split": case["split"],
               "prompt_tokens": resp["usage"]["prompt_tokens"],
               "ctx_tokens": ctx_tokens, "latency_s": round(dt, 3),
               "program_truncated": trunc, "retrieved": rinfo, "raw": raw}
        res = scorer(a, case, {"raw": raw, "arm": arm}, scratch)
        reason, fb = res.pop("_reason"), res.pop("_feedback")
        prev_diag = res.pop("_diag", None)
        rec.update(res)
        attempts.append({**rec, "attempt": k, "reason": reason, "feedback_given": feedback,
                         "user_prompt": user if a.keep_prompts else None})
        if reason not in RETRY_REASONS or k == a.attempts:
            break
        feedback = fb
    if a.attempts > 1:
        for at in attempts:
            for key in ("id", "arm", "kind", "error_code", "split"):
                at.pop(key, None)
            if at["user_prompt"] is None:
                del at["user_prompt"]
        rec["attempts"] = attempts
        rec["n_calls"] = len(attempts)
        rec["correct_at"] = next((x["attempt"] for x in attempts if x["correct"]), None)
        rec["compiled_at"] = next((x["attempt"] for x in attempts if x["compiles"]), None)
        rec["latency_total_s"] = round(sum(x["latency_s"] for x in attempts), 3)
        rec["final_reason"] = attempts[-1]["reason"]
    return rec


def selftest(server) -> None:
    """The leakage assertion must fire on crafted bad cases."""
    case = {"id": "q", "source_test": "t/a.tr", "split": "test"}
    good = {"id": "g", "source_test": "t/b.tr", "split": "train"}
    L.assert_no_leak(case, [good])
    for bad in ({**good, "source_test": "t/a.tr"}, {**good, "split": "test"}, {**good, "id": "q"}):
        try:
            L.assert_no_leak(case, [bad])
        except AssertionError as e:
            print("leak assertion fired:", e)
        else:
            sys.exit("SELFTEST FAILED: bad example was not rejected")
    print("selftest ok")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus-dir", default=str(HERE / "corpus"))
    ap.add_argument("--split", default="test")
    ap.add_argument("--arms", default=",".join(L.ARMS))
    ap.add_argument("--limit", type=int)
    ap.add_argument("--run-name", default="run")
    ap.add_argument("--results-dir", default=str(HERE / "results"))
    ap.add_argument("--server-url")
    ap.add_argument("--llama-server", default=str(REPO / "external/llama.cpp/build/bin/llama-server"))
    ap.add_argument("--model", default=str(REPO / "models/qwen2.5-coder-1.5b-instruct-q4_k_m.gguf"))
    ap.add_argument("--turfc", default=str(REPO / "build/turfc"))
    ap.add_argument("--threads", type=int, default=8)
    ap.add_argument("--gpu-layers", type=int, default=0)
    ap.add_argument("--port", type=int, default=8089)
    ap.add_argument("--budget", type=int, default=L.DEFAULT_BUDGET)
    ap.add_argument("--scratch", default=os.path.join(tempfile.gettempdir(), "rag_runs"))
    ap.add_argument("--cache-prompt", action="store_true",
                    help="reuse the KV cache of the shared prompt prefix (faster, tiny numeric drift)")
    ap.add_argument("--patch-format", choices=("line", "text"), default="line",
                    help="line: the model names a line number; text: it copies original_code and "
                         "the harness finds it (what production --smart does)")
    ap.add_argument("--attempts", type=int, default=1,
                    help="model calls per (case, arm); a retry happens only when production's sandbox "
                         "would reject the patch, with the rejection fed back (default 1: no retry)")
    ap.add_argument("--keep-prompts", action="store_true",
                    help="store each attempt's full user prompt in the result row (large)")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--dump", action="store_true", help="print each prompt's context and exit after --limit cases without scoring")
    a = ap.parse_args()
    os.environ.pop("TURF_HOME", None)
    arms = a.arms.split(",")
    for x in arms:
        assert x in L.ARMS, f"unknown arm {x}"
    scratch = Path(a.scratch)
    scratch.mkdir(parents=True, exist_ok=True)

    cdir = Path(a.corpus_dir)
    train = load_jsonl(cdir / "train.jsonl")
    assert a.attempts >= 1
    cases = load_jsonl(cdir / SPLIT_FILES.get(a.split, f"{a.split}.jsonl"))
    assert all(c["split"] == a.split for c in cases)
    if a.limit:
        cases = cases[: a.limit]

    proc, url = start_server(a)
    server = L.Server(url)
    try:
        if a.selftest:
            selftest(server)
            return 0
        retr = L.Retriever(server, train, a.budget)
        rdir = Path(a.results_dir)
        rdir.mkdir(parents=True, exist_ok=True)
        rfile = rdir / f"{a.run_name}.jsonl"
        meta = rdir / f"{a.run_name}.meta.json"
        done = set()
        if rfile.exists():
            done = {(r["id"], r["arm"]) for r in load_jsonl(rfile)}
        info = {"platform": platform.platform(), "machine": platform.machine(),
                "python": platform.python_version(),
                "model": os.path.basename(a.model),
                "model_sha256": sha256(Path(a.model)) if Path(a.model).exists() else None,
                "llama_server_version": None, "gpu_layers": a.gpu_layers,
                "threads": a.threads, "budget": a.budget, "arms": arms,
                "patch_format": a.patch_format, "attempts": a.attempts, "split": a.split}
        try:
            v = subprocess.run([a.llama_server, "--version"], capture_output=True, text=True, timeout=30)
            info["llama_server_version"] = (v.stdout + v.stderr).strip().split("\n")[0]
        except Exception:
            pass
        if a.server_url:
            info["server_url"] = a.server_url
        if meta.exists():
            old = json.loads(meta.read_text())
            keys = ("platform", "machine", "model_sha256", "llama_server_version", "patch_format", "attempts")
            old.setdefault("patch_format", "line")
            old.setdefault("attempts", 1)
            diff = [k for k in keys if old.get(k) != info.get(k)]
            if diff:
                sys.exit(f"run {a.run_name} was started with a different {diff}; use a new --run-name")
        else:
            meta.write_text(json.dumps(info, indent=1))

        total = len(cases) * len(arms)
        n = 0
        with open(rfile, "a") as out:
            for case in cases:
                for arm in arms:
                    n += 1
                    if (case["id"], arm) in done:
                        continue
                    rec = solve(a, server, retr, case, arm, scratch)
                    if rec is None:  # --dump
                        continue
                    out.write(json.dumps(rec) + "\n")
                    out.flush()
                    extra = f" calls={rec['n_calls']}" if "attempts" in rec else ""
                    print(f"[{n}/{total}] {case['id']:<32} {arm:<20} {rec['latency_s']:6.1f}s "
                          f"ctx={rec['ctx_tokens']:<5} correct={rec['correct']} compiles={rec['compiles']}{extra}",
                          flush=True)
    finally:
        if proc and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(30)
            except subprocess.TimeoutExpired:
                proc.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
