// GatewayJournalRestartTest — an order the order-entry port acknowledged must
// still be resting after that process restarts.
//
// WHAT WAS WRONG. GatewayServer — the only shipped binary that takes client
// orders over the network — had no journal. Not a misconfigured one: the word
// "journal" did not appear in src/gateway_main.cpp. Every order it acknowledged
// lived only in memory, so any restart, clean or not, lost the whole book, and
// the one binary that DID journal (OrderEngine) had no order-entry listener at
// all. No sync policy, group commit or durable-ack mechanism could fix
// "acknowledged, then restarted" while the journal and the port lived in
// different processes. It now runs the same bootJournal() sequence as
// OrderEngine, before its listener accepts anything.
//
// HOW IT IS OBSERVED. Black-box, through the order port alone — GatewayServer
// has no admin endpoint to read the book with. Run 1 acknowledges a resting
// order and stops cleanly. Run 2, on the same journal, is asked to CANCEL that
// order: an Ack means the order was there to cancel; "Rejected: OrderNotFound"
// means the restart lost it. A second cancel must then be rejected, which proves
// the probe distinguishes the two answers and that the first cancel removed a
// real order rather than being acknowledged regardless.
//
// TWO PROPERTIES, one flow. A CLEAN restart (SIGTERM) must keep the order, which
// needs a journal at all. And an ACKNOWLEDGED order must survive kill -9 sent the
// instant the ack arrives, which needs more: the ack must not leave the process
// until the order is on disk. Under group commit the entry sits in a user-space
// batch when the ack goes out, so kill -9 loses an order the client was told it
// has. The second scenario is the "acked, then kill -9'd" question, answered by
// the real binary rather than argued.
//
// Usage: GatewayJournalRestartTest <path-to-GatewayServer>

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
// to be refused, or the probe could not tell a restored order from a cancel that
// always says yes.
void acknowledgeThenRestart(const char* bin, uint16_t& port, Stop stop, OrderId id,
                            const char* label) {
    const std::string journal = tempJournalPath(
        stop == Stop::Clean ? "ob_gateway_journal_clean" : "ob_gateway_journal_kill9");
    std::remove(journal.c_str());

    pid_t pid = startOnFreePort(bin, port, journal);
    assert(pid > 0 && "GatewayServer never started listening");
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
        assert(stopCleanly(pid) && "GatewayServer did not exit cleanly on SIGTERM");
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
    assert(pid > 0 && "GatewayServer did not restart on its own journal");
    {
        Client c;
        assert(c.connect(port));
        assert(c.send(request(OrderRequest::Type::Cancel, id)));
        GatewayResponse first{};
        assert(c.answerFor(id, first) && "no answer to the cancel");
        if (first.type != GatewayResponse::Type::Ack) {
            std::printf("  %s: after restart, cancel answered \"%s\"\n", label,
                        first.errorMessage);
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
            std::remove(journal.c_str());
        }
        assert(first.type == GatewayResponse::Type::Ack &&
               "an order the gateway ACKNOWLEDGED is gone after the restart");

        assert(c.send(request(OrderRequest::Type::Cancel, id)));
        GatewayResponse second{};
        assert(c.answerFor(id, second));
        assert(second.type == GatewayResponse::Type::Error &&
               second.rejectReason == RejectReason::OrderNotFound &&
               "a second cancel was not refused, so the first proved nothing");
    }
    assert(stopCleanly(pid));
    std::remove(journal.c_str());
    std::printf("  %s: restored after restart, cancelled exactly once\n", label);
    ++port;
}

}  // namespace

int main(int argc, char** argv) {
    assert(argc == 2 && "usage: GatewayJournalRestartTest <path-to-GatewayServer>");
    const char* bin = argv[1];
    uint16_t port = 48010;

    acknowledgeThenRestart(bin, port, Stop::Clean, 5001, "clean restart");
    acknowledgeThenRestart(bin, port, Stop::Kill9, 6001, "kill -9 after ack");

    std::puts("GatewayJournalRestartTest passed");
    return 0;
}
