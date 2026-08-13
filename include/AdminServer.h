#pragma once

#include "MatchingEngine.h"
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace OrderMatcher {

// Forward declaration — full definition (with POSIX networking deps)
// lives in ReplicationProtocol.h. AdminServer only needs an opaque
// pointer plus the methods it calls; including the full header here
// would pull <arpa/inet.h> and friends into every AdminServer client.
class ReplicationCoordinator;

class AdminServer {
public:
    AdminServer(MatchingEngine& engine, uint16_t port);
    ~AdminServer();

    void start();
    void stop();

    // Optional: attach a replication coordinator so /role and
    // /replication endpoints return live state. Pass nullptr (the
    // default) on engines that run without replication.
    void setReplicationCoordinator(ReplicationCoordinator* coord) {
        coord_ = coord;
    }

    // All endpoints except /health and /readyz require an
    // "Authorization: Bearer <token>" header. Call before start().
    //
    // Authentication is REQUIRED by default: start() refuses to listen
    // unless either a token is set here or setAuthDisabled(true) is called
    // explicitly. The admin surface exposes the full order book (/book),
    // participant risk state, and — when OB_CHAOS_INJECT is on — order
    // injection, on a socket bound to INADDR_ANY. An empty-token default
    // meant a deployment that simply forgot to configure a token served all
    // of that to anyone with network reach, silently. Forgetting is now a
    // startup failure rather than an open port.
    void setAdminToken(const std::string& token) { adminToken_ = token; }

    // Explicit opt-out from the auth requirement, for local development and
    // tests. Deliberately verbose and deliberately not the default: running
    // the admin port unauthenticated has to be a decision someone made, not
    // a config they omitted.
    void setAuthDisabled(bool disabled) { authDisabled_ = disabled; }

    // Readiness gate — false until the caller signals the engine is
    // ready to serve traffic. Flipped via setReady(true) after warm-up
    // (journal replay, risk-limit load, peer handshake, etc.).
    // /readyz returns 200 when ready, 503 otherwise.
    void setReady(bool r) { ready_.store(r, std::memory_order_release); }
    bool isReady() const { return ready_.load(std::memory_order_acquire); }

private:
    void listenLoop();
    void handleConnection(int clientSocket);

    std::string buildHttpResponse(const std::string& body,
                                  const char* contentType = "application/json") const;
    std::string generateMetricsResponse() const;
    std::string generatePrometheusResponse() const;
    std::string generateHealthResponse() const;
    std::string generateOtrResponse(ParticipantId pid) const;
    std::string generateBookResponse(SymbolId sym) const;
    std::string generateAuctionResponse(SymbolId sym) const;
    std::string generateFeesResponse(ParticipantId pid) const;
    std::string generateRiskResponse(uint32_t tier, uint64_t entityId) const;
    std::string generateCatResponse() const;
    std::string generateAuditResponse(SymbolId sym) const;
    std::string generateRoleResponse() const;
    std::string generateReplicationResponse() const;
    std::string generateJournalHeadResponse() const;
    std::string generateVersionResponse() const;
    std::string generateReadyzResponse() const;
    // Chaos-only order injection (guarded by OB_CHAOS_INJECT=1 env
    // AND, if OB_CHAOS_TOKEN is set, a matching X-Chaos-Token header).
    // Not exposed unless the env var is set at process start — the
    // gate is checked once in the constructor and cached.
    std::string generateChaosOrderResponse(const std::string& query,
                                           const std::string& token);

    MatchingEngine& engine_;
    uint16_t port_;
    // ponytail: atomic — stop() writes -1 while listenLoop()'s accept()
    // reads it on another thread (TSan data race on the plain int otherwise).
    std::atomic<int> serverSocket_{-1};
    std::atomic<bool> running_{false};
    std::thread listenThread_;
    ReplicationCoordinator* coord_{nullptr};

    // Captured at construction from OB_CHAOS_INJECT — guards the
    // /chaos/order endpoint at parse time. Compiled in unconditionally
    // so the binary is identical across environments; activation is
    // environment-only.
    bool chaosInjectEnabled_{false};

    // Shared-secret token for /chaos/order. Captured from
    // OB_CHAOS_TOKEN at startup. When set AND chaosInjectEnabled_,
    // requests must carry a matching `X-Chaos-Token` header.
    // Empty token = inject open to anyone with network reach
    // (logged as a security warning at startup).
    std::string chaosToken_;

    // Bearer token for all non-/health admin endpoints. Empty is only a
    // legal configuration alongside authDisabled_ — see setAdminToken().
    std::string adminToken_;

    // Explicit "I know this port is unauthenticated" acknowledgement.
    bool authDisabled_{false};

    // Readiness flag — starts false. Set via setReady(true) once the
    // engine has completed start-up (journal replay, warm-up, etc.).
    std::atomic<bool> ready_{false};
};

} // namespace OrderMatcher
