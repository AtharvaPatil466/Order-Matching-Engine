#!/usr/bin/env bash
#
# wire_latency_aws.sh — two-instance wire-to-wire latency measurement on AWS
# c6in.metal (baseline kernel TCP, no DPDK). Select a role via the ROLE env var:
#
#   ROLE=receiver bash scripts/wire_latency_aws.sh          # Instance B
#   RECEIVER_IP=<B-eth1-ip> ROLE=sender bash scripts/wire_latency_aws.sh   # Instance A
#
# Both instances run WireLatencyReceiver/Sender over the SECONDARY ENI (eth1);
# the primary interface stays kernel-managed so SSH is not lost. This is the
# BASELINE kernel-TCP path (ENABLE_DPDK=OFF). DPDK/F-Stack setup is a separate
# scripts/dpdk_aws.sh for Session 2 — deliberately not implemented here.
#
# On Linux with a HW-timestamp-capable ENA + kernel, WireLatency* use
# SO_TIMESTAMPING hardware timestamps; otherwise they fall back to
# CLOCK_MONOTONIC software timestamps (each CSV row carries a `hardware` flag).

set -euo pipefail   # abort on first error

# ─── Paths, run id, logging ─────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

ROLE="${ROLE:-}"
PORT="${PORT:-9001}"
ORDERS="${ORDERS:-10000}"
ENI="${ENI:-eth1}"
NPROC="$(nproc)"
BUILD_DIR="build-wire-latency"
RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)"

case "$ROLE" in
    sender)   LOG="$REPO_ROOT/wire-sender-$RUN_ID.log" ;;
    receiver) LOG="$REPO_ROOT/wire-receiver-$RUN_ID.log" ;;
    *) echo "FATAL: set ROLE=sender or ROLE=receiver" >&2; exit 1 ;;
esac

# Tee all stdout+stderr to the timestamped log AND the terminal.
exec > >(tee -a "$LOG") 2>&1

banner() { printf '\n\033[1m======================================================================\n%s\n======================================================================\033[0m\n' "$*"; }
warn()   { printf '\033[33mWARN: %s\033[0m\n' "$*"; }
die()    { printf '\033[31mFATAL: %s\033[0m\n' "$*"; exit 1; }

# ─── Shared checks ──────────────────────────────────────────────────────────

# Secondary ENI must be present — both roles use it for the measurement path.
check_secondary_eni() {
    banner "CHECK — secondary ENI ($ENI)"
    if ! ip link show "$ENI" >/dev/null 2>&1; then
        die "secondary ENI '$ENI' not found. Attach a second ENI to this instance (AWS console / aws ec2 attach-network-interface) and bring it up before running. Override the name with ENI=<iface>."
    fi
    ip -o link show "$ENI"
    local ip
    ip="$(ip -4 -o addr show "$ENI" 2>/dev/null | awk '{print $4}' | cut -d/ -f1)"
    [[ -n "$ip" ]] || die "secondary ENI '$ENI' has no IPv4 address. Assign one (e.g. via DHCP / 'ip addr add') before running."
    echo "  $ENI IPv4: $ip"
    ENI_IP="$ip"
}

# ENA hardware SO_TIMESTAMPING support landed in the ~4.13 era. Warn (do not
# abort) below that — the tools fall back to software timestamps regardless.
check_kernel_timestamping() {
    banner "CHECK — kernel SO_TIMESTAMPING support"
    local krel kmaj kmin rest
    krel="$(uname -r)"
    echo "  kernel: $krel"
    kmaj="${krel%%.*}"
    rest="${krel#*.}"
    kmin="${rest%%.*}"
    kmaj="${kmaj//[!0-9]/}"; kmin="${kmin//[!0-9]/}"
    if [[ -z "$kmaj" || -z "$kmin" ]]; then
        warn "could not parse kernel version '$krel' — cannot verify SO_TIMESTAMPING support"
        return 0
    fi
    if (( kmaj < 4 || (kmaj == 4 && kmin < 13) )); then
        warn "kernel $krel < 4.13 — ENA hardware SO_TIMESTAMPING may be unavailable; the tools will fall back to CLOCK_MONOTONIC software timestamps (the 'hardware' CSV column will read 0)."
    else
        echo "  kernel >= 4.13 — ENA hardware SO_TIMESTAMPING supported"
    fi
}

# Hugepages are needed by the Session-2 DPDK path, not by this baseline. Checked
# here as early prep so the operator can configure them before Session 2. WARN
# only (do not auto-configure: needs root and possibly a reboot; do not abort:
# the baseline kernel-TCP run does not use hugepages).
check_hugepages() {
    banner "CHECK — hugepages (for the later DPDK session; baseline does not use them)"
    local nr
    nr="$(cat /proc/sys/vm/nr_hugepages 2>/dev/null || echo 0)"
    echo "  /proc/sys/vm/nr_hugepages = $nr"
    if (( nr < 1024 )); then
        warn "fewer than 1024 hugepages configured. NOT auto-configuring (requires root; a reboot may be needed). Before the DPDK session, run as root:"
        echo "      echo 1024 > /proc/sys/vm/nr_hugepages          # transient"
        echo "      # or persist: add 'vm.nr_hugepages=1024' to /etc/sysctl.conf"
        echo "      # then mount hugetlbfs if not already: mkdir -p /mnt/huge && mount -t hugetlbfs nodev /mnt/huge"
    else
        echo "  hugepages OK (>= 1024)"
    fi
}

# ─── Shared build ───────────────────────────────────────────────────────────
# Clang forced (same as aws_benchmark.sh); -Wno-error=character-conversion is the
# same workaround. BUILD_WIRE_LATENCY_TOOLS=ON builds the harness; ENABLE_DPDK=OFF
# keeps this the baseline kernel-TCP path.
build_wire_tools() {
    banner "BUILD — wire-latency tools (Clang, BUILD_WIRE_LATENCY_TOOLS=ON, ENABLE_DPDK=OFF)"
    command -v clang++ >/dev/null 2>&1 || die "clang++ not found — install clang (apt-get install -y clang)."
    command -v cmake   >/dev/null 2>&1 || die "cmake not found — install cmake."
    echo "  clang: $(clang++ --version | head -1)"
    cmake -S . -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang \
        -DCMAKE_CXX_FLAGS="-Wno-error=character-conversion" \
        -DBUILD_TOOLS=ON -DBUILD_WIRE_LATENCY_TOOLS=ON -DENABLE_DPDK=OFF
    cmake --build "$BUILD_DIR" --parallel "$NPROC" \
        --target WireLatencySender WireLatencyReceiver
}

# ─── Sender summary: min/P50/P90/P99/max of one-way estimate (CSV col 5) ─────
summarize_oneway() {
    local log="$1" vals
    # Data rows are "seq,tx_ns,rx_ns,rtt_ns,oneway_ns,hardware"; skip banners/header.
    vals="$(grep -E '^[0-9]+,' "$log" | awk -F, 'NF==6 && $5 ~ /^[0-9]+$/ {print $5}' | sort -n || true)"
    if [[ -z "$vals" ]]; then
        warn "no one-way latency samples found in $log — nothing to summarize"
        return 0
    fi
    banner "SUMMARY — one-way latency estimate (round-trip / 2), nanoseconds"
    printf '%s\n' "$vals" | awk '
        function pct(p,   i){ i=int((p/100.0)*(n-1))+1; if(i<1)i=1; if(i>n)i=n; return a[i] }
        { a[NR]=$1 }
        END {
            n=NR
            printf "  samples : %d\n", n
            printf "  min     : %d ns\n", a[1]
            printf "  P50     : %d ns\n", pct(50)
            printf "  P90     : %d ns\n", pct(90)
            printf "  P99     : %d ns\n", pct(99)
            printf "  max     : %d ns\n", a[n]
        }'
}

# ─── Role: receiver (Instance B) ────────────────────────────────────────────
run_receiver() {
    banner "wire_latency_aws.sh — ROLE=receiver — run $RUN_ID"
    echo "host: $(uname -srm)"
    echo "log:  $LOG"
    check_hugepages
    check_secondary_eni
    check_kernel_timestamping
    build_wire_tools

    local bin="$BUILD_DIR/tools/WireLatencyReceiver"
    [[ -x "$bin" ]] || die "receiver binary missing: $bin"

    banner "RUN — WireLatencyReceiver on port $PORT"
    # The receiver binds INADDR_ANY (all interfaces), so it is reachable on the
    # secondary ENI IP below — give this IP to the sender as RECEIVER_IP.
    echo "  >>> On the SENDER instance run:"
    echo "        RECEIVER_IP=$ENI_IP ROLE=sender bash scripts/wire_latency_aws.sh"
    echo "  Receiver runs until the sender disconnects. Logs RX-arrival + TX-ack"
    echo "  timestamps per order (CSV: seq,rx_arrival_ns,tx_ack_ns,processing_ns,hardware)."
    # Match the sender's timestamp basis (and the DPDK session's F-Stack
    # receiver, which is software-only) so the whole baseline is CLOCK_MONOTONIC.
    # Match the sender's timestamp basis (and the DPDK session's F-Stack
    # receiver, which is software-only) so the whole baseline is CLOCK_MONOTONIC.
    "$bin" --port "$PORT" --software-timestamps --software-timestamps
    banner "DONE — receiver log: $LOG"
}

# ─── Role: sender (Instance A) ──────────────────────────────────────────────
run_sender() {
    banner "wire_latency_aws.sh — ROLE=sender — run $RUN_ID"
    echo "host: $(uname -srm)"
    echo "log:  $LOG"
    [[ -n "${RECEIVER_IP:-}" ]] || die "RECEIVER_IP not set. Pass Instance B's secondary-ENI IP: RECEIVER_IP=<ip> ROLE=sender bash scripts/wire_latency_aws.sh"
    check_secondary_eni
    check_kernel_timestamping
    build_wire_tools

    local bin="$BUILD_DIR/tools/WireLatencySender"
    [[ -x "$bin" ]] || die "sender binary missing: $bin"

    banner "RUN — WireLatencySender -> $RECEIVER_IP:$PORT, $ORDERS orders"
    "$bin" --host "$RECEIVER_IP" --port "$PORT" --count "$ORDERS"

    summarize_oneway "$LOG"
    banner "DONE — sender log: $LOG"
}

# ─── Dispatch ───────────────────────────────────────────────────────────────
case "$ROLE" in
    sender)   run_sender ;;
    receiver) run_receiver ;;
esac
