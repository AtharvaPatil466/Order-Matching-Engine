// TestLeaseEpoch — the lease fencing epoch survives a restart via EpochStore,
// closing the split-brain window where a restarted node reset its epoch to 0.

#include "ReplicationProtocol.h"

#include <cassert>
#include <cstdio>
#include <iostream>
#include <string>
#include <unistd.h>

using namespace OrderMatcher;

static int tests_passed = 0;
#define SECTION(name) std::cout << "\n  " << name << "..." << std::flush
#define PASS()        do { ++tests_passed; std::cout << " PASS" << std::endl; } while (0)

void test_epoch_survives_restart() {
    SECTION("lease epoch persists across a simulated restart");
    const std::string p = "/tmp/lease_epoch_" + std::to_string(::getpid()) + ".bin";
    std::remove(p.c_str());
    {
        LeaderLease lease(/*nodeId=*/1, /*leaseMs=*/5000, /*epochPath=*/p);
        assert(lease.epoch() == 0);          // fresh node
        assert(lease.tryAcquire());
        assert(lease.epoch() == 1);          // bumped and persisted
    }
    {
        LeaderLease restarted(1, 5000, p);   // a new instance == a restart
        assert(restarted.epoch() == 1);      // reloaded — NOT reset to 0
        assert(restarted.tryAcquire());
        assert(restarted.epoch() == 2);      // monotone continuation, no regression
    }
    std::remove(p.c_str());
    PASS();
}

void test_in_memory_default() {
    SECTION("no epoch path keeps the legacy in-memory behavior");
    LeaderLease a(1, 5000);   // no path -> in-memory only
    assert(a.tryAcquire());
    assert(a.epoch() == 1);
    LeaderLease b(1, 5000);   // independent in-memory lease
    assert(b.epoch() == 0);   // starts fresh at 0
    PASS();
}

int main() {
    std::cout << "Running lease-epoch durability tests..." << std::endl;
    test_epoch_survives_restart();
    test_in_memory_default();
    std::cout << "\nAll " << tests_passed << " lease-epoch test sections passed." << std::endl;
    return 0;
}
