# Order Matching Engine — Project Overview

> **What this is.** A C++20 order matching engine: one binary, `OrderEngine`, that takes orders over TCP in its own binary protocol, matches them on per-symbol worker threads, journals them, and publishes an L2 feed to shared memory. It is a single node with no replication. It is **not production-ready**, and §9 and §10 list why.
>
> **What this document describes.** Branch `round8/integration` as of commit `a48cc27`, which has every fix lane of this round and its documentation; after it come one added test case and edits to this document, and no source file changed. Every capability below says whether the shipped binary does it, which test fails if it stops doing it, and where the code is. Where nothing proves a sentence, it says "not verified"; where nothing measured a number, it says "not measured".
>
> **State of this branch, plainly.** The branch is pushed as a pull request to `main` and has been through CI (§11); it is not merged. No benchmark has been run on this branch on x86, so **no latency or throughput figure in this document describes this code** (§5). The full suite passes on macOS, and the Linux Release, TSan, fault-injection and ASan/UBSan lanes pass in a local container, all at `a48cc27` and all run by hand. CI then ran on the pull request: at `88b8e32` every job passed except one of two ThreadSanitizer runs, which timed out in one test; that test is fixed in the commit after (§11).

---

## 1. Summary

The engine's core is an O(1) price-level array (`FlatPriceMap`), intrusive order lists, lock-free MPSC queues feeding thread-per-symbol workers, a CRC-32 journal replayed on boot, a session and auction state machine, self-trade prevention, a volatility breaker that checks every continuous print, and per-participant authentication on the order port. Around it sit library components that the binary does not switch on: pro-rata matching and market-maker roles, OCO links, hierarchical risk, fees, a CAT trail, and an ITCH 5.0 / MoldUDP64 / SoupBinTCP market-data stack. Those are tested as libraries and labelled as such below.

Compared with `main` (`adc7b5b`), this branch **deleted** the second binary (`GatewayServer`), the FIX, OUCH and SBE codecs, the DPDK path, primary/backup replication, `JournalFollower`, `EpochStore`, LULD and wash-trade detection. Of the protocols, only the native binary protocol remains served. Replication was deleted rather than finished: it split the brain when a backup was merely slow, and a promoted backup kept applying the old primary's stream. Asking for a role (`--role`, `OB_NODE_ROLE`) now refuses to boot. <!-- claim:repl-refuses-role -->

### What the shipped binary does

"Yes" means `src/main.cpp` turns it on in `OrderEngine`. "Library" means the code exists and has tests, and nothing in the binary reaches it.

| Capability | In `OrderEngine` | Test that fails if it is false · code |
|---|---|---|
| Order entry, native binary protocol (V1 and V2 frames) over TCP | Yes, `--gateway-port` (default 9001) | `GatewayIntegrationTest`, `OrderEngineGatewayTest` · `src/TcpGateway.cpp`, `src/main.cpp` |
| Participant login and authorisation | Yes; refuses to boot without credentials unless `--no-participant-auth` | `GatewayLoginTest`, `GatewayRequiresCredentialsTest` · `include/ParticipantAuth.h` |
| Order ids scoped by participant | Yes | `GatewayParticipantIsolationTest` · `src/TcpGateway.cpp` |
| Execution reports (fills, engine cancels, post-ack rejects, amend outcomes) | Yes, a stop's election included (`Triggered`) | `GatewayExecutionReportTest` · `src/TcpGateway.cpp`, MatchingRules Rule 2.6 |
| Ack only after the order's journal entry is synced | Yes, with `--journal` | `GatewayJournalRestartTest`, `CrashRecoveryE2ETest` · `src/main.cpp` (`SyncPolicy::Immediate`) |
| Reports and market data only after the entry behind them is synced | Yes | `GatewayExecutionReportTest` (`durable`), `JournalRecordBeforeEventTest` · `src/TcpGateway.cpp` |
| Journal replay on boot; any refused entry stops the boot | Yes | `EngineReplaysJournalTest`, `ReplayRefusedEntryTest` · `include/JournalBoot.h` |
| Checkpoint that restores the complete matching state | Yes (size trigger, compiled-in triggers, shutdown) | `CheckpointEquivalenceTest`, `CheckpointCompleteStateTest` · `src/MatchingEngine.cpp` |
| A halted journal freezes every book | Yes | `JournalHaltFreezeTest` · MatchingRules Rule 2.7 |
| Price band per symbol, required at boot | Yes, unless `--no-price-band` | `EngineRequiresControlsTest`, `PriceRangeTest` · `include/ConfigValidator.h` |
| Position limit per participant, required at boot | Yes, unless `--no-risk-limits` | `EngineRequiresControlsTest` · `src/main.cpp` |
| Volatility breaker on every continuous print | Yes, fixed threshold (no setter is called) | `PrintBandTest`, `VolatilityBreakerTest` · `src/OrderBook.cpp` (`PrintBand`) |
| Market-order collar | Yes, fixed default | `MarketCollarTest` · `include/OrderBook.h` |
| Self-trade prevention, cancel the incoming order | Yes, the default for every participant | `StpSemanticsTest` · `src/OrderBook.cpp` |
| Participant kill (one-shot cancel of all its orders) | Yes, a gateway message | `KillSwitchCompletenessTest`, `KillJournalTest` · `src/MatchingEngine.cpp` |
| Graduated kill switch (throttle, symbol and global halts) | Consulted at admission; **nothing in the binary can engage it** | `CheckpointCompleteStateTest` (persistence only) · `include/GraduatedKillSwitch.h` |
| Per-participant rate limit | Off unless `rate_limit.default_rate`/`default_burst` are set; applied at boot and on SIGHUP | `EngineRateLimitConfigTest` · `src/main.cpp` |
| Sessions, auctions, closing cross | Only with `--session-schedule` | `SessionSchedulerTest`, `EngineSessionScheduleConfigTest`, `UncrossOracleTest` · `src/SessionScheduler.cpp` |
| MOC / LOC | Execute only with `--session-schedule`; without it nothing refuses them and nothing reaches the close (read from code, not tested) | `MocLocTest` · `src/OrderBook.cpp` |
| GTD / DAY expiry | Yes: a sweep every second plus a maker-side check; DAY needs a schedule or an expiry | `EngineExpiresGtdTest`, `GatewayExpiresGtdTest`, `AdmissionTifTest` · MatchingRules §8 |
| Shared-memory L2 feed `/orderbook_md` | Yes, published from the order port's event thread after durability | `MarketDataGapTest`, `GatewayMarketDataThreadTest` · `include/MarketDataPublisher.h` |
| Admin HTTP port | Yes, loopback by default, bearer token required | `AdminAuthTest`, `AdminBindsLoopbackTest` · `src/AdminServer.cpp` |
| Webhook alerts | Plain HTTP only; `CapacityMonitor` can fire, the graduated switch cannot be engaged | `AlertDispatcherTest` · `src/main.cpp` |
| Pro-rata matching, LMM/DMM roles | Library | `ProRataLevelTest`, `LmmDmmTest` · `src/OrderBook.cpp` |
| OCO links | Library: no gateway message or admin route registers one | `OcoScopeTest` · `include/ContingencyManager.h` |
| Hierarchical risk, fees, CAT trail, queue backpressure | Library; their admin endpoints were deleted because they always answered zero | `include/HierarchicalRiskManager.h`, `include/FeeEngine.h`, `include/CatReporter.h` |
| ITCH 5.0, MoldUDP64, SoupBinTCP retransmission | Library | `ItchPublisherTest`, `MoldUDP64Test`, `ItchRetransmissionTest` · `include/Itch*.h`, `include/MoldUDP64.h` |
| Replication, FIX, OUCH, SBE, DPDK, `GatewayServer` | Deleted | `EngineRefusesReplicationRoleTest` (a role refuses to boot) |

### Codebase

| Item | Count on this branch |
|---|---|
| Headers in `include/` | 76 |
| Sources in `src/` | 11 (`OrderBook.cpp` and `MatchingEngine.cpp` are each over 3,000 lines) |
| Binaries | `OrderEngine`, plus `MdSubscriber` (a feed reader) and the research library `OrderMatcherResearch` |
| TLA+ specifications | 7, plus one trace-validation module; all 7 in `tla.yml` |
| Tests | `ctest -N` lists them; 715 on macOS at `a48cc27` (§11) |

---

## 2. Matching

`docs/MatchingRules.md` is the rulebook; it cites the code for each rule and lists known defects in its §10.

**Order types.** Limit, Market, IOC and FOK (order types here, not time-in-force values), Stop (a stop-market), StopLimit, TrailingStop, MIT, Iceberg (display at least 1/100 of size), Hidden, PostOnly, Pegged (MidPeg, PrimaryPeg) <!-- claim:peg-never-crosses -->, MOC and LOC. Time in force is GTC, GTD or DAY.
- A GTD or DAY order whose expiry has already passed is refused (`InvalidFieldValue`). IOC, FOK and Market orders with DAY need no session end. (`AdmissionTifTest`)
- Minimum quantity is met on arrival or the order is cancelled; an order cannot rest and wait for its minimum. A replace that would execute below an unmet minimum is refused. The uncross applies the same rule: an unexecuted minimum-quantity order the cross would fill below its minimum is cancelled untraded, and the cross is priced again without it. (`RestingOrderRulesTest`) **Gap:** an order the cross does not reach rests into Continuous with its minimum unmet, and as a maker it can still execute below it (§10).

**Priority.** Price-time FIFO. An order's age is a per-book arrival sequence, not a clock reading; it is re-drawn whenever the order loses priority (a price change, a growth, a peg move). This replaced a wall-clock timestamp that a checkpoint used to re-stamp, which changed an auction's outcome after a restart.

**Self-trade prevention** <!-- claim:stp-default-cancel-incoming -->: four modes; the binary uses the default, cancel the incoming order, for everyone. `DecreaseAndCancel` now reports the resting order's new size to its owner.

**Volatility breaker.** The band is checked at **every fill** inside the match loop, so a fresh order, a crossing replace and an elected stop are all held to it. <!-- claim:circuit-breaker --> A print outside the band does not happen: the book enters a volatility auction, the order keeps its in-band fills, and the rest joins the auction. The reference price is set only by a print, an uncross, configuration or a checkpoint; a rejected order can no longer seed it. (`PrintBandTest`) The threshold is fixed: `setCircuitBreakerThreshold` has no caller in `src/`.
- Elections latched while the book could not execute them, and stops the reopening cross printed through, elect at the transition into Continuous (the open, the end of a volatility auction, a halt resumed), and pegs reprice there. (`PrintBandTest`) If those elections trip the breaker again, the trip is journaled after the transition, and a restart lands in the same state from a checkpoint and from the plain journal (`CheckpointCompleteStateTest`, `reopen_trips_breaker`).
- Stops elected by one print execute, and are deferred past the 64-per-sweep budget, in arrival order: the stop, trailing-stop and peg lists keep arrival order when one is removed (they used to swap-remove). (`StopSweepOrderTest`; `validateIntegrity` checks the stop lists.)
- **Decision recorded, not settled:** an elected stop-limit is not re-screened against the admission band; only its prints are bounded by the breaker.

**Sessions and auctions.** PreOpen, AuctionOpen, Continuous, AuctionClose, PostClose, VolatilityAuction and Halted, through one enforced transition table. <!-- claim:tla-auction --> An uncross prints at one price, fills parked market orders first, and then allocates in arrival order. <!-- claim:auction-uncross --> This branch changed three things here:
- parked market and on-close orders keep arrival order when one is cancelled (it used to swap-remove);
- a same-owner pair is resolved before the cross only if both legs are executable at the clearing price, iterated to a fixed point;
- a close reached through a halt cancels released MOC/LOC and parked market orders.

`UncrossOracleTest` compares the uncross with an independent reference written from the rulebook on 40,000 random books per run. `SessionScheduler` drives the day only when `--session-schedule` is given; **by default the binary has no open, no close and no DAY expiry**.

**Pro-rata (library).** Allocation now covers the whole level (it used to stop at the first 1,024 orders) and an elected stop in a pro-rata book is allocated pro-rata. Neither binary path creates a pro-rata book. <!-- claim:prorata-lmm-floor -->

---

## 3. Order entry

`OrderEngine` is the only order-entry server, and `TcpGateway`'s native binary protocol is its only protocol. <!-- claim:gw-proto-compat -->

- **Authentication.** Credentials come from their own file (`user:secret:id[,id...]`); a malformed line fails the whole load. <!-- claim:gw-credentials-malformed --> The binary refuses to start without credentials unless `--no-participant-auth` is passed. <!-- claim:gw-auth-required --> A session logs in before its first order and may act only for the participants its credential covers, the kill switch included. <!-- claim:gw-login -->
- **Order ids** are the client's own, scoped by participant: the book holds `(participant << 54) | id`. Two firms may use the same id; neither can see or touch the other's order; a participant id of 1024 or more is refused on every request type, which closes the old "kill-all" sentinel. <!-- claim:gw-ids-per-participant -->
- **What an Ack means.** The order port answers after the worker has run the request and its journal entry has synced. An Ack says the request is durable, **not that the book accepted it**: a reject decided while matching (post-only, FOK, a cancel of an unknown id) still answers Ack, and the outcome follows as an execution report.
- **Execution reports.** A `TradeNotification` goes to both sides of every fill; an `OrderUpdateNotification` goes to the owner for every status change, including the engine's own cancels (IOC/FOK/Market remainders, expiry, kill, session end, STP), a post-ack reject, and the outcome of each cancel, modify and replace. They leave only once the journal entry behind them is durable, and never if the journal refused it. <!-- claim:gw-exec-reports --> A session that stops reading is disconnected at 1 MiB of queued output.
- **Stop elections and replaces.** A stop's election is reported once, as `Triggered` (status value 7 on the wire; not terminal, not an execution), before any fill it makes. A replace is restated before the sweep its new price causes, so its own fills carry the new leaves. (`GatewayExecutionReportTest`, case `elections`)
- **What is still not delivered:** reports to a participant with no live session are dropped and counted, with no replay on reconnect and no drop copy. The library's synchronous `TcpGateway` mode sends no reports.

---

## 4. Durability and recovery

> **Single node.** Recovery is from the journal on local disk. There is no replica.

- **Journal.** A CRC-32-checked log of requests with a 24-byte header (magic, version, record size, epoch). `OrderEngine` runs it with Immediate sync: each entry is synced before the order port answers. <!-- claim:gw-durable-ack --> A torn final write is cut and kept in `<journal>.torn`; anything else that does not decode refuses boot. <!-- claim:journal-torn-tail --> A new journal file's directory is synced before its first record.
- **Not write-ahead.** The book applies a request before its entry is appended. Since this round the effect of that is contained but not removed:
  - a write or sync error halts the journal, and from then on **nothing changes a book**: cancels, modifies, replaces, the kill switch, expiry and session transitions are refused or suspended until a restart, and the admin book views answer 503; <!-- claim:journal-halt-freeze -->
  - engine-initiated cancels (kill, DAY close, OCO siblings, expiry) are journaled before they are applied, and so are client cancels, modifies and replaces on both engines: the book runs its refusal checks, then the journal write under its lock, and a refused write leaves the order as it was and gives back a replace's reserved growth (`JournalRecordBeforeEventTest`, six refused-write cases; run against the code from before the fix, the five that fix covers each report the request applied anyway);
  - **open (MEDIUM):** a new order, a session transition or an uncross whose *own* record fails is already applied in memory. Nothing can observe it (reports and the feed are withheld, the admin views refuse, nothing trades), and a restart drops it. An expiry sweep that halts partway also finishes that book's expiries in memory.
- **Replay on boot.** Both the plain journal and a checkpoint are replayed before any port opens. <!-- claim:journal-replay-on-boot --> Replay now counts entries *applied*: any entry the live engine applied and replay refuses stops the boot with `replay_entry_refused` (a legacy journal under `--replay-legacy-journal` only warns). A cancel, modify or replace record whose own book lacks the order is a no-op; only a legacy journal falls back to searching the other books (`MIDS-10`). This closes the review's most severe finding, that a checkpoint of a closed or halted book restored zero orders and logged success.
- **Checkpoints** replace the journal atomically <!-- claim:checkpoint-atomic --> and now carry everything later matching depends on: each order's age, the anchored band, the reference and last-trade price, the volatility auction's dwell clock, the engine kill switch and the graduated switch's scopes, stop latches and trail references, peg and LOC queue order, and session state. <!-- claim:checkpoint-restore --> `CheckpointEquivalenceTest` runs a random 400-operation flow, cuts it at a random point three ways (live, checkpoint, plain journal) and requires identical trades, updates and books after every later operation.
  - It also compares every order's status, and runs `validateIntegrity` on the live book after every operation. Comparing status found that an order part-filled by an uncross read `Accepted` live and `PartiallyFilled` after a restore; the uncross now sets `PartiallyFilled`.
  - **Not covered by that test:** the async checkpoint cut, GTD/DAY expiry and the dwell clock (they need the expiry clock).
  - The plain journal does not record the dwell start, so a restart without a checkpoint restarts the dwell.
- **Graceful stop.** SIGTERM drains the queues and checkpoints every resting order, DAY orders included, in every trading state.

---

## 5. Performance: what is known

**Nothing has been measured on this branch.** There is no headline figure. The figures that earlier versions of this document led with (a core-matching P50 from one unlogged bare-metal session at `d2e688c`, with a PGO profile trained on the workload it was measured on) are withdrawn from this document; they predate a rewrite of the match path and every change below.

The only raw benchmark output that exists for recent code is one `bench-x86.yml` run at `adc7b5b` (`main`, **before** this branch's changes): a GitHub-hosted Ubuntu VM, AMD EPYC 9V74, 4 vCPU, five repetitions, a clock on a roughly 10 ns grid. The workflow recorded the run as failed because its `perf` steps had no hardware counters; its outputs are kept with the adversarial review, not in this repository. A shared VM supports order-of-magnitude statements, not an SLA.

| At `adc7b5b`, hosted x86 VM, five runs (not this branch) | Result |
|---|---|
| Core matching, no listener (`HonestBenchmark` Path A), median of five | P50 280 ns (251–290), P99 1.1 µs |
| Venue-shaped flow (`RealisticFlowBenchmark`) | combined P50 391 ns, P99 4.0 µs; cancel 441 ns against 251 ns for a new order |
| First touch after eviction (`ColdCacheBenchmark`) | 4.96 µs P50 against 450 ns warm |
| Journal on, ack before durable (Path C) | P50 451 ns, P99 224 µs, 37–147k orders/s |
| Durable ack (Path D, one fsync per order) | P50 173 µs, P99 1.2–3 ms, 1.7–3.5k orders/s |
| Sustained async load (`SustainedLoadTest`) | held 50k/s; at 400k/s offered the rate decayed to about 107k/s with 9 s queueing, and the run exited 0 |

What changed since that run and could move every row:
- **Listener cost.** At `adc7b5b` a book with a listener attached walked every order at a price level on each add, fill and cancel, so cost grew with level depth; every number above is listener-free, and the shipped binary always attaches one. Level totals are now maintained incrementally; `LevelTotalsTest` proves the per-update work is independent of depth by counting, not by timing. HonestBenchmark gained Path L (Path A with a counting listener). **Neither has been run on x86.**
- **Breaker.** Every fill now consults the price band.
- **Order age.** A per-book counter replaced a clock read per order.
- **Execution reports.** Each request's events pass through an MPSC queue to the order port's thread, which waits for the journal sync.
- **Write-ahead amends.** A cancel, modify or replace now writes its journal record from inside the book's lock, before it changes the book.
- **SustainedLoadTest** now fails when the backlog at the end of the offered window exceeds 100 ms of offered load, so the 400k/s run above would fail.

What that leaves:
- **The latency and throughput of the shipped configuration are not measured anywhere.** `OrderEngine` syncs one journal entry per request and its order port waits for it, so its throughput is bounded by the disk's fsync latency; the Path D row (a synchronous engine, not `OrderEngine`) is the nearest measurement and is from a different commit.
- The lanes of this round ran `HonestBenchmark` before and after their changes on an Apple Silicon laptop to look for regressions. Those runs are indicative only (a ~41 ns clock tick, other lanes loading the machine), their outputs are not in the repository, and none is quoted here.
- The latency gate in CI (`BenchmarkRegression`) cannot catch a doubling: its thresholds sit far above what it measures, and a hosted runner's single-run P99 moves by about as much on unchanged code. The A/B gate in `benchmark-gate.yml` is the instrument for that, and it is not a required check.
- `BENCHMARKS.md` keeps the history of earlier measurements with their provenance; `docs/PERF_SOURCES.md` gives every number in these documents its source.

---

## 6. Verification

**Tests.** `ctest -N` lists the suite: unit, property, differential, chaos and fault-injection, crash-recovery, real-binary shell tests, and benchmarks. The count measures how many targets exist, not how much behaviour they pin. Mechanisms worth knowing:
- `CheckpointEquivalenceTest` (above) and `UncrossOracleTest` (the uncross against an independent reference on random books).
- `LevelTotalsTest` checks every published L2 level against a recomputation from the orders after every operation, and `validateIntegrity` now checks the level totals everywhere it runs; each of the nine maintenance sites was removed in turn and the test failed each time.
- `CrashRecoveryE2ETest` SIGKILLs the real binary mid-stream and checks that every acked order and every reported trade is in the replayed journal.
- `DifferentialFuzzTest` against a reference book; `PropertyTest`; `fuzz_gateway_engine` (libFuzzer through the gateway framer into the engine).
- Sanitizer lanes (ASan/UBSan, TSan), a fault-injection build, an io_uring lane, and `scripts/verify_linux.sh` to run a lane in an `ubuntu:24.04` container.

**Coverage** of the match path was measured once, by the review on `main`: about 95% of `OrderBook.cpp` lines and 86% of its branches. It has not been measured on this branch.

**TLA+.** Seven specifications, checked by TLC at small bounds; `docs/Verification.md` gives each one's properties, bounds and the code it models. `spec/check.sh` now checks for deadlock (it used to disable that for every spec) and pins the TLC jar by hash.
- `MatchingEngine.tla`: price-time matching with STP, modify and replace; 2,732,033 distinct states at `MatchingEngine.cfg` (three orders) and 10,637,301 at `MatchingEngineDeep.cfg` (four orders at one price, so a fill reaches a level three deep). Eight broken configurations must fail. <!-- claim:tla-matching-engine -->
- `Auction.tla` (199,348 distinct), `MpscQueue` (3,672), `EngineConsumer` (323), `SnapshotLocked` (221,140). <!-- claim:tla-concurrency-specs --> The queue property is "nothing invented or duplicated, per-producer FIFO, everything consumed" under sequentially consistent interleavings, not linearizability; the C++ uses weaker atomics.
- `Oco` (198) and `Risk` (800) model library code the binary never calls. <!-- claim:tla-small-specs -->
- **Trace validation, first cut.** `TraceValidationTest` runs 300 seeded `OrderBook` flows (limit orders, cancel, modify, replace) and has TLC check each trace against `MatchingEngine.tla`; a reordered sweep is rejected. It covers only what the spec models, drives `OrderBook` directly, and skips without Java.
- What TLA+ does not give: a refinement proof. The specs model the design at small bounds; the trace check is sampled, not exhaustive.
- `tla.yml` passed on the pull request at `88b8e32`, trace validation included, with the pinned `v1.7.4` TLC jar. Its first two runs failed for reasons in the workflow, not the specs: the hash first pinned was of a `v1.8.0` pre-release jar that is rebuilt in place, and one check expected a newer TLC's wording. The figures above are from one local run on 2026-11-14 and were reproduced with the pinned jar.

**The documents are checked too.** `ClaimsCheckTest` ties each `<!-- claim:ID -->` marker in this file and four others to a row of `docs/CLAIMS.md`, and requires each test the row names to exist and its source to contain one of the calls the row's evidence entry lists. That catches a test that never reaches the claim's subject; it cannot show that the test asserts the claim. `DocPerfNumbersSourced` requires every performance number in this file and four others (percentages and per-order ratios included) to have a source row, and a withdrawn, unrecorded or estimated number to say so in the document where it appears.

---

## 7. Operations

- **Boot refuses an uncontrolled order port.** `OrderEngine` exits 1, listing every problem, without: a price band for every symbol, a journal, a position limit for every participant its credentials name, credentials, and an admin token. Each is waived only by name (`--no-price-band`, `--no-journal`, `--no-risk-limits`, `--no-participant-auth`, `--admin-no-auth`). A `--config` that cannot be read, or has a line that is not `key = value`, is fatal. <!-- claim:boot-requires-controls -->
- **Config keys that are not applied are refused**, not ignored: `instrument.<s>.tick_size`, `lot_size` and `participant.<id>.otr_limit`. Every book has a fixed 0.0001 tick and at most 200,000 ticks of band; per-instrument tick size is not supported.
- **Admin HTTP** (`--port`, default 8080): binds 127.0.0.1 unless `--admin-bind` <!-- claim:admin-loopback -->; every endpoint except `/health` and `/readyz` needs the bearer token <!-- claim:admin-auth -->; the port opens after replay. Endpoints: `/health`, `/readyz`, `/metrics`, `/prometheus`, `/book`, `/auction`, `/audit`, `/otr` (symbol 0 only), `/journal/head`, `/version`, and the test-only `/chaos/order` (off unless armed). `/fees`, `/risk` and `/cat` were deleted.
- **SIGHUP** re-reads the config; only the rate limit takes effect.
- **Docker.** The image loads `config/engine.conf.example` and journals to a volume; `docker compose up` was checked once by hand. No CI step runs compose.
- **Runbook.** `docs/Runbook.md`. A CI job, `runbook-smoke`, runs its bash blocks; it passed on the pull request at `88b8e32`.

---

## 8. Research layer

`bcs_research/` drives the engine from Python through a pybind11 bridge to study the Budish–Cramton–Shim latency arms race; `paper/bcs_paper_draft.md` is a draft, not peer reviewed. The adversarial review read it as a referee and recommended major revision; its findings stand on this branch, which did not touch the research layer:
- the batch-auction results (Exp 4) **do not reproduce** on the current engine, and no result file records the engine commit it ran on;
- the competing-maker headline is gross of fees, and at ordinary maker and taker fees the sniper's edge mostly disappears at three or more makers;
- the envelope check against external data cannot fail, the simulation tick is never varied, and "model-checked" overstates what TLA+ covers (no oracle for the uncross at the time; no participants in `Auction.tla`).

`NoResearchInEngineTest` fails if the engine library or `OrderEngine` carries a research symbol.

---

## 9. Open decisions for the author

Each lane of this round chose a rule where the review did not settle one. They are written into the rulebook and are listed here to be confirmed or reversed.

**Checkpoint and replay (lane a)**
1. Order age is a per-book arrival sequence, re-drawn whenever an order loses priority; a triggered stop keeps its admission age.
2. Only the engine-wide kill flag and the graduated switch's scopes persist; throttle credit and `kill()` do not.
3. A refused order or replace leaves no trace in the age counter or the reference price. OTR `ordersSubmitted` still counts refused orders live, which plain replay cannot reproduce.
4. Replay refusals stop the boot, except a legacy journal under `--replay-legacy-journal`; a cancel that finds no order is not a refusal.
5. The volatility dwell start is carried by checkpoints only.

**Breaker, STP, minimum quantity (lane b)**
1. The breaker is per print: a sweep stops at the band, keeps its in-band fills, and the rest is an auction arrival.
2. Replay evaluates the breaker, so a replaying engine must use the live threshold.
3. A replace is no longer refused on its limit price; `VolatilityCircuitBreaker` is never emitted (kept for the wire).
4. A FOK or minimum-quantity order the band cannot fill is refused or cancelled without tripping the breaker.
5. Elected stop-limits and trailing stops are not re-screened against the admission band.
6. Before the first print there is no reference, so no breaker and no band; an uncross seeds it if none is set.
7. Minimum quantity is met on arrival or the order is cancelled. (Amended by lane j2: the uncross now enforces it too; only orders the cross does not reach rest with an unmet minimum, unprotected as makers.)
8. `DecreaseAndCancel`'s update to the resting order carries that order's own status.

**Execution reports and ids (lanes c and j)**
1. A report with no live session is dropped and counted; no drop copy or replay on reconnect.
2. With auth off, a session receives reports for every participant it has claimed, from the claim onward.
3. Reports are always on, unsolicited, with `clientRequestId` 0; lockstep clients must skip them.
4. `Accepted` updates are sent too; on the async path an Ack means "ran and journaled", not "booked".
5. A full report queue drops and counts the event; the worker never blocks.
6. The library's synchronous `TcpGateway` sends no reports.
7. Under a non-Immediate journal, reports force a flush per request.
8. Cancel, modify and replace must name the participant that entered the order; journals written before scoped ids hold unscoped ids, so after an upgrade those orders cannot be cancelled by id unless their participant is 0.
9. An amend outcome is an `OrderUpdateNotification` whose `rejectReason` on a non-Rejected status means "refused"; "no such order" is `Rejected`/`OrderNotFound`.
10. While the journal is halted the admin book views refuse; the in-process `getSnapshot` still serves memory.

**Halted journal (lane d)**
1. The kill switch is refused on a halted journal; `setKillSwitch(true)` still sets its in-memory flag.
2. Engine-initiated cancels are journaled before they are applied.
3. The request whose own record halts the journal stays applied in memory until restart.
4. Under GroupCommit, `durableClientAcks()` reports false and `startAsync` silently turns the gate off rather than refusing.
5. A directory that cannot be synced halts the new journal at its first record rather than refusing to boot.

**Level totals and benchmarks (lane e)**
1. Every notify draws a sequence number, listener or not, so a listener-free book's trade sequence numbers changed.
2. Pro-rata `DecreaseAndCancel` publishes the shrunk level at once (an extra L2 update).
3. `SustainedLoadTest` fails above 100 ms of backlog; `bench_x86` records the 400k/s run without failing the whole run.
4. The coordinated-omission benchmark defaults to fixed arrivals for comparability; Poisson is arguably the honest default.
5. No absolute latency threshold change; to catch a doubling, make the A/B gate a required check.

**Auction, OCO, admission (lane f)**
1. A same-owner pair is resolved before the cross only if both are executable at the clearing price, to a fixed point.
2. The in-loop auction STP branch is deleted and replaced by an assertion.
3. A halted close cancels released MOC/LOC and parked market orders, status `Cancelled`.
4. OCO links are scoped to the book and the live order, with no owner argument.
5. A past expiry is refused as `InvalidFieldValue`; Market joins IOC and FOK in needing no session end for DAY.
6. Fat-finger setters throw past the symbol bound.

**Boot and config (lane g)**
1. Waivers are per control and named; a waived control that is configured anyway still applies.
2. With credentials on, every credentialed participant needs a position limit; with auth off and no waiver, the `participants` list must be non-empty.
3. `tick_size`, `lot_size` and `otr_limit` keys are refused, which breaks existing configs that set them.
4. Fees, risk tiers and CAT were deleted from the binary's surface rather than wired.
5. The image defaults to the example config (participants 100 and 101) and a journal on its volume; other participants need a mounted config or the engine refuses to start.

**Second follow-up lane (j2)**
1. `Triggered` is a new status on the wire (value 7), reported once per election, deferred or not; it does not change `Order::status`.
2. Every transition into Continuous runs the stop, trailing-stop and peg sweeps, so pegs also reprice at the open. A reopen whose elections trip the breaker is applied and journaled, then its trip is journaled.
3. An order the cross would fill below its minimum is cancelled untraded and the cross is priced again. An order the cross does not reach rests into Continuous with its minimum unmet, unprotected as a maker; cancelling those at the reopen is the remaining option.
4. Replay searches other books for a missing order only in a legacy-epoch journal; in any other journal a record that misses its own book is a no-op.
5. On a replace the minimum-quantity screen runs before risk, so when both would refuse, the reason is `FOKInsufficientLiquidity`.

**Formal methods (lane h)**
1. `Oco.tla` and `Risk.tla` are kept as library-only specs.
2. The auction self-cross and the replace/breaker interaction are covered by tests and stated as outside the specs, not modelled.
3. Traces are a TLA+ module, not JSON.
4. `check.sh` pins the TLC jar's hash.
5. `MatchingEngineDeep` uses one price; price priority across levels stays with `MatchingEngine.cfg`.

---

## 10. Known gaps

Open on this commit.

**Matching**
- An order the uncross does not reach rests into Continuous with its minimum quantity unmet and can execute below it as a maker (MEDIUM). Nothing tests a parked minimum-quantity market order in the cross; that path is unexercised.
- The released-LOC list still swap-removes (LOW). The list only decides the order in which released LOC orders are cancelled at the close; their priority comes from the book's level queues, so matching is unaffected, but after a removal those cancel events are not in arrival order.
- MOC/LOC are accepted with no session schedule and can never execute (MEDIUM).
- A market order's notional with the collar off is valued at the reference price, an underestimate for a buy that walks the book.
- Pro-rata and market-maker roles cannot be enabled in the binary.

**Order entry**
- No drop copy or replay on reconnect; reports to an absent session are lost.
- Admin reads of a book (`/book`, `/audit`, `/auction`, `/otr`) take that book's lock and wait behind its worker. While the worker is saturated a read is slowed by orders of magnitude, though not starved (seen once in a local flood on Linux, not recorded as a measurement).
- Refusal and "no such order" updates from the engine carry sequence id 0.
- An early return in `main` after `startAsync` destroys the gateway before the workers stop (error paths only).
- Each report scans all sessions; one MPSC queue serves all workers.

**Durability**
- A new order, a session transition or an uncross whose own record fails stays applied in memory until restart (MEDIUM: contained by the halt freeze, and dropped by the restart). A new order needs `addOrder` split around its inline expiry of makers; a transition needs the record inside `setTradingStateLocked`.
- An expiry sweep that halts partway finishes that book's expiries in memory.
- The plain journal does not record the volatility dwell start.
- Only one write-and-fdatasync chain can be outstanding under GroupCommit (`PERF-4`); `OrderEngine` uses Immediate and is not affected.
- Concurrent synchronous submitters can journal out of apply order (`JOURNAL-8`, library sync mode).
- `CheckpointEquivalenceTest` does not drive the async cut or the expiry clock.
- Kill-switch throttle credit is not persisted.

**Configuration and deployment**
- Per-instrument tick and lot sizes are not supported (fixed 0.0001 tick).
- `config/engine.conf.example` still carries keys nothing reads, labelled as such.
- No CI step runs `docker compose`; the feed was not checked end to end through compose.

**Performance and measurement**
- Nothing measured on this branch, on any machine, that this document will quote (§5).
- No latency gate times a deep-level or listener-heavy workload.
- `bench_x86.sh`'s handling of a VM with `perf` but no hardware counters has not run on x86.
- Running all HonestBenchmark paths in one process has an order effect; `--only` runs one path per process.

**Formal methods**
- The two-price, four-order `MatchingEngine` bound has not been run; `MpscQueue` has no broken configuration.
- Trace validation is sampled, limited to limit orders and their amendments, and drives `OrderBook` only.

**Research**
- The BCS paper's Exp 4 does not reproduce on the current engine, and its headline is gross of fees (§8).

---

## 11. Verification record of this branch

All of it was run by hand on one machine; none of it is CI.

**At `a48cc27`, the commit this document describes:**
- macOS (Apple clang, Release), full suite: 715/715; `WorkerPinTest` skips by design on macOS.
- Linux (`ubuntu:24.04` container, GCC 13), all four lanes:
  - Release: 715/715.
  - ThreadSanitizer: 714/714.
  - Fault injection: 714/714.
  - ASan/UBSan: 710/710 through ctest, plus five tests that time out in this slow container and pass when run directly without a time limit (`PropertyTest`, `CheckpointEquivalenceTest`, `SessionControlRaceTest`, `MarketDataTornReadTest`, `AdminServerEndpointsTest`).
- After `a48cc27`: one added test case (`7214ac6`, the reopen-trips-the-breaker restart in `CheckpointCompleteStateTest`), which passes on macOS and under ASan/UBSan on Linux, and edits to this document. No source file changed.

**What integration and the Linux lanes caught on the way** (each fixed before the run above):
- a client that reset its connection killed the engine with `SIGPIPE` (on `main` too; missed by the review);
- a data race on the OCO event list between a session-end sweep and the book's worker, introduced by this round's own OCO fix;
- references bound to misaligned fields of packed structs in two places (the gateway's request handling, the kill-switch restore), which UBSan aborts on;
- a latched trailing stop losing its trail reference across a checkpoint, found when the equivalence test was also run with a tight price band;
- two tests that were wrong rather than the engine: a lock-order report caused by stack address reuse, and a race in the "nothing ahead of the journal" test, which failed in 248 of 400 loaded runs before its fix and in 0 of 300 after (a diagnostic build showed no trade ever reached the feed early).

**CI, on the pull request to `main`:**
- At `88b8e32`: `ci.yml` (macOS Release, Ubuntu Debug Sanitizers, Fault Injection, io_uring Journal, Lean Mode, Release Latency Gate, libFuzzer, Wide Differential Fuzz, Docker image, Runbook smoke), `tla.yml` and `benchmark-gate.yml` passed. Of the two ThreadSanitizer runs of that commit, one passed and one timed out in `AdminServerEndpointsTest`.
- The timeout was a test that could not finish on a slow runner, not a race. Under TSan a saturated worker hands the book's lock to the admin thread rarely, so a fixed number of scrapes that took seconds on one runner passed the limit on another. The test now runs a minimum number of scrapes and then stops at a time budget (`1bd383c`); in a container held to two CPUs with competing load it finishes inside the budget with no TSan report.
- The two earlier failures were in the TLA+ job's own setup and are described in §6.

- **Not done:** a merge to `main`; any x86 benchmark on this branch (`bench-x86.yml` runs only when dispatched and has not been).

---

## 12. File structure

```
include/        76 headers
src/            11 sources; main.cpp is OrderEngine
tests/          test sources, shell tests of the real binary, CTest registrations
benchmarks/     HonestBenchmark, RealisticFlow, CoordinatedOmission, ColdCache,
                SustainedLoadTest and others
fuzz/           fuzz_gateway_engine (libFuzzer) and a standalone driver
spec/           7 TLA+ specifications, their broken configurations, trace module
tools/          journal replay CLI, journal mutation fuzzer, ITCH replay, time machine
config/         engine.conf.example
docs/           Runbook, MatchingRules, Compliance, Verification, CLAIMS,
                PERF_SOURCES, CapacityPlanning, ProductionReadiness, OSTuning, BUILD
bcs_research/   Python research on the engine via pybind11 (§8)
```
