#include "MatchingEngine.h"
#include "AdminServer.h"
#include <iostream>
#include <thread>
#include <csignal>

using namespace OrderMatcher;

static std::atomic<bool> g_running{true};

void signalHandler(int) {
    g_running.store(false, std::memory_order_release);
}

int main(int argc, char* argv[]) {
    std::cout << "╔═══════════════════════════════════════════════════╗\n"
              << "║   Low-Latency Order Matching Engine v3.0          ║\n"
              << "║   Thread-Per-Symbol | Admin Server | Production   ║\n"
              << "╚═══════════════════════════════════════════════════╝\n\n";

    // Parse CLI args
    size_t numThreads = 4;
    uint16_t adminPort = 8080;
    size_t numSymbols = 4;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--threads" && i + 1 < argc)
            numThreads = std::stoul(argv[++i]);
        else if (arg == "--port" && i + 1 < argc)
            adminPort = static_cast<uint16_t>(std::stoul(argv[++i]));
        else if (arg == "--symbols" && i + 1 < argc)
            numSymbols = std::stoul(argv[++i]);
        else if (arg == "--help") {
            std::cout << "Usage: OrderEngine [options]\n"
                      << "  --threads N    Worker threads (default: 4)\n"
                      << "  --port P       Admin HTTP port (default: 8080)\n"
                      << "  --symbols S    Number of symbols (default: 4)\n";
            return 0;
        }
    }

    // Register signal handlers for graceful shutdown
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    // Initialize engine
    MatchingEngine engine;
    for (size_t s = 0; s < numSymbols; ++s) {
        engine.addSymbol(static_cast<SymbolId>(s));
    }

    std::cout << "[Engine] Registered " << numSymbols << " symbols\n";
    std::cout << "[Engine] Starting async mode with " << numThreads << " worker threads...\n";
    engine.startAsync(numThreads, 8192);

    // Start admin HTTP server
    AdminServer admin(engine, adminPort);
    admin.start();

    // Warmup: inject a few orders into each symbol to prime the price maps
    std::cout << "[Engine] Warming up books...\n";
    uint64_t warmupId = 1;
    for (size_t s = 0; s < numSymbols; ++s) {
        auto sym = static_cast<SymbolId>(s);
        for (int i = 0; i < 100; ++i) {
            engine.processOrder(sym, warmupId++, 1, Side::Buy, 100000 + (i % 100), 10, OrderType::Limit);
            engine.processOrder(sym, warmupId++, 2, Side::Sell, 100100 + (i % 100), 10, OrderType::Limit);
        }
    }
    engine.waitForDrain();
    std::cout << "[Engine] Warmup complete. System ready.\n\n";

    std::cout << "═══════════════════════════════════════════════════\n";
    std::cout << "  Admin server:  http://localhost:" << adminPort << "\n";
    std::cout << "  Endpoints:\n";
    std::cout << "    GET /metrics              — throughput & queue depth\n";
    std::cout << "    GET /otr?participantId=X  — order-to-trade ratio\n";
    std::cout << "    GET /book?symbolId=Y      — L2 order book snapshot\n";
    std::cout << "═══════════════════════════════════════════════════\n";
    std::cout << "Press Ctrl-C to shut down.\n\n";

    // Block until SIGINT/SIGTERM
    while (g_running.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Graceful shutdown
    std::cout << "\n[Engine] Shutting down...\n";
    admin.stop();
    engine.stopAsync();
    std::cout << "[Engine] Shutdown complete.\n";

    return 0;
}
