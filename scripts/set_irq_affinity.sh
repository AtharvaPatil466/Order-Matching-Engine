#!/usr/bin/env bash
#
# set_irq_affinity.sh — pin all IRQs of a NIC to a housekeeping core mask so
# hardware interrupts (and their softirq bottom halves) stop landing on the
# isolated matching / DPDK-poll cores. See docs/OSTuning.md §3 (P2-13).
#
# Run this BEFORE starting the engine, and again after any NIC reset, driver
# reload, or `ethtool -L` queue-count change (those re-create the IRQs).
#
# Disable irqbalance first, or it will move the IRQs back:
#   sudo systemctl stop irqbalance && sudo systemctl disable irqbalance
#
# TARGET: x86_64 Linux (AWS c6in.metal). Reads /proc/interrupts + /proc/irq —
# Linux-only, does not exist on macOS.

set -euo pipefail

usage() {
    cat <<'EOF'
Usage: sudo set_irq_affinity.sh <nic_dev> <housekeeping_mask>

  <nic_dev>            NIC kernel name whose IRQs to pin (e.g. ens1).
                       Detect it — do NOT assume eth0 on c6in.metal.
  <housekeeping_mask>  Hex CPU bitmask of the NON-isolated core(s) that should
                       absorb the IRQs (e.g. 0x1 = core 0, 0x3 = cores 0-1).

Example:
  sudo set_irq_affinity.sh ens1 0x1     # pin all ens1 IRQs to core 0

Verify afterwards:
  grep ens1 /proc/interrupts | awk -F: '{print $1}'
  cat /proc/irq/<N>/smp_affinity        # should equal the mask
EOF
}

warn() { printf '\033[33mWARN: %s\033[0m\n' "$*"; }
die()  { printf '\033[31mFATAL: %s\033[0m\n' "$*"; exit 1; }

# ─── Args + preflight ───────────────────────────────────────────────────────
if [[ $# -ne 2 ]]; then usage; exit 2; fi
NIC="$1"
MASK="$2"

[[ "$(uname -s)" == "Linux" ]] || die "not Linux — /proc/irq affinity is Linux-only (AWS c6in.metal)."
[[ "${EUID}" -eq 0 ]] || die "must run as root (writing /proc/irq/*/smp_affinity). Re-run with: sudo $0 $NIC $MASK"
ip link show "$NIC" >/dev/null 2>&1 || die "NIC '$NIC' not found (ip link show). Pass the correct device name."

# irqbalance would silently undo this — warn (non-fatal) if it's still active.
if command -v systemctl >/dev/null 2>&1 && systemctl is-active --quiet irqbalance 2>/dev/null; then
    warn "irqbalance is active — it will move these IRQs back. Stop+disable it: systemctl disable --now irqbalance"
fi

# Collect the NIC's IRQ numbers (strip the leading whitespace awk leaves in $1).
mapfile -t IRQS < <(grep -w "$NIC" /proc/interrupts | awk -F: '{gsub(/ /,"",$1); print $1}')
[[ "${#IRQS[@]}" -gt 0 ]] || die "no IRQs found for '$NIC' in /proc/interrupts (does the driver expose named IRQs?)."

echo "Pinning ${#IRQS[@]} IRQ(s) for '$NIC' to mask $MASK ..."

# ─── Pin + verify each IRQ ──────────────────────────────────────────────────
# Managed IRQs (AWS ena driver) reject affinity writes with EIO — that is
# expected; treat it as a warning and keep going. For those, the effective
# lever is the boot-time `irqaffinity=<mask>` param (docs/OSTuning.md §2/§3).
FAILED=0
for irq in "${IRQS[@]}"; do
    aff="/proc/irq/${irq}/smp_affinity"
    [[ -w "$aff" ]] || { warn "IRQ $irq: $aff not writable — skipping"; FAILED=$((FAILED+1)); continue; }
    if echo "$MASK" > "$aff" 2>/dev/null; then
        printf '  IRQ %-5s -> %s\n' "$irq" "$(cat "$aff")"   # verification echo
    else
        warn "IRQ $irq: write failed (managed IRQ? use irqaffinity= boot param — see docs/OSTuning.md §3)"
        FAILED=$((FAILED+1))
    fi
done

if [[ "$FAILED" -gt 0 ]]; then
    warn "$FAILED of ${#IRQS[@]} IRQ(s) could not be pinned at runtime — see docs/OSTuning.md §3 (managed IRQs)."
else
    echo "Done — all ${#IRQS[@]} '$NIC' IRQ(s) pinned to $MASK."
fi
