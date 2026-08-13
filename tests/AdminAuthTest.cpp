// AdminAuthTest — verifies Bearer token auth on admin endpoints and
// readiness-gate behavior of /readyz.
//
// Tests:
//  1. /health is exempt from auth — returns 200 without a token.
//  2. /readyz is exempt from auth — never returns 401.
//  3. /readyz returns 503 before setReady(true).
//  4. Protected endpoint (/metrics) without token → 401.
//  5. Protected endpoint with wrong token → 401.
//  6. Protected endpoint with correct token → 200.
//  7. /readyz returns 200 after setReady(true).

#include "AdminServer.h"
#include "MatchingEngine.h"

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

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

using namespace OrderMatcher;

namespace {

// Send a minimal HTTP GET and return the full response, or "" on
// failure. `authHeader` is inserted verbatim as the Authorization
// header value (e.g. "Bearer secret123"). Leave empty for no header.
std::string httpGet(uint16_t port, const std::string& path,
                    const std::string& authHeader = "") {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return {};

    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

    // 2-second recv timeout — prevents the test hanging on a slow or
    // non-responding server.
    struct timeval tv{2, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return {};
    }

    std::string req = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\n";
    if (!authHeader.empty())
        req += "Authorization: " + authHeader + "\r\n";
    req += "Connection: close\r\n\r\n";
    ::send(fd, req.data(), req.size(), MSG_NOSIGNAL);

    std::string out;
    char buf[4096];
    for (;;) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    ::close(fd);
    return out;
}

// Pick a stable-but-unlikely-to-collide admin port based on the PID.
uint16_t pickPort() {
    return static_cast<uint16_t>(45000 + (::getpid() % 3000));
}

}  // namespace

int main() {
    MatchingEngine engine;
    engine.addSymbol(0);
    engine.startAsync(1, 256);

    const uint16_t port = pickPort();
    AdminServer admin(engine, port);
    admin.setAdminToken("secret123");
    admin.start();

    // Give the listen thread time to bind and start accept()ing.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // ── 1. /health is exempt: no token required, always 200 ────────────
    {
        auto r = httpGet(port, "/health");
        assert(!r.empty() && "no response for /health");
        assert(r.find("HTTP/1.1 200") != std::string::npos &&
               "/health must return 200 without auth");
        std::puts("/health (no auth): 200 OK");
    }

    // ── 2 & 3. /readyz is exempt from auth and initially 503 ───────────
    {
        auto r = httpGet(port, "/readyz");
        assert(!r.empty() && "no response for /readyz");
        // Must never 401 even without a token.
        assert(r.find("HTTP/1.1 401") == std::string::npos &&
               "/readyz must never return 401");
        // Before setReady(true) the engine is not ready.
        assert(r.find("503") != std::string::npos &&
               "/readyz must return 503 before setReady(true)");
        std::puts("/readyz (before setReady): 503 (auth-exempt)");
    }

    // ── 4. Protected endpoint without token → 401 ───────────────────────
    {
        auto r = httpGet(port, "/metrics");
        assert(!r.empty() && "no response for /metrics (no auth)");
        assert(r.find("HTTP/1.1 401") != std::string::npos &&
               "/metrics without token must return 401");
        std::puts("/metrics (no auth): 401 Unauthorized");
    }

    // ── 5. Protected endpoint with wrong token → 401 ────────────────────
    {
        auto r = httpGet(port, "/metrics", "Bearer wrongtoken");
        assert(!r.empty() && "no response for /metrics (wrong token)");
        assert(r.find("HTTP/1.1 401") != std::string::npos &&
               "/metrics with wrong token must return 401");
        std::puts("/metrics (wrong token): 401 Unauthorized");
    }

    // ── 6. Protected endpoint with correct token → 200 ──────────────────
    {
        auto r = httpGet(port, "/metrics", "Bearer secret123");
        assert(!r.empty() && "no response for /metrics (correct token)");
        assert(r.find("HTTP/1.1 200") != std::string::npos &&
               "/metrics with correct token must return 200");
        std::puts("/metrics (correct token): 200 OK");
    }

    // ── 7. /readyz returns 200 after setReady(true) ──────────────────────
    admin.setReady(true);
    {
        auto r = httpGet(port, "/readyz");
        assert(!r.empty() && "no response for /readyz (after setReady)");
        assert(r.find("HTTP/1.1 200") != std::string::npos &&
               "/readyz must return 200 after setReady(true)");
        std::puts("/readyz (after setReady): 200 OK");
    }

    // ── 8. 401 response is well-formed HTTP: Content-Length == body ────
    // Regression guard: the 401 previously hardcoded Content-Length: 21
    // for a 28-byte body, so spec-compliant clients truncated it.
    {
        auto r = httpGet(port, "/metrics");
        assert(r.find("HTTP/1.1 401") != std::string::npos &&
               "/metrics without token must return 401");
        size_t clPos = r.find("Content-Length: ");
        assert(clPos != std::string::npos && "401 must declare Content-Length");
        size_t clEnd = r.find("\r\n", clPos);
        const size_t declared =
            std::stoul(r.substr(clPos + 16, clEnd - clPos - 16));
        size_t bodyPos = r.find("\r\n\r\n");
        assert(bodyPos != std::string::npos && "401 must terminate headers");
        const size_t actual = r.size() - (bodyPos + 4);
        assert(declared == actual &&
               "401 Content-Length must match actual body size");
        std::puts("/metrics 401 response: Content-Length matches body");
    }

    admin.stop();

    // ── 9. Fail closed: no token and no explicit opt-out → never binds ──
    // The regression this guards: an operator who forgets to configure a
    // token used to get a fully open admin port serving /book, /otr and
    // /risk to anyone with network reach. Omitting the token must now be a
    // startup failure, not a silent exposure.
    {
        const uint16_t openPort = pickPort();
        AdminServer unconfigured(engine, openPort);
        unconfigured.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        auto r = httpGet(openPort, "/health");
        assert(r.empty() &&
               "AdminServer must not listen without a token or explicit opt-out");
        std::puts("no token, no opt-out: port closed");
        unconfigured.stop();  // no-op; never started
    }

    // ── 10. Explicit opt-out still works, for local dev and tests ───────
    {
        const uint16_t openPort = pickPort();
        AdminServer opted(engine, openPort);
        opted.setAuthDisabled(true);
        opted.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        auto r = httpGet(openPort, "/metrics");
        assert(!r.empty() && "explicit opt-out must still serve");
        assert(r.find("HTTP/1.1 200") != std::string::npos &&
               "with auth explicitly disabled, /metrics must return 200");
        std::puts("explicit opt-out: /metrics 200 OK");
        opted.stop();
    }

    engine.stopAsync();

    std::puts("AdminAuthTest: all assertions passed");
    return 0;
}
