# OS & CPU Tuning Guide — Order Matching Engine

> Host-level Linux tuning for deterministic tail latency on AWS c6in.metal (or any
> x86_64 bare-metal box). This is the layer *below* [CapacityPlanning.md](./CapacityPlanning.md)
> (which sizes cores/RAM) and feeds the "High Latency / Queue Buildup" path in
> [Runbook.md](./Runbook.md). Apply all of it before you trust a P99.9 number.

**Per-deployment values you MUST fill in** (placeholders below use `<...>`):
- `<matching_core>` — physical core the worker/matching thread is pinned to.
- `<dpdk_poll_core>` — busy-poll lcore for the F-Stack/DPDK receiver (in `dpdk_aws.sh`
  this is core 2, `lcore_mask=0x4`).
- `<housekeeping_mask>` — hex bitmask of the NON-isolated cores that absorb OS work,
  IRQs, and RCU callbacks (e.g. `0x1` = core 0).
- `<nic_dev>` — kernel name of the trading NIC (e.g. `ens1`; NOT `eth0` on c6in.metal —
  detect it, do not assume). See `dpdk_aws.sh` for the detection pattern.

Pick the isolated cores and the NIC device **on the same NUMA node** — see §4.

---

## 1. Why `taskset` Alone Is Not Enough

Pinning the engine with `taskset -c <matching_core>` (or the systemd `CPUAffinity=`
in [Runbook.md](./Runbook.md) §1) only constrains *where your threads run*. It does
**nothing** to stop the kernel from running its own work on that same core:

| Contender on a "pinned" core | Left unaddressed by `taskset` | Fixed by |
|------------------------------|-------------------------------|----------|
| Scheduler timer tick (250/1000 Hz) | fires every 1–4 ms even with one runnable task | `nohz_full` |
| RCU grace-period callbacks | batched, then dumped on the local core | `rcu_nocbs` |
| Other runnable tasks / kernel threads / workqueues | scheduler load-balances onto it | `isolcpus` |
| Hardware IRQ + softirq (NIC, timers) | delivered to any online CPU | IRQ affinity (§3) |
| Deep C-state wakeup + frequency ramp | core idles between orders | C-states / governor (§5) |

**Symptom of skipping this:** the mean and median look fine, but P99.9 shows random
**50–200 µs spikes** with no correlation to order flow. That is the timer tick + an
RCU callback batch + an ill-timed IRQ landing on the matching core. `taskset` cannot
see or prevent any of it.

The rest of this guide closes each gap. Sections §2 and §4 are one-time host setup
(boot params, hardware layout); §3 and §5 run at every engine start.

---

## 2. CPU Isolation (Kernel Boot Parameters) — P2-12

Isolate the matching + DPDK-poll cores from the kernel scheduler, the timer tick, and
RCU callback processing. Edit `GRUB_CMDLINE_LINUX` in `/etc/default/grub`:

```bash
# /etc/default/grub  — append to GRUB_CMDLINE_LINUX (keep existing values)
GRUB_CMDLINE_LINUX="... isolcpus=<matching_core>,<dpdk_poll_core> nohz_full=<matching_core>,<dpdk_poll_core> rcu_nocbs=<matching_core>,<dpdk_poll_core> irqaffinity=<housekeeping_mask>"
```

| Parameter | Effect | Why it matters here |
|-----------|--------|---------------------|
| `isolcpus=<cores>` | Removes cores from the scheduler's general load-balancing | Nothing else gets scheduled onto the matching/DPDK cores |
| `nohz_full=<cores>` | Full dynticks — stops the periodic scheduler tick on those cores while ≤1 task is runnable | Kills the 1–4 ms tick jitter |
| `rcu_nocbs=<cores>` | Offloads RCU callback invocation to housekeeping cores (`rcuoc/rcuog` kthreads) | RCU callbacks no longer batch-and-dump on the hot core |
| `irqaffinity=<mask>` | Default IRQ affinity mask applied at boot | Steers IRQs off the isolated cores from boot (belt-and-suspenders with §3) |

**Rules / gotchas:**
- The **same core list** should appear in `isolcpus`, `nohz_full`, and `rcu_nocbs`.
  The kernel requires every `nohz_full` core to also be in `rcu_nocbs`; if it is not,
  `nohz_full` is silently dropped.
- **Never isolate every core.** Leave at least one housekeeping core (typically core 0)
  out of all three lists — it runs the offloaded ticks, RCU callbacks, and IRQs. That
  housekeeping core is the one whose bit you set in `<housekeeping_mask>` (§3).
- `nohz_full` only stops the tick when exactly one task is runnable on the core. Pin
  **one** worker per isolated core; a second runnable thread re-arms the tick.

Apply and verify:
```bash
sudo update-grub          # Ubuntu/Debian; on RHEL: grub2-mkconfig -o /boot/grub2/grub.cfg
sudo reboot
# After reboot, confirm the kernel accepted them:
cat /proc/cmdline                              # shows the flags you set
cat /sys/devices/system/cpu/isolated           # should list <matching_core>,<dpdk_poll_core>
cat /sys/devices/system/cpu/nohz_full          # should list the same cores
```

---

## 3. NIC IRQ Affinity — P2-13

Even with `isolcpus`, the NIC's hardware IRQs (and their softirq bottom halves) can be
delivered to the isolated cores and preempt the matching thread. Pin **all** NIC IRQs
to the housekeeping core mask, and stop `irqbalance` from moving them back.

```bash
# irqbalance will fight you — disable it first, permanently.
sudo systemctl stop irqbalance
sudo systemctl disable irqbalance

# Pin every IRQ belonging to <nic_dev> to the housekeeping mask.
sudo scripts/set_irq_affinity.sh <nic_dev> <housekeeping_mask>
# e.g.  sudo scripts/set_irq_affinity.sh ens1 0x1     # all ens1 IRQs -> core 0
```

Run this **before** starting the engine, and re-run it after any NIC reset, driver
reload, or `ethtool -L` queue-count change (those re-create the IRQs).

**AWS ENA gotcha (managed IRQs):** the `ena` driver on c6in.metal registers its queue
IRQs as *kernel-managed* IRQs. Writes to `/proc/irq/<N>/smp_affinity` for managed IRQs
are rejected with `write error: Input/output error`. The script treats that as a
non-fatal warning and keeps going. For managed-IRQ NICs the affinity is fixed at
allocation time from the boot-time mask — so the real lever is `irqaffinity=<housekeeping_mask>`
in §2, not runtime `/proc` writes. Set both; verify with the checks below.

Verify (the script also echoes this per IRQ):
```bash
# List the NIC's IRQs, then read back each mask:
grep <nic_dev> /proc/interrupts | awk -F: '{print $1}'
cat /proc/irq/<N>/smp_affinity        # should equal <housekeeping_mask>
# Watch that interrupts are NOT landing on the isolated cores under load:
watch -n1 "grep <nic_dev> /proc/interrupts"
```

---

## 4. NIC ↔ Core NUMA Affinity — P2-14

On a multi-socket box the NIC's PCIe root is attached to exactly one NUMA node. If the
matching + DPDK cores live on a **different** node than the NIC, every packet DMA
descriptor, RX buffer, and MMIO doorbell crosses the inter-socket link (Intel **UPI**).
That adds ~100–300 ns per remote access **plus** queuing under contention, and shows up
as elevated *and variable* wire-to-wire latency that no amount of core isolation fixes.

Check the NIC's node and require it to equal the cores' node:
```bash
cat /sys/class/net/<nic_dev>/device/numa_node   # e.g. 0  — the NIC's NUMA node
# Node of an isolated core:
cat /sys/devices/system/cpu/cpu<matching_core>/topology/physical_package_id
# Or see the whole picture:
numactl --hardware        # node<->core map
lstopo --no-io            # visual topology (hwloc)
```

**Requirement:** `numa_node` of `<nic_dev>` **must equal** the node hosting
`<matching_core>` and `<dpdk_poll_core>`.

- A value of **`-1`** means the kernel couldn't determine the node (common inside VMs).
  On c6in.metal bare metal it should report a real node — if it reads `-1` there,
  investigate before trusting latency numbers.

**Remediation if they don't match:**
1. **Preferred — move the cores to the NIC's node.** Re-pick `<matching_core>` /
   `<dpdk_poll_core>` on the NIC's node, update `isolcpus`/`nohz_full`/`rcu_nocbs` (§2),
   and update the F-Stack `lcore_mask` / `cpu_mask` in `/etc/f-stack.conf` (`dpdk_aws.sh`)
   and any `numactl --membind` to that node.
2. **If the NIC can't be moved and no local cores are free** — change the host /
   instance so the trading NIC and your budgeted cores land on the same node.

---

## 5. C-States & Frequency Scaling — P2-16

A busy-poll core (DPDK poll loop, matching thread) is rarely idle *under load* — but any
gap (overnight lull, warmup, a quiet symbol) lets the core drop into a deep C-state or
down-clock. The **next** order after the gap then pays:
- **C-state exit latency** — waking a core from a deep package C-state (C6) costs tens of
  µs; that latency lands entirely on the first order after the idle window.
- **Frequency ramp** — under a scaling governor (`ondemand`/`schedutil`) an idled core
  wakes at a low P-state and takes hundreds of µs to ramp back to max clock.

Pin the frequency high and forbid deep idle:
```bash
# Max, fixed clock — no ramp after idle:
sudo cpupower frequency-set -g performance

# Forbid deep C-states (keep only C0/C1). Either disable a specific index:
sudo cpupower idle-set -d 2          # disable idle state index 2 (repeat for 3,4,... )
# ...or, more robustly, disable everything slower than 1 µs to exit:
sudo cpupower idle-set -D 1          # disable all C-states with exit latency > 1 µs

# Inspect what exists / what's disabled:
cpupower idle-info
cpupower frequency-info | grep -i governor
```

Or set it at boot in `GRUB_CMDLINE_LINUX` (§2) instead of at runtime:
```
intel_idle.max_cstate=1 processor.max_cstate=1     # cap C-states
# cpuidle.off=1                                     # disable cpuidle entirely (blunter)
# idle=poll                                         # never idle — max power/heat, zero wakeup
```

The software parts (governor + C-state disable) are wrapped in
`scripts/set_cpu_perf.sh` — run it before engine start:
```bash
sudo scripts/set_cpu_perf.sh                 # performance governor + disable deep C-states
```

**BIOS/UEFI note (on-prem / colo hardware):** on many server platforms the OS-level
C-state controls above are overridden or ignored unless deep package C-states (C6, and
sometimes C1E) are disabled in **firmware** (BIOS → Power/CPU → C-State Control). If OS
knobs don't stick, that is why — set it in BIOS. On AWS c6in.metal you do not get BIOS
access; rely on the OS-level controls and boot params above.

---

## 6. Journal Storage Backing — P2-7

CPU isolation and IRQ pinning remove the *scheduling* tail; a network-backed
journal re-introduces a much larger *I/O* tail that no amount of core tuning can
hide. The engine flushes each group commit to durable storage, so the journal
device's flush latency is added straight onto the order-entry path.

> **HARD REQUIREMENT: the journal MUST be on instance-store (local) NVMe or a
> RAM-backed tmpfs. EBS / Persistent Disk / Managed Disk / SAN / NFS is NOT
> acceptable for the journal path.** A network block device pays a ~1–4 ms
> round-trip on every flush — orders of magnitude larger than the ~10–30 µs of
> a local NVMe and completely dominant over the ns-scale matching cost.

- **Instance selection**: use a family with local NVMe instance-store
  (`c6id`/`c7gd`/`m6id`/`i4i`/`c6gd` on AWS; equivalents elsewhere). Bare metal
  such as `c6id.metal` is ideal.
- **Placement**: mount the instance-store volume and point `--journal` at it (or
  at a `tmpfs`). Never leave the journal on the root EBS/boot volume.
- **tmpfs (RAM) option**: lowest latency, but durability must then come from
  primary-backup replication (see [CapacityPlanning.md](./CapacityPlanning.md) §5).
- **Nitro gotcha**: instance-store *and* EBS both appear as `/dev/nvme*`, so
  `TRAN=nvme` is not proof of locality — check the device model string
  (`Amazon EC2 NVMe Instance Storage` vs `Amazon Elastic Block Store`). The
  ready-to-wire check is in [Runbook.md](./Runbook.md) §1 "Journal Storage
  Pre-Flight".

**Benchmark caveat.** The published Path C P99 of **3,568 ns was measured with
the journal on EBS** (see [BENCHMARKS.md](../BENCHMARKS.md)); a large part of it
is EBS network round-trip, a *deployment* artifact rather than a code cost. The
production P99 must be **re-benchmarked with the journal on instance-store NVMe
or tmpfs** and that re-measured number reported as the real figure.

---

## 7. Huge Pages (2 MB) — P2-15

The hot data structures — the `FlatPriceMap` directory + bitmaps, the resting-order
slab, the `ObjectPool<Order>` backing store, and the `MpscQueue` slot rings — are
large enough that 4 KB paging burns real dTLB entries on the matching hot path. A
2 MB huge page maps 512× the address range per TLB entry, so the whole working set
stays covered by a handful of entries. `hugeAlloc()` (see `include/HugePageAllocator.h`)
backs each of those regions with
`mmap(..., MAP_PRIVATE|MAP_ANONYMOUS|MAP_HUGETLB|MAP_HUGE_2MB, ...)` and falls back
to the historical aligned allocation on failure or on non-Linux.

> **HARD REQUIREMENT: Transparent Huge Pages (THP) MUST be disabled.** THP's
> background compaction (`khugepaged`) can stall a core for **10–100 ms** while it
> defragments and collapses pages — orders of magnitude larger than the ns-scale
> matching cost, and a direct source of the P99.9 tail this whole guide exists to
> kill. Explicit (`MAP_HUGETLB`) huge pages are reserved up front and are **not**
> subject to khugepaged, so they give the TLB win without the pause risk.

```bash
# Disable THP (do this every boot, before starting the engine):
echo never | sudo tee /sys/kernel/mm/transparent_hugepage/enabled
echo never | sudo tee /sys/kernel/mm/transparent_hugepage/defrag
# Verify — the selected value is the one in [brackets]:
cat /sys/kernel/mm/transparent_hugepage/enabled     # want: always madvise [never]
```

**Reserve enough explicit 2 MB pages for the working set.** The engine reads
`/proc/sys/vm/nr_hugepages` at pool construction; if explicit pages were requested
but the reservation is short, it logs a one-line `[hugepages] WARNING` to stderr
naming the shortfall and the `sysctl` to fix it, then transparently falls back to
4 KB pages (correct, just with more dTLB pressure). Size the reservation from the
configured working set — the dominant consumer is the per-symbol `Order` pool
(`orderPoolCapacity × sizeof(Order)`, `sizeof(Order) == 192 B`), plus the two
`FlatPriceMap` storage blocks and the `MpscQueue` rings:

```bash
# Example: 1,000,000-order pool ≈ 192 MB ≈ 92 x 2MB pages per symbol.
# Round up generously and reserve the total across all symbols + rings:
sudo sysctl -w vm.nr_hugepages=256
# Persist across reboot:
echo 'vm.nr_hugepages = 256' | sudo tee /etc/sysctl.d/60-hugepages.conf
# Verify reserved vs free:
grep -E 'HugePages_(Total|Free|Rsvd)|Hugepagesize' /proc/meminfo
cat /proc/sys/vm/nr_hugepages
```

Reserve huge pages **early at boot**, before memory fragments — a busy host may be
unable to assemble contiguous 2 MB regions later. For a hard guarantee, reserve on
the kernel command line instead of `sysctl` (add to `GRUB_CMDLINE_LINUX` in §2):
`default_hugepagesz=2M hugepagesz=2M hugepages=256`.

---

## 8. Bring-Up Order & Checklist

One-time host setup (survives reboot):
- [ ] §2 — `isolcpus` / `nohz_full` / `rcu_nocbs` / `irqaffinity` in GRUB, `update-grub`, reboot.
- [ ] §2 — verified `cat /proc/cmdline` and `/sys/.../cpu/isolated` show the isolated cores.
- [ ] §4 — verified `<nic_dev>` NUMA node == isolated-cores node (`≠ -1`).
- [ ] §3 — `irqbalance` stopped **and** disabled.
- [ ] §6 — instance has **local NVMe instance-store** (or tmpfs) for the journal; journal path is **not** on EBS/PD/SAN/NFS.
- [ ] §7 — `vm.nr_hugepages` reserved for the working set (persisted in `/etc/sysctl.d` or on the kernel command line).

Every engine start (before launching `OrderEngine` / the F-Stack receiver):
- [ ] §6 — journal path verified on local NVMe / tmpfs ([Runbook.md](./Runbook.md) §1 pre-flight).
- [ ] §7 — Transparent Huge Pages set to `never` (`cat /sys/kernel/mm/transparent_hugepage/enabled`).
- [ ] §7 — `HugePages_Free` in `/proc/meminfo` covers the working set (no startup `[hugepages] WARNING`).
- [ ] §3 — `sudo scripts/set_irq_affinity.sh <nic_dev> <housekeeping_mask>`
- [ ] §5 — `sudo scripts/set_cpu_perf.sh`
- [ ] Start the engine pinned to the isolated cores (`taskset -c <matching_core> ...` /
      F-Stack `lcore_mask`), per [Runbook.md](./Runbook.md) §1.

Then confirm the spikes are gone: run the benchmark and check that P99.9 no longer
shows the 50–200 µs outliers described in §1.
