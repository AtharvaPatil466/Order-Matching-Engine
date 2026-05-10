#include "TcpGateway.h"
#include "MarketDataPublisher.h"
#include <iostream>
#include <csignal>
#include <thread>
#include <chrono>
#include <vector>
#include <memory>

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

int main(int argc, char* argv[]) {
    uint16_t port = 9876;
    if (argc > 1) port = static_cast<uint16_t>(std::stoi(argv[1]));

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

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

    return 0;
}
