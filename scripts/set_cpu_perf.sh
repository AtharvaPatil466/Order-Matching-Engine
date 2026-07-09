#!/usr/bin/env bash
#
# set_cpu_perf.sh — pin CPUs to max frequency and forbid deep C-states, so the
# first order after an idle gap doesn't pay a C-state wakeup + frequency ramp.
# See docs/OSTuning.md §5 (P2-16). Run BEFORE starting the engine.
#
# This covers only the SOFTWARE parts (governor + cpuidle). On on-prem / colo
# hardware, deep package C-states may also need disabling in BIOS/UEFI — the OS
# knobs below can be overridden by firmware (docs/OSTuning.md §5).
#
# TARGET: x86_64 Linux (AWS c6in.metal). Uses cpupower — Linux-only.

set -euo pipefail

# Default: disable every C-state with an exit latency slower than this (µs).
# `cpupower idle-set -D 1` keeps only C0/C1 (sub-µs exit) enabled.
MAX_CSTATE_LATENCY_US="${MAX_CSTATE_LATENCY_US:-1}"
GOVERNOR="${GOVERNOR:-performance}"

usage() {
    cat <<EOF
Usage: sudo set_cpu_perf.sh [--governor NAME] [--cstate-latency US]

  --governor NAME      cpufreq governor to set on all cores (default: performance)
  --cstate-latency US  disable all C-states with exit latency > US microseconds
                       (default: 1 — keeps only C0/C1). Set 0 to skip C-state changes.

Env overrides: GOVERNOR=..., MAX_CSTATE_LATENCY_US=...

Applies (docs/OSTuning.md §5):
  cpupower frequency-set -g <governor>
  cpupower idle-set -D <cstate-latency>
EOF
}

warn() { printf '\033[33mWARN: %s\033[0m\n' "$*"; }
die()  { printf '\033[31mFATAL: %s\033[0m\n' "$*"; exit 1; }

# ─── Args ───────────────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --governor)       GOVERNOR="${2:?--governor needs a value}"; shift 2 ;;
        --cstate-latency) MAX_CSTATE_LATENCY_US="${2:?--cstate-latency needs a value}"; shift 2 ;;
        -h|--help)        usage; exit 0 ;;
        *)                usage; die "unknown argument: $1" ;;
    esac
done

# ─── Preflight ──────────────────────────────────────────────────────────────
[[ "$(uname -s)" == "Linux" ]] || die "not Linux — cpupower is Linux-only (AWS c6in.metal)."
[[ "${EUID}" -eq 0 ]] || die "must run as root (cpufreq/cpuidle sysfs). Re-run with: sudo $0"
command -v cpupower >/dev/null 2>&1 \
    || die "cpupower not found. Install: apt-get install -y linux-tools-common linux-tools-\$(uname -r)"

# ─── Frequency governor ─────────────────────────────────────────────────────
echo "Setting cpufreq governor -> $GOVERNOR ..."
if cpupower frequency-set -g "$GOVERNOR" >/dev/null; then
    echo "  governor now: $(cpupower frequency-info 2>/dev/null | awk -F'\"' '/governor/{print $2; exit}')"
else
    warn "could not set governor '$GOVERNOR' (no cpufreq driver? governor unsupported?)."
fi

# ─── Deep C-states ──────────────────────────────────────────────────────────
if [[ "$MAX_CSTATE_LATENCY_US" == "0" ]]; then
    echo "Skipping C-state changes (--cstate-latency 0)."
else
    echo "Disabling C-states with exit latency > ${MAX_CSTATE_LATENCY_US} µs (keeps C0/C1) ..."
    if cpupower idle-set -D "$MAX_CSTATE_LATENCY_US" >/dev/null; then
        cpupower idle-info 2>/dev/null | grep -iE 'name|disable' | sed 's/^/  /' || true
    else
        warn "cpupower idle-set failed — cpuidle may be off, or set deep C-states in BIOS (docs/OSTuning.md §5)."
    fi
fi

echo "Done — verify with: cpupower frequency-info | grep -i governor ; cpupower idle-info"
