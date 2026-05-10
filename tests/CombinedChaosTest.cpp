// CombinedChaosTest — soak the full pipeline (TCP gateway → MpscQueue →
// MatchingEngine → Journal) with every fault point armed simultaneously,
// from N concurrent clients. Verifies the system as a whole degrades
// gracefully under combined faults rather than failing in pairs.
//
// Per-client invariants:
//   * accepted + rejected == sent  (no orders silently lost in transit)
//   * Every reject reason is QueueBackpressure (only legitimate one in
//     this scenario; anything else means the test setup is wrong or a
//     fault is causing an unintended path).
//
// Global invariants:
//   * Book contains exactly the accepted orders, no extras, no missing.
//   * Journal entries are all CRC-valid on recovery (lax mode).
//   * Test completes inside the timeout (no livelock from pop spurious
//     empties or recv loops).
//
// Requires -DENABLE_FAULT_INJECTION=ON. Without it the faults are no-ops
// and the test still runs as a happy-path soak.

#include "FaultInjector.h"
#include "Journal.h"
#include "MatchingEngine.h"
#include "TcpGateway.h"

#include <arpa/inet.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <unordered_set>
#include <vector>

using namespace OrderMatcher;
namespace fs = std::filesystem;

namespace {

class Client {
public:
    ~Client() { if (fd_ >= 0) close(fd_); }
    bool connect(uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) return false;
        int flag = 1;
        setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
#ifdef SO_NOSIGPIPE
        setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &flag, sizeof(flag));
#endif
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        return ::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    }
    bool send(const OrderRequest& req) {
        uint32_t len = htonl(sizeof(OrderRequest));
        return sendAll(&len, sizeof(len)) && sendAll(&req, sizeof(req));
    }
    bool recv(GatewayResponse& resp) {
        uint32_t len;
        if (!recvAll(&len, sizeof(len))) return false;
        if (ntohl(len) != sizeof(GatewayResponse)) return false;
        return recvAll(&resp, sizeof(resp));
    }
private:
    bool sendAll(const void* b, size_t n) {
        auto* p = static_cast<const char*>(b);
        while (n) {
            ssize_t r = ::send(fd_, p, n, 0);
            if (r <= 0) return false;
            p += r; n -= size_t(r);
        }
        return true;
    }
    bool recvAll(void* b, size_t n) {
        auto* p = static_cast<char*>(b);
        while (n) {
            ssize_t r = ::recv(fd_, p, n, 0);
            if (r <= 0) return false;
            p += r; n -= size_t(r);
        }
        return true;
    }
    int fd_{-1};
};

struct ClientStats {
    int sent{0};
    int accepted{0};
    int rejected{0};
};

}  // namespace

int main() {
#ifndef OB_ENABLE_FAULT_INJECTION
    std::puts("CombinedChaosTest: OB_ENABLE_FAULT_INJECTION not defined — "
              "running degenerate happy-path. Reconfigure with "
              "-DENABLE_FAULT_INJECTION=ON.");
#endif

    auto& fi = FaultInjector::instance();
    fi.reset();
    fi.seed(0xC0FFEE);

    // Moderate probabilities across every category. Push-side aggressive
    // enough to actually exercise the rejection path with the lowered
    // retry count below.
    fi.arm("gateway.recv.short_read",      0.4);
    fi.arm("gateway.recv.eagain",          0.05);
    fi.arm("queue.push.spurious_fail",     0.5);
    fi.arm("queue.pop.spurious_empty",     0.2);
    fi.arm("journal.commit.short_write",   0.02);
    fi.arm("journal.commit.fsync_fail",    0.10);
    // Skip journal.commit.bit_flip — it would corrupt the very orders we
    // are trying to verify against the book; bit-flip is covered by
    // JournalCorruptionTest in isolation.

    auto journalPath = fs::temp_directory_path() /
        ("combined_chaos_" + std::to_string(::getpid()) + ".log");
    fs::remove(journalPath);

    MatchingEngine engine;
    constexpr SymbolId kSymbol = 1;
    engine.addSymbol(kSymbol);
    engine.enableJournal(journalPath.string());
    engine.setMaxPushRetries(3);  // low enough that push faults can win
    engine.startAsync(/*numThreads=*/1, /*queueSize=*/8192);

    TcpGateway gateway(engine);
    assert(gateway.start(0));
    uint16_t port = gateway.port();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    constexpr int kClients = 4;
    constexpr int kOrdersPerClient = 250;
    std::vector<std::thread>      workers;
    std::vector<ClientStats>      stats(kClients);
    std::vector<std::vector<OrderId>> accepted(kClients);

    [[maybe_unused]] auto t0 = std::chrono::steady_clock::now();
    for (int c = 0; c < kClients; ++c) {
        workers.emplace_back([&, c] {
            Client cl;
            assert(cl.connect(port));
            // Order ID space: 1M apart per client so we can identify which
            // client owns each ID at verification time.
            uint64_t base = uint64_t(c + 1) * 1'000'000;
            for (int i = 0; i < kOrdersPerClient; ++i) {
                OrderRequest req{};
                req.type = OrderRequest::Type::NewOrder;
                req.symbolId = kSymbol;
                req.orderId = base + uint64_t(i + 1);
                req.participantId = ParticipantId(c + 1);
                req.side = Side::Buy;
                req.price = 10000;  // single deep price level — no crosses, no vol breaker
                req.qty = 1;
                req.orderType = OrderType::Limit;

                assert(cl.send(req));
                GatewayResponse resp{};
                assert(cl.recv(resp));
                assert(resp.orderId == req.orderId &&
                       "response orderId mismatch — gateway framing leak");

                ++stats[c].sent;
                if (resp.type == GatewayResponse::Type::Ack) {
                    ++stats[c].accepted;
                    accepted[c].push_back(req.orderId);
                } else {
                    // The only legitimate failure mode in this scenario.
                    if (resp.rejectReason != RejectReason::QueueBackpressure) {
                        std::fprintf(stderr,
                            "client %d unexpected reject reason=%d at i=%d\n",
                            c, int(resp.rejectReason), i);
                        std::abort();
                    }
                    ++stats[c].rejected;
                }
            }
        });
    }
    for (auto& w : workers) w.join();
    [[maybe_unused]] auto t1 = std::chrono::steady_clock::now();

    // Per-client accounting.
    [[maybe_unused]] int totalSent = 0, totalAccepted = 0, totalRejected = 0;
    std::unordered_set<OrderId> allAccepted;
    for (int c = 0; c < kClients; ++c) {
        assert(stats[c].sent == kOrdersPerClient);
        assert(stats[c].accepted + stats[c].rejected == stats[c].sent &&
               "per-client accounting drift");
        totalSent     += stats[c].sent;
        totalAccepted += stats[c].accepted;
        totalRejected += stats[c].rejected;
        for (OrderId id : accepted[c]) {
            assert(allAccepted.insert(id).second &&
                   "duplicate orderId across clients — engine accepted twice");
        }
    }

    // Drain the engine, then verify book matches the accepted set exactly.
    engine.waitForDrain();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto* book = engine.getOrderBook(kSymbol);
    assert(book);
    std::vector<const Order*> all(allAccepted.size() + 64);
    size_t bookCount = book->getAllOrders(all.data(), all.size());
    assert(bookCount == allAccepted.size() &&
           "book size mismatch with accepted set");
    for (size_t i = 0; i < bookCount; ++i) {
        assert(allAccepted.count(all[i]->id) == 1 &&
               "book contains an orderId nobody accepted");
    }

    gateway.stop();
    engine.stop();

    // Journal recovery: every entry that survived to disk must be CRC-valid
    // and sequence-monotone (lax). Recovered count is bounded above by the
    // engine's total work (acceptances + any internal log entries).
    auto entries = Journal(journalPath.string()).readAll(true, false);
    uint64_t lastSeq = 0;
    for (const auto& e : entries) {
        uint32_t expected = computeCRC32(&e, offsetof(JournalEntry, checksum));
        assert(e.checksum == expected && "torn or corrupt entry leaked");
        assert(e.sequenceNumber > lastSeq && "non-monotone sequence");
        lastSeq = e.sequenceNumber;
    }

#ifdef OB_ENABLE_FAULT_INJECTION
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    std::printf("combined chaos: %d clients × %d orders = %d sent, "
                "accepted=%d rejected=%d (%.1f%% reject) in %lld ms\n"
                "  faults fired: short_read=%llu eagain=%llu push_fail=%llu "
                "pop_empty=%llu short_write=%llu fsync_fail=%llu\n"
                "  journal recovered %zu entries (lax)\n",
                kClients, kOrdersPerClient, totalSent,
                totalAccepted, totalRejected,
                100.0 * totalRejected / std::max(1, totalSent),
                static_cast<long long>(ms),
                (unsigned long long)fi.activations("gateway.recv.short_read"),
                (unsigned long long)fi.activations("gateway.recv.eagain"),
                (unsigned long long)fi.activations("queue.push.spurious_fail"),
                (unsigned long long)fi.activations("queue.pop.spurious_empty"),
                (unsigned long long)fi.activations("journal.commit.short_write"),
                (unsigned long long)fi.activations("journal.commit.fsync_fail"),
                entries.size());

    // Sanity: every category fired at least a few times — proves the wiring
    // is actually hot under combined load. Lower bounds are intentionally
    // loose to avoid flakes from unlucky RNG draws.
    assert(fi.activations("gateway.recv.short_read") > 50);
    assert(fi.activations("queue.push.spurious_fail") > 50);
    assert(fi.activations("queue.pop.spurious_empty") > 5);
#endif

    fs::remove(journalPath);
    std::puts("CombinedChaosTest passed");
    return 0;
}
