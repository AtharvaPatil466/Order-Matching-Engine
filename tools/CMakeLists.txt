# Replay CLI (Phase 3, Week 9)
add_executable(JournalReplayCLI JournalReplayCLI.cpp)
target_link_libraries(JournalReplayCLI PRIVATE OrderMatcher)

# Latency Correlator (Phase 5, Week 18)
add_executable(LatencyCorrelator LatencyCorrelator.cpp)
target_link_libraries(LatencyCorrelator PRIVATE OrderMatcher)

# Mutation Fuzzer (Phase 3, Week 11)
add_executable(JournalMutationFuzzer JournalMutationFuzzer.cpp)
target_link_libraries(JournalMutationFuzzer PRIVATE OrderMatcher)
