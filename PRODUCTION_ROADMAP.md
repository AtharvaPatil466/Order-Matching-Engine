# Order Matching Engine: Tier-1 Production Roadmap

This document outlines the architectural, operational, and regulatory delta between the current hyper-optimized C++ matching engine prototype and a **Tier-1 Institutional Production Exchange** (e.g., NASDAQ, CME, or a top-tier Alternative Trading System).

While the core algorithmic latency (~40ns adds, ~70ns matches) is state-of-the-art for user-space, deploying a real financial venue requires solving hardware networking, distributed consensus, and rigorous mathematical verification.

---

## 1. True Kernel Bypass & Networking
To achieve physical wire-to-wire low latency, the OS networking stack must be entirely bypassed.
- [ ] **DPDK / Solarflare ef_vi:** Implement direct NIC-to-user-space memory access. (Note: `io_uring` is kernel-assisted, not true bypass).
- [ ] **Binary Wire Protocols:** Transition from the text-based FIX parser to binary protocols like SBE (Simple Binary Encoding), NASDAQ ITCH/OUCH, or CME MDP 3.0.
- [ ] **FIX Session Layer State Machine:** Implement full session management including heartbeat, test-request, resend-request, gap-fill, sequence-reset, and logon-auth state machines.
- [ ] **Multicast Market Data:** Replace local SHM IPC with UDP + PIM-SM Multicast for market data, ensuring A/B feed redundancy and snapshot + incremental recovery.
- [ ] **PCAP Tooling:** Build packet capture (PCAP) and replay tooling for zero-latency backtesting and postmortem analysis.

## 2. Distributed Consensus & High Availability (HA)
A production exchange must survive catastrophic hardware failure without losing a single financial transaction.
- [ ] **Replicated State Machine (consensus):** Current design is deterministic primary-backup with lease-fenced promotion (the CME / NYSE Arca pattern), not Raft/Paxos. Adding N-replica consensus would be a substantial architecture pivot.
- [x] **Hot Standby & Failover:** Primary-backup wired end-to-end in `src/main.cpp` (`OB_NODE_ROLE` driven). Backup runs in replay mode applying journal-entry messages; auto-reconnects on socket loss; `setReplayModeAllBooks(false)` on promotion. Verified live via `deploy/chaos/test_failover_detection.py` (~400ms RTO) and `test_rolling_restart.py`. Reconnect backoff: exponential 500ms → 30s cap on peer-reconnect retries (no more 500ms socket storms during sustained primary outages).
- [ ] **Cross-DC Disaster Recovery:** Async cross-DC replication still unimplemented. Chaos suite uses 1+1 same-host containers; multi-region needs WAN-aware transport tuning.
- [x] **Split-Brain Protection:** `LeaderLease` epoch-based fencing + `LeaseGrant` propagation. `BackupPromote` requires heartbeat timeout AND local lease expiry. Verified empirically (chaos suite scenarios: bidirectional partition, asymmetric partition, 30% packet loss, +30s backup clock skew — all 0 split brain) and formally (TLA+ `Replication.tla` lease-propagation model, MaxEntries=10).

## 3. Advanced Exchange Semantics
Real-world venues require complex business logic and state transitions to manage market integrity.
- [x] **Auction State Machines:** Implement Pre-Open, Opening/Closing crosses, Volatility Auctions, and Halt-Resume mechanics. Implemented: `TradingState` enum with PreOpen, AuctionOpen, AuctionClose, Halted, VolatilityAuction. Opening/closing crosses via `uncross()`. Verified by TLA+ `Auction.tla` + `TestAuctionSession` test suite.
- [x] **Market Protection:** Implement price collars and market-order protection (preventing sweeps that clear the entire book). LULD-style price collars (`priceBandPct_` admission filter), volatility circuit breakers halting into `VolatilityAuction` state, market-order protection against full-book sweep.
- [x] **Advanced Order Types:** Support Implied, Spread, Cross, Complex orders, OCO/OSO/Bracket, MIT, MOC/LOC, and Peg-to-Primary. MIT (Market-if-Touched), MOC (Market-on-Close), LOC (Limit-on-Close), OCO/OSO implemented. All existing types: Limit/Market/IOC/FOK/Stop/StopLimit/TrailingStop/Pegged/Iceberg/Hidden/PostOnly retained. Verified by `TestAdvancedOrders`, `TestMocLocOrders`.
- [x] **Venue-Specific Allocation:** Formalize priority tiers, LMM/DMM privileges, and exact Iceberg refresh / hidden-order allocation priorities. `ParticipantRole` enum (Regular/LMM/DMM). ProRata matching guarantees 40% floor allocation to LMM/DMM orders; rounding remainders go to privileged roles first. Iceberg refresh retains queue position. Verified by `TestLmmDmmAllocation`.
- [ ] **Expanded SMP Variants:** Add Cancel-Maker, Decrement-and-Cancel, and hierarchy-aware Self-Match Prevention (currently only Cancel-Taker).

## 4. Risk, Clearing, and Compliance
Handling institutional money requires rigorous pre-trade and post-trade guardrails.
- [x] **Multi-Tier Limits:** Persist limits across Firm, Account, Strategy, and Trader hierarchies. `HierarchicalRiskManager` enforces Firm/Account/Strategy/Trader hierarchy. Limits applied before order enters matching. Verified by `TestHierarchicalRisk`.
- [x] **Pre-Trade Fat-Finger Checks:** Block orders based on maximum notional size, reference price limits, and percentage away from the NBBO. Max notional, max order size, and percentage-from-NBBO checks in `HierarchicalRiskManager`. Verified by `TestHierarchicalRisk` and `TestCompliance`.
- [x] **Regulatory Audit Trail:** Support CAT (Consolidated Audit Trail), MiFID II transaction reports, and RTS 24/25 record retention. `CatReporter` writes NDJSON event records (order accept/cancel/fill) with nanosecond timestamps for CAT and MiFID II RTS 24/25. Hourly rotation. Verified by `TestCatReporter`.
- [ ] **Clock Synchronization:** Achieve PTP / NTP clock sync to MiFID II RTS 25 microsecond tolerances.
- [ ] **Clearing Handoff (STP):** Build post-trade Straight-Through Processing (STP) pipelines.
- [x] **Fee/Rebate Engine:** Real-time calculation of maker-taker schedules. `FeeEngine` computes maker-taker schedules on every fill event in real time. Verified by `TestFeeEngine`.

## 5. Formal Verification & QA
Unit tests are insufficient for lock-free concurrency. The engine must survive mathematical and adversarial scrutiny.
- [x] **TLA+ Modeling:** 7 specs. `MatchingEngine.tla` exhaustively verified at 454M states. `Replication.tla` has a realistic lease-propagation model (no god-mode primary-alive guard) verified at MaxEntries=10. `MpscQueue.tla` proves linearizability. `Snapshot.tla` + `SnapshotLocked.tla` proved the snapshot fix. Bug-injected sanity check on the Replication spec reproduces split brain — verification is genuine, not vacuous.
- [ ] **Long-Running Soak Tests:** Harness exists (`scripts/tsan_soak.sh`); 24h+ runs still clock time bound.
- [x] **Coverage-Guided Fuzzing (partial):** libFuzzer harnesses on FIX framer + parser; corpora committed. Journal replay fuzzer in `tools/`.
- [x] **Chaos / Jepsen-Style Fault Injection:** **19-scenario multi-container chaos suite** in `deploy/chaos/`. Covers kill, partition (sym/asym), 30% packet loss, slow link, pause/recovery, rolling restart, clock skew (libfaketime), snapshot catchup on join, committed-loss safety under SIGKILL, token-auth on chaos endpoint, Prometheus replication counters, plus steady-state regression pins. Runs against the actual running binaries in Docker Compose.
- [x] **Byte-Identical Determinism:** `JournalReplayPropertyTest` verifies randomized command streams replay to the original final book state. `JournalCrashTest` covers crash recovery determinism.

## 6. Observability, Metrics & Ops
If you cannot measure it in production, you cannot operate it.
- [x] **Structured Logging:** `StructuredSink` API (`NullSink`, `JsonStderrSink`, `CapturingSink`) + 7 typed call sites wired in `OrderBook.cpp` (order accepted, cancelled, rejected, trade fill, circuit breaker, trading state change). Typed helper functions in `StructuredLog.h` enforce required fields. Backends swap without changing call sites. Verified by `StructuredLogTest`.
- [x] **Prometheus Metrics (partial):** `/prometheus` text-exposition endpoint exposes counters/gauges/histograms including `journal_entries_committed_total`, `replication_entries_shipped_total`, `replication_bytes_sent_total`, `replication_snapshot_streams_total`, `replication_snapshot_entries_total`. OpenTelemetry tracing not yet integrated.
- [x] **k8s Readiness Probe (`/readyz`):** Separate from the liveness `/health` probe. Returns HTTP 503 until `admin.setReady(true)` is called after warmup (or backup replay mode entry). Auth-exempt so k8s readinessProbe works without credentials. Verified by `AdminAuthTest`.
- [x] **`/version` Build-Metadata Endpoint:** Returns `gitSha` (12-char) + UTC `buildTime` + `engineVersion`. Captured at CMake configure time; falls back to `"unknown"` for Docker builds that exclude `.git/`.
- [ ] **Hardware Tuning Runbooks:** Document exact OS-level tuning including NUMA pinning, hugepages allocation, IRQ steering, and CPU core isolation. (See `docs/Runbook.md` for the runbook scaffold.)
- [x] **Configuration Management:** `Config` class with `OB_`-prefixed env override; per-key Listener callbacks fire on set/loadFile/loadMap. [x] **Config Hot-Reload via SIGHUP:** `--config PATH` CLI flag; async-signal-safe `g_reload_config` atomic; SIGHUP handler sets flag; main event loop calls `cfg.loadFile(configPath)` on receipt. Rate limiter live reconfiguration: SIGHUP propagates to `RateLimiter::reconfigure()` — all participant token buckets reset to new defaults so no client is grandfathered. Journal auto-rotation: `OB_JOURNAL_MAX_SIZE_MB` triggers `engine.checkpoint()` each main-loop tick when the size limit is exceeded.
- [x] **Protocol Versioning:** SBE forward-compat proven by test (v1 reader reads v2 prefix unchanged); FIX/OUCH/ITCH versioning not yet codified as a contract. `ProtocolVersion.h` — wire versioning enum with backward/forward compat. `ProtocolVersioningTest` proves v1 reader reads v2 prefix unchanged.

## 7. Performance Methodology Upgrade
Benchmarking must move beyond localized user-space averages.
- [x] **HdrHistogram Tracking:** Implement coordinated-omission-free end-to-end latency tracking. `LatencyHistogram` header implements coordinated-omission-free end-to-end latency tracking with percentile export. Verified by `TestLatencyHistogram`.
- [ ] **Hardware Perf Counters:** Wire LLC misses and branch mispredictions directly into the benchmark suite.
- [ ] **Linux Isolated Cores Validation:** Transition benchmarks from macOS/M-series to production Linux servers with isolated cores and disabled hyperthreading.
