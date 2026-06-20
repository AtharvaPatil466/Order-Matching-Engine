#include "TcpGateway.h"
#include "MarketDataPublisher.h"
#include <iostream>
#include <csignal>
#include <thread>
#include <chrono>
#include <vector>
#include <memory>
#include <string>
#include <cstring>
#include <cstdlib>
#include <atomic>
#include <mutex>

// POSIX networking for ForwardingProxy
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>

using namespace OrderMatcher;

static volatile bool running = true;
void signalHandler(int) { running = false; }

// Combined listener: logs trades + publishes market data to shared memory
struct GatewayListener : EventListener {
    uint32_t sym;
    MarketDataPublisher* mdPub;

    GatewayListener(uint32_t s, MarketDataPublisher* pub) : sym(s), mdPub(pub) {}

    void onTrade(const Trade& t) override {
        std::cout << "[TRADE] sym=" << sym
                  << " price=" << toDouble(t.price)
                  << " qty=" << t.quantity
                  << " buyer=" << t.buyerId
                  << " seller=" << t.sellerId
                  << std::endl;
    }

    void onMarketData(const MarketDataUpdate& u) override {
        if (mdPub) mdPub->publishUpdate(u);
    }
};

// ---------------------------------------------------------------------------
// ForwardingProxy: TCP proxy that reads GatewayRequestV2 from clients and
// forwards them verbatim to a remote replicated engine, then relays the
// GatewayResponse back to the client.
//
// Lifecycle:
//   - Construct with host, port (remote engine) and listenPort (local bind).
//   - Call start() — returns false if the listen socket cannot be opened.
//   - Call stop() to request shutdown; the accept loop will notice &running
//     going false at the next wakeup.
//
// Thread model: one std::thread (detached) per accepted client connection.
// A single forwardFd_ is shared across all client threads, protected by
// forwardMu_.  Any thread that finds the connection broken closes it, resets
// forwardFd_ to -1, and retries the connect before continuing.
// ---------------------------------------------------------------------------
class ForwardingProxy {
public:
    ForwardingProxy(std::string host, uint16_t remotePort, uint16_t listenPort)
        : remoteHost_(std::move(host))
        , remotePort_(remotePort)
        , listenPort_(listenPort)
        , listenFd_(-1)
        , forwardFd_(-1)
        , orderCount_(0)
    {}

    ~ForwardingProxy() { stop(); }

    ForwardingProxy(const ForwardingProxy&) = delete;
    ForwardingProxy& operator=(const ForwardingProxy&) = delete;

    bool start() {
        listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listenFd_ < 0) {
            std::cerr << "[Gateway] Failed to create listen socket: " << std::strerror(errno) << std::endl;
            return false;
        }

        int opt = 1;
        ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port        = htons(listenPort_);

        if (::bind(listenFd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            std::cerr << "[Gateway] bind failed on port " << listenPort_
                      << ": " << std::strerror(errno) << std::endl;
            ::close(listenFd_);
            listenFd_ = -1;
            return false;
        }

        if (::listen(listenFd_, 128) < 0) {
            std::cerr << "[Gateway] listen failed: " << std::strerror(errno) << std::endl;
            ::close(listenFd_);
            listenFd_ = -1;
            return false;
        }

        // Set a timeout on accept() so we can check ::running periodically
        struct timeval tv{};
        tv.tv_sec  = 1;
        tv.tv_usec = 0;
        ::setsockopt(listenFd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        // Initial connection attempt to the remote engine (best-effort; will
        // be retried per client thread if this fails).
        connectToEngine();

        return true;
    }

    void stop() {
        if (listenFd_ >= 0) {
            ::close(listenFd_);
            listenFd_ = -1;
        }
        std::lock_guard<std::mutex> lk(forwardMu_);
        if (forwardFd_ >= 0) {
            ::close(forwardFd_);
            forwardFd_ = -1;
        }
    }

    uint16_t port() const { return listenPort_; }

    // Main accept loop — call from the main thread.
    void run() {
        while (running) {
            struct sockaddr_in clientAddr{};
            socklen_t addrLen = sizeof(clientAddr);
            int clientFd = ::accept(listenFd_,
                                    reinterpret_cast<struct sockaddr*>(&clientAddr),
                                    &addrLen);
            if (clientFd < 0) {
                // EAGAIN/EWOULDBLOCK = timeout; loop back to check running.
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                    continue;
                if (!running) break;
                std::cerr << "[Gateway] accept error: " << std::strerror(errno) << std::endl;
                continue;
            }

            // Disable Nagle on the client socket for low latency round-trips,
            // and suppress SIGPIPE so a dead client does not kill the process.
            int noDelay = 1;
            ::setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, &noDelay, sizeof(noDelay));
            suppressSigpipe(clientFd);

            std::thread([this, clientFd]() { handleClient(clientFd); }).detach();
        }
    }

private:
    // Try to open a TCP connection to the remote engine.
    // Caller must NOT hold forwardMu_ (this acquires it).
    // Returns true if a connection is live after the call.
    bool connectToEngine() {
        std::lock_guard<std::mutex> lk(forwardMu_);
        if (forwardFd_ >= 0) return true;  // already connected

        struct addrinfo hints{};
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        struct addrinfo* res = nullptr;
        std::string portStr = std::to_string(remotePort_);
        int rc = ::getaddrinfo(remoteHost_.c_str(), portStr.c_str(), &hints, &res);
        if (rc != 0) {
            std::cerr << "[Gateway] getaddrinfo(" << remoteHost_ << "): "
                      << gai_strerror(rc) << std::endl;
            return false;
        }

        int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd < 0) {
            ::freeaddrinfo(res);
            std::cerr << "[Gateway] socket(): " << std::strerror(errno) << std::endl;
            return false;
        }

        // TCP_NODELAY and SIGPIPE suppression for the engine-side link.
        int noDelay = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &noDelay, sizeof(noDelay));
        suppressSigpipe(fd);

        if (::connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
            ::freeaddrinfo(res);
            ::close(fd);
            std::cerr << "[Gateway] connect to " << remoteHost_ << ":" << remotePort_
                      << " failed: " << std::strerror(errno) << std::endl;
            return false;
        }

        ::freeaddrinfo(res);
        forwardFd_ = fd;
        std::cout << "[Gateway] Connected to engine at "
                  << remoteHost_ << ":" << remotePort_ << std::endl;
        return true;
    }

    // Attempt reconnect with a 500 ms delay.
    // Called by a client thread when it detects a broken engine connection.
    void reconnectWithBackoff() {
        {
            std::lock_guard<std::mutex> lk(forwardMu_);
            if (forwardFd_ >= 0) {
                ::close(forwardFd_);
                forwardFd_ = -1;
            }
        }
        std::cerr << "[Gateway] Engine connection lost; retrying in 500 ms..." << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        connectToEngine();
    }

    // Suppress SIGPIPE on a socket fd.  Call once after socket creation.
    // On Linux MSG_NOSIGNAL is used per-send; on macOS SO_NOSIGPIPE is set.
    static void suppressSigpipe(int fd) {
#ifdef __APPLE__
        int nosig = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosig, sizeof(nosig));
#else
        (void)fd; // MSG_NOSIGNAL used at send time
#endif
    }

    // Fully write `len` bytes from `buf` to `fd`.
    // Returns false on partial write or error.
    static bool sendAll(int fd, const void* buf, size_t len) {
#ifdef __APPLE__
        constexpr int kSendFlags = 0;
#else
        constexpr int kSendFlags = MSG_NOSIGNAL;
#endif
        const char* ptr = static_cast<const char*>(buf);
        size_t remaining = len;
        while (remaining > 0) {
            ssize_t n = ::send(fd, ptr, remaining, kSendFlags);
            if (n <= 0) return false;
            ptr       += n;
            remaining -= static_cast<size_t>(n);
        }
        return true;
    }

    // Fully read `len` bytes into `buf` from `fd`.
    // Returns false on EOF or error.
    static bool recvAll(int fd, void* buf, size_t len) {
        char* ptr = static_cast<char*>(buf);
        size_t remaining = len;
        while (remaining > 0) {
            // MSG_WAITALL is available on both macOS and Linux.
            ssize_t n = ::recv(fd, ptr, remaining, MSG_WAITALL);
            if (n <= 0) return false;
            ptr       += n;
            remaining -= static_cast<size_t>(n);
        }
        return true;
    }

    // Per-client handler (runs in its own detached thread).
    void handleClient(int clientFd) {
        while (running) {
            // 1. Read a full GatewayRequestV2 frame from the client.
            GatewayRequestV2 req{};
            if (!recvAll(clientFd, &req, sizeof(GatewayRequestV2))) {
                // Client disconnected or sent a partial frame.
                break;
            }

            // Basic protocol validation.
            if (req.header.magic != GATEWAY_PROTOCOL_MAGIC) {
                std::cerr << "[Gateway] Invalid magic from client; dropping connection." << std::endl;
                break;
            }

            // 2. Forward to the remote engine, reconnecting if necessary.
            bool forwarded = false;
            for (int attempt = 0; attempt < 2 && running; ++attempt) {
                if (!ensureConnected()) {
                    // connectToEngine already logged; wait and retry once
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    continue;
                }

                int engineFd;
                {
                    std::lock_guard<std::mutex> lk(forwardMu_);
                    engineFd = forwardFd_;
                }

                if (!sendAll(engineFd, &req, sizeof(GatewayRequestV2))) {
                    reconnectWithBackoff();
                    continue;
                }

                // 3. Read the GatewayResponse back from the engine.
                GatewayResponse resp{};
                if (!recvAll(engineFd, &resp, sizeof(GatewayResponse))) {
                    reconnectWithBackoff();
                    continue;
                }

                // 4. Forward the response back to the originating client.
                if (!sendAll(clientFd, &resp, sizeof(GatewayResponse))) {
                    // Client went away; exit the per-client loop.
                    forwarded = true;  // engine side is fine
                    goto client_done;
                }

                forwarded = true;

                // Periodic order count logging.
                uint64_t count = ++orderCount_;
                if (count % 1000 == 0) {
                    std::cout << "[Gateway] Forwarded " << count << " orders to "
                              << remoteHost_ << ":" << remotePort_ << std::endl;
                }

                break;  // success; move to next frame
            }

            if (!forwarded) {
                // Exhausted retries (engine unreachable). Close the client connection
                // so the client can fail fast rather than hanging indefinitely.
                std::cerr << "[Gateway] Could not forward order after retries; "
                             "closing client connection." << std::endl;
                break;
            }
        }

client_done:
        ::close(clientFd);
    }

    // Return true if a live engine connection exists (or was just established).
    bool ensureConnected() {
        {
            std::lock_guard<std::mutex> lk(forwardMu_);
            if (forwardFd_ >= 0) return true;
        }
        return connectToEngine();
    }

    std::string  remoteHost_;
    uint16_t     remotePort_;
    uint16_t     listenPort_;
    int          listenFd_;
    int          forwardFd_;     // shared engine-side socket; guarded by forwardMu_
    std::mutex   forwardMu_;
    std::atomic<uint64_t> orderCount_;
};

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    uint16_t port = 9876;
    if (argc > 1) port = static_cast<uint16_t>(std::stoi(argv[1]));

    std::signal(SIGINT,  signalHandler);
    std::signal(SIGTERM, signalHandler);

    // Check for forwarding mode environment variables.
    const char* engineHost = std::getenv("OB_ENGINE_HOST");
    const char* enginePortStr = std::getenv("OB_ENGINE_PORT");

    if (engineHost && enginePortStr) {
        // ----------------------------------------------------------------
        // FORWARDING MODE: proxy all client frames to the remote engine.
        // ----------------------------------------------------------------
        uint16_t enginePort = static_cast<uint16_t>(std::stoi(enginePortStr));

        std::cout << "[Gateway] Forwarding mode: orders -> "
                  << engineHost << ":" << enginePort << std::endl;

        ForwardingProxy proxy(engineHost, enginePort, port);
        if (!proxy.start()) {
            std::cerr << "[Gateway] Failed to start forwarding proxy on port " << port << std::endl;
            return 1;
        }

        std::cout << "[Gateway] Proxy listening on port " << proxy.port()
                  << " -> " << engineHost << ":" << enginePort << std::endl;
        std::cout << "Press Ctrl+C to stop." << std::endl;

        proxy.run();  // blocks until running == false

        std::cout << "\n[Gateway] Shutting down forwarding proxy..." << std::endl;
        proxy.stop();

    } else {
        // ----------------------------------------------------------------
        // LOCAL MODE: embedded matching engine (original behaviour).
        // ----------------------------------------------------------------
        std::cout << "[Gateway] Local mode: embedded engine" << std::endl;

        // Create matching engine
        MatchingEngine engine;
        engine.start();
        engine.addSymbol(0);
        engine.addSymbol(1);

        // Start market data publisher
        MarketDataPublisher mdPub("orderbook_md");
        bool mdStarted = mdPub.start();
        if (mdStarted) {
            std::cout << "Market data publisher started on /orderbook_md" << std::endl;
        }

        // Set up event listeners for each symbol
        std::vector<std::unique_ptr<GatewayListener>> listeners;
        for (uint32_t sym = 0; sym < 2; ++sym) {
            auto* book = engine.getOrderBook(sym);
            if (book) {
                auto listener = std::make_unique<GatewayListener>(sym, mdStarted ? &mdPub : nullptr);
                book->setEventListener(listener.get());
                listeners.push_back(std::move(listener));
            }
        }

        // Start TCP gateway
        TcpGateway gateway(engine);
        if (!gateway.start(port)) {
            std::cerr << "Failed to start gateway on port " << port << std::endl;
            return 1;
        }

        std::cout << "Order Gateway listening on port " << gateway.port() << std::endl;
        std::cout << "Press Ctrl+C to stop." << std::endl;

        // Periodic snapshot publishing
        while (running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (mdStarted) {
                for (uint32_t sym = 0; sym < 2; ++sym) {
                    auto* book = engine.getOrderBook(sym);
                    if (book) {
                        auto snap = book->getSnapshot(5);
                        mdPub.publishSnapshot(snap);
                    }
                }
            }
        }

        std::cout << "\nShutting down..." << std::endl;
        gateway.stop();
        mdPub.stop();
        engine.stop();
    }

    return 0;
}
