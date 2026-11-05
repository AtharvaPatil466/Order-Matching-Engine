# Claims and the tests behind them

Roadmap 2.4: no guarantee goes in a doc without a test that would fail if it
were false. Each guarantee in [README.md](../README.md),
[Compliance.md](Compliance.md), [Verification.md](Verification.md) and
[ProductionReadiness.md](ProductionReadiness.md) carries an invisible marker,
`<!-- claim:ID -->`, and has a row here naming that test.
`scripts/check_claims.py` (ctest `ClaimsCheckTest`, label `project`, so every
CI lane runs it) fails when a marker has no row, a row has no marker, or a row
names a test that does not exist.

**What counts as a guarantee.** A sentence promising behaviour that someone
would rely on for correctness, durability, safety or security: durability and
recovery, matching priority, self-trade prevention, risk and admission
controls, authentication, market-data integrity and gap recovery, and the
model-checking results. Feature descriptions, stated limitations and
performance figures (roadmap 2.11) are not marked. A sentence that promised
more than any test shows was rewritten, not marked.

**Test column.** A name in backticks is a ctest test with the `project` label,
a gtest case under its discovered name (`Suite.Case`). `tla:Cfg` is a TLC run
of `spec/Cfg.cfg` that `.github/workflows/tla.yml` performs; a `Broken` cfg
there must fail. `no test — pending decision 1.x` is accepted only for the IDs
the script allowlists; there are none today.

**Adding a guarantee.** Write the test, put the marker next to the sentence,
add the row. A row whose "Not covered" cell grows long is a sign the sentence
should be narrowed.

| ID | Where | Claim | Test | Fails when · not covered |
|---|---|---|---|---|
| `gw-durable-ack` | README | `GatewayServer --journal` syncs each order before acknowledging it; an acked order survives `kill -9` | `GatewayJournalRestartTest`, `CrashRecoveryE2ETest` | The real binary is SIGKILLed the moment an ack arrives (and at a random point of a pipelined stream) and an acked order or cancel is missing after restart |
| `gw-durable-events` | README | `GatewayServer` holds fills and order updates until their journal entry is durable | `DurableGateCoverageTest`, `DurableAckTest` | An event for a cancel, modify, replace, expiry or kill is released before the journal flush (in-process engine with durable acks) |
| `journal-replay-on-boot` | README, Compliance | The journal is replayed on boot, so resting orders survive a restart | `EngineReplaysJournalTest`, `GatewayJournalRestartTest` | The real `OrderEngine` restarted on its journal shows a different `/book` level, or `GatewayServer` loses a resting order across a clean restart |
| `journal-torn-tail` | README | A journal that does not decode is refused; a torn final write is cut and kept in `<journal>.torn`, never appended behind | `EngineRefusesUnreadableJournalTest` | The real binary boots on undecodable bytes, refuses a torn tail, or loses an order taken after cutting one |
| `journal-crc` | ProductionReadiness | CRC-validated replay stops at truncated or corrupted records | `JournalCorruptionTest`, `EngineRefusesUnreadableJournalTest` | Recovery under injected bit flips and fsync failures returns a corrupted entry |
| `checkpoint-atomic` | README | A checkpoint replaces the journal atomically | `CheckpointChaosTest` | Under injected faults the file after `rewriteAtomically` is neither the before nor the after state, or fails its CRC |
| `checkpoint-restore` | ProductionReadiness | Checkpoint replay restores time priority, last trade and reference price, stop latches, pegs' prices and reprice order, and session state | `CheckpointTimePriorityTest`, `CheckpointPriceStateTest`, `StopLatchCheckpointTest`, `CheckpointPeggedOrderTest`, `SessionTransitionTest` | Any of those differs between the live book and one replayed from a checkpoint |
| `replay-determinism` | ProductionReadiness | Randomized command streams replay to the same final book | `JournalReplayPropertyTest` | `replayJournal()` on an engine-written journal, sync and async, holds different resting orders |
| `gtd-replay` | README | DAY/GTD expiries are journaled and replay identically under the virtual clock | `GTDReplayTest` | The replayed engine's final state differs from the live one |
| `repl-refuses-role` | README, Compliance | Replication was removed: a role refuses to boot, and the old opt-in opens nothing | `EngineRefusesReplicationRoleTest` | The real `OrderEngine` with `--role` or `OB_NODE_ROLE`, with or without `OB_ALLOW_UNSAFE_REPLICATION`, starts serving, or does not start without a role |
| `tla-matching-engine` | README, Compliance, Verification | `MatchingEngine.tla` holds price and time priority, passive execution price, no crossed book, no self-trade, conservation; each broken cfg fails | `tla:MatchingEngine`, `tla:MatchingEngineBroken`, `tla:MatchingEngineBrokenPrice`, `tla:MatchingEngineBrokenExecPrice`, `tla:MatchingEngineBrokenCross`, `tla:MatchingEngineBrokenStp`, `tla:MatchingEngineBrokenModify`, `tla:MatchingEngineBrokenReplace` | TLC finds a violation at `MatchingEngine.cfg`, or a broken cfg passes or fails on the wrong property · conservation and non-negativity have no broken cfg; no refinement links the spec to the C++ |
| `tla-auction` | README, Compliance, Verification | `Auction.tla` holds the transition table, no trade outside Continuous but the uncross, `NoReopenAfterClose`, and volatility auctions reopen | `tla:Auction`, `tla:AuctionBroken`, `tla:AuctionBrokenEdge`, `tla:AuctionBrokenDate`, `tla:AuctionBrokenHalt` | As above |
| `tla-small-specs` | README, Compliance, Verification | `Oco`, `Risk` and `FixSession` hold their safety properties, and each broken cfg fails | `tla:Oco`, `tla:OcoBroken`, `tla:Risk`, `tla:RiskBroken`, `tla:FixSession`, `tla:FixSessionBroken` | As above · 4 to 198 distinct states |
| `tla-concurrency-specs` | README, Compliance | `MpscQueue` and `EngineConsumer` invent and duplicate nothing and drain; `SnapshotLocked` snapshots are point-in-time | `tla:MpscQueue`, `tla:EngineConsumer`, `tla:Snapshot`, `tla:SnapshotLocked` | TLC finds a violation · no broken cfg shows any of these properties can fail |
| `auction-uncross` | README | An auction uncrosses at one clearing price chosen by maximum volume, then minimum imbalance, then reference proximity, and the indicative price matches it | `AuctionSessionTest` | The executed price differs from the indicative one or a tie-break is applied out of order |
| `peg-never-crosses` | README | A pegged order never rests at or through the opposite touch | `PegOrderTest` | A peg at or through the touch is admitted, or a reprice reaches it |
| `prorata-lmm-floor` | README, Compliance, ProductionReadiness | Pro-rata gives LMM/DMM orders a 40% floor and the rounding remainder first | `LmmDmm.ProRata_DmmFloorGuarantee_SmallIncoming`, `LmmDmm.ProRata_DmmFloorGuarantee_LargeIncoming`, `LmmDmm.ProRata_LmmFloor`, `LmmDmm.ProRata_DmmGetsRemainderFirst` | An allocation below the floor, or the remainder to a regular order first |
| `stp-default-cancel-incoming` | README, Compliance | Self-trade prevention cancels the incoming order by default, reports `CancelledBySTP` and leaves the resting order | `StpSemantics.ModeNoneSelfCrossReportsCancelledBySTPNotFilled`, `StpSemantics.ModeNoneSelfCrossLeavesRestingOrderIntact`, `StpProRata.DefaultCancelIncomingKillsTakerAndReportsCancelledBySTP` | An unconfigured self-cross trades, reports a fill, or touches the resting order |
| `circuit-breaker` | README, Compliance, ProductionReadiness | The 5% breaker trips on an order's first fill, starts a volatility auction, and reopens with a cross after the dwell | `VolatilityBreakerTest` | A far passive order trips it, an out-of-band first fill prints, or the auction does not reopen on the engine's clock |
| `price-range` | Compliance, ProductionReadiness | With a config file each symbol's static price range is required and enforced at admission | `PriceRangeTest`, `EngineRequiresPriceBandTest` | An order or replace outside the range is accepted, or the real `OrderEngine` starts with a symbol missing its range |
| `session-schedule` | ProductionReadiness | `--session-schedule` drives the scheduled auction states | `SessionSchedulerTest`, `EngineSessionScheduleConfigTest` | The scheduler does not move books through the timetable, or the binary ignores `session_schedule` |
| `moc-loc` | Compliance, ProductionReadiness | MOC/LOC orders release at `AuctionClose`, fill at the uncross price, and an unfilled LOC is cancelled; none outlives its session | `MocLocTest.MOC_ReleasedOnAuctionCloseTransition`, `MocLocTest.MOC_FillsAtUncrossPrice`, `MocLocTest.LOC_ParticipatesInCloseAtLimitPrice`, `MocLocTest.LOC_OutsideClearingPrice_CancelledAfterUncross`, `OnCloseSessionEndTest` | Release, price or cancellation differ from the rule |
| `otr` | README, Compliance | Order-to-trade ratio is tracked per participant and a cancel does not inflate it | `AuditFixes.CancelDoesNotInflateOrderToTradeRatio`, `RiskFixture.OtrEngagesThenReleasesAfterWindow` | The ratio counts a cancel, or the throttle neither engages nor releases · no test requests `/otr` |
| `kill-participant` | README, Compliance | A participant kill cancels every resting order of that participant on every symbol, journaled, before it returns; only a matching credential can send it | `KillSwitchCompletenessTest`, `KillJournalTest`, `GatewayLoginTest` | An order survives the kill under a contended queue, a killed order returns on restart, or another firm's session can kill |
| `risk-limits` | README | Per-participant max order size, notional and position limits reject at admission | `OrderBookTest.RiskLimits_MaxOrderSize`, `CriticalFixes.NotionalOverflowDoesNotBypassRiskLimit`, `RiskFixture.PositionRejectsOverLimitLong`, `RiskFixture.PositionRejectsOverLimitShort` | An order over a limit is accepted |
| `rate-limit` | README | Token-bucket rate limiting rejects at ingress | `SubmitResultTest`, `RateLimiterReconfigureTest` | The second order inside the bucket is accepted |
| `rate-limit-reload` | Compliance, ProductionReadiness | A reload reconfigures the default rate and burst and clears every bucket | `RateLimiterReconfigureTest` | `reconfigure()` leaves an old bucket or the old rate in force · no ctest sends SIGHUP to the binary |
| `capacity-monitor` | Compliance | `OrderEngine` runs the capacity monitor with live queue-depth and disk sources | `CapacityMonitorIntegrationTest` | The monitor is not started by the engine or never sees a breach |
| `submit-field-validation` | README | `submitOrder` refuses an undefined side, order type, time in force or peg type with `InvalidFieldValue` | `EngineFieldValidationTest` | Such a value reaches the book in sync or async mode |
| `ingress-reject-reasons` | ProductionReadiness | Submit returns explicit reasons for a stopped engine, unknown symbol, bad order and rate limit | `SubmitResultTest` | A reject carries the wrong reason |
| `gw-reject-responses` | ProductionReadiness | The gateway answers an ingress reject with an error carrying the reason | `GatewayIntegrationTest` | An order for an unknown symbol is acked |
| `admin-auth` | README, ProductionReadiness | Every admin endpoint but `/health` and `/readyz` needs the bearer token; no token and no opt-out never binds; `/chaos/order` is off unless armed with `OB_CHAOS_TOKEN` and also needs that token | `AdminAuthTest` | A protected endpoint answers without the right token, the server binds with neither, or `/chaos/order` takes a request with a missing or wrong `X-Chaos-Token` |
| `admin-loopback` | README, ProductionReadiness | The admin port binds 127.0.0.1 unless `--admin-bind` widens it | `AdminBindsLoopbackTest` | The real binary answers on a non-loopback address by default |
| `admin-readyz` | README, ProductionReadiness | `/readyz` is 503 until the engine is ready and 200 after, without auth | `AdminAuthTest` | Either answer is wrong or needs a token · that `main` opens the port only after replay is not tested |
| `gw-auth-required` | README | `GatewayServer` refuses to start without credentials unless auth is explicitly off | `GatewayRequiresCredentialsTest` | The real binary serves with an absent, empty, comments-only or unreadable credentials file |
| `gw-credentials-malformed` | README | A malformed credentials line, or a participant id of 1024 or more, fails the whole load | `ParticipantAuthTest` | A bad line is skipped or half-applied |
| `gw-login` | README | A session logs in before its first order, may act only as the participants its credential covers, and a failed login closes it | `GatewayLoginTest`, `OrderOwnershipTest` | An order before login is served, a session claims or cancels for another firm, or a bad secret stays connected |
| `gw-proto-compat` | README | The native protocol's V1 and V2 payloads are forward and backward compatible | `GatewayProtocolTest` | A V1 payload fails the V2 reader or a V2 payload the V1-compatible one |
| `md-shm-no-torn-read` | README | A lapped shared-memory reader never accepts a torn entry | `MarketDataTornReadTest` | An accepted entry's fields disagree with its own sequence |
| `md-shm-gap-reset` | README, Compliance | The shared-memory feed reports lost entries (`Gap`) and a restarted publisher (`Reset`); `GatewayServer` publishes from one thread | `MarketDataGapTest`, `GatewayMarketDataThreadTest`, `MarketDataSchemaTest` | A lapped subscriber gets an entry instead of `Gap`, a restart goes unnoticed, or a second thread writes the ring |
| `md-l2-sequence` | README | A book's `MarketDataUpdate` sequence numbers strictly increase and rebuild its depth | `MarketDataL2ReconstructionTest` | A sequence repeats or goes back, or the rebuilt depth differs from the book |
| `shadow-mode` | README | Shadow mode flags a FIFO violation and a snapshot divergence, and stays silent on identical streams | `ShadowMode.DetectsFIFOViolationViaTradeDivergence`, `ShadowMode.DetectsBookStateDivergenceViaSnapshot`, `ShadowMode.LargeIdenticalStreamProducesZeroDivergence` | Either detection misses or the 200-order clean run diverges |
| `sbe-forward-compat` | README | The first 24 bytes of an SBE v2 block equal the v1 encoding | `SbeProtocolTest` | The prefixes differ (library code, roadmap 1.2) |
| `itch-32bit-admission` | README | A book with an ITCH publisher rejects prices and quantities ITCH cannot carry | `ItchPublisherTest` | A 2^32 price or quantity is accepted (library code, roadmap 1.2) |
| `itch-ring-drop-resync` | README | A dropped async ITCH event is signalled and followed by a full resync in priority order | `ItchReconstructionTest` | No resync marker, or the re-adds are out of priority order (library code, roadmap 1.2) |
| `mold-gap-detection` | README | The MoldUDP64 subscriber detects gaps, including silent ones via heartbeat, and reports a session mismatch | `MoldUDP64Test` | A gap or mismatched session is delivered as if in order (library code, roadmap 1.2) |
| `itch-gap-recovery` | README | A subscriber gap is recovered by SoupBinTCP re-request from the packet journal, byte for byte | `ItchRetransmissionTest` | Replayed messages differ from the journaled ones (library code, roadmap 1.2) |
