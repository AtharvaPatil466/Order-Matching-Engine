// EngineFieldValidationTest — audit PROTO-1, roadmap 1.6-F1.
//
// The 0.13 stop-gap range-checked side, orderType, tif and pegType in
// TcpGateway::processMessage, so only that one ingress was covered. Every
// other caller of MatchingEngine::submitOrder — the admin endpoint, OUCH, SBE,
// FIX, an in-process strategy, the async worker it enqueues for — handed the
// raw enum straight to the book. A side byte of 2 rested in the asks (the book
// picks a side with `side == Buy ? bids_ : asks_`), and an orderType past LOC
// fell through every switch in matching, market data and the journal and
// rested as if it were a limit.
//
// WHAT IS PINNED. submitOrder itself refuses a value the schema does not
// define, with InvalidFieldValue, before the order is enqueued or booked — in
// sync and async mode alike — and nothing reaches the book.

#include "MatchingEngine.h"

#include <cassert>
#include <cstdio>

using namespace OrderMatcher;

namespace {

constexpr SymbolId kSym = 1;
constexpr Price kPx = 10000;

// One past the last enumerator: the first value the schema does not define.
template <typename E, E Last>
constexpr E pastEnd() { return static_cast<E>(static_cast<uint8_t>(Last) + 1); }

struct Fields {
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    TimeInForce tif = TimeInForce::GTC;
    PegType peg = PegType::None;
};

SubmitResult submit(MatchingEngine& e, OrderId id, const Fields& f) {
    return e.submitOrder(kSym, id, /*participantId=*/1, f.side, kPx, /*qty=*/10, f.type,
                         /*stopPrice=*/0, /*displayQty=*/0, f.tif, /*expiryTime=*/0,
                         /*stopLimitPrice=*/0, f.peg);
}

void undefinedFieldsAreRefused(MatchingEngine& e, const char* mode) {
    Fields side;  side.side = pastEnd<Side, Side::Sell>();
    Fields type;  type.type = pastEnd<OrderType, OrderType::LOC>();
    Fields tif;   tif.tif   = pastEnd<TimeInForce, TimeInForce::DAY>();
    Fields peg;   peg.peg   = pastEnd<PegType, PegType::PrimaryPeg>();
    Fields wild;  wild.side = static_cast<Side>(7); wild.type = static_cast<OrderType>(200);

    const Fields bad[] = {side, type, tif, peg, wild};
    OrderId id = 1;
    for (const Fields& f : bad) {
        const SubmitResult r = submit(e, id, f);
        if (r.isAccepted() || r.rejectReason != RejectReason::InvalidFieldValue) {
            std::fprintf(stderr, "[%s] order %llu side=%u type=%u tif=%u peg=%u: %s reason=%u\n",
                         mode, (unsigned long long)id, (unsigned)f.side, (unsigned)f.type,
                         (unsigned)f.tif, (unsigned)f.peg,
                         r.isAccepted() ? "ACCEPTED" : "rejected", (unsigned)r.rejectReason);
            assert(false && "submitOrder admitted an enum value the schema does not define");
        }
        ++id;
    }

    e.waitForDrain();
    const OrderBook* book = e.getOrderBook(kSym);
    for (OrderId i = 1; i < id; ++i) assert(book->getOrder(i) == nullptr);
    assert(book->getBidLevelsCount() == 0 && book->getAskLevelsCount() == 0 &&
           "nothing a rejected order carried may be in the book");

    // Control: the last enumerator of each field is still defined.
    Fields last;
    last.side = Side::Sell;
    last.tif = TimeInForce::GTC;
    assert(submit(e, 100, last).isAccepted());
    e.waitForDrain();
    assert(book->getOrder(100) != nullptr);
    std::printf("  %s: undefined side/orderType/tif/pegType rejected (InvalidFieldValue)\n", mode);
}

}  // namespace

int main() {
    {
        MatchingEngine e;
        e.addSymbol(kSym);
        e.start();
        undefinedFieldsAreRefused(e, "sync");
        e.stop();
    }
    {
        MatchingEngine e;
        e.addSymbol(kSym);
        e.startAsync(1);
        undefinedFieldsAreRefused(e, "async");
        e.stopAsync();
    }
    std::puts("EngineFieldValidationTest passed");
    return 0;
}
