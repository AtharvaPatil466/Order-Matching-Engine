include(FetchContent)

# Pinned by content, not just by tag. Without URL_HASH, every CI run built and
# executed whatever bytes that URL happened to serve — a tag can be moved, and
# GitHub's auto-generated archives are regenerated rather than immutable. The
# test binaries link this and run in CI with the repository checked out, so
# "whatever arrived" is a code-execution path.
#
# If the hash ever stops matching, the build fails loudly instead of building
# the new bytes. That is the point: a mismatch is something to investigate, not
# to paper over by deleting the line.
FetchContent_Declare(
  googletest
  URL      https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
  URL_HASH SHA256=1f357c27ca988c3f7c6b4bf68a9395005ac6761f034046e9dde0896e3aba00e4
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_MakeAvailable(googletest)

add_executable(UnitTests
    TestOrderBook.cpp
    RejectReasonContractTest.cpp)
target_link_libraries(UnitTests PRIVATE OrderMatcher gtest_main)

# DISCOVERY_TIMEOUT 60 on every gtest_discover_tests call below (CMake's
# default is 5s). Discovery runs the test binary with --gtest_list_tests AT
# BUILD TIME, and under a parallel build that competes with 8 compiler
# processes for CPU. When it times out the build FAILS — and it fails looking
# nothing like a timeout:
#
#     CMake Error ... GoogleTestAddTests.cmake:381 (message):
#       Error running test executable.
#       Result: Process terminated due to timeout
#     make[2]: *** [tests/CalibrationPipelineTest] Error 1
#
# no "error:" anywhere in the log, and the target deleted so the next build
# recompiles it. Observed on three different targets in one afternoon
# (CalibrationPipelineTest, SignalGeneratorTest, OptimalExecutionTest), which
# is what identifies it as load-dependent rather than one hanging binary: a
# bare re-run of the identical command succeeds.
#
# 60s is generous because the cost of being wrong is asymmetric. A slow
# discovery costs seconds; a spurious failure reddens a CI lane for reasons
# unrelated to the change under test, and a lane that goes red at random is
# a lane people stop reading.
include(GoogleTest)
gtest_discover_tests(UnitTests
    DISCOVERY_TIMEOUT 60
    XML_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results
    PROPERTIES LABELS "project\;unit")

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
    DISCOVERY_TIMEOUT 60
    XML_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results
    # RUN_SERIAL: latency thresholds are calibrated single-threaded/uncontended,
    # so these must not run concurrently with other ctest tests under -j — CPU
    # contention inflates p99 past the bar and false-fails (same principle as the
    # Debug+FI gate above: only measure/enforce in the calibrated configuration).
    PROPERTIES LABELS "project\;benchmark-regression" RUN_SERIAL TRUE)

# The p99 thresholds are calibrated for an optimized Release build on x86 bare
# metal. Debug is 3-5x slower and fault injection adds hot-path overhead, so the
# threshold is only ENFORCED in a Release build without fault injection. NDEBUG
# can't carry this signal: ENABLE_ASSERTIONS=ON compiles -UNDEBUG, leaving NDEBUG
# undefined even in Release. Elsewhere the workload still runs and prints, but the
# assertion is replaced by GTEST_SKIP.
if(CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT ENABLE_FAULT_INJECTION)
    target_compile_definitions(BenchmarkRegression PRIVATE OB_ENFORCE_LATENCY_THRESHOLD=1)
endif()

# Engine lifecycle surfaces: expiry timer, auctions, replay mode (H11).
add_executable(EngineLifecycleTest EngineLifecycleTest.cpp)
target_link_libraries(EngineLifecycleTest PRIVATE OrderMatcher)
enable_assertions(EngineLifecycleTest)
add_test(NAME EngineLifecycleTest COMMAND EngineLifecycleTest)
set_tests_properties(EngineLifecycleTest PROPERTIES TIMEOUT 60 LABELS "project;lifecycle")

# Replication apply + snapshot (H11). No GTest dependency.
add_executable(ReplicationApplyTest ReplicationApplyTest.cpp)
target_link_libraries(ReplicationApplyTest PRIVATE OrderMatcher)
enable_assertions(ReplicationApplyTest)
add_test(NAME ReplicationApplyTest COMMAND ReplicationApplyTest)
set_tests_properties(ReplicationApplyTest PROPERTIES TIMEOUT 60 LABELS "project;replication")

# Participant authentication (H7). No GTest dependency.
add_executable(ParticipantAuthTest ParticipantAuthTest.cpp)
target_link_libraries(ParticipantAuthTest PRIVATE OrderMatcher)
enable_assertions(ParticipantAuthTest)
add_test(NAME ParticipantAuthTest COMMAND ParticipantAuthTest)
set_tests_properties(ParticipantAuthTest PROPERTIES TIMEOUT 60 LABELS "project;security")

# Binary gateway login frame + per-participant authorisation (H7).
add_executable(GatewayLoginTest GatewayLoginTest.cpp)
target_link_libraries(GatewayLoginTest PRIVATE OrderMatcher)
enable_assertions(GatewayLoginTest)
add_test(NAME GatewayLoginTest COMMAND GatewayLoginTest)
set_tests_properties(GatewayLoginTest PROPERTIES TIMEOUT 120 LABELS "project;security;integration")

# Who may cancel whose order (H7). The login frame proves identity; this is
# the authorisation half it cannot reach.
add_executable(OrderOwnershipTest OrderOwnershipTest.cpp)
target_link_libraries(OrderOwnershipTest PRIVATE OrderMatcher)
enable_assertions(OrderOwnershipTest)
add_test(NAME OrderOwnershipTest COMMAND OrderOwnershipTest)
set_tests_properties(OrderOwnershipTest PROPERTIES TIMEOUT 120 LABELS "project;security;integration")

# A shutdown that outruns its budget must say which worker it is waiting on.
add_executable(ShutdownWatchdogTest ShutdownWatchdogTest.cpp)
target_link_libraries(ShutdownWatchdogTest PRIVATE OrderMatcher)
enable_assertions(ShutdownWatchdogTest)
add_test(NAME ShutdownWatchdogTest COMMAND ShutdownWatchdogTest)
set_tests_properties(ShutdownWatchdogTest PROPERTIES TIMEOUT 60 LABELS "project;lifecycle;concurrency")

# The order-entry port must refuse to serve unauthenticated (H7, GW-6), and must
# still start when it is configured or when the opt-out is explicit. Driven
# against the real binary because the decision lives in src/gateway_main.cpp.
#
# This REPLACES a pure-CMake `add_test(... COMMAND GatewayServer 0)` with
# WILL_FAIL TRUE. That test rotted: once gateway_main learned to validate its
# port argument, "0" was rejected with "PORT must be 1-65535" and the process
# exited 1 before reaching the credentials check at all. WILL_FAIL cannot tell a
# refusal from an argument error, so it stayed green while testing nothing.
#
# The script pins WHY each case exits, and runs the refusing cases with a
# deadline — a gateway that decides to serve never exits, so an exit-code
# assertion cannot observe that failure and a command substitution would hang.
add_test(NAME GatewayRequiresCredentialsTest
         COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/gateway_requires_credentials.sh
                 $<TARGET_FILE:GatewayServer>)
set_tests_properties(GatewayRequiresCredentialsTest PROPERTIES
    TIMEOUT 180
    LABELS "project;security")

# Nobody's orders but the clients'. The engine used to submit 200 Limit orders
# per symbol as participants 1 and 2 before announcing readiness, and served them
# on /book. Driven against the real binary and read through /book, because the
# defect was in src/main.cpp and the harm was what clients traded against. Waits
# on /readyz, which is the only signal that cannot fire before warmup drained.
add_test(NAME EngineNoWarmupOrdersTest
         COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/engine_no_warmup_orders.sh
                 $<TARGET_FILE:OrderEngine>)
set_tests_properties(EngineNoWarmupOrdersTest PROPERTIES
    TIMEOUT 180
    LABELS "project;lifecycle")

# A resting order must survive a restart. replayJournal() was covered by tests
# and called by nothing in src/main.cpp, so the WAL was write-only and every
# restart opened an empty book. Driven against the real binary because a direct
# replayJournal() call passes on the broken tree — the function was never the
# problem, main not calling it was. The order goes in through /chaos/order (a
# real submitOrder, so it journals) and the level is compared field by field:
# a replay that restores the wrong size is worse than one that restores nothing.
add_test(NAME EngineReplaysJournalTest
         COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/engine_replays_journal.sh
                 $<TARGET_FILE:OrderEngine>)
set_tests_properties(EngineReplaysJournalTest PROPERTIES
    TIMEOUT 180
    LABELS "project;recovery")

# The other half of wiring replay in: a journal written before the boot-time
# warmup was deleted holds 200 synthetic orders per symbol, so replaying one
# restores fabricated liquidity and reports a clean recovery. Keyed on
# JournalFileHeader::contentEpoch, deliberately not a formatVersion bump — the
# records frame perfectly, so this is content trust, not layout compatibility.
# Covers the refusal, the operator's override, and that a journal this build
# wrote still replays.
add_test(NAME EngineRefusesLegacyJournalTest
         COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/engine_refuses_legacy_journal.sh
                 $<TARGET_FILE:OrderEngine>)
set_tests_properties(EngineRefusesLegacyJournalTest PROPERTIES
    TIMEOUT 180
    LABELS "project;recovery;security")

# The engine must refuse to start on a journal it cannot read, rather than
# serving an empty book while the real orders sit unreadable on disk. Driven
# against the real binary because the decision lives in src/main.cpp; the
# script also checks the file was left byte-identical.
add_test(NAME EngineRefusesUnreadableJournalTest
         COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/refuses_unreadable_journal.sh
                 $<TARGET_FILE:OrderEngine>)
set_tests_properties(EngineRefusesUnreadableJournalTest PROPERTIES
    TIMEOUT 60
    LABELS "project;recovery")

# Durable client acknowledgements (C4). No GTest dependency.
add_executable(DurableAckTest DurableAckTest.cpp)
target_link_libraries(DurableAckTest PRIVATE OrderMatcher)
enable_assertions(DurableAckTest)
add_test(NAME DurableAckTest COMMAND DurableAckTest)
set_tests_properties(DurableAckTest PROPERTIES TIMEOUT 60 LABELS "project;durability")

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
    DISCOVERY_TIMEOUT 60
    XML_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results
    PROPERTIES LABELS "project\;risk")

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

# Differential fuzzer: the engine against RefMatcher.h, an independently
# written price-time FIFO matcher. Every other test here was written by
# whoever wrote the code under test; this one compares against a matcher
# derived from the DEFINITION of price-time priority, so it can disagree with
# the engine about something its author never considered.
#
# The registered run is a FIXED seed and a small operation count so CI is
# deterministic and quick. The soak is a flag, not something every push pays
# for: --seeds N --ops M (and --stats for measured generator bias).
# The corpus argument replays every recorded finding in tests/fuzz_corpus
# before fuzzing; see runCorpus() for the .known.flow convention.
add_executable(DifferentialFuzzTest DifferentialFuzzTest.cpp)
target_link_libraries(DifferentialFuzzTest PRIVATE OrderMatcher)
enable_assertions(DifferentialFuzzTest)
add_test(NAME DifferentialFuzzTest
         COMMAND DifferentialFuzzTest --corpus ${CMAKE_CURRENT_SOURCE_DIR}/fuzz_corpus)
# The timeout is PER ENVIRONMENT, because the environments differ by 50x and one
# number cannot serve both. Measured under ASan/UBSan:
#
#     GitHub x86 runner (the CI lane)            8.99s at dc807e1, 19.58s at 1bfa4fc
#     macOS, native                              13.8s
#     ubuntu:24.04 in Docker on Apple Silicon    470.93s, 479.27s   (native arm64)
#
# 180 is right for CI: a 9-20x margin that still fails a genuine hang in three
# minutes. The container is 25-50x slower for this test in particular —
# RefMatcher churns std::map and std::vector, and ASan's allocator in that VM is
# where the cost lands — so scripts/verify_linux.sh raises it for its own
# sanitizer lane with -DOB_DIFF_FUZZ_TIMEOUT. A single limit sized for the
# container would let CI pass a 45x slowdown or a near-hang for fifteen minutes.
#
# CORRECTION to 25a293b, which set this to 900 and is already pushed. Its message
# says this lane "has had no margin since the day it landed" and that my earlier
# claim that every lane was green at 1bfa4fc "was wrong". Both are false. CI's
# sanitizer lane passed at 1bfa4fc, in 19.58s against 180. Only the local
# container ever timed out; I read its number as though it were CI's.
set(OB_DIFF_FUZZ_TIMEOUT 180 CACHE STRING
    "ctest TIMEOUT for DifferentialFuzzTest; raise only for known-slow environments")
set_tests_properties(DifferentialFuzzTest PROPERTIES
    TIMEOUT ${OB_DIFF_FUZZ_TIMEOUT} LABELS "project;property")

# Stop-order fan-out bound (kMaxStopExecutionsPerSweep): one incoming order does
# bounded work, and every deferred election still fires.
add_executable(StopCascadeBoundTest StopCascadeBoundTest.cpp)
target_link_libraries(StopCascadeBoundTest PRIVATE OrderMatcher)
enable_assertions(StopCascadeBoundTest)
add_test(NAME StopCascadeBoundTest COMMAND StopCascadeBoundTest)
set_tests_properties(StopCascadeBoundTest PROPERTIES TIMEOUT 120 LABELS "project;property")

# Modify-to-zero is refused. Its own alarm(30) turns the pro-rata livelock into a
# signal, so the ctest timeout is only a backstop and never the thing that reports.
add_executable(ModifyToZeroTest ModifyToZeroTest.cpp)
target_link_libraries(ModifyToZeroTest PRIVATE OrderMatcher)
enable_assertions(ModifyToZeroTest)
add_test(NAME ModifyToZeroTest COMMAND ModifyToZeroTest)
set_tests_properties(ModifyToZeroTest PROPERTIES TIMEOUT 60 LABELS "project;property")

# The second route to the same livelock: an iceberg resting through the minQty
# path skipped the slice and entered the book at zero displayed quantity. Same
# shape as ModifyToZeroTest — both algorithms, invariant not return code, and an
# alarm(30) so the pro-rata spin fails rather than hangs.
add_executable(IcebergMinQtyTest IcebergMinQtyTest.cpp)
target_link_libraries(IcebergMinQtyTest PRIVATE OrderMatcher)
enable_assertions(IcebergMinQtyTest)
add_test(NAME IcebergMinQtyTest COMMAND IcebergMinQtyTest)
set_tests_properties(IcebergMinQtyTest PROPERTIES TIMEOUT 60 LABELS "project;property")

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

# H1 — cancels racing gracefulShutdown() must not touch freed pool memory.
# Only meaningful under ASan/TSan; in Release it is a smoke test.
add_executable(ShutdownCancelRaceTest ShutdownCancelRaceTest.cpp)
target_link_libraries(ShutdownCancelRaceTest PRIVATE OrderMatcher)
enable_assertions(ShutdownCancelRaceTest)
add_test(NAME ShutdownCancelRaceTest COMMAND ShutdownCancelRaceTest)
set_tests_properties(ShutdownCancelRaceTest PROPERTIES TIMEOUT 120
                     LABELS "project;lifecycle;concurrency")

# Kill-switch completeness (C5) — the safety control must be complete, not
# best-effort: after setKillSwitch returns, zero resting orders may survive.
# Exercises the async path with a small ring and maxPushRetries=1, the only
# regime where the dropped-control-message bug manifests.
add_executable(KillSwitchCompletenessTest KillSwitchCompletenessTest.cpp)
target_link_libraries(KillSwitchCompletenessTest PRIVATE OrderMatcher)
enable_assertions(KillSwitchCompletenessTest)
add_test(NAME KillSwitchCompletenessTest COMMAND KillSwitchCompletenessTest)
set_tests_properties(KillSwitchCompletenessTest PROPERTIES TIMEOUT 120 LABELS "project;risk;concurrency")

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

# recordInterval must COUNT a sub-tick op, not drop it. The old guard threw away
# the fastest samples, biasing every published percentile upward.
add_executable(LatencyTrackerIntervalTest LatencyTrackerIntervalTest.cpp)
target_link_libraries(LatencyTrackerIntervalTest PRIVATE OrderMatcher)
enable_assertions(LatencyTrackerIntervalTest)
add_test(NAME LatencyTrackerIntervalTest COMMAND LatencyTrackerIntervalTest)
set_tests_properties(LatencyTrackerIntervalTest PROPERTIES TIMEOUT 30
                     LABELS "project;unit")

# Sequence ids are now handed out in per-thread BLOCKS to remove a contended
# global fetch_add. Uniqueness across threads is the property that change could
# break, and SubmitResultTest only covers the single-threaded case.
add_executable(SequenceIdBlockTest SequenceIdBlockTest.cpp)
target_link_libraries(SequenceIdBlockTest PRIVATE OrderMatcher)
enable_assertions(SequenceIdBlockTest)
add_test(NAME SequenceIdBlockTest COMMAND SequenceIdBlockTest)
set_tests_properties(SequenceIdBlockTest PROPERTIES TIMEOUT 60
                     LABELS "project;unit")

# The single-publisher-thread invariant that lets MoldUDP64Publisher::nextSeq_
# stay a plain uint64_t: correct single-threaded and construct-here-publish-
# there use pass, a second thread advancing the sequence aborts (fork'd death
# test, assertions-on builds only).
add_executable(MoldPublishThreadTest MoldPublishThreadTest.cpp)
target_link_libraries(MoldPublishThreadTest PRIVATE OrderMatcher)
enable_assertions(MoldPublishThreadTest)
add_test(NAME MoldPublishThreadTest COMMAND MoldPublishThreadTest)
set_tests_properties(MoldPublishThreadTest PROPERTIES TIMEOUT 30
                     LABELS "project;protocol;concurrency")

# H18 — the capacity monitor is wired into a real engine, not just unit-tested
# in isolation: startAsync installs the resource callbacks and runs the
# monitor, a breach reaches the structured sink, stopAsync joins promptly.
add_executable(CapacityMonitorIntegrationTest CapacityMonitorIntegrationTest.cpp)
target_link_libraries(CapacityMonitorIntegrationTest PRIVATE OrderMatcher)
enable_assertions(CapacityMonitorIntegrationTest)
add_test(NAME CapacityMonitorIntegrationTest COMMAND CapacityMonitorIntegrationTest)
set_tests_properties(CapacityMonitorIntegrationTest PROPERTIES TIMEOUT 60
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
# C1 — what OrderStatus::CancelledBySTP is allowed to mean. Pins the
# classification (not a fill, not an OCO trigger, terminal, distinct from
# client-initiated Cancelled) independently of the matching paths that emit it.
add_executable(StpSemanticsTest StpSemanticsTest.cpp)
target_link_libraries(StpSemanticsTest PRIVATE OrderMatcher gtest_main)
gtest_discover_tests(StpSemanticsTest
    DISCOVERY_TIMEOUT 60
    XML_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results
    PROPERTIES LABELS "project\;matching\;stp")

add_executable(CriticalFixesTest CriticalFixesTest.cpp)
target_link_libraries(CriticalFixesTest PRIVATE OrderMatcher gtest_main)
gtest_discover_tests(CriticalFixesTest
    DISCOVERY_TIMEOUT 60
    XML_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results
    PROPERTIES LABELS "project\;correctness")

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
    DISCOVERY_TIMEOUT 60
    XML_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results
    PROPERTIES LABELS "project\;shadow\;integration")

# FIX inbound boundary-validation tests (seqnum, logon gate, required
# fields, BodyLength cross-check, TargetCompID, unknown enums)
add_executable(FixValidationTest FixValidationTest.cpp)
target_link_libraries(FixValidationTest PRIVATE OrderMatcher gtest_main)
gtest_discover_tests(FixValidationTest
    DISCOVERY_TIMEOUT 60
    XML_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results
    PROPERTIES LABELS "project\;fix\;validation")

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
    DISCOVERY_TIMEOUT 60
    XML_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results
    PROPERTIES LABELS "project\;quant\;impact")

# Agent-based market simulation: NoiseTrader, MarketMaker, InformedTrader, SimulationDriver
add_executable(SimulationTest TestSimulation.cpp)
target_link_libraries(SimulationTest PRIVATE OrderMatcher gtest_main)
gtest_discover_tests(SimulationTest
    DISCOVERY_TIMEOUT 60
    XML_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results
    PROPERTIES LABELS "project\;simulation")

# Research replay harness: journal-driven replay with trade/book-update callbacks
add_executable(TestResearchHarness TestResearchHarness.cpp)
target_link_libraries(TestResearchHarness PRIVATE OrderMatcher gtest_main)
gtest_discover_tests(TestResearchHarness
    DISCOVERY_TIMEOUT 60
    XML_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results
    PROPERTIES LABELS "project\;research\;unit")

# Microstructure metrics: effective spread, realized spread, Kyle's lambda, Amihud, VPIN
add_executable(TestMicrostructureMetrics TestMicrostructureMetrics.cpp)
target_link_libraries(TestMicrostructureMetrics PRIVATE OrderMatcher gtest_main)
gtest_discover_tests(TestMicrostructureMetrics
    DISCOVERY_TIMEOUT 60
    XML_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results
    PROPERTIES LABELS "project\;research\;unit")

# Almgren-Chriss optimal execution trajectory planner (header-only)
add_executable(OptimalExecutionTest TestOptimalExecution.cpp)
target_link_libraries(OptimalExecutionTest PRIVATE OrderMatcher gtest_main)
gtest_discover_tests(OptimalExecutionTest
    DISCOVERY_TIMEOUT 60
    XML_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results
    PROPERTIES LABELS "project\;quant\;execution")

# Execution quality analytics: IS decomposition, VWAP/TWAP benchmarks, spread capture
add_executable(ExecutionAnalyticsTest TestExecutionAnalytics.cpp)
target_link_libraries(ExecutionAnalyticsTest PRIVATE OrderMatcher gtest_main)
gtest_discover_tests(ExecutionAnalyticsTest
    DISCOVERY_TIMEOUT 60
    XML_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results
    PROPERTIES LABELS "project\;quant\;execution")

add_executable(OrderFlowAnalyticsTest TestOrderFlowAnalytics.cpp)
target_link_libraries(OrderFlowAnalyticsTest PRIVATE OrderMatcher gtest_main)
gtest_discover_tests(OrderFlowAnalyticsTest
    DISCOVERY_TIMEOUT 60
    XML_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results
    PROPERTIES LABELS "project\;quant\;microstructure")

add_executable(ResearchSerializerTest TestResearchSerializer.cpp)
target_link_libraries(ResearchSerializerTest PRIVATE OrderMatcher gtest_main)
gtest_discover_tests(ResearchSerializerTest
    DISCOVERY_TIMEOUT 60
    XML_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results
    PROPERTIES LABELS "project\;quant\;serializer")

# VPIN (Volume-Synchronized Probability of Informed Trading) calculator
add_executable(VPINTest TestVPINCalculator.cpp)
target_link_libraries(VPINTest PRIVATE OrderMatcher gtest_main)
gtest_discover_tests(VPINTest
    DISCOVERY_TIMEOUT 60
    XML_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results
    PROPERTIES LABELS "project\;quant\;microstructure")

add_executable(SpreadDecompositionTest TestSpreadDecomposition.cpp)
target_link_libraries(SpreadDecompositionTest PRIVATE OrderMatcher gtest_main)
gtest_discover_tests(SpreadDecompositionTest
    DISCOVERY_TIMEOUT 60
    XML_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results
    PROPERTIES LABELS "project\;quant\;microstructure")

add_executable(CalibrationPipelineTest TestCalibrationPipeline.cpp)
target_link_libraries(CalibrationPipelineTest PRIVATE OrderMatcher gtest_main)
gtest_discover_tests(CalibrationPipelineTest
    DISCOVERY_TIMEOUT 60
    XML_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results
    PROPERTIES LABELS "project\;quant\;backtest")

add_executable(BacktestEngineTest TestBacktestEngine.cpp)
target_link_libraries(BacktestEngineTest PRIVATE OrderMatcher gtest_main)
gtest_discover_tests(BacktestEngineTest
    DISCOVERY_TIMEOUT 60
    XML_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results
    PROPERTIES LABELS "project\;quant\;backtest")

add_executable(SignalGeneratorTest TestSignalGenerator.cpp)
target_link_libraries(SignalGeneratorTest PRIVATE OrderMatcher gtest_main)
gtest_discover_tests(SignalGeneratorTest
    DISCOVERY_TIMEOUT 60
    XML_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results
    PROPERTIES LABELS "project\;quant\;signals")

add_executable(ResearchDashboardTest TestResearchDashboard.cpp)
target_link_libraries(ResearchDashboardTest PRIVATE OrderMatcher gtest_main)
gtest_discover_tests(ResearchDashboardTest
    DISCOVERY_TIMEOUT 60
    XML_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results
    PROPERTIES LABELS "project\;quant\;dashboard")

# Signal-driven paper trading loop (header-only PaperTrader)
add_executable(PaperTraderTest TestPaperTrader.cpp)
target_link_libraries(PaperTraderTest PRIVATE OrderMatcher gtest_main)
target_include_directories(PaperTraderTest PRIVATE include)
gtest_discover_tests(PaperTraderTest
    DISCOVERY_TIMEOUT 60
    XML_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results
    PROPERTIES LABELS "project\;quant\;paper-trading")

# MOC/LOC on-close order types
add_executable(MocLocTest TestMocLocOrders.cpp)
target_link_libraries(MocLocTest PRIVATE OrderMatcher gtest_main)
gtest_discover_tests(MocLocTest
    DISCOVERY_TIMEOUT 60
    XML_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results
    PROPERTIES LABELS "project\;auction\;moc\;loc")

# LMM/DMM market-maker role allocation
add_executable(LmmDmmTest TestLmmDmmAllocation.cpp)
target_link_libraries(LmmDmmTest PRIVATE OrderMatcher gtest_main)
gtest_discover_tests(LmmDmmTest
    DISCOVERY_TIMEOUT 60
    XML_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results
    PROPERTIES LABELS "project\;allocation\;dmm")

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
