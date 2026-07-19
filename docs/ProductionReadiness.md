# Production Readiness Notes

This project is a low-latency matching-engine implementation for research and portfolio use. It intentionally models many exchange-system concerns, but production deployment would require more work than passing the local test suite.

## Semantics To Specify Before Production

- Public submit semantics now distinguish ingress acceptance from final order-book outcome in async mode:
  `SubmitResult::Accepted` means the request passed ingress validation and was queued, while order-level
  rejects that occur on the worker are delivered through order-update/event channels.
- Self-match prevention policy variants: cancel taker, cancel maker, decrement-and-cancel, and participant hierarchy rules.
- Cancel/replace priority rules for every amendment type, including hidden and iceberg orders.
- ~~Market order protection, price collars, auction states, halt/resume transitions~~: Implemented (LULD, VolatilityAuction, TradingState enum, RejectReason contract).
- Iceberg refresh priority and hidden-order allocation rules for the exact venue model.
- End-to-end sequence guarantees for trades, order updates, market data, replay, and gap recovery.
- Risk-limit policy for open orders, filled positions, rejected orders, cancels, and cross-symbol exposure.

## Validation Required

- Sanitizer runs: AddressSanitizer, UndefinedBehaviorSanitizer, and ThreadSanitizer.
- Long-running randomized soak tests with fixed seeds saved on failure.
- Concurrency tests for shutdown, backpressure, rate limiting, kill switch, snapshot reads, and checkpoint/replay.
- Crash tests covering partial journal writes, checkpoint replacement, corrupted records, and replay idempotency.
- Benchmark runs with repeated samples, hardware details, compiler version, power settings, and variance.

## Validation Already Covered In This Repo

- CRC-validated journal replay stops at truncated or corrupted records.
- Checkpoint replay restores active orders after journal rewrite.
- Randomized command streams are replayed and compared against the original final book state.
- Ingress submit APIs return explicit reject reasons for stopped engines, unknown symbols, bad orders, rate limits, and queue backpressure.
- Gateway requests surface ingress rejects as error responses instead of unconditional ACKs.
- CI runs project tests under Release, ASan/UBSan, and ThreadSanitizer configurations; Ubuntu sanitizer builds also run an E2E benchmark smoke check.
- `ReplicationProtocolTest` is now TSan-clean and runs under the TSan job. Previously excluded due to atomic-fd and `sendSeq_` races during teardown — closed by atomic fds + `shutdown(2)` before `close()` + atomic `sendSeq_`.
- `Journal::now()` uses `steady_clock`, so journal timestamps survive NTP step adjustments.
- `Journal` constructor unlinks any stale `<path>.tmp` from a crashed `rewriteAtomically()`.
- MOC/LOC on-close orders: `cancelLocOrders()` correctly triggered on both the `!hasCross` early-return path and normal uncross completion. Price validation exclusion for MOC (price=0 is valid). Verified by `TestMocLocOrders` (11 cases).
- LMM/DMM ProRata floor guarantee: 40% floor allocation with correct redistribution (does not inflate `allocated` counter, preventing unsigned underflow). Verified by `TestLmmDmmAllocation` (7 cases).
- Config SIGHUP hot-reload: Async-signal-safe pattern verified by `TestPaperTrader` integration path and manual test.

## Live Multi-Container Chaos Suite (`deploy/chaos/`)

19 scenarios run against the actual `OrderEngine` binary inside Docker Compose. The replication path that was previously library-only (verified by unit test but not exercised in the running binary) is now wired into `src/main.cpp` via `OB_NODE_ROLE` / `OB_PRIMARY_HOST` / `OB_JOURNAL_PATH` env vars and validated end-to-end.

Empirically verified:

- **NoCommittedLoss under SIGKILL**: 200 orders → 192 primary-committed → 192 on backup. Same property the TLA+ spec proves, now measured live.
- **No split brain** under: bidirectional partition, asymmetric partition, 30% packet loss, +30s clock skew on backup (via `libfaketime`).
- **Snapshot catchup on backup join**: primary streams all currently-resting orders via `MatchingEngine::streamSnapshot` when a backup connects. Closes the rolling-restart gap where pre-restart entries would otherwise never reach a late-joining backup.
- **Transport auto-reconnect**: backup re-runs `connectTo()` from its receive loop after socket loss — bilateral recovery in ~3 ms after primary recovery, no external orchestration needed. Reconnect now uses exponential backoff (500ms → 30s cap), resetting to base delay on each successful connect. `ReconnectBackoffTest` covers progression and reset.
- **Lease propagation under partial failure**: backup's local lease is refreshed by every primary heartbeat-tick `LeaseGrant`; `BackupPromote` is gated on local lease expiry, not just heartbeat miss.
- **Auth gating on `/chaos/order`**: rejects missing / wrong token, accepts matching `X-Chaos-Token` header when `OB_CHAOS_TOKEN` is set.
- **Observability**: Prometheus counters (`replication_entries_shipped_total`, `_bytes_sent_total`, `_snapshot_streams_total`, `_snapshot_entries_total`) advance under load and on backup rejoin.
- **`/readyz` k8s readiness probe**: HTTP 503 until `admin.setReady(true)` is called after engine warmup, then 200. Auth-exempt (same as `/health`). `AdminAuthTest` covers 7 scenarios including readyz pre/post-warmup transitions.
- **Build-metadata endpoint**: `/version` returns `gitSha` + `buildTime` for ops.

See [deploy/chaos/README.md](../deploy/chaos/README.md) for the full scenario catalog, RTO/RPO measurements, and instructions to run the suite locally.

## Benchmark Artifact Workflow

Run `./scripts/run_benchmarks.sh` on an idle machine. The script writes a timestamped artifact directory containing:

- `machine.txt` with OS, compiler, and CPU metadata.
- `manual_benchmark.txt` for processing-latency measurements.
- `e2e_benchmark.txt` for ingress-to-completion measurements.

README latency tables should be updated only from saved artifacts, preferably using the median of repeated runs.

## Operational Work Required

- ~~Structured logging with stable event schemas~~ — done. 7 call sites wired in `OrderBook.cpp` via typed helpers in `StructuredLog.h` (order accepted ×2 paths, cancelled, rejected/risk, trade fill, circuit breaker, trading state change). `StructuredSink` backends remain hot-swappable.
- Metrics contracts for order flow, rejects, queue depth, latency histograms, journal health, and gateway sessions. Replication metrics are in (see chaos suite section above).
- Configuration files for symbols, risk limits, gateway ports, admin endpoints, rate limits, and persistence.
- Deployment and recovery runbooks (started — see `docs/Runbook.md`).
- Hot-reload via SIGHUP is now wired: `--config PATH` CLI flag in `src/main.cpp`; async-signal-safe `g_reload_config` atomic; SIGHUP sets flag; main event loop calls `cfg.loadFile(configPath)`. SIGHUP now also calls `RateLimiter::reconfigure()` — updates default rate/burst from config keys `rate_limit.default_rate` / `rate_limit.default_burst` and clears all participant token buckets (`RateLimiterReconfigureTest` covers this path). Journal auto-rotation is wired in the main loop: `OB_JOURNAL_MAX_SIZE_MB` env var sets the threshold; main loop calls `engine.checkpoint()` when `Journal::needsCheckpoint()` fires. Remaining cold-restart-only settings: thread count, journal path, and ports.
- Backward-compatible protocol versioning for gateway and market-data consumers.
- **Admin auth: done — TLS still open.** Every admin endpoint except the k8s probes (`/health`, `/readyz`) requires `Authorization: Bearer <OB_ADMIN_TOKEN>` (constant-time compare, JSON 401 otherwise). Wired via `--admin-token` / `OB_ADMIN_TOKEN` in `src/main.cpp`; `docker-compose.yml` fails closed if the token is unset, and the chaos topology runs with auth enabled so every scenario exercises the authenticated path (`test_chaos_endpoint_auth.py` pins the 401/200 contract, `AdminAuthTest` covers it in-process). Remaining: TLS — terminate at a reverse proxy or ingress in front of the admin port; the in-process server intentionally stays plain HTTP.

## Architectural Gaps Worth Documenting

- **`GatewayServer` runs its own embedded `MatchingEngine`**, not the replicated `OrderEngine`. Wiring FIX through the replication topology would require merging the two binaries or building an order-forwarding RPC between them. The same safety properties are verified via `/chaos/order` (calls `engine.submitOrder` directly, the same path FIX converges on after parsing) plus the existing in-tree `FixTcpGatewayTest`.
- **Snapshot stream concurrency: fixed.** The wire protocol brackets every snapshot with `SnapshotStart`/`SnapshotEnd`. The backup engages buffering the moment it connects, drops entries that raced ahead of `SnapshotStart` (their effects are already inside the snapshot cut), and replays the buffered window in arrival order at `SnapshotEnd`. Arrival order is safe because `streamSnapshot` ships each captured order while still holding the book lock — a Cancel for a captured order can only be applied (and therefore shipped) after its Snapshot entry is on the wire, so Cancel-before-Insert phantoms cannot occur. `TEST(SnapshotOrdering)` in `ReplicationProtocolTest` pins the drop-and-replay semantics; `src/main.cpp` registers both the commit hook and the snapshot-on-join hook before `startAsPrimary()` so no backup can connect before the bracket is armed. Residual (documented, availability not correctness): a backup that promotes mid-snapshot — primary dies between `SnapshotStart` and `SnapshotEnd` — leaves its buffered window unapplied and promotes from its own journal state.
- **TOCTOU on `peerFd_` during stop()**: after the atomic-fd / `shutdown(2)` fix, all TSan races are gone. A theoretical residual: between a thread loading `peerFd_` and using it in a syscall, `stop()` could close the fd and the kernel could reuse the number. Not observable in practice (stop is teardown-only), would need fd refcounting for a complete fix.
