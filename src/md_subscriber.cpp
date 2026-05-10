#include "MarketDataPublisher.h"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <thread>
#include <csignal>

using namespace OrderMatcher;

static volatile bool running = true;

void signalHandler(int) { running = false; }

int main(int argc, char* argv[]) {
    std::string shmName = (argc > 1) ? argv[1] : "orderbook_md";

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    MarketDataSubscriber sub(shmName);

    std::cout << "Connecting to shared memory: /" << shmName << std::endl;

    // Wait for publisher to start
    while (running && !sub.connect()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!running) return 0;
    std::cout << "Connected. Listening for market data..." << std::endl;

    ShmEntry entry;
    uint64_t updateCount = 0;

    while (running) {
        if (sub.poll(entry)) {
            updateCount++;

            if (entry.type == ShmEntry::Type::IncrementalUpdate) {
                const char* action = "???";
                switch (entry.update.action) {
                    case MarketDataUpdate::Action::Add:    action = "ADD"; break;
                    case MarketDataUpdate::Action::Modify:  action = "MOD"; break;
                    case MarketDataUpdate::Action::Delete:  action = "DEL"; break;
                }
                const char* side = (entry.update.side == Side::Buy) ? "BID" : "ASK";

                std::cout << "[" << std::setw(8) << entry.sequence << "] "
                          << action << " " << side
                          << " price=" << std::fixed << std::setprecision(4) << toDouble(entry.update.level.price)
                          << " qty=" << entry.update.level.totalQuantity
                          << " orders=" << entry.update.level.orderCount
                          << std::endl;

            } else if (entry.type == ShmEntry::Type::Snapshot) {
                std::cout << "[" << std::setw(8) << entry.sequence << "] SNAPSHOT"
                          << " symbol=" << entry.symbolId
                          << " last=" << std::fixed << std::setprecision(4) << toDouble(entry.lastTradePrice)
                          << " x" << entry.lastTradeQty << std::endl;

                std::cout << "  BIDS: ";
                for (uint32_t i = 0; i < entry.bidCount; ++i) {
                    std::cout << toDouble(entry.bidLevels[i].price) << "x" << entry.bidLevels[i].totalQuantity << " ";
                }
                std::cout << std::endl;

                std::cout << "  ASKS: ";
                for (uint32_t i = 0; i < entry.askCount; ++i) {
                    std::cout << toDouble(entry.askLevels[i].price) << "x" << entry.askLevels[i].totalQuantity << " ";
                }
                std::cout << std::endl;
            }
        } else {
            // No new data — yield briefly
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    }

    std::cout << "\nReceived " << updateCount << " updates. Disconnecting." << std::endl;
    sub.disconnect();
    return 0;
}
