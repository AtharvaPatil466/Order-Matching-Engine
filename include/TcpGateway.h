#pragma once

#include "MatchingEngine.h"
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <mutex>

// POSIX networking
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

// Event loop: kqueue on macOS, epoll on Linux
#ifdef __APPLE__
#include <sys/event.h>
#define USE_KQUEUE 1
#elif defined(__linux__)
#include <sys/epoll.h>
#define USE_EPOLL 1
#endif

namespace OrderMatcher {

constexpr uint32_t GATEWAY_PROTOCOL_MAGIC = 0x4F424757; // "OBGW"
constexpr uint16_t GATEWAY_PROTOCOL_V1 = 1;
constexpr uint16_t GATEWAY_PROTOCOL_V2 = 2;
constexpr uint32_t GATEWAY_MAX_FRAME_SIZE = 1024 * 1024;
constexpr uint16_t GATEWAY_REQUEST_HEADER_SIZE = 24;
constexpr uint16_t GATEWAY_RESPONSE_HEADER_SIZE = 24;

#pragma pack(push, 1)
struct GatewayRequestHeader {
    uint32_t magic{GATEWAY_PROTOCOL_MAGIC};
    uint16_t version{GATEWAY_PROTOCOL_V2};
    uint16_t headerSize{GATEWAY_REQUEST_HEADER_SIZE};
    uint32_t payloadSize{sizeof(OrderRequest)};
    uint32_t flags{0};
    uint64_t clientRequestId{0};
};

struct GatewayRequestV2 {
    GatewayRequestHeader header{};
    OrderRequest request{};
};
#pragma pack(pop)

struct GatewayResponse {
    enum class Type : uint8_t {
        Ack = 1,
        TradeNotification = 2,
        OrderUpdateNotification = 3,
        Error = 4
    };

    Type type;
    uint8_t padding[3];
    OrderId orderId;
    RejectReason rejectReason{RejectReason::None};
    uint64_t sequenceId{0};
    Trade trade;
    OrderUpdate orderUpdate;
    char errorMessage[128];
};

#pragma pack(push, 1)
struct GatewayResponseHeader {
    uint32_t magic{GATEWAY_PROTOCOL_MAGIC};
    uint16_t version{GATEWAY_PROTOCOL_V2};
    uint16_t headerSize{GATEWAY_RESPONSE_HEADER_SIZE};
    uint32_t payloadSize{sizeof(GatewayResponse)};
    uint32_t flags{0};
    uint64_t clientRequestId{0};
};

struct GatewayResponseV2 {
    GatewayResponseHeader header{};
    GatewayResponse response{};
};
#pragma pack(pop)

class TcpGateway {
public:
    explicit TcpGateway(MatchingEngine& engine)
        : engine_(engine), listenFd_(-1), evFd_(-1), running_(false) {}

    ~TcpGateway() { stop(); }

    TcpGateway(const TcpGateway&) = delete;
    TcpGateway& operator=(const TcpGateway&) = delete;

    bool start(uint16_t port, int backlog = 128);
    void stop();
    bool isRunning() const { return running_; }
    uint16_t port() const { return port_; }
    size_t clientCount() const {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        return clients_.size();
    }

    // IP whitelist: if empty, all IPs allowed. If non-empty, only listed IPs accepted.
    void addAllowedIP(const std::string& ip) {
        struct in_addr addr;
        if (inet_pton(AF_INET, ip.c_str(), &addr) == 1) {
            allowedIPs_.insert(addr.s_addr);
        }
    }

    void clearAllowedIPs() { allowedIPs_.clear(); }

private:
    // Per-client state for partial read buffering and write queueing
    struct ClientState {
        std::vector<char> readBuf;
        size_t readPos{0};
        struct sockaddr_in addr;
        // Write-side output buffering
        std::vector<char> writeBuf;
        // Idle timeout tracking
        std::chrono::steady_clock::time_point lastActivity;
        uint16_t protocolVersion{GATEWAY_PROTOCOL_V1};
        uint64_t lastClientRequestId{0};
    };

    void eventLoop();
    void handleAccept();
    void handleClientData(int fd);
    // Frame + decode + submit every whole frame currently buffered in `state`.
    // Extracted from handleClientData so kernel and kernel-bypass transports
    // share one framer. Returns false if the client was removed (caller stops).
    bool feedBytes(int fd, ClientState& state);
    void processMessage(int fd, const OrderRequest& req);
    void removeClient(int fd);
    bool sendResponse(int fd, const GatewayResponse& resp);
    bool decodeFrame(ClientState& state, uint32_t msgLen, OrderRequest& req,
                     GatewayResponse& errorResp);
    bool isIPAllowed(uint32_t ip) const;
    void flushWriteBuffer(int fd);
    void checkIdleTimeouts();

    static void setNonBlocking(int fd);
    void addToEventLoop(int fd);
    void addWriteToEventLoop(int fd);
    void removeWriteFromEventLoop(int fd);
    void removeFromEventLoop(int fd);
    void wakeEventLoop();

    MatchingEngine& engine_;
    int listenFd_;
    int evFd_;           // kqueue or epoll fd
    uint16_t port_{0};
    std::atomic<bool> running_;
    std::thread eventThread_;

    std::unordered_map<int, ClientState> clients_;
    mutable std::mutex clientsMutex_;
    std::unordered_set<uint32_t> allowedIPs_;  // IPv4 addresses in network byte order

    // Graceful shutdown: self-pipe to wake event loop
    int shutdownPipe_[2] = {-1, -1};

    // Idle timeout (default 60 seconds)
    std::chrono::seconds idleTimeout_{60};

public:
    void setIdleTimeout(std::chrono::seconds timeout) { idleTimeout_ = timeout; }
};
} // namespace OrderMatcher
