// AdminServerEndpointsTest — verifies the /prometheus and /health
// endpoints over real HTTP sockets. The Prometheus endpoint returns the
// metrics registry's text exposition; /health returns a small JSON
// status. Both shape-checked.
//
// Why a real-socket test rather than a unit-level call: the response
// shape (HTTP headers, content-type) is part of the contract for
// Prometheus scrapers and k8s liveness probes, and a unit test wouldn't
// catch a header mistake.

#include "AdminServer.h"
#include "MatchingEngine.h"
#include "Metrics.h"

#include <arpa/inet.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

using namespace OrderMatcher;

namespace {

// Minimal HTTP GET. Returns the full response or "" on failure.
std::string httpGet(uint16_t port, const std::string& path) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return {};
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return {};
    }
    std::string req = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\n\r\n";
    ::send(fd, req.data(), req.size(), MSG_NOSIGNAL);

    std::string out;
    char buf[4096];
    for (;;) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        out.append(buf, size_t(n));
    }
    ::close(fd);
    return out;
}

// Scrape the endpoints that read worker-owned state while an async worker
// is matching. Roadmap 1.7-G3 (THR-14): /audit read the book's level counts
// with no lock, racing the worker that mutates them. Run under TSan.
void scrapeUnderLiveFlow() {
    constexpr SymbolId kSym = 1;
    constexpr int kScrapes = 200;
    MatchingEngine engine;
    engine.addSymbol(kSym);
    engine.startAsync(1);
    AdminServer admin(engine, 0);
    admin.setAuthDisabled(true);
    assert(admin.start() && "admin server failed to bind");

    std::atomic<bool> stop{false};
    std::thread producer([&] {
        // Levels come and go: alternating sides over a spread of prices.
        for (OrderId id = 1; !stop.load(std::memory_order_acquire); ++id) {
            const Side side = (id % 2) ? Side::Buy : Side::Sell;
            const Price px = 100 + static_cast<Price>(id % 9);
            engine.submitOrder(kSym, id, 7, side, px, 1, OrderType::Limit);
        }
    });

    for (int i = 0; i < kScrapes; ++i) {
        const std::string audit = httpGet(admin.port(), "/audit?symbolId=1");
        assert(audit.find("\"bidLevels\":") != std::string::npos);
    }

    stop.store(true, std::memory_order_release);
    producer.join();
    admin.stop();
    engine.stopAsync();
    std::puts("/audit under live order flow: ok");
}

}  // namespace

int main() {
    MatchingEngine engine;
    engine.start();

    // Bump a metric to make the Prometheus output non-trivial.
    auto& c = MetricsRegistry::instance().counter(
        "admin_test_marker_total", "test marker");
    c.increment();
    c.increment();

    // Port 0: the OS picks, start() reports. The pid-derived port this used
    // to guess is in the ephemeral range, so anything else on the machine
    // could already hold it — and start() had no way to say the bind failed.
    AdminServer admin(engine, 0);
    // This test exercises endpoint bodies, not auth — opt out explicitly,
    // since start() now refuses to listen without a token by default.
    admin.setAuthDisabled(true);
    assert(admin.start() && "admin server failed to bind");
    const uint16_t port = admin.port();

    // ---- /prometheus -------------------------------------------------------
    {
        auto resp = httpGet(port, "/prometheus");
        if (resp.empty()) {
            std::fprintf(stderr, "no response for /prometheus on port %u\n", port);
            std::abort();
        }
        // Headers contain the right Content-Type.
        assert(resp.find("HTTP/1.1 200 OK") != std::string::npos);
        assert(resp.find("Content-Type: text/plain; version=0.0.4")
               != std::string::npos &&
               "Prometheus endpoint must return text/plain;version=0.0.4");
        // Body contains our test counter and its TYPE annotation.
        assert(resp.find("# TYPE admin_test_marker_total counter")
               != std::string::npos);
        assert(resp.find("admin_test_marker_total 2") != std::string::npos);
        std::puts("/prometheus: shape OK, counter exported");
    }

    // ---- /health -----------------------------------------------------------
    {
        auto resp = httpGet(port, "/health");
        assert(resp.find("HTTP/1.1 200 OK") != std::string::npos);
        assert(resp.find("Content-Type: application/json") != std::string::npos);
        assert(resp.find("\"status\":\"ok\"") != std::string::npos);
        std::puts("/health: ok");
    }

    // ---- Discovery -------------------------------------------------------
    {
        auto resp = httpGet(port, "/");
        assert(resp.find("/prometheus") != std::string::npos &&
               "discovery JSON should advertise the prometheus endpoint");
        assert(resp.find("/health") != std::string::npos);
        std::puts("/: discovery advertises new endpoints");
    }

    admin.stop();
    engine.stop();

    scrapeUnderLiveFlow();
    std::puts("AdminServerEndpointsTest passed");
    return 0;
}
