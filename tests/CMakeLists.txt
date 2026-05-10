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
    PROPERTIES LABELS "project;benchmark-regression")

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

# GTD virtual clock deterministic replay tests
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

