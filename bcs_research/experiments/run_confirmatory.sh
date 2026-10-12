#!/usr/bin/env bash
# run_confirmatory.sh — the confirmatory batch (PREREGISTRATION signed off 2026-10-12, 6c46c01).
#
# Runs every confirmatory driver SERIALLY, each with ONE process pool of $WORKERS
# (default 8: the box has 11 cores and three concurrent pools made it crawl).
# Each driver checkpoints every run under results/experiments/checkpoints/, so
# after a crash, a usage-limit cut-off or a reboot just run this script again:
# finished drivers are skipped (their summary JSON carries the confirmatory
# status) and an interrupted one resumes where its checkpoint ends. It stops at
# the first failure and says which driver and which log.
#
#   WORKERS=8 nohup bcs_research/experiments/run_confirmatory.sh > confirm.out 2>&1 &
#   PYTHON=/path/to/python   overrides the interpreter (default: the repo's bcs_research/.venv)
#
# Estimated wall time at WORKERS=8, measured 2026-10-12 on the dev box (Apple
# Silicon, 11 cores) from a 1-seed smoke of each confirmatory mode at FULL
# horizon, scaled linearly to the pre-registered seed count (runs are
# independent, so wall time is linear in seeds). exp12's solver is not linear
# in seeds: its figure is per-run cost x the run counts of a reduced smoke.
#
#   driver  prediction(s)                  size                                     1-seed smoke   est. wall
#   exp9    P1  replication                40 seeds x 600 s, 12 cells + batch arm   20 s (2 seeds)  0.1 h (+0.1 h if all cells re-run at 80)
#   exp10   P4  speed + free entry         100 seeds x 1800 s, 23 cells               6 s           0.2 h
#   exp11   P9  batch semantics            100 seeds x 1800 s, 31 arms               44 s           1.2 h
#   exp15   P10, P10b, P10 tick secondary  59 configs x 100 seeds x 1800 s          320 s           8.9 h
#   exp17   P3  jump vs random walk        8,800 runs                                CONFIRM-B       1.5 h
#   exp19   P6  noise heterogeneity        7,400 runs                                CONFIRM-B       2.4 h
#   exp18   P5  fee schedules              18,100 runs                               CONFIRM-B       3.3 h
#   exp20   P7  maker strategies           12,800 runs                               CONFIRM-B       4.2 h
#   exp16   P2  race incidence             23,800 runs                               CONFIRM-B       4.3 h
#   exp12   P8  endogenous makers          17 Nash solves + 100 report seeds         see note       5-8 h  (2.8/5.1/7.1/11.1 s per run at M = 1/2/3/5)
#   exp13   P11 global sensitivity         N = 512 (P11's, the verdict of record)   938 s (N=16)   8.3 h
#   ------  every pre-registered verdict done                                                       ~41 h
#   exp13   EXTENSION to N = 2048           shares the N = 512 checkpoint                           +25 h  (reported beside, not of record)
#   exp14   incidence priors (no pred.)    2 x 4 x 5 x 10 cells x 100 seeds          810 s           22.5 h (descriptive)
#   total                                                                                           ~88 h
#
# Re-time if the engine or the box changes.

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"                      # bcs_research/
WORKERS="${WORKERS:-8}"
OUT="${OUT:-$ROOT/results/experiments}"          # override only to rehearse the script
LOGS="$OUT/logs"
mkdir -p "$LOGS"

if [[ -z "${PYTHON:-}" ]]; then
  PYTHON="$ROOT/.venv/bin/python"
  if [[ ! -x "$PYTHON" ]]; then                      # a git worktree: the venv lives in the main checkout
    common="$(git -C "$ROOT" rev-parse --path-format=absolute --git-common-dir 2>/dev/null)"
    PYTHON="$(dirname "$common")/bcs_research/.venv/bin/python"
  fi
fi
if [[ ! -x "$PYTHON" ]]; then
  echo "run_confirmatory: no python at '$PYTHON'; set PYTHON=/path/to/bcs_research/.venv/bin/python" >&2
  exit 2
fi

run() {   # run <name> <script> [args...]: skip if done, else run with a log, stop on failure
  local name="$1" script="$2"; shift 2
  local log="$LOGS/$name.log" summary="$OUT/$name.json"
  if [[ -f "$summary" ]] && grep -q '"status": "confirmatory' "$summary"; then
    echo "[$(date '+%F %T')] $name: already done ($summary), skipping"
    return 0
  fi
  echo "[$(date '+%F %T')] $name: starting (log $log)"
  local t0=$SECONDS
  if ! (cd "$ROOT" && "$PYTHON" "experiments/$script" --confirmatory --workers "$WORKERS" --out-dir "$OUT" "$@") \
      >> "$log" 2>&1; then
    echo "[$(date '+%F %T')] $name: FAILED after $(( SECONDS - t0 )) s — see $log." >&2
    echo "Fix the cause and rerun this script; finished runs are kept in $OUT/checkpoints/." >&2
    exit 1
  fi
  echo "[$(date '+%F %T')] $name: done in $(( (SECONDS - t0) / 60 )) min -> $summary"
}

# Every pre-registered verdict first, shortest first, so a batch stopped early
# has as many complete experiments as possible.
run exp9_bcs_replication       exp9_bcs_replication.py
run exp10_speed_entry          exp10_speed_entry.py
run exp11_batch_semantics      exp11_batch_semantics.py
run exp17_jump_vs_walk         exp17_jump_vs_walk.py
run exp19_noise_heterogeneity  exp19_noise_heterogeneity.py
run exp18_fee_schedules        exp18_fee_schedules.py
run exp20_maker_strategies     exp20_maker_strategies.py
run exp16_race_incidence       exp16_race_incidence.py
run exp12_endogenous_makers    exp12_endogenous_makers.py
run exp13_global_sensitivity   exp13_global_sensitivity.py
run exp15_heldout_who_pays     exp15_heldout_who_pays.py
echo "[$(date '+%F %T')] every pre-registered verdict is done; extras follow"

# Extras, not of record: P11 at N = 2,048 (reuses the N = 512 runs) and the
# descriptive incidence priors.
run exp13_global_sensitivity_n2048 exp13_global_sensitivity.py --n 2048
run exp14_incidence_priors     exp14_incidence_priors.py

echo "[$(date '+%F %T')] confirmatory batch complete; summaries in $OUT"
