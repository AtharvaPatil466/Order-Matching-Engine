#include "AdminServer.h"
#include "LatencyTracker.h"
#include "Metrics.h"
#include "Journal.h"
#include "ReplicationProtocol.h"
#include <charconv>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <cstring>
#include <string>
#include <string_view>
#include <sys/time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

namespace {

// Parse a non-negative integer from a URL query value without
// throwing. Returns the parsed value, or `defaultVal` on any parse
// failure (empty, non-digit, overflow). std::stoul-style throwing
// parsers in HTTP handlers are a DoS vector — one malformed request
// kills the listener thread.
template <typename T>
T parseQueryUInt(std::string_view query, std::string_view key, T defaultVal = 0) {
    size_t pos = query.find(key);
    if (pos == std::string_view::npos) return defaultVal;
    pos += key.size();
    if (pos >= query.size() || query[pos] != '=') return defaultVal;
    ++pos;
    size_t end = query.find('&', pos);
    std::string_view raw = query.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos);
    if (raw.empty()) return defaultVal;
    T value = 0;
    auto [ptr, ec] = std::from_chars(raw.data(), raw.data() + raw.size(), value);
    if (ec != std::errc{} || ptr != raw.data() + raw.size()) return defaultVal;
    return value;
}

}  // namespace

namespace OrderMatcher {

AdminServer::AdminServer(MatchingEngine& engine, uint16_t port)
    : engine_(engine), port_(port) {
    if (const char* v = std::getenv("OB_CHAOS_INJECT")) {
        chaosInjectEnabled_ = (std::string(v) == "1" || std::string(v) == "true");
    }
    if (const char* t = std::getenv("OB_CHAOS_TOKEN")) {
        chaosToken_ = t;
    }
    if (chaosInjectEnabled_) {
        if (chaosToken_.empty()) {
            std::cerr << "[AdminServer] WARNING: OB_CHAOS_INJECT=1 with NO OB_CHAOS_TOKEN.\n"
                      << "             /chaos/order is callable by ANY client that can\n"
                      << "             reach the admin port. Set OB_CHAOS_TOKEN to a strong\n"
                      << "             secret in any environment beyond local development.\n";
        } else {
            std::cout << "[AdminServer] OB_CHAOS_INJECT=1 — /chaos/order endpoint ENABLED\n"
                      << "             (token auth required via X-Chaos-Token header)\n";
        }
    }
}

AdminServer::~AdminServer() {
    stop();
}

void AdminServer::start() {
    if (running_) return;
    
    int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        std::cerr << "[AdminServer] Failed to create socket\n";
        return;
    }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);

    if (::bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[AdminServer] Failed to bind to port " << port_ << "\n";
        ::close(sock);
        return;
    }

    if (::listen(sock, 8) < 0) {
        std::cerr << "[AdminServer] Failed to listen\n";
        ::close(sock);
        return;
    }

    serverSocket_.store(sock);
    running_ = true;
    listenThread_ = std::thread(&AdminServer::listenLoop, this);
    std::cout << "[AdminServer] Listening on http://0.0.0.0:" << port_ << "\n";
}

void AdminServer::stop() {
    if (!running_) return;
    running_ = false;

    // shutdown() wakes the blocked accept(); close() is what wakes it on
    // macOS (shutdown alone doesn't on listening sockets), so keep the
    // close here rather than deferring past the join.
    int sock = serverSocket_.exchange(-1);
    if (sock >= 0) {
        ::shutdown(sock, SHUT_RDWR);
        ::close(sock);
    }

    if (listenThread_.joinable()) {
        listenThread_.join();
    }
}

void AdminServer::listenLoop() {
    while (running_) {
        struct sockaddr_in clientAddr{};
        socklen_t clientLen = sizeof(clientAddr);
        int clientSock = ::accept(serverSocket_.load(), (struct sockaddr*)&clientAddr, &clientLen);
        if (clientSock < 0) {
            if (!running_) break; // Server shutting down
            continue;
        }

        // Closes slowloris DoS: a client that opens a TCP connection
        // and never sends bytes (or sends headers byte-by-byte
        // slower than the timeout) would otherwise block the
        // single-threaded listenLoop indefinitely. Timeout is
        // generous enough for any legitimate scrape/probe over a
        // healthy network.
        struct timeval tv{};
        tv.tv_sec = 2;
        tv.tv_usec = 0;
        ::setsockopt(clientSock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(clientSock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        handleConnection(clientSock);
        ::close(clientSock);
    }
}

void AdminServer::handleConnection(int clientSocket) {
    // Drain until we see the end-of-headers marker (\r\n\r\n) or the
    // request exceeds our cap. A single recv() can return less than a
    // full request line under TCP segmentation; the prior fixed-recv
    // implementation silently truncated such requests. Cap at 8 KiB —
    // we only accept GETs, which never legitimately exceed this.
    constexpr size_t kMaxRequestBytes = 8192;
    std::string request;
    request.reserve(2048);
    char buf[2048];
    bool headersComplete = false;
    while (request.size() < kMaxRequestBytes) {
        ssize_t n = ::recv(clientSocket, buf, sizeof(buf), 0);
        if (n <= 0) {
            // Closed or recv timeout (slowloris guard). Drop the
            // connection without responding.
            return;
        }
        request.append(buf, static_cast<size_t>(n));
        if (request.find("\r\n\r\n") != std::string::npos) {
            headersComplete = true;
            break;
        }
    }
    if (!headersComplete) {
        // Oversized or malformed; respond 400 and drop.
        std::string resp = buildHttpResponse("{\"error\":\"Bad request\"}");
        ::send(clientSocket, resp.c_str(), resp.size(), MSG_NOSIGNAL);
        return;
    }

    // Minimal HTTP GET parser
    if (request.compare(0, 3, "GET") != 0) {
        std::string resp = buildHttpResponse("{\"error\":\"Method not allowed\"}");
        ::send(clientSocket, resp.c_str(), resp.size(), MSG_NOSIGNAL);
        return;
    }

    // Extract path: "GET /path?query HTTP/1.1"
    size_t pathStart = 4; // skip "GET "
    if (pathStart >= request.size()) return;
    size_t pathEnd = request.find(' ', pathStart);
    if (pathEnd == std::string::npos) {
        // Malformed request line (no second space before HTTP/x.x).
        std::string resp = buildHttpResponse("{\"error\":\"Bad request\"}");
        ::send(clientSocket, resp.c_str(), resp.size(), MSG_NOSIGNAL);
        return;
    }
    std::string fullPath = request.substr(pathStart, pathEnd - pathStart);

    // Split path and query string
    std::string path = fullPath;
    std::string query;
    size_t qPos = fullPath.find('?');
    if (qPos != std::string::npos) {
        path = fullPath.substr(0, qPos);
        query = fullPath.substr(qPos + 1);
    }

    // Header extraction — currently only used for X-Chaos-Token, but
    // the same scan supports future header-based auth (X-API-Key etc).
    // Case-insensitive match on the header name; value taken verbatim
    // minus surrounding whitespace.
    auto headerValue = [&request](std::string_view name) -> std::string {
        size_t pos = 0;
        // Skip request line.
        size_t lineEnd = request.find("\r\n");
        if (lineEnd == std::string::npos) return "";
        pos = lineEnd + 2;
        while (pos < request.size()) {
            size_t end = request.find("\r\n", pos);
            if (end == std::string::npos || end == pos) break;  // blank line
            std::string_view line(&request[pos], end - pos);
            size_t colon = line.find(':');
            if (colon != std::string_view::npos) {
                std::string_view k = line.substr(0, colon);
                if (k.size() == name.size()) {
                    bool match = true;
                    for (size_t i = 0; i < k.size(); ++i) {
                        char a = k[i], b = name[i];
                        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + 32);
                        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b + 32);
                        if (a != b) { match = false; break; }
                    }
                    if (match) {
                        std::string_view v = line.substr(colon + 1);
                        while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
                        while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.remove_suffix(1);
                        return std::string(v);
                    }
                }
            }
            pos = end + 2;
        }
        return "";
    };
    const std::string chaosTokenHeader = headerValue("X-Chaos-Token");

    // Bearer token auth — enforced on all endpoints except /health and
    // /readyz so that k8s liveness and readiness probes always succeed
    // without credentials.
    if (!adminToken_.empty() && path != "/health" && path != "/readyz") {
        const std::string authHeader = headerValue("Authorization");
        const std::string expected = "Bearer " + adminToken_;
        if (authHeader != expected) {
            const std::string resp =
                "HTTP/1.1 401 Unauthorized\r\n"
                "Content-Length: 21\r\n"
                "Connection: close\r\n"
                "\r\n"
                "Unauthorized: token required";
            ::send(clientSocket, resp.c_str(), resp.size(), MSG_NOSIGNAL);
            return;
        }
    }

    std::string body;
    const char* contentType = "application/json";

    if (path == "/metrics") {
        body = generateMetricsResponse();
    } else if (path == "/prometheus") {
        // Prometheus text-exposition format. Scrape this with a
        // Prometheus server's standard scrape config:
        //   scrape_configs:
        //     - job_name: 'order-engine'
        //       static_configs: [{ targets: ['host:port'] }]
        //       metrics_path: /prometheus
        body = generatePrometheusResponse();
        contentType = "text/plain; version=0.0.4";
    } else if (path == "/health") {
        // Plain liveness probe — returns OK while the engine is
        // running. k8s livenessProbe / load balancer health-check.
        body = generateHealthResponse();
    } else if (path == "/readyz") {
        // Kubernetes readiness probe. Returns 200 {"status":"ready"}
        // once setReady(true) has been called (after journal replay,
        // warm-up, etc.), 503 {"status":"not_ready"} otherwise.
        // generateReadyzResponse() returns the full HTTP response so
        // that the 503 status code is preserved; send directly and
        // return without going through buildHttpResponse (which always
        // emits 200).
        std::string readyzResp = generateReadyzResponse();
        ::send(clientSocket, readyzResp.c_str(), readyzResp.size(), MSG_NOSIGNAL);
        return;
    } else if (path == "/otr") {
        ParticipantId pid = parseQueryUInt<ParticipantId>(query, "participantId");
        body = generateOtrResponse(pid);
    } else if (path == "/book") {
        SymbolId sym = parseQueryUInt<SymbolId>(query, "symbolId");
        body = generateBookResponse(sym);
    } else if (path == "/auction") {
        SymbolId sym = parseQueryUInt<SymbolId>(query, "symbolId");
        body = generateAuctionResponse(sym);
    } else if (path == "/fees") {
        ParticipantId pid = parseQueryUInt<ParticipantId>(query, "participantId");
        body = generateFeesResponse(pid);
    } else if (path == "/risk") {
        uint32_t tier = parseQueryUInt<uint32_t>(query, "tier");
        uint64_t ent = parseQueryUInt<uint64_t>(query, "entityId");
        body = generateRiskResponse(tier, ent);
    } else if (path == "/cat") {
        body = generateCatResponse();
    } else if (path == "/audit") {
        SymbolId sym = parseQueryUInt<SymbolId>(query, "symbolId");
        body = generateAuditResponse(sym);
    } else if (path == "/role") {
        body = generateRoleResponse();
    } else if (path == "/replication") {
        body = generateReplicationResponse();
    } else if (path == "/journal/head") {
        body = generateJournalHeadResponse();
    } else if (path == "/version") {
        body = generateVersionResponse();
    } else if (path == "/chaos/order") {
        body = generateChaosOrderResponse(query, chaosTokenHeader);
    } else {
        body = "{\"status\":\"ok\",\"endpoints\":["
               "\"/metrics\",\"/prometheus\",\"/health\",\"/readyz\","
               "\"/otr?participantId=X\",\"/book?symbolId=Y\","
               "\"/auction?symbolId=Y\",\"/audit?symbolId=Y\","
               "\"/fees?participantId=X\",\"/risk?tier=T&entityId=E\",\"/cat\","
               "\"/role\",\"/replication\","
               "\"/journal/head\",\"/version\"]}";
    }

    std::string resp = buildHttpResponse(body, contentType);
    ::send(clientSocket, resp.c_str(), resp.size(), MSG_NOSIGNAL);
}

std::string AdminServer::buildHttpResponse(const std::string& body,
                                           const char* contentType) const {
    std::ostringstream oss;
    oss << "HTTP/1.1 200 OK\r\n"
        << "Content-Type: " << contentType << "\r\n"
        << "Access-Control-Allow-Origin: *\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n"
        << "\r\n"
        << body;
    return oss.str();
}

std::string AdminServer::generatePrometheusResponse() const {
    return MetricsRegistry::instance().exportPrometheus();
}

std::string AdminServer::generateHealthResponse() const {
    // Simple liveness signal. A future refinement could surface a
    // status that distinguishes "starting" / "running" / "draining".
    return "{\"status\":\"ok\"}";
}

std::string AdminServer::generateReadyzResponse() const {
    // Returns the full HTTP response (not just a body) so the caller
    // can send a 503 without going through buildHttpResponse, which
    // always emits a 200.
    if (ready_.load(std::memory_order_acquire)) {
        return buildHttpResponse("{\"status\":\"ready\"}");
    }
    const std::string body = "{\"status\":\"not_ready\"}";
    return "HTTP/1.1 503 Service Unavailable\r\n"
           "Content-Type: application/json\r\n"
           "Content-Length: " + std::to_string(body.size()) + "\r\n"
           "Connection: close\r\n"
           "\r\n" + body;
}

std::string AdminServer::generateMetricsResponse() const {
    LatencyTracker e2e = engine_.getAggregateE2ELatency();
    std::ostringstream oss;
    oss << "{";
    oss << "\"submitted\":" << engine_.getSubmittedCount() << ",";
    oss << "\"processed\":" << engine_.getProcessedCount() << ",";
    oss << "\"rateLimited\":" << engine_.getRateLimitedCount() << ",";
    oss << "\"backpressureRejected\":" << engine_.getBackpressureRejectCount() << ",";
    oss << "\"queueDropped\":" << engine_.getDroppedCount() << ",";
    
    // Queue depth per thread
    uint64_t pending = engine_.getSubmittedCount() - engine_.getProcessedCount();
    oss << "\"pendingOrders\":" << pending << ",";
    oss << "\"e2eLatency\":{";
    oss << "\"count\":" << e2e.getCount() << ",";
    oss << "\"meanNs\":" << static_cast<uint64_t>(e2e.getMean()) << ",";
    oss << "\"p50Ns\":" << e2e.getP50() << ",";
    oss << "\"p99Ns\":" << e2e.getP99() << ",";
    oss << "\"p999Ns\":" << e2e.getP999() << ",";
    oss << "\"maxNs\":" << e2e.getMax();
    oss << "}";
    oss << "}";
    return oss.str();
}

std::string AdminServer::generateOtrResponse(ParticipantId pid) const {
    std::ostringstream oss;
    oss << "{\"participantId\":" << pid << ",";
    
    // Aggregate OTR stats across all books
    // Note: In lean mode, OTR stats are disabled
#ifndef OB_LEAN_MODE
    // Try to get stats from all known books
    uint64_t totalOrders = 0, totalTrades = 0, totalRejected = 0;
    int64_t totalPosition = 0;
    
    // We access only the default book for now — a production system would aggregate
    auto* book = engine_.getOrderBook(0);
    if (book) {
        auto stats = book->getParticipantStats(pid);
        totalOrders = stats.ordersSubmitted;
        totalTrades = stats.tradesExecuted;
        totalRejected = stats.rejectedOrders;
        totalPosition = stats.netPosition;
    }

    double otr = static_cast<double>(totalOrders) / std::max(static_cast<uint64_t>(1), totalTrades);
    oss << "\"ordersSubmitted\":" << totalOrders << ",";
    oss << "\"tradesExecuted\":" << totalTrades << ",";
    oss << "\"rejectedOrders\":" << totalRejected << ",";
    oss << "\"netPosition\":" << totalPosition << ",";
    oss << "\"otr\":" << otr;
#else
    oss << "\"note\":\"OTR stats disabled in lean mode\"";
#endif
    oss << "}";
    return oss.str();
}

std::string AdminServer::generateBookResponse(SymbolId sym) const {
    MarketDataSnapshot snap = engine_.getSnapshot(sym, 10);
    
    std::ostringstream oss;
    oss << "{\"symbolId\":" << sym << ",";
    oss << "\"lastTradePrice\":" << snap.lastTradePrice << ",";
    oss << "\"lastTradeQty\":" << snap.lastTradeQty << ",";
    
    // Bids
    oss << "\"bids\":[";
    for (size_t i = 0; i < snap.bidCount; ++i) {
        if (i > 0) oss << ",";
        oss << "{\"price\":" << snap.bids[i].price
            << ",\"qty\":" << snap.bids[i].totalQuantity
            << ",\"orders\":" << snap.bids[i].orderCount << "}";
    }
    oss << "],";
    
    // Asks
    oss << "\"asks\":[";
    for (size_t i = 0; i < snap.askCount; ++i) {
        if (i > 0) oss << ",";
        oss << "{\"price\":" << snap.asks[i].price
            << ",\"qty\":" << snap.asks[i].totalQuantity
            << ",\"orders\":" << snap.asks[i].orderCount << "}";
    }
    oss << "]}";
    
    return oss.str();
}
std::string AdminServer::generateAuditResponse(SymbolId sym) const {
    MarketDataSnapshot snap = engine_.getSnapshot(sym, 5);

    std::ostringstream oss;
    oss << "{\"symbolId\":" << sym << ",";
    oss << "\"timestamp\":" << nowNs() << ",";
    oss << "\"bestBid\":" << (snap.bidCount > 0 ? snap.bids[0].price : 0) << ",";
    oss << "\"bestAsk\":" << (snap.askCount > 0 ? snap.asks[0].price : 0) << ",";

    // Spread
    Price bestBid = (snap.bidCount > 0) ? snap.bids[0].price : 0;
    Price bestAsk = (snap.askCount > 0) ? snap.asks[0].price : 0;
    Price spread = (bestBid > 0 && bestAsk > 0) ? bestAsk - bestBid : 0;
    oss << "\"spread\":" << spread << ",";

    // Last trade
    oss << "\"lastTradePrice\":" << snap.lastTradePrice << ",";
    oss << "\"lastTradeQty\":" << snap.lastTradeQty << ",";

    // 5-level depth
    oss << "\"depth\":{\"bids\":[";
    for (size_t i = 0; i < snap.bidCount && i < 5; ++i) {
        if (i > 0) oss << ",";
        oss << "{\"price\":" << snap.bids[i].price
            << ",\"qty\":" << snap.bids[i].totalQuantity
            << ",\"orders\":" << snap.bids[i].orderCount << "}";
    }
    oss << "],\"asks\":[";
    for (size_t i = 0; i < snap.askCount && i < 5; ++i) {
        if (i > 0) oss << ",";
        oss << "{\"price\":" << snap.asks[i].price
            << ",\"qty\":" << snap.asks[i].totalQuantity
            << ",\"orders\":" << snap.asks[i].orderCount << "}";
    }
    oss << "]},";

    // Active order count (bid levels + ask levels)
    const OrderBook* book = engine_.getOrderBook(sym);
    size_t bidLevels = book ? book->getBidLevelsCount() : 0;
    size_t askLevels = book ? book->getAskLevelsCount() : 0;
    oss << "\"bidLevels\":" << bidLevels << ",";
    oss << "\"askLevels\":" << askLevels;
    oss << "}";

    return oss.str();
}

std::string AdminServer::generateAuctionResponse(SymbolId sym) const {
    std::ostringstream oss;
    const OrderBook* book = engine_.getOrderBook(sym);
    if (!book) {
        oss << "{\"symbolId\":" << sym << ",\"error\":\"unknown symbol\"}";
        return oss.str();
    }
    auto stateName = [](TradingState s) -> const char* {
        switch (s) {
        case TradingState::Continuous:        return "Continuous";
        case TradingState::Halted:            return "Halted";
        case TradingState::AuctionOpen:       return "AuctionOpen";
        case TradingState::AuctionClose:      return "AuctionClose";
        case TradingState::PreOpen:           return "PreOpen";
        case TradingState::PostClose:         return "PostClose";
        case TradingState::VolatilityAuction: return "VolatilityAuction";
        }
        return "Unknown";
    };
    // NASDAQ NOII analogue: indicative clearing price + paired volume and the
    // order imbalance, computed non-destructively over the current book.
    // Meaningful during auction states; mid-continuous it answers "what would
    // cross right now".
    const AuctionResult a = book->computeAuctionState();
    oss << "{"
        << "\"symbolId\":" << sym << ","
        << "\"tradingState\":\"" << stateName(book->getTradingState()) << "\","
        << "\"hasCross\":" << (a.hasCross ? "true" : "false") << ","
        << "\"indicativePrice\":" << a.indicativePrice << ","
        << "\"pairedVolume\":" << a.pairedVolume << ","
        << "\"imbalanceQty\":" << a.imbalanceQty << ","
        << "\"imbalanceSide\":\"" << (a.imbalanceSide == Side::Buy ? "buy" : "sell") << "\""
        << "}";
    return oss.str();
}

std::string AdminServer::generateFeesResponse(ParticipantId pid) const {
    std::ostringstream oss;
    oss << "{\"participantId\":" << pid
        << ",\"accruedFee\":" << engine_.accruedFee(pid) << "}";
    return oss.str();
}

std::string AdminServer::generateRiskResponse(uint32_t tier, uint64_t entityId) const {
    std::ostringstream oss;
    if (tier > 3) {
        oss << "{\"error\":\"tier must be 0=Trader,1=Strategy,2=Account,3=Firm\"}";
        return oss.str();
    }
    static const char* kTierNames[] = {"Trader", "Strategy", "Account", "Firm"};
    oss << "{\"tier\":\"" << kTierNames[tier] << "\",\"entityId\":" << entityId
        << ",\"netPosition\":"
        << engine_.riskNetPosition(static_cast<RiskTier>(tier), entityId) << "}";
    return oss.str();
}

std::string AdminServer::generateCatResponse() const {
    std::ostringstream oss;
    oss << "{\"auditRecordCount\":" << engine_.auditRecordCount() << "}";
    return oss.str();
}

std::string AdminServer::generateRoleResponse() const {
    std::ostringstream oss;
    if (!coord_) {
        // Replication not configured for this engine — report
        // standalone explicitly so chaos harnesses / operators can
        // distinguish from "coordinator exists but is down".
        oss << "{\"role\":\"standalone\",\"replicationEnabled\":false}";
        return oss.str();
    }
    const char* role = (coord_->role() == NodeRole::Primary) ? "primary" : "backup";
    oss << "{"
        << "\"role\":\"" << role << "\","
        << "\"replicationEnabled\":true,"
        << "\"epoch\":" << coord_->epoch() << ","
        << "\"isLeader\":" << (coord_->isLeader() ? "true" : "false") << ","
        << "\"running\":" << (coord_->isRunning() ? "true" : "false")
        << "}";
    return oss.str();
}

std::string AdminServer::generateReplicationResponse() const {
    std::ostringstream oss;
    if (!coord_) {
        oss << "{\"replicationEnabled\":false}";
        return oss.str();
    }
    oss << "{"
        << "\"replicationEnabled\":true,"
        << "\"peerAlive\":" << (coord_->isPeerAlive() ? "true" : "false") << ","
        << "\"running\":" << (coord_->isRunning() ? "true" : "false") << ","
        << "\"epoch\":" << coord_->epoch()
        << "}";
    return oss.str();
}

std::string AdminServer::generateVersionResponse() const {
    // Compile-time constants set by CMake. The #ifndefs guard against
    // unusual build paths (e.g. integration into a parent build that
    // doesn't define them).
#ifndef OB_GIT_SHA
#define OB_GIT_SHA "unknown"
#endif
#ifndef OB_BUILD_TIME
#define OB_BUILD_TIME "unknown"
#endif
    std::ostringstream oss;
    oss << "{"
        << "\"gitSha\":\"" << OB_GIT_SHA << "\","
        << "\"buildTime\":\"" << OB_BUILD_TIME << "\","
        << "\"engineVersion\":\"3.0\""
        << "}";
    return oss.str();
}

std::string AdminServer::generateJournalHeadResponse() const {
    std::ostringstream oss;
    Journal* j = engine_.getJournal();
    if (!j) {
        oss << "{\"journalEnabled\":false}";
        return oss.str();
    }
    oss << "{\"journalEnabled\":true,\"sequence\":" << j->getSequence() << "}";
    return oss.str();
}

std::string AdminServer::generateChaosOrderResponse(const std::string& query,
                                                    const std::string& token) {
    // Hard gate: must have been enabled at process start via env var.
    // Returns 200 with {"enabled":false} rather than a 4xx so chaos
    // tests can detect "endpoint exists but isn't armed" cleanly.
    if (!chaosInjectEnabled_) {
        return "{\"enabled\":false,\"reason\":\"OB_CHAOS_INJECT not set\"}";
    }
    // Token check: when OB_CHAOS_TOKEN is configured, the request
    // must carry a matching X-Chaos-Token header. Empty server-side
    // token means no auth (warned at startup) — allow.
    if (!chaosToken_.empty() && token != chaosToken_) {
        return "{\"enabled\":true,\"accepted\":false,\"error\":\"unauthorized\"}";
    }

    auto getU = [&query](std::string_view key, uint64_t def) -> uint64_t {
        return parseQueryUInt<uint64_t>(query, key, def);
    };
    auto getU32 = [&query](std::string_view key, uint32_t def) -> uint32_t {
        return parseQueryUInt<uint32_t>(query, key, def);
    };

    OrderId orderId        = getU("orderId",       0);
    ParticipantId pid      = getU32("participantId", 0);
    SymbolId symbolId      = getU32("symbolId",    0);
    Price price            = getU("price",         0);
    Quantity qty           = getU("qty",           0);
    // side: 0=Buy, 1=Sell (matches Side enum)
    uint32_t sideRaw       = getU32("side",        0);

    if (orderId == 0 || pid == 0 || price == 0 || qty == 0) {
        return "{\"enabled\":true,\"accepted\":false,"
               "\"error\":\"missing required field (orderId, participantId, price, qty)\"}";
    }

    Side side = (sideRaw == 0) ? Side::Buy : Side::Sell;
    SubmitResult r = engine_.submitOrder(
        symbolId, orderId, pid, side, price, qty, OrderType::Limit);

    std::ostringstream oss;
    oss << "{\"enabled\":true,"
        << "\"accepted\":" << (r.isAccepted() ? "true" : "false") << ","
        << "\"sequenceId\":" << r.sequenceId << ","
        << "\"rejectReason\":" << static_cast<int>(r.rejectReason)
        << "}";
    return oss.str();
}

} // namespace OrderMatcher
