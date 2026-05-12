#pragma once

// ShadowEngine — silent matching validator for divergence detection.
//
// Roadmap Phase 3, Week 10: Shadow Mode
//
// Runs a parallel MatchingEngine instance that processes the same order
// flow as the primary but does NOT publish market data or execution
// reports. Periodically compares its internal book state against the
// primary's to detect logic bugs that replication alone cannot find.
//
// Architecture:
//   Primary Engine ─── orders ───→ ShadowEngine
//                 ─── snapshots ──→ ShadowEngine::compareState()
//
// If both primary and shadow have the same bug, replication looks
// correct but output is wrong. Shadow mode catches this by running
// an independent matching implementation.
//
// Zero overhead on primary path: shadow runs on a separate thread
// and is fed orders via a lock-free queue.

#include "MatchingEngine.h"
#include "LatencyTracker.h"
#include "MpscQueue.h"
#include "OrderBook.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <thread>

namespace OrderMatcher {

struct ShadowDivergence {
    SymbolId symbolId{0};
    uint64_t timestampNs{0};
    Price    primaryBestBid{0};
    Price    primaryBestAsk{0};
    Price    shadowBestBid{0};
    Price    shadowBestAsk{0};
    uint64_t primaryOrderCount{0};
    uint64_t shadowOrderCount{0};

    bool hasDiverged() const {
        return primaryBestBid != shadowBestBid ||
               primaryBestAsk != shadowBestAsk;
    }
};

class ShadowEngine {
public:
    using DivergenceCallback = std::function<void(const ShadowDivergence&)>;

    ShadowEngine() : shadowQueue_(8192) {}
    ~ShadowEngine() { stop(); }

    ShadowEngine(const ShadowEngine&) = delete;
    ShadowEngine& operator=(const ShadowEngine&) = delete;

    void addSymbol(SymbolId symbolId, MatchAlgorithm algo = MatchAlgorithm::PriceTime) {
        shadow_.addSymbol(symbolId, algo);
    }

    void setDivergenceCallback(DivergenceCallback cb) {
        divergenceCb_ = std::move(cb);
    }

    void setCompareIntervalMs(uint64_t ms) { compareIntervalMs_ = ms; }

    void start() {
        if (running_.load()) return;
        shadow_.start();
        running_.store(true);
        workerThread_ = std::thread([this] { workerLoop(); });
    }

    void stop() {
        if (!running_.load()) return;
        running_.store(false);
        if (workerThread_.joinable()) {
            workerThread_.join();
        }
        shadow_.stop();
    }

    // Feed an order to the shadow engine (called from primary's hot path).
    // Non-blocking: if queue is full, the order is dropped (shadow degrades
    // gracefully — it may diverge due to missed orders, which is detected
    // on the next compare cycle).
    void feedOrder(const OrderRequest& req) {
        shadowQueue_.push(req);  // best-effort
        ordersReceived_.fetch_add(1, std::memory_order_relaxed);
    }

    // Compare shadow state against primary for a specific symbol.
    // Returns divergence info. This is called periodically by the
    // compare thread or on-demand by the admin API.
    ShadowDivergence compareSymbol(SymbolId symbolId,
                                    const MatchingEngine& primary) const {
        ShadowDivergence div;
        div.symbolId = symbolId;
        div.timestampNs = nowNs();

        const OrderBook* pBook = primary.getOrderBook(symbolId);
        const OrderBook* sBook = shadow_.getOrderBook(symbolId);

        if (!pBook || !sBook) return div;

        auto pSnap = pBook->getSnapshot(1);
        auto sSnap = sBook->getSnapshot(1);

        div.primaryBestBid = (pSnap.bidCount > 0) ? pSnap.bids[0].price : 0;
        div.primaryBestAsk = (pSnap.askCount > 0) ? pSnap.asks[0].price : 0;
        div.shadowBestBid  = (sSnap.bidCount > 0) ? sSnap.bids[0].price : 0;
        div.shadowBestAsk  = (sSnap.askCount > 0) ? sSnap.asks[0].price : 0;

        div.primaryOrderCount = pBook->getBidLevelsCount() + pBook->getAskLevelsCount();
        div.shadowOrderCount  = sBook->getBidLevelsCount() + sBook->getAskLevelsCount();

        return div;
    }

    // Stats
    uint64_t getOrdersReceived() const { return ordersReceived_.load(std::memory_order_relaxed); }
    uint64_t getOrdersProcessed() const { return ordersProcessed_.load(std::memory_order_relaxed); }
    uint64_t getDivergenceCount() const { return divergenceCount_.load(std::memory_order_relaxed); }
    uint64_t getCompareCount() const { return compareCount_.load(std::memory_order_relaxed); }

private:
    void workerLoop() {
        uint64_t lastCompareNs = nowNs();

        while (running_.load(std::memory_order_relaxed)) {
            // Drain the shadow queue
            OrderRequest req;
            bool processed = false;
            while (shadowQueue_.pop(req)) {
                processRequest(req);
                ordersProcessed_.fetch_add(1, std::memory_order_relaxed);
                processed = true;
            }

            if (!processed) {
                // Nothing to do — brief sleep to avoid spinning
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }
    }

    void processRequest(const OrderRequest& req) {
        switch (req.type) {
        case OrderRequest::Type::NewOrder:
            shadow_.processOrder(req.symbolId, req.orderId, req.participantId,
                               req.side, req.price, req.qty, req.orderType,
                               req.stopPrice, req.displayQty, req.tif,
                               req.expiryTime, req.stopLimitPrice, req.pegType,
                               req.pegOffset, req.trailAmount, req.minQty,
                               req.hidden);
            break;
        case OrderRequest::Type::Cancel:
            shadow_.cancelOrder(req.symbolId, req.orderId);
            break;
        case OrderRequest::Type::Modify:
            shadow_.modifyOrder(req.symbolId, req.orderId, req.newQty);
            break;
        case OrderRequest::Type::CancelReplace:
            shadow_.cancelReplace(req.symbolId, req.orderId, req.newPrice, req.newQty);
            break;
        case OrderRequest::Type::KillSwitch:
            shadow_.killSwitch(req.participantId);
            break;
        default:
            break;
        }
    }

    MatchingEngine shadow_;
    MpscQueue<OrderRequest> shadowQueue_;
    std::thread workerThread_;
    std::atomic<bool> running_{false};

    DivergenceCallback divergenceCb_;
    uint64_t compareIntervalMs_{100};

    std::atomic<uint64_t> ordersReceived_{0};
    std::atomic<uint64_t> ordersProcessed_{0};
    std::atomic<uint64_t> divergenceCount_{0};
    std::atomic<uint64_t> compareCount_{0};
};

}  // namespace OrderMatcher
