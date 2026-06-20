cmake_minimum_required(VERSION 3.15)
project(OrderMatchingEngine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(ENABLE_THREAD_SANITIZER "Enable ThreadSanitizer" OFF)
option(ENABLE_LTO "Enable link-time optimization in Release builds" OFF)
option(ENABLE_ASSERTIONS "Keep C assert checks enabled in all build types" ON)
option(ENABLE_FAULT_INJECTION "Compile fault-injection points (zero overhead when off)" OFF)
if(ENABLE_SANITIZERS AND ENABLE_THREAD_SANITIZER)
    message(FATAL_ERROR "ENABLE_SANITIZERS and ENABLE_THREAD_SANITIZER cannot be enabled together")
endif()
if(ENABLE_SANITIZERS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    else()
        message(WARNING "ENABLE_SANITIZERS is only configured for Clang/GNU")
    endif()
endif()
if(ENABLE_THREAD_SANITIZER)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
        add_link_options(-fsanitize=thread)
    else()
        message(WARNING "ENABLE_THREAD_SANITIZER is only configured for Clang/GNU")
    endif()
endif()

# Aggressive Optimizations for HFT
if(CMAKE_BUILD_TYPE MATCHES "Release")
    message(STATUS "Enabling HFT Optimizations: -O3 -march=native -mtune=native")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -march=native -mtune=native -Wall -Wextra -Werror -Wno-error=character-conversion")
    
    if(ENABLE_LTO)
        # Link Time Optimization (LTO)
        include(CheckIPOSupported)
        check_ipo_supported(RESULT result OUTPUT output)
        if(result)
            set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
            message(STATUS "LTO Enabled")
        else()
            message(WARNING "LTO not supported: ${output}")
        endif()
    endif()
endif()

# Lean Mode Option
option(USE_LEAN_MODE "Build with enterprise features disabled for raw speed" OFF)
if(USE_LEAN_MODE)
    message(STATUS "OB_LEAN_MODE Enabled: Compiling without risk/stats features")
    add_compile_definitions(OB_LEAN_MODE)
endif()

# Capture build metadata for the /version admin endpoint.
# Best-effort — in Docker builds the .git directory is excluded by
# .dockerignore so we fall back to "unknown". A baked-in version
# string still beats no version info at all when someone is trying
# to figure out which binary is running in production.
find_program(GIT_EXECUTABLE git)
if(GIT_EXECUTABLE)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short=12 HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE OB_GIT_SHA_VAR
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE GIT_RESULT
    )
    if(NOT GIT_RESULT EQUAL 0 OR "${OB_GIT_SHA_VAR}" STREQUAL "")
        set(OB_GIT_SHA_VAR "unknown")
    endif()
else()
    set(OB_GIT_SHA_VAR "unknown")
endif()
string(TIMESTAMP OB_BUILD_TIME_VAR "%Y-%m-%dT%H:%M:%SZ" UTC)
add_compile_definitions(
    OB_GIT_SHA="${OB_GIT_SHA_VAR}"
    OB_BUILD_TIME="${OB_BUILD_TIME_VAR}"
)
message(STATUS "Build metadata: git=${OB_GIT_SHA_VAR}, time=${OB_BUILD_TIME_VAR}")

# Threading support
find_package(Threads REQUIRED)

# Include directories
include_directories(include)

# Main Engine Library
add_library(OrderMatcher
    src/AdminServer.cpp
    src/OrderBook.cpp
    src/MatchingEngine.cpp
    src/SessionScheduler.cpp
    src/ContingencyManager.cpp
    src/HierarchicalRiskManager.cpp
    src/EpochStore.cpp
    src/TcpGateway.cpp
    src/FaultInjector.cpp
    src/SimulationDriver.cpp
)
target_link_libraries(OrderMatcher PUBLIC Threads::Threads)
if(ENABLE_ASSERTIONS)
    target_compile_options(OrderMatcher PUBLIC -UNDEBUG)
endif()
if(ENABLE_FAULT_INJECTION)
    # PUBLIC so the macro reaches every consumer (tests, gateway, etc.) and
    # the call sites in headers compile to active fault points.
    target_compile_definitions(OrderMatcher PUBLIC OB_ENABLE_FAULT_INJECTION=1)
    message(STATUS "Fault-injection points enabled (OB_ENABLE_FAULT_INJECTION)")
endif()

# Main Executable
add_executable(OrderEngine src/main.cpp)
target_link_libraries(OrderEngine PRIVATE OrderMatcher)

# Gateway Server
add_executable(GatewayServer src/gateway_main.cpp)
target_link_libraries(GatewayServer PRIVATE OrderMatcher)

# Market Data Subscriber
add_executable(MdSubscriber src/md_subscriber.cpp)
target_link_libraries(MdSubscriber PRIVATE OrderMatcher)

# Testing
option(BUILD_TESTS "Build tests" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Benchmarking
option(BUILD_BENCHMARKS "Build benchmarks" ON)
if(BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

# Fuzzing
option(BUILD_FUZZERS "Build fuzz harnesses (ASan/UBSan replay always; libFuzzer if supported)" OFF)
if(BUILD_FUZZERS)
    add_subdirectory(fuzz)
endif()

# Tools (Replay CLI, etc.)
option(BUILD_TOOLS "Build operational tools" ON)
if(BUILD_TOOLS)
    add_subdirectory(tools)
endif()

