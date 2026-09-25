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
// This covers a CLEAN restart only. Surviving kill -9 is a different, stronger
// property that depends on when the journal is synced relative to the ack; the
// durable-ack commit that follows this one pins that separately.
//
// Usage: GatewayJournalRestartTest <path-to-GatewayServer>

#include "MatchingEngine.h"
#include "TcpGateway.h"

#include <arpa/inet.h>
#include <cassert>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace OrderMatcher;

namespace {

constexpr OrderId       kOrderId = 5001;
constexpr ParticipantId kOwner   = 7;
constexpr SymbolId      kSymbol  = 0;

// The length-prefixed framing CombinedChaosTest already drives TcpGateway with.
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
        timeval tv{5, 0};   // a lost answer must fail the test, not hang it
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        return ::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    }
    bool send(const OrderRequest& req) {
        const uint32_t len = htonl(sizeof(OrderRequest));
        return sendAll(&len, sizeof(len)) && sendAll(&req, sizeof(req));
    }
    // The Ack or Error answering `orderId`, skipping any notification frames.
    bool answerFor(OrderId orderId, GatewayResponse& out) {
        for (int frames = 0; frames < 64; ++frames) {
            uint32_t len = 0;
            if (!recvAll(&len, sizeof(len)) || ntohl(len) != sizeof(GatewayResponse)) return false;
            if (!recvAll(&out, sizeof(out))) return false;
            const bool isAnswer = out.type == GatewayResponse::Type::Ack ||
                                  out.type == GatewayResponse::Type::Error;
            if (isAnswer && out.orderId == orderId) return true;
        }
        return false;
    }

private:
    bool sendAll(const void* b, size_t n) {
        auto* p = static_cast<const char*>(b);
        while (n) {
            const ssize_t r = ::send(fd_, p, n, MSG_NOSIGNAL);
            if (r <= 0) return false;
            p += r; n -= static_cast<size_t>(r);
        }
        return true;
    }
    bool recvAll(void* b, size_t n) {
        auto* p = static_cast<char*>(b);
        while (n) {
            const ssize_t r = ::recv(fd_, p, n, 0);
            if (r <= 0) return false;
            p += r; n -= static_cast<size_t>(r);
        }
        return true;
    }
    int fd_{-1};
};

// Start GatewayServer on `port` with `journal`, or return -1 if it exited
// before listening (a taken port, or a refusal to boot).
pid_t startGateway(const char* bin, uint16_t port, const std::string& journal) {
    const pid_t pid = fork();
    if (pid == 0) {
        // Unset, not blank: a set-but-empty OB_ENGINE_HOST sends the gateway
        // down the forwarding path, and inherited credentials would change
        // which boot this is.
        for (const char* v : {"OB_ENGINE_HOST", "OB_ENGINE_PORT", "OB_JOURNAL_PATH",
                              "OB_PARTICIPANT_CREDENTIALS", "OB_NO_PARTICIPANT_AUTH",
                              "OB_REPLAY_LEGACY_JOURNAL"})
            unsetenv(v);
        const std::string p = std::to_string(port);
        execl(bin, bin, p.c_str(), "--journal", journal.c_str(), "--no-participant-auth",
              static_cast<char*>(nullptr));
        _exit(127);
    }
    for (int i = 0; i < 200; ++i) {                 // up to 10 s
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid) return -1;
        Client probe;
        if (probe.connect(port)) return pid;
        usleep(50 * 1000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    return -1;
}

pid_t startOnFreePort(const char* bin, uint16_t& port, const std::string& journal) {
    for (int attempt = 0; attempt < 10; ++attempt, ++port) {
        const pid_t pid = startGateway(bin, port, journal);
        if (pid > 0) return pid;
    }
    return -1;
}

// SIGTERM and wait for a clean exit — the path that flushes the journal.
bool stopCleanly(pid_t pid) {
    kill(pid, SIGTERM);
    for (int i = 0; i < 200; ++i) {                 // up to 10 s
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid)
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        usleep(50 * 1000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    return false;
}

OrderRequest request(OrderRequest::Type type) {
    OrderRequest r{};
    r.type          = type;
    r.symbolId      = kSymbol;
    r.orderId       = kOrderId;
    r.participantId = kOwner;
    r.side          = Side::Buy;
    r.price         = 10000;
    r.qty           = 10;
    r.orderType     = OrderType::Limit;
    r.tif           = TimeInForce::GTC;
    return r;
}

std::string tempJournalPath() {
    const char* dir = std::getenv("TMPDIR");
    std::string base = (dir && *dir) ? dir : "/tmp";
    if (base.back() != '/') base += '/';
    return base + "ob_gateway_journal_" + std::to_string(getpid()) + ".wal";
}

}  // namespace

int main(int argc, char** argv) {
    assert(argc == 2 && "usage: GatewayJournalRestartTest <path-to-GatewayServer>");
    const char* bin = argv[1];
    const std::string journal = tempJournalPath();
    std::remove(journal.c_str());
    uint16_t port = 48010;

    // ── Run 1: acknowledge a resting order, then stop cleanly ───────────────
    pid_t pid = startOnFreePort(bin, port, journal);
    assert(pid > 0 && "GatewayServer never started listening");
    {
        Client c;
        assert(c.connect(port));
        assert(c.send(request(OrderRequest::Type::NewOrder)));
        GatewayResponse r{};
        assert(c.answerFor(kOrderId, r) && "no answer to the new order");
        assert(r.type == GatewayResponse::Type::Ack &&
               "the gateway did not acknowledge the order this test depends on");
    }
    assert(stopCleanly(pid) && "GatewayServer did not exit cleanly on SIGTERM");
    std::puts("  run 1: order acknowledged, gateway stopped cleanly");

    // ── Run 2: same journal — the order must still be there ─────────────────
    ++port;
    pid = startOnFreePort(bin, port, journal);
    assert(pid > 0 && "GatewayServer did not restart on its own journal");
    {
        Client c;
        assert(c.connect(port));

        assert(c.send(request(OrderRequest::Type::Cancel)));
        GatewayResponse first{};
        assert(c.answerFor(kOrderId, first) && "no answer to the cancel");
        if (first.type != GatewayResponse::Type::Ack) {
            std::printf("  run 2: cancel answered \"%s\"\n", first.errorMessage);
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
            std::remove(journal.c_str());
        }
        assert(first.type == GatewayResponse::Type::Ack &&
               "the order the gateway ACKNOWLEDGED before the restart is gone — "
               "the order-entry port is not journaling what it accepts");

        // Control: the same cancel again must be refused, or the probe above
        // could not tell a restored order from a cancel that always says yes.
        assert(c.send(request(OrderRequest::Type::Cancel)));
        GatewayResponse second{};
        assert(c.answerFor(kOrderId, second));
        assert(second.type == GatewayResponse::Type::Error &&
               second.rejectReason == RejectReason::OrderNotFound &&
               "a second cancel was not refused, so the first proved nothing");
    }
    assert(stopCleanly(pid));
    std::remove(journal.c_str());

    std::puts("  run 2: the acknowledged order was restored, and cancelled exactly once");
    std::puts("GatewayJournalRestartTest passed");
    return 0;
}
