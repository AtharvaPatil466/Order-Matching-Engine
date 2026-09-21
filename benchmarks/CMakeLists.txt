# Manual benchmark (no external deps)
add_executable(ManualBenchmark ManualBenchmark.cpp)
target_link_libraries(ManualBenchmark PRIVATE OrderMatcher)

# End-to-end latency benchmark (measures ingress → completion including queue delay)
add_executable(E2EBenchmark E2EBenchmark.cpp)
target_link_libraries(E2EBenchmark PRIVATE OrderMatcher)

# Deterministic benchmark with HW timestamps (Phase 5, Week 17)
add_executable(DeterministicBenchmark DeterministicBenchmark.cpp)
target_link_libraries(DeterministicBenchmark PRIVATE OrderMatcher)

# Sustained load test with degradation detection (Phase 5, Week 17)
add_executable(SustainedLoadTest SustainedLoadTest.cpp)
target_link_libraries(SustainedLoadTest PRIVATE OrderMatcher)

# Sharded order book stress test (Phase 1, Issue 2, Week 4)
add_executable(ShardedBookStressTest ShardedBookStressTest.cpp)
target_link_libraries(ShardedBookStressTest PRIVATE OrderMatcher)

# Honest three-path benchmark (single source of truth)
add_executable(HonestBenchmark HonestBenchmark.cpp)
target_link_libraries(HonestBenchmark PRIVATE OrderMatcher)

# Binary-codec microbenchmark — comparing OUCH / ITCH / SBE encode rates
add_executable(BinaryCodecBenchmark BinaryCodecBenchmark.cpp)
target_link_libraries(BinaryCodecBenchmark PRIVATE OrderMatcher)

# ─── Honest-tail benchmarks (P2-18..21) ──────────────────────────────────────
# All use the VENDORED header-only bench::BenchLatencyRecorder (BenchLatencyRecorder.h)
# and shared bench::WorkloadGenerator (RealisticWorkload.h) under benchmarks/.
# ZERO external dependencies — no HdrHistogram, no FetchContent.

# P2-18: realistic flow (power-law sizes, 44% cancel / 46% new / 8% IOC / 2% modify)
add_executable(RealisticFlowBenchmark RealisticFlowBenchmark.cpp)
target_link_libraries(RealisticFlowBenchmark PRIVATE OrderMatcher)

# P2-19: coordinated-omission correction (latency vs INTENDED start at fixed rate)
add_executable(CoordinatedOmissionBenchmark CoordinatedOmissionBenchmark.cpp)
target_link_libraries(CoordinatedOmissionBenchmark PRIVATE OrderMatcher)

# P2-20: cold-cache variant (evict working set before each timed op)
add_executable(ColdCacheBenchmark ColdCacheBenchmark.cpp)
target_link_libraries(ColdCacheBenchmark PRIVATE OrderMatcher)

# H6 audit: journalMutex_ contention — engine end-to-end + critical section alone
add_executable(JournalContentionBenchmark JournalContentionBenchmark.cpp)
target_link_libraries(JournalContentionBenchmark PRIVATE OrderMatcher)

# Thread-scaling curve — MatchingEngine async thread-per-symbol path, worker sweep
add_executable(ScalingBenchmark ScalingBenchmark.cpp)
target_link_libraries(ScalingBenchmark PRIVATE OrderMatcher)
