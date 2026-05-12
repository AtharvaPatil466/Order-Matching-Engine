include(FetchContent)

FetchContent_Declare(
  googlebenchmark
  URL https://github.com/google/benchmark/archive/refs/tags/v1.8.3.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googlebenchmark)

add_executable(MicroBenchmark MicroBenchmark.cpp)
target_link_libraries(MicroBenchmark PRIVATE OrderMatcher benchmark::benchmark benchmark::benchmark_main)

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

