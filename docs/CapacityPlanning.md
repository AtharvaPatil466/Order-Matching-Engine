# Capacity Planning Guide — Order Matching Engine

> Use this guide to size your deployment based on expected throughput, order book depth, and latency requirements.

---

## 1. Memory Sizing

### Per-Order Memory
| Component | Size | Notes |
|-----------|------|-------|
| `Order` struct | 128 bytes | Packed, intrusive linked list pointers |
| ObjectPool overhead | 16 bytes/slot | Pool metadata + free list pointer |
| FlatPriceMap slot | 8 bytes/price level | Pointer to price level head |
| Price level metadata | 32 bytes | Total qty, order count, head/tail |
| **Total per live order** | **~184 bytes** | |

### Sizing Formula
```
Memory = (max_live_orders × 184 bytes)
       + (price_range × 8 bytes × num_symbols)      # FlatPriceMap
       + (num_symbols × 64KB)                         # per-book overhead
       + (queue_capacity × sizeof(OrderRequest))      # MPSC queues
       + 256MB                                        # framework overhead
```

### Examples

| Scenario | Live Orders | Symbols | Memory |
|----------|-------------|---------|--------|
| Small (dev/test) | 10K | 4 | ~256 MB |
| Medium (prop desk) | 100K | 50 | ~512 MB |
| Large (exchange) | 1M | 500 | ~2 GB |
| Ultra (HFT venue) | 5M | 2000 | ~8 GB |

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

> **Note**: These are theoretical maximums assuming perfect symbol distribution. Real-world throughput depends on match rate, order book depth, and cross-symbol correlation.

### CPU Affinity Best Practices
```bash
# Isolate cores 2-5 for matching threads (Linux)
echo "2-5" > /sys/devices/system/cpu/cpufreq/policy*/affected_cpus

# Pin the engine with taskset
taskset -c 2-5 ./bin/OrderEngine --threads 4

# On macOS, use thread affinity hints (less granular)
# The engine sets THREAD_AFFINITY_POLICY internally
```

---

## 3. Disk Sizing (Journal)

### Journal Entry Size
| Entry Type | Size | Notes |
|------------|------|-------|
| AddOrder | ~80 bytes | All order fields + CRC |
| CancelOrder | ~24 bytes | OrderId + CRC |
| ModifyOrder | ~32 bytes | OrderId + newQty + CRC |
| Checkpoint header | ~16 bytes | Sequence + CRC |
| Snapshot entry | ~100 bytes | Full order state |

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

### Disk IOPS Requirements
| Sync Policy | IOPS Needed | Latency Impact |
|-------------|-------------|----------------|
| `None` | ~100 | 0 (async) |
| `GroupCommit` (default) | ~1K-5K | ~1-5μs per batch |
| `EveryEntry` | 10K-100K | ~10-100μs per entry |

### Recommended Disk
- **Development**: Any SSD
- **Production**: NVMe SSD with ≥100K IOPS write
- **Ultra-low-latency**: Intel Optane or tmpfs (if durability handled by replication)

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
Memory: 512 MB (500K × 184 bytes + overhead)
Disk:   NVMe, 200 GB capacity (50K × 86400s × 80 bytes / 1e9 = 345 GB/day
        → checkpoint every 10K entries reclaims ~90%)
Network: 10 GbE
```

---

## 7. Monitoring Checklist

Run these checks daily:

- [ ] `df -h /journal/path` — disk < 80% used
- [ ] `curl /prometheus | grep queue_depth` — queue < 50% capacity
- [ ] `curl /prometheus | grep p99` — latency within SLA
- [ ] `curl /health` — returns 200
- [ ] Backup replication lag < 1 second
- [ ] Journal size < 80% of checkpoint threshold
