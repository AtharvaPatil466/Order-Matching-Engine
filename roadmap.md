Phase 1: Core Engine Optimization
Issue 1: Full-Mode Performance Cliff
Your current architecture has a 13x performance gap between lean mode and full mode. This is unacceptable for production because real venues always run with risk checks, OTR tracking, and circuit breakers enabled. The gap indicates these features are implemented as afterthoughts rather than being co-designed with the matching core.
Week 1: Profiling and Data Structure Restructuring
Begin with systematic profiling using Linux perf. Run perf record -e cycles,cache-misses,branch-misses -g ./bin/ManualBenchmark --full and generate flame graphs with Brendan Gregg's tools. You need to identify exactly where the 520ns average is spent. Specifically, measure L1 cache miss rate on the risk check path, branch misprediction rate on the circuit breaker check, and TLB misses on OTR lookups.
Concurrently, restructure the participant risk state. Currently, risk data for multiple participants likely shares cache lines, causing false sharing when different threads access different participants. Define struct alignas(64) ParticipantRiskState where every field is sized to fill or pad to 64 bytes. Place the most frequently accessed fields first: position limit, current exposure, order count in flight. Use __attribute__((packed)) carefully only where it improves cache density without crossing alignment boundaries. Document the layout with ASCII diagrams in the header file.
Week 2: OTR and Risk Check Optimization
Your OTR tracking must not use any standard library associative container. Extend your existing FlatHashMap to serve OTR lookups. The key is a 64-bit composite of participant ID and symbol ID. The hash function should be a simple multiplicative hash or even the identity if your participant IDs are already well-distributed. Ensure the probing sequence touches at most two cache lines. Pre-size the table to the next power of two above your maximum expected participant count, and never rehash on the hot path.
For risk checks, implement batch validation. Instead of checking one order against risk limits, accumulate a batch of 8–16 orders in a ring buffer, then validate them in a tight loop using SIMD comparisons if possible. Even without SIMD, the loop unrolling and instruction-level parallelism will improve throughput. The batch size should be configurable but default to 8. Measure the optimal batch size on your target hardware.
Week 3: Circuit Breaker and Event Path Optimization
Move the circuit breaker check from per-order to per-batch. Maintain a per-symbol atomic flag that is checked once per batch or once per microsecond, whichever comes first. The flag should be in its own cache line, updated by a background thread that monitors price volatility. On the hot path, this becomes a single predictable branch: if (unlikely(symbol->circuit_broken.load(std::memory_order_acquire))) handle_break();. The unlikely macro ensures the branch predictor learns the fast path.
For event notifications, eliminate all dynamic allocation. Pre-allocate a ring of event buffers per worker thread. Each buffer is a fixed-size array of trade records. When a batch completes, publish the buffer index to a consumer thread via a single-producer single-consumer queue. The consumer serializes to the output format. This separates allocation from the hot path entirely.
Week 4: Integration and Benchmarking
Integrate all optimizations and run comparative benchmarks. The benchmark harness must run lean mode, then full mode with identical order flow, and report the ratio. Target: full mode within 3x of lean mode. If lean mode averages 100ns, full mode must average under 300ns. Document every optimization with before/after numbers in docs/PerformanceWhitepaper.md. If the target is not met, return to profiling and iterate.
Issue 2: Single-Symbol Throughput Ceiling
Your thread-per-symbol design means one hot symbol cannot exceed one thread's capacity. In practice, BTC-USD or ETH-USD during high volatility will saturate a single core while others idle.
Week 1: Price-Range Sharding Design
Design a price-range sharding scheme. Divide the possible price space into N contiguous ranges, where N equals your worker thread count. For example, with 4 threads and a price range of $0 to $200K, assign Thread 0 to $0–$50K, Thread 1 to $50K–$100K, and so on. The shard boundaries are stored in an array that is updated infrequently. Each thread maintains its own order book segment using the same FlatPriceMap structure.
The critical design decision is handling orders that cross shard boundaries. A buy order at $49K on Thread 0 might match against a sell at $51K on Thread 1. For this, implement a cross-shard match protocol.
Week 2: Non-Crossing Fast Path and Cross-Shard Protocol
For the common case where an order does not cross into another shard's price range, process it entirely within the local thread with zero coordination. This should be 95% or more of orders in normal market conditions.
For the remaining 5%, implement a lock-free single-producer single-consumer request queue between adjacent shards. When Thread 0 detects a potential cross-shard match, it publishes a match request containing the order ID, price, and quantity to Thread 1's queue. Thread 1 polls this queue between processing its own inbound orders. The response includes the matched quantity or a rejection. To prevent deadlocks, always acquire shards in price order: lower price shard first.
Week 3: Shard Boundary Migration and Sequencing
Market prices move. If BTC breaks above $50K, Thread 0 becomes overloaded and Thread 1 underutilized. Implement coarse boundary migration: every 10 seconds or when queue depth imbalance exceeds 2x, the admin thread recomputes boundaries based on current order distribution. Migration is not real-time; it is a background rebalancing. During migration, both old and new boundaries are valid for a brief overlap period to ensure no orders are lost.
For global sequencing, maintain a per-thread atomic sequence number. Cross-shard trades need a global sequence. Use a sharded sequence allocator: each thread has a local sequence space (e.g., Thread 0 gets 0, 4, 8...; Thread 1 gets 1, 5, 9...). This eliminates contention on a single atomic while preserving approximate ordering. For exact ordering of cross-shard trades, use a separate global atomic only for those events.
Week 4: Stress Testing
Run a 4-thread stress test with a single symbol. Generate 50,000 orders with prices clustered around a moving center to force cross-shard matches. Measure throughput, P50, P99, and P99.9. Compare to the single-thread baseline. Target: 4x throughput with P99.9 under 10 microseconds. Document the cross-shard match ratio and latency penalty.
Phase 2: Correctness and Verification
Issue 3: Incomplete TLA+ Coverage
You have specifications for the MPSC queue, engine consumer loop, and snapshot mechanisms, but not for the matching algorithm itself or the replication protocol. This leaves your most complex logic unverified.
Week 5: Matching Engine TLA+ Specification
Write MatchingEngine.tla. Model a simplified but complete order book with two price levels, two participants, and the full order type matrix: limit, market, IOC, FOK, GTD, cancel. Define the variables: bids, asks, orders (map from ID to order), trades (sequence of executed trades), and sequence_number.
Define actions: PlaceLimit, PlaceMarket, PlaceIOC, PlaceFOK, PlaceGTD, CancelOrder, ExpireGTD. Each action updates the book state and appends to trades if execution occurs. The matching logic must implement price-time priority exactly as your C++ code does.
Prove the following invariants:
NoNegativeQuantity: No order or trade has negative quantity.
FIFO_Preservation: For any two resting orders at the same price, if order A was placed before order B, then A is fully filled before B receives any fill.
Quantity_Conservation: The sum of all resting quantities plus all traded quantities equals the sum of all placed quantities minus canceled quantities.
GTD_Expiry_Correctness: No GTD order exists past its expiry time.
Run TLC model checker with at least 500,000 states. If state space explodes, use symmetry reduction on participant IDs and price values.
Week 6: Replication Protocol TLA+ Specification
Write Replication.tla. Model two nodes: Primary and Backup. Variables: primary_log, backup_log, primary_epoch, backup_epoch, heartbeat_timer, network_partition.
Define actions: PrimaryAppend, ShipLog, BackupAck, Heartbeat, PrimaryCrash, BackupPromote, NetworkPartition, NetworkHeal. The promotion action must only occur when backup has not received heartbeat for timeout duration AND backup's epoch is greater than last seen primary epoch.
Prove invariants:
NoCommittedLoss: If a trade appears in primary_log and the primary does not crash before acknowledging it to a client, then either the trade is in backup_log at promotion time, or the client received a rejection.
NoDuplicateExecution: No trade appears twice in the final committed log.
NoSplitBrain: Two nodes never simultaneously believe they are primary.
Run TLC with fault injection: primary crash mid-batch, network partition during promotion, journal truncation at arbitrary byte offset.
Week 7: Refinement Mapping
Write Refinement.tla that maps your C++ implementation's observable behavior to the abstract TLA+ specs. Define a refinement mapping function Abs that takes implementation state variables (memory addresses, atomic values, queue indices) and produces abstract state (order book, trade log).
Prove that every implementation action corresponds to a stuttering step or a valid abstract action. This is the hardest part and may require simplifying assumptions (e.g., treating memory as sequentially consistent). Document all assumptions explicitly.
Week 8: CI Integration
Integrate TLC into your GitHub Actions workflow. Install TLC via Java, run all specs on every pull request. Fail the build if any invariant is violated or if state space coverage is below threshold. Add a badge to README.md showing verification status.
Issue 4: Memory Ordering and Lock-Free Correctness
Your lock-free structures use atomics, but the exact memory ordering choices may not be correct across ARM and x86, and the ABA problem may exist in your ring buffer.
Week 5: Atomic Audit
Audit every std::atomic operation in MpscQueue.h, IntrusiveList, RingBuffer, and FlatPriceMap. Create a spreadsheet with columns: file, line number, variable name, operation (load/store/exchange), current memory order, justification, recommended memory order, rationale.
For each operation, write a litmus test: a small C++ program that demonstrates the visibility guarantee you need, and verify it with ThreadSanitizer and on both x86 and ARM if possible. Document the litmus test in a comment block above the atomic operation.
Week 6: ABA Protection and Reclamation
In your ring buffer, if head and tail indices can wrap around, two different operations may appear identical to a CAS loop, causing ABA. Implement tagged indices: use a 64-bit value where the high 16 bits are a tag incremented on every wrap, and the low 48 bits are the actual index. This requires compare_exchange_weak on the full 64-bit value.
For ObjectPool reclamation, verify that when a thread deallocates an order node, no other thread can still be reading it. If your worker threads are the only consumers of their own pool, this is trivial. If pools are shared, implement epoch-based reclamation: maintain a global epoch counter, threads announce quiescence, nodes are freed only after all threads have passed the epoch in which they were retired.
Week 7: ThreadSanitizer Expansion
Your current CI runs ThreadSanitizer on unit tests. Expand this to stress tests and chaos tests. Specifically, run TSan on StressTest, QueueChaosTest, and CombinedChaosTest. TSan is slow; expect 10x slowdown. Run for 30 minutes per test. Any data race, even benign, must be fixed or documented with a TSan suppression and engineering justification.
Week 8: ARM Verification
Your benchmarks run on Apple Silicon, which is ARM64. Verify that your atomic operations compile to ldxr/stxr or casal instructions, not locks. Use objdump -d on the binary and grep for these instructions. Document the mapping in docs/Architecture.md. If any atomic compiles to a library call (__atomic_load_8), refactor to use std::atomic with the correct type size to ensure inline expansion.
Phase 3: Operational Hardening
Issue 5: Deterministic Replay as Debug Tool
Your journaling supports crash recovery replay, but this capability is not exposed for debugging, testing, or regulatory queries.
Week 9: Time Machine CLI
Build a command-line tool OrderEngine --replay --journal file.journal --from 2026-05-01T09:30:00.000Z --to 2026-05-01T09:31:00.000Z --output state.json. This reconstructs the exact order book state, active orders, and trade history at every nanosecond within the range. The implementation reads journal entries sequentially, applies them to a fresh engine instance, and snapshots state at requested intervals.
Add --divergence-check primary.log which replays the journal and compares every output event (trades, cancels, market data updates) byte-for-byte against a recorded primary log. Report the first nanosecond where divergence occurs, with expected vs actual state.
Week 10: Shadow Mode
Implement shadow mode in the backup engine. Normally, the backup consumes replication stream but does not execute trades. In shadow mode, it executes the full matching logic silently: it maintains an internal book, generates internal trades, but does not publish market data or send execution reports. Every 100ms, it compares its internal book state against the primary's published state (via replication stream). Any divergence triggers an alert.
This catches logic bugs that replication alone would not: if both primary and backup have the same bug, replication looks correct but the output is wrong. Shadow mode with divergence detection finds this.
Week 11: Record-and-Mutate Fuzzing
Record live or synthetic traffic into a journal file. Build a mutator that reads the journal and produces variants: drop every Nth order, delay cancels by random microseconds, flip price by one tick, duplicate orders with new IDs. Replay each variant and verify invariants: no negative quantities, no orphan orders, sequence numbers monotonic.
This is coverage-guided fuzzing at the system level, not just protocol parsing. Integrate with libFuzzer by treating the mutation seed as the fuzz input.
Week 12: Regulatory Audit API
Add HTTP endpoint GET /audit?timestamp=1234567890123456789&symbol=BTC-USD. This returns the exact book state at that nanosecond: best bid/ask, spread, depth at 5 levels, last trade price and quantity, active order count. The implementation replays the journal to that point or uses a stored snapshot if available within 100ms.
This must be deterministic: same timestamp, same output, every time. Test by querying 1000 times and verifying byte-identical responses.
Issue 6: Market Data Gap Recovery
Your shared memory market data feed has no mechanism for consumers that start late or miss updates.
Week 9: Snapshot Plus Incremental Protocol
Design a protocol where the shared memory segment contains a rotating ring of messages. Every 100ms, a full book snapshot is written, followed by incremental updates. The snapshot includes magic number, version, sequence number of first incremental, and the full L2 depth. Incrementals are delta messages: add, modify, delete.
The ring has fixed-size entries. When the ring wraps, old entries are overwritten. A consumer that is faster than the producer reads sequentially. A consumer that falls behind detects a sequence gap and requests a fresh snapshot.
Week 10: Versioned Snapshot Ring
Implement the ring as two contiguous memory-mapped regions: one for the current snapshot, one for incrementals. Use POSIX shared memory with named semaphores for producer-consumer synchronization. The snapshot region is written atomically using a sequence number flip: write new snapshot, then atomically update a header pointer. Consumers always read the header pointer first, then read the referenced snapshot without locking.
Week 11: Gap Detection and Catch-Up
Consumers track the sequence number of last processed message. If the next message's sequence does not equal last plus one, a gap exists. The consumer sets a flag and enters catch-up mode: it reads the latest snapshot, then applies all incrementals from the snapshot's base sequence forward.
Add admin endpoint POST /marketdata/snapshot?symbol=BTC-USD to force an immediate snapshot. This is useful when a new consumer connects.
Week 12: Catch-Up Latency Benchmark
Measure the time for a consumer to start 5 seconds late and catch up to real-time. Generate 10,000 updates per second. The consumer should catch up within 10ms of connecting. Document the measurement methodology: consumer timestamp on connect, timestamp when sequence gap closes, difference.
Phase 4: Compliance and Safety
Issue 7: Missing Regulatory Features
Your risk checks are basic. Production venues need self-trade prevention, wash trade detection, and market-wide circuit breakers.
Week 13: Self-Trade Prevention
Implement STP with four modes per participant:
CancelResting: When a new order would match against the same participant's resting order, cancel the resting order.
CancelIncoming: Cancel the new order.
CancelBoth: Cancel both.
DecreaseAndCancel: Decrease the resting order's quantity by the match amount, cancel if zero.
Store STP mode in ParticipantRiskState. Check it in the matching loop before executing a trade. If STP triggers, generate a cancel acknowledgment but no trade. This must not allocate; use pre-allocated cancel records.
Week 13: Wash Trade Detection
Maintain a map of beneficial owner IDs. Each participant maps to one or more beneficial owners. Before executing a trade, check if both sides share any beneficial owner. If yes, flag the trade in the audit log and optionally reject based on configuration. The check is a bitset intersection: represent each participant's owners as a 256-bit bitset, AND the bitsets, check for any set bit. This is O(1) and SIMD-friendly.
Week 14: Limit Up-Limit Down
Implement LULD-style volatility pauses. Track the reference price (last auction price or rolling VWAP) per symbol. If the best bid or ask deviates more than X% from reference, enter a pause state. In pause, accept orders but do not match. Publish a market data message indicating pause. After Y seconds, resume with an auction. X and Y are configurable per symbol.
Week 14: Graduated Kill Switch
Extend the existing kill switch to four levels:
Throttle: Per-participant token bucket rate reduced to 10%.
SymbolHalt: Stop matching for one symbol, accept cancels only.
GlobalHalt: Stop all matching, accept cancels only.
Kill: Terminate process after writing checkpoint.
Each level has a distinct HTTP admin endpoint and a distinct alert severity. Document the escalation policy: when to use each level.
Week 15: Reg SCI Incident Logging
Every halt, circuit break, or kill switch must generate a structured log entry with: timestamp (nanosecond), level, symbol (if applicable), triggering condition, current book state summary, participant count, order count. Log to a separate file from normal operations, rotated hourly. Format is newline-delimited JSON for easy ingestion.
Week 15: Capacity Exhaustion Alerts
Add alerts for: queue depth > 90% of capacity, memory usage > 95% of limit, journal disk usage > 90%, replication lag > 100ms. Each alert triggers a webhook with configurable severity. Integrate with your existing AlertDispatcher.
Week 16: Compliance Documentation
Create docs/Compliance.md. Map every implemented feature to a specific regulatory requirement:
MiFID II RTS 6 Article 4: Kill switch → KillSwitch.h
MiFID II RTS 6 Article 5: Self-trade prevention → STP in MatchingEngine
SEC Reg SCI Rule 1001(a): Capacity planning → CapacityPlanning.md
SEC Reg SCI Rule 1001(b)(1): Business continuity → ReplicationProtocol.h
This document is for auditors and regulators. Be precise with citations.
Phase 5: Testing and Benchmarking Discipline
Issue 8: Non-Reproducible and Insufficient Benchmarks
Your current benchmarks are machine-specific, run on Apple Silicon, and do not stress tail latency or sustained load.
Week 17: Deterministic Benchmark Harness
Build a harness that accepts a fixed seed for random number generation, a fixed order sequence file, and runs the exact same workload 100 times. Report P50, P99, P99.9, P99.99, P99.999 across all runs. Use hardware timestamp counters (rdtsc on x86, cntvct_el0 on ARM) for measurement. Calibrate TSC to nanoseconds using a one-time measurement against clock_gettime(CLOCK_MONOTONIC).
The harness outputs a JSON file with the full distribution, not just averages. Store this JSON in CI artifacts for regression comparison.
Week 17: Sustained Load Test
Run 1 million orders at a constant rate of 100,000 orders per second for 60 seconds. Measure latency every millisecond. Plot latency over time. Look for drift: does P99 increase from 500ns at second 1 to 2μs at second 60? This indicates garbage accumulation, journal rotation stalls, or memory fragmentation. Fix any drift before proceeding.
Week 18: Latency Correlation Analysis
Instrument the engine to emit events: journal rotation start/end, snapshot start/end, config reload. In post-processing, correlate these events with latency spikes. Build a simple tool that reads the benchmark JSON and the event log, and reports: "Journal rotation at T+45.3s correlated with 3μs latency spike in 0.1% of orders."
Week 18: Memory Pressure Test
Run the engine with ulimit -v set to 95% of normal working set size. Measure throughput and latency degradation. If the engine crashes or latency increases 10x, identify the allocation path and fix it. All memory should be pre-allocated; this test verifies that.
Week 19: Multi-Day Soak Test
Run the engine continuously for 72 hours with realistic order flow: 10% of orders are GTD with 1-hour expiry, journal rotates every hour, config reloads every 6 hours. Monitor for: memory growth (should be flat), latency drift (should be stable), journal corruption (verify with CRC on every rotation), GTD expiry accuracy (verify no expired orders remain).
Week 19: Benchmark Regression Gates
In CI, run the deterministic benchmark on every pull request. Compare P99 against the baseline from the main branch. If P99 increases by more than 10%, fail the build. Store baseline results in a Git LFS file or CI cache. Document the methodology in docs/Benchmarking.md.
Week 20: Reproducible Benchmark Kit
Publish a Docker image with pinned compiler version, standard sysctl settings documented, and a script that runs the full benchmark suite. Include a README that explains: how to disable CPU frequency scaling, how to isolate cores, how to interpret results. This allows anyone to reproduce your numbers on compatible hardware.
Integration and Dependencies Between Phases
Phase 1 must complete before Phase 3 because operational tools are meaningless if the core engine is slow. Phase 2 can proceed in parallel with Phase 1 after Week 2, as TLA+ specification is independent of implementation optimization. Phase 4 depends on Phase 2 because compliance features must be formally correct. Phase 5 runs continuously and validates all previous phases.
The critical path is: Week 1–4 (core perf) → Week 9–12 (replay and market data) → Week 17–20 (benchmarking). The parallel track is: Week 5–8 (TLA+) and Week 13–16 (compliance), which can proceed alongside the critical path after Week 4.
Success Criteria for S-Tier



