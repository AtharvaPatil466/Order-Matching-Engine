# Capacity Planning Guide — Order Matching Engine

> Use this guide to size your deployment based on expected throughput, order book depth, and latency requirements.

---

## 1. Memory Sizing

### Per-Order Memory
| Component | Size | Notes |
|-----------|------|-------|
| `Order` struct | 192 bytes | `alignas(64)`, intrusive linked list pointers (`sizeof(Order) == 192`) |
| ObjectPool overhead | 16 bytes/slot | Pool metadata + free list pointer |
| FlatPriceMap slot | 8 bytes/price level | Pointer to price level head |
| Price level metadata | 32 bytes | Total qty, order count, head/tail |
| **Total per live order** | **~248 bytes** | 192 + 16 + 8 + 32 |

### Sizing Formula
```
Memory = (max_live_orders × 248 bytes)
       + (price_range × 8 bytes × num_symbols)      # FlatPriceMap
       + (num_symbols × 64KB)                         # per-book overhead
       + (queue_capacity × sizeof(OrderRequest))      # MPSC queues
       + 256MB                                        # framework overhead
```

> **This formula does not describe the engine.** A book is allocated in full
> when its symbol is added, whether or not an order ever arrives: measured
> **10.3 MiB per empty book** at the defaults (10,000 order slots, 65,536-trade
> ring — see `config/engine.conf.example`), not 64 KB. Memory scales with
> `symbols × order_pool_capacity`, not with live orders; the ~248 B above is
> only the marginal cost of one more order (Runbook §6).

### Examples

| Scenario | Live Orders | Symbols | Memory |
|----------|-------------|---------|--------|
| Small (dev/test) | 10K | 4 | ~256 MB |
| Medium (prop desk) | 100K | 50 | ~512 MB |
| Large (exchange) | 1M | 500 | ~2 GB |
| Ultra (HFT venue) | 5M | 2000 | ~8 GB |

These totals are understated. At the default sizing, book memory alone is
`symbols × 10.3 MiB`: about 0.5 GiB for 50 symbols, 5 GiB for 500 and 20 GiB
for 2,000, before any other overhead.

---

## 2. CPU Sizing

### Thread Allocation
| Thread | Count | CPU Affinity |
|--------|-------|-------------|
| Worker (matching) | 1 per partition | Pin to isolated core |
| Gateway (FIX I/O) | 1 | Pin to core |
| Admin HTTP | 1 | Shared core OK |
| Journal flush | 1 (background) | Shared core OK |
| Replication | 1 (if HA) | Shared core OK |
| **Total** | **N + 3-4** | |

### Sizing Formula
```
Cores = num_worker_threads + 3 (gateway + admin + journal)
      + 1 if replication enabled
      + 1 headroom for OS/monitoring
```

### Performance by Core Count

| Cores | Workers | Throughput (lean) | Throughput (full) |
|-------|---------|-------------------|-------------------|
| 4 | 1 | 25M ops/s | 1.9M ops/s |
| 8 | 4 | 80M ops/s | 7M ops/s |
| 16 | 8 | 150M ops/s | 14M ops/s |
| 32 | 16 | 280M ops/s | 25M ops/s |

> **Note**: These are not measurements. No benchmark in this repository produces them, and no lean-mode figure is published anywhere. For comparison, the published x86 single-thread core-matching figure is 2.80M ops/s (3.10M with PGO) at commit `d2e688c`, with no journal (BENCHMARKS.md). Real-world throughput depends on match rate, order book depth, and cross-symbol correlation.

### CPU Affinity Best Practices
```bash
# Isolate cores 2-5 for matching threads (Linux)
echo "2-5" > /sys/devices/system/cpu/cpufreq/policy*/affected_cpus

# Pin the engine with taskset
taskset -c 2-5 ./bin/OrderEngine --threads 4

# On macOS, use thread affinity hints (less granular)
# The engine sets THREAD_AFFINITY_POLICY internally
```

> **`taskset` pins your threads but does not stop the kernel from running timer
> ticks, RCU callbacks, and IRQs on the same cores** — the usual cause of random
> 50–200 µs P99.9 spikes. For the full host tuning (kernel `isolcpus`/`nohz_full`/
> `rcu_nocbs`, NIC IRQ affinity, NIC↔core NUMA alignment, C-states/governor) see
> [OSTuning.md](./OSTuning.md).

---

## 3. Disk Sizing (Journal)

### Journal Entry Size
Every record is a packed `JournalEntry` of **134 bytes**, whatever its type
(`include/Journal.h`); the file starts with a 24-byte header. An earlier table
here gave 24-100 bytes by type.

| Entry Type | Size | Notes |
|------------|------|-------|
| AddOrder | 134 bytes | All order fields + CRC |
| CancelOrder | 134 bytes | Same fixed record |
| ModifyOrder | 134 bytes | Same fixed record |
| File header | 24 bytes | Once per file |
| Snapshot entry | 134 bytes | Same fixed record |

### Daily Journal Growth
```
Journal/day = orders_per_day × avg_entry_size
            + checkpoints_per_day × (live_orders × 100 bytes)
```

| Scenario | Orders/Day | Checkpoints | Journal/Day | Journal/Month |
|----------|-----------|-------------|-------------|---------------|
| Small | 100K | 10 | ~10 MB | ~300 MB |
| Medium | 1M | 50 | ~150 MB | ~4.5 GB |
| Large | 10M | 100 | ~1.5 GB | ~45 GB |
| Ultra | 100M | 500 | ~15 GB | ~450 GB |

These are bytes **written**, not bytes kept. A checkpoint replaces the journal
with a snapshot of the resting orders, and one runs automatically at 250,000
entries or 64 MiB (compiled in), at `--journal-max-mb`, and on every clean
OrderEngine shutdown — so the file never holds a day's history, and fills and
cancels before the last checkpoint are gone unless something copied the file
first.

### Disk IOPS Requirements
| Sync Policy | IOPS Needed | Latency Impact |
|-------------|-------------|----------------|
| `GroupCommit` (OrderEngine) | ~1K-5K | one `fdatasync` per 64-entry batch — the only in-repo measurement is ~438 µs on x86 CI ext4 (`include/Journal.h`), not the ~1-5 µs this table used to give |
| `Immediate` (GatewayServer) | 10K-100K | one `fdatasync` per entry |

There is no `None` or `EveryEntry` policy: `Journal::SyncPolicy` is
`{Immediate, GroupCommit}`, and each binary hard-codes its choice.

### Recommended Disk
- **Development**: Any SSD
- **Production**: NVMe SSD with ≥100K IOPS write
- **Ultra-low-latency**: Intel Optane. tmpfs only if losing the journal on a host reboot is acceptable: replication, which this line used to lean on, is disabled by default and has known CRITICAL defects

### Journal Storage Backing — HARD Deployment Constraint

> **The journal MUST live on instance-store (local) NVMe or a RAM-backed
> tmpfs. A network block device — AWS EBS, GCP Persistent Disk, Azure
> Managed Disk, or any iSCSI/NFS/SAN volume — is NOT acceptable for the
> journal path.** The engine `fdatasync()`s (or io_uring-acks) each group
> commit; on a network volume every flush pays a network round-trip, which
> lands directly in the order-entry P99 tail. This is a **provisioning
> requirement**, not a tuning suggestion.

| Backing | Journal flush latency | Verdict |
|---------|-----------------------|---------|
| RAM tmpfs (`/dev/shm`) | ~0 | ⚠️ Lost on host reboot. Replication, which was supposed to cover that, is disabled by default with known CRITICAL defects — no durability across a host failure |
| Instance-store NVMe (e.g. `c6id.metal`, `c6gd`, `i4i`) | ~10–30 µs | ✅ Required minimum for production |
| Local physical NVMe (bare metal) | ~10–30 µs | ✅ Equivalent to instance-store NVMe |
| **EBS / PD / Managed Disk / SAN / NFS** | **~1–4 ms round-trip** | ❌ **NOT acceptable for the journal** |

**Why this matters for the published numbers.** The current benchmarked
**Path C P99 of 3,568 ns** was measured with the journal at
`/tmp/honest_benchmark.journal` (`benchmarks/HonestBenchmark.cpp`), on a host
whose `/tmp` backing — EBS or tmpfs — was not recorded. Earlier revisions of
this page and [BENCHMARKS.md](../BENCHMARKS.md) called it EBS-backed; nothing
shows that, so how much of the figure is storage latency is unknown.
**The production P99 must be re-benchmarked with the journal on
instance-store NVMe (or tmpfs) and that re-measured number reported as the
real Path C P99.** Do not cite the 3,568 ns as the engine's
achievable production tail.

**Instance selection (AWS example).** Choose an instance family with local
NVMe instance-store — `c6id`/`c7gd`/`m6id`/`i4i`/`c6gd` — and place the
journal on the mounted instance-store volume (or a tmpfs). Do **not** put
the journal on the root EBS volume. A RAM tmpfs journal is only durable if
something else is, and replication is disabled by default with known CRITICAL
defects.

---

## 4. Network Sizing

### Bandwidth per Protocol
| Protocol | Message Size | At 10K msg/s | At 100K msg/s |
|----------|-------------|-------------|--------------|
| FIX 4.4 inbound | 200-400 bytes | 4 MB/s | 40 MB/s |
| FIX 4.4 outbound | 300-600 bytes | 6 MB/s | 60 MB/s |
| Market data (SHM) | sizeof(ShmEntry) ≈ 300 bytes | 3 MB/s | 30 MB/s |
| Replication | ~100 bytes/entry | 1 MB/s | 10 MB/s |
| Admin HTTP | ~500 bytes/req | negligible | negligible |
| **Total** | | **~14 MB/s** | **~140 MB/s** |

### Network Requirements
| Scenario | NIC | Latency |
|----------|-----|---------|
| Development | 1 GbE | ~50-100μs |
| Production | 10 GbE | ~5-10μs |
| HFT | 25 GbE + kernel bypass (DPDK/Solarflare) | ~1-2μs |

---

## 5. Replication Sizing (HA)

### Replication Lag Budget
```
Replication lag = network_rtt + journal_entry_size / bandwidth
```

| Network | RTT | Lag per Entry | Max Sustainable Rate |
|---------|-----|---------------|---------------------|
| Same rack | 10μs | ~11μs | 90K entries/s |
| Same DC | 100μs | ~101μs | 10K entries/s |
| Cross-DC | 1ms | ~1.1ms | 1K entries/s |

### Recommendation
- **Same rack**: Full synchronous replication viable
- **Same DC**: Semi-synchronous (ack after write, don't wait for fsync)
- **Cross-DC**: Async replication only — accept data loss window

---

## 6. Quick Sizing Calculator

For a given target:

```
Target: 50K orders/sec, 100 symbols, 500K max live orders, 99th %ile < 10μs

CPU:    8 cores (4 workers + 4 overhead)
Memory: 512 MB (500K × 248 bytes ≈ 124 MB + overhead)
Disk:   NVMe, 200 GB capacity (50K × 86400s × 80 bytes / 1e9 = 345 GB/day
        → checkpoint every 10K entries reclaims ~90%)
Network: 10 GbE
```

---

## 7. Monitoring Checklist

Run these checks daily:

- [ ] `df -h /journal/path` — disk < 80% used
- [ ] Journal path is on **local NVMe / tmpfs, not a network block device** — see [Runbook.md](./Runbook.md) §1 "Journal Storage Pre-Flight" (verify at every start, re-check after any host/AMI/volume change)
- [ ] `curl /prometheus | grep queue_depth` — queue < 50% capacity
- [ ] `curl /prometheus | grep p99` — latency within SLA
- [ ] `curl /health` — returns 200
- [ ] Backup replication lag < 1 second
- [ ] Journal size < 80% of checkpoint threshold
