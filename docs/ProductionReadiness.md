# Production Readiness Notes

This project is a low-latency matching-engine implementation for research and portfolio use. It intentionally models many exchange-system concerns, but production deployment would require more work than passing the local test suite.

## Semantics To Specify Before Production

- Public submit semantics now distinguish ingress acceptance from final order-book outcome in async mode:
  `SubmitResult::Accepted` means the request passed ingress validation and was queued, while order-level
  rejects that occur on the worker are delivered through order-update/event channels. On `OrderEngine`'s
  order port an Ack is sent once the worker has run the request and its journal entry is synced, so it
  means durable; it still does not carry a worker-side reject (post-only, FOK, a cancel of an unknown id),
  and no session hears about its fills yet (roadmap 1.9-I7).
- Self-match prevention policy variants: cancel taker, cancel maker, decrement-and-cancel, and participant hierarchy rules.
- Cancel/replace priority rules for every amendment type, including hidden and iceberg orders.
- Market order protection, price collars, auction states, halt/resume transitions. What `OrderEngine` runs:
  a 5% circuit breaker on every continuous print that puts the symbol into a timed volatility
  auction (`printBand` in `src/OrderBook.cpp`; `VolatilityBreakerTest`, `PrintBandTest`) <!-- claim:circuit-breaker -->, in OrderEngine with
  a config file each symbol's static price range (`instrument.<s>.min_price` / `max_price`, enforced at
  admission; `PriceRangeTest`, `EngineRequiresPriceBandTest`) <!-- claim:price-range -->, and, only in OrderEngine with
  `--session-schedule`, the scheduled auction states (`SessionSchedulerTest`) <!-- claim:session-schedule -->, and in every book a
  5% market-order collar on both algorithms (`MarketCollarTest`). <!-- claim:market-collar --> Library only, not
  enabled: the percentage price band,
  and any halt or resume (no binary or admin endpoint sets `TradingState::Halted`).
  See [Compliance.md](Compliance.md).
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

- CRC-validated journal replay stops at truncated or corrupted records. <!-- claim:journal-crc -->
- Checkpoint replay restores every resting and parked order after journal rewrite, in whatever trading state the book was in (halted and closed included), and the book then takes its next transitions as it would have; an order a restart cannot put back stops the boot instead of being dropped (`CheckpointTradingStateTest`, `CheckpointRestoreRefusedTest`). It keeps each level's time priority, every book's last trade and reference price, a trailing stop's ratcheted trigger, a stop's election latch, a peg's price and the order the next reprice moves the pegs in, and an undated session's start (`CheckpointTimePriorityTest`, `CheckpointPriceStateTest`, `StopLatchCheckpointTest`, `CheckpointPeggedOrderTest`, `SessionTransitionTest`). <!-- claim:checkpoint-restore -->
- Randomized command streams are replayed and compared against the original final book state. <!-- claim:replay-determinism -->
- Ingress submit APIs return explicit reject reasons for stopped engines, unknown symbols, bad orders and rate limits (`SubmitResultTest`). No test drives a queue past its backpressure threshold. <!-- claim:ingress-reject-reasons -->
- Gateway requests surface ingress rejects as error responses instead of unconditional ACKs. <!-- claim:gw-reject-responses -->
- CI runs project tests under Release, ASan/UBSan, and ThreadSanitizer configurations; Ubuntu sanitizer builds also run an E2E benchmark smoke check.
- `Journal::now()` uses `steady_clock`, so journal timestamps survive NTP step adjustments.
- `Journal` constructor unlinks any stale `<path>.tmp` from a crashed `rewriteAtomically()`.
- MOC/LOC on-close orders: `cancelLocOrders()` correctly triggered on both the `!hasCross` early-return path and normal uncross completion. Price validation exclusion for MOC (price=0 is valid). Verified by `MocLocTest` and `OnCloseSessionEndTest`. <!-- claim:moc-loc -->
- LMM/DMM ProRata floor guarantee: 40% floor allocation with correct redistribution (does not inflate `allocated` counter, preventing unsigned underflow). Verified by the `LmmDmm` cases in `TestLmmDmmAllocation.cpp`. <!-- claim:prorata-lmm-floor -->
- Config SIGHUP hot-reload: `RateLimiterReconfigureTest` covers the `reconfigure()` a reload calls. No ctest sends SIGHUP to the binary; Runbook §6 under `scripts/runbook_smoke.sh` (CI job `runbook-smoke`) does. <!-- claim:rate-limit-reload -->

## Admin Endpoints

- **Auth gating on `/chaos/order`**: off unless `OB_CHAOS_INJECT=1` and `OB_CHAOS_TOKEN` are set; refuses a missing or wrong `X-Chaos-Token` even with a valid bearer token (`AdminAuthTest`). <!-- claim:admin-auth -->
- **`/readyz` k8s readiness probe**: HTTP 503 until `admin.setReady(true)` is called, which is after journal replay has finished, then 200. Auth-exempt (same as `/health`). `AdminAuthTest` covers both probes, including `/readyz` before and after ready. <!-- claim:admin-readyz -->
- **Build-metadata endpoint**: `/version` returns `gitSha` + `buildTime` for ops.

## Benchmark Artifact Workflow

Run `./scripts/run_benchmarks.sh` on an idle machine. The script writes a timestamped artifact directory containing:

- `machine.txt` with OS, compiler, and CPU metadata.
- `manual_benchmark.txt` for processing-latency measurements.
- `e2e_benchmark.txt` for ingress-to-completion measurements.

README latency tables should be updated only from saved artifacts, preferably using the median of repeated runs.

## Operational Work Required

- ~~Structured logging with stable event schemas~~ — done. 7 call sites wired in `OrderBook.cpp` via typed helpers in `StructuredLog.h` (order accepted ×2 paths, cancelled, rejected/risk, trade fill, circuit breaker, trading state change). `StructuredSink` backends remain hot-swappable.
- Metrics contracts for order flow, rejects, queue depth, latency histograms, journal health, and gateway sessions.
- Configuration files for symbols, risk limits, gateway ports, admin endpoints, rate limits, and persistence.
- Deployment and recovery runbooks (started — see `docs/Runbook.md`).
- Hot-reload via SIGHUP is now wired: `--config PATH` CLI flag in `src/main.cpp`; async-signal-safe `g_reload_config` atomic; SIGHUP sets flag; main event loop calls `cfg.loadFile(configPath)`. SIGHUP now also calls `RateLimiter::reconfigure()` — updates default rate/burst from config keys `rate_limit.default_rate` / `rate_limit.default_burst` and clears all participant token buckets (`RateLimiterReconfigureTest` covers this path). <!-- claim:rate-limit-reload --> Journal auto-rotation is wired in the main loop: `OB_JOURNAL_MAX_SIZE_MB` env var sets the threshold; main loop calls `engine.checkpoint()` when `Journal::needsCheckpoint()` fires. Remaining cold-restart-only settings: thread count, journal path, and ports.
- Backward-compatible protocol versioning for gateway and market-data consumers.
- **Admin auth: done — TLS still open.** Every admin endpoint except the k8s probes (`/health`, `/readyz`) requires `Authorization: Bearer <OB_ADMIN_TOKEN>` (constant-time compare, JSON 401 otherwise). Wired via `--admin-token` / `OB_ADMIN_TOKEN` in `src/main.cpp`; `docker-compose.yml` fails closed if the token is unset, and `AdminAuthTest` pins the 401/200 contract in-process. <!-- claim:admin-auth --> The port binds `127.0.0.1` unless `--admin-bind` / `OB_ADMIN_BIND` widens it (the Docker image sets `0.0.0.0`; `AdminBindsLoopbackTest` probes it from a non-loopback address). <!-- claim:admin-loopback --> Remaining: TLS — terminate at a reverse proxy or ingress in front of the admin port; the in-process server intentionally stays plain HTTP.

## Architectural Gaps Worth Documenting

- **No high availability.** A single node recovers by replaying its journal; a host loss is an outage. Replication was removed rather than finished (roadmap 1.1).
