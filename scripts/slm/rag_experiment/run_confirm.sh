#!/bin/zsh
# Confirmation run (see PREREGISTRATION.md). Resumable: rerun to continue; finished (id, arm) pairs are skipped.
# Everything is recorded under results/confirm-logs/:
#   00_environment.txt     git state, frozen-file hashes, tool/model versions, machine
#   master.log             timestamped record of every step in this driver
#   <run>.log              full stdout+stderr of run_rag.py (per-case lines, every attempt)
#   <run>.analyze.txt      analyze.py output
#   <run>.scratch/         llama-server logs and per-case work dirs (copied from scratch at the end)
# Per-attempt prompts and model outputs are in results/<run>.jsonl (--keep-prompts).
cd /Users/puang/turf/turf-code/turf-lang || exit 1
R=scripts/slm/rag_experiment
L=$R/results/confirm-logs
mkdir -p $L
SCR=${TMPDIR:-/tmp}/confirm_scratch
mkdir -p $SCR

M15=models/qwen2.5-coder-1.5b-instruct-q4_k_m.gguf
M3=/Users/puang/models/qwen2.5-coder-3b/qwen2.5-coder-3b-instruct-q4_k_m.gguf
M7=/Users/puang/models/qwen2.5-coder-7b/qwen2.5-coder-7b-instruct-q4_k_m.gguf
ARMS=A0_none,A1p_production,A3_examples,A7_examples_reretrieve

log() { echo "[$(date '+%F %T')] $*" | tee -a $L/master.log; }

# --- frozen-input check: refuse to run if anything differs from PREREGISTRATION.md
check() { # file expected_sha
  got=$(shasum -a 256 "$1" | awk '{print $1}')
  [[ $got == $2 ]] || { log "HASH MISMATCH $1: $got != $2"; exit 2; }
}
check $R/run_rag.py 29128875fca9087fc5b7ccfd9caeb50b38e54426d116863a64a8dbc8419cd51d
check $R/rag_lib.py ba1a8a5c7656db724dd937cea78d2a4e34336dd113145b191cb7fa5ad77e247a
check $R/analyze.py bb3b943f941c65c117731e69edff012658adec79f3420914cbdc7a17fa09f739
check $R/corpus/train.jsonl 1e42e5537be1dad7abb947bbd7d32c07c0c44f2472c2caf483266c7fe1cdeef6
check $R/corpus/test_fresh.jsonl e3eca1493c5502818eba4714bd4a2d9ce6aa39653f7ae513b4ac8e5cc24b5aef

# --- environment snapshot (appended on every start, so resumes are visible)
{
  echo "===== $(date '+%F %T') driver start ====="
  echo "git HEAD: $(git rev-parse HEAD)  branch: $(git rev-parse --abbrev-ref HEAD)"
  echo "git status (harness dir):"; git status --short $R
  echo "--- sha256 of frozen inputs"
  shasum -a 256 $R/run_rag.py $R/rag_lib.py $R/analyze.py $R/corpus/train.jsonl $R/corpus/test_fresh.jsonl $R/run_confirm.sh
  echo "--- models"
  shasum -a 256 $M15 $M3 $M7
  echo "--- tools"
  ls -la build/turfc; shasum -a 256 build/turfc
  /opt/homebrew/bin/llama-server --version 2>&1 | head -2
  python3 --version
  echo "--- machine"
  sw_vers; sysctl -n machdep.cpu.brand_string hw.memsize
  pmset -g batt 2>/dev/null | head -2
} >> $L/00_environment.txt 2>&1

# caffeinate keeps the Mac awake for the lifetime of this script
caffeinate -i -w $$ &

step() { name=$1; model=$2
  log "START $name model=$model"
  python3 -u $R/run_rag.py --run-name $name --split test_fresh --patch-format text --attempts 3 \
    --arms $ARMS --keep-prompts --model $model \
    --llama-server /opt/homebrew/bin/llama-server --gpu-layers 99 --threads 4 \
    --scratch $SCR/$name 2>&1 | while IFS= read -r line; do echo "[$(date '+%T')] $line"; done >> $L/$name.log
  rc=${pipestatus[1]}
  log "END run_rag $name rc=$rc"
  python3 $R/analyze.py $name > $L/$name.analyze.txt 2>&1
  log "analyzed $name rc=$?"
  mkdir -p $L/$name.scratch && cp -R $SCR/$name/. $L/$name.scratch/ 2>/dev/null
  return $rc
}

step confirm-1.5b $M15 || log "WARNING confirm-1.5b failed; continuing"
step confirm-3b   $M3  || log "WARNING confirm-3b failed; continuing"
step confirm-7b   $M7  || log "WARNING confirm-7b failed; continuing"
log "ALL DONE"
