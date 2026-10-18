// ZeroQuantityTradeTest — match() never prints a trade of quantity zero
// (roadmap 1.5-E2, MATCH-2).
//
// A resting order with nothing available — an iceberg with an empty slice, an
// order left at remainingQty 0 — used to reach the tape as a TRADE OF ZERO, to
// both participants, and could elect stops on a print that moved nothing. The
// routes that produced such orders are closed (modify-to-zero is refused, the
// slice is taken in addToBook), so match() now asserts it never gets there:
// the next route found fails loudly in every assertion-enabled build instead
// of printing. The state is injected, in a child process, and the child must
// die on the assertion rather than return.

#include "OrderBook.h"

#include <cassert>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <sys/wait.h>
#include <unistd.h>

using namespace OrderMatcher;

namespace {

struct Tape : EventListener {
    int zeroTrades = 0;
    void onTrade(const Trade& t) override { zeroTrades += t.quantity == 0; }
};

[[noreturn]] void child() {
    OrderBook book(1);
    book.setCircuitBreakerThreshold(1e9);
    Tape tape;
    book.setEventListener(&tape);
    book.addOrder(1, 1, Side::Sell, 10100, 100, OrderType::Iceberg, 0, /*displayQty=*/10);
    const_cast<Order*>(book.getOrder(1))->visibleQty = 0;  // fault injection
    book.addOrder(2, 2, Side::Buy, 10100, 5, OrderType::Limit);
    std::_Exit(tape.zeroTrades > 0 ? 3 : 0);
}

}  // namespace

int main() {
    const pid_t pid = ::fork();
    assert(pid >= 0);
    if (pid == 0) child();

    int status = 0;
    assert(::waitpid(pid, &status, 0) == pid);
    if (WIFEXITED(status))
        std::printf("  child returned %d: %s\n", WEXITSTATUS(status),
                    WEXITSTATUS(status) == 3 ? "a zero-quantity trade was printed"
                                             : "no trade and no assertion");
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT &&
           "match() must assert before emitting a zero-quantity trade");
    std::puts("ZeroQuantityTradeTest passed");
    return 0;
}
