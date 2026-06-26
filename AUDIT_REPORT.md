# Repository Audit — Matching Engine

**Date:** 2026-06-27
**Branch:** bcs-research-phase1
**Type:** Read-only audit — no code was modified.

## Method

Five parallel read-only reviewers across the four audit dimensions (correctness split
into invariants/order-types/overflow + concurrency/replication). Findings consolidated
and severity-adjudicated below. Line numbers are as reported by the reviewers. Each
**Critical** should be confirmed with a targeted regression test before any fix.

**Severity scale:** Critical = invariant violation / data loss / memory-unsafe reachable
at runtime · High = real bug or edge case · Medium = latent / maintainability · Low = minor.

## Tally

| Severity | Count |
|----------|-------|
| Critical | 7     |
| High     | 19    |
| Medium   | 18    |
| Low      | 6     |

**Top risks (read these first):** notional-overflow risk bypass (`OrderBook.cpp:124`),
trailing-stop price underflow (`OrderBook.cpp:521/1311`), ack-before-fsync durability
hole (`Journal.h:399`), MIT-cancel use-after-free (`OrderBook.cpp:1035`), per-fill
virtual dispatch + per-order `shared_mutex` on the hot path, and the MoldUDP64 infinite
re-request loop (`MoldUDP64.h:283`).

---

## 1. Correctness Issues

### (a) TLA+ invariant violations reachable at runtime

| File:Line | Severity | Issue |
|-----------|----------|-------|
| `src/OrderBook.cpp:124` | **Critical** | `Price notional = price * (Price)qty` is an unchecked signed int64 multiply; large qty×price wraps negative, so `notional > maxOrderNotional` is false and the order-notional risk limit is bypassed. `HierarchicalRiskManager.cpp:57` widens to `long double` — the book-level check never got that fix. |
| `src/OrderBook.cpp:521`, `:1311` | **Critical** | Sell-side TrailingStop `stopPrice = trailRefPrice - trailAmount` underflows when `trailAmount > trailRefPrice` (init) or on repeated ratchets; the wrapped `stopPrice` fires the trigger immediately / at a corrupted price. The mirror add on the buy side (`:518`) is unchecked signed-overflow UB at extreme inputs. |
| `src/OrderBook.cpp:717-728` | High | STP `DecreaseResting` on an Iceberg decrements `remainingQty` but not `visibleQty`, breaking `visibleQty ≤ remainingQty`; a later fill (`:738`) subtracts from both and underflows `remainingQty` to `UINT64_MAX` — a direct `NoNegativeQuantity` break. |
| `src/OrderBook.cpp:1213-1214` | High | GTD expiry uses `currentTime >= expiryTime`; a GTD/DAY order left at the `expiryTime = 0` default (Order.h init) is expired on the first `expireOrders()` tick regardless of clock. |
| `src/OrderBook.cpp:160-177` | Medium (latent) | `checkLiquidity` (FOK pre-check) sums `remainingQty` for icebergs while `match()` consumes only `visibleQty`. Reviewer partially self-refuted (multi-slice refill usually closes the gap); real risk only for price-limited FOK against multi-level books with icebergs. |

### (b) Concurrency / races the chaos suite may not cover

| File:Line | Severity | Issue |
|-----------|----------|-------|
| `src/MatchingEngine.cpp:1182-1306`, `:523-540` | High | The replication apply path calls `addSymbol` (mutating unsynchronized `FlatHashMap books_` / `symbolIds_`, possibly rehashing) on the receive thread while workers do `getOrderBook`/`find`; `booksFrozen_` doesn't guard the replication path. Data race on `books_`. |
| `src/MatchingEngine.cpp:1147-1179`, `:1308-1336` | High | `streamSnapshot` and `checkpointInternal` iterate `forEachOrder` **without** `bookLock_`, racing live worker mutations of `orderLookup_`. |
| `include/ShardedOrderBook.h:170` | High | `getShardIndex` reads `boundaries_[]` without `migrationMutex_` while `rebalance()` writes them — torn read can misroute an order across shards. |
| `include/OrderBook.h:494` / `src/SimulationDriver.cpp:79` | Medium | SPSC `RingBuffer tradeHistory_` written under `bookLock_` but read via `getTradeRingBuffer()` with no lock; not chaos-tested. |
| `src/MatchingEngine.cpp:1007-1024` | Medium | Sync-mode `expireOrders` iterates `symbolIds_` unsynchronized vs. the expiry timer thread. |

*Investigated, not flaws:* `MpscQueue.h:56` slot-sequence release pattern (sound for trivially-copyable `OrderRequest`); `checkpointInternal` double-invoke (`:1308`) produces a redundant-but-valid checkpoint, not data loss (Medium maintainability).

### (c) Replication losing a committed entry

| File:Line | Severity | Issue |
|-----------|----------|-------|
| `include/Journal.h:399-431` | **Critical** | `onCommit_` (which ships bytes to the backup) fires after `fflush` but **before** `fdatasync`/`F_FULLFSYNC`. A primary crash in that window leaves entries acked to the backup but not durable locally → backup diverges on promotion. Classic ack-before-fsync. |
| `include/ReplicationProtocol.h:815-822` | High | Live `JournalEntry` messages in flight before the backup processes `SnapshotStart` can apply ahead of the snapshot (Cancel-before-Insert); the `inSnapshot_` guard only closes the window after `SnapshotStart` is processed. |
| `include/ReplicationProtocol.h:700-705` | High | Backup computes lease validity against its **own** clock (`grantedAtMs = nowMs()` at accept); clock skew > `leaseDurationMs` lets it promote while the primary is alive → split-brain. |
| `include/ReplicationProtocol.h:393-399` | Medium | `sendSeq_` `fetch_add` happens before `sendMu_`, so wire `sequenceNum` can go out of order and is useless for gap detection. |
| `include/JournalFollower.h:127-141` | Medium | Follower re-reads via a fresh `Journal(path).readAll` with no advisory lock; can read a partially-written final entry (CRC-stops, silently truncating the view) — untested under fault injection. |
| `include/ReplicationProtocol.h:628-682` | Low | `startAsPrimary` leaks heartbeat + receive threads if `lease_.tryAcquire()` returns false after they're started. |

### (d) Order-type edge cases with no test

| File:Line | Severity | Issue |
|-----------|----------|-------|
| `src/OrderBook.cpp:1035` | **Critical** (UAF) | `cancelOrderImpl` guards only `Stop`/`StopLimit`, **not MIT**; cancelling a parked MIT frees the Order but leaves a dangling pointer in `stopOrders_`, dereferenced by the next `checkStopOrders`. No test cancels a resting MIT. |
| `src/OrderBook.cpp:542-547` | High | Pegged order ignores `addToBook`'s return; on price-out-of-range it is marked Accepted + tracked but never rests. No test. |
| `src/OrderBook.cpp:514-525` | High | Sell-side TrailingStop init with no prior trade (`trailRefPrice = price`) has no test verifying trigger price. |
| `tests/` (multiple) | Medium/Low | Coverage gaps: STP `DecreaseResting` (any path) untested; GTD with `expiryTime=0` untested; Pegged re-priced out of range by `updatePeggedOrders` untested; IOC+`minQty` cancel-status untested. |

### (e) Integer overflow in price×quantity

| File:Line | Severity | Issue |
|-----------|----------|-------|
| `src/OrderBook.cpp:124` | **Critical** | Same as 1(a) — notional multiply. |
| `src/OrderBook.cpp:1382-1384` | Medium | VWAP `double price*qty` loses precision at high session volume; `totalQty_ += qty` (uint64) can wrap, diverging the VWAP denominator. |

---

## 2. Performance Issues

### (a) Heap allocation on the hot path

| File:Line | Severity | Issue |
|-----------|----------|-------|
| `include/FlatHashMap.h:253-254` | High (dormant) | `rehash()` does `new Entry[]` mid-`insert`; dormant because `orderLookup_` is pre-sized to 200k, but there is no guard — >~100k simultaneous resting orders triggers a stop-the-world alloc inside `addOrder`. |
| `include/MatchingEngine.h:427-428` | High | `OcoBookListener` `executed`/`trades` are `std::vector` with no `reserve`; `push_back` per fill when observers active. |
| `src/MatchingEngine.cpp:577-583` | High | `driveOco` swaps out `std::vector` buffers and `contingency_.onExecuted` returns a vector by value — per-order/per-fill allocations when OCO/observers active. |
| `src/MatchingEngine.cpp:504-509`, `:1012-1015` | Medium | `std::function` constructed per timer tick / expiry (may heap-allocate). |

### (b) Virtual dispatch on the hot path

| File:Line | Severity | Issue |
|-----------|----------|-------|
| `src/OrderBook.cpp:776`, `:952` | **Critical** | `listener_->onTrade(t)` + `engineListener_->onTrade(t)` — two vtable indirect calls **per fill** in both `match()` and `matchProRata()`; defeats inlining and pollutes the indirect-call predictor. |
| `src/OrderBook.cpp:35-36` | High | `notifyOrderUpdate` → two virtual `onOrderUpdate` per accepted order/fill/cancel (guarded by `#ifndef OB_LEAN_MODE`; live in default build). |
| `src/OrderBook.cpp:67` | Medium | `onMarketData` virtual on resting-add (LEAN-guarded). |

### (c) Unnecessary Order/Trade copies

| File:Line | Severity | Issue |
|-----------|----------|-------|
| `src/OrderBook.cpp:763-776`, `:939` | High | 72-byte `Trade` passed **by value** into `tradeHistory_.push` and both `onTrade` calls every fill; should be const-ref/move. |
| `src/OrderBook.cpp:26` | Medium | `OrderUpdate` passed by value to both listeners (LEAN-guarded). |

*Note:* no by-value `Order` copies found on the hot path — the pointer/intrusive-list discipline holds.

### (d) Locks on the matching hot path

| File:Line | Severity | Issue |
|-----------|----------|-------|
| `src/OrderBook.cpp:208` | **Critical** | `addOrder` takes `unique_lock<std::shared_mutex> bookLock_` per order even on the single-threaded sync path; `shared_mutex` write-lock is materially costlier than a plain mutex and is paid unconditionally. |
| `src/MatchingEngine.cpp:624`, `:414-427`, `:581-616` | High | Feature-gated per-order/per-fill mutexes on the async worker path: `riskMutex_` (nested under `bookLock_`), `journalMutex_`, `catMutex_`, `feeMutex_`, `contingencyMutex_`. |

### (e) std::map / std::unordered_map instead of Flat*

**NONE.** All hot-path lookups use `FlatHashMap` (`orderLookup_`, `participantRisk_`, `stpModes_`)
and `FlatPriceMap` (`bids_`/`asks_`). Category clean.

---

## 3. Protocol Correctness

### (a) FIX fields parsed but not validated

| File:Line | Severity | Issue |
|-----------|----------|-------|
| `include/FixSession.h:212` | High | Missing `MsgSeqNum` (tag 34) returns `true` — a client omitting the required tag bypasses all sequence enforcement. |
| `include/FixSession.h:114-200` | High | `dispatch()` routes app messages (35=D/F/G) without checking `loggedOn_` — orders accepted before Logon. |
| `include/FIXParser.h:291-362` | High | NewOrderSingle required fields (11/55/54/38/40) silently coerced to 0/'\0' with `valid=true`; qty=0 / id=0 reach the engine with no reject reason. |
| `include/FIXParser.h:257-262` / `FixFramer.h:159-161` | Medium | `BodyLength` (tag 9) presence-checked but never cross-verified against measured body bytes. |
| `include/FixSession.h` (tag 56) | Medium | `TargetCompID` parsed but not validated against the venue's CompID. |
| `include/FIXParser.h:309-335` | Low | Unknown `OrdType` silently → `Limit`; unknown TIF → GTC (should business-reject). |

### (b) ITCH types generated but untested

| File:Line | Severity | Issue |
|-----------|----------|-------|
| `include/ItchProtocol.h:152` ('C' ExecutedWithPrice) | High | No byte-layout test, and the publisher never emits it → a price-differing (mid-peg/hidden) fill is mis-reported as plain 'E', violating ITCH 5.0 §4.7. |
| `include/ItchProtocol.h:169` ('X' OrderCancel) | High | Implemented, no layout test, never emitted by `ItchPublisher` → partial reduces invisible on the feed. |
| `include/ItchProtocol.h:193` ('P' Trade) | Medium | No layout test; non-display crosses produce no public message. |

### (c) SBE schema mismatch

**NONE.** Hand-coded codec in `SbeProtocol.h` is internally consistent — header
(blockLength/templateId/schemaId/version @0/2/4/6 LE), NewOrderV1 24B, NewOrderV2 32B,
OrderAck 12B all match and are pinned by `SbeProtocolTest.cpp`. Endianness correct.

### (d) MoldUDP64 sequence-gap scenarios

| File:Line | Severity | Issue |
|-----------|----------|-------|
| `include/MoldUDP64.h:283-300` / `ItchRetransmissionService.h:197` | **Critical** | Retransmitted messages are delivered out-of-band and never advance the subscriber's `nextExpectedSeq_`; when the live feed resumes the subscriber re-fires `onGapDetected` for the same range → infinite re-request loop. Untested. |
| `include/MoldUDP64.h:271-274` | High | Multi-packet gap (>1 missing) handling has no test verifying `onGapDetected` fires once, not per arriving packet. |
| `include/MoldPacketJournal.h:70-74` | Medium | `count==0` computes `endSeq = back().seq + 1`, wrapping to 0 at `UINT64_MAX` → silent empty replay. |
| `include/ItchRetransmissionService.h:82-83` | Medium | No session-ID validation → cross-session (`SESS_A` vs `SESS_B`) replays go undetected. |
| `include/ItchRetransmissionService.h:197` | Medium | Replay count up to 65535 with no server-side cap/timeout → a slow subscriber blocks the connection thread (DoS). |

---

## 4. Documentation vs Code Mismatches

### (a) Doc claims the code doesn't support

| Doc:Line (vs Code) | Severity | Issue |
|--------------------|----------|-------|
| `docs/CapacityPlanning.md:12` (vs `include/Order.h:93`) | High | Claims `Order` is **128 bytes**; code enforces `static_assert(sizeof(Order)==192)`. All downstream per-order memory formulas are understated. |
| `README.md:78,91` / `PerformanceWhitepaper.md:34` (vs `LatencyTracker.h:12-14`) | Medium | Claim timing via `mach_absolute_time`/`rdtsc`/`clock_gettime_nsec_np`; `HonestBenchmark`'s `nowNs()` uses `std::chrono::high_resolution_clock`. Those calls exist only in `ManualBenchmark.cpp`. |
| `README.md:82` / `PRODUCTION_ROADMAP.md:43` (vs `spec/`) | Medium | Say "7 specs"; there are 12 TLA+ specs (README is even self-inconsistent — :288/:369 say 12). |
| `Architecture.md:130` (vs `src/MatchingEngine.cpp:37-42`) | Low | References a `CPUAffinity` class; affinity is set inline via `pthread_setaffinity_np`/`thread_policy_set` — no such symbol exists. |

### (b) Benchmark numbers vs HonestBenchmark

| Doc:Line | Severity | Issue |
|----------|----------|-------|
| `BENCHMARKS.md:55-59` vs `PerformanceWhitepaper.md:68-78` | High | Same stated run (50k, seed 42, M3 Pro) but Path B P50 = **125 ns** vs **84 ns**, throughput 5.5M vs 7.1M; Path C also self-inconsistent within BENCHMARKS.md (958 ns raw vs 1,040 ns in TL;DR). Numbers from different runs presented as one. |
| `BENCHMARKS.md:3` vs `PerformanceWhitepaper.md:45` | Medium | Same results labeled `-O3 -march=native` in one doc, `-O2` in the other. |
| `benchmarks/HonestBenchmark.cpp:14` | Medium | Header comment says Path C uses `SyncPolicy::Immediate`; code uses `GroupCommit` batch=64 (function name/output are correct). |
| `Project_Overview.md` vs `README.md`/`BENCHMARKS.md` | Low | 396 vs 395 CTest count (actual is 396 per current `ctest`). |

*Cross-check with current local run:* `HonestBenchmark` P50 ≈ **125 ns** on Apple Silicon;
the historical **279 ns** baseline is x86-Linux-only and not reproduced locally — consistent
with the BENCHMARKS.md (125 ns) side of the conflict above.

### (c) TLA+ specs referencing renamed/restructured components

| Spec:Line (vs Code) | Severity | Issue |
|---------------------|----------|-------|
| `spec/Risk.tla:6-8` (vs `HierarchicalRiskManager.h:11-18`) | Medium | Models a **two-tier** (Firm/Trader) hierarchy; C++ `HierarchicalRiskManager` implements **four tiers** (Trader→Strategy→Account→Firm). `README.md:293` overstates the spec's scope. |
| `spec/MatchingEngine.tla:9` (vs `:153-158`) | Medium | Header claims "full order matrix (Limit/Market/IOC/FOK/GTD/Cancel) + price-time matching," but `Next` only has `PlaceLimit`/`CancelOrder`/`ExpireGTD`/`AdvanceTime` — no Market/IOC/FOK actions and **no matching/crossing action at all** (`docs/Verification.md:56` admits this; the spec header does not). |

---

## Caveats

- Reviewers were scoped by dimension across the relevant files (core engine,
  concurrency/replication, protocols, docs/spec) rather than each reading all 87 headers —
  coverage is thorough on the targeted subsystems; deep, rarely-touched headers
  (e.g. analytics, calibration) were not exhaustively audited.
- A few items are **dormant** (FlatHashMap rehash under correct pre-sizing) or
  **coverage gaps** (untested ITCH/Mold paths) rather than active bugs — labeled inline.
- Recommended verification order for the Criticals: (1) `OrderBook.cpp:124` notional
  overflow and (4) `:1035` MIT-cancel UAF are the cheapest to confirm with a unit test;
  (3) `Journal.h` ack-before-fsync needs a crash-injection test; the perf Criticals are
  confirmable by inspection.

*Generated by a read-only audit. No source files were modified.*
