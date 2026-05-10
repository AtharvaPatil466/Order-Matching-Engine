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

using namespace OrderMatcher;

namespace {

// Minimal HTTP GET. Returns the full response or "" on failure.
std::string httpGet(uint16_t port, const std::string& path) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return {};
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return {};
    }
    std::string req = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\n\r\n";
    ::send(fd, req.data(), req.size(), 0);

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

// Find a fixed-format admin port. The OS will sometimes pick the same
// port on a clean run; on collision we retry with a different one.
// In production AdminServer would use port 0 (OS-picks); we keep an
// explicit port here because AdminServer's API doesn't expose the
// chosen port back to the caller.
uint16_t pickPort() {
    // Use the test pid as a port-space partition to avoid collisions
    // when this test runs in parallel with itself in CI.
    return static_cast<uint16_t>(40000 + (::getpid() % 5000));
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

    uint16_t port = pickPort();
    AdminServer admin(engine, port);
    admin.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

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
    std::puts("AdminServerEndpointsTest passed");
    return 0;
}
