#pragma once

#include "MatchingEngine.h"
#include <string>
#include <thread>
#include <atomic>

namespace OrderMatcher {

class AdminServer {
public:
    AdminServer(MatchingEngine& engine, uint16_t port);
    ~AdminServer();

    void start();
    void stop();

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
    std::string generateAuditResponse(SymbolId sym) const;

    MatchingEngine& engine_;
    uint16_t port_;
    int serverSocket_{-1};
    std::atomic<bool> running_{false};
    std::thread listenThread_;
};

} // namespace OrderMatcher
