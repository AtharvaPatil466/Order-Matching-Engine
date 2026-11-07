// GatewayJournalRestartTest — an order the order-entry port acknowledged must
// still be resting after that process restarts.
//
// WHAT WAS WRONG. The binary that took client orders over the network had no
// journal, so any restart lost the book, and the binary that did journal had no
// order port. No sync policy could fix "acknowledged, then restarted" while the
// journal and the port lived in different processes.
//
// HOW IT IS OBSERVED. Through the order port and /book. Run 1 acknowledges a
// resting order. Run 2, on the same journal, is asked to CANCEL it: /book losing
// an order means it was there to cancel. A second cancel must lose nothing, which
// proves the probe distinguishes the two.
//
// TWO PROPERTIES, one flow. A CLEAN restart (SIGTERM) must keep the order, which
// needs a journal at all. And an ACKNOWLEDGED order must survive kill -9 sent the
// instant the ack arrives, which needs more: the ack must not leave the process
// until the order is on disk. Under group commit the entry sits in a user-space
// batch when the ack goes out, so kill -9 loses an order the client was told it
// has. The second scenario is the "acked, then kill -9'd" question, answered by
// the real binary rather than argued.
//
// Usage: GatewayJournalRestartTest <path-to-OrderEngine>

#include "GatewayProcess.h"

#include <cassert>
#include <cstdio>
#include <string>

using namespace OrderMatcher;
using namespace gwtest;

namespace {

constexpr ParticipantId kOwner   = 7;
constexpr SymbolId      kSymbol  = 0;

OrderRequest request(OrderRequest::Type type, OrderId orderId) {
    OrderRequest r{};
    r.type          = type;
    r.symbolId      = kSymbol;
    r.orderId       = orderId;
    r.participantId = kOwner;
    r.side          = Side::Buy;
    r.price         = 10000;
    r.qty           = 10;
    r.orderType     = OrderType::Limit;
    r.tif           = TimeInForce::GTC;
    return r;
}

enum class Stop { Clean, Kill9 };

// Acknowledge a resting order, end the process the given way, restart on the same
// journal, and require the order to still be there — then require a second cancel
// to change nothing, or the probe could not tell a restored order from a cancel
// that always removes something.
void acknowledgeThenRestart(const char* bin, uint16_t& port, Stop stop, OrderId id,
                            const char* label) {
    const std::string journal = tempJournalPath(
        stop == Stop::Clean ? "ob_gateway_journal_clean" : "ob_gateway_journal_kill9");
    std::remove(journal.c_str());

    pid_t pid = startOnFreePort(bin, port, journal);
    assert(pid > 0 && "OrderEngine never started listening");
    {
        Client c;
        assert(c.connect(port));
        assert(c.send(request(OrderRequest::Type::NewOrder, id)));
        GatewayResponse r{};
        assert(c.answerFor(id, r) && "no answer to the new order");
        assert(r.type == GatewayResponse::Type::Ack &&
               "the gateway did not acknowledge the order this test depends on");
    }
    if (stop == Stop::Clean) {
        assert(stopCleanly(pid) && "OrderEngine did not exit cleanly on SIGTERM");
    } else {
        // The instant the ack is in hand: nothing gets a chance to flush.
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
    }
    std::printf("  %s: order %llu acknowledged, then %s\n", label,
                static_cast<unsigned long long>(id),
                stop == Stop::Clean ? "SIGTERM" : "kill -9");

    ++port;
    pid = startOnFreePort(bin, port, journal);
    assert(pid > 0 && "OrderEngine did not restart on its own journal");
    const uint16_t admin = static_cast<uint16_t>(port + kAdminPortOffset);
    bool restored, removedTwice;
    {
        Client c;
        assert(c.connect(port));
        restored = cancelRemoved(c, admin, request(OrderRequest::Type::Cancel, id));
        removedTwice = cancelRemoved(c, admin, request(OrderRequest::Type::Cancel, id));
    }
    const bool stopped = stopCleanly(pid);
    std::remove(journal.c_str());
    assert(restored && "an order the gateway ACKNOWLEDGED is gone after the restart");
    assert(!removedTwice && "a second cancel removed an order, so the first proved nothing");
    assert(stopped);
    std::printf("  %s: restored after restart, cancelled exactly once\n", label);
    ++port;
}

}  // namespace

int main(int argc, char** argv) {
    assert(argc == 2 && "usage: GatewayJournalRestartTest <path-to-OrderEngine>");
    const char* bin = argv[1];
    uint16_t port = 31520;

    acknowledgeThenRestart(bin, port, Stop::Clean, 5001, "clean restart");
    acknowledgeThenRestart(bin, port, Stop::Kill9, 6001, "kill -9 after ack");

    std::puts("GatewayJournalRestartTest passed");
    return 0;
}
