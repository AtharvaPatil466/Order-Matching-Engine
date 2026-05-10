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
