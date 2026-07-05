#!/usr/bin/env bash
#
# dpdk_aws.sh — Session 2 DPDK/F-Stack RECEIVER setup + execution for the
# wire-to-wire latency session on AWS c6in.metal (Instance B only). The SENDER
# still uses scripts/wire_latency_aws.sh with ROLE=sender on Instance A.
#
# ┌──────────────────────────────────────────────────────────────────────────┐
# │ WARNING: this script BINDS eth1 (the secondary ENI) to DPDK via vfio-pci  │
# │ and makes it UNAVAILABLE to the kernel. Ensure your SSH session is on     │
# │ eth0 (the PRIMARY ENI) before running — otherwise you WILL lose access.   │
# └──────────────────────────────────────────────────────────────────────────┘
#
# Must run as root (hugepages, vfio bind, mount, install all require it).
#
# Steps: (1) prereqs (2) hugepages (3) install DPDK + F-Stack (idempotent)
#        (4) bind eth1 to vfio-pci (5) build -DENABLE_DPDK=ON (6) write
#        /etc/f-stack.conf (7) run the F-Stack receiver.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

ENI="${ENI:-eth1}"
PORT="${PORT:-9001}"
BUILD_DIR="build-dpdk"
FSTACK_CONF="${FSTACK_CONF:-/etc/f-stack.conf}"
HUGE_MNT="/mnt/huge"
RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)"
LOG="$REPO_ROOT/dpdk-setup-$RUN_ID.log"
exec > >(tee -a "$LOG") 2>&1

banner() { printf '\n\033[1m======================================================================\n%s\n======================================================================\033[0m\n' "$*"; }
warn()   { printf '\033[33mWARN: %s\033[0m\n' "$*"; }
die()    { printf '\033[31mFATAL: %s\033[0m\n' "$*"; exit 1; }

banner "dpdk_aws.sh — F-Stack DPDK receiver setup — run $RUN_ID"
printf '\033[31m%s\033[0m\n' "!!! This binds $ENI to DPDK and removes it from the kernel. SSH MUST be on eth0. !!!"
echo "log: $LOG"

# ─── Step 1: prerequisites ──────────────────────────────────────────────────
banner "STEP 1/7 — prerequisites"
[[ "$(uname -s)" == "Linux" ]] || die "not Linux — DPDK/F-Stack require Linux (AWS c6in.metal)."
echo "  os: $(uname -srm)"
# F-Stack has historically lagged bleeding-edge kernels; warn (non-fatal) on 7.x+.
KMAJ="$(uname -r)"; KMAJ="${KMAJ%%.*}"; KMAJ="${KMAJ//[!0-9]/}"
if [[ -n "$KMAJ" ]] && (( KMAJ > 6 )); then
    warn "Kernel $(uname -r) detected — F-Stack compatibility unverified, proceed with caution"
fi
ip link show "$ENI" >/dev/null 2>&1 || die "secondary ENI '$ENI' not found. Attach + configure a second ENI first."
echo "  secondary ENI '$ENI' present"
command -v clang++ >/dev/null 2>&1 || die "clang++ not found — install clang (apt-get install -y clang)."
echo "  clang: $(clang++ --version | head -1)"
[[ "${EUID}" -eq 0 ]] || die "must run as root (DPDK/hugepages/vfio bind require it). Re-run with: sudo bash scripts/dpdk_aws.sh"
echo "  running as root"

# ─── Step 2: hugepages (required for DPDK) ──────────────────────────────────
banner "STEP 2/7 — hugepages"
NR_HUGE="$(cat /proc/sys/vm/nr_hugepages 2>/dev/null || echo 0)"
echo "  current nr_hugepages = $NR_HUGE"
if (( NR_HUGE < 1024 )); then
    echo "  configuring 1024 hugepages..."
    echo 1024 > /proc/sys/vm/nr_hugepages
fi
NR_HUGE="$(cat /proc/sys/vm/nr_hugepages 2>/dev/null || echo 0)"
(( NR_HUGE >= 1024 )) || die "hugepage allocation did not take (nr_hugepages=$NR_HUGE). Free memory may be too fragmented — a reboot with 'hugepages=1024' on the kernel cmdline may be required."
echo "  nr_hugepages = $NR_HUGE (OK)"
if ! mountpoint -q "$HUGE_MNT" 2>/dev/null && ! grep -qs "$HUGE_MNT" /proc/mounts; then
    echo "  mounting hugetlbfs at $HUGE_MNT..."
    mkdir -p "$HUGE_MNT"
    mount -t hugetlbfs nodev "$HUGE_MNT"
fi
echo "  hugetlbfs mounted at $HUGE_MNT"

# ─── Step 3: install DPDK + F-Stack (idempotent) ────────────────────────────
banner "STEP 3/7 — install DPDK + F-Stack (idempotent)"
# No distro dpdk/dpdk-dev/libdpdk-dev — F-Stack's bundled/patched DPDK replaces
# them (built from source below). apt-get install -y is idempotent, so run it
# unconditionally. Deps (F-Stack README + lib/Makefile): build-essential
# (gcc/make), pkg-config (lib/Makefile runs `pkg-config --exists libdpdk`),
# libssl-dev (F-Stack links OpenSSL; also provides libcrypto — no separate
# libcrypto-dev on Debian/Ubuntu), libnuma-dev + libpcap-dev (DPDK), and
# python3-pyelftools + meson + ninja-build to build the bundled DPDK.
echo "  installing F-Stack + bundled-DPDK build dependencies..."
# linux-headers-$(uname -r): the F-Stack dev-branch DPDK build uses
# `meson setup -Denable_kmods=true`, which compiles kernel modules and needs the
# running kernel's headers.
apt-get install -y \
    build-essential pkg-config libssl-dev libnuma-dev libpcap-dev \
    python3-pyelftools meson ninja-build "linux-headers-$(uname -r)"

if [[ -f /usr/local/lib/libfstack.a ]]; then
    echo "  F-Stack already installed (/usr/local/lib/libfstack.a) — skipping build"
else
    echo "  building F-Stack (bundled DPDK) from source..."
    if [[ ! -d /tmp/f-stack ]]; then
        # 'dev' is F-Stack's default/active branch (confirmed via GitHub API,
        # 2026-07; repo not archived). Pin it explicitly so a future default
        # rename doesn't silently change what we build.
        git clone -b dev https://github.com/F-Stack/f-stack.git /tmp/f-stack
    fi
    # 1) Build + install F-Stack's BUNDLED/patched DPDK (replaces the distro
    #    libdpdk). Exact F-Stack dev-branch build (README): meson with
    #    -Denable_kmods=true, then ninja build + install. meson's default prefix
    #    is /usr/local, so libdpdk.pc installs under /usr/local/lib*/pkgconfig and
    #    the libs under /usr/local/lib*. (pyelftools comes from the apt
    #    python3-pyelftools installed above — no pip needed.)
    # Try the dev-branch build WITH kernel modules first. If the kmods fail to
    # compile (common on bleeding-edge kernels, e.g. 7.x), fall back to a
    # kmod-free build in a FRESH build dir (meson won't switch an existing dir's
    # options). vfio-pci binding does not need the DPDK kmods, so the kmod-free
    # DPDK is fully usable for our path. The `if ( ... )` condition keeps set -e
    # from aborting on the first failure.
    if ( cd /tmp/f-stack/dpdk \
            && meson setup -Denable_kmods=true build \
            && ninja -C build \
            && ninja -C build install ); then
        echo "  DPDK built with kernel modules (-Denable_kmods=true)"
    else
        warn "kmod build failed — retrying without kernel modules (vfio-pci binding still works without kmods)"
        ( cd /tmp/f-stack/dpdk \
            && meson setup -Denable_kmods=false build_nokmods \
            && ninja -C build_nokmods \
            && ninja -C build_nokmods install ) \
            || die "DPDK build failed even without kmods — check the meson/ninja output above."
        echo "  DPDK built without kernel modules (-Denable_kmods=false)"
    fi
    ldconfig
    # 2) Build + install F-Stack lib against that bundled DPDK. lib/Makefile runs
    #    `pkg-config --exists libdpdk`, so point PKG_CONFIG_PATH at the DPDK we
    #    just installed under /usr/local. FF_PATH is F-Stack's documented root.
    export PKG_CONFIG_PATH="/usr/local/lib/x86_64-linux-gnu/pkgconfig:/usr/local/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
    export FF_PATH=/tmp/f-stack
    ( cd /tmp/f-stack/lib && make && make install )
fi
[[ -f /usr/local/lib/libfstack.a ]]   || die "F-Stack install failed: /usr/local/lib/libfstack.a not found."
[[ -f /usr/local/include/ff_api.h ]]  || die "F-Stack install failed: /usr/local/include/ff_api.h not found."
echo "  F-Stack OK: libfstack.a + ff_api.h present"

# ─── Step 4: bind secondary ENI to DPDK PMD ─────────────────────────────────
banner "STEP 4/7 — bind $ENI to vfio-pci"
# Capture the ENI's IPv4 config BEFORE bringing it down — Step 6 needs it and it
# vanishes from the kernel once vfio-bound.
ENI_CIDR="$(ip -4 -o addr show "$ENI" 2>/dev/null | awk '{print $4}' | head -1)"
[[ -n "$ENI_CIDR" ]] || die "$ENI has no IPv4 address to record for the F-Stack config."
ENI_IP="${ENI_CIDR%/*}"
ENI_PREFIX="${ENI_CIDR#*/}"
PCI_ADDR="$(ethtool -i "$ENI" 2>/dev/null | awk '/bus-info:/ {print $2}')"
[[ -n "$PCI_ADDR" ]] || die "could not read PCI bus-info for $ENI via ethtool -i."
echo "  $ENI: ip=$ENI_IP/$ENI_PREFIX pci=$PCI_ADDR"

# Derive netmask / broadcast / gateway (AWS VPC: gateway = subnet base + 1).
prefix_to_netmask() {
    local p="$1" i out="" byte
    for i in 1 2 3 4; do
        if (( p >= 8 )); then byte=255; p=$((p - 8))
        elif (( p <= 0 )); then byte=0
        else byte=$(( 256 - (1 << (8 - p)) )); p=0; fi
        out+="$byte"; [[ $i -lt 4 ]] && out+="."
    done
    printf '%s' "$out"
}
NETMASK="$(prefix_to_netmask "$ENI_PREFIX")"
IFS=. read -r a b c d <<< "$ENI_IP"
IFS=. read -r m1 m2 m3 m4 <<< "$NETMASK"
NET="$((a & m1)).$((b & m2)).$((c & m3)).$((d & m4))"
IFS=. read -r n1 n2 n3 n4 <<< "$NET"
GATEWAY="$n1.$n2.$n3.$((n4 + 1))"
BROADCAST="$((n1 | (255 ^ m1))).$((n2 | (255 ^ m2))).$((n3 | (255 ^ m3))).$((n4 | (255 ^ m4)))"
echo "  derived: netmask=$NETMASK broadcast=$BROADCAST gateway=$GATEWAY (verify against your VPC subnet)"

echo "  bringing $ENI down + binding to vfio-pci..."
ip link set "$ENI" down
modprobe vfio-pci
command -v dpdk-devbind.py >/dev/null 2>&1 || die "dpdk-devbind.py not found on PATH (part of the DPDK tools)."
dpdk-devbind.py --bind=vfio-pci "$PCI_ADDR"
if dpdk-devbind.py --status 2>/dev/null | grep -E "$PCI_ADDR" | grep -q vfio-pci; then
    echo "  $PCI_ADDR bound to vfio-pci (confirmed)"
else
    die "bind verification failed — $PCI_ADDR is not showing vfio-pci in dpdk-devbind.py --status."
fi
printf '\033[31mWARNING: %s is now UNBOUND from the kernel — SSH access must be on eth0 (primary ENI).\033[0m\n' "$ENI"

# ─── Step 5: build with DPDK enabled ────────────────────────────────────────
banner "STEP 5/7 — build (-DENABLE_DPDK=ON)"
rm -rf "$BUILD_DIR"
CFG_LOG="$(mktemp)"
# NOTE: BUILD_WIRE_LATENCY_TOOLS (upper-case) is the real CMake option name.
cmake -B "$BUILD_DIR" -S . \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_FLAGS="-Wno-error=character-conversion" \
    -DENABLE_DPDK=ON \
    -DBUILD_WIRE_LATENCY_TOOLS=ON 2>&1 | tee "$CFG_LOG"
if grep -qi 'io_uring journal path enabled' "$CFG_LOG"; then
    echo "  (io_uring journal path enabled)"
fi
if grep -qiE 'DPDK.*(found|enabled)' "$CFG_LOG"; then
    echo "  DPDK detected by CMake — OB_HAVE_DPDK active"
else
    warn "CMake did not report DPDK found/enabled — the build may fall back to kernel TCP (OB_HAVE_DPDK undefined). Check that libdpdk + F-Stack are discoverable (pkg-config --libs libdpdk; FF_PATH)."
fi
rm -f "$CFG_LOG"
cmake --build "$BUILD_DIR" -- -j"$(nproc)" || die "step 5: DPDK build failed."
echo "  build OK"

# ─── Step 6: generate F-Stack config ────────────────────────────────────────
banner "STEP 6/7 — generate $FSTACK_CONF"
# lcore_mask=0x4 → core 2 (isolate it via isolcpus for a clean busy-poll lcore).
# port_list=0 is required for F-Stack to bring up [port0]; the PCI address is
# selected via the vfio binding from Step 4 (recorded here for reference).
cat > "$FSTACK_CONF" <<EOF
[dpdk]
lcore_mask=0x4
channel=4
promiscuous=1
port_list=0

[port0]
# PCI (bound to vfio-pci in Step 4): $PCI_ADDR
addr=$ENI_IP
netmask=$NETMASK
broadcast=$BROADCAST
gateway=$GATEWAY

[freebsd.boot]
hz=100
EOF
echo "  wrote $FSTACK_CONF:"
echo "  ----------------------------------------------------------------------"
sed 's/^/  | /' "$FSTACK_CONF"
echo "  ----------------------------------------------------------------------"
warn "Review the config above (esp. netmask/broadcast/gateway — derived, verify vs your VPC subnet, and confirm core 2 is isolated via isolcpus)."
echo "Review config above. Ctrl-C to abort, continuing in 5s..."
sleep 5

# ─── Step 7: run the F-Stack DPDK receiver ──────────────────────────────────
banner "STEP 7/7 — run receiver with DPDK ($FSTACK_CONF)"
RECV_LOG="$REPO_ROOT/wire-dpdk-receiver-$RUN_ID.log"
RECV_BIN="$BUILD_DIR/tools/WireLatencyReceiver"
[[ -x "$RECV_BIN" ]] || die "receiver binary missing: $RECV_BIN"

# WireLatencyReceiver --conf now takes the F-Stack receive path directly: it
# runs a self-contained F-Stack echo receiver (ff_init/ff_run/ff_recv/ff_send)
# and emits the SAME CSV as the kernel path, so this session's log is directly
# comparable to the baseline. Note: F-Stack's ff_recv exposes no NIC hardware
# timestamp, so the DPDK path uses CLOCK_MONOTONIC software timestamps (the
# 'hardware' CSV column reads 0). For a strict apples-to-apples comparison,
# collect the baseline with software timestamps too.
echo "  logging to: $RECV_LOG"
echo "  >>> On the SENDER instance (Instance A) run:"
echo "        RECEIVER_IP=$ENI_IP ROLE=sender bash scripts/wire_latency_aws.sh"
echo
"$RECV_BIN" --port "$PORT" --conf "$FSTACK_CONF" 2>&1 | tee "$RECV_LOG"

banner "DONE — setup log: $LOG  |  receiver log: $RECV_LOG"
