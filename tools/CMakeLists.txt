# Replay CLI (Phase 3, Week 9)
add_executable(JournalReplayCLI JournalReplayCLI.cpp)
target_link_libraries(JournalReplayCLI PRIVATE OrderMatcher)

# Latency Correlator (Phase 5, Week 18)
add_executable(LatencyCorrelator LatencyCorrelator.cpp)
target_link_libraries(LatencyCorrelator PRIVATE OrderMatcher)

# Mutation Fuzzer (Phase 3, Week 11)
add_executable(JournalMutationFuzzer JournalMutationFuzzer.cpp)
target_link_libraries(JournalMutationFuzzer PRIVATE OrderMatcher)

# Time Machine — regulatory audit / postmortem replay tool
add_executable(TimeMachine time_machine.cpp)
target_link_libraries(TimeMachine PRIVATE OrderMatcher)
set_target_properties(TimeMachine PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# NASDAQ TotalView-ITCH 5.0 replay + book-reconstruction validator.
# With a file argument it replays a real ftp.nasdaqtrader.com sample; with no
# arguments it runs a self-contained SYNTHETIC self-test (registered below as a
# ctest) that exercises the replay+compare path end-to-end. Real-data
# validation still requires feeding a decompressed .NASDAQ_ITCH50 sample.
add_executable(NasdaqItchReplay NasdaqItchReplay.cpp)
target_link_libraries(NasdaqItchReplay PRIVATE OrderMatcher)
set_target_properties(NasdaqItchReplay PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(BUILD_TESTS)
    add_test(NAME NasdaqItchReplaySynthetic COMMAND NasdaqItchReplay)
    set_tests_properties(NasdaqItchReplaySynthetic PROPERTIES TIMEOUT 60
                         LABELS "project;tools;replay")
endif()

# ─── Wire-to-wire latency harness (OUCH / SO_TIMESTAMPING) ───────────────────
# Two-host OUCH order-entry wire-latency measurement using NIC hardware
# timestamps (Linux SO_TIMESTAMPING) with a CLOCK_MONOTONIC software fallback.
# OFF by default: needs two machines / a NIC to be useful and must not affect
# normal builds.
option(BUILD_WIRE_LATENCY_TOOLS "Build the OUCH wire-latency measurement tools" OFF)
if(BUILD_WIRE_LATENCY_TOOLS)
    add_executable(WireLatencySender WireLatencySender.cpp)
    target_link_libraries(WireLatencySender PRIVATE OrderMatcher)

    add_executable(WireLatencyReceiver WireLatencyReceiver.cpp)
    target_link_libraries(WireLatencyReceiver PRIVATE OrderMatcher)
endif()
