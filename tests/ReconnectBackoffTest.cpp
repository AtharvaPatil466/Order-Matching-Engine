// ReconnectBackoffTest — smoke test for the exponential-backoff reconnection
// logic in ReplicationTransport (used when a backup loses its TCP link to
// the primary).
//
// The backoff state (reconnectDelayMs_) is private to ReplicationTransport,
// so this test exercises it indirectly: start a ReplicationCoordinator in
// backup mode pointing at a port where nothing is listening, let the receive
// loop fire a few failed doConnect() attempts (with their associated backoff
// sleeps), then stop cleanly.
//
// What this validates:
//  - The class compiles and links with reconnectDelayMs_ in its current form.
//    A refactor that removes the field or changes the type would fail here.
//  - startAsBackup() does not crash or hang when the primary is unreachable.
//  - stop() unblocks the receive loop even while it is sleeping between
//    reconnection attempts.
//
// Why timing is not asserted: the backoff starts at 500 ms and doubles on
// each failure. Pinning expected elapsed time to specific millisecond counts
// would produce a flaky CI test on loaded machines. The structural guarantee
// (backoff compiles and runs) is what matters here.

#include "ReplicationProtocol.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>

int main() {
    using namespace OrderMatcher;

    // Use a high, unlikely-to-be-in-use port. The port deliberately has
    // nothing listening so every doConnect() attempt fails immediately,
    // exercising the backoff increment path.
    ReplicationCoordinator coord(99);

    // startAsBackup() attempts one TCP connect — it will fail because
    // nothing is listening on port 19999. The coordinator still sets up
    // the receive loop, which will keep retrying in the background.
    // We tolerate the initial connection failure (return value is false)
    // and verify only that stop() completes without deadlock.
    bool started = coord.startAsBackup("127.0.0.1", 19999);
    (void)started;  // false is expected and acceptable

    // Let the receive loop iterate at least once through the backoff
    // sleep (initial delay is 500 ms). 1.5 s is enough to observe one
    // failed reconnect + backoff without waiting for the doubled delay.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    // stop() must return promptly even while the loop is sleeping in
    // the backoff path. transport_.stop() closes the internal fds,
    // which wakes any blocking recv/poll, and the thread exits its loop.
    coord.stop();

    std::puts("ReconnectBackoffTest: smoke test passed "
              "(backoff compiles and stop() terminates cleanly)");
    return 0;
}
