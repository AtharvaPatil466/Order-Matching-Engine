// ExpiryAdmissionTest — audit PRIOR-7, roadmap 1.6-F6.
//
// WHAT WAS WRONG. expireOrders skips an order whose expiryTime is 0, so a GTD
// order sent without one was accepted and rested as GTC: nothing ever expired
// it, and the client believed it was time-bounded (audit h3 H17: accepted, and
// still resting after expireOrders(UINT64_MAX)). A DAY order with no
// expiryTime ends with the session — endTradingSession cancels DAY orders —
// but on an engine with no session schedule nothing ever calls that, so it
// was the same silent GTC.
//
// WHAT IS PINNED. submitOrder refuses, with MissingRequiredField, a GTD order
// with expiryTime 0, and a DAY order with expiryTime 0 on an engine with no
// session end scheduled — how OrderEngine and GatewayServer start; a
// SessionScheduler declares one. Orders that carry an expiry, and DAY orders
// on an engine whose session ends, are admitted as before.

#include "MatchingEngine.h"
#include "SessionScheduler.h"

#include <cassert>
#include <cstdio>

using namespace OrderMatcher;

namespace {

constexpr SymbolId kSym = 0;

SubmitResult submit(MatchingEngine& e, OrderId id, TimeInForce tif, uint64_t expiry) {
    return e.submitOrder(kSym, id, /*participantId=*/1, Side::Buy, 10000 + id, /*qty=*/5,
                         OrderType::Limit, /*stopPrice=*/0, /*displayQty=*/0, tif, expiry);
}

void expect(const SubmitResult& r, bool accepted, const char* what) {
    const bool ok = accepted ? r.isAccepted()
                             : !r.isAccepted() &&
                                   r.rejectReason == RejectReason::MissingRequiredField;
    if (!ok) {
        std::fprintf(stderr, "%s: %s reason=%u\n", what, r.isAccepted() ? "ACCEPTED" : "rejected",
                     (unsigned)r.rejectReason);
        assert(false && "expiry admission");
    }
}

void run(MatchingEngine& e, const char* mode) {
    e.setSessionEndScheduled(false);  // as OrderEngine / GatewayServer start
    expect(submit(e, 1, TimeInForce::GTD, 0), false, "GTD with expiryTime 0");
    expect(submit(e, 2, TimeInForce::DAY, 0), false, "DAY with expiryTime 0, no session end");
    expect(submit(e, 3, TimeInForce::GTD, 5'000), true, "GTD with an expiry");
    expect(submit(e, 4, TimeInForce::DAY, 5'000), true, "DAY with an expiry");
    expect(submit(e, 5, TimeInForce::GTC, 0), true, "GTC");
    e.waitForDrain();
    const OrderBook* book = e.getOrderBook(kSym);
    assert(book->getOrder(1) == nullptr && book->getOrder(2) == nullptr);
    assert(book->getOrder(3) && book->getOrder(4) && book->getOrder(5));

    // Audit H17: the order that is admitted still expires.
    e.getOrderBook(kSym)->expireOrders(5'000);
    assert(book->getOrder(3) == nullptr && book->getOrder(4) == nullptr &&
           book->getOrder(5) != nullptr);
    std::printf("  %s: GTD/DAY with no expiry and no session end rejected\n", mode);
}

}  // namespace

int main() {
    {
        MatchingEngine e;
        e.addSymbol(kSym);
        e.start();
        run(e, "sync");
        e.stop();
    }
    {
        MatchingEngine e;
        e.addSymbol(kSym);
        e.startAsync(1);
        run(e, "async");
        e.stopAsync();
    }
    {
        // A scheduled session end is what ends a DAY order with no expiry.
        MatchingEngine e;
        e.addSymbol(kSym);
        e.start();
        e.setSessionEndScheduled(false);
        SessionScheduler scheduler(e, {kSym});
        expect(submit(e, 10, TimeInForce::DAY, 0), true, "DAY with no expiry, session end scheduled");
        expect(submit(e, 11, TimeInForce::GTD, 0), false, "GTD with expiryTime 0, session end scheduled");
        assert(e.endTradingSession({kSym}) == 1 && e.getOrderBook(kSym)->getOrder(10) == nullptr);
        e.stop();
        std::puts("  with a SessionScheduler: DAY with no expiry admitted, ended by the session");
    }
    std::puts("ExpiryAdmissionTest passed");
    return 0;
}
