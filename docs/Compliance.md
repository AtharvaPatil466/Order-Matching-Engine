# Compliance Feature Matrix

> This document maps engine features to regulatory requirements. It is
> intended for auditors and regulators.
>
> **Status means what the shipped binaries do** (`OrderEngine`, `src/main.cpp`;
> `GatewayServer`, `src/gateway_main.cpp`). A row that says a feature runs
> cites, in its **Wiring · test** column, the line in a binary that turns it
> on and the ctest that checks it. "Library only, not enabled" means the code
> exists and is unit-tested, but neither binary switches it on and no
> configuration key can; those rows have no wiring to cite. Most rows were
> once marked Implemented on the strength of library code alone.

## Regulatory Coverage

### MiFID II — RTS 6 (Algorithmic Trading)

| Requirement | Article | Implementation | File | Wiring · test | Status |
|------------|---------|---------------|------|---------------|--------|
| Kill switch | Art. 4(1) | `GraduatedKillSwitch` with 4 escalation levels: Throttle (10% rate), SymbolHalt, GlobalHalt, Kill. No admission path consults its state, so no level stops an order. What is live is GatewayServer's per-participant kill message: a one-shot cancel of that participant's resting orders, each cancel journaled. It is not sticky: the participant can send new orders straight away. | [GraduatedKillSwitch.h](../include/GraduatedKillSwitch.h), [TcpGateway.cpp](../src/TcpGateway.cpp) | Kill message: `src/gateway_main.cpp:644` (TcpGateway) → `src/TcpGateway.cpp:714-715` (`engine_.killSwitch`) · `KillJournalTest`, `GatewayLoginTest` (KillSwitchRequiresMatchingCredential). `GraduatedKillSwitch`: — | ⚠️ Per-participant cancel only |
| Self-trade prevention | Art. 5(1) | `SelfTradeProtection` with 4 modes: CancelResting, CancelIncoming, CancelBoth, DecreaseAndCancel. Both binaries run the default, which cancels the incoming order, for every participant; the other modes need `setSTPMode()`, which neither binary calls. | [SelfTradeProtection.h](../include/SelfTradeProtection.h) | Default: `include/OrderBook.h:362`, applied in the match loop at `src/OrderBook.cpp:1362-1384` · `StpSemanticsTest` (StpSemantics.ModeNoneSelfCrossReportsCancelledBySTPNotFilled); `NoSelfTrade` in `spec/MatchingEngine.tla` (tla.yml) | ⚠️ Default mode only |
| Throttling mechanisms | Art. 4(2) | Per-participant token bucket (`RateLimiter`), off by default. OrderEngine turns it on only when a SIGHUP reload finds `rate_limit.default_rate` above 0 in its `--config` file; GatewayServer never. Kill-switch Level 1 throttle is never consulted. | [RateLimiter.h](../include/RateLimiter.h) | `src/main.cpp:572-578` (SIGHUP → `reconfigure`), checked per order at `src/MatchingEngine.cpp:1583` · `RateLimiterReconfigureTest`; the real binary's reload is exercised by Runbook §6 under `scripts/runbook_smoke.sh` (not in ctest) | ⚠️ Off by default |
| Order-to-trade ratio monitoring | Art. 7 | OTR tracked per participant via `ParticipantStats`. `/otr?participantId=X` on OrderEngine's admin port reports symbol 0 only. GatewayServer has no admin port. | [OrderBook.h](../include/OrderBook.h), [AdminServer.cpp](../src/AdminServer.cpp) | `src/main.cpp:473` (AdminServer) → `src/AdminServer.cpp:352`, symbol 0 at `:472` · counting: `CriticalFixesTest` (AuditFixes.CancelDoesNotInflateOrderToTradeRatio). No test requests `/otr`. | ⚠️ Partial, endpoint untested |
| Market making obligations | Art. 8 | `MatchAlgorithm::ProRata` matching for designated contracts. `ParticipantRole` enum (`Regular`/`LMM`/`DMM`) with 40% floor allocation guarantee and rounding-remainder priority for market makers. Both binaries create every symbol as PriceTime and assign no roles. | [OrderBook.h](../include/OrderBook.h) | — | ⚠️ Library only, not enabled |

### MiFID II — RTS 7 (Direct Electronic Access)

| Requirement | Article | Implementation | File | Wiring · test | Status |
|------------|---------|---------------|------|---------------|--------|
| Pre-trade risk limits | Art. 2 | Per-participant `RiskLimits` (max order size, max notional, max position). Checked before order acceptance, but limits default to 0 (unlimited) and neither binary calls `setRiskLimits()` or `setPositionLimit()`; the `max_*` config keys are read by nothing. | [OrderBook.h](../include/OrderBook.h), [ParticipantRiskState.h](../include/ParticipantRiskState.h) | — | ⚠️ Library only, not enabled |
| Message throttling | Art. 3 | `RateLimiter` with per-participant and default rates. Off by default (see Art. 4(2) above). | [RateLimiter.h](../include/RateLimiter.h) | As Art. 4(2) | ⚠️ Off by default |

### SEC Regulation SCI

| Requirement | Rule | Implementation | File | Wiring · test | Status |
|------------|------|---------------|------|---------------|--------|
| Capacity planning | 1001(a) | Capacity monitoring (queue depth 90%, memory 95%, disk 90%, repl lag 100ms) at the compiled-in thresholds. Only the queue-depth and journal-disk checks have a data source, so the memory and replication-lag checks never fire. Webhook alerting when `alert_webhook_url` is set. Runs in OrderEngine only: GatewayServer starts its engine with `start()` (`src/gateway_main.cpp:578`), which does not start the monitor. | [CapacityMonitor.h](../include/CapacityMonitor.h), [docs/CapacityPlanning.md](CapacityPlanning.md) | `src/main.cpp:291` (`startAsync`) → `src/MatchingEngine.cpp:381-382`; data sources `:394` (queue), `:426` (disk); alerts `src/main.cpp:266` · `CapacityMonitorIntegrationTest` | ⚠️ Partial |
| Business continuity | 1001(b)(1) | Primary-backup replication with epoch-based fencing, heartbeat-driven failover, journal log shipping. Disabled unless `--allow-unsafe-replication` is passed, because of open CRITICAL defects (REPL-1..6: a deposed primary is never fenced; a promoted backup can execute orders twice). No binary constructs `JournalFollower`. | [ReplicationProtocol.h](../include/ReplicationProtocol.h), [JournalFollower.h](../include/JournalFollower.h) | Refused without the flag · `EngineRefusesReplicationRoleTest` | ❌ Disabled, known defects |
| Incident reporting | 1002(b) | `IncidentLogger` writes NDJSON incident records with ns-precision timestamps, symbol, trigger condition, book state summary. Hourly rotation. Neither binary calls `enableIncidentLog()`. | [IncidentLogger.h](../include/IncidentLogger.h) | — | ⚠️ Library only, not enabled |
| System intrusion | 1001(a)(2) | N/A — network security is deployment-specific (firewall, TLS termination via sidecar). | — | — | ⚠️ Deploy-time |

### SEC Rule 15c3-5 (Market Access Rule)

| Requirement | Section | Implementation | File | Wiring · test | Status |
|------------|---------|---------------|------|---------------|--------|
| Financial risk management | (c)(1)(i) | Pre-trade risk limits with max order size, max notional, position limits. Unlimited by default; nothing sets them (see RTS 7 Art. 2). | [ParticipantRiskState.h](../include/ParticipantRiskState.h) | — | ⚠️ Library only, not enabled |
| Erroneous order prevention | (c)(1)(ii) | Circuit breaker only (fixed 5%, see below). The price band defaults to off and nothing sets it; `LULDManager` is never called. | [OrderBook.h](../include/OrderBook.h) | As Circuit breakers below | ⚠️ Circuit breaker only |
| Kill switch | (c)(2) | `GraduatedKillSwitch` with process-level termination. Not consulted by any order path (see RTS 6 Art. 4(1)). | [GraduatedKillSwitch.h](../include/GraduatedKillSwitch.h) | — | ⚠️ Library only, not enabled |

### Volatility Controls

| Feature | Standard | Implementation | File | Wiring · test | Status |
|---------|----------|---------------|------|---------------|--------|
| LULD-style pauses | NMS Plan | `LULDManager` with configurable band % and pause duration per symbol. Reference price tracking. A member of `OrderBook` that no code path calls. | [LULDManager.h](../include/LULDManager.h) | — | ⚠️ Library only, not enabled |
| Circuit breakers | Exchange rules | Per-symbol circuit breaker at 5%. `setCircuitBreakerThreshold()` has no caller in either binary, so it is fixed. Rejects the order and leaves the symbol trading; it does not switch to `TradingState::VolatilityAuction`. | [OrderBook.h](../include/OrderBook.h) | Default in every book: `include/OrderBook.h:992`; admission check `src/OrderBook.cpp:1069` · `BreakerNoAuctionTest` | ✅ Enabled |
| Price bands | Exchange rules | `priceBandPct_` admission filter rejects individual orders outside [ref±X%]. Defaults to 0 (off); `setPriceBandPct()` has no caller and `price_band_pct` is read by nothing. | [OrderBook.h](../include/OrderBook.h) | — | ⚠️ Library only, not enabled |

### On-Close Order Types

| Feature | Standard | Implementation | File | Wiring · test | Status |
|---------|----------|---------------|------|---------------|--------|
| Market-on-Close (MOC) | Exchange rules | MOC orders park in `onCloseOrders_` during Continuous/PreOpen/AuctionOpen; released to auction market orders at `AuctionClose`; fill at the uncross clearing price. Only OrderEngine started with `--session-schedule` moves books to `AuctionClose`. Without it, and always in GatewayServer, MOC orders are accepted, never execute, and are not cancelled until a session end. | [OrderBook.h](../include/OrderBook.h), [SessionScheduler.cpp](../src/SessionScheduler.cpp) | `src/main.cpp:297-301` (`--session-schedule`) → `src/SessionScheduler.cpp:138` (`AuctionClose`) · `MocLocTest` (MOC_ReleasedOnAuctionCloseTransition, MOC_FillsAtUncrossPrice), `SessionSchedulerTest`, `OnCloseSessionEndTest` | ⚠️ Off by default (`--session-schedule`) |
| Limit-on-Close (LOC) | Exchange rules | LOC orders park until `AuctionClose`; admitted to the limit book; the unfilled remainder is cancelled via `cancelLocOrders()` after the uncross. Same wiring as MOC. | [OrderBook.h](../include/OrderBook.h) | As MOC · `MocLocTest` (LOC_ParticipatesInCloseAtLimitPrice, LOC_OutsideClearingPrice_CancelledAfterUncross) | ⚠️ Off by default (`--session-schedule`) |

### Market Integrity

| Feature | Regulation | Implementation | File | Wiring · test | Status |
|---------|-----------|---------------|------|---------------|--------|
| Wash trade detection | Dodd-Frank § 747 | `WashTradeDetector` with 256-bit beneficial owner bitsets. O(1) intersection check. Configurable action (flag/reject). A member of `OrderBook` that no code path calls. | [WashTradeDetector.h](../include/WashTradeDetector.h) | — | ⚠️ Library only, not enabled |
| Audit trail | SEC 17a-25 | Journal (with `--journal`) with `steady_clock` timestamps — monotonic, not wall-clock time — replayed on boot. Not a full trail: a checkpoint **replaces** the journal with a snapshot of resting orders, discarding every fill and cancel before it, and one runs automatically at 250,000 entries / 64 MiB and on every clean OrderEngine shutdown. | [Journal.h](../include/Journal.h), [JournalBoot.h](../include/JournalBoot.h) | `src/main.cpp:202`, `src/gateway_main.cpp:595` (`bootJournal`) · `EngineReplaysJournalTest` (real OrderEngine), `GatewayJournalRestartTest` | ⚠️ Not a retained trail |
| Market data integrity | Reg NMS | Shared-memory market data feed with sequence numbers, gap detection, versioned schema. Published by GatewayServer only; OrderEngine publishes no market data. | [MarketDataPublisher.h](../include/MarketDataPublisher.h) | `src/gateway_main.cpp:605-606` (publisher), `:664` (snapshots) · `GatewayMarketDataThreadTest`, `MarketDataGapTest`, `MarketDataSchemaTest` | ✅ Enabled (GatewayServer) |
| Maker-Taker Fee Calculation | Dodd-Frank / exchange rules | `FeeEngine` calculates maker-taker rebates on every fill event — once a fee schedule is set. Neither binary sets one, so it never runs. | [FeeEngine.h](../include/FeeEngine.h) | — | ⚠️ Library only, not enabled |

## Observability Infrastructure

All endpoints are on OrderEngine's admin port (`src/main.cpp:473`); GatewayServer has none.

| Feature | Implementation | Endpoint | Wiring · test |
|---------|---------------|----------|---------------|
| Prometheus metrics | `MetricsRegistry` with counter/gauge/histogram export | `GET /prometheus` | `src/AdminServer.cpp:328` · `AdminServerEndpointsTest` |
| Health check | Liveness probe | `GET /health` | `src/AdminServer.cpp:337` · `AdminServerEndpointsTest` |
| Order book snapshot | L2 depth (10 levels) | `GET /book?symbolId=X` | `src/AdminServer.cpp:355` · `EngineReplaysJournalTest` (reads `/book` from the real binary) |
| OTR monitoring | Per-participant stats, symbol 0 only | `GET /otr?participantId=X` | `src/AdminServer.cpp:352` · no test requests it |
| Structured logging | `JsonStderrSink` when `log_sink = stderr`; `NullSink` otherwise | N/A (stderr) | `src/main.cpp:131-134` · `StructuredLogTest` |
| Webhook alerts | `AlertDispatcher` with Slack, PagerDuty, Generic formats; `http://` only | N/A (push) | `src/main.cpp:227-267` · `AlertDispatcherTest` |

## Formal Verification

11 TLA+ specifications model parts of the engine at small bounds. None is linked to the C++ by a refinement proof; see [Verification.md](Verification.md) for what each checks.

| Component | Spec File | Invariants Verified |
|-----------|-----------|-------------------|
| MPSC Queue | `spec/MpscQueue.tla` | Linearizability, No data loss |
| Engine Consumer Loop | `spec/EngineConsumer.tla` | Shutdown completeness |
| Snapshot Consistency | `spec/Snapshot.tla` | No torn reads |
| Matching Engine | `spec/MatchingEngine.tla` | Price priority, time priority, execution at the passive price, no crossed book, no self-trade (default STP), no empty resting order, FIFO queues, quantity conservation — Limit orders, modify and replace, at `MatchingEngine.cfg` bounds; each property but conservation and non-negativity has a broken cfg that CI requires to fail |
| Replication Protocol | `spec/Replication.tla` | No committed loss, No duplicate execution, No split-brain — **vacuously**: under the shipped configs the backup can never promote. See [Verification.md](Verification.md). |
| Auction Protocol | `spec/Auction.tla` | Uncross price validity, admission rules |
| OCO Atomicity | `spec/Oco.tla` | No double-fill on OCO pairs |
| Risk Hierarchy | `spec/Risk.tla` | Hierarchical limit enforcement |
| FIX Session | `spec/FixSession.tla` | Session sequence safety |
| Epoch Durability | `spec/EpochDurability.tla` | Epoch entry durability |

## Audit Contact

For questions about this compliance matrix, contact the engineering team.
Document last updated: 2026-10-22.
