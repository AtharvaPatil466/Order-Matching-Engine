# Operational Runbook — Order Matching Engine

> This document covers day-to-day operations, incident response, and maintenance procedures for the matching engine deployment.

---

## 1. Starting the Engine

### Bare Metal
```bash
# Load config and start with 4 worker threads
./bin/OrderEngine --threads 4 --port 8080 --symbols 4

# With config file (supports SIGHUP hot-reload)
./bin/OrderEngine --threads 4 --port 8080 --symbols 4 --config /etc/orderengine/engine.conf

# Hot-reload config without restart
kill -SIGHUP $(pidof OrderEngine)

# Lean mode (disables risk checks — HFT deployments only)
./bin/OrderEngine --lean --threads 4 --port 8080 --symbols 4
```

### Docker
```bash
docker-compose up -d engine-primary
# Verify health
curl -sf http://localhost:8080/health
```

### Systemd
```ini
[Unit]
Description=Order Matching Engine
After=network.target

[Service]
Type=simple
ExecStart=/opt/orderengine/bin/OrderEngine --threads 4 --port 8080 --symbols 4
WorkingDirectory=/opt/orderengine
Restart=on-failure
RestartSec=5
LimitNOFILE=65536
LimitMEMLOCK=infinity
CPUAffinity=0-3

[Install]
WantedBy=multi-user.target
```

---

## 2. Health Monitoring

### Endpoints

| Endpoint | Purpose | Expected Response |
|----------|---------|-------------------|
| `GET /health` | K8s liveness probe | `200 OK {"status":"healthy"}` |
| `GET /readyz` | K8s readiness probe | Returns 503 until engine warmup completes, then 200. Use as k8s `readinessProbe`. Auth-exempt. |
| `GET /metrics` | Internal counters | JSON with throughput, queue depth, latency |
| `GET /prometheus` | Prometheus scrape | Text exposition format |
| `GET /book?symbolId=0` | L2 book snapshot | JSON with bids/asks/trades |
| `GET /otr?participantId=1` | OTR ratio | JSON with order/trade counts |

### Prometheus Scrape Config
```yaml
scrape_configs:
  - job_name: 'orderengine'
    scrape_interval: 5s
    static_configs:
      - targets: ['localhost:8080']
    metrics_path: '/prometheus'
```

### Key Metrics to Alert On

| Metric | Warning | Critical | Action |
|--------|---------|----------|--------|
| `queue_depth` | > 50% capacity | > 80% capacity | Check consumer threads |
| `orders_rejected_total` (rate) | > 100/s | > 1000/s | Check rate limits / risk config |
| `processing_latency_p99` | > 1μs (lean) / > 5μs (full) | > 10μs | Check CPU affinity, competing workloads |
| `journal_entries_committed_total` (rate) | — | drops to 0 | Journal disk may be full |
| Health endpoint | — | Returns non-200 | Restart engine |

---

## 3. Incident Response

### Engine Unresponsive (Health Check Failing)
1. Check process is running: `ps aux | grep OrderEngine`
2. Check logs: `journalctl -u orderengine -n 100`
3. Check disk space (journal): `df -h /var/lib/orderengine/`
4. If OOM: increase memory limit, check for leak
5. If stuck: `kill -SIGABRT <pid>` to get core dump, then restart

### Circuit Breaker Triggered
1. Check `/metrics` for which symbol halted
2. Review the price move that triggered it
3. If legitimate: wait for `halt_duration_ms` to auto-resume
4. If erroneous: manually resume via admin endpoint
5. Post-incident: review if `price_band_pct` is too tight

### Kill Switch Activated
1. All orders for the participant are cancelled immediately
2. Check OTR ratio at `/otr?participantId=<id>`
3. Review the participant's recent order flow
4. Re-enable by restarting the engine (kill switch is sticky per session)

### Journal Corruption Detected
1. Engine will log a CRC mismatch and halt replay
2. **Do NOT delete the journal** — it's the audit trail
3. Copy the corrupt journal: `cp journal.wal journal.wal.corrupt`
4. Truncate to the last valid entry: the engine's `replayJournal()` stops at the first corruption
5. Restart the engine — it will replay up to the corruption point
6. Manual reconciliation may be needed for orders after the corruption

### High Latency / Queue Buildup
1. Check CPU affinity: `taskset -p <pid>` — workers should be pinned
2. Check for competing processes: `top -H -p <pid>`
3. Check NUMA topology: workers should be on the same NUMA node
4. Check journal disk latency: `iostat -x 1`
5. If disk-bound: switch to `SyncPolicy::GroupCommit` (default) or `None`
6. If CPU-bound: increase thread count or reduce symbol count per thread

---

## 4. Backup / Recovery

### Journal Checkpoint
The engine automatically checkpoints when the journal exceeds `checkpoint_entries` or `checkpoint_bytes`. Manual checkpoint:

```bash
# Via the engine's API (if exposed), or by sending a signal:
kill -USR1 <pid>  # triggers checkpoint if wired
```

### Disaster Recovery from Journal
```bash
# Start a fresh engine pointed at the existing journal
./bin/OrderEngine --journal /var/lib/orderengine/journal.wal --replay-only

# The engine will replay all journal entries and reach the last consistent state
```

### Backup Promotion
If the primary fails and a backup is running:
1. The backup's `HeartbeatMonitor` detects the failure (within `heartbeat_timeout_ms`)
2. The backup acquires a new `LeaderLease` with a higher epoch
3. The backup calls `JournalFollower::promote()` to become writable
4. Clients reconnect to the backup's gateway port

---

## 5. Configuration Changes

### Hot-Reloadable Settings
The following can be changed without restart:

**Via SIGHUP** (send `kill -SIGHUP <pid>` — reloads the config file):
- Any setting in the config file pointed to by `--config PATH`
- Rate limit parameters, alert webhooks, symbol config
- `RateLimiter::reconfigure()` is now called on every SIGHUP: reads `rate_limit.default_rate` and `rate_limit.default_burst` from the reloaded config, updates the default rate/burst, and clears all existing participant token buckets so they inherit the new limits on their next request.

**Via environment variables** (take effect on next config reload or restart):
- `OB_RATE_LIMIT_*` — per-participant rate limit parameters
- `OB_ALERT_WEBHOOK_URL` — webhook target
- `OB_JOURNAL_MAX_SIZE_MB` — maximum journal file size in megabytes before auto-checkpoint. Default: disabled (0). Set to e.g. `256` to rotate at 256 MB. When the threshold is reached, `Journal::needsCheckpoint()` fires and the main loop calls `engine.checkpoint()` automatically.

### Cold Settings (Require Restart)
- Thread count, symbol count
- Journal path
- Network ports
- Node role (primary/backup)

### Config Precedence
1. Environment variable `OB_<KEY>` (highest)
2. Config file value
3. Compiled default (lowest)

---

## 6. Capacity Planning

See [CapacityPlanning.md](./CapacityPlanning.md) for detailed sizing guidance.

Quick reference:
- **Memory**: ~1KB per live order + ObjectPool overhead. 1M orders ≈ 1.5GB
- **CPU**: 1 core per symbol-partition thread + 1 for gateway + 1 for admin
- **Disk**: Journal grows at ~100 bytes/entry. At 1M orders/day ≈ 100MB/day
- **Network**: Each FIX message is 200-500 bytes. At 10K orders/sec ≈ 5 MB/s

---

## 7. Maintenance Windows

### Pre-Market
1. Verify journal replays cleanly: `./bin/OrderEngine --replay-only`
2. Check disk space: need at least 10x daily journal size free
3. Run health check: `curl localhost:8080/health`
4. Verify time sync: `chronyc tracking` — clock skew < 1ms

### Post-Market
1. Archive the journal: `cp journal.wal journal.$(date +%Y%m%d).wal`
2. Run the benchmark regression suite: `./bin/ManualBenchmark`
3. Check for latency regression in P99 numbers
4. Rotate logs if using `stderr` sink

### Weekly
1. Run full test suite: `./build.sh && ./bin/ManualTest && ./bin/StressTest`
2. Review Prometheus metrics for trends
3. Check backup replication lag
