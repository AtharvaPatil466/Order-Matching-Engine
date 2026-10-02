# Compliance Feature Matrix

> Roadmap Phase 4, Week 16: Regulatory Compliance Documentation
>
> This document maps implemented features to specific regulatory requirements.
> It is intended for auditors and regulators.
>
> **Status means what the shipped binaries do** (`OrderEngine`, `src/main.cpp`;
> `GatewayServer`, `src/gateway_main.cpp`). "Library only, not enabled" means
> the code exists and is unit-tested, but neither binary switches it on, and no
> configuration key can. Most rows were previously marked Implemented on the
> strength of library code alone.

## Regulatory Coverage

### MiFID II — RTS 6 (Algorithmic Trading)

| Requirement | Article | Implementation | File | Status |
|------------|---------|---------------|------|--------|
| Kill switch | Art. 4(1) | `GraduatedKillSwitch` with 4 escalation levels: Throttle (10% rate), SymbolHalt, GlobalHalt, Kill (checkpoint + terminate). No admission path consults its state, so no level stops an order. What is live: GatewayServer's per-participant kill message, a one-shot cancel of that participant's resting orders (not sticky, not journaled). | [GraduatedKillSwitch.h](../include/GraduatedKillSwitch.h) | ⚠️ Library only, not enabled |
| Self-trade prevention | Art. 5(1) | `SelfTradeProtection` with 4 modes: CancelResting, CancelIncoming, CancelBoth, DecreaseAndCancel. Both binaries run CancelIncoming for every participant (the default); the other modes need `setSTPMode()`, which neither binary calls. | [SelfTradeProtection.h](../include/SelfTradeProtection.h) | ⚠️ Default mode only |
| Throttling mechanisms | Art. 4(2) | Per-participant token bucket rate limiting via `RateLimiter` — off by default; OrderEngine turns it on only when a SIGHUP reload finds `rate_limit.default_rate` in its `--config` file, GatewayServer never. Kill-switch Level 1 throttle is never consulted. | [RateLimiter.h](../include/RateLimiter.h), [GraduatedKillSwitch.h](../include/GraduatedKillSwitch.h) | ⚠️ Off by default |
| Order-to-trade ratio monitoring | Art. 7 | OTR tracked per participant via `ParticipantStats`. Accessible via `/otr?participantId=X` on OrderEngine's admin port, which reports symbol 0 only. GatewayServer has no admin port. | [OrderBook.h](../include/OrderBook.h), [AdminServer.cpp](../src/AdminServer.cpp) | ⚠️ Partial |
| Market making obligations | Art. 8 | `MatchAlgorithm::ProRata` matching for designated contracts. `ParticipantRole` enum (`Regular`/`LMM`/`DMM`) with 40% floor allocation guarantee and rounding-remainder priority for market makers. Both binaries create every symbol as PriceTime and assign no roles. | [OrderBook.h](../include/OrderBook.h) | ⚠️ Library only, not enabled |

### MiFID II — RTS 7 (Direct Electronic Access)

| Requirement | Article | Implementation | File | Status |
|------------|---------|---------------|------|--------|
| Pre-trade risk limits | Art. 2 | Per-participant `RiskLimits` (max order size, max notional, max position). Checked before order acceptance, but limits default to 0 (unlimited) and neither binary calls `setRiskLimits()`; the `max_*` config keys are read by nothing. | [OrderBook.h](../include/OrderBook.h), [ParticipantRiskState.h](../include/ParticipantRiskState.h) | ⚠️ Library only, not enabled |
| Message throttling | Art. 3 | `RateLimiter` with per-participant and default rates. Token bucket algorithm. Off by default (see Art. 4(2) above). | [RateLimiter.h](../include/RateLimiter.h) | ⚠️ Off by default |

### SEC Regulation SCI

| Requirement | Rule | Implementation | File | Status |
|------------|------|---------------|------|--------|
| Capacity planning | 1001(a) | Capacity monitoring (queue depth 90%, memory 95%, disk 90%, repl lag 100ms). Runs in OrderEngine only, at the compiled-in thresholds; only the queue-depth and journal-disk checks have a data source, so the memory and replication-lag checks never fire. Webhook alerting when `alert_webhook_url` is set. | [CapacityMonitor.h](../include/CapacityMonitor.h), [docs/CapacityPlanning.md](CapacityPlanning.md) | ⚠️ Partial |
| Business continuity | 1001(b)(1) | Primary-backup replication with epoch-based fencing, heartbeat-driven failover, journal log shipping. Disabled unless `--allow-unsafe-replication` is passed, because of open CRITICAL defects (REPL-1..6: a deposed primary is never fenced; a promoted backup can execute orders twice). No binary constructs `JournalFollower`. | [ReplicationProtocol.h](../include/ReplicationProtocol.h), [JournalFollower.h](../include/JournalFollower.h) | ❌ Disabled, known defects |
| Incident reporting | 1002(b) | `IncidentLogger` writes NDJSON incident records with ns-precision timestamps, symbol, trigger condition, book state summary. Hourly rotation. Neither binary calls `enableIncidentLog()`. | [IncidentLogger.h](../include/IncidentLogger.h) | ⚠️ Library only, not enabled |
| System intrusion | 1001(a)(2) | N/A — network security is deployment-specific (firewall, TLS termination via sidecar). | — | ⚠️ Deploy-time |

### SEC Rule 15c3-5 (Market Access Rule)

| Requirement | Section | Implementation | File | Status |
|------------|---------|---------------|------|--------|
| Financial risk management | (c)(1)(i) | Pre-trade risk limits with max order size, max notional, position limits. Unlimited by default; nothing sets them (see RTS 7 Art. 2). | [ParticipantRiskState.h](../include/ParticipantRiskState.h) | ⚠️ Library only, not enabled |
| Erroneous order prevention | (c)(1)(ii) | Circuit breaker only (fixed 5%, see below). The price band defaults to off and nothing sets it; `LULDManager` is never called. | [LULDManager.h](../include/LULDManager.h), [OrderBook.h](../include/OrderBook.h) | ⚠️ Circuit breaker only |
| Kill switch | (c)(2) | `GraduatedKillSwitch` with immediate process-level termination capability. Not consulted by any order path (see RTS 6 Art. 4(1)). | [GraduatedKillSwitch.h](../include/GraduatedKillSwitch.h) | ⚠️ Library only, not enabled |

### Volatility Controls

| Feature | Standard | Implementation | File | Status |
|---------|----------|---------------|------|--------|
| LULD-style pauses | NMS Plan | `LULDManager` with configurable band % and pause duration per symbol. Reference price tracking. A member of `OrderBook` that no code path calls. | [LULDManager.h](../include/LULDManager.h) | ⚠️ Library only, not enabled |
| Circuit breakers | Exchange rules | Per-symbol circuit breaker at 5% (the setter has no caller, so it is fixed). Rejects the order and leaves the symbol trading; it no longer switches to `TradingState::VolatilityAuction`, which nothing in either binary resumed. | [OrderBook.h](../include/OrderBook.h) | ✅ Enabled |
| Price bands | Exchange rules | `priceBandPct_` admission filter rejects individual orders outside [ref±X%]. Defaults to 0 (off); `setPriceBandPct()` has no caller and `price_band_pct` is read by nothing. | [OrderBook.h](../include/OrderBook.h) | ⚠️ Library only, not enabled |

### On-Close Order Types

| Feature | Standard | Implementation | File | Status |
|---------|----------|---------------|------|--------|
| Market-on-Close (MOC) | Exchange rules | MOC orders park in `onCloseOrders_` during Continuous/PreOpen/AuctionOpen; released to auction market orders at `AuctionClose`; fill at uncross clearing price. `OrderEngine` moves books to `AuctionClose` only when started with `--session-schedule` (off by default); otherwise neither binary does, so MOC orders are accepted, never execute, and are not cancelled until a session end. | [OrderBook.h](../include/OrderBook.h) | ⚠️ Library only, not enabled |
| Limit-on-Close (LOC) | Exchange rules | LOC orders park until `AuctionClose`; admitted to limit book; unfilled remainder cancelled via `cancelLocOrders()` after uncross completes. Same as MOC: no binary drives `AuctionClose`. | [OrderBook.h](../include/OrderBook.h) | ⚠️ Library only, not enabled |

### Market Integrity

| Feature | Regulation | Implementation | File | Status |
|---------|-----------|---------------|------|--------|
| Wash trade detection | Dodd-Frank § 747 | `WashTradeDetector` with 256-bit beneficial owner bitsets. O(1) intersection check. Configurable action (flag/reject). A member of `OrderBook` that no code path calls. | [WashTradeDetector.h](../include/WashTradeDetector.h) | ⚠️ Library only, not enabled |
| Audit trail | SEC 17a-25 | Journal (with `--journal`) with `steady_clock` timestamps — monotonic, not wall-clock time — replayed on boot. Not a full trail: a checkpoint **replaces** the journal with a snapshot of resting orders, discarding every fill and cancel before it, and one runs automatically at 250,000 entries / 64 MiB and on every clean OrderEngine shutdown. | [Journal.h](../include/Journal.h) | ⚠️ Not a retained trail |
| Market data integrity | Reg NMS | Shared-memory market data feed with sequence numbers, gap detection, versioned schema. Published by GatewayServer only; OrderEngine publishes no market data. | [MarketDataPublisher.h](../include/MarketDataPublisher.h) | ✅ Implemented (GatewayServer) |
| Maker-Taker Fee Calculation | Dodd-Frank / exchange rules | `FeeEngine` calculates maker-taker rebates on every fill event — once a fee schedule is set. Neither binary sets one, so it never runs. | [FeeEngine.h](../include/FeeEngine.h) | ⚠️ Library only, not enabled |

## Observability Infrastructure

| Feature | Implementation | Endpoint |
|---------|---------------|----------|
| Prometheus metrics | `MetricsRegistry` with counter/gauge/histogram export | `GET /prometheus` |
| Health check | Liveness probe | `GET /health` |
| Order book snapshot | L2 depth (10 levels) | `GET /book?symbolId=X` |
| OTR monitoring | Per-participant stats | `GET /otr?participantId=X` |
| Structured logging | Pluggable `StructuredSink` (NullSink, JsonStderrSink, CapturingSink) | N/A (code) |
| Webhook alerts | `AlertDispatcher` with Slack, PagerDuty, Generic formats | N/A (push) |

## Formal Verification

12 TLA+ specifications cover the core safety and liveness properties of the engine.

| Component | Spec File | Invariants Verified |
|-----------|-----------|-------------------|
| MPSC Queue | `spec/MpscQueue.tla` | Linearizability, No data loss |
| Engine Consumer Loop | `spec/EngineConsumer.tla` | Shutdown completeness |
| Snapshot Consistency | `spec/Snapshot.tla` | No torn reads |
| Matching Engine | `spec/MatchingEngine.tla` | FIFO preservation, matching conservation, no negative qty, time priority at a price level. (`Quantity_Conservation` is not checked and is false; GTD expiry holds vacuously — the model places no GTD order. See [Verification.md](Verification.md).) |
| Replication Protocol | `spec/Replication.tla` | No committed loss, No duplicate execution, No split-brain — **vacuously**: under the shipped configs the backup can never promote. See [Verification.md](Verification.md). |
| Auction Protocol | `spec/Auction.tla` | Uncross price validity, admission rules |
| OCO Atomicity | `spec/Oco.tla` | No double-fill on OCO pairs |
| Risk Hierarchy | `spec/Risk.tla` | Hierarchical limit enforcement |
| FIX Session | `spec/FixSession.tla` | Session sequence safety |
| Epoch Durability | `spec/EpochDurability.tla` | Epoch entry durability |

## Audit Contact

For questions about this compliance matrix, contact the engineering team.
Document last updated: 2026-05-28.
