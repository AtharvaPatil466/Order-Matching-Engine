#!/bin/bash

# Fallback build script for Order Matching Engine
# Use this if CMake is not available.

set -e

# Detect Compiler
if command -v clang++ >/dev/null 2>&1; then
    CXX="clang++"
elif command -v g++ >/dev/null 2>&1; then
    CXX="g++"
else
    echo "Error: No C++ compiler found (clang++ or g++)."
    exit 1
fi

echo "Using compiler: $CXX"

# Optimization flags (similar to CMake Release)
FLAGS="-O3 -march=native -std=c++20 -Iinclude -Wall -Wextra -pthread"

for arg in "$@"; do
    case "$arg" in
        --lean)
            echo "Building in LEAN mode (-DOB_LEAN_MODE)"
            FLAGS="$FLAGS -DOB_LEAN_MODE"
            ;;
        --sanitizers)
            echo "Building with AddressSanitizer and UndefinedBehaviorSanitizer"
            FLAGS="$FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer"
            ;;
        *)
            echo "Unknown option: $arg"
            echo "Usage: $0 [--lean] [--sanitizers]"
            exit 1
            ;;
    esac
done

# Create output directory
mkdir -p bin

echo "Compiling OrderMatcher library..."
$CXX $FLAGS -c src/OrderBook.cpp -o bin/OrderBook.o
$CXX $FLAGS -c src/MatchingEngine.cpp -o bin/MatchingEngine.o
$CXX $FLAGS -c src/TcpGateway.cpp -o bin/TcpGateway.o
$CXX $FLAGS -c src/AdminServer.cpp -o bin/AdminServer.o
$CXX $FLAGS -c src/FaultInjector.cpp -o bin/FaultInjector.o

# All downstream binaries link the FaultInjector singleton (the engine,
# OrderBook, and Journal all reference instance()). Bundling the .o into
# every link keeps this script's invocations parallel to the CMake target.
LIB_OBJS="bin/OrderBook.o bin/MatchingEngine.o bin/FaultInjector.o"

echo "Compiling main executable..."
$CXX $FLAGS src/main.cpp $LIB_OBJS bin/AdminServer.o -o bin/OrderEngine

echo "Compiling gateway server..."
$CXX $FLAGS src/gateway_main.cpp $LIB_OBJS bin/TcpGateway.o -o bin/GatewayServer

echo "Compiling market data subscriber..."
$CXX $FLAGS src/md_subscriber.cpp $LIB_OBJS -o bin/MdSubscriber

echo "Compiling manual tests..."
$CXX $FLAGS tests/ManualTest.cpp $LIB_OBJS -o bin/ManualTest

echo "Compiling stress tests..."
$CXX $FLAGS tests/StressTest.cpp $LIB_OBJS -o bin/StressTest

echo "Compiling journal crash tests..."
$CXX $FLAGS tests/JournalCrashTest.cpp $LIB_OBJS -o bin/JournalCrashTest

echo "Compiling gateway integration tests..."
$CXX $FLAGS tests/GatewayIntegrationTest.cpp $LIB_OBJS bin/TcpGateway.o -o bin/GatewayIntegrationTest

echo "Compiling property tests..."
$CXX $FLAGS tests/PropertyTest.cpp $LIB_OBJS -o bin/PropertyTest

echo "Compiling manual benchmarks..."
$CXX $FLAGS benchmarks/ManualBenchmark.cpp $LIB_OBJS -o bin/ManualBenchmark

echo "Compiling end-to-end benchmarks..."
$CXX $FLAGS benchmarks/E2EBenchmark.cpp $LIB_OBJS bin/TcpGateway.o bin/AdminServer.o -o bin/E2EBenchmark

echo "Compiling gateway protocol tests..."
$CXX $FLAGS tests/GatewayProtocolTest.cpp $LIB_OBJS -o bin/GatewayProtocolTest

echo "Compiling GTD replay tests..."
$CXX $FLAGS tests/GTDReplayTest.cpp $LIB_OBJS -o bin/GTDReplayTest

echo "Compiling MD feed schema compat tests..."
$CXX $FLAGS tests/MdFeedSchemaCompatTest.cpp $LIB_OBJS -o bin/MdFeedSchemaCompatTest

echo "Compiling config tests..."
$CXX $FLAGS tests/ConfigTest.cpp $LIB_OBJS -o bin/ConfigTest

echo "Compiling alert dispatcher tests..."
$CXX $FLAGS tests/AlertDispatcherTest.cpp $LIB_OBJS -o bin/AlertDispatcherTest

echo "Compiling replication protocol tests..."
$CXX $FLAGS tests/ReplicationProtocolTest.cpp $LIB_OBJS -o bin/ReplicationProtocolTest

echo "Build successful!"
echo "To run Engine:           ./bin/OrderEngine"
echo "To run Gateway:          ./bin/GatewayServer [port]"
echo "To run MD Subscriber:    ./bin/MdSubscriber [shm_name]"
echo "To run Tests:            ./bin/ManualTest"
echo "To run Stress Tests:     ./bin/StressTest"
echo "To run Property Tests:   ./bin/PropertyTest"
echo "To run Crash Tests:      ./bin/JournalCrashTest"
echo "To run Bench:            ./bin/ManualBenchmark"
echo "To run E2E Bench:        ./bin/E2EBenchmark"
echo "To run Protocol Tests:   ./bin/GatewayProtocolTest"
echo "To run GTD Tests:        ./bin/GTDReplayTest"
echo "To run MD Compat:        ./bin/MdFeedSchemaCompatTest"
echo "To run Config Tests:     ./bin/ConfigTest"
echo "To run Alert Tests:      ./bin/AlertDispatcherTest"
echo "To run Replication Tests: ./bin/ReplicationProtocolTest"
