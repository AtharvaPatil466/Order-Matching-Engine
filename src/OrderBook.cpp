#include "OrderBook.h"
#include "FaultInjector.h"
#include "LatencyTracker.h"  // for nowNs()
#include "StructuredLog.h"
#include <algorithm>
#include <cmath>
#include <mutex>

namespace OrderMatcher {

constexpr size_t INITIAL_CAPACITY = 200000;

OrderBook::OrderBook(SymbolId symbolId, MatchAlgorithm algo)
    : bids_(Side::Buy, 200001), asks_(Side::Sell, 200001),
      orderLookup_(INITIAL_CAPACITY), orderPool_(INITIAL_CAPACITY),
      symbolId_(symbolId), matchAlgorithm_(algo),
      otrStats_(1024), riskLimits_(1024), participantOrders_(64) {
}

// ─── Notifications ───────────────────────────────────────────────────────────

void OrderBook::notifyOrderUpdate(OrderId orderId, OrderStatus status, Quantity filledQty,
                                   Quantity remainingQty, Price lastFillPrice, RejectReason reason) {
#ifndef OB_LEAN_MODE
    if (replayMode_) return;
    OrderUpdate u{};
    u.orderId = orderId;
    u.status = status;
    u.filledQty = filledQty;
    u.remainingQty = remainingQty;
    u.lastFillPrice = lastFillPrice;
    u.rejectReason = reason;
    u.timestamp = nowNs();
    u.sequenceNumber = nextSequenceNumber_++;
    listener_->onOrderUpdate(u);
#else
    (void)orderId; (void)status; (void)filledQty; (void)remainingQty; (void)lastFillPrice; (void)reason;
#endif
}

void OrderBook::notifyMarketData(MarketDataUpdate::Action action, Side side, Price price) {
#ifndef OB_LEAN_MODE
    if (replayMode_) return;

    MarketDataUpdate update{};
    update.action = action;
    update.side = side;
    update.timestamp = nowNs();
    update.sequenceNumber = nextSequenceNumber_++;

    PriceLevel lvl{};
    lvl.price = price;

    auto& book = (side == Side::Buy) ? bids_ : asks_;
    const OrderList* list = book.at(price);
    if (list) {
        for (Order* o = list->front(); o; o = o->next) {
            if (!o->isHidden) {
                lvl.totalQuantity += (o->type == OrderType::Iceberg) ? o->visibleQty : o->remainingQty;
                lvl.orderCount++;
            }
        }
    }

    update.level = lvl;
    listener_->onMarketData(update);
#else
    (void)action; (void)side; (void)price;
#endif
}

// ─── Book helpers ────────────────────────────────────────────────────────────

bool OrderBook::canAddToBook(const Order* order) const {
    if (maxDepthPerSide_ == 0) return true;
    const auto& book = (order->side == Side::Buy) ? bids_ : asks_;
    return book.contains(order->price) || book.size() < maxDepthPerSide_;
}

void OrderBook::ensurePriceRange(Price price) {
    if (!bids_.rangeSet()) {
        // Center the range around the first price seen
        Price halfRange = static_cast<Price>(bids_.capacity() / 2);
        Price minP = price - halfRange;
        bids_.setRange(minP);
        asks_.setRange(minP);
    }
}

bool OrderBook::addToBook(Order* order) {
    if (!canAddToBook(order)) return false;
    ensurePriceRange(order->price);
    auto& book = (order->side == Side::Buy) ? bids_ : asks_;
    if (!book.insert(order->price, order)) {
        // Price outside FlatPriceMap range — reject explicitly (no silent data loss)
        notifyOrderUpdate(order->id, OrderStatus::Rejected, 0, order->remainingQty,
                          0, RejectReason::OutOfPriceRange);
        return false;
    }
    return true;
}

void OrderBook::removeFromBook(Order* order) {
    auto& book = (order->side == Side::Buy) ? bids_ : asks_;
    book.remove(order->price, order);
}

// ─── Risk & Validation ──────────────────────────────────────────────────────

bool OrderBook::checkRiskLimits(ParticipantId participantId, Price price, Quantity qty) {
    const auto* lim = riskLimits_.find(participantId);
    if (!lim) return true;

    if (lim->maxOrderSize > 0 && qty > lim->maxOrderSize) return false;

    if (lim->maxOrderNotional > 0) {
        Price notional = price * static_cast<Price>(qty) / PRICE_PRECISION;
        if (notional > lim->maxOrderNotional) return false;
    }

    if (lim->maxPositionSize > 0) {
        const auto* stats = otrStats_.find(participantId);
        if (stats) {
            int64_t projected = stats->netPosition + static_cast<int64_t>(qty);
            if (static_cast<uint64_t>(std::abs(projected)) > lim->maxPositionSize) return false;
        }
    }

    return true;
}

void OrderBook::setRiskLimits(ParticipantId participantId, const RiskLimits& limits) {
    riskLimits_.insert(participantId, limits);
}

bool OrderBook::checkCircuitBreaker(Price price) {
    if (referencePrice_ == 0) return true;
    double deviation = std::abs(static_cast<double>(price - referencePrice_)) / referencePrice_;
    return deviation <= cbThreshold_;
}

bool OrderBook::checkSMP(const Order& incoming, const Order& resting) const {
    return incoming.participantId == resting.participantId;
}

bool OrderBook::checkLiquidity(Side side, Price price, Quantity qty, OrderType type) const {
    Quantity remaining = qty;

    const auto& opposite = (side == Side::Buy) ? asks_ : bids_;
    opposite.forEachLevelWhile([&](Price levelPrice, const OrderList& level) -> bool {
        if (side == Side::Buy) {
            if (type != OrderType::Market && levelPrice > price) return false;
        } else {
            if (type != OrderType::Market && levelPrice < price) return false;
        }
        for (Order* o = level.front(); o; o = o->next) {
            remaining -= std::min(remaining, o->remainingQty);
            if (remaining == 0) return false;
        }
        return true;
    });

    return remaining == 0;
}

bool OrderBook::checkMinQty(Side side, Price price, Quantity minQty) const {
    Quantity available = 0;

    const auto& opposite = (side == Side::Buy) ? asks_ : bids_;
    bool found = false;
    opposite.forEachLevelWhile([&](Price levelPrice, const OrderList& level) -> bool {
        if (side == Side::Buy) {
            if (levelPrice > price) return false;
        } else {
            if (levelPrice < price) return false;
        }
        for (Order* o = level.front(); o; o = o->next) {
            available += (o->type == OrderType::Iceberg) ? o->visibleQty : o->remainingQty;
            if (available >= minQty) { found = true; return false; }
        }
        return true;
    });

    return found;
}

// ─── addOrder ────────────────────────────────────────────────────────────────

AddOrderResult OrderBook::addOrder(OrderId orderId, ParticipantId participantId, Side side, Price price,
                          Quantity qty, OrderType type, Price stopPrice, Quantity displayQty,
                          TimeInForce tif, uint64_t expiryTime, Price stopLimitPrice,
                          PegType pegType, Price pegOffset, Price trailAmount,
                          Quantity minQty, bool hidden) {
    std::unique_lock<std::shared_mutex> lock(bookLock_);

    // --- Trading-state admission ---
    // Halted: regulator/auto halt; reject new orders, cancels still flow
    // through cancelOrder() which has no state gate.
    if (tradingState_ == TradingState::Halted) [[unlikely]] {
#ifndef OB_LEAN_MODE
        otrStats_[participantId].rejectedOrders++;
#endif
        notifyOrderUpdate(orderId, OrderStatus::Rejected, 0, qty, 0, RejectReason::MarketHalted);
        return RejectReason::MarketHalted;
    }
    // PostClose: scheduled session end. Same effect as Halted but a
    // different reject reason for client clarity.
    if (tradingState_ == TradingState::PostClose) [[unlikely]] {
#ifndef OB_LEAN_MODE
        otrStats_[participantId].rejectedOrders++;
#endif
        notifyOrderUpdate(orderId, OrderStatus::Rejected, 0, qty, 0, RejectReason::MarketClosed);
        return RejectReason::MarketClosed;
    }

    // PreOpen / AuctionOpen / AuctionClose: orders accumulate without
    // continuous matching. IOC/FOK are still rejected — they require
    // immediate fills, which are unavailable until the uncross runs.
    // Market orders ARE accepted here: they get parked in
    // auctionMarketOrders_ and participate in the uncross at the
    // discovered price (see below + uncross()).
    if (tradingState_ == TradingState::PreOpen      ||
        tradingState_ == TradingState::AuctionOpen  ||
        tradingState_ == TradingState::AuctionClose) [[unlikely]] {
        if (type == OrderType::IOC || type == OrderType::FOK) {
#ifndef OB_LEAN_MODE
            otrStats_[participantId].rejectedOrders++;
#endif
            notifyOrderUpdate(orderId, OrderStatus::Rejected, 0, qty, 0,
                              RejectReason::OrderTypeNotAllowedInState);
            return RejectReason::OrderTypeNotAllowedInState;
        }
    }

    // --- Input validation ---
    if (qty == 0) [[unlikely]] {
#ifndef OB_LEAN_MODE
        otrStats_[participantId].rejectedOrders++;
#endif
        notifyOrderUpdate(orderId, OrderStatus::Rejected, 0, 0, 0, RejectReason::InvalidQuantity);
        return RejectReason::InvalidQuantity;
    }

    if (type != OrderType::Market && price <= 0
                 && type != OrderType::Stop && type != OrderType::StopLimit
                 && type != OrderType::TrailingStop) [[unlikely]] {
#ifndef OB_LEAN_MODE
        otrStats_[participantId].rejectedOrders++;
#endif
        notifyOrderUpdate(orderId, OrderStatus::Rejected, 0, qty, 0, RejectReason::InvalidPrice);
        return RejectReason::InvalidPrice;
    }

    // --- Duplicate orderId check ---
    // FlatHashMap::insert silently overwrites on duplicate key, which
    // would orphan the prior order in its price-level list and pool.
    // Reject explicitly so the duplicate is observable to the client
    // and no resources leak.
    if (orderLookup_.find(orderId) != nullptr) [[unlikely]] {
#ifndef OB_LEAN_MODE
        otrStats_[participantId].rejectedOrders++;
#endif
        notifyOrderUpdate(orderId, OrderStatus::Rejected, 0, qty, 0,
                          RejectReason::DuplicateOrderId);
        return RejectReason::DuplicateOrderId;
    }

    // --- Pre-trade risk checks ---
#ifndef OB_LEAN_MODE
    if (!checkRiskLimits(participantId, price, qty)) [[unlikely]] {
        otrStats_[participantId].rejectedOrders++;
        notifyOrderUpdate(orderId, OrderStatus::Rejected, 0, qty, 0, RejectReason::RiskLimitBreached);
        return RejectReason::RiskLimitBreached;
    }

    otrStats_[participantId].ordersSubmitted++;
#endif

    // --- Reference price for circuit breaker ---
    if (referencePrice_ == 0 && type != OrderType::Market
                 && type != OrderType::Stop && type != OrderType::StopLimit
                 && type != OrderType::TrailingStop) [[unlikely]] {
        referencePrice_ = price;
    }

    // --- Price band (LULD) admission filter ---
    // Reject orders priced outside [ref*(1-pct), ref*(1+pct)] when a band
    // is configured and a reference price has been established. Distinct
    // from the volatility breaker below: this rejects the individual
    // order without halting the market, lets subsequent in-band orders
    // continue trading. Skipped on order types without a meaningful limit
    // price (Market / Stop families / Pegged).
    if (priceBandPct_ > 0.0 && referencePrice_ > 0 &&
        (type == OrderType::Limit  || type == OrderType::IOC ||
         type == OrderType::FOK    || type == OrderType::PostOnly ||
         type == OrderType::Iceberg || type == OrderType::Hidden)) {
        // Use integer math against an absolute deviation rather than
        // floating-point ratio to keep the check exact at the tick grid
        // and stable under hot-loop reordering.
        Price half = static_cast<Price>(
            static_cast<double>(referencePrice_) * priceBandPct_);
        Price lo = referencePrice_ - half;
        Price hi = referencePrice_ + half;
        if (price < lo || price > hi) {
#ifndef OB_LEAN_MODE
            otrStats_[participantId].rejectedOrders++;
#endif
            notifyOrderUpdate(orderId, OrderStatus::Rejected, 0, qty, 0,
                              RejectReason::OutsidePriceBand);
            return RejectReason::OutsidePriceBand;
        }
    }

    // --- Circuit breaker check ---
#ifndef OB_LEAN_MODE
    if (type == OrderType::Limit || type == OrderType::IOC || type == OrderType::FOK
        || type == OrderType::PostOnly || type == OrderType::Iceberg || type == OrderType::Hidden) {
        if (!checkCircuitBreaker(price)) {
            // The order that trips the breaker is reported with
            // VolatilityCircuitBreaker. Subsequent orders during the
            // resulting halt are reported with MarketHalted (caught by the
            // state check at the top of addOrder).
            tradingState_ = TradingState::Halted;
            obSink().log(obEvent("breaker_trip", LogSeverity::Warn)
                .kv("symbol", (long long)symbolId_)
                .kv("price", (long long)price)
                .kv("ref", (long long)referencePrice_)
                .kv("threshold_pct", cbThreshold_));
            notifyOrderUpdate(orderId, OrderStatus::Rejected, 0, qty, 0, RejectReason::VolatilityCircuitBreaker);
            return RejectReason::VolatilityCircuitBreaker;
        }
    }
#endif

    // --- Post-Only: reject if would cross the spread ---
    if (type == OrderType::PostOnly) [[unlikely]] {
        bool wouldCross = false;
        if (side == Side::Buy)
            wouldCross = !asks_.empty() && price >= asks_.bestPrice();
        else
            wouldCross = !bids_.empty() && price <= bids_.bestPrice();

        if (wouldCross) {
#ifndef OB_LEAN_MODE
            otrStats_[participantId].rejectedOrders++;
#endif
            notifyOrderUpdate(orderId, OrderStatus::Rejected, 0, qty, 0, RejectReason::PostOnlyWouldCross);
            return RejectReason::PostOnlyWouldCross;
        }
    }

    // --- Allocate order from pool ---
    // Fault injection: simulate pool exhaustion. The natural path
    // (allocate 200k orders) is too slow for a unit test; this point
    // exercises the CapacityExhausted reject path deterministically.
    Order* order = FaultInjector::instance().shouldFail("pool.allocate.fail")
                       ? nullptr : orderPool_.allocate();
    if (!order) [[unlikely]] {
#ifndef OB_LEAN_MODE
        otrStats_[participantId].rejectedOrders++;
#endif
        notifyOrderUpdate(orderId, OrderStatus::Rejected, 0, qty, 0, RejectReason::CapacityExhausted);
        return RejectReason::CapacityExhausted;
    }

    order->id = orderId;
    order->participantId = participantId;
    order->side = side;
    order->price = price;
    order->initialQty = qty;
    order->remainingQty = qty;
    order->type = type;
    order->status = OrderStatus::Accepted;
    order->timeInForce = tif;
    order->expiryTime = expiryTime;
    order->stopPrice = stopPrice;
    order->stopLimitPrice = stopLimitPrice;
    order->displayQty = displayQty;
    order->visibleQty = 0;
    order->pegType = pegType;
    order->pegOffset = pegOffset;
    order->trailAmount = trailAmount;
    order->trailRefPrice = 0;
    order->minQty = minQty;
    order->isHidden = hidden || (type == OrderType::Hidden);
    order->isStopTriggered = false;
    order->symbolId = symbolId_;
    order->timestamp = nowNs();
    order->next = nullptr;
    order->prev = nullptr;

    // --- Register for O(1) lookup ---
    orderLookup_.insert(orderId, order);
#ifndef OB_LEAN_MODE
    participantOrders_[participantId].push_back(orderId);
#endif

    notifyOrderUpdate(orderId, OrderStatus::Accepted, 0, qty);

    // --- Auction Market orders: park for the uncross ---
    // A Market order in an auction state has no limit price; it cannot
    // rest in the FlatPriceMap and must not match continuously (we are
    // accumulating, not matching). Hold it in auctionMarketOrders_;
    // uncross() folds these into volume discovery and inserts them at
    // the discovered price for execution. Any unfilled remainder is
    // cancelled at the end of uncross.
    if (type == OrderType::Market &&
        (tradingState_ == TradingState::PreOpen     ||
         tradingState_ == TradingState::AuctionOpen ||
         tradingState_ == TradingState::AuctionClose)) {
        auctionMarketOrders_.push_back(order);
        return orderId;
    }

    // --- Stop / StopLimit: park until triggered ---
    if (type == OrderType::Stop || type == OrderType::StopLimit) {
        stopOrders_.push_back(order);
        return orderId;
    }

    // --- Trailing Stop: park and initialize reference ---
    if (type == OrderType::TrailingStop) {
        if (side == Side::Buy) {
            order->trailRefPrice = lastTradePrice_ > 0 ? lastTradePrice_ : price;
            order->stopPrice = order->trailRefPrice + trailAmount;
        } else {
            order->trailRefPrice = lastTradePrice_ > 0 ? lastTradePrice_ : price;
            order->stopPrice = order->trailRefPrice - trailAmount;
        }
        trailingStopOrders_.push_back(order);
        return orderId;
    }

    // --- Pegged: compute price from reference and rest ---
    if (type == OrderType::Pegged) {
        Price pegPrice = price; // fallback
        if (pegType == PegType::MidPeg) {
            Price mid = getMidPrice();
            if (mid > 0) pegPrice = mid + pegOffset;
        } else if (pegType == PegType::PrimaryPeg) {
            if (side == Side::Buy) {
                Price bb = getBestBid();
                if (bb > 0) pegPrice = bb + pegOffset;
            } else {
                Price ba = getBestAsk();
                if (ba < std::numeric_limits<Price>::max()) pegPrice = ba + pegOffset;
            }
        }
        order->price = pegPrice;
        peggedOrders_.push_back(order);
        addToBook(order);
        if (!order->isHidden)
            notifyMarketData(MarketDataUpdate::Action::Add, side, order->price);
        return orderId;
    }

    // --- FOK: require full liquidity ---
    if (type == OrderType::FOK) {
        if (!checkLiquidity(side, price, qty, type)) {
            orderLookup_.erase(orderId);
            orderPool_.deallocate(order);
            notifyOrderUpdate(orderId, OrderStatus::Rejected, 0, qty, 0, RejectReason::FOKInsufficientLiquidity);
            return RejectReason::FOKInsufficientLiquidity;
        }
    }

    // --- Min quantity check ---
    if (minQty > 0 && type != OrderType::FOK) [[unlikely]] {
        if (!checkMinQty(side, price, minQty)) {
            if (type == OrderType::IOC) {
                orderLookup_.erase(orderId);
                orderPool_.deallocate(order);
                notifyOrderUpdate(orderId, OrderStatus::Cancelled, 0, qty);
                return orderId;
            }
            // For limit orders, rest in book without matching (will match later)
            addToBook(order);
            if (!order->isHidden)
                notifyMarketData(MarketDataUpdate::Action::Add, side, price);
            return orderId;
        }
    }

    // --- Match (skipped during auction / pre-open) ---
    // PreOpen / AuctionOpen / AuctionClose all accumulate without
    // continuous matching; uncross() at the appropriate session boundary
    // produces all trades at the single discovered uncross price.
    const bool inAuction =
        (tradingState_ == TradingState::AuctionOpen) ||
        (tradingState_ == TradingState::AuctionClose) ||
        (tradingState_ == TradingState::PreOpen);
    if (!inAuction) {
        if (matchAlgorithm_ == MatchAlgorithm::ProRata)
            matchProRata(order);
        else
            match(order);
    }

    // --- Trigger stops / update pegs ---
    if (lastTradePrice_ > 0) {
        checkStopOrders(lastTradePrice_);
        updateTrailingStops(lastTradePrice_);
    }
    if (!peggedOrders_.empty())
        updatePeggedOrders();

    // --- Post-match: handle remaining quantity ---
    if (order->remainingQty > 0) [[likely]] {
        if (type == OrderType::IOC || type == OrderType::FOK || type == OrderType::Market) {
            Quantity filled = order->initialQty - order->remainingQty;
            OrderStatus st = (filled > 0) ? OrderStatus::PartiallyFilled : OrderStatus::Cancelled;
            notifyOrderUpdate(orderId, st, filled, 0);
            orderLookup_.erase(orderId);
            orderPool_.deallocate(order);
        } else {
            // Rest in book (Limit, PostOnly, Hidden, Iceberg)
            if (type == OrderType::Iceberg)
                order->visibleQty = std::min(order->remainingQty, order->displayQty);
            if (!addToBook(order)) {
                // Depth limit or price range exceeded — cancel the order
                Quantity filled = order->initialQty - order->remainingQty;
                notifyOrderUpdate(orderId, OrderStatus::Cancelled, filled, 0);
                orderLookup_.erase(orderId);
                orderPool_.deallocate(order);
            } else {
                order->status = (order->remainingQty < order->initialQty)
                                ? OrderStatus::PartiallyFilled : OrderStatus::Accepted;
                if (!order->isHidden)
                    notifyMarketData(MarketDataUpdate::Action::Add, side, price);
            }
        }
    } else {
        order->status = OrderStatus::Filled;
        notifyOrderUpdate(orderId, OrderStatus::Filled, order->initialQty, 0, lastTradePrice_);
        orderLookup_.erase(orderId);
        orderPool_.deallocate(order);
    }
    return orderId;
}

// ─── Match (Price-Time FIFO) ─────────────────────────────────────────────────

void OrderBook::match(Order* incoming) {
    while (incoming->remainingQty > 0) {
        bool isBuy = (incoming->side == Side::Buy);
        auto& opposite = isBuy ? asks_ : bids_;

        if (opposite.empty()) [[unlikely]] break;

        Price bestPrice = opposite.bestPrice();

        if (isBuy) {
            if (incoming->type != OrderType::Market && incoming->price < bestPrice) [[likely]]
                break;
        } else {
            if (incoming->type != OrderType::Market && incoming->price > bestPrice) [[likely]]
                break;
        }

        OrderList* level = opposite.bestLevel();
        Order* bookOrder = level->front();

        if (checkSMP(*incoming, *bookOrder)) [[unlikely]] {
            incoming->remainingQty = 0;
            break;
        }

        Quantity available = (bookOrder->type == OrderType::Iceberg)
                             ? bookOrder->visibleQty : bookOrder->remainingQty;
        Quantity fillQty = std::min(incoming->remainingQty, available);

        incoming->remainingQty -= fillQty;
        bookOrder->remainingQty -= fillQty;
        if (bookOrder->type == OrderType::Iceberg) bookOrder->visibleQty -= fillQty;

        lastTradePrice_ = bestPrice;
        lastTradeQty_ = fillQty;
        updateAnalytics(bestPrice, fillQty, bookOrder->participantId, incoming->participantId);

        OrderId buyId = isBuy ? incoming->id : bookOrder->id;
        OrderId sellId = isBuy ? bookOrder->id : incoming->id;
        ParticipantId buyerId = isBuy ? incoming->participantId : bookOrder->participantId;
        ParticipantId sellerId = isBuy ? bookOrder->participantId : incoming->participantId;

#ifndef OB_LEAN_MODE
        otrStats_[buyerId].netPosition += static_cast<int64_t>(fillQty);
        otrStats_[sellerId].netPosition -= static_cast<int64_t>(fillQty);
#endif

        Trade t{};
        t.tradeId = nextTradeId_++;
        t.buyOrderId = buyId;
        t.sellOrderId = sellId;
        t.buyerId = buyerId;
        t.sellerId = sellerId;
        t.price = bestPrice;
        t.quantity = fillQty;
        t.timestamp = nowNs();
        t.sequenceNumber = nextSequenceNumber_++;
        t.symbolId = symbolId_;
        tradeHistory_.push(t);
        if (!replayMode_) listener_->onTrade(t);

        if (bookOrder->remainingQty == 0) [[unlikely]] {
            bookOrder->status = OrderStatus::Filled;
            notifyOrderUpdate(bookOrder->id, OrderStatus::Filled, bookOrder->initialQty, 0, bestPrice);
            level->remove(bookOrder);
            orderLookup_.erase(bookOrder->id);
            orderPool_.deallocate(bookOrder);
            if (level->empty()) [[unlikely]] opposite.eraseBest();
        } else if (bookOrder->type == OrderType::Iceberg && bookOrder->visibleQty == 0) [[unlikely]] {
            // Price-time iceberg refresh: the freshly-revealed slice
            // forfeits its time priority and joins the back of the queue
            // at this price level. This is the convention at NYSE / Nasdaq
            // / LSE — a refreshed slice is treated as a "new arrival" and
            // any orders that arrived after the original iceberg
            // placement (but before the slice was refilled) fill ahead of
            // it. Tested in IcebergPriority_PriceTime_LosesPriorityOnRefresh.
            bookOrder->visibleQty = std::min(bookOrder->remainingQty, bookOrder->displayQty);
            bookOrder->status = OrderStatus::PartiallyFilled;
            level->remove(bookOrder);
            level->push_back(bookOrder);
        } else {
            bookOrder->status = OrderStatus::PartiallyFilled;
        }
    }
}

// ─── Match (Pro-Rata) ────────────────────────────────────────────────────────
// Zero-allocation: uses stack-allocated arrays instead of std::vector.

void OrderBook::matchProRata(Order* incoming) {
    struct Alloc { Order* order; Quantity qty; };
    static constexpr size_t MAX_LEVEL_ORDERS = 1024;

    while (incoming->remainingQty > 0) {
        bool isBuy = (incoming->side == Side::Buy);

        auto& opposite = isBuy ? asks_ : bids_;
        if (opposite.empty()) break;

        Price bestPrice = opposite.bestPrice();
        if (isBuy) {
            if (incoming->type != OrderType::Market && incoming->price < bestPrice) break;
        } else {
            if (incoming->type != OrderType::Market && incoming->price > bestPrice) break;
        }

        OrderList& level = *opposite.bestLevel();

        // Calculate total quantity at this level
        Quantity totalLevelQty = 0;
        for (Order* o = level.front(); o; o = o->next) {
            totalLevelQty += (o->type == OrderType::Iceberg) ? o->visibleQty : o->remainingQty;
        }
        if (totalLevelQty == 0) {
            opposite.eraseBest();
            continue;
        }

        Quantity toFill = std::min(incoming->remainingQty, totalLevelQty);

        // Compute proportional allocations (stack-allocated)
        Alloc allocs[MAX_LEVEL_ORDERS];
        size_t allocCount = 0;
        Quantity allocated = 0;

        for (Order* o = level.front(); o && allocCount < MAX_LEVEL_ORDERS; o = o->next) {
            if (checkSMP(*incoming, *o)) {
                incoming->remainingQty = 0;
                return;
            }
            Quantity avail = (o->type == OrderType::Iceberg) ? o->visibleQty : o->remainingQty;
            Quantity share = (totalLevelQty > 0)
                ? static_cast<Quantity>(static_cast<double>(avail) / totalLevelQty * toFill)
                : 0;
            allocs[allocCount++] = {o, share};
            allocated += share;
        }

        // Distribute rounding remainder by time priority
        Quantity remainder = toFill - allocated;
        for (size_t i = 0; i < allocCount && remainder > 0; ++i) {
            Quantity avail = (allocs[i].order->type == OrderType::Iceberg)
                ? allocs[i].order->visibleQty : allocs[i].order->remainingQty;
            Quantity extra = std::min(remainder, avail - allocs[i].qty);
            allocs[i].qty += extra;
            remainder -= extra;
        }

        // Execute fills (collect removals on stack)
        Order* toRemove[MAX_LEVEL_ORDERS];
        size_t removeCount = 0;

        for (size_t i = 0; i < allocCount; ++i) {
            if (allocs[i].qty == 0) continue;
            Order* bookOrder = allocs[i].order;
            Quantity fillQty = allocs[i].qty;

            incoming->remainingQty -= fillQty;
            bookOrder->remainingQty -= fillQty;
            if (bookOrder->type == OrderType::Iceberg) bookOrder->visibleQty -= fillQty;

            lastTradePrice_ = bestPrice;
            lastTradeQty_ = fillQty;

            OrderId buyId = isBuy ? incoming->id : bookOrder->id;
            OrderId sellId = isBuy ? bookOrder->id : incoming->id;
            ParticipantId buyerId = isBuy ? incoming->participantId : bookOrder->participantId;
            ParticipantId sellerId = isBuy ? bookOrder->participantId : incoming->participantId;

            updateAnalytics(bestPrice, fillQty, bookOrder->participantId, incoming->participantId);
#ifndef OB_LEAN_MODE
            otrStats_[buyerId].netPosition += static_cast<int64_t>(fillQty);
            otrStats_[sellerId].netPosition -= static_cast<int64_t>(fillQty);
#endif

            Trade t{};
            t.tradeId = nextTradeId_++;
            t.buyOrderId = buyId;
            t.sellOrderId = sellId;
            t.buyerId = buyerId;
            t.sellerId = sellerId;
            t.price = bestPrice;
            t.quantity = fillQty;
            t.timestamp = nowNs();
            t.sequenceNumber = nextSequenceNumber_++;
            t.symbolId = symbolId_;
            tradeHistory_.push(t);
            if (!replayMode_) listener_->onTrade(t);

            if (bookOrder->remainingQty == 0) {
                bookOrder->status = OrderStatus::Filled;
                notifyOrderUpdate(bookOrder->id, OrderStatus::Filled, bookOrder->initialQty, 0, bestPrice);
                toRemove[removeCount++] = bookOrder;
            } else if (bookOrder->type == OrderType::Iceberg && bookOrder->visibleQty == 0) {
                // Pro-rata iceberg refresh: the slice is replenished in
                // place. Pro-rata allocation is weight-by-volume (not
                // time), so position in the level list does not affect
                // fill distribution; refreshing without re-queuing
                // matches CME-style behavior where the iceberg
                // participates continuously in proportional allocation
                // for as long as hidden quantity remains. Tested in
                // IcebergPriority_ProRata_KeepsPriorityOnRefresh.
                bookOrder->visibleQty = std::min(bookOrder->remainingQty, bookOrder->displayQty);
                bookOrder->status = OrderStatus::PartiallyFilled;
            } else {
                bookOrder->status = OrderStatus::PartiallyFilled;
            }
        }

        for (size_t i = 0; i < removeCount; ++i) {
            level.remove(toRemove[i]);
            orderLookup_.erase(toRemove[i]->id);
            orderPool_.deallocate(toRemove[i]);
        }

        if (level.empty()) {
            opposite.eraseBest();
        }
    }
}

// ─── Cancel ──────────────────────────────────────────────────────────────────

void OrderBook::cancelOrder(OrderId orderId) {
    std::unique_lock<std::shared_mutex> lock(bookLock_);
    cancelOrderImpl(orderId);
}

namespace {
const char* tradingStateName(TradingState s) {
    switch (s) {
    case TradingState::Continuous:    return "Continuous";
    case TradingState::Halted:        return "Halted";
    case TradingState::AuctionOpen:   return "AuctionOpen";
    case TradingState::AuctionClose:  return "AuctionClose";
    case TradingState::PreOpen:       return "PreOpen";
    case TradingState::PostClose:     return "PostClose";
    }
    return "Unknown";
}
}  // namespace

void OrderBook::setTradingState(TradingState s) {
    if (tradingState_ == s) return;  // no-op transitions don't emit events
    obSink().log(obEvent("trading_state_change")
                     .kv("symbol", (long long)symbolId_)
                     .kv("from", tradingStateName(tradingState_))
                     .kv("to",   tradingStateName(s)));
    tradingState_ = s;
}

void OrderBook::cancelOrderImpl(OrderId orderId) {
    auto* orderPtr = orderLookup_.find(orderId);
    if (!orderPtr) [[unlikely]] return;

    Order* order = *orderPtr;
    if (!order) return;
#ifndef OB_LEAN_MODE
    otrStats_[order->participantId].ordersSubmitted++;
#endif

    // Remove from special tracking lists (FixedVector::erase_value — O(n) swap-erase)
    if (order->type == OrderType::Stop || order->type == OrderType::StopLimit) {
        stopOrders_.erase_value(order);
    } else if (order->type == OrderType::TrailingStop) {
        trailingStopOrders_.erase_value(order);
    } else if (order->type == OrderType::Pegged) {
        peggedOrders_.erase_value(order);
    }

    removeFromBook(order);

    if (!order->isHidden)
        notifyMarketData(MarketDataUpdate::Action::Delete, order->side, order->price);

    Quantity filledQty = order->initialQty - order->remainingQty;
    notifyOrderUpdate(orderId, OrderStatus::Cancelled, filledQty, 0);

    orderLookup_.erase(orderId);
    orderPool_.deallocate(order);
}

// ─── Modify (quantity reduction only, preserves time priority) ───────────────

bool OrderBook::modifyOrder(OrderId orderId, Quantity newQty) {
    std::unique_lock<std::shared_mutex> lock(bookLock_);

    auto* orderPtr = orderLookup_.find(orderId);
    if (!orderPtr) [[unlikely]] return false;

    Order* order = *orderPtr;
    if (!order) return false;

    if (newQty < order->remainingQty) {
        order->remainingQty = newQty;
        if (!order->isHidden)
            notifyMarketData(MarketDataUpdate::Action::Modify, order->side, order->price);
        return true;
    }

    return false;
}

// ─── Cancel/Replace (full amendment, price change loses priority) ────────────

bool OrderBook::cancelReplace(OrderId orderId, Price newPrice, Quantity newQty) {
    std::unique_lock<std::shared_mutex> lock(bookLock_);

    auto* orderPtr = orderLookup_.find(orderId);
    if (!orderPtr) [[unlikely]] return false;

    Order* order = *orderPtr;
    if (!order) return false;

    if (newQty == 0 || newPrice <= 0) [[unlikely]] return false;

    // Parked order types (Stop, StopLimit, TrailingStop, Pegged) live in
    // their own tracking lists and not in the priced book. cancelReplace
    // is defined for in-book orders and would leave a parked order in
    // both its tracking list AND the book if applied — leading to a
    // double-deallocate when the trigger path fires later. Refuse the
    // operation; clients should cancel + add fresh for these types.
    if (order->type == OrderType::Stop ||
        order->type == OrderType::StopLimit ||
        order->type == OrderType::TrailingStop ||
        order->type == OrderType::Pegged) [[unlikely]] {
        return false;
    }

    Price oldPrice = order->price;
    bool priceChanged = (newPrice != oldPrice);

    if (priceChanged) {
        removeFromBook(order);
        if (!order->isHidden)
            notifyMarketData(MarketDataUpdate::Action::Delete, order->side, oldPrice);

        order->price = newPrice;
        order->remainingQty = newQty;
        order->timestamp = nowNs();

        // Check if new price crosses — if so, match first
        bool wouldCross = false;
        if (order->side == Side::Buy)
            wouldCross = !asks_.empty() && newPrice >= asks_.bestPrice();
        else
            wouldCross = !bids_.empty() && newPrice <= bids_.bestPrice();

        if (wouldCross) {
            if (matchAlgorithm_ == MatchAlgorithm::ProRata)
                matchProRata(order);
            else
                match(order);
        }

        if (order->remainingQty > 0) {
            if (!addToBook(order)) {
                Quantity filled = order->initialQty - order->remainingQty;
                notifyOrderUpdate(orderId, OrderStatus::Cancelled, filled, 0);
                orderLookup_.erase(orderId);
                orderPool_.deallocate(order);
            } else {
                if (!order->isHidden)
                    notifyMarketData(MarketDataUpdate::Action::Add, order->side, newPrice);
            }
        } else {
            order->status = OrderStatus::Filled;
            notifyOrderUpdate(orderId, OrderStatus::Filled, order->initialQty, 0, lastTradePrice_);
            orderLookup_.erase(orderId);
            orderPool_.deallocate(order);
        }
    } else {
        // Same price
        if (newQty < order->remainingQty) {
            order->remainingQty = newQty;
        } else {
            // Quantity increase loses time priority
            removeFromBook(order);
            order->remainingQty = newQty;
            order->timestamp = nowNs();
            addToBook(order);
        }
        if (!order->isHidden)
            notifyMarketData(MarketDataUpdate::Action::Modify, order->side, order->price);
    }

    return true;
}

// ─── Kill Switch ─────────────────────────────────────────────────────────────

uint64_t OrderBook::cancelAllForParticipant(ParticipantId participantId) {
    std::unique_lock<std::shared_mutex> lock(bookLock_);

    uint64_t count = 0;
    OrderId ids[4096];
    size_t idCount = 0;

#ifndef OB_LEAN_MODE
    auto* orders = participantOrders_.find(participantId);
    if (!orders) return 0;

    for (size_t i = 0; i < orders->size() && idCount < 4096; ++i) {
        ids[idCount++] = (*orders)[i];
    }
    orders->clear();
#else
    orderLookup_.forEach([&](OrderId id, Order* order) {
        if (order && order->participantId == participantId && idCount < 4096) {
            ids[idCount++] = id;
        }
    });
#endif

    for (size_t i = 0; i < idCount; ++i) {
        if (orderLookup_.contains(ids[i])) {
            cancelOrderImpl(ids[i]);  // already holding the lock
            ++count;
        }
    }

    return count;
}

// ─── Time-based expiry ───────────────────────────────────────────────────────

void OrderBook::expireOrders(uint64_t currentTime,
                             const std::function<void(OrderId)>& onExpire) {
    std::unique_lock<std::shared_mutex> lock(bookLock_);

    // Collect expired order IDs into stack buffer (avoids heap allocation)
    OrderId toExpire[4096];
    size_t expireCount = 0;

    orderLookup_.forEach([&](OrderId id, Order* order) {
        if (!order || expireCount >= 4096) return;
        if ((order->timeInForce == TimeInForce::GTD || order->timeInForce == TimeInForce::DAY)
            && currentTime >= order->expiryTime) {
            toExpire[expireCount++] = id;
        }
    });

    for (size_t i = 0; i < expireCount; ++i) {
        if (onExpire) onExpire(toExpire[i]);  // notify (e.g. journal) BEFORE cancel
        cancelOrderImpl(toExpire[i]);          // already holding the lock
    }
}

// ─── Stop Order Triggers ─────────────────────────────────────────────────────

void OrderBook::checkStopOrders(Price lastTradePrice) {
    if (stopOrders_.empty()) [[unlikely]] return;

    // Index-based iteration with erase_swap (FixedVector compatible)
    size_t i = 0;
    while (i < stopOrders_.size()) {
        Order* order = stopOrders_[i];
        bool triggered = false;

        if (order->side == Side::Buy) {
            if (lastTradePrice >= order->stopPrice) triggered = true;
        } else {
            if (lastTradePrice <= order->stopPrice) triggered = true;
        }

        if (triggered) {
            order->isStopTriggered = true;

            if (order->type == OrderType::StopLimit) {
                order->type = OrderType::Limit;
                order->price = order->stopLimitPrice;
            } else {
                order->type = OrderType::Limit;
            }

            stopOrders_.erase_swap(i);
            match(order);

            if (order->remainingQty > 0) {
                if (!addToBook(order)) {
                    Quantity filled = order->initialQty - order->remainingQty;
                    notifyOrderUpdate(order->id, OrderStatus::Cancelled, filled, 0);
                    orderLookup_.erase(order->id);
                    orderPool_.deallocate(order);
                }
            } else {
                order->status = OrderStatus::Filled;
                orderLookup_.erase(order->id);
                orderPool_.deallocate(order);
            }
            // Don't increment i — erase_swap moved the last element here
        } else {
            ++i;
        }
    }
}

// ─── Trailing Stop Updates ───────────────────────────────────────────────────

void OrderBook::updateTrailingStops(Price lastTradePrice) {
    if (trailingStopOrders_.empty()) [[unlikely]] return;

    size_t i = 0;
    while (i < trailingStopOrders_.size()) {
        Order* order = trailingStopOrders_[i];
        bool triggered = false;

        if (order->side == Side::Buy) {
            if (lastTradePrice < order->trailRefPrice) {
                order->trailRefPrice = lastTradePrice;
                order->stopPrice = order->trailRefPrice + order->trailAmount;
            }
            if (lastTradePrice >= order->stopPrice) triggered = true;
        } else {
            if (lastTradePrice > order->trailRefPrice) {
                order->trailRefPrice = lastTradePrice;
                order->stopPrice = order->trailRefPrice - order->trailAmount;
            }
            if (lastTradePrice <= order->stopPrice) triggered = true;
        }

        if (triggered) {
            order->type = OrderType::Limit;
            order->price = order->stopPrice;
            trailingStopOrders_.erase_swap(i);
            match(order);

            if (order->remainingQty > 0) {
                if (!addToBook(order)) {
                    Quantity filled = order->initialQty - order->remainingQty;
                    notifyOrderUpdate(order->id, OrderStatus::Cancelled, filled, 0);
                    orderLookup_.erase(order->id);
                    orderPool_.deallocate(order);
                }
            } else {
                order->status = OrderStatus::Filled;
                orderLookup_.erase(order->id);
                orderPool_.deallocate(order);
            }
        } else {
            ++i;
        }
    }
}

// ─── Pegged Order Re-pricing ─────────────────────────────────────────────────

void OrderBook::updatePeggedOrders() {
    size_t i = 0;
    while (i < peggedOrders_.size()) {
        Order* order = peggedOrders_[i];

        // Check if order was removed
        auto* lookupPtr = orderLookup_.find(order->id);
        if (!lookupPtr || *lookupPtr != order) {
            peggedOrders_.erase_swap(i);
            continue;
        }

        Price newPrice = order->price;
        if (order->pegType == PegType::MidPeg) {
            Price mid = getMidPrice();
            if (mid > 0) newPrice = mid + order->pegOffset;
        } else if (order->pegType == PegType::PrimaryPeg) {
            if (order->side == Side::Buy) {
                Price bb = getBestBid();
                if (bb > 0) newPrice = bb + order->pegOffset;
            } else {
                Price ba = getBestAsk();
                if (ba < std::numeric_limits<Price>::max()) newPrice = ba + order->pegOffset;
            }
        }

        if (newPrice != order->price && newPrice > 0) {
            removeFromBook(order);
            order->price = newPrice;
            order->timestamp = nowNs();
            addToBook(order);
        }

        ++i;
    }
}

// ─── Analytics ───────────────────────────────────────────────────────────────

void OrderBook::updateAnalytics(Price price, Quantity qty, ParticipantId p1, ParticipantId p2) {
    double tradeValue = static_cast<double>(price) * qty;
    vwap_ = (vwap_ * totalQty_ + tradeValue) / (totalQty_ + qty);
    totalQty_ += qty;

    cumulativePrice_ += price;
    priceUpdates_++;

    if (p1 > 0) otrStats_[p1].tradesExecuted++;
    if (p2 > 0) otrStats_[p2].tradesExecuted++;
}

// ─── Uncross (Auction) ──────────────────────────────────────────────────────
// Uses stack-allocated FixedVector to avoid heap allocation during auction.

void OrderBook::uncross() {
    std::unique_lock<std::shared_mutex> lock(bookLock_);

    // Sum any market orders parked during the auction; they participate
    // in volume discovery without contributing candidate prices (they
    // have no limit price to anchor on).
    Quantity marketBuyTotal = 0, marketSellTotal = 0;
    for (Order* m : auctionMarketOrders_) {
        if (m->side == Side::Buy) marketBuyTotal  += m->remainingQty;
        else                       marketSellTotal += m->remainingQty;
    }

    const bool hasLimits  = !bids_.empty() || !asks_.empty();
    const bool hasMarkets = marketBuyTotal > 0 || marketSellTotal > 0;
    if (!hasLimits && !hasMarkets) return;

    // No limit orders → no candidate prices. Real venues fall back to the
    // prior session's reference price; we don't track that, so cancel the
    // markets rather than execute at an arbitrary price.
    if (!hasLimits) {
        cancelAuctionMarketOrders();
        return;
    }

    // Step 1: Discover the uncross price that maximizes matched volume.
    // Market orders are willing to trade at any price, so they
    // contribute uniformly to cumBuy/cumSell at every candidate.
    Price bestUncrossPrice = 0;
    Quantity maxVolume = 0;

    FixedVector<Price, 4096> prices;
    bids_.forEachLevel([&](Price p, OrderList&) { prices.push_back(p); });
    asks_.forEachLevel([&](Price p, OrderList&) { prices.push_back(p); });
    std::sort(prices.begin(), prices.end());
    auto* newEnd = std::unique(prices.begin(), prices.end());
    size_t uniqueCount = static_cast<size_t>(newEnd - prices.begin());

    for (size_t pi = 0; pi < uniqueCount; ++pi) {
        Price p = prices[pi];
        Quantity cumBuy  = marketBuyTotal;
        Quantity cumSell = marketSellTotal;

        bids_.forEachLevel([&](Price bp, OrderList& list) {
            if (bp >= p) {
                for (Order* o = list.front(); o; o = o->next) cumBuy += o->remainingQty;
            }
        });
        asks_.forEachLevel([&](Price ap, OrderList& list) {
            if (ap <= p) {
                for (Order* o = list.front(); o; o = o->next) cumSell += o->remainingQty;
            }
        });

        Quantity volume = std::min(cumBuy, cumSell);
        if (volume > maxVolume) {
            maxVolume = volume;
            bestUncrossPrice = p;
        }
    }

    if (maxVolume == 0) {
        // No volume achievable at any candidate price (e.g., a single
        // limit order with no counterparty, market orders only on one
        // side). Cancel the parked markets — they cannot fill.
        cancelAuctionMarketOrders();
        return;
    }

    // Step 1.5: Insert market orders into the regular book at the
    // discovered uncross price. The execution loop below treats them
    // identically to limit orders queued at that price (they fill at
    // bestUncrossPrice, which is what their "any price" mandate means
    // for a discrete auction). auctionMarketOrders_ is kept around so
    // we can identify and cancel any unfilled remainder afterwards.
    for (Order* m : auctionMarketOrders_) {
        m->price = bestUncrossPrice;
        addToBook(m);
    }

    // Step 2: Execute trades at the uncross price
    Quantity remainingVolume = maxVolume;

    while (remainingVolume > 0) {
        if (bids_.empty()) break;
        Price bestBid = bids_.bestPrice();
        if (bestBid < bestUncrossPrice) break;

        if (asks_.empty()) break;
        Price bestAsk = asks_.bestPrice();
        if (bestAsk > bestUncrossPrice) break;

        OrderList* bidLevel = bids_.bestLevel();
        OrderList* askLevel = asks_.bestLevel();

        Order* buyer = bidLevel->front();
        Order* seller = askLevel->front();

        if (!buyer || !seller) break;

        // SMP check — skip this pair in auction
        if (checkSMP(*buyer, *seller)) {
            bidLevel->remove(buyer);
            notifyOrderUpdate(buyer->id, OrderStatus::Cancelled, 0, buyer->remainingQty);
            orderLookup_.erase(buyer->id);
            orderPool_.deallocate(buyer);
            if (bidLevel->empty()) bids_.eraseBest();
            continue;
        }

        Quantity fillQty = std::min({buyer->remainingQty, seller->remainingQty, remainingVolume});

        buyer->remainingQty -= fillQty;
        seller->remainingQty -= fillQty;
        remainingVolume -= fillQty;

        lastTradePrice_ = bestUncrossPrice;
        lastTradeQty_ = fillQty;
        updateAnalytics(bestUncrossPrice, fillQty, buyer->participantId, seller->participantId);
        otrStats_[buyer->participantId].netPosition += static_cast<int64_t>(fillQty);
        otrStats_[seller->participantId].netPosition -= static_cast<int64_t>(fillQty);

        Trade t{};
        t.tradeId = nextTradeId_++;
        t.buyOrderId = buyer->id;
        t.sellOrderId = seller->id;
        t.buyerId = buyer->participantId;
        t.sellerId = seller->participantId;
        t.price = bestUncrossPrice;
        t.quantity = fillQty;
        t.timestamp = nowNs();
        t.sequenceNumber = nextSequenceNumber_++;
        t.symbolId = symbolId_;
        tradeHistory_.push(t);
        if (!replayMode_) listener_->onTrade(t);

        if (buyer->remainingQty == 0) {
            buyer->status = OrderStatus::Filled;
            notifyOrderUpdate(buyer->id, OrderStatus::Filled, buyer->initialQty, 0, bestUncrossPrice);
            bidLevel->remove(buyer);
            orderLookup_.erase(buyer->id);
            orderPool_.deallocate(buyer);
            if (bidLevel->empty()) bids_.eraseBest();
        }

        if (seller->remainingQty == 0) {
            seller->status = OrderStatus::Filled;
            notifyOrderUpdate(seller->id, OrderStatus::Filled, seller->initialQty, 0, bestUncrossPrice);
            askLevel->remove(seller);
            orderLookup_.erase(seller->id);
            orderPool_.deallocate(seller);
            if (askLevel->empty()) asks_.eraseBest();
        }
    }

    // Step 3: cancel any parked market orders that did not fully fill.
    // A market order is auction-only; once the uncross is done, it does
    // not get to rest as a limit at bestUncrossPrice (it would lie about
    // its admission semantics). Walk auctionMarketOrders_ and cancel
    // anything that still has remainingQty > 0. Fully-filled orders were
    // deallocated by the execution loop above; skip those by checking
    // orderLookup_ first.
    for (Order* m : auctionMarketOrders_) {
        auto* p = orderLookup_.find(m->id);
        if (!p || *p != m) continue;          // already deallocated
        if (m->remainingQty == 0) continue;   // edge: filled-but-still-queued
        Quantity filled = m->initialQty - m->remainingQty;
        removeFromBook(m);
        notifyOrderUpdate(m->id, OrderStatus::Cancelled, filled, 0);
        orderLookup_.erase(m->id);
        orderPool_.deallocate(m);
    }
    auctionMarketOrders_.clear();
}

void OrderBook::cancelAuctionMarketOrders() {
    for (Order* m : auctionMarketOrders_) {
        auto* p = orderLookup_.find(m->id);
        if (!p || *p != m) continue;
        notifyOrderUpdate(m->id, OrderStatus::Cancelled, 0, 0);
        orderLookup_.erase(m->id);
        orderPool_.deallocate(m);
    }
    auctionMarketOrders_.clear();
}

// ─── Market Data Snapshot ────────────────────────────────────────────────────

MarketDataSnapshot OrderBook::getSnapshot(size_t depth) const {
    // Shared lock: many concurrent readers, no writes during the read.
    // Closes the torn-read window documented in spec/Snapshot.tla — a
    // side-flipping cancelReplace can no longer interleave between the
    // bid traversal and the ask traversal.
    std::shared_lock<std::shared_mutex> lock(bookLock_);

    MarketDataSnapshot snap{};
    snap.symbolId = symbolId_;
    snap.lastTradePrice = lastTradePrice_;
    snap.lastTradeQty = lastTradeQty_;
    snap.timestamp = nowNs();

    size_t maxDepth = std::min(depth, MarketDataSnapshot::MAX_DEPTH);

    bids_.forEachLevelWhile([&](Price price, const OrderList& level) -> bool {
        if (snap.bidCount >= maxDepth) return false;
        PriceLevel pl{};
        pl.price = price;
        for (Order* o = level.front(); o; o = o->next) {
            if (!o->isHidden) {
                pl.totalQuantity += (o->type == OrderType::Iceberg) ? o->visibleQty : o->remainingQty;
                pl.orderCount++;
            }
        }
        if (pl.orderCount > 0) {
            snap.bids[snap.bidCount++] = pl;
        }
        return true;
    });

    asks_.forEachLevelWhile([&](Price price, const OrderList& level) -> bool {
        if (snap.askCount >= maxDepth) return false;
        PriceLevel pl{};
        pl.price = price;
        for (Order* o = level.front(); o; o = o->next) {
            if (!o->isHidden) {
                pl.totalQuantity += (o->type == OrderType::Iceberg) ? o->visibleQty : o->remainingQty;
                pl.orderCount++;
            }
        }
        if (pl.orderCount > 0) {
            snap.asks[snap.askCount++] = pl;
        }
        return true;
    });

    return snap;
}

// ─── All Orders (for snapshots) ──────────────────────────────────────────────

size_t OrderBook::getAllOrders(const Order** out, size_t maxOrders) const {
    size_t count = 0;
    orderLookup_.forEach([&](OrderId, Order* order) {
        if (order && count < maxOrders) {
            out[count++] = order;
        }
    });
    return count;
}

// ─── Getters ─────────────────────────────────────────────────────────────────

const Order* OrderBook::getOrder(OrderId orderId) const {
    const auto* ptr = orderLookup_.find(orderId);
    return ptr ? *ptr : nullptr;
}

size_t OrderBook::getBidLevelsCount() const { return bids_.size(); }
size_t OrderBook::getAskLevelsCount() const { return asks_.size(); }

Price OrderBook::getBestBid() const {
    return bids_.bestPrice();
}

Price OrderBook::getBestAsk() const {
    return asks_.bestPrice();
}

Price OrderBook::getMidPrice() const {
    Price bb = getBestBid();
    Price ba = getBestAsk();
    if (bb == 0 || ba == std::numeric_limits<Price>::max()) return 0;
    return (bb + ba) / 2;
}

} // namespace OrderMatcher
