include(FetchContent)

FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
# For older CMake or if URL fails, we might need alternatives, but this is standard.
# However, to be safe and avoid network, I might check if GTest is installed.
# But FetchContent is standard for modern CMake.
# I'll stick to FetchContent.

FetchContent_MakeAvailable(googletest)

add_executable(UnitTests
    TestOrderBook.cpp
    RejectReasonContractTest.cpp)
target_link_libraries(UnitTests PRIVATE OrderMatcher gtest_main)

include(GoogleTest)
gtest_discover_tests(UnitTests
    XML_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results
    PROPERTIES LABELS "project;unit")

# enable_assertions(target) — applied per-test-binary to force
# assertion-rich, debug-friendly builds of TEST bodies. Note: this
# only affects the test executable's own .cpp files; the linked
# OrderMatcher library uses whatever the top-level CMake build_type
# selected (which itself enables -UNDEBUG when ENABLE_ASSERTIONS=ON,
# the default). So in a Release build the test body runs at -O0/-g
# while library calls stay at -O3 — inlining boundaries are at the
# call site, which matters when reading sanitized stacks.
function(enable_assertions target)
    target_compile_options(${target} PRIVATE -UNDEBUG -O0 -g)
    set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)
endfunction()

# Manual functional tests (assert-based)
add_executable(ManualTest ManualTest.cpp)
target_link_libraries(ManualTest PRIVATE OrderMatcher)
enable_assertions(ManualTest)
add_test(NAME ManualTest COMMAND ManualTest)
set_tests_properties(ManualTest PROPERTIES TIMEOUT 120 LABELS "project;functional")

# Stress Tests (no GTest dependency)
add_executable(StressTest StressTest.cpp)
target_link_libraries(StressTest PRIVATE OrderMatcher)
enable_assertions(StressTest)
add_test(NAME StressTest COMMAND StressTest)
set_tests_properties(StressTest PROPERTIES TIMEOUT 120 LABELS "project;stress")

# Benchmark Regression Tests (GTest, asserts p99 thresholds)
add_executable(BenchmarkRegression BenchmarkRegression.cpp)
target_link_libraries(BenchmarkRegression PRIVATE OrderMatcher gtest_main)
gtest_discover_tests(BenchmarkRegression
    XML_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results
    # RUN_SERIAL: latency thresholds are calibrated single-threaded/uncontended,
    # so these must not run concurrently with other ctest tests under -j — CPU
    # contention inflates p99 past the bar and false-fails (same principle as the
    # Debug+FI gate above: only measure/enforce in the calibrated configuration).
    PROPERTIES LABELS "project;benchmark-regression" RUN_SERIAL TRUE)

# The p99 thresholds are calibrated for an optimized Release build on x86 bare
# metal. Debug is 3-5x slower and fault injection adds hot-path overhead, so the
# threshold is only ENFORCED in a Release build without fault injection. NDEBUG
# can't carry this signal: ENABLE_ASSERTIONS=ON compiles -UNDEBUG, leaving NDEBUG
# undefined even in Release. Elsewhere the workload still runs and prints, but the
# assertion is replaced by GTEST_SKIP.
if(CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT ENABLE_FAULT_INJECTION)
    target_compile_definitions(BenchmarkRegression PRIVATE OB_ENFORCE_LATENCY_THRESHOLD=1)
endif()

# Journal Crash Recovery Tests (no GTest dependency)
add_executable(JournalCrashTest JournalCrashTest.cpp)
target_link_libraries(JournalCrashTest PRIVATE OrderMatcher)
enable_assertions(JournalCrashTest)
add_test(NAME JournalCrashTest COMMAND JournalCrashTest)
set_tests_properties(JournalCrashTest PROPERTIES TIMEOUT 120 LABELS "project;recovery")

# Gateway Integration Tests
add_executable(GatewayIntegrationTest GatewayIntegrationTest.cpp)
target_link_libraries(GatewayIntegrationTest PRIVATE OrderMatcher)
enable_assertions(GatewayIntegrationTest)
add_test(NAME GatewayIntegrationTest COMMAND GatewayIntegrationTest)
set_tests_properties(GatewayIntegrationTest PROPERTIES TIMEOUT 120 LABELS "project;integration")

# FIX TCP gateway integration test
add_executable(FixTcpGatewayTest FixTcpGatewayTest.cpp)
target_link_libraries(FixTcpGatewayTest PRIVATE OrderMatcher)
enable_assertions(FixTcpGatewayTest)
add_test(NAME FixTcpGatewayTest COMMAND FixTcpGatewayTest)
set_tests_properties(FixTcpGatewayTest PROPERTIES TIMEOUT 60 LABELS "project;integration")

# Submit-result contract tests
add_executable(SubmitResultTest SubmitResultTest.cpp)
target_link_libraries(SubmitResultTest PRIVATE OrderMatcher)
enable_assertions(SubmitResultTest)
add_test(NAME SubmitResultTest COMMAND SubmitResultTest)
set_tests_properties(SubmitResultTest PROPERTIES TIMEOUT 120 LABELS "project;api")

# Hot-path pre-trade risk controls (P2-8 kill switch, P2-9 position limits,
# P2-10 fat-finger, P2-11 OTR throttle)
add_executable(RiskControlsTest RiskControlsTest.cpp)
target_link_libraries(RiskControlsTest PRIVATE OrderMatcher gtest_main)
enable_assertions(RiskControlsTest)
gtest_discover_tests(RiskControlsTest
    XML_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results
    PROPERTIES LABELS "project;risk")

# AdminServer endpoint integration test (HTTP /prometheus, /health)
add_executable(AdminServerEndpointsTest AdminServerEndpointsTest.cpp)
target_link_libraries(AdminServerEndpointsTest PRIVATE OrderMatcher)
enable_assertions(AdminServerEndpointsTest)
add_test(NAME AdminServerEndpointsTest COMMAND AdminServerEndpointsTest)
set_tests_properties(AdminServerEndpointsTest PROPERTIES TIMEOUT 30
                     LABELS "project;ops;integration")

# Market data shared-memory schema compatibility / validation.
add_executable(MarketDataSchemaTest MarketDataSchemaTest.cpp)
target_link_libraries(MarketDataSchemaTest PRIVATE OrderMatcher)
enable_assertions(MarketDataSchemaTest)
add_test(NAME MarketDataSchemaTest COMMAND MarketDataSchemaTest)
set_tests_properties(MarketDataSchemaTest PROPERTIES TIMEOUT 30
                     LABELS "project;protocol;integration")

# Structured event sink — pluggable telemetry abstraction
add_executable(StructuredLogTest StructuredLogTest.cpp)
target_link_libraries(StructuredLogTest PRIVATE OrderMatcher)
enable_assertions(StructuredLogTest)
add_test(NAME StructuredLogTest COMMAND StructuredLogTest)
set_tests_properties(StructuredLogTest PROPERTIES TIMEOUT 30
                     LABELS "project;ops")

# Journal follower convergence test (HA replication primitive)
add_executable(JournalFollowerTest JournalFollowerTest.cpp)
target_link_libraries(JournalFollowerTest PRIVATE OrderMatcher)
enable_assertions(JournalFollowerTest)
add_test(NAME JournalFollowerTest COMMAND JournalFollowerTest)
set_tests_properties(JournalFollowerTest PROPERTIES TIMEOUT 60
                     LABELS "project;property")

# Snapshot consistency under concurrent writes
add_executable(SnapshotConsistencyTest SnapshotConsistencyTest.cpp)
target_link_libraries(SnapshotConsistencyTest PRIVATE OrderMatcher)
enable_assertions(SnapshotConsistencyTest)
add_test(NAME SnapshotConsistencyTest COMMAND SnapshotConsistencyTest)
set_tests_properties(SnapshotConsistencyTest PROPERTIES TIMEOUT 30
                     LABELS "project;property")

# Property tests (assert-based randomized invariant checks)
add_executable(PropertyTest PropertyTest.cpp)
target_link_libraries(PropertyTest PRIVATE OrderMatcher)
enable_assertions(PropertyTest)
add_test(NAME PropertyTest COMMAND PropertyTest)
set_tests_properties(PropertyTest PROPERTIES TIMEOUT 180 LABELS "project;property")

# Journal replay determinism property test
add_executable(JournalReplayPropertyTest JournalReplayPropertyTest.cpp)
target_link_libraries(JournalReplayPropertyTest PRIVATE OrderMatcher)
enable_assertions(JournalReplayPropertyTest)
add_test(NAME JournalReplayPropertyTest COMMAND JournalReplayPropertyTest)
set_tests_properties(JournalReplayPropertyTest PROPERTIES TIMEOUT 60
                     LABELS "project;property")

# Fault-injection scaffold tests
add_executable(FaultInjectorTest FaultInjectorTest.cpp)
target_link_libraries(FaultInjectorTest PRIVATE OrderMatcher)
enable_assertions(FaultInjectorTest)
add_test(NAME FaultInjectorTest COMMAND FaultInjectorTest)
set_tests_properties(FaultInjectorTest PROPERTIES TIMEOUT 60 LABELS "project;chaos")

# Pool exhaustion chaos test — exercises CapacityExhausted reject path.
add_executable(PoolExhaustionTest PoolExhaustionTest.cpp)
target_link_libraries(PoolExhaustionTest PRIVATE OrderMatcher)
enable_assertions(PoolExhaustionTest)
add_test(NAME PoolExhaustionTest COMMAND PoolExhaustionTest)
set_tests_properties(PoolExhaustionTest PROPERTIES TIMEOUT 60 LABELS "project;chaos")

# Pool capacity monitoring + controlled degradation (P3-8): utilization gauge,
# 80% warn, 95% shed (PoolCapacityExceeded), 100% kill-switch-like alert.
add_executable(PoolCapacityTest PoolCapacityTest.cpp)
target_link_libraries(PoolCapacityTest PRIVATE OrderMatcher)
enable_assertions(PoolCapacityTest)
add_test(NAME PoolCapacityTest COMMAND PoolCapacityTest)
set_tests_properties(PoolCapacityTest PROPERTIES TIMEOUT 30 LABELS "project;ops")

# Order lifecycle audit trail (include/AuditLog.h)
add_executable(AuditLogTest AuditLogTest.cpp)
target_link_libraries(AuditLogTest PRIVATE OrderMatcher)
enable_assertions(AuditLogTest)
add_test(NAME AuditLogTest COMMAND AuditLogTest)
set_tests_properties(AuditLogTest PROPERTIES TIMEOUT 30 LABELS "project;compliance")

# Journal chaos test — only meaningful when ENABLE_FAULT_INJECTION=ON, but
# always built so the harness compiles in both configurations.
add_executable(JournalChaosTest JournalChaosTest.cpp)
target_link_libraries(JournalChaosTest PRIVATE OrderMatcher)
enable_assertions(JournalChaosTest)
add_test(NAME JournalChaosTest COMMAND JournalChaosTest)
set_tests_properties(JournalChaosTest PROPERTIES TIMEOUT 60 LABELS "project;chaos")

# Journal corruption test — bit-flip and fsync-fail faults.
add_executable(JournalCorruptionTest JournalCorruptionTest.cpp)
target_link_libraries(JournalCorruptionTest PRIVATE OrderMatcher)
enable_assertions(JournalCorruptionTest)
add_test(NAME JournalCorruptionTest COMMAND JournalCorruptionTest)
set_tests_properties(JournalCorruptionTest PROPERTIES TIMEOUT 60 LABELS "project;chaos")

# Journal ack-durability test — onCommit (replication ack) must fire strictly
# after a successful durable sync, never before / when fsync is skipped.
add_executable(JournalAckDurabilityTest JournalAckDurabilityTest.cpp)
target_link_libraries(JournalAckDurabilityTest PRIVATE OrderMatcher)
enable_assertions(JournalAckDurabilityTest)
add_test(NAME JournalAckDurabilityTest COMMAND JournalAckDurabilityTest)
set_tests_properties(JournalAckDurabilityTest PROPERTIES TIMEOUT 60 LABELS "project;chaos")

# Journal crash-replay test — kill -9 mid-journal-write, restart, replay, and
# assert the recovered state equals the pre-crash committed state with contiguous
# sequences across the crash boundary. Portable (fdatasync/F_FULLFSYNC path);
# richer torn-write coverage when configured with -DENABLE_FAULT_INJECTION=ON.
add_executable(JournalCrashReplayTest JournalCrashReplayTest.cpp)
target_link_libraries(JournalCrashReplayTest PRIVATE OrderMatcher)
enable_assertions(JournalCrashReplayTest)
add_test(NAME JournalCrashReplayTest COMMAND JournalCrashReplayTest)
set_tests_properties(JournalCrashReplayTest PROPERTIES TIMEOUT 60 LABELS "project;recovery;chaos")

# Gateway chaos test — fragmented recv + spurious EAGAIN.
add_executable(GatewayChaosTest GatewayChaosTest.cpp)
target_link_libraries(GatewayChaosTest PRIVATE OrderMatcher)
enable_assertions(GatewayChaosTest)
add_test(NAME GatewayChaosTest COMMAND GatewayChaosTest)
set_tests_properties(GatewayChaosTest PROPERTIES TIMEOUT 90 LABELS "project;chaos")

# Queue chaos test — spurious push failures + spurious pop empties.
add_executable(QueueChaosTest QueueChaosTest.cpp)
target_link_libraries(QueueChaosTest PRIVATE OrderMatcher)
enable_assertions(QueueChaosTest)
add_test(NAME QueueChaosTest COMMAND QueueChaosTest)
set_tests_properties(QueueChaosTest PROPERTIES TIMEOUT 60 LABELS "project;chaos")

# MpscQueue concurrency regression (H5) — multi-producer enqueueSafe against a
# draining consumer. Its real value is under TSan: pre-fix, the plain readPos_
# raced with pop(). Labelled "concurrency" so the TSan lane can select it.
add_executable(MpscQueueRaceTest MpscQueueRaceTest.cpp)
target_link_libraries(MpscQueueRaceTest PRIVATE OrderMatcher)
enable_assertions(MpscQueueRaceTest)
add_test(NAME MpscQueueRaceTest COMMAND MpscQueueRaceTest)
set_tests_properties(MpscQueueRaceTest PROPERTIES TIMEOUT 120 LABELS "project;concurrency")

# Checkpoint chaos test — rewriteAtomically atomicity under faults.
add_executable(CheckpointChaosTest CheckpointChaosTest.cpp)
target_link_libraries(CheckpointChaosTest PRIVATE OrderMatcher)
enable_assertions(CheckpointChaosTest)
add_test(NAME CheckpointChaosTest COMMAND CheckpointChaosTest)
set_tests_properties(CheckpointChaosTest PROPERTIES TIMEOUT 60 LABELS "project;chaos")

# Combined chaos test — full pipeline (gateway+queue+journal) soak.
add_executable(CombinedChaosTest CombinedChaosTest.cpp)
target_link_libraries(CombinedChaosTest PRIVATE OrderMatcher)
enable_assertions(CombinedChaosTest)
add_test(NAME CombinedChaosTest COMMAND CombinedChaosTest)
set_tests_properties(CombinedChaosTest PROPERTIES TIMEOUT 120 LABELS "project;chaos")

# Gateway binary protocol schema evolution tests
add_executable(GatewayProtocolTest GatewayProtocolTest.cpp)
target_link_libraries(GatewayProtocolTest PRIVATE OrderMatcher)
enable_assertions(GatewayProtocolTest)
add_test(NAME GatewayProtocolTest COMMAND GatewayProtocolTest)
set_tests_properties(GatewayProtocolTest PROPERTIES TIMEOUT 30
                     LABELS "project;protocol")

# OUCH 4.2 binary order-entry codec + session-layer tests
add_executable(OuchSessionTest OuchSessionTest.cpp)
target_link_libraries(OuchSessionTest PRIVATE OrderMatcher)
enable_assertions(OuchSessionTest)
add_test(NAME OuchSessionTest COMMAND OuchSessionTest)
set_tests_properties(OuchSessionTest PROPERTIES TIMEOUT 30
                     LABELS "project;protocol")

# ITCH 5.0 market-data publisher + codec tests
add_executable(ItchPublisherTest ItchPublisherTest.cpp)
target_link_libraries(ItchPublisherTest PRIVATE OrderMatcher)
enable_assertions(ItchPublisherTest)
add_test(NAME ItchPublisherTest COMMAND ItchPublisherTest)
set_tests_properties(ItchPublisherTest PROPERTIES TIMEOUT 30
                     LABELS "project;protocol")

# Gap recovery: publish over MoldUDP64, drop datagrams, recover the holes from
# the packet journal, and assert the replayed book still equals the engine's.
add_executable(GapRecoveryReconstructionTest GapRecoveryReconstructionTest.cpp)
target_link_libraries(GapRecoveryReconstructionTest PRIVATE OrderMatcher)
enable_assertions(GapRecoveryReconstructionTest)
add_test(NAME GapRecoveryReconstructionTest COMMAND GapRecoveryReconstructionTest)
set_tests_properties(GapRecoveryReconstructionTest PROPERTIES TIMEOUT 60
                     LABELS "project;protocol;recovery")

# L2 incremental feed fidelity: apply only onMarketData updates and assert the
# reconstructed depth equals the engine's. Covers the blind spot the L3 test
# cannot reach, since that one compares against getSnapshot() directly.
add_executable(MarketDataL2ReconstructionTest MarketDataL2ReconstructionTest.cpp)
target_link_libraries(MarketDataL2ReconstructionTest PRIVATE OrderMatcher)
enable_assertions(MarketDataL2ReconstructionTest)
add_test(NAME MarketDataL2ReconstructionTest COMMAND MarketDataL2ReconstructionTest)
set_tests_properties(MarketDataL2ReconstructionTest PROPERTIES TIMEOUT 60
                     LABELS "project;protocol")

# ITCH 5.0 feed fidelity: replay the wire stream into an independent book and
# assert it equals the engine's displayed depth (hidden excluded, iceberg
# sliced). Pins the L3 feed against the L2 snapshot so they cannot drift.
add_executable(ItchReconstructionTest ItchReconstructionTest.cpp)
target_link_libraries(ItchReconstructionTest PRIVATE OrderMatcher)
enable_assertions(ItchReconstructionTest)
add_test(NAME ItchReconstructionTest COMMAND ItchReconstructionTest)
set_tests_properties(ItchReconstructionTest PROPERTIES TIMEOUT 60
                     LABELS "project;protocol")

# ITCH 5.0 exact wire byte-layout / field-offset tests (C / X / P)
add_executable(ItchByteLayoutTest ItchByteLayoutTest.cpp)
target_link_libraries(ItchByteLayoutTest PRIVATE OrderMatcher)
enable_assertions(ItchByteLayoutTest)
add_test(NAME ItchByteLayoutTest COMMAND ItchByteLayoutTest)
set_tests_properties(ItchByteLayoutTest PROPERTIES TIMEOUT 30
                     LABELS "project;protocol")

# SoupBinTCP transport: envelope codec, framer, session state machine
add_executable(SoupBinTcpTest SoupBinTcpTest.cpp)
target_link_libraries(SoupBinTcpTest PRIVATE OrderMatcher)
enable_assertions(SoupBinTcpTest)
add_test(NAME SoupBinTcpTest COMMAND SoupBinTcpTest)
set_tests_properties(SoupBinTcpTest PROPERTIES TIMEOUT 30
                     LABELS "project;protocol")

# MoldUDP64 multicast transport: header, batched publisher, gap-detecting subscriber
add_executable(MoldUDP64Test MoldUDP64Test.cpp)
target_link_libraries(MoldUDP64Test PRIVATE OrderMatcher)
enable_assertions(MoldUDP64Test)
add_test(NAME MoldUDP64Test COMMAND MoldUDP64Test)
set_tests_properties(MoldUDP64Test PROPERTIES TIMEOUT 30
                     LABELS "project;protocol")

# OUCH-over-SoupBinTCP end-to-end gateway over real loopback TCP
add_executable(OuchTcpGatewayTest OuchTcpGatewayTest.cpp)
target_link_libraries(OuchTcpGatewayTest PRIVATE OrderMatcher)
enable_assertions(OuchTcpGatewayTest)
add_test(NAME OuchTcpGatewayTest COMMAND OuchTcpGatewayTest)
set_tests_properties(OuchTcpGatewayTest PROPERTIES TIMEOUT 30
                     LABELS "project;protocol;integration")

# ITCH-over-MoldUDP64 transport over real loopback UDP
add_executable(ItchUdpTransportTest ItchUdpTransportTest.cpp)
target_link_libraries(ItchUdpTransportTest PRIVATE OrderMatcher)
enable_assertions(ItchUdpTransportTest)
add_test(NAME ItchUdpTransportTest COMMAND ItchUdpTransportTest)
set_tests_properties(ItchUdpTransportTest PROPERTIES TIMEOUT 30
                     LABELS "project;protocol;integration")

# ITCH gap-recovery: journal + retransmission service over loopback TCP
add_executable(ItchRetransmissionTest ItchRetransmissionTest.cpp)
target_link_libraries(ItchRetransmissionTest PRIVATE OrderMatcher)
enable_assertions(ItchRetransmissionTest)
add_test(NAME ItchRetransmissionTest COMMAND ItchRetransmissionTest)
set_tests_properties(ItchRetransmissionTest PROPERTIES TIMEOUT 30
                     LABELS "project;protocol;integration;recovery")

# Integrated MD publish stack: engine → ITCH → UDP + journal → retransmission
add_executable(ItchMarketDataFeedTest ItchMarketDataFeedTest.cpp)
target_link_libraries(ItchMarketDataFeedTest PRIVATE OrderMatcher)
enable_assertions(ItchMarketDataFeedTest)
add_test(NAME ItchMarketDataFeedTest COMMAND ItchMarketDataFeedTest)
set_tests_properties(ItchMarketDataFeedTest PROPERTIES TIMEOUT 30
                     LABELS "project;protocol;integration;recovery")

# P3-1: ITCH publication decoupled from the matching thread (async ring +
# dedicated publisher thread; matching thread only does a non-blocking push)
add_executable(ItchPublisherAsyncTest ItchPublisherAsyncTest.cpp)
target_link_libraries(ItchPublisherAsyncTest PRIVATE OrderMatcher)
enable_assertions(ItchPublisherAsyncTest)
add_test(NAME ItchPublisherAsyncTest COMMAND ItchPublisherAsyncTest)
set_tests_properties(ItchPublisherAsyncTest PROPERTIES TIMEOUT 60
                     LABELS "project;protocol;concurrency")

# P3-2: 10k-packet burst gap recovery — service keeps up, live UDP feed not
# stalled by TCP retransmit, journal ring holds the backlog without overflow
add_executable(MoldUDP64BurstRecoveryTest MoldUDP64BurstRecoveryTest.cpp)
target_link_libraries(MoldUDP64BurstRecoveryTest PRIVATE OrderMatcher)
enable_assertions(MoldUDP64BurstRecoveryTest)
add_test(NAME MoldUDP64BurstRecoveryTest COMMAND MoldUDP64BurstRecoveryTest)
set_tests_properties(MoldUDP64BurstRecoveryTest PROPERTIES TIMEOUT 90
                     LABELS "project;protocol;integration;recovery")

# P3-3: MoldUDP64 sequence-number continuity across a restart (persist the
# high-water on each group commit; resume from it, never rewind to 1)
add_executable(MoldSeqPersistenceTest MoldSeqPersistenceTest.cpp)
target_link_libraries(MoldSeqPersistenceTest PRIVATE OrderMatcher)
enable_assertions(MoldSeqPersistenceTest)
add_test(NAME MoldSeqPersistenceTest COMMAND MoldSeqPersistenceTest)
set_tests_properties(MoldSeqPersistenceTest PROPERTIES TIMEOUT 30
                     LABELS "project;protocol;durability;recovery")

# Option A UDP gap recovery: reorder buffer (hold-and-drain out-of-order
# datagrams, NAK only the new gap) + sender retransmit window. DPDK-free logic,
# runs headless on CI; the rte_eth_tx_burst NAK path is validated on hardware.
add_executable(UdpGapRecoveryTest UdpGapRecoveryTest.cpp)
target_link_libraries(UdpGapRecoveryTest PRIVATE OrderMatcher)
enable_assertions(UdpGapRecoveryTest)
add_test(NAME UdpGapRecoveryTest COMMAND UdpGapRecoveryTest)
set_tests_properties(UdpGapRecoveryTest PROPERTIES TIMEOUT 30
                     LABELS "project;protocol;recovery")

# STP occupancy counter (P1-2): O(1) self-trade pre-check replacing the O(depth)
# scan. Parity test — the counter must always equal a ground-truth resting scan
# across fill/cancel/STP/expiry/iceberg-reorder/pro-rata/uncross.
add_executable(StpOccupancyTest StpOccupancyTest.cpp)
target_link_libraries(StpOccupancyTest PRIVATE OrderMatcher)
enable_assertions(StpOccupancyTest)
add_test(NAME StpOccupancyTest COMMAND StpOccupancyTest)
set_tests_properties(StpOccupancyTest PROPERTIES TIMEOUT 30
                     LABELS "project;matching;stp")

# SBE — schema-driven binary protocol with forward-compat versioning
add_executable(SbeProtocolTest SbeProtocolTest.cpp)
target_link_libraries(SbeProtocolTest PRIVATE OrderMatcher)
enable_assertions(SbeProtocolTest)
add_test(NAME SbeProtocolTest COMMAND SbeProtocolTest)
set_tests_properties(SbeProtocolTest PROPERTIES TIMEOUT 30
                     LABELS "project;protocol")

# SBE session: schema-driven NewOrder → engine + OrderAck reply
add_executable(SbeSessionTest SbeSessionTest.cpp)
target_link_libraries(SbeSessionTest PRIVATE OrderMatcher)
enable_assertions(SbeSessionTest)
add_test(NAME SbeSessionTest COMMAND SbeSessionTest)
set_tests_properties(SbeSessionTest PROPERTIES TIMEOUT 30
                     LABELS "project;protocol;integration")

# GTD virtual clock deterministic replay tests
# Graceful shutdown with state persistence (P3-6): GTD/GTC persist + restore,
# DAY cancelled at session end, in-flight IOC/partial-fill drained.
add_executable(GracefulShutdownTest GracefulShutdownTest.cpp)
target_link_libraries(GracefulShutdownTest PRIVATE OrderMatcher)
enable_assertions(GracefulShutdownTest)
add_test(NAME GracefulShutdownTest COMMAND GracefulShutdownTest)
set_tests_properties(GracefulShutdownTest PROPERTIES TIMEOUT 60
                     LABELS "project;persistence")

add_executable(GTDReplayTest GTDReplayTest.cpp)
target_link_libraries(GTDReplayTest PRIVATE OrderMatcher)
enable_assertions(GTDReplayTest)
add_test(NAME GTDReplayTest COMMAND GTDReplayTest)
set_tests_properties(GTDReplayTest PROPERTIES TIMEOUT 60
                     LABELS "project;property;replay")

# Market data feed schema compatibility tests
add_executable(MdFeedSchemaCompatTest MdFeedSchemaCompatTest.cpp)
target_link_libraries(MdFeedSchemaCompatTest PRIVATE OrderMatcher)
enable_assertions(MdFeedSchemaCompatTest)
add_test(NAME MdFeedSchemaCompatTest COMMAND MdFeedSchemaCompatTest)
set_tests_properties(MdFeedSchemaCompatTest PROPERTIES TIMEOUT 30
                     LABELS "project;protocol;schema")

# Config system tests
add_executable(ConfigTest ConfigTest.cpp)
target_link_libraries(ConfigTest PRIVATE OrderMatcher)
enable_assertions(ConfigTest)
add_test(NAME ConfigTest COMMAND ConfigTest)
set_tests_properties(ConfigTest PROPERTIES TIMEOUT 15
                     LABELS "project;config")

# Startup config validation (P3-7): fail-fast on bad instrument/participant/
# journal config.
add_executable(ConfigValidationTest ConfigValidationTest.cpp)
target_link_libraries(ConfigValidationTest PRIVATE OrderMatcher)
enable_assertions(ConfigValidationTest)
add_test(NAME ConfigValidationTest COMMAND ConfigValidationTest)
set_tests_properties(ConfigValidationTest PROPERTIES TIMEOUT 15
                     LABELS "project;config")

# AlertDispatcher webhook tests
add_executable(AlertDispatcherTest AlertDispatcherTest.cpp)
target_link_libraries(AlertDispatcherTest PRIVATE OrderMatcher)
enable_assertions(AlertDispatcherTest)
add_test(NAME AlertDispatcherTest COMMAND AlertDispatcherTest)
set_tests_properties(AlertDispatcherTest PROPERTIES TIMEOUT 30
                     LABELS "project;ops")

# Replication protocol HA tests
add_executable(ReplicationProtocolTest ReplicationProtocolTest.cpp)
target_link_libraries(ReplicationProtocolTest PRIVATE OrderMatcher)
enable_assertions(ReplicationProtocolTest)
add_test(NAME ReplicationProtocolTest COMMAND ReplicationProtocolTest)
set_tests_properties(ReplicationProtocolTest PROPERTIES TIMEOUT 60
                     LABELS "project;ha;replication")

# Session scheduler: deterministic daily-lifecycle sequencing via virtual clock
add_executable(SessionSchedulerTest TestSessionScheduler.cpp)
target_link_libraries(SessionSchedulerTest PRIVATE OrderMatcher)
enable_assertions(SessionSchedulerTest)
add_test(NAME SessionSchedulerTest COMMAND SessionSchedulerTest)
set_tests_properties(SessionSchedulerTest PROPERTIES TIMEOUT 30
                     LABELS "project;session;lifecycle")

# Auction price discovery, NOII indicative/imbalance, volatility-auction lifecycle
add_executable(AuctionSessionTest TestAuctionSession.cpp)
target_link_libraries(AuctionSessionTest PRIVATE OrderMatcher)
enable_assertions(AuctionSessionTest)
add_test(NAME AuctionSessionTest COMMAND AuctionSessionTest)
set_tests_properties(AuctionSessionTest PROPERTIES TIMEOUT 30
                     LABELS "project;session;auction")

# Advanced order types: MIT, market sweep protection, OCO via the engine
# Critical correctness regression tests (from AUDIT_REPORT.md section 1)
add_executable(CriticalFixesTest CriticalFixesTest.cpp)
target_link_libraries(CriticalFixesTest PRIVATE OrderMatcher gtest_main)
gtest_discover_tests(CriticalFixesTest
    XML_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results
    PROPERTIES LABELS "project;correctness")

add_executable(AdvancedOrdersTest TestAdvancedOrders.cpp)
target_link_libraries(AdvancedOrdersTest PRIVATE OrderMatcher)
enable_assertions(AdvancedOrdersTest)
add_test(NAME AdvancedOrdersTest COMMAND AdvancedOrdersTest)
set_tests_properties(AdvancedOrdersTest PROPERTIES TIMEOUT 30
                     LABELS "project;orders;advanced")

# Maker-taker fee/rebate engine
add_executable(FeeEngineTest TestFeeEngine.cpp)
target_link_libraries(FeeEngineTest PRIVATE OrderMatcher)
enable_assertions(FeeEngineTest)
add_test(NAME FeeEngineTest COMMAND FeeEngineTest)
set_tests_properties(FeeEngineTest PROPERTIES TIMEOUT 30
                     LABELS "project;compliance;fees")

# CAT-format audit-trail reporter
add_executable(CatReporterTest TestCatReporter.cpp)
target_link_libraries(CatReporterTest PRIVATE OrderMatcher)
enable_assertions(CatReporterTest)
add_test(NAME CatReporterTest COMMAND CatReporterTest)
set_tests_properties(CatReporterTest PROPERTIES TIMEOUT 30
                     LABELS "project;compliance;audit")

# Multi-tier hierarchical risk limits + fat-finger
add_executable(HierarchicalRiskTest TestHierarchicalRisk.cpp)
target_link_libraries(HierarchicalRiskTest PRIVATE OrderMatcher)
enable_assertions(HierarchicalRiskTest)
add_test(NAME HierarchicalRiskTest COMMAND HierarchicalRiskTest)
set_tests_properties(HierarchicalRiskTest PROPERTIES TIMEOUT 30
                     LABELS "project;compliance;risk")

# Risk / fees / audit wired end-to-end through the engine
add_executable(ComplianceTest TestCompliance.cpp)
target_link_libraries(ComplianceTest PRIVATE OrderMatcher)
enable_assertions(ComplianceTest)
add_test(NAME ComplianceTest COMMAND ComplianceTest)
set_tests_properties(ComplianceTest PROPERTIES TIMEOUT 30
                     LABELS "project;compliance;integration")

# Durable lease-epoch persistence
add_executable(EpochStoreTest TestEpochStore.cpp)
target_link_libraries(EpochStoreTest PRIVATE OrderMatcher)
enable_assertions(EpochStoreTest)
add_test(NAME EpochStoreTest COMMAND EpochStoreTest)
set_tests_properties(EpochStoreTest PROPERTIES TIMEOUT 30
                     LABELS "project;ha;durability")

# FIX session-layer sequence / recovery / heartbeat state machine
add_executable(FixSessionStateTest TestFixSessionState.cpp)
target_link_libraries(FixSessionStateTest PRIVATE OrderMatcher)
enable_assertions(FixSessionStateTest)
add_test(NAME FixSessionStateTest COMMAND FixSessionStateTest)
set_tests_properties(FixSessionStateTest PROPERTIES TIMEOUT 30
                     LABELS "project;protocol;fix")

# Durable lease epoch survives restart (split-brain hardening)
add_executable(LeaseEpochTest TestLeaseEpoch.cpp)
target_link_libraries(LeaseEpochTest PRIVATE OrderMatcher)
enable_assertions(LeaseEpochTest)
add_test(NAME LeaseEpochTest COMMAND LeaseEpochTest)
set_tests_properties(LeaseEpochTest PROPERTIES TIMEOUT 30
                     LABELS "project;ha;replication")

# Wire-protocol versioning contract (FIX/SBE/ITCH/OUCH)
add_executable(ProtocolVersioningTest ProtocolVersioningTest.cpp)
target_link_libraries(ProtocolVersioningTest PRIVATE OrderMatcher)
enable_assertions(ProtocolVersioningTest)
add_test(NAME ProtocolVersioningTest COMMAND ProtocolVersioningTest)
set_tests_properties(ProtocolVersioningTest PROPERTIES TIMEOUT 30
                     LABELS "project;protocol;versioning")

# Shadow mode divergence detection test
add_executable(ShadowModeTest ShadowModeTest.cpp)
target_link_libraries(ShadowModeTest PRIVATE OrderMatcher gtest_main)
gtest_discover_tests(ShadowModeTest
    XML_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results
    PROPERTIES LABELS "project;shadow;integration")

# FIX inbound boundary-validation tests (seqnum, logon gate, required
# fields, BodyLength cross-check, TargetCompID, unknown enums)
add_executable(FixValidationTest FixValidationTest.cpp)
target_link_libraries(FixValidationTest PRIVATE OrderMatcher gtest_main)
gtest_discover_tests(FixValidationTest
    XML_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results
    PROPERTIES LABELS "project;fix;validation")

# ContingencyManager OCO linkage tests (assert-based, engine-agnostic)
add_executable(ContingencyManagerTest TestContingencyManager.cpp)
target_link_libraries(ContingencyManagerTest PRIVATE OrderMatcher)
enable_assertions(ContingencyManagerTest)
add_test(NAME ContingencyManagerTest COMMAND ContingencyManagerTest)
set_tests_properties(ContingencyManagerTest PROPERTIES TIMEOUT 30
                     LABELS "project;orders;oco")

# Market impact models: Almgren-Chriss and square-root law (header-only)
add_executable(ImpactModelTest TestImpactModel.cpp)
target_link_libraries(ImpactModelTest PRIVATE OrderMatcher gtest_main)
gtest_discover_tests(ImpactModelTest
    XML_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results
    PROPERTIES LABELS "project;quant;impact")

# Agent-based market simulation: NoiseTrader, MarketMaker, InformedTrader, SimulationDriver
add_executable(SimulationTest TestSimulation.cpp)
target_link_libraries(SimulationTest PRIVATE OrderMatcher gtest_main)
gtest_discover_tests(SimulationTest
    XML_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results
    PROPERTIES LABELS "project;simulation")

# Research replay harness: journal-driven replay with trade/book-update callbacks
add_executable(TestResearchHarness TestResearchHarness.cpp)
target_link_libraries(TestResearchHarness PRIVATE OrderMatcher gtest_main)
gtest_discover_tests(TestResearchHarness
    XML_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results
    PROPERTIES LABELS "project;research;unit")

# Microstructure metrics: effective spread, realized spread, Kyle's lambda, Amihud, VPIN
add_executable(TestMicrostructureMetrics TestMicrostructureMetrics.cpp)
target_link_libraries(TestMicrostructureMetrics PRIVATE OrderMatcher gtest_main)
gtest_discover_tests(TestMicrostructureMetrics
    XML_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results
    PROPERTIES LABELS "project;research;unit")

# Almgren-Chriss optimal execution trajectory planner (header-only)
add_executable(OptimalExecutionTest TestOptimalExecution.cpp)
target_link_libraries(OptimalExecutionTest PRIVATE OrderMatcher gtest_main)
gtest_discover_tests(OptimalExecutionTest
    XML_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results
    PROPERTIES LABELS "project;quant;execution")

# Execution quality analytics: IS decomposition, VWAP/TWAP benchmarks, spread capture
add_executable(ExecutionAnalyticsTest TestExecutionAnalytics.cpp)
target_link_libraries(ExecutionAnalyticsTest PRIVATE OrderMatcher gtest_main)
gtest_discover_tests(ExecutionAnalyticsTest
    XML_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results
    PROPERTIES LABELS "project;quant;execution")

add_executable(OrderFlowAnalyticsTest TestOrderFlowAnalytics.cpp)
target_link_libraries(OrderFlowAnalyticsTest PRIVATE OrderMatcher gtest_main)
gtest_discover_tests(OrderFlowAnalyticsTest
    XML_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results
    PROPERTIES LABELS "project;quant;microstructure")

add_executable(ResearchSerializerTest TestResearchSerializer.cpp)
target_link_libraries(ResearchSerializerTest PRIVATE OrderMatcher gtest_main)
gtest_discover_tests(ResearchSerializerTest
    XML_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results
    PROPERTIES LABELS "project;quant;serializer")

# VPIN (Volume-Synchronized Probability of Informed Trading) calculator
add_executable(VPINTest TestVPINCalculator.cpp)
target_link_libraries(VPINTest PRIVATE OrderMatcher gtest_main)
gtest_discover_tests(VPINTest
    XML_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results
    PROPERTIES LABELS "project;quant;microstructure")

add_executable(SpreadDecompositionTest TestSpreadDecomposition.cpp)
target_link_libraries(SpreadDecompositionTest PRIVATE OrderMatcher gtest_main)
gtest_discover_tests(SpreadDecompositionTest
    XML_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results
    PROPERTIES LABELS "project;quant;microstructure")

add_executable(CalibrationPipelineTest TestCalibrationPipeline.cpp)
target_link_libraries(CalibrationPipelineTest PRIVATE OrderMatcher gtest_main)
gtest_discover_tests(CalibrationPipelineTest
    XML_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results
    PROPERTIES LABELS "project;quant;backtest")

add_executable(BacktestEngineTest TestBacktestEngine.cpp)
target_link_libraries(BacktestEngineTest PRIVATE OrderMatcher gtest_main)
gtest_discover_tests(BacktestEngineTest
    XML_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results
    PROPERTIES LABELS "project;quant;backtest")

add_executable(SignalGeneratorTest TestSignalGenerator.cpp)
target_link_libraries(SignalGeneratorTest PRIVATE OrderMatcher gtest_main)
gtest_discover_tests(SignalGeneratorTest
    XML_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results
    PROPERTIES LABELS "project;quant;signals")

add_executable(ResearchDashboardTest TestResearchDashboard.cpp)
target_link_libraries(ResearchDashboardTest PRIVATE OrderMatcher gtest_main)
gtest_discover_tests(ResearchDashboardTest
    XML_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results
    PROPERTIES LABELS "project;quant;dashboard")

# Signal-driven paper trading loop (header-only PaperTrader)
add_executable(PaperTraderTest TestPaperTrader.cpp)
target_link_libraries(PaperTraderTest PRIVATE OrderMatcher gtest_main)
target_include_directories(PaperTraderTest PRIVATE include)
gtest_discover_tests(PaperTraderTest
    XML_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results
    PROPERTIES LABELS "project;quant;paper-trading")

# MOC/LOC on-close order types
add_executable(MocLocTest TestMocLocOrders.cpp)
target_link_libraries(MocLocTest PRIVATE OrderMatcher gtest_main)
gtest_discover_tests(MocLocTest
    XML_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results
    PROPERTIES LABELS "project;auction;moc;loc")

# LMM/DMM market-maker role allocation
add_executable(LmmDmmTest TestLmmDmmAllocation.cpp)
target_link_libraries(LmmDmmTest PRIVATE OrderMatcher gtest_main)
gtest_discover_tests(LmmDmmTest
    XML_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results
    PROPERTIES LABELS "project;allocation;dmm")

# Admin bearer token + /readyz endpoint tests
add_executable(AdminAuthTest AdminAuthTest.cpp)
target_link_libraries(AdminAuthTest PRIVATE OrderMatcher)
enable_assertions(AdminAuthTest)
add_test(NAME AdminAuthTest COMMAND AdminAuthTest)
set_tests_properties(AdminAuthTest PROPERTIES TIMEOUT 30
                     LABELS "project;ops;security")

# RateLimiter live reconfiguration test
add_executable(RateLimiterReconfigureTest RateLimiterReconfigureTest.cpp)
target_link_libraries(RateLimiterReconfigureTest PRIVATE OrderMatcher)
enable_assertions(RateLimiterReconfigureTest)
add_test(NAME RateLimiterReconfigureTest COMMAND RateLimiterReconfigureTest)
set_tests_properties(RateLimiterReconfigureTest PROPERTIES TIMEOUT 15
                     LABELS "project;ops;rate-limit")

# Reconnect backoff smoke test
add_executable(ReconnectBackoffTest ReconnectBackoffTest.cpp)
target_link_libraries(ReconnectBackoffTest PRIVATE OrderMatcher)
enable_assertions(ReconnectBackoffTest)
add_test(NAME ReconnectBackoffTest COMMAND ReconnectBackoffTest)
set_tests_properties(ReconnectBackoffTest PROPERTIES TIMEOUT 30
                     LABELS "project;ha;replication")
