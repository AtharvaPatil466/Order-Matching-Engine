# Compliance Feature Matrix

> Roadmap Phase 4, Week 16: Regulatory Compliance Documentation
>
> This document maps implemented features to specific regulatory requirements.
> It is intended for auditors and regulators.

## Regulatory Coverage

### MiFID II — RTS 6 (Algorithmic Trading)

| Requirement | Article | Implementation | File | Status |
|------------|---------|---------------|------|--------|
| Kill switch | Art. 4(1) | `GraduatedKillSwitch` with 4 escalation levels: Throttle (10% rate), SymbolHalt, GlobalHalt, Kill (checkpoint + terminate) | [GraduatedKillSwitch.h](../include/GraduatedKillSwitch.h) | ✅ Implemented |
| Self-trade prevention | Art. 5(1) | `SelfTradeProtection` with 4 modes: CancelResting, CancelIncoming, CancelBoth, DecreaseAndCancel. Per-participant configurable. | [SelfTradeProtection.h](../include/SelfTradeProtection.h) | ✅ Implemented |
| Throttling mechanisms | Art. 4(2) | Per-participant token bucket rate limiting via `RateLimiter`. Graduated throttle via kill switch Level 1. | [RateLimiter.h](../include/RateLimiter.h), [GraduatedKillSwitch.h](../include/GraduatedKillSwitch.h) | ✅ Implemented |
| Order-to-trade ratio monitoring | Art. 7 | OTR tracked per participant via `ParticipantStats`. Accessible via `/otr?participantId=X` admin endpoint. | [OrderBook.h](../include/OrderBook.h), [AdminServer.cpp](../src/AdminServer.cpp) | ✅ Implemented |
| Market making obligations | Art. 8 | `MatchAlgorithm::ProRata` matching for designated contracts. | [OrderBook.h](../include/OrderBook.h) | ✅ Implemented |

### MiFID II — RTS 7 (Direct Electronic Access)

| Requirement | Article | Implementation | File | Status |
|------------|---------|---------------|------|--------|
| Pre-trade risk limits | Art. 2 | Per-participant `RiskLimits` (max order size, max notional, max position). Checked before order acceptance. | [OrderBook.h](../include/OrderBook.h), [ParticipantRiskState.h](../include/ParticipantRiskState.h) | ✅ Implemented |
| Message throttling | Art. 3 | `RateLimiter` with per-participant and default rates. Token bucket algorithm. | [RateLimiter.h](../include/RateLimiter.h) | ✅ Implemented |

### SEC Regulation SCI

| Requirement | Rule | Implementation | File | Status |
|------------|------|---------------|------|--------|
| Capacity planning | 1001(a) | Capacity monitoring with configurable thresholds (queue depth 90%, memory 95%, disk 90%, repl lag 100ms). Webhook alerting. | [CapacityMonitor.h](../include/CapacityMonitor.h), [docs/CapacityPlanning.md](CapacityPlanning.md) | ✅ Implemented |
| Business continuity | 1001(b)(1) | Primary-backup replication with epoch-based fencing, heartbeat-driven failover, journal log shipping. | [ReplicationProtocol.h](../include/ReplicationProtocol.h), [JournalFollower.h](../include/JournalFollower.h) | ✅ Implemented |
| Incident reporting | 1002(b) | `IncidentLogger` writes NDJSON incident records with ns-precision timestamps, symbol, trigger condition, book state summary. Hourly rotation. | [IncidentLogger.h](../include/IncidentLogger.h) | ✅ Implemented |
| System intrusion | 1001(a)(2) | N/A — network security is deployment-specific (firewall, TLS termination via sidecar). | — | ⚠️ Deploy-time |

### SEC Rule 15c3-5 (Market Access Rule)

| Requirement | Section | Implementation | File | Status |
|------------|---------|---------------|------|--------|
| Financial risk management | (c)(1)(i) | Pre-trade risk limits with max order size, max notional, position limits. | [ParticipantRiskState.h](../include/ParticipantRiskState.h) | ✅ Implemented |
| Erroneous order prevention | (c)(1)(ii) | Price band validation (LULD-style), circuit breakers. | [LULDManager.h](../include/LULDManager.h), [OrderBook.h](../include/OrderBook.h) | ✅ Implemented |
| Kill switch | (c)(2) | `GraduatedKillSwitch` with immediate process-level termination capability. | [GraduatedKillSwitch.h](../include/GraduatedKillSwitch.h) | ✅ Implemented |

### Volatility Controls

| Feature | Standard | Implementation | File | Status |
|---------|----------|---------------|------|--------|
| LULD-style pauses | NMS Plan | `LULDManager` with configurable band % and pause duration per symbol. Reference price tracking. | [LULDManager.h](../include/LULDManager.h) | ✅ Implemented |
| Circuit breakers | Exchange rules | Per-symbol circuit breaker with configurable threshold (default 5%). Triggers `TradingState::Halted`. | [OrderBook.h](../include/OrderBook.h) | ✅ Implemented |
| Price bands | Exchange rules | `priceBandPct_` admission filter rejects individual orders outside [ref±X%]. | [OrderBook.h](../include/OrderBook.h) | ✅ Implemented |

### Market Integrity

| Feature | Regulation | Implementation | File | Status |
|---------|-----------|---------------|------|--------|
| Wash trade detection | Dodd-Frank § 747 | `WashTradeDetector` with 256-bit beneficial owner bitsets. O(1) intersection check. Configurable action (flag/reject). | [WashTradeDetector.h](../include/WashTradeDetector.h) | ✅ Implemented |
| Audit trail | SEC 17a-25 | Full journal with ns timestamps, crash recovery replay, checkpoint/snapshot. | [Journal.h](../include/Journal.h) | ✅ Implemented |
| Market data integrity | Reg NMS | Shared-memory market data feed with sequence numbers, gap detection, versioned schema. | [MarketDataPublisher.h](../include/MarketDataPublisher.h) | ✅ Implemented |

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

| Component | Spec File | Invariants Verified |
|-----------|-----------|-------------------|
| MPSC Queue | `spec/MpscQueue.tla` | Linearizability, No data loss |
| Engine Consumer Loop | `spec/EngineConsumer.tla` | Shutdown completeness |
| Snapshot Consistency | `spec/Snapshot.tla` | No torn reads |
| Matching Engine | `spec/MatchingEngine.tla` | FIFO preservation, Quantity conservation, No negative qty, GTD expiry |
| Replication Protocol | `spec/Replication.tla` | No committed loss, No duplicate execution, No split-brain |

## Audit Contact

For questions about this compliance matrix, contact the engineering team.
Document last updated: 2026-05-12.
