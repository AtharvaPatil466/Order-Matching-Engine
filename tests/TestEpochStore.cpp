// TestEpochStore — durable, monotonic epoch persistence across "restart".

#include "EpochStore.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

using namespace OrderMatcher;

static int tests_passed = 0;
#define SECTION(name) std::cout << "\n  " << name << "..." << std::flush
#define PASS()        do { ++tests_passed; std::cout << " PASS" << std::endl; } while (0)

static std::string tmpPath() {
    return "/tmp/epochstore_test_" + std::to_string(::getpid()) + ".bin";
}

void test_round_trip_and_restart() {
    SECTION("store/load round-trips and survives a simulated restart");
    const std::string p = tmpPath();
    std::remove(p.c_str());
    {
        EpochStore s(p);
        assert(s.load() == 0);          // missing file -> fresh node
        s.store(42);
        assert(s.load() == 42);
    }
    {
        EpochStore restarted(p);        // a new instance == a process restart
        assert(restarted.load() == 42); // value survived
        restarted.store(100);           // monotone bump
        assert(restarted.load() == 100);
    }
    std::remove(p.c_str());
    PASS();
}

void test_corrupt_or_short_file_reads_zero() {
    SECTION("a truncated / garbage file loads as epoch 0");
    const std::string p = tmpPath();
    {
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        f << "xyz";                     // 3 bytes — not a complete 8-byte record
    }
    EpochStore s(p);
    assert(s.load() == 0);
    std::remove(p.c_str());
    PASS();
}

int main() {
    std::cout << "Running EpochStore tests..." << std::endl;
    test_round_trip_and_restart();
    test_corrupt_or_short_file_reads_zero();
    std::cout << "\nAll " << tests_passed << " EpochStore test sections passed." << std::endl;
    return 0;
}
