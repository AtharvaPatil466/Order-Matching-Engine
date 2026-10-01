// GatewayMarketDataThreadTest — GatewayServer's market-data ring has one writer,
// and nothing reads the book off the thread that changes it.
//
// WHAT WAS WRONG (LF-2). Orders run on TcpGateway's event thread, and every
// book change publishes an update into the shared-memory ring from there. Once
// a second, main()'s loop called book->getSnapshot() and publishSnapshot() from
// the MAIN thread: it read the book while the event thread was changing it, and
// both threads claimed the ring's next slot (load writeSeq, write the slot,
// store writeSeq + 1), so one entry of a colliding pair was overwritten unseen.
//
// HOW IT IS OBSERVED. Black-box: rest and cancel orders through the order port
// for long enough that several once-a-second snapshots land among the updates,
// then stop the server and require a clean exit. Under ThreadSanitizer a race
// makes the process exit 66 instead of 0, so this test is a check in the TSan
// lane; elsewhere it only shows that the server takes orders while publishing.
//
// Usage: GatewayMarketDataThreadTest <path-to-GatewayServer>

#include "GatewayProcess.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <string>

using namespace OrderMatcher;
using namespace gwtest;

namespace {

// Three snapshot ticks (one a second), with margin for a slow sanitizer build.
constexpr auto TRAFFIC_FOR = std::chrono::milliseconds(3500);

OrderRequest order(OrderRequest::Type type, OrderId id) {
    OrderRequest r{};
    r.type          = type;
    r.symbolId      = 0;
    r.orderId       = id;
    r.participantId = 7;
    r.side          = (id & 1) ? Side::Sell : Side::Buy;
    r.price         = (id & 1) ? 10100 + static_cast<Price>(id % 50)
                               : 9900 - static_cast<Price>(id % 50);
    r.qty           = 10;
    r.orderType     = OrderType::Limit;
    r.tif           = TimeInForce::GTC;
    return r;
}

bool acked(Client& c, const OrderRequest& r) {
    GatewayResponse resp{};
    if (c.send(r) && c.answerFor(r.orderId, resp) && resp.type == GatewayResponse::Type::Ack)
        return true;
    std::printf("  order %llu (request type %d): answer type %d, reject reason %d\n",
                static_cast<unsigned long long>(r.orderId), static_cast<int>(r.type),
                static_cast<int>(resp.type), static_cast<int>(resp.rejectReason));
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    assert(argc == 2 && "usage: GatewayMarketDataThreadTest <path-to-GatewayServer>");
    uint16_t port = 48810;
    const std::string journal = tempJournalPath("ob_gateway_md_thread");
    std::remove(journal.c_str());

    const pid_t pid = startOnFreePort(argv[1], port, journal);
    assert(pid > 0 && "GatewayServer never started listening");

    Client c;
    assert(c.connect(port));
    uint64_t orders = 0;
    bool allAcked = true;
    const auto until = std::chrono::steady_clock::now() + TRAFFIC_FOR;
    for (OrderId id = 1; allAcked && std::chrono::steady_clock::now() < until; ++id) {
        allAcked = acked(c, order(OrderRequest::Type::NewOrder, id)) &&
                   acked(c, order(OrderRequest::Type::Cancel, id));
        ++orders;
    }

    // Stop before asserting, so a failure does not leak the process.
    const bool stopped = stopCleanly(pid);
    std::remove(journal.c_str());

    std::printf("  %llu orders rested and cancelled across the snapshot ticks\n",
                static_cast<unsigned long long>(orders));
    assert(allAcked && "the gateway refused or lost an order this test depends on");
    assert(stopped && "GatewayServer did not exit cleanly (exit 66 = ThreadSanitizer report)");

    std::puts("GatewayMarketDataThreadTest passed");
    return 0;
}
