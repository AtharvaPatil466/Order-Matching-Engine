#include "TcpGateway.h"
#include <cassert>
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>

using namespace OrderMatcher;

// ─── Helper: TCP client ─────────────────────────────────────────────────────

class TestClient {
public:
    TestClient() : fd_(-1) {}
    ~TestClient() { disconnect(); }

    bool connect(uint16_t port) {
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) return false;

        int flag = 1;
        setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

        if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            close(fd_); fd_ = -1;
            return false;
        }
        return true;
    }

    void disconnect() {
        if (fd_ >= 0) { close(fd_); fd_ = -1; }
    }

    bool sendOrder(const OrderRequest& req) {
        uint32_t len = htonl(sizeof(OrderRequest));
        if (!sendAll(&len, sizeof(len))) return false;
        return sendAll(&req, sizeof(req));
    }

    bool sendOrderV2(const OrderRequest& req, uint64_t clientRequestId) {
        GatewayRequestV2 frame{};
        frame.header.clientRequestId = clientRequestId;
        frame.request = req;
        uint32_t len = htonl(sizeof(frame));
        return sendAll(&len, sizeof(len)) && sendAll(&frame, sizeof(frame));
    }

    bool sendUnsupportedV2Version(const OrderRequest& req) {
        GatewayRequestV2 frame{};
        frame.header.version = 99;
        frame.request = req;
        uint32_t len = htonl(sizeof(frame));
        return sendAll(&len, sizeof(len)) && sendAll(&frame, sizeof(frame));
    }

    bool recvResponse(GatewayResponse& resp) {
        uint32_t len;
        if (!recvAll(&len, sizeof(len))) return false;
        len = ntohl(len);
        if (len != sizeof(GatewayResponse)) return false;
        return recvAll(&resp, sizeof(resp));
    }

    bool recvResponseV2(GatewayResponseV2& resp) {
        uint32_t len;
        if (!recvAll(&len, sizeof(len))) return false;
        len = ntohl(len);
        if (len != sizeof(GatewayResponseV2)) return false;
        return recvAll(&resp, sizeof(resp));
    }

    int fd() const { return fd_; }

private:
    bool sendAll(const void* buf, size_t len) {
        auto* ptr = static_cast<const char*>(buf);
        while (len > 0) {
            ssize_t n = send(fd_, ptr, len, 0);
            if (n <= 0) return false;
            ptr += n; len -= static_cast<size_t>(n);
        }
        return true;
    }

    bool recvAll(void* buf, size_t len) {
        auto* ptr = static_cast<char*>(buf);
        while (len > 0) {
            ssize_t n = recv(fd_, ptr, len, 0);
            if (n <= 0) return false;
            ptr += n; len -= static_cast<size_t>(n);
        }
        return true;
    }

    int fd_;
};

// ─── Test 1: Connect, send orders, receive ACKs, verify trade ───────────────

void testBasicOrderFlow() {
    std::cout << "Running testBasicOrderFlow..." << std::endl;

    MatchingEngine engine;
    engine.start();
    engine.addSymbol(0);

    std::atomic<int> tradeCount{0};
    auto* book = engine.getOrderBook(0);
    struct TradeCounter : EventListener {
        std::atomic<int>& count;
        TradeCounter(std::atomic<int>& c) : count(c) {}
        void onTrade(const Trade&) override { count.fetch_add(1, std::memory_order_relaxed); }
    };
    TradeCounter counter(tradeCount);
    book->setEventListener(&counter);

    TcpGateway gateway(engine);
    assert(gateway.start(0)); // port 0 = OS picks a free port
    uint16_t port = gateway.port();
    assert(port > 0);

    // Give event loop a moment to start
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Connect client
    TestClient client;
    assert(client.connect(port));

    // Give kqueue time to register the new connection
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    assert(gateway.clientCount() == 1);

    // Send a resting sell order
    OrderRequest req1{};
    req1.type = OrderRequest::Type::NewOrder;
    req1.symbolId = 0;
    req1.orderId = 1;
    req1.participantId = 100;
    req1.side = Side::Sell;
    req1.price = toPrice(100.00);
    req1.qty = 50;
    req1.orderType = OrderType::Limit;

    assert(client.sendOrder(req1));

    GatewayResponse resp1;
    assert(client.recvResponse(resp1));
    assert(resp1.type == GatewayResponse::Type::Ack);
    assert(resp1.orderId == 1);

    // Send a crossing buy order — should trigger trade
    OrderRequest req2{};
    req2.type = OrderRequest::Type::NewOrder;
    req2.symbolId = 0;
    req2.orderId = 2;
    req2.participantId = 200;
    req2.side = Side::Buy;
    req2.price = toPrice(100.00);
    req2.qty = 30;
    req2.orderType = OrderType::Limit;

    assert(client.sendOrder(req2));

    GatewayResponse resp2;
    assert(client.recvResponse(resp2));
    assert(resp2.type == GatewayResponse::Type::Ack);
    assert(resp2.orderId == 2);

    // Verify trade happened
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    assert(tradeCount.load() == 1);

    // Verify book state: sell order should have 20 remaining
    const Order* sell = book->getOrder(1);
    assert(sell != nullptr);
    assert(sell->remainingQty == 20);

    // Buy order should be fully filled (gone from book)
    const Order* buy = book->getOrder(2);
    assert(buy == nullptr);

    client.disconnect();
    gateway.stop();
    engine.stop();

    std::cout << "testBasicOrderFlow PASSED" << std::endl;
}

// ─── Test 2: Cancel order via gateway ───────────────────────────────────────

void testCancelViaGateway() {
    std::cout << "Running testCancelViaGateway..." << std::endl;

    MatchingEngine engine;
    engine.start();
    engine.addSymbol(0);

    TcpGateway gateway(engine);
    assert(gateway.start(0));

    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    TestClient client;
    assert(client.connect(gateway.port()));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Send a resting order
    OrderRequest req{};
    req.type = OrderRequest::Type::NewOrder;
    req.symbolId = 0;
    req.orderId = 1;
    req.participantId = 100;
    req.side = Side::Buy;
    req.price = toPrice(99.00);
    req.qty = 50;
    req.orderType = OrderType::Limit;

    assert(client.sendOrder(req));
    GatewayResponse resp;
    assert(client.recvResponse(resp));
    assert(resp.type == GatewayResponse::Type::Ack);

    // Verify order exists
    auto* book = engine.getOrderBook(0);
    assert(book->getOrder(1) != nullptr);

    // Cancel it
    OrderRequest cancel{};
    cancel.type = OrderRequest::Type::Cancel;
    cancel.symbolId = 0;
    cancel.orderId = 1;

    assert(client.sendOrder(cancel));
    assert(client.recvResponse(resp));
    assert(resp.type == GatewayResponse::Type::Ack);

    // Verify order is gone
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    assert(book->getOrder(1) == nullptr);

    client.disconnect();
    gateway.stop();
    engine.stop();

    std::cout << "testCancelViaGateway PASSED" << std::endl;
}

// ─── Test 3: Multiple clients ───────────────────────────────────────────────

void testMultipleClients() {
    std::cout << "Running testMultipleClients..." << std::endl;

    MatchingEngine engine;
    engine.start();
    engine.addSymbol(0);

    std::atomic<int> tradeCount{0};
    auto* book = engine.getOrderBook(0);
    struct TradeCounter : EventListener {
        std::atomic<int>& count;
        TradeCounter(std::atomic<int>& c) : count(c) {}
        void onTrade(const Trade&) override { count.fetch_add(1, std::memory_order_relaxed); }
    };
    TradeCounter counter(tradeCount);
    book->setEventListener(&counter);

    TcpGateway gateway(engine);
    assert(gateway.start(0));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    TestClient client1, client2;
    assert(client1.connect(gateway.port()));
    assert(client2.connect(gateway.port()));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Client 1: sell
    OrderRequest sell{};
    sell.type = OrderRequest::Type::NewOrder;
    sell.symbolId = 0;
    sell.orderId = 1;
    sell.participantId = 100;
    sell.side = Side::Sell;
    sell.price = toPrice(100.00);
    sell.qty = 100;
    sell.orderType = OrderType::Limit;

    assert(client1.sendOrder(sell));
    GatewayResponse resp;
    assert(client1.recvResponse(resp));
    assert(resp.type == GatewayResponse::Type::Ack);

    // Client 2: crossing buy
    OrderRequest buy{};
    buy.type = OrderRequest::Type::NewOrder;
    buy.symbolId = 0;
    buy.orderId = 2;
    buy.participantId = 200;
    buy.side = Side::Buy;
    buy.price = toPrice(100.00);
    buy.qty = 60;
    buy.orderType = OrderType::Limit;

    assert(client2.sendOrder(buy));
    assert(client2.recvResponse(resp));
    assert(resp.type == GatewayResponse::Type::Ack);

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    assert(tradeCount.load() == 1);

    // Sell order partially filled
    assert(book->getOrder(1)->remainingQty == 40);

    client1.disconnect();
    client2.disconnect();
    gateway.stop();
    engine.stop();

    std::cout << "testMultipleClients PASSED" << std::endl;
}

// ─── Test 4: IP whitelist ───────────────────────────────────────────────────

void testIPWhitelist() {
    std::cout << "Running testIPWhitelist..." << std::endl;

    MatchingEngine engine;
    engine.start();

    TcpGateway gateway(engine);
    // Only allow a non-localhost IP — localhost connections should be rejected
    gateway.addAllowedIP("10.0.0.1");
    assert(gateway.start(0));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Try connecting from localhost — should be rejected
    TestClient client;
    bool connected = client.connect(gateway.port());
    if (connected) {
        // Connection may succeed at TCP level but gateway closes it
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        // Try sending data — should fail since gateway closed the connection
        OrderRequest req{};
        req.type = OrderRequest::Type::NewOrder;
        req.orderId = 1;
        bool sent = client.sendOrder(req);
        GatewayResponse resp;
        bool recvd = sent && client.recvResponse(resp);
        // If we managed to send and receive, check that no client is registered
        // The key point: the gateway should have rejected us
        assert(!recvd || gateway.clientCount() == 0);
    }

    client.disconnect();

    // Now allow localhost
    gateway.stop();
    TcpGateway gateway2(engine);
    gateway2.addAllowedIP("127.0.0.1");
    assert(gateway2.start(0));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    TestClient client2;
    assert(client2.connect(gateway2.port()));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    assert(gateway2.clientCount() == 1);

    client2.disconnect();
    gateway2.stop();
    engine.stop();

    std::cout << "testIPWhitelist PASSED" << std::endl;
}

// ─── Test 5: Pipelined messages ─────────────────────────────────────────────

void testPipelinedMessages() {
    std::cout << "Running testPipelinedMessages..." << std::endl;

    MatchingEngine engine;
    engine.start();
    engine.addSymbol(0);

    TcpGateway gateway(engine);
    assert(gateway.start(0));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    TestClient client;
    assert(client.connect(gateway.port()));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Send 10 orders back-to-back without waiting for responses
    constexpr int N = 10;
    for (int i = 0; i < N; ++i) {
        OrderRequest req{};
        req.type = OrderRequest::Type::NewOrder;
        req.symbolId = 0;
        req.orderId = static_cast<OrderId>(i + 1);
        req.participantId = 100;
        req.side = Side::Buy;
        req.price = toPrice(90.00 + (0.01 * i));
        req.qty = 10;
        req.orderType = OrderType::Limit;
        assert(client.sendOrder(req));
    }

    // Now read all N responses
    for (int i = 0; i < N; ++i) {
        GatewayResponse resp;
        assert(client.recvResponse(resp));
        assert(resp.type == GatewayResponse::Type::Ack);
        assert(resp.orderId == static_cast<OrderId>(i + 1));
    }

    client.disconnect();
    gateway.stop();
    engine.stop();

    std::cout << "testPipelinedMessages PASSED" << std::endl;
}

// ─── Test 6: gateway returns explicit reject details ────────────────────────

void testGatewayRejectResponse() {
    std::cout << "Running testGatewayRejectResponse..." << std::endl;

    MatchingEngine engine;
    engine.start();

    TcpGateway gateway(engine);
    assert(gateway.start(0));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    TestClient client;
    assert(client.connect(gateway.port()));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    OrderRequest req{};
    req.type = OrderRequest::Type::NewOrder;
    req.symbolId = 404;
    req.orderId = 404;
    req.participantId = 100;
    req.side = Side::Buy;
    req.price = toPrice(99.00);
    req.qty = 10;
    req.orderType = OrderType::Limit;

    assert(client.sendOrder(req));
    GatewayResponse resp{};
    assert(client.recvResponse(resp));
    assert(resp.type == GatewayResponse::Type::Error);
    assert(resp.orderId == 404);
    assert(resp.rejectReason == RejectReason::SymbolNotFound);
    assert(std::strlen(resp.errorMessage) > 0);

    client.disconnect();
    gateway.stop();
    engine.stop();

    std::cout << "testGatewayRejectResponse PASSED" << std::endl;
}

// ─── Test 7: versioned binary protocol envelope ────────────────────────────

void testGatewayProtocolV2Envelope() {
    std::cout << "Running testGatewayProtocolV2Envelope..." << std::endl;

    MatchingEngine engine;
    engine.start();
    engine.addSymbol(0);

    TcpGateway gateway(engine);
    assert(gateway.start(0));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    TestClient client;
    assert(client.connect(gateway.port()));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    OrderRequest req{};
    req.type = OrderRequest::Type::NewOrder;
    req.symbolId = 0;
    req.orderId = 9001;
    req.participantId = 100;
    req.side = Side::Buy;
    req.price = toPrice(101.25);
    req.qty = 25;
    req.orderType = OrderType::Limit;

    constexpr uint64_t clientRequestId = 0xABCDEF1234567890ULL;
    assert(client.sendOrderV2(req, clientRequestId));

    GatewayResponseV2 resp{};
    assert(client.recvResponseV2(resp));
    assert(resp.header.magic == GATEWAY_PROTOCOL_MAGIC);
    assert(resp.header.version == GATEWAY_PROTOCOL_V2);
    assert(resp.header.headerSize == sizeof(GatewayResponseHeader));
    assert(resp.header.payloadSize == sizeof(GatewayResponse));
    assert(resp.header.clientRequestId == clientRequestId);
    assert(resp.response.type == GatewayResponse::Type::Ack);
    assert(resp.response.orderId == 9001);

    client.disconnect();
    gateway.stop();
    engine.stop();

    std::cout << "testGatewayProtocolV2Envelope PASSED" << std::endl;
}

void testGatewayProtocolRejectsUnsupportedVersion() {
    std::cout << "Running testGatewayProtocolRejectsUnsupportedVersion..." << std::endl;

    MatchingEngine engine;
    engine.start();
    engine.addSymbol(0);

    TcpGateway gateway(engine);
    assert(gateway.start(0));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    TestClient client;
    assert(client.connect(gateway.port()));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    OrderRequest req{};
    req.type = OrderRequest::Type::NewOrder;
    req.symbolId = 0;
    req.orderId = 9002;
    req.participantId = 100;
    req.side = Side::Buy;
    req.price = toPrice(101.25);
    req.qty = 25;
    req.orderType = OrderType::Limit;

    assert(client.sendUnsupportedV2Version(req));
    GatewayResponse resp{};
    assert(client.recvResponse(resp));
    assert(resp.type == GatewayResponse::Type::Error);
    assert(std::strstr(resp.errorMessage, "Unsupported gateway protocol version") != nullptr);

    // The malformed version must be rejected before reaching the engine.
    auto* book = engine.getOrderBook(0);
    assert(book != nullptr);
    assert(book->getOrder(9002) == nullptr);

    client.disconnect();
    gateway.stop();
    engine.stop();

    std::cout << "testGatewayProtocolRejectsUnsupportedVersion PASSED" << std::endl;
}

int main() {
    std::cout << "\n=== Gateway Integration Tests ===" << std::endl;

    testBasicOrderFlow();
    testCancelViaGateway();
    testMultipleClients();
    testIPWhitelist();
    testPipelinedMessages();
    testGatewayRejectResponse();
    testGatewayProtocolV2Envelope();
    testGatewayProtocolRejectsUnsupportedVersion();

    std::cout << "\nALL GATEWAY INTEGRATION TESTS PASSED!" << std::endl;
    return 0;
}
