#include "MatchingEngine.h"
#include <cassert>
#include <iostream>

using namespace OrderMatcher;

void testSyncSubmitResults() {
    std::cout << "Running testSyncSubmitResults..." << std::endl;

    MatchingEngine engine;
    SubmitResult stopped = engine.submitOrder(0, 1, 1, Side::Buy, toPrice(99.00), 10,
                                             OrderType::Limit);
    assert(!stopped.isAccepted());
    assert(stopped.rejectReason == RejectReason::EngineStopped);

    engine.start();

    SubmitResult missingSymbol = engine.submitOrder(99, 1, 1, Side::Buy, toPrice(99.00),
                                                    10, OrderType::Limit);
    assert(!missingSymbol.isAccepted());
    assert(missingSymbol.rejectReason == RejectReason::SymbolNotFound);

    SubmitResult badQty = engine.submitOrder(0, 1, 1, Side::Buy, toPrice(99.00), 0,
                                             OrderType::Limit);
    assert(!badQty.isAccepted());
    assert(badQty.rejectReason == RejectReason::InvalidQuantity);

    SubmitResult accepted = engine.submitOrder(0, 2, 1, Side::Buy, toPrice(99.00), 10,
                                               OrderType::Limit);
    assert(accepted.isAccepted());
    assert(accepted.sequenceId != 0);

    SubmitResult cancelMissing = engine.submitCancel(0, 999);
    assert(!cancelMissing.isAccepted());
    assert(cancelMissing.rejectReason == RejectReason::OrderNotFound);

    SubmitResult cancelled = engine.submitCancel(0, 2);
    assert(cancelled.isAccepted());
    assert(cancelled.sequenceId > accepted.sequenceId);

    engine.stop();
    std::cout << "testSyncSubmitResults PASSED" << std::endl;
}

void testAsyncIngressRejects() {
    std::cout << "Running testAsyncIngressRejects..." << std::endl;

    MatchingEngine engine;
    engine.addSymbol(1);
    engine.startAsync(1, 8);

    SubmitResult missingSymbol = engine.submitOrder(99, 1, 1, Side::Buy, toPrice(99.00),
                                                    10, OrderType::Limit);
    assert(!missingSymbol.isAccepted());
    assert(missingSymbol.rejectReason == RejectReason::SymbolNotFound);

    engine.setRateLimit(1, 1);
    SubmitResult first = engine.submitOrder(1, 2, 7, Side::Buy, toPrice(99.00), 10,
                                            OrderType::Limit);
    SubmitResult second = engine.submitOrder(1, 3, 7, Side::Buy, toPrice(99.00), 10,
                                             OrderType::Limit);
    assert(first.isAccepted());
    assert(!second.isAccepted());
    assert(second.rejectReason == RejectReason::RateLimitExceeded);
    assert(engine.getRateLimitedCount() == 1);

    engine.waitForDrain();
    engine.stopAsync();
    std::cout << "testAsyncIngressRejects PASSED" << std::endl;
}

int main() {
    std::cout << "\n=== Submit Result Tests ===" << std::endl;
    testSyncSubmitResults();
    testAsyncIngressRejects();
    std::cout << "\nALL SUBMIT RESULT TESTS PASSED!" << std::endl;
    return 0;
}
