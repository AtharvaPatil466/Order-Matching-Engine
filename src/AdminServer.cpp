#include "AdminServer.h"
#include "Metrics.h"
#include <iostream>
#include <sstream>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

namespace OrderMatcher {

AdminServer::AdminServer(MatchingEngine& engine, uint16_t port)
    : engine_(engine), port_(port) {}

AdminServer::~AdminServer() {
    stop();
}

void AdminServer::start() {
    if (running_) return;
    
    serverSocket_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket_ < 0) {
        std::cerr << "[AdminServer] Failed to create socket\n";
        return;
    }

    int opt = 1;
    setsockopt(serverSocket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);

    if (::bind(serverSocket_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[AdminServer] Failed to bind to port " << port_ << "\n";
        ::close(serverSocket_);
        serverSocket_ = -1;
        return;
    }

    if (::listen(serverSocket_, 8) < 0) {
        std::cerr << "[AdminServer] Failed to listen\n";
        ::close(serverSocket_);
        serverSocket_ = -1;
        return;
    }

    running_ = true;
    listenThread_ = std::thread(&AdminServer::listenLoop, this);
    std::cout << "[AdminServer] Listening on http://0.0.0.0:" << port_ << "\n";
}

void AdminServer::stop() {
    if (!running_) return;
    running_ = false;

    if (serverSocket_ >= 0) {
        ::shutdown(serverSocket_, SHUT_RDWR);
        ::close(serverSocket_);
        serverSocket_ = -1;
    }

    if (listenThread_.joinable()) {
        listenThread_.join();
    }
}

void AdminServer::listenLoop() {
    while (running_) {
        struct sockaddr_in clientAddr{};
        socklen_t clientLen = sizeof(clientAddr);
        int clientSock = ::accept(serverSocket_, (struct sockaddr*)&clientAddr, &clientLen);
        if (clientSock < 0) {
            if (!running_) break; // Server shutting down
            continue;
        }
        handleConnection(clientSock);
        ::close(clientSock);
    }
}

void AdminServer::handleConnection(int clientSocket) {
    char buf[2048];
    ssize_t n = ::recv(clientSocket, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return;
    buf[n] = '\0';

    // Minimal HTTP GET parser
    std::string request(buf);
    if (request.substr(0, 3) != "GET") {
        std::string resp = buildHttpResponse("{\"error\":\"Method not allowed\"}");
        ::send(clientSocket, resp.c_str(), resp.size(), 0);
        return;
    }

    // Extract path: "GET /path?query HTTP/1.1"
    size_t pathStart = 4; // skip "GET "
    size_t pathEnd = request.find(' ', pathStart);
    std::string fullPath = request.substr(pathStart, pathEnd - pathStart);

    // Split path and query string
    std::string path = fullPath;
    std::string query;
    size_t qPos = fullPath.find('?');
    if (qPos != std::string::npos) {
        path = fullPath.substr(0, qPos);
        query = fullPath.substr(qPos + 1);
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
    } else if (path == "/otr") {
        // Parse ?participantId=X
        ParticipantId pid = 0;
        size_t pidPos = query.find("participantId=");
        if (pidPos != std::string::npos) {
            pid = static_cast<ParticipantId>(std::stoul(query.substr(pidPos + 14)));
        }
        body = generateOtrResponse(pid);
    } else if (path == "/book") {
        // Parse ?symbolId=Y
        SymbolId sym = 0;
        size_t symPos = query.find("symbolId=");
        if (symPos != std::string::npos) {
            sym = static_cast<SymbolId>(std::stoul(query.substr(symPos + 9)));
        }
        body = generateBookResponse(sym);
    } else {
        body = "{\"status\":\"ok\",\"endpoints\":["
               "\"/metrics\",\"/prometheus\",\"/health\","
               "\"/otr?participantId=X\",\"/book?symbolId=Y\"]}";
    }

    std::string resp = buildHttpResponse(body, contentType);
    ::send(clientSocket, resp.c_str(), resp.size(), 0);
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

} // namespace OrderMatcher
