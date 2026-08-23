#include "OrderBook.h"
#include "FaultInjector.h"
#include "LatencyTracker.h"  // for nowNs()
#include "Metrics.h"         // for the per-symbol pool-utilization gauge (P3-8)
#include "StructuredLog.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>

namespace OrderMatcher {

constexpr size_t INITIAL_CAPACITY = 200000;

OrderBook::OrderBook(SymbolId symbolId, MatchAlgorithm algo, size_t orderPoolCapacity)
    : bids_(Side::Buy, 200001), asks_(Side::Sell, 200001),
      orderLookup_(INITIAL_CAPACITY),
      orderPool_(orderPoolCapacity ? orderPoolCapacity : INITIAL_CAPACITY),
      symbolId_(symbolId), matchAlgorithm_(algo),
      participantRisk_(1024), participantOrders_(64) {
    // orderLookup_ is sized to the engine's default pool capacity (>= any custom
    // orderPoolCapacity), so with the 50% load factor its rehash threshold sits
    // well above the max live orders the pool can hand out — it can never rehash
    // during matching. Freeze it so any future sizing regression that breaks that
    // invariant is caught (debug assert) instead of silently paying a
    // stop-the-world rehash on the hot path.
    orderLookup_.disallowRehash();

    // P3-8: per-symbol pool-utilization gauge. The label is embedded in the
    // metric name so each book gets its own series in the simple registry.
    poolGauge_ = &MetricsRegistry::instance().gauge(
        "order_pool_utilization_percent{symbol=\"" + std::to_string(symbolId) + "\"}",
        "Order pool utilization percent (in-use / capacity) for this symbol's book");
}

void OrderBook::updatePoolUtilization(size_t inUse, size_t cap) {
    if (cap == 0) return;
    if (poolGauge_) poolGauge_->set(static_cast<int64_t>((inUse * 100) / cap));

    int band = 0;
    if (inUse >= cap)                 band = 3;   // 100% — exhausted (kill-switch-like)
    else if (inUse * 100 >= cap * 95) band = 2;   // >= 95% — reject new orders
    else if (inUse * 100 >= cap * 80) band = 1;   // >= 80% — warn
    // Log/alert only when pressure escalates, so a full book does not spam.
    if (band > poolPressureBand_) {
        switch (band) {
        case 1:
            obSink().log(obEvent("order_pool_pressure_warn", LogSeverity::Warn)
                             .kv("symbol", (long long)symbolId_)
                             .kv("in_use", (unsigned long long)inUse)
                             .kv("capacity", (unsigned long long)cap));
            break;
        case 2:
            obSink().log(obEvent("order_pool_rejecting_new", LogSeverity::Warn)
                             .kv("symbol", (long long)symbolId_)
                             .kv("in_use", (unsigned long long)inUse)
                             .kv("capacity", (unsigned long long)cap));
            break;
        case 3:
            // 100%: behave like the kill switch — no new orders + a critical alert.
            obSink().log(obEvent("order_pool_exhausted", LogSeverity::Error)
                             .kv("symbol", (long long)symbolId_)
                             .kv("in_use", (unsigned long long)inUse)
                             .kv("capacity", (unsigned long long)cap));
            break;
        default:
            break;
        }
    }
    poolPressureBand_ = band;
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
    // Skip the vtable dispatch when no real listener is wired (the flag is set
    // by setEventListener/setEngineListener). Same guard as the onTrade path.
    if (hasTradeListener_) {
        listener_->onOrderUpdate(u);
        engineListener_->onOrderUpdate(u);
    }
#else
    (void)orderId; (void)status; (void)filledQty; (void)remainingQty; (void)lastFillPrice; (void)reason;
#endif
}

void OrderBook::notifyMarketData(MarketDataUpdate::Action action, Side side, Price price) {
#ifndef OB_LEAN_MODE
    if (replayMode_) return;
    // Early-out before the O(level) walk below. The walk used to run even with
    // no listener attached, only to discard the result at the dispatch check —
    // pure waste on the benchmark configuration, which runs listener-free.
    if (!hasTradeListener_) return;

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
    if (hasTradeListener_) listener_->onMarketData(update);
#else
    (void)action; (void)side; (void)price;
#endif
}

void OrderBook::notifyBookVisible(BookVisibleUpdate::Action action, OrderId orderId,
                                  Side side, Price price, Quantity quantity) {
#ifndef OB_LEAN_MODE
    if (replayMode_) return;

    BookVisibleUpdate update{};
    update.action = action;
    update.orderId = orderId;
    update.side = side;
    update.price = price;
    update.quantity = quantity;
    update.timestamp = nowNs();
    update.sequenceNumber = nextSequenceNumber_++;

    // Public market-data channel: same dispatch shape as notifyMarketData
    // (user listener only). engineListener_ is the internal contingent-order
    // observer and has no interest in the displayed-book projection.
    if (hasTradeListener_) listener_->onBookVisible(update);
#else
    (void)action; (void)orderId; (void)side; (void)price; (void)quantity;
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
        // Center the range around the first price seen. The range
        // is FIXED after this — subsequent orders priced outside
        // [minP, minP + capacity) are rejected with OutOfPriceRange.
        // There is no automatic re-centering today; a symbol whose
        // price moves into a new regime far from its first quote
        // would need an explicit book rebuild. Documented as a
        // known scaling limitation.
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
    stpNoteAdded(order);  // now resting: bump this participant's STP occupancy
    // Public display event. Fired HERE rather than beside the callers'
    // Accepted notify because this is the single choke point all eleven add
    // paths funnel through, and it is the first moment the display quantity
    // is final (the iceberg slice is sized just before the call).
    if (!order->isHidden)
        notifyBookVisible(BookVisibleUpdate::Action::Rest, order->id, order->side,
                          order->price, displayQuantity(*order));
    return true;
}

void OrderBook::removeFromBook(Order* order) {
    // A parked order — Stop / StopLimit / MIT / TrailingStop / Pegged / MOC /
    // LOC, or an auction market order before the uncross — carries a price but
    // was never linked into bids_/asks_, so its next and prev are both null.
    // IntrusiveList::remove reads those pointers to splice: with both null it
    // sets head = null AND tail = null, wiping every genuine order resting at
    // that price. Those orders stay allocated and in orderLookup_ but become
    // unreachable from the book — they can never fill or be cancelled, and
    // their owners are never told. The level is then deactivated as "empty".
    //
    // Cancelling a parked stop whose limit price happens to collide with an
    // active level is enough to trigger it, which is ordinary flow rather than
    // an edge case. stpNoteRemoved already guards on this flag; the book
    // removal has to as well.
    if (!order->inBook) return;
    stpNoteRemoved(order);  // leaving the book: drop STP occupancy
    auto& book = (order->side == Side::Buy) ? bids_ : asks_;
    book.remove(order->price, order);
}

// ─── Risk & Validation ──────────────────────────────────────────────────────

bool OrderBook::checkRiskLimits(ParticipantId participantId, Price price, Quantity qty) {
    const auto* state = participantRisk_.find(OTRKey(participantId, symbolId_));
    if (!state) return true;

    if (state->maxOrderSize > 0 && qty > state->maxOrderSize) return false;

    if (state->maxOrderNotional > 0) {
        // price * qty can exceed int64 for large orders; computing it in int64
        // is signed-overflow UB that wraps negative and silently bypasses the
        // cap. Widen to __int128 so the notional comparison is always exact.
        __int128 notional = (static_cast<__int128>(price) * static_cast<__int128>(qty))
                            / PRICE_PRECISION;
        if (notional > static_cast<__int128>(state->maxOrderNotional)) return false;
    }

    if (state->maxPositionSize > 0) {
        int64_t projected = state->currentExposure + static_cast<int64_t>(qty);
        // std::abs(INT64_MIN) is UB. Convert to a saturating
        // unsigned magnitude that handles the full int64 range.
        uint64_t magnitude = projected < 0
            ? static_cast<uint64_t>(-(projected + 1)) + 1u
            : static_cast<uint64_t>(projected);
        if (magnitude > state->maxPositionSize) return false;
    }

    return true;
}

void OrderBook::setRiskLimits(ParticipantId participantId, const RiskLimits& limits) {
    auto& state = participantRisk_[OTRKey(participantId, symbolId_)];
    state.maxOrderSize = limits.maxOrderSize;
    state.maxOrderNotional = limits.maxOrderNotional;
    state.maxPositionSize = limits.maxPositionSize;
}

bool OrderBook::checkCircuitBreaker(Price price) {
    if (referencePrice_ == 0) return true;
    double deviation = std::abs(static_cast<double>(price - referencePrice_)) / referencePrice_;
    return deviation <= cbThreshold_;
}

bool OrderBook::checkSMP(const Order& incoming, const Order& resting) const {
    // Detect same-participant self-trade. STP mode controls the ACTION
    // (cancel incoming/resting/both) but detection is always on.
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
                          Quantity minQty, bool hidden, bool riskChecksBypassed) {
    std::unique_lock<std::mutex> lock(bookLock_);

    // --- Trading-state admission ---
    // Halted: regulator/auto halt; reject new orders, cancels still flow
    // through cancelOrder() which has no state gate.
    if (tradingState_ == TradingState::Halted) [[unlikely]] {
#ifndef OB_LEAN_MODE
        participantRisk_[OTRKey(participantId, symbolId_)].rejectedOrders++;
#endif
        notifyOrderUpdate(orderId, OrderStatus::Rejected, 0, qty, 0, RejectReason::MarketHalted);
        return RejectReason::MarketHalted;
    }
    // PostClose: scheduled session end. Same effect as Halted but a
    // different reject reason for client clarity.
    if (tradingState_ == TradingState::PostClose) [[unlikely]] {
#ifndef OB_LEAN_MODE
        participantRisk_[OTRKey(participantId, symbolId_)].rejectedOrders++;
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
    if (tradingState_ == TradingState::PreOpen           ||
        tradingState_ == TradingState::AuctionOpen       ||
        tradingState_ == TradingState::AuctionClose      ||
        tradingState_ == TradingState::VolatilityAuction) [[unlikely]] {
        if (type == OrderType::IOC || type == OrderType::FOK) {
#ifndef OB_LEAN_MODE
            participantRisk_[OTRKey(participantId, symbolId_)].rejectedOrders++;
#endif
            notifyOrderUpdate(orderId, OrderStatus::Rejected, 0, qty, 0,
                              RejectReason::OrderTypeNotAllowedInState);
            return RejectReason::OrderTypeNotAllowedInState;
        }
    }

    // --- Input validation ---
    if (qty == 0) [[unlikely]] {
#ifndef OB_LEAN_MODE
        participantRisk_[OTRKey(participantId, symbolId_)].rejectedOrders++;
#endif
        notifyOrderUpdate(orderId, OrderStatus::Rejected, 0, 0, 0, RejectReason::InvalidQuantity);
        return RejectReason::InvalidQuantity;
    }

    // An Iceberg with displayQty == 0 rests with visibleQty == 0. match() then
    // computes available == 0 -> fillQty == 0, emits a zero-quantity trade, and
    // the refresh branch re-slices min(remainingQty, 0) == 0 — so
    // `while (incoming->remainingQty > 0)` never terminates and the matching
    // thread livelocks on a single malformed order. Reject at admission.
    // displayQty > qty is merely nonsensical rather than fatal (every use site
    // already takes min(remainingQty, displayQty)); clamp it so the stored
    // field satisfies displayQty <= initialQty like every other path assumes.
    if (type == OrderType::Iceberg) [[unlikely]] {
        if (displayQty == 0) {
#ifndef OB_LEAN_MODE
            participantRisk_[OTRKey(participantId, symbolId_)].rejectedOrders++;
#endif
            notifyOrderUpdate(orderId, OrderStatus::Rejected, 0, qty, 0,
                              RejectReason::InvalidDisplayQty);
            return RejectReason::InvalidDisplayQty;
        }
        if (displayQty > qty) displayQty = qty;
    }

    if (type != OrderType::Market && type != OrderType::MOC && price <= 0
                 && type != OrderType::Stop && type != OrderType::StopLimit
                 && type != OrderType::TrailingStop && type != OrderType::MIT) [[unlikely]] {
#ifndef OB_LEAN_MODE
        participantRisk_[OTRKey(participantId, symbolId_)].rejectedOrders++;
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
        participantRisk_[OTRKey(participantId, symbolId_)].rejectedOrders++;
#endif
        notifyOrderUpdate(orderId, OrderStatus::Rejected, 0, qty, 0,
                          RejectReason::DuplicateOrderId);
        return RejectReason::DuplicateOrderId;
    }

    // --- P3-8: order-pool pressure, controlled degradation ---
    // Measure utilization and shed NEW orders before the pool is fully
    // exhausted, so the book keeps serving cancels/matches for resting orders.
    // 80% warns, >= 95% rejects with PoolCapacityExceeded, 100% additionally
    // raises a critical (kill-switch-like) alert. Never crashes, never drops
    // silently — the client always gets an explicit reject. This runs before
    // either allocation site (MOC/LOC park and the main path) so both are covered.
    {
        const size_t poolCap = orderPool_.capacity();
        const size_t poolInUseNow = poolCap - orderPool_.available();
        updatePoolUtilization(poolInUseNow, poolCap);
        if (poolCap > 0 && poolInUseNow * 100 >= poolCap * 95) [[unlikely]] {
            ++poolRejects_;
#ifndef OB_LEAN_MODE
            participantRisk_[OTRKey(participantId, symbolId_)].rejectedOrders++;
#endif
            notifyOrderUpdate(orderId, OrderStatus::Rejected, 0, qty, 0,
                              RejectReason::PoolCapacityExceeded);
            return RejectReason::PoolCapacityExceeded;
        }
    }

    // --- Pre-trade risk checks ---
#ifndef OB_LEAN_MODE
    if (!riskChecksBypassed && !checkRiskLimits(participantId, price, qty)) [[unlikely]] {
        participantRisk_[OTRKey(participantId, symbolId_)].rejectedOrders++;
        notifyOrderUpdate(orderId, OrderStatus::Rejected, 0, qty, 0, RejectReason::RiskLimitBreached);
        obSink().log(logOrderRejected(orderId, participantId, "risk_limit_breached"));
        return RejectReason::RiskLimitBreached;
    }

    participantRisk_[OTRKey(participantId, symbolId_)].recordOrderSubmit();
#endif

    // --- Reference price for circuit breaker ---
    if (referencePrice_ == 0 && type != OrderType::Market
                 && type != OrderType::Stop && type != OrderType::StopLimit
                 && type != OrderType::TrailingStop && type != OrderType::MIT) [[unlikely]] {
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
            participantRisk_[OTRKey(participantId, symbolId_)].rejectedOrders++;
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
            // Volatility breach: enter a short volatility auction rather
            // than an outright halt. Orders now accumulate and the
            // indicative/imbalance is published until a reopening cross
            // (resumeVolatilityAuction) returns the book to continuous
            // trading. A manual halt remains a hard halt. The order that
            // tripped the breach is still rejected; later orders are
            // admitted into the auction (no MarketHalted while auctioning).
            tradingState_ = TradingState::VolatilityAuction;
            // Compliance/monitoring contract: a breach must always emit the
            // `breaker_trip` event (symbol/price/ref/threshold_pct). Kept
            // alongside the newer structured `risk.circuit_breaker` event so
            // existing alerting and StructuredLogTest continue to observe it.
            obSink().log(obEvent("breaker_trip", LogSeverity::Warn)
                .kv("symbol", (long long)symbolId_)
                .kv("price", (long long)price)
                .kv("ref", (long long)referencePrice_)
                .kv("threshold_pct", cbThreshold_));
            obSink().log(logCircuitBreaker(symbolId_, cbThreshold_, referencePrice_, price));
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
            participantRisk_[OTRKey(participantId, symbolId_)].rejectedOrders++;
#endif
            notifyOrderUpdate(orderId, OrderStatus::Rejected, 0, qty, 0, RejectReason::PostOnlyWouldCross);
            return RejectReason::PostOnlyWouldCross;
        }
    }

    // --- MOC/LOC: park or release immediately ---
    // MOC (Market-on-Close) and LOC (Limit-on-Close) only execute during the
    // AuctionClose uncross. In every other state they are parked in
    // onCloseOrders_. When the book transitions to AuctionClose,
    // releaseOnCloseOrders() moves them to the appropriate structures.
    // Any LOC remaining unfilled after uncross() is cancelled by cancelLocOrders().
    if (type == OrderType::MOC || type == OrderType::LOC) {
        Order* order = FaultInjector::instance().shouldFail("pool.allocate.fail")
                           ? nullptr : orderPool_.allocate();
        if (!order) [[unlikely]] {
#ifndef OB_LEAN_MODE
            participantRisk_[OTRKey(participantId, symbolId_)].rejectedOrders++;
#endif
            notifyOrderUpdate(orderId, OrderStatus::Rejected, 0, qty, 0, RejectReason::CapacityExhausted);
            return RejectReason::CapacityExhausted;
        }
        order->id = orderId;
        order->inBook = false;  // pool reuse hands back raw memory — start clean
        order->participantId = participantId;
        order->side = side;
        order->price = price;   // only meaningful for LOC
        order->initialQty = qty;
        order->remainingQty = qty;
        order->type = type;
        order->status = OrderStatus::Accepted;
        order->timeInForce = tif;
        order->expiryTime = expiryTime;
        order->stopPrice = 0;
        order->stopLimitPrice = 0;
        order->displayQty = 0;
        order->visibleQty = 0;
        order->pegType = PegType::None;
        order->pegOffset = 0;
        order->trailAmount = 0;
        order->trailRefPrice = 0;
        order->minQty = 0;
        order->isHidden = false;
        order->isStopTriggered = false;
        order->symbolId = symbolId_;
        order->timestamp = nowNs();
        order->next = nullptr;
        order->prev = nullptr;

        orderLookup_.insert(orderId, order);
#ifndef OB_LEAN_MODE
        participantOrders_[participantId].push_back(orderId);
#endif
        notifyOrderUpdate(orderId, OrderStatus::Accepted, 0, qty);
        if (obSinkActive()) obSink().log(logOrderAccepted(orderId, symbolId_, participantId, price, qty));

        if (tradingState_ == TradingState::AuctionClose) {
            if (type == OrderType::MOC) {
                auctionMarketOrders_.push_back(order);
            } else {
                if (!addToBook(order)) {
                    orderLookup_.erase(orderId);
                    orderPool_.deallocate(order);
                    return RejectReason::CapacityExhausted;
                }
                locActiveIds_.push_back(orderId);
            }
        } else {
            onCloseOrders_.push_back(order);
        }
        return orderId;
    }

    // --- Allocate order from pool ---
    // Fault injection: simulate pool exhaustion. The natural path
    // (allocate 200k orders) is too slow for a unit test; this point
    // exercises the CapacityExhausted reject path deterministically.
    Order* order = FaultInjector::instance().shouldFail("pool.allocate.fail")
                       ? nullptr : orderPool_.allocate();
    if (!order) [[unlikely]] {
#ifndef OB_LEAN_MODE
        participantRisk_[OTRKey(participantId, symbolId_)].rejectedOrders++;
#endif
        notifyOrderUpdate(orderId, OrderStatus::Rejected, 0, qty, 0, RejectReason::CapacityExhausted);
        return RejectReason::CapacityExhausted;
    }

    order->id = orderId;
    order->inBook = false;  // pool reuse hands back raw memory — start clean
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
    if (obSinkActive()) obSink().log(logOrderAccepted(orderId, symbolId_, participantId, price, qty));

    // --- Auction Market orders: park for the uncross ---
    // A Market order in an auction state has no limit price; it cannot
    // rest in the FlatPriceMap and must not match continuously (we are
    // accumulating, not matching). Hold it in auctionMarketOrders_;
    // uncross() folds these into volume discovery and inserts them at
    // the discovered price for execution. Any unfilled remainder is
    // cancelled at the end of uncross.
    if (type == OrderType::Market &&
        (tradingState_ == TradingState::PreOpen           ||
         tradingState_ == TradingState::AuctionOpen       ||
         tradingState_ == TradingState::AuctionClose      ||
         tradingState_ == TradingState::VolatilityAuction)) [[unlikely]] {
        auctionMarketOrders_.push_back(order);
        return orderId;
    }

    // --- Stop / StopLimit: park until triggered ---
    if (type == OrderType::Stop || type == OrderType::StopLimit
        || type == OrderType::MIT) [[unlikely]] {
        stopOrders_.push_back(order);
        return orderId;
    }

    // --- Trailing Stop: park and initialize reference ---
    if (type == OrderType::TrailingStop) [[unlikely]] {
        if (side == Side::Buy) {
            order->trailRefPrice = lastTradePrice_ > 0 ? lastTradePrice_ : price;
            order->stopPrice = order->trailRefPrice + trailAmount;
        } else {
            order->trailRefPrice = lastTradePrice_ > 0 ? lastTradePrice_ : price;
            // Floor at 0: a trail wider than the reference would otherwise make
            // the stop price negative (nonsensical; UB for pathological inputs).
            order->stopPrice = (trailAmount <= order->trailRefPrice)
                               ? order->trailRefPrice - trailAmount : 0;
        }
        trailingStopOrders_.push_back(order);
        return orderId;
    }

    // --- Pegged: compute price from reference and rest ---
    if (type == OrderType::Pegged) [[unlikely]] {
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
        if (!addToBook(order)) {
            // Out of price range / depth: don't leave the order marked Accepted
            // but dangling in orderLookup_ and peggedOrders_ without resting.
            notifyOrderUpdate(orderId, OrderStatus::Cancelled, 0, qty);
            peggedOrders_.erase_value(order);
            orderLookup_.erase(orderId);
            orderPool_.deallocate(order);
            return orderId;
        }
        if (!order->isHidden)
            notifyMarketData(MarketDataUpdate::Action::Add, side, order->price);
        return orderId;
    }

    // --- FOK: require full liquidity ---
    if (type == OrderType::FOK) [[unlikely]] {
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
        (tradingState_ == TradingState::PreOpen) ||
        (tradingState_ == TradingState::VolatilityAuction);
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
        if (type == OrderType::IOC || type == OrderType::FOK || type == OrderType::Market) [[unlikely]] {
            Quantity filled = order->initialQty - order->remainingQty;
            OrderStatus st = (filled > 0) ? OrderStatus::PartiallyFilled : OrderStatus::Cancelled;
            notifyOrderUpdate(orderId, st, filled, 0);
            orderLookup_.erase(orderId);
            orderPool_.deallocate(order);
        } else {
            // Rest in book (Limit, PostOnly, Hidden, Iceberg)
            if (type == OrderType::Iceberg) [[unlikely]]
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
    // P1-1 HOIST: incoming->side never changes across the whole match, so the
    // buy/sell flag and the opposite-side (resting) book are loop invariants.
    // Resolve them ONCE here instead of re-selecting them every iteration.
    // Branchless opposite-book select: incoming side is ~50/50 and so
    // unpredictable by any HW branch predictor. Index a pointer array instead
    // of branching. books[isBuy] is the same-side book (isBuy=true -> bids_);
    // the resting/opposite book is books[!isBuy].
    const bool isBuy = (incoming->side == Side::Buy);
    FlatPriceMap* books[2] = {&asks_, &bids_};
    FlatPriceMap& opposite = *books[static_cast<int>(!isBuy)];

    // Market-order sweep protection: cap how far a market order may walk from
    // the arrival touch price so one order can't clear the book at runaway
    // prices. Disabled when marketProtectionPct_ == 0; any unfilled remainder
    // is cancelled by addOrder's post-match handler (Market orders don't rest).
    const bool useMktProt =
        (incoming->type == OrderType::Market && marketProtectionPct_ > 0.0);
    Price mktProtBound = 0;
    if (useMktProt) {
        if (!opposite.empty()) {
            const Price touch = opposite.bestPrice();
            const Price band =
                static_cast<Price>(static_cast<double>(touch) * marketProtectionPct_);
            mktProtBound = isBuy ? touch + band : touch - band;
        }
    }

    // P1-2 checkSMP OFF THE HOT PATH: a self-trade is purely a same-participant
    // test, so the incoming order can only ever self-trade against its OWN
    // resting orders. The per-participant occupancy counter (stpResting_) answers
    // "does this participant have ANY resting order in this book?" in O(1) — no
    // scan. If none, stpClear is true and the per-fill checkSMP()/STP handling is
    // skipped entirely on the common path. If some rest (or the id is outside the
    // counter's range), stpClear is false and the loop runs the exact original STP
    // path, so semantics are preserved. The counter is maintained by
    // stpNoteAdded/stpNoteRemoved at every book entry/exit; it can only overcount
    // (=> stpClear=false, a harmless extra STP pass), never undercount, so a
    // self-trade can never slip past this check. Note it counts BOTH sides, which
    // is at worst slightly conservative vs the old crossable-levels-only scan
    // (the per-fill checkSMP still gates the actual STP action, so no behaviour
    // change) — and O(1) instead of O(depth).
    const bool stpClear = stpNoneResting(incoming->participantId);

    // P1-3/P1-4/P1-5: buffer per-fill side effects on the stack and flush them
    // AFTER the loop rather than firing virtual onTrade dispatches, audit logs
    // and O(1)-lookup erases interleaved with book mutation on every fill.
    //   pendingFills — batched trade events for onTrade + logTradeFill.
    //   toErase      — ids of fully-filled resting orders (their nodes are already
    //                  unlinked from their level and returned to the pool below;
    //                  matching never consults orderLookup_, so the id is dropped
    //                  in one batch instead of a hash erase per fill).
    std::array<FillEvent, kMaxFillsPerOrder> pendingFills;
    int fillCount = 0;
    std::array<OrderId, kMaxFillsPerOrder> toErase;
    int eraseCount = 0;

    // Price levels this sweep consumed from. The L2 incremental feed has to
    // republish them: fills change displayed depth, and nothing in the match
    // path used to say so, leaving every incremental subscriber holding
    // pre-trade quantities forever. Published once per level after the sweep
    // rather than once per fill — notifyMarketData recomputes the whole level,
    // so per-fill would be O(depth) work repeated for each order consumed.
    // Bounded by kMaxFillsPerOrder: a level needs at least one fill to appear.
    std::array<Price, kMaxFillsPerOrder> touchedPrices;
    int touchedCount = 0;
    const Side oppositeSide = isBuy ? Side::Sell : Side::Buy;
    auto publishTouchedLevels = [&]() {
        for (int i = 0; i < touchedCount; ++i)
            notifyMarketData(MarketDataUpdate::Action::Modify, oppositeSide,
                             touchedPrices[i]);
        touchedCount = 0;
    };

    // Flush the batched work: dispatch onTrade, emit trade-fill audit logs, then
    // apply the deferred lookup erases. Order among fills is preserved exactly
    // (== original per-fill order: logTradeFill then onTrade). Called when the
    // buffer fills mid-sweep (flush-and-continue — never truncates a fill) and
    // once after the loop.
    auto flushFills = [&]() {
        const bool dispatchTrades = (!replayMode_ && hasTradeListener_);
        const bool auditLog = obSinkActive();
        // dispatchOne carries the exact per-fill side effects (audit log then
        // onTrade, in the original order), shared by the fast and slow paths so
        // the two cannot drift.
        auto dispatchOne = [&](const Trade& t) {
            if (auditLog) [[unlikely]]
                obSink().log(logTradeFill(t.buyOrderId, t.sellOrderId,
                                          t.symbolId, t.price, t.quantity));
            if (dispatchTrades) { listener_->onTrade(t); engineListener_->onTrade(t); }
        };
        // Fast path: a marketable order almost always produces exactly one fill —
        // dispatch it (and its single deferred erase) directly, skipping the loop
        // setup. Falls back to the batched loop for multi-fill sweeps.
        if (fillCount == 1) [[likely]] {
            dispatchOne(pendingFills[0].trade);
        } else {
            for (int i = 0; i < fillCount; ++i) dispatchOne(pendingFills[i].trade);
        }
        if (eraseCount == 1) {
            orderLookup_.erase(toErase[0]);
        } else {
            for (int i = 0; i < eraseCount; ++i) orderLookup_.erase(toErase[i]);
        }
        fillCount = 0;
        eraseCount = 0;
    };

    while (incoming->remainingQty > 0) {
        // Overflow guard (flush-and-continue): keep at least one free slot for
        // this iteration's fill. A fill fully-filling a resting order also
        // consumes one toErase slot, but eraseCount <= fillCount always, so
        // guarding fillCount covers both buffers.
        //
        // touchedCount needs its OWN guard: a level is recorded before the STP
        // block, and CancelResting / DecreaseResting `continue` without ever
        // producing a fill. A participant resting >=256 orders across distinct
        // price levels and then crossing itself under STP=CancelResting drives
        // touchedCount past 256 while fillCount stays 0 — a stack write past
        // the end of touchedPrices on the matching thread, from public order
        // semantics alone. Only one append happens per iteration, so checking
        // == here keeps every write in [0, 255].
        if (fillCount == kMaxFillsPerOrder ||
            touchedCount == kMaxFillsPerOrder) [[unlikely]] {
            flushFills();
            publishTouchedLevels();
        }

        if (opposite.empty()) [[unlikely]] break;

        Price bestPrice = opposite.bestPrice();

        if (useMktProt) [[unlikely]] {
            if (isBuy  && bestPrice > mktProtBound) break;
            if (!isBuy && bestPrice < mktProtBound) break;
        }

        if (isBuy) {
            if (incoming->type != OrderType::Market && incoming->price < bestPrice) [[likely]]
                break;
        } else {
            if (incoming->type != OrderType::Market && incoming->price > bestPrice) [[likely]]
                break;
        }

        OrderList* level = opposite.bestLevel();
        Order* bookOrder = level->front();

        // Past every break: this level is about to change, by a fill or an STP
        // removal. Recorded after the cross-check so a non-crossing order —
        // the common case — publishes nothing. The sweep walks levels in price
        // order, so comparing against the last entry is enough to dedupe.
        if (touchedCount == 0 || touchedPrices[touchedCount - 1] != bestPrice)
            touchedPrices[touchedCount++] = bestPrice;

        if (!stpClear && checkSMP(*incoming, *bookOrder)) [[unlikely]] {
            // Phase 4: mode-aware STP — action depends on participant config
            STPMode mode = getSTPMode(incoming->participantId);
            STPResult stp = SelfTradeProtection::check(
                incoming->participantId, bookOrder->participantId,
                mode, std::min(incoming->remainingQty,
                    (bookOrder->type == OrderType::Iceberg)
                    ? bookOrder->visibleQty : bookOrder->remainingQty));

            switch (stp.action) {
            case STPResult::Action::CancelIncoming:
                incoming->remainingQty = 0;
                break;
            case STPResult::Action::CancelResting:
                // Remove the resting order and continue matching
                bookOrder->status = OrderStatus::Cancelled;
                notifyOrderUpdate(bookOrder->id, OrderStatus::Cancelled,
                    bookOrder->initialQty - bookOrder->remainingQty, 0);
                stpNoteRemoved(bookOrder);
                level->remove(bookOrder);
                orderLookup_.erase(bookOrder->id);
                orderPool_.deallocate(bookOrder);
                if (level->empty()) opposite.eraseBest();
                continue;  // try next resting order
            case STPResult::Action::CancelBoth:
                incoming->remainingQty = 0;
                bookOrder->status = OrderStatus::Cancelled;
                notifyOrderUpdate(bookOrder->id, OrderStatus::Cancelled,
                    bookOrder->initialQty - bookOrder->remainingQty, 0);
                stpNoteRemoved(bookOrder);
                level->remove(bookOrder);
                orderLookup_.erase(bookOrder->id);
                orderPool_.deallocate(bookOrder);
                if (level->empty()) opposite.eraseBest();
                break;
            case STPResult::Action::DecreaseResting:
                if (stp.decreaseAmount >= bookOrder->remainingQty) {
                    bookOrder->status = OrderStatus::Cancelled;
                    notifyOrderUpdate(bookOrder->id, OrderStatus::Cancelled,
                        bookOrder->initialQty - bookOrder->remainingQty, 0);
                    stpNoteRemoved(bookOrder);
                    level->remove(bookOrder);
                    orderLookup_.erase(bookOrder->id);
                    orderPool_.deallocate(bookOrder);
                    if (level->empty()) opposite.eraseBest();
                } else {
                    bookOrder->remainingQty -= stp.decreaseAmount;
                    // Preserve the iceberg invariant visibleQty <= remainingQty
                    // (no-op for non-iceberg, where visibleQty == 0). Without it
                    // a later fill underflows remainingQty to UINT64_MAX.
                    bookOrder->visibleQty =
                        std::min(bookOrder->visibleQty, bookOrder->remainingQty);
                }
                continue;  // try next resting order
            default:
                incoming->remainingQty = 0;
                break;
            }
            break;
        }

        Quantity available = (bookOrder->type == OrderType::Iceberg)
                             ? bookOrder->visibleQty : bookOrder->remainingQty;
        Quantity fillQty = std::min(incoming->remainingQty, available);

        incoming->remainingQty -= fillQty;
        bookOrder->remainingQty -= fillQty;
        if (bookOrder->type == OrderType::Iceberg) [[unlikely]] bookOrder->visibleQty -= fillQty;

        lastTradePrice_ = bestPrice;
        lastTradeQty_ = fillQty;
        updateAnalytics(bestPrice, fillQty, bookOrder->participantId, incoming->participantId);

        OrderId buyId = isBuy ? incoming->id : bookOrder->id;
        OrderId sellId = isBuy ? bookOrder->id : incoming->id;
        ParticipantId buyerId = isBuy ? incoming->participantId : bookOrder->participantId;
        ParticipantId sellerId = isBuy ? bookOrder->participantId : incoming->participantId;

#ifndef OB_LEAN_MODE
        participantRisk_[OTRKey(buyerId, symbolId_)].currentExposure += static_cast<int64_t>(fillQty);
        participantRisk_[OTRKey(sellerId, symbolId_)].currentExposure -= static_cast<int64_t>(fillQty);
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
        t.aggressorSide = incoming->side;
        tradeHistory_.push(t);
        // P1-3 BATCH FILL EVENTS: buffer the trade instead of firing the onTrade
        // vtable dispatch here; flushFills() dispatches all of them after the loop
        // in the exact same order (and P1-5 emits logTradeFill from the same
        // buffer). tradeHistory_ push stays inline (SPSC ring, not a vtable call),
        // so its write ordering — and every nextSequenceNumber_/nowNs() stamp —
        // is byte-for-byte unchanged.
        pendingFills[fillCount++].trade = t;

        if (bookOrder->remainingQty == 0) [[unlikely]] {
            bookOrder->status = OrderStatus::Filled;
            notifyOrderUpdate(bookOrder->id, OrderStatus::Filled, bookOrder->initialQty, 0, bestPrice);
            stpNoteRemoved(bookOrder);
            level->remove(bookOrder);
            // P1-4 DEFER erase: the node is already unlinked from its level and
            // returned to the pool below, and matching never consults
            // orderLookup_, so drop the id in one batch in flushFills(). No
            // allocate() runs during matching, so the pooled slot cannot be
            // re-handed-out before the deferred erase clears the stale mapping.
            toErase[eraseCount++] = bookOrder->id;
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
            if (!bookOrder->isHidden) {
                // Drain the buffered fills BEFORE publishing the replenished
                // slice. Two ways this goes wrong otherwise: a subscriber that
                // sees the refresh ahead of the executions that emptied the
                // previous slice decrements the NEW slice by the OLD slice's
                // fills; and deferring the event to the end of the sweep would
                // publish a slice size that later fills in this same sweep have
                // already reduced. Flushing here fixes both — and costs nothing
                // in the common case, since a refresh only happens when a slice
                // empties.
                flushFills();
                notifyBookVisible(BookVisibleUpdate::Action::Rest, bookOrder->id,
                                  bookOrder->side, bookOrder->price,
                                  displayQuantity(*bookOrder));
            }
        } else {
            bookOrder->status = OrderStatus::PartiallyFilled;
        }
    }

    // Dispatch any fills still buffered (and apply their deferred erases).
    flushFills();
    // Then the depth changes those fills caused. After flushFills so the wire
    // order is executions-then-depth, and after the loop so each level is
    // published once at its final state rather than once per order consumed.
    publishTouchedLevels();
}

// ─── Match (Pro-Rata) ────────────────────────────────────────────────────────
// Zero-allocation: uses stack-allocated arrays instead of std::vector.

void OrderBook::matchProRata(Order* incoming) {
    struct Alloc { Order* order; Quantity qty; };
    static constexpr size_t MAX_LEVEL_ORDERS = 1024;

    while (incoming->remainingQty > 0) {
        bool isBuy = (incoming->side == Side::Buy);

        // Branchless opposite-book select: incoming side is ~50/50 and so
        // unpredictable by any HW branch predictor. Index a pointer array
        // instead of branching. books[isBuy] is the same-side book
        // (isBuy=true -> bids_); the resting/opposite book is books[!isBuy].
        FlatPriceMap* books[2] = {&asks_, &bids_};
        FlatPriceMap& opposite = *books[static_cast<int>(!isBuy)];
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

        // ── LMM/DMM floor guarantee (pro-rata only) ──────────────────────────
        // DMM/LMM orders are guaranteed at least DMM_FLOOR_PCT of their available
        // qty from this price level. Excess beyond natural pro-rata share is taken
        // from Regular participants' allocations.
        static constexpr double DMM_FLOOR_PCT = 0.40;
        for (size_t i = 0; i < allocCount; ++i) {
            ParticipantRole role = getParticipantRole(allocs[i].order->participantId);
            if (role == ParticipantRole::Regular) continue;
            Quantity avail = (allocs[i].order->type == OrderType::Iceberg)
                ? allocs[i].order->visibleQty : allocs[i].order->remainingQty;
            Quantity floor = static_cast<Quantity>(avail * DMM_FLOOR_PCT + 0.5);
            floor = std::min(floor, toFill);
            if (allocs[i].qty < floor) {
                Quantity bump = floor - allocs[i].qty;
                Quantity remaining_bump = bump;
                for (size_t j = 0; j < allocCount && remaining_bump > 0; ++j) {
                    if (j == i) continue;
                    ParticipantRole rj = getParticipantRole(allocs[j].order->participantId);
                    if (rj != ParticipantRole::Regular) continue;
                    Quantity take = std::min(remaining_bump, allocs[j].qty);
                    allocs[j].qty -= take;
                    remaining_bump -= take;
                }
                allocs[i].qty += (bump - remaining_bump);
                // Do NOT increment allocated — this is a redistribution from Regular,
                // not new volume. Incrementing would corrupt the remainder calculation.
            }
        }

        // Distribute rounding remainder — DMM/LMM get priority, then FIFO for Regular
        Quantity remainder = toFill - allocated;
        // First pass: LMM/DMM
        for (size_t i = 0; i < allocCount && remainder > 0; ++i) {
            ParticipantRole role = getParticipantRole(allocs[i].order->participantId);
            if (role == ParticipantRole::Regular) continue;
            Quantity avail = (allocs[i].order->type == OrderType::Iceberg)
                ? allocs[i].order->visibleQty : allocs[i].order->remainingQty;
            Quantity extra = std::min(remainder, avail - allocs[i].qty);
            allocs[i].qty += extra;
            remainder -= extra;
        }
        // Second pass: Regular (FIFO)
        for (size_t i = 0; i < allocCount && remainder > 0; ++i) {
            ParticipantRole role = getParticipantRole(allocs[i].order->participantId);
            if (role != ParticipantRole::Regular) continue;
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
            participantRisk_[OTRKey(buyerId, symbolId_)].currentExposure += static_cast<int64_t>(fillQty);
            participantRisk_[OTRKey(sellerId, symbolId_)].currentExposure -= static_cast<int64_t>(fillQty);
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
            t.aggressorSide = incoming->side;
            tradeHistory_.push(t);
            if (!replayMode_ && hasTradeListener_) { listener_->onTrade(t); engineListener_->onTrade(t); }

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
                // Safe to fire inline here: unlike match(), pro-rata dispatches
                // onTrade per fill rather than batching, so the executions that
                // emptied the previous slice have already gone out.
                if (!bookOrder->isHidden)
                    notifyBookVisible(BookVisibleUpdate::Action::Rest, bookOrder->id,
                                      bookOrder->side, bookOrder->price,
                                      displayQuantity(*bookOrder));
            } else {
                bookOrder->status = OrderStatus::PartiallyFilled;
            }
        }

        for (size_t i = 0; i < removeCount; ++i) {
            stpNoteRemoved(toRemove[i]);
            level.remove(toRemove[i]);
            orderLookup_.erase(toRemove[i]->id);
            orderPool_.deallocate(toRemove[i]);
        }

        if (level.empty()) {
            opposite.eraseBest();
        }

        // Republish the level this allocation consumed from, for the same
        // reason as the price-time path: fills change displayed depth and
        // nothing else reports it. Once per level, after the removals, so the
        // published state is final. Safe inline here — pro-rata dispatches
        // onTrade per fill rather than batching, so executions already went out.
        notifyMarketData(MarketDataUpdate::Action::Modify,
                         isBuy ? Side::Sell : Side::Buy, bestPrice);
    }
}

// ─── Cancel ──────────────────────────────────────────────────────────────────

void OrderBook::cancelOrder(OrderId orderId) {
    std::unique_lock<std::mutex> lock(bookLock_);
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
    case TradingState::VolatilityAuction: return "VolatilityAuction";
    }
    return "Unknown";
}
}  // namespace

void OrderBook::setTradingState(TradingState s) {
    if (tradingState_ == s) return;  // no-op transitions don't emit events
    // Observability/audit contract: emit `trading_state_change` (symbol/from/to)
    // alongside the newer structured `trading.state_change` event so existing
    // monitoring and StructuredLogTest continue to observe the transition.
    obSink().log(obEvent("trading_state_change")
                     .kv("symbol", (long long)symbolId_)
                     .kv("from", tradingStateName(tradingState_))
                     .kv("to",   tradingStateName(s)));
    obSink().log(logTradingStateChange(symbolId_, tradingStateName(tradingState_), tradingStateName(s)));
    tradingState_ = s;
    if (s == TradingState::AuctionClose) {
        releaseOnCloseOrders();
    }
}

void OrderBook::cancelOrderImpl(OrderId orderId) {
    auto* orderPtr = orderLookup_.find(orderId);
    if (!orderPtr) [[unlikely]] return;

    Order* order = *orderPtr;
    if (!order) return;
#ifndef OB_LEAN_MODE
    participantRisk_[OTRKey(order->participantId, symbolId_)].ordersSubmitted++;
#endif

    // Remove from special tracking lists (FixedVector::erase_value — O(n) swap-erase)
    if (order->type == OrderType::Stop || order->type == OrderType::StopLimit
        || order->type == OrderType::MIT) {
        stopOrders_.erase_value(order);
    } else if (order->type == OrderType::TrailingStop) {
        trailingStopOrders_.erase_value(order);
    } else if (order->type == OrderType::Pegged) {
        peggedOrders_.erase_value(order);
    } else if (order->type == OrderType::MOC || order->type == OrderType::LOC) {
        onCloseOrders_.erase_value(order);
        locActiveIds_.erase_value(order->id);
    }

    // Capture before removeFromBook clears the flag. A parked order was never
    // displayed at this price, so cancelling it changes no visible level and
    // must not publish a Delete for one.
    const bool wasDisplayed = order->inBook && !order->isHidden;
    removeFromBook(order);

    if (wasDisplayed)
        notifyMarketData(MarketDataUpdate::Action::Delete, order->side, order->price);

    Quantity filledQty = order->initialQty - order->remainingQty;
    notifyOrderUpdate(orderId, OrderStatus::Cancelled, filledQty, 0);
    obSink().log(logOrderCancelled(orderId, symbolId_, order->participantId));

    orderLookup_.erase(orderId);
    orderPool_.deallocate(order);
}

// ─── Modify (quantity reduction only, preserves time priority) ───────────────

bool OrderBook::modifyOrder(OrderId orderId, Quantity newQty) {
    std::unique_lock<std::mutex> lock(bookLock_);

    auto* orderPtr = orderLookup_.find(orderId);
    if (!orderPtr) [[unlikely]] return false;

    Order* order = *orderPtr;
    if (!order) return false;

    if (newQty < order->remainingQty) {
        const Quantity displayBefore = displayQuantity(*order);
        order->remainingQty = newQty;
        // Preserve the iceberg invariant visibleQty <= remainingQty (the same
        // clamp the match loop applies). Without it a modify-down below the
        // current slice leaves the order displaying more than it can deliver.
        if (order->type == OrderType::Iceberg)
            order->visibleQty = std::min(order->visibleQty, order->remainingQty);
        const Quantity displayAfter = displayQuantity(*order);

        if (!order->isHidden) {
            notifyMarketData(MarketDataUpdate::Action::Modify, order->side, order->price);
            // Only the displayed shrink is public. A modify-down that consumes
            // an iceberg's reserve without touching its slice changes nothing
            // the market can see, so it emits nothing.
            if (displayAfter < displayBefore)
                notifyBookVisible(BookVisibleUpdate::Action::Reduce, order->id,
                                  order->side, order->price,
                                  displayBefore - displayAfter);
        }
        return true;
    }

    return false;
}

// ─── Cancel/Replace (full amendment, price change loses priority) ────────────

bool OrderBook::cancelReplace(OrderId orderId, Price newPrice, Quantity newQty) {
    std::unique_lock<std::mutex> lock(bookLock_);

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
        if (!order->isHidden) {
            notifyMarketData(MarketDataUpdate::Action::Delete, order->side, oldPrice);
            // Stop displaying it BEFORE the match below. While repriced the
            // order is off the book and acts as an aggressor, so its fills
            // must surface through the resting maker's execution only — as
            // any aggressor's do. Publishing them against the stale entry
            // reported the trade a second time, at the pre-replace size.
            notifyBookVisible(BookVisibleUpdate::Action::Remove, order->id,
                              order->side, oldPrice, 0);
        }

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
        const Quantity displayBefore = displayQuantity(*order);
        if (newQty < order->remainingQty) {
            // Shrink in place: the order keeps its queue position, so this is
            // a reduction rather than a re-add. Same shape as modifyOrder —
            // and it needs the same order-level event, or the displayed feed
            // keeps the old size forever while the L2 feed sees the modify.
            order->remainingQty = newQty;
            if (order->type == OrderType::Iceberg)
                order->visibleQty = std::min(order->visibleQty, order->remainingQty);
            const Quantity displayAfter = displayQuantity(*order);
            if (!order->isHidden && displayAfter < displayBefore)
                notifyBookVisible(BookVisibleUpdate::Action::Reduce, order->id,
                                  order->side, order->price,
                                  displayBefore - displayAfter);
        } else {
            // Quantity increase loses time priority
            removeFromBook(order);
            order->remainingQty = newQty;
            // Re-slice an iceberg against its new size, the same way the
            // initial rest and the refresh path do.
            if (order->type == OrderType::Iceberg)
                order->visibleQty = std::min(order->remainingQty, order->displayQty);
            order->timestamp = nowNs();
            addToBook(order);  // re-announces: back of the queue, new size
        }
        if (!order->isHidden)
            notifyMarketData(MarketDataUpdate::Action::Modify, order->side, order->price);
    }

    return true;
}

// ─── Kill Switch ─────────────────────────────────────────────────────────────

uint64_t OrderBook::cancelAllForParticipant(ParticipantId participantId) {
    std::unique_lock<std::mutex> lock(bookLock_);

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
    std::unique_lock<std::mutex> lock(bookLock_);

    // Collect expired order IDs into stack buffer (avoids heap allocation)
    OrderId toExpire[4096];
    size_t expireCount = 0;

    orderLookup_.forEach([&](OrderId id, Order* order) {
        if (!order || expireCount >= 4096) return;
        if ((order->timeInForce == TimeInForce::GTD || order->timeInForce == TimeInForce::DAY)
            && order->expiryTime != 0 && currentTime >= order->expiryTime) {
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

        if (order->type == OrderType::MIT) {
            // Market-if-Touched: favorable-direction mirror of a stop. A buy
            // triggers when price falls to/through the level; a sell when it
            // rises to/through it.
            if (order->side == Side::Buy) {
                if (lastTradePrice <= order->stopPrice) triggered = true;
            } else {
                if (lastTradePrice >= order->stopPrice) triggered = true;
            }
        } else {
            // Stop / StopLimit: momentum trigger.
            if (order->side == Side::Buy) {
                if (lastTradePrice >= order->stopPrice) triggered = true;
            } else {
                if (lastTradePrice <= order->stopPrice) triggered = true;
            }
        }

        if (triggered) {
            order->isStopTriggered = true;

            bool becameMarket = false;
            if (order->type == OrderType::StopLimit) {
                order->type = OrderType::Limit;
                order->price = order->stopLimitPrice;
            } else if (order->type == OrderType::MIT) {
                order->type = OrderType::Market;   // MIT fires a market order
                becameMarket = true;
            } else {
                order->type = OrderType::Limit;
            }

            stopOrders_.erase_swap(i);
            match(order);

            if (order->remainingQty > 0) {
                // A triggered MIT is a market order — its unfilled remainder
                // must not rest; cancel it (also the addToBook-failure path).
                if (becameMarket || !addToBook(order)) {
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
                // Floor at 0 (see addOrder TrailingStop init).
                order->stopPrice = (order->trailAmount <= order->trailRefPrice)
                                   ? order->trailRefPrice - order->trailAmount : 0;
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
            // A peg moving is a depth change at two levels, and it happens
            // without any client action — nothing else would tell a subscriber.
            // Unpublished, every incremental consumer kept showing the order at
            // a price it had left, indefinitely.
            const Price oldPrice = order->price;
            removeFromBook(order);
            order->price = newPrice;
            order->timestamp = nowNs();
            const bool rested = addToBook(order);
            if (!order->isHidden) {
                notifyMarketData(MarketDataUpdate::Action::Delete, order->side, oldPrice);
                if (rested)
                    notifyMarketData(MarketDataUpdate::Action::Add, order->side, newPrice);
            }
        }

        ++i;
    }
}

// ─── Analytics ───────────────────────────────────────────────────────────────

void OrderBook::updateAnalytics(Price price, Quantity qty, ParticipantId p1, ParticipantId p2) {
    double tradeValue = static_cast<double>(price) * qty;
    // Saturate rather than silently wrap the uint64 VWAP denominator at extreme
    // session volume (which would diverge the next computed VWAP).
    if (qty > std::numeric_limits<Quantity>::max() - totalQty_) [[unlikely]] {
        totalQty_ = std::numeric_limits<Quantity>::max();
    } else {
        vwap_ = (vwap_ * totalQty_ + tradeValue) / static_cast<double>(totalQty_ + qty);
        totalQty_ += qty;
    }

    cumulativePrice_ += price;
    priceUpdates_++;

    if (p1 > 0) participantRisk_[OTRKey(p1, symbolId_)].tradesExecuted++;
    if (p2 > 0) participantRisk_[OTRKey(p2, symbolId_)].tradesExecuted++;
}

// ─── Uncross (Auction) ──────────────────────────────────────────────────────
// Uses stack-allocated FixedVector to avoid heap allocation during auction.

void OrderBook::uncross() {
    std::unique_lock<std::mutex> lock(bookLock_);

    // Discover the clearing price via the shared, non-destructive core —
    // the same logic computeAuctionState() publishes pre-cross, so the
    // indicative feed and the executed price can never disagree.
    AuctionResult res = discoverUncrossPrice();
    if (!res.hasCross) {
        // Nothing crosses: empty / one-sided / non-overlapping book, or
        // parked market orders with no limit prices to anchor on. Parked
        // markets cannot fill — cancel them (no-op if there are none).
        cancelAuctionMarketOrders();
        cancelLocOrders();
        return;
    }

    const Price bestUncrossPrice = res.indicativePrice;
    Quantity remainingVolume = res.pairedVolume;

    // An auction fills against an order's FULL remaining size, not its
    // displayed slice, so an iceberg's slice has to be re-derived after every
    // fill. Two things break without this: visibleQty can end up larger than
    // remainingQty (so the L2 snapshot advertises depth that no longer
    // exists), and the order-level feed — which only ever saw the slice —
    // cannot follow a fill that exceeded it. Re-announcing the order resyncs
    // subscribers to the exact post-fill slice regardless of what the
    // execution message could express.
    auto resliceIceberg = [&](Order* o) {
        if (o->type != OrderType::Iceberg || o->remainingQty == 0) return;
        o->visibleQty = std::min(o->remainingQty, o->displayQty);
        if (!o->isHidden)
            notifyBookVisible(BookVisibleUpdate::Action::Rest, o->id, o->side,
                              o->price, displayQuantity(*o));
    };

    // Insert parked market orders into the book at the discovered price so
    // the execution loop treats them like limit orders queued there.
    // auctionMarketOrders_ is retained to cancel any unfilled remainder.
    for (Order* m : auctionMarketOrders_) {
        m->price = bestUncrossPrice;
        addToBook(m);
    }

    // Execute trades at the uncross price

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
            stpNoteRemoved(buyer);
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
        participantRisk_[OTRKey(buyer->participantId, symbolId_)].currentExposure += static_cast<int64_t>(fillQty);
        participantRisk_[OTRKey(seller->participantId, symbolId_)].currentExposure -= static_cast<int64_t>(fillQty);

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
        if (!replayMode_ && hasTradeListener_) { listener_->onTrade(t); engineListener_->onTrade(t); }

        // After the execution, before either side is torn down — both
        // pointers are still valid here, and a fully-filled order is skipped
        // by the lambda's own guard.
        resliceIceberg(buyer);
        resliceIceberg(seller);

        // Both sides' levels changed. Published per fill rather than batched:
        // an auction runs once per session, so the repeated level walk costs
        // nothing that matters, and it keeps this path obviously correct.
        const Price buyerPrice = buyer->price;
        const Price sellerPrice = seller->price;

        if (buyer->remainingQty == 0) {
            buyer->status = OrderStatus::Filled;
            notifyOrderUpdate(buyer->id, OrderStatus::Filled, buyer->initialQty, 0, bestUncrossPrice);
            stpNoteRemoved(buyer);
            bidLevel->remove(buyer);
            orderLookup_.erase(buyer->id);
            orderPool_.deallocate(buyer);
            if (bidLevel->empty()) bids_.eraseBest();
        }

        if (seller->remainingQty == 0) {
            seller->status = OrderStatus::Filled;
            notifyOrderUpdate(seller->id, OrderStatus::Filled, seller->initialQty, 0, bestUncrossPrice);
            stpNoteRemoved(seller);
            askLevel->remove(seller);
            orderLookup_.erase(seller->id);
            orderPool_.deallocate(seller);
            if (askLevel->empty()) asks_.eraseBest();
        }

        // Publish after the teardown so each level reports its settled state.
        notifyMarketData(MarketDataUpdate::Action::Modify, Side::Buy, buyerPrice);
        notifyMarketData(MarketDataUpdate::Action::Modify, Side::Sell, sellerPrice);
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

    // Cancel any LOC orders that did not fill at the uncross price.
    cancelLocOrders();
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

// ─── On-Close Order Release ──────────────────────────────────────────────────

void OrderBook::releaseOnCloseOrders() {
    for (Order* o : onCloseOrders_) {
        if (o->type == OrderType::MOC) {
            auctionMarketOrders_.push_back(o);
        } else {
            if (addToBook(o)) {
                locActiveIds_.push_back(o->id);
            } else {
                Quantity filled = o->initialQty - o->remainingQty;
                notifyOrderUpdate(o->id, OrderStatus::Cancelled, filled, 0);
                orderLookup_.erase(o->id);
                orderPool_.deallocate(o);
            }
        }
    }
    onCloseOrders_.clear();
}

void OrderBook::cancelLocOrders() {
    for (OrderId id : locActiveIds_) {
        cancelOrderImpl(id);
    }
    locActiveIds_.clear();
}

// ─── Auction price discovery (shared core, non-destructive) ──────────────────
// Single source of truth for the auction clearing price. uncross() drives
// execution from it; computeAuctionState() publishes it as the indicative.
// Both routing through here is what guarantees the pre-cross indicative the
// market sees equals the price the cross actually executes at.
AuctionResult OrderBook::discoverUncrossPrice() const {
    // Parked market orders participate at every candidate price — they have
    // no limit to anchor on, so they add uniformly to both cumulants.
    Quantity marketBuyTotal = 0, marketSellTotal = 0;
    for (Order* m : auctionMarketOrders_) {
        if (m->side == Side::Buy) marketBuyTotal += m->remainingQty;
        else                      marketSellTotal += m->remainingQty;
    }

    // Book-wide totals — the imbalance to publish when nothing crosses (a
    // one-sided book still carries a meaningful NOII imbalance figure).
    Quantity totalBuy = marketBuyTotal, totalSell = marketSellTotal;
    bids_.forEachLevel([&](Price, const OrderList& list) {
        for (Order* o = list.front(); o; o = o->next) totalBuy += o->remainingQty;
    });
    asks_.forEachLevel([&](Price, const OrderList& list) {
        for (Order* o = list.front(); o; o = o->next) totalSell += o->remainingQty;
    });

    AuctionResult res{};
    auto fillNoCross = [&]() {
        res.imbalanceQty  = (totalBuy >= totalSell) ? totalBuy - totalSell
                                                    : totalSell - totalBuy;
        res.imbalanceSide = (totalBuy >= totalSell) ? Side::Buy : Side::Sell;
    };

    // No limit levels on either side → no candidate prices to discover.
    // (Market orders alone cannot anchor a price.)
    if (bids_.empty() && asks_.empty()) {
        fillNoCross();
        return res;
    }

    // Candidate prices: every populated limit level on either side.
    FixedVector<Price, 4096> prices;
    bids_.forEachLevel([&](Price p, const OrderList&) { prices.push_back(p); });
    asks_.forEachLevel([&](Price p, const OrderList&) { prices.push_back(p); });
    std::sort(prices.begin(), prices.end());
    auto* newEnd = std::unique(prices.begin(), prices.end());
    size_t uniqueCount = static_cast<size_t>(newEnd - prices.begin());

    bool     haveBest = false;
    Price    bestPrice = 0;
    Quantity bestVol = 0, bestImb = 0;
    bool     bestBuySurplus = true;

    for (size_t pi = 0; pi < uniqueCount; ++pi) {
        Price p = prices[pi];
        Quantity cumBuy  = marketBuyTotal;
        Quantity cumSell = marketSellTotal;

        bids_.forEachLevel([&](Price bp, const OrderList& list) {
            if (bp >= p) for (Order* o = list.front(); o; o = o->next) cumBuy += o->remainingQty;
        });
        asks_.forEachLevel([&](Price ap, const OrderList& list) {
            if (ap <= p) for (Order* o = list.front(); o; o = o->next) cumSell += o->remainingQty;
        });

        Quantity vol = std::min(cumBuy, cumSell);
        Quantity imb = (cumBuy >= cumSell) ? cumBuy - cumSell : cumSell - cumBuy;
        bool buySurplus = (cumBuy >= cumSell);

        // Deterministic venue tie-break cascade, applied as a strict
        // ordering over candidate prices:
        //   1. maximize executable volume
        //   2. minimize imbalance (unmatched quantity)
        //   3. minimize distance to the reference price (when one exists)
        //   4. market pressure: buy surplus clears higher, sell surplus lower
        bool better;
        if (!haveBest) {
            better = true;
        } else if (vol != bestVol) {
            better = vol > bestVol;
        } else if (imb != bestImb) {
            better = imb < bestImb;
        } else if (referencePrice_ > 0) {
            Price dCur  = p - referencePrice_;
            Price dBest = bestPrice - referencePrice_;
            if (dCur  < 0) dCur  = -dCur;
            if (dBest < 0) dBest = -dBest;
            better = (dCur != dBest) ? (dCur < dBest)
                                     : (bestBuySurplus ? (p > bestPrice) : (p < bestPrice));
        } else {
            better = bestBuySurplus ? (p > bestPrice) : (p < bestPrice);
        }

        if (better) {
            haveBest = true;
            bestPrice = p; bestVol = vol; bestImb = imb; bestBuySurplus = buySurplus;
        }
    }

    if (bestVol == 0) {
        // A two-sided book exists but the spread never closes (best bid <
        // best ask): no candidate price produces any crossing volume.
        fillNoCross();
        return res;
    }

    res.hasCross        = true;
    res.indicativePrice = bestPrice;
    res.pairedVolume    = bestVol;
    res.imbalanceQty    = bestImb;
    res.imbalanceSide   = bestBuySurplus ? Side::Buy : Side::Sell;
    return res;
}

AuctionResult OrderBook::computeAuctionState() const {
    std::unique_lock<std::mutex> lock(bookLock_);
    return discoverUncrossPrice();
}

bool OrderBook::resumeVolatilityAuction() {
    // Pre-check without the lock: tradingState_ is a plain field read in
    // the engine's single-writer-per-book model (same relaxed treatment
    // as isHalted()). uncross() acquires the unique lock itself, so we
    // must not be holding one across the call.
    if (tradingState_ != TradingState::VolatilityAuction) return false;

    uncross();   // reopening cross at the discovered clearing price

    std::unique_lock<std::mutex> lock(bookLock_);
    // Re-anchor the volatility reference to the reopening print so the
    // post-auction circuit-breaker bands measure from the fresh price.
    if (lastTradePrice_ > 0) referencePrice_ = lastTradePrice_;
    tradingState_ = TradingState::Continuous;
    return true;
}

// ─── Market Data Snapshot ────────────────────────────────────────────────────

MarketDataSnapshot OrderBook::getSnapshot(size_t depth) const {
    // Shared lock: many concurrent readers, no writes during the read.
    // Closes the torn-read window documented in spec/Snapshot.tla — a
    // side-flipping cancelReplace can no longer interleave between the
    // bid traversal and the ask traversal.
    std::unique_lock<std::mutex> lock(bookLock_);

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
