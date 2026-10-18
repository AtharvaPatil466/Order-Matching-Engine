// JournalCommitDelayTest — group commit is bounded in TIME, not only in count
// (roadmap 1.4-D3, JRN-3).
//
// GroupCommit(64) committed when 64 entries had accumulated and at no other
// time. On a quiet book the batch never fills, so every order acknowledged
// since the last full batch sat in a user-space vector, indefinitely, and a
// plain kill -9 lost all of them (audit h5: 40 acked, 200 ms quiet, SIGKILL,
// journal 0 bytes). OrderEngine runs exactly that: async, GroupCommit(64).
//
// Now a batch commits when it is full OR when its oldest entry has waited the
// commit delay (default 200 µs):
//   - on the next append, under load (journalCommitsOnAgeUnderLoad);
//   - from the async worker's idle path when the orders stop
//     (asyncQuietPeriodThenKill — the audit's h5);
//   - from the TCP gateway's event loop on a synchronous engine
//     (syncGatewayQuietPeriodThenKill).
// TCP ports come from 49100-49109.

#include "GatewayProcess.h"
#include "Journal.h"
#include "MatchingEngine.h"
#include "OrderBook.h"
#include "TcpGateway.h"
#include "TempPath.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using namespace OrderMatcher;

namespace {

constexpr SymbolId kSym = 0;
constexpr int kOrders = 40;          // well under one 64-entry batch
constexpr auto kQuiet = std::chrono::milliseconds(200);

OrderRequest restingBuy(OrderId id) {
    OrderRequest r{};
    r.type = OrderRequest::Type::NewOrder;
    r.symbolId = kSym;
    r.orderId = id;
    r.participantId = 7;
    r.side = Side::Buy;
    r.price = 10000 - static_cast<Price>(id);
    r.qty = 10;
    r.orderType = OrderType::Limit;
    r.tif = TimeInForce::GTC;
    return r;
}

size_t restoredFrom(const std::string& path) {
    MatchingEngine e;
    e.addSymbol(kSym);
    e.enableJournal(path);
    e.replayJournal();
    size_t n = 0;
    for (OrderId id = 1; id <= kOrders; ++id)
        if (e.getOrderBook(kSym)->getOrder(id)) ++n;
    return n;
}

// The child acks kOrders orders, then idles. The parent waits for the acks,
// lets the book stay quiet, and kills the child without warning.
size_t ackQuietKill(const std::string& path, const std::function<void(int)>& child,
                    const std::function<void(int)>& driveFromParent) {
    std::filesystem::remove(path);
    int fds[2];
    assert(pipe(fds) == 0);
    std::fflush(stdout);
    const pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(fds[0]);
        child(fds[1]);
        for (;;) pause();
    }
    close(fds[1]);
    driveFromParent(fds[0]);
    close(fds[0]);
    std::this_thread::sleep_for(kQuiet);
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    const size_t restored = restoredFrom(path);
    std::filesystem::remove(path);
    return restored;
}

void journalCommitsOnAgeUnderLoad() {
    const std::string path = uniqueTempPath("commit_delay_unit.wal");
    std::filesystem::remove(path);
    {
        Journal j(path, Journal::SyncPolicy::GroupCommit, 64);
        j.logAddOrder(1, 7, kSym, Side::Buy, 10000, 10, OrderType::Limit);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        j.logAddOrder(2, 7, kSym, Side::Buy, 10000, 10, OrderType::Limit);
        std::printf("  under load: %zu pending after an append 2 ms behind the first\n",
                    j.pendingEntries());
        assert(j.pendingEntries() == 0 &&
               "the oldest entry was 2 ms old and the batch still did not commit");

        // Configurable: a long delay holds the batch, as before.
        j.setMaxCommitDelay(std::chrono::hours(1));
        j.logAddOrder(3, 7, kSym, Side::Buy, 10000, 10, OrderType::Limit);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        j.logAddOrder(4, 7, kSym, Side::Buy, 10000, 10, OrderType::Limit);
        assert(j.pendingEntries() == 2 && "setMaxCommitDelay did not hold the batch");
    }
    std::filesystem::remove(path);
}

// The audit's h5: OrderEngine's configuration — async, GroupCommit(64).
void asyncQuietPeriodThenKill() {
    struct Acks : EventListener {
        std::atomic<int> news{0};
        void onTrade(const Trade&) override {}
        void onOrderUpdate(const OrderUpdate& u) override {
            if (u.status == OrderStatus::Accepted) news.fetch_add(1, std::memory_order_release);
        }
        void onMarketData(const MarketDataUpdate&) override {}
        void onBookVisible(const BookVisibleUpdate&) override {}
    };
    const std::string path = uniqueTempPath("commit_delay_async.wal");
    const size_t restored = ackQuietKill(
        path,
        [&](int report) {
            static MatchingEngine engine;
            static Acks acks;
            engine.addSymbol(kSym);
            engine.getOrderBook(kSym)->setEventListener(&acks);
            engine.enableJournal(path);   // GroupCommit(64), as OrderEngine
            engine.startAsync(1);
            for (OrderId id = 1; id <= kOrders; ++id) {
                const OrderRequest r = restingBuy(id);
                engine.submitOrder(r.symbolId, r.orderId, r.participantId, r.side, r.price,
                                   r.qty, r.orderType);
            }
            while (acks.news.load(std::memory_order_acquire) < kOrders)
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            const char done = 1;
            (void)!write(report, &done, 1);
        },
        [](int fromChild) {
            char done = 0;
            assert(read(fromChild, &done, 1) == 1 && "the child never acked its orders");
        });
    std::printf("  async: %zu of %d acked orders restored after %lld ms quiet + SIGKILL\n",
                restored, kOrders, static_cast<long long>(kQuiet.count()));
    assert(restored == static_cast<size_t>(kOrders) &&
           "acked orders sat in the user-space batch through the quiet period");
}

// A synchronous engine behind TcpGateway, GroupCommit(64).
void syncGatewayQuietPeriodThenKill() {
    const std::string path = uniqueTempPath("commit_delay_sync.wal");
    const size_t restored = ackQuietKill(
        path,
        [&](int report) {
            static MatchingEngine engine;
            engine.start();
            engine.addSymbol(kSym);
            engine.enableJournal(path);
            static TcpGateway gateway(engine);
            uint16_t port = 0;
            for (uint16_t p = 49100; p < 49110 && port == 0; ++p)
                if (gateway.start(p)) port = p;
            (void)!write(report, &port, sizeof port);
        },
        [](int fromChild) {
            uint16_t port = 0;
            assert(read(fromChild, &port, sizeof port) == sizeof port);
            assert(port != 0 && "no free port in 49100-49109");
            gwtest::Client c;
            assert(c.connect(port));
            for (OrderId id = 1; id <= kOrders; ++id) {
                assert(c.send(restingBuy(id)));
                GatewayResponse r{};
                assert(c.answerFor(id, r) && r.type == GatewayResponse::Type::Ack);
            }
        });
    std::printf("  sync gateway: %zu of %d acked orders restored after %lld ms quiet + SIGKILL\n",
                restored, kOrders, static_cast<long long>(kQuiet.count()));
    assert(restored == static_cast<size_t>(kOrders) &&
           "acked orders sat in the user-space batch through the quiet period");
}

}  // namespace

int main() {
    journalCommitsOnAgeUnderLoad();
    asyncQuietPeriodThenKill();
    syncGatewayQuietPeriodThenKill();
    std::puts("JournalCommitDelayTest passed");
    return 0;
}
