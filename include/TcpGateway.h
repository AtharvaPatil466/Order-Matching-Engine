#pragma once

#include "MatchingEngine.h"
#include "ParticipantAuth.h"
#include <string>
#include <string_view>
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

// Frame kind rides in the V2 header's `flags` word, which was reserved and has
// only ever been sent as zero. Zero still means "this payload is an
// OrderRequest", so every existing client keeps working untouched and no
// version bump is needed. Unknown bits are rejected rather than ignored —
// quietly dropping a flag is how an authenticated frame gets downgraded into
// an unauthenticated one.
constexpr uint32_t GATEWAY_FLAG_LOGIN  = 1u << 0;
constexpr uint32_t GATEWAY_FLAGS_KNOWN = GATEWAY_FLAG_LOGIN;

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

// Login payload (header.flags & GATEWAY_FLAG_LOGIN). Fixed-width fields rather
// than length-prefixed strings, matching the rest of this protocol; the
// gateway reads them with strnlen, so an unterminated field is truncated, not
// run off the end of.
struct GatewayLoginRequest {
    char user[32];
    char secret[64];
};

struct GatewayLoginV2 {
    GatewayRequestHeader header{};
    GatewayLoginRequest  login{};
};
#pragma pack(pop)

// Build a login frame. One place knows the wire rules — the flag bit, the
// payload size, and that both fields truncate rather than overrun — so a
// client cannot get them subtly wrong.
inline GatewayLoginV2 makeGatewayLogin(std::string_view user,
                                       std::string_view secret,
                                       uint64_t clientRequestId = 0) {
    GatewayLoginV2 frame{};
    frame.header.payloadSize     = sizeof(GatewayLoginRequest);
    frame.header.flags           = GATEWAY_FLAG_LOGIN;
    frame.header.clientRequestId = clientRequestId;
    user.copy(frame.login.user, sizeof(frame.login.user) - 1);
    secret.copy(frame.login.secret, sizeof(frame.login.secret) - 1);
    return frame;
}

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

    // H7: install the credential store. With one configured, a connection must
    // send a login frame (GATEWAY_FLAG_LOGIN) before any order, and may only
    // act as a participant its credential covers. With none, the gateway keeps
    // the pre-H7 behaviour and takes req.participantId on trust.
    void setParticipantAuth(const ParticipantAuth* auth) { auth_ = auth; }

private:
    // Per-client state for partial read buffering and write queueing
    struct ClientState {
        std::vector<char> readBuf;
        size_t readPos{0};
        struct sockaddr_in addr;
        // Write-side output buffering. Bounded: a client that stops reading
        // must not be able to make the gateway queue unbounded memory on its
        // behalf (writeBufOverflowed marks it for reaping).
        std::vector<char> writeBuf;
        bool writeBufOverflowed{false};
        // Idle timeout tracking
        std::chrono::steady_clock::time_point lastActivity;
        uint16_t protocolVersion{GATEWAY_PROTOCOL_V1};
        uint64_t lastClientRequestId{0};
        // Who this connection proved it is. Default-constructed permits
        // nothing, so a session that never logged in fails closed.
        AuthorizedIdentity identity;
    };

    // What one framed request turned out to be. The two payloads are small and
    // a connection sends at most a handful of logins, so they sit side by side
    // rather than in a variant.
    struct DecodedFrame {
        enum class Kind { Order, Login };
        Kind                kind{Kind::Order};
        OrderRequest        order{};
        GatewayLoginRequest login{};
    };

    void eventLoop();
    void handleAccept();
    void handleClientData(int fd);
    // Frame + decode + submit every whole frame currently buffered in `state`.
    // Extracted from handleClientData so kernel and kernel-bypass transports
    // share one framer. Returns false if the client was removed (caller stops).
    bool feedBytes(int fd, ClientState& state);
    void processMessage(int fd, ClientState& state, const OrderRequest& req);
    // Returns false when the connection must be closed. It does NOT remove the
    // client itself: feedBytes still holds a reference into clients_ for this
    // fd, so the caller does the removal once it is done with `state`.
    bool handleLogin(int fd, ClientState& state, const GatewayLoginRequest& login);
    void removeClient(int fd);
    bool sendResponse(int fd, const GatewayResponse& resp);
    bool decodeFrame(ClientState& state, uint32_t msgLen, DecodedFrame& out,
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
    const ParticipantAuth* auth_{nullptr};
    std::chrono::seconds idleTimeout_{60};

    // Ceiling on one client's queued outbound bytes. A slow reader is not
    // idle — it keeps sending, refreshing lastActivity — so the idle timeout
    // never reaps it and the queue is the only thing that bounds its memory.
    // ponytail: fixed byte cap, no per-client tuning until a deployment needs it.
    static constexpr size_t kMaxWriteBufBytes = 1u << 20;  // 1 MiB

public:
    void setIdleTimeout(std::chrono::seconds timeout) { idleTimeout_ = timeout; }
};
} // namespace OrderMatcher
