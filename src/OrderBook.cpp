#include "OrderBook.h"
#include "Config.h"          // for the per-symbol sizing keys (see below)
#include "FaultInjector.h"
#include "LatencyTracker.h"  // for nowNs()
#include "Metrics.h"         // for the per-symbol pool-utilization gauge (P3-8)
#include "StructuredLog.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <set>
#include <string>

namespace OrderMatcher {

// ─── Per-symbol sizing (config-driven) ──────────────────────────────────────
//
// These sizes are RESIDENT, not reserved: the pool constructs every Order up
// front, and the hash map / price map memset their storage, so an empty book
// faults in its whole footprint at construction. They are therefore the number
// that decides how many symbols fit on a box — see OrderBook.h for the
// per-book arithmetic.
//
// Keys follow the convention every other engine setting uses (Config.h): a
// key=value line in the config file, overridden by OB_<UPPER_KEY> in the
// environment — the same shape as journal_path / OB_JOURNAL_PATH. They are
// documented in config/engine.conf.example.
//
//   order_pool_capacity     OB_ORDER_POOL_CAPACITY      default 10000 slots
//   trade_history_capacity  OB_TRADE_HISTORY_CAPACITY   default 65536 trades
//
// There is deliberately NO key for orderLookup_: it is derived from the pool
// object itself so it cannot be sized independently of it (see the ctor).
constexpr size_t DEFAULT_ORDER_POOL_CAPACITY = 10000;
constexpr size_t DEFAULT_TRADE_HISTORY_CAPACITY = 65536;

// The config a book sizes itself from. OrderBook is constructed by
// MatchingEngine, which takes no config object, so the values are read the way
// the rest of the engine's OB_* settings are: the file named by OB_CONFIG_PATH
// (the environment spelling of main.cpp's `--config`), plus the OB_<KEY>
// environment override Config applies on every get. Parsed once per process;
// env overrides are still re-read per book, so a test can set one and build a
// book.
static const Config& sizingConfig() {
    static Config cfg;
    static const bool loaded = [] {
        if (const char* path = std::getenv("OB_CONFIG_PATH")) cfg.loadFile(path);
        return true;
    }();
    (void)loaded;
    return cfg;
}

// A positive size from config, else the built-in default. Zero/negative/garbage
// means "not configured" rather than "size this book to nothing".
static size_t configuredSize(const char* key, size_t fallback) {
    const int64_t v = sizingConfig().getInt64(key, static_cast<int64_t>(fallback));
    return v > 0 ? static_cast<size_t>(v) : fallback;
}

// requested == 0 keeps the documented ctor semantics: "engine default".
static size_t resolveOrderPoolCapacity(size_t requested) {
    return requested ? requested
                     : configuredSize("order_pool_capacity", DEFAULT_ORDER_POOL_CAPACITY);
}

// RingBuffer asserts a power-of-two size (it masks instead of dividing), so an
// operator's round number is rounded UP rather than rejected at startup. Floor
// of 2 because a 1-slot ring can hold nothing.
static size_t resolveTradeHistoryCapacity() {
    const size_t want = configuredSize("trade_history_capacity",
                                       DEFAULT_TRADE_HISTORY_CAPACITY);
    return std::bit_ceil(want < 2 ? size_t(2) : want);
}

// Next-order prefetch in the matching loops (opt-in, -DENABLE_MATCH_PREFETCH=ON).
// Pulls the node the loop reaches next into L1 while the current fill computes,
// instead of stalling on the pointer chase. Hint only — __builtin_prefetch
// cannot fault and changes no result, so matching semantics are identical
// either way. Compiles to nothing when the flag is off.
// `o` is non-null at all three call sites (loop condition / front() of a level
// the caller is about to dereference), so only ->next needs the guard — as in
// the original e3a1b56, no extra branch.
#ifdef OB_MATCH_PREFETCH
#define OB_PREFETCH_NEXT(o) \
    do { if ((o)->next) __builtin_prefetch((o)->next, 0, 3); } while (0)
#else
#define OB_PREFETCH_NEXT(o) ((void)0)
#endif

OrderBook::OrderBook(SymbolId symbolId, MatchAlgorithm algo, size_t orderPoolCapacity)
    : bids_(Side::Buy, 200001), asks_(Side::Sell, 200001),
      orderPool_(resolveOrderPoolCapacity(orderPoolCapacity)),
      orderLookup_(orderPool_.capacity()),
      symbolId_(symbolId), matchAlgorithm_(algo),
      participantRisk_(1024),
      tradeHistory_(resolveTradeHistoryCapacity()) {
    // orderLookup_ is sized FROM THE POOL ITSELF — orderPool_ is declared
    // before it in OrderBook.h precisely so this read is well-defined — so it
    // holds every order the pool can hand out with room to spare (50% load
    // factor) and can never rehash during matching. It used to be pinned at a
    // fixed 200,000 while orderPoolCapacity was a caller-supplied parameter
    // with no upper bound: any caller asking for a bigger pool got a frozen map
    // it could overflow, which is a stop-the-world rehash — an allocation on
    // the matching thread, and reallocated storage under any concurrent reader.
    // The invariant was stated in a comment and enforced nowhere. Deriving one
    // from the other makes it true by construction, and keeps it true now that
    // the pool size is config-driven: there is no second knob to get wrong.
    // The freeze below then catches a future sizing regression instead of
    // silently paying for it on the hot path.
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
    // An iceberg must never rest with nothing displayed. Enforced HERE, at the
    // choke point every add path funnels through, rather than trusted to the
    // eleven callers — because the trust already failed once. screenMinQty
    // rested a minQty order via a direct addToBook without slicing it, and
    // orders are allocated with visibleQty = 0, so an iceberg taking that route
    // entered the book at ZERO displayed quantity with its full size behind it.
    //
    // That is the second route to the MATCH-1 livelock (modify-to-zero was the
    // first). Price-time printed a zero-size trade before re-slicing; pro-rata
    // summed the level to 0, and eraseBest will not deactivate a populated
    // level, so the matching thread spun forever. Reachable by one client alone:
    // a buy iceberg with minQty against an empty ask side.
    //
    // Slicing rather than refusing, because three callers ignore this function's
    // return value, and a refusal they drop is its own bug. It cannot change any
    // correct path: admission rejects displayQty == 0 and a resting order has
    // remainingQty > 0, so every caller that already slices arrives with
    // visibleQty > 0 and never reaches this branch.
    if (order->type == OrderType::Iceberg && order->visibleQty == 0) [[unlikely]]
        order->visibleQty = std::min(order->remainingQty, order->displayQty);

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
    // is final (the iceberg slice is sized by the caller, or by the guard at
    // the top of this function for a caller that did not).
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
        if (orderNotional(price, qty) > static_cast<__int128>(state->maxOrderNotional))
            return false;
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

// qty and riskChecksBypassed are [[maybe_unused]] because OB_LEAN_MODE compiles
// out the risk-limit block that is their only consumer. Without the attribute a
// lean build dies on -Werror,-Wunused-parameter — which it silently did, because
// no CI job ever built USE_LEAN_MODE=ON while README and CapacityPlanning.md
// both quote lean-mode figures.
RejectReason OrderBook::checkAdmission(ParticipantId participantId, Side side,
                                       Price price, [[maybe_unused]] Quantity qty,
                                       OrderType type,
                                       [[maybe_unused]] bool riskChecksBypassed) {
    // Trading state. A replace is a new admission decision, so the same states
    // that refuse new orders refuse a replace.
    if (tradingState_ == TradingState::Halted)    return RejectReason::MarketHalted;
    if (tradingState_ == TradingState::PostClose) return RejectReason::MarketClosed;

#ifndef OB_LEAN_MODE
    // Risk limits — this is also what bounds an arbitrary quantity increase
    // (maxOrderSize / maxOrderNotional / maxPositionSize).
    if (!riskChecksBypassed && !checkRiskLimits(participantId, price, qty))
        return RejectReason::RiskLimitBreached;
#else
    (void)participantId; (void)riskChecksBypassed;
#endif

    const bool priced = (type == OrderType::Limit    || type == OrderType::IOC  ||
                         type == OrderType::FOK      || type == OrderType::PostOnly ||
                         type == OrderType::Iceberg  || type == OrderType::Hidden);

    // Price band (LULD). Same integer-exact form as addOrder's copy.
    if (priced && priceBandPct_ > 0.0 && referencePrice_ > 0) {
        Price half = static_cast<Price>(
            static_cast<double>(referencePrice_) * priceBandPct_);
        if (price < referencePrice_ - half || price > referencePrice_ + half)
            return RejectReason::OutsidePriceBand;
    }

#ifndef OB_LEAN_MODE
    // Circuit breaker: the TEST only. addOrder additionally transitions the
    // book into VolatilityAuction on a breach; a replace must not move the
    // market's trading state, so it only declines.
    if (priced && !checkCircuitBreaker(price))
        return RejectReason::VolatilityCircuitBreaker;
#endif

    // PostOnly must never cross. Without this a repriced PostOnly matched as
    // an aggressor, which is the exact inversion of what PostOnly means.
    if (type == OrderType::PostOnly) {
        const bool wouldCross = (side == Side::Buy)
            ? (!asks_.empty() && price >= asks_.bestPrice())
            : (!bids_.empty() && price <= bids_.bestPrice());
        if (wouldCross) return RejectReason::PostOnlyWouldCross;
    }

    return RejectReason::None;
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

// ─── addOrder: admission / allocation / park / finalize steps ────────────────
//
// addOrder takes bookLock_ ONCE at the top and then runs a sequence of guards
// before matching. Every helper in this section is one step of that sequence,
// so they all share a single precondition: bookLock_ is ALREADY HELD by the
// caller. bookLock_ is a plain std::mutex and is NOT recursive — a helper that
// re-takes it deadlocks on the spot. They are defined here, in the same TU and
// immediately above their only call site, so the compiler folds them straight
// back into the hot path they were split out of.
//
// Two signalling conventions, chosen to preserve addOrder's early-return shape
// exactly rather than to flatten it into nested conditionals or flags:
//   * admit* / validate* / screen* -> std::optional<...>: nullopt means
//     "passed, keep going"; a value means "addOrder returns this, now".
//   * park*                        -> std::optional<AddOrderResult>: nullopt
//     means "this order type is not handled here, keep going"; a value is the
//     finished result (an OrderId for a parked order, or a reject reason).

// The reject tail shared by every admission failure that counts against the
// participant: bump the reject counter (full mode only), publish the Rejected
// update, hand the reason back so the caller can `return` it.
// Precondition: bookLock_ held.
RejectReason OrderBook::rejectOrder(OrderId orderId,
                                    [[maybe_unused]] ParticipantId participantId,
                                    Quantity qty, RejectReason reason) {
#ifndef OB_LEAN_MODE
    participantRisk_[OTRKey(participantId, symbolId_)].rejectedOrders++;
#endif
    notifyOrderUpdate(orderId, OrderStatus::Rejected, 0, qty, 0, reason);
    return reason;
}

// --- Trading-state admission ---  Precondition: bookLock_ held.
std::optional<RejectReason> OrderBook::admitForTradingState(OrderId orderId,
                                                            ParticipantId participantId,
                                                            Quantity qty, OrderType type) {
    // Halted: regulator/auto halt; reject new orders, cancels still flow
    // through cancelOrder() which has no state gate.
    if (tradingState_ == TradingState::Halted) [[unlikely]]
        return rejectOrder(orderId, participantId, qty, RejectReason::MarketHalted);

    // PostClose: scheduled session end. Same effect as Halted but a
    // different reject reason for client clarity.
    if (tradingState_ == TradingState::PostClose) [[unlikely]]
        return rejectOrder(orderId, participantId, qty, RejectReason::MarketClosed);

    // PreOpen / AuctionOpen / AuctionClose: orders accumulate without
    // continuous matching. IOC/FOK are still rejected — they require
    // immediate fills, which are unavailable until the uncross runs.
    // Market orders ARE accepted here: they get parked in
    // auctionMarketOrders_ and participate in the uncross at the
    // discovered price (see parkNonMatchingOrder + uncross()).
    if (tradingState_ == TradingState::PreOpen           ||
        tradingState_ == TradingState::AuctionOpen       ||
        tradingState_ == TradingState::AuctionClose      ||
        tradingState_ == TradingState::VolatilityAuction) [[unlikely]] {
        if (type == OrderType::IOC || type == OrderType::FOK)
            return rejectOrder(orderId, participantId, qty,
                               RejectReason::OrderTypeNotAllowedInState);
    }
    return std::nullopt;
}

// --- Input validation + duplicate orderId ---  Precondition: bookLock_ held.
// `displayQty` is in/out: an Iceberg's display size is clamped here, at exactly
// the point the original inline block clamped it, and the clamped value is what
// the caller goes on to store on the Order.
std::optional<RejectReason> OrderBook::validateOrderRequest(OrderId orderId,
                                                            ParticipantId participantId,
                                                            Price price, Quantity qty,
                                                            OrderType type,
                                                            Quantity& displayQty) {
    if (qty == 0) [[unlikely]]
        return rejectOrder(orderId, participantId, qty, RejectReason::InvalidQuantity);

    // An Iceberg with displayQty == 0 rests with visibleQty == 0. match() then
    // computes available == 0 -> fillQty == 0, emits a zero-quantity trade, and
    // the refresh branch re-slices min(remainingQty, 0) == 0 — so
    // `while (incoming->remainingQty > 0)` never terminates and the matching
    // thread livelocks on a single malformed order. Reject at admission.
    // displayQty > qty is merely nonsensical rather than fatal (every use site
    // already takes min(remainingQty, displayQty)); clamp it so the stored
    // field satisfies displayQty <= initialQty like every other path assumes.
    if (type == OrderType::Iceberg) [[unlikely]] {
        if (displayQty == 0)
            return rejectOrder(orderId, participantId, qty, RejectReason::InvalidDisplayQty);
        if (displayQty > qty) displayQty = qty;
    }

    if (type != OrderType::Market && type != OrderType::MOC && price <= 0
                 && type != OrderType::Stop && type != OrderType::StopLimit
                 && type != OrderType::TrailingStop && type != OrderType::MIT) [[unlikely]]
        return rejectOrder(orderId, participantId, qty, RejectReason::InvalidPrice);

    // --- Duplicate orderId check ---
    // FlatHashMap::insert silently overwrites on duplicate key, which
    // would orphan the prior order in its price-level list and pool.
    // Reject explicitly so the duplicate is observable to the client
    // and no resources leak.
    if (orderLookup_.find(orderId) != nullptr) [[unlikely]]
        return rejectOrder(orderId, participantId, qty, RejectReason::DuplicateOrderId);

    return std::nullopt;
}

// --- P3-8: order-pool pressure, controlled degradation ---
// Measure utilization and shed NEW orders before the pool is fully
// exhausted, so the book keeps serving cancels/matches for resting orders.
// 80% warns, >= 95% rejects with PoolCapacityExceeded, 100% additionally
// raises a critical (kill-switch-like) alert. Never crashes, never drops
// silently — the client always gets an explicit reject.
// Precondition: bookLock_ held.
std::optional<RejectReason> OrderBook::admitPoolPressure(OrderId orderId,
                                                         ParticipantId participantId,
                                                         Quantity qty) {
    const size_t poolCap = orderPool_.capacity();
    const size_t poolInUseNow = poolCap - orderPool_.available();
    updatePoolUtilization(poolInUseNow, poolCap);
    if (poolCap > 0 && poolInUseNow * 100 >= poolCap * 95) [[unlikely]] {
        ++poolRejects_;
        return rejectOrder(orderId, participantId, qty, RejectReason::PoolCapacityExceeded);
    }
    return std::nullopt;
}

// --- Reference price + price band (LULD) admission filter ---
// Seeds referencePrice_ from the first order carrying a meaningful limit price;
// that seeding is what makes both the band below and the circuit breaker live,
// so it stays attached to it.
//
// Then rejects orders priced outside [ref*(1-pct), ref*(1+pct)] when a band
// is configured and a reference price has been established. Distinct
// from the volatility breaker: this rejects the individual
// order without halting the market, lets subsequent in-band orders
// continue trading. Skipped on order types without a meaningful limit
// price (Market / Stop families / Pegged).
// Precondition: bookLock_ held.
std::optional<RejectReason> OrderBook::admitPriceBand(OrderId orderId,
                                                      ParticipantId participantId,
                                                      Price price, Quantity qty,
                                                      OrderType type) {
    if (referencePrice_ == 0 && type != OrderType::Market
                 && type != OrderType::Stop && type != OrderType::StopLimit
                 && type != OrderType::TrailingStop && type != OrderType::MIT) [[unlikely]] {
        referencePrice_ = price;
    }

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
        if (price < lo || price > hi)
            return rejectOrder(orderId, participantId, qty, RejectReason::OutsidePriceBand);
    }
    return std::nullopt;
}

#ifndef OB_LEAN_MODE
// --- Circuit breaker check ---  Precondition: bookLock_ held.
// Deliberately does NOT go through rejectOrder(): a breaker trip is a venue
// event, not a participant's fault, and the original never counted it against
// the participant's rejectedOrders. Keeping the notify inline preserves that.
std::optional<RejectReason> OrderBook::admitCircuitBreaker(OrderId orderId, Price price,
                                                           Quantity qty, OrderType type) {
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
    return std::nullopt;
}
#endif

// --- Post-Only: reject if would cross the spread ---  Precondition: bookLock_ held.
std::optional<RejectReason> OrderBook::admitPostOnly(OrderId orderId, ParticipantId participantId,
                                                     Side side, Price price, Quantity qty,
                                                     OrderType type) {
    if (type == OrderType::PostOnly) [[unlikely]] {
        bool wouldCross = false;
        if (side == Side::Buy)
            wouldCross = !asks_.empty() && price >= asks_.bestPrice();
        else
            wouldCross = !bids_.empty() && price <= bids_.bestPrice();

        if (wouldCross)
            return rejectOrder(orderId, participantId, qty, RejectReason::PostOnlyWouldCross);
    }
    return std::nullopt;
}

// --- Allocate order from pool + register for O(1) lookup ---
// Shared by the MOC/LOC park path and the main path: both allocated through the
// same fault-injection point, filled the SAME fields in the SAME order, inserted
// into orderLookup_, published Accepted and audit-logged it. Returns nullptr
// when the pool is exhausted; both callers then reject with CapacityExhausted.
//
// Fault injection: simulate pool exhaustion. The natural path (allocate 200k
// orders) is too slow for a unit test; this point exercises the
// CapacityExhausted reject path deterministically.
//
// The parameter list mirrors addOrder's own, in the same order, so the main
// call site reads as a straight forward and the MOC/LOC one as the same list
// with the fields that only mean something to a live book order zeroed.
// Precondition: bookLock_ held.
Order* OrderBook::allocateAndRegisterOrder(OrderId orderId, ParticipantId participantId,
                                           Side side, Price price, Quantity qty, OrderType type,
                                           Price stopPrice, Quantity displayQty,
                                           TimeInForce tif, uint64_t expiryTime,
                                           Price stopLimitPrice, PegType pegType,
                                           Price pegOffset, Price trailAmount,
                                           Quantity minQty, bool hidden) {
    Order* order = FaultInjector::instance().shouldFail("pool.allocate.fail")
                       ? nullptr : orderPool_.allocate();
    if (!order) [[unlikely]] return nullptr;

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

    orderLookup_.insert(orderId, order);
    notifyOrderUpdate(orderId, OrderStatus::Accepted, 0, qty);
    if (obSinkActive()) obSink().log(logOrderAccepted(orderId, symbolId_, participantId, price, qty));
    return order;
}

// --- MOC/LOC: park or release immediately ---
// MOC (Market-on-Close) and LOC (Limit-on-Close) only execute during the
// AuctionClose uncross. In every other state they are parked in
// onCloseOrders_. When the book transitions to AuctionClose,
// releaseOnCloseOrders() moves them to the appropriate structures.
// Any LOC remaining unfilled after uncross() is cancelled by cancelLocOrders().
//
// nullopt => not an on-close order, addOrder carries on to the main path.
// Precondition: bookLock_ held.
std::optional<AddOrderResult> OrderBook::parkOnCloseOrder(OrderId orderId,
                                                          ParticipantId participantId, Side side,
                                                          Price price, Quantity qty, OrderType type,
                                                          TimeInForce tif, uint64_t expiryTime) {
    if (type != OrderType::MOC && type != OrderType::LOC) return std::nullopt;

    // price is only meaningful for LOC; everything a resting/triggering order
    // would need is zeroed.
    Order* order = allocateAndRegisterOrder(orderId, participantId, side, price, qty, type,
                                            /*stopPrice*/ 0, /*displayQty*/ 0, tif, expiryTime,
                                            /*stopLimitPrice*/ 0, PegType::None, /*pegOffset*/ 0,
                                            /*trailAmount*/ 0, /*minQty*/ 0, /*hidden*/ false);
    if (!order) [[unlikely]]
        return AddOrderResult{rejectOrder(orderId, participantId, qty,
                                          RejectReason::CapacityExhausted)};

    if (tradingState_ == TradingState::AuctionClose) {
        if (type == OrderType::MOC) {
            auctionMarketOrders_.push_back(order);
        } else {
            if (!addToBook(order)) {
                orderLookup_.erase(orderId);
                orderPool_.deallocate(order);
                return AddOrderResult{RejectReason::CapacityExhausted};
            }
            locActiveIds_.push_back(orderId);
        }
    } else {
        onCloseOrders_.push_back(order);
    }
    return AddOrderResult{orderId};
}

// --- Pegged: compute price from reference and rest ---
// Both exits in the original returned orderId, so this returns void and the
// caller does. A rest that cannot be placed (out of price range / depth) is torn
// down here rather than left marked Accepted but dangling in orderLookup_ and
// peggedOrders_ without resting.
// Precondition: bookLock_ held.
void OrderBook::restPeggedOrder(Order* order, OrderId orderId, Side side, Price price,
                                Quantity qty, PegType pegType, Price pegOffset) {
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
        notifyOrderUpdate(orderId, OrderStatus::Cancelled, 0, qty);
        peggedOrders_.erase_value(order);
        orderLookup_.erase(orderId);
        orderPool_.deallocate(order);
        return;
    }
    if (!order->isHidden)
        notifyMarketData(MarketDataUpdate::Action::Add, side, order->price);
}

// --- Order types that never match on arrival: park them and we are done ---
// Auction Market park, Stop/StopLimit/MIT park, TrailingStop park, Pegged rest.
// nullopt => this order goes on to match; a value => addOrder returns it.
// Precondition: bookLock_ held.
std::optional<OrderId> OrderBook::parkNonMatchingOrder(Order* order, OrderId orderId, Side side,
                                                       Price price, Quantity qty, OrderType type,
                                                       PegType pegType, Price pegOffset,
                                                       Price trailAmount) {
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

    if (type == OrderType::Pegged) [[unlikely]] {
        restPeggedOrder(order, orderId, side, price, qty, pegType, pegOffset);
        return orderId;
    }

    return std::nullopt;
}

// --- FOK: require full liquidity ---  Precondition: bookLock_ held.
std::optional<RejectReason> OrderBook::screenFOK(Order* order, OrderId orderId, Side side,
                                                 Price price, Quantity qty, OrderType type) {
    if (type == OrderType::FOK) [[unlikely]] {
        if (!checkLiquidity(side, price, qty, type)) {
            orderLookup_.erase(orderId);
            orderPool_.deallocate(order);
            notifyOrderUpdate(orderId, OrderStatus::Rejected, 0, qty, 0, RejectReason::FOKInsufficientLiquidity);
            return RejectReason::FOKInsufficientLiquidity;
        }
    }
    return std::nullopt;
}

// --- Min quantity check ---  Precondition: bookLock_ held.
// nullopt => the order goes on to match. A value => the order was finalized
// here (cancelled, or rested without matching) and addOrder returns it.
std::optional<OrderId> OrderBook::screenMinQty(Order* order, OrderId orderId, Side side,
                                               Price price, Quantity qty, OrderType type,
                                               Quantity minQty) {
    if (minQty > 0 && type != OrderType::FOK) [[unlikely]] {
        if (!checkMinQty(side, price, minQty)) {
            if (type == OrderType::IOC) {
                orderLookup_.erase(orderId);
                orderPool_.deallocate(order);
                notifyOrderUpdate(orderId, OrderStatus::Cancelled, 0, qty);
                return orderId;
            }
            // H3: a limit order may rest here ONLY if it does not cross.
            //
            // checkMinQty counts liquidity at crossing prices only, so a false
            // return still permits some crossable size — just less than
            // minQty. Resting unconditionally therefore parked the order at a
            // price that crosses the opposite side, leaving bid >= ask: an
            // invalid book. (Ask 30 @ 100, buy 100 @ 100 minQty 50 => both
            // rest at 100, locked.)
            //
            // The semantic is a minimum on the FIRST execution: minQty is
            // checked once at admission and the stored field is never read
            // again, so matching has no per-fill minimum to enforce. A
            // non-crossing order can therefore rest and wait for liquidity —
            // that is the useful case and it stays. A crossing one cannot
            // wait, because waiting is what produces the locked book, so it is
            // cancelled, exactly as the IOC branch above does.
            const bool wouldCross = (side == Side::Buy)
                ? (!asks_.empty() && price >= asks_.bestPrice())
                : (!bids_.empty() && price <= bids_.bestPrice());
            if (wouldCross) [[unlikely]] {
                orderLookup_.erase(orderId);
                orderPool_.deallocate(order);
                notifyOrderUpdate(orderId, OrderStatus::Cancelled, 0, qty);
                return orderId;
            }
            addToBook(order);
            if (!order->isHidden)
                notifyMarketData(MarketDataUpdate::Action::Add, side, price);
            return orderId;
        }
    }
    return std::nullopt;
}

// --- Post-match: handle remaining quantity ---  Precondition: bookLock_ held.
// The caller MUST have run finalizeIfStpCancelled() first: the remainingQty == 0
// branch below reports Filled at full initialQty, which is a phantom fill for an
// order STP zeroed without trading.
void OrderBook::finalizeRemainingQty(Order* order, OrderId orderId, Side side, Price price,
                                     OrderType type) {
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
}

// Admission -> allocation -> park -> match -> finalize. bookLock_ is taken once,
// on the first line, and held to the end; none of the helpers above re-take it.
AddOrderResult OrderBook::addOrder(OrderId orderId, ParticipantId participantId, Side side, Price price,
                          Quantity qty, OrderType type, Price stopPrice, Quantity displayQty,
                          TimeInForce tif, uint64_t expiryTime, Price stopLimitPrice,
                          PegType pegType, Price pegOffset, Price trailAmount,
                          Quantity minQty, bool hidden,
                          [[maybe_unused]] bool riskChecksBypassed) {
    std::unique_lock<std::mutex> lock(bookLock_);

    if (auto r = admitForTradingState(orderId, participantId, qty, type)) return *r;
    if (auto r = validateOrderRequest(orderId, participantId, price, qty, type, displayQty)) return *r;
    // Pool pressure runs before EITHER allocation site (the MOC/LOC park and the
    // main path) so both are covered.
    if (auto r = admitPoolPressure(orderId, participantId, qty)) return *r;

    // --- Pre-trade risk checks ---
#ifndef OB_LEAN_MODE
    if (!riskChecksBypassed && !checkRiskLimits(participantId, price, qty)) [[unlikely]] {
        const RejectReason reason =
            rejectOrder(orderId, participantId, qty, RejectReason::RiskLimitBreached);
        obSink().log(logOrderRejected(orderId, participantId, "risk_limit_breached"));
        return reason;
    }

    participantRisk_[OTRKey(participantId, symbolId_)].recordOrderSubmit();
#endif

    if (auto r = admitPriceBand(orderId, participantId, price, qty, type)) return *r;
#ifndef OB_LEAN_MODE
    if (auto r = admitCircuitBreaker(orderId, price, qty, type)) return *r;
#endif
    if (auto r = admitPostOnly(orderId, participantId, side, price, qty, type)) return *r;

    if (auto res = parkOnCloseOrder(orderId, participantId, side, price, qty, type, tif, expiryTime))
        return *res;

    Order* order = allocateAndRegisterOrder(orderId, participantId, side, price, qty, type,
                                           stopPrice, displayQty, tif, expiryTime, stopLimitPrice,
                                           pegType, pegOffset, trailAmount, minQty, hidden);
    if (!order) [[unlikely]]
        return rejectOrder(orderId, participantId, qty, RejectReason::CapacityExhausted);

    if (auto id = parkNonMatchingOrder(order, orderId, side, price, qty, type,
                                       pegType, pegOffset, trailAmount)) return *id;

    if (auto r = screenFOK(order, orderId, side, price, qty, type)) return *r;
    if (auto id = screenMinQty(order, orderId, side, price, qty, type, minQty)) return *id;

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

    // C1: an order STP zeroed has remainingQty == 0 WITHOUT having traded for
    // it. This test must come first, because the remainingQty == 0 branch in
    // finalizeRemainingQty reports Filled at full initialQty — the phantom fill
    // that won OCO groups and cancelled innocent siblings for an order that
    // traded nothing.
    if (finalizeIfStpCancelled(order)) return orderId;

    finalizeRemainingQty(order, orderId, side, price, type);
    return orderId;
}

// ─── Match (Price-Time FIFO) ─────────────────────────────────────────────────

// C1: STP removal of a resting order, shared by all three matching paths —
// price-time match(), matchProRata()'s pre-pass, and the auction uncross. Three
// STP actions (CancelResting, CancelBoth, DecreaseResting-to-zero) perform the
// identical teardown; sharing it means the status they report cannot drift
// apart, which is the class of bug C1 was. CancelledBySTP, never Cancelled: the
// owner did not ask for this. `lvl` must be `book`'s current best level.
void OrderBook::stpCancelRestingOrder(Order* victim, OrderList* lvl, FlatPriceMap& book) {
    victim->status = OrderStatus::CancelledBySTP;
    notifyOrderUpdate(victim->id, OrderStatus::CancelledBySTP,
                      victim->initialQty - victim->remainingQty, 0);

    // Capture before the teardown frees the node.
    const bool     wasDisplayed = victim->inBook && !victim->isHidden;
    const OrderId  victimId     = victim->id;
    const Side     victimSide   = victim->side;
    const Price    victimPrice  = victim->price;

    stpNoteRemoved(victim);
    untrackOrder(victim);
    lvl->remove(victim);
    orderLookup_.erase(victim->id);
    orderPool_.deallocate(victim);
    if (lvl->empty()) book.eraseBest();

    // Publish the removal. Without this an STP-cancelled resting order left
    // the book silently as far as market data was concerned: the public feed
    // kept advertising displayed size that no longer existed and could never
    // trade, indefinitely — phantom liquidity created by the venue's own STP
    // action. Every other path that takes a displayed order off the book
    // publishes this pair; this one did not.
    if (wasDisplayed) {
        notifyMarketData(MarketDataUpdate::Action::Delete, victimSide, victimPrice);
        notifyBookVisible(BookVisibleUpdate::Action::Remove, victimId,
                          victimSide, victimPrice, 0);
    }
}

// C1: terminal handling for an order that STP removed mid-match. Returns true
// if the order was finalized and deallocated, in which case the caller must NOT
// rest it or touch it again.
//
// This exists because match() has FOUR call sites — addOrder, cancelReplace,
// the triggered-stop sweep and the trailing-stop sweep — each with its own
// copy of the post-match "rest it or report it filled" logic. Fixing only
// addOrder's copy (the one the C1 report names) left the other three resting
// an order that STP had just killed. Sharing the check is what makes the
// guarantee hold on every path rather than on the one that was reported.
bool OrderBook::finalizeIfStpCancelled(Order* order) {
    if (order->status != OrderStatus::CancelledBySTP) [[likely]] return false;

    const Quantity filled = order->initialQty - order->remainingQty;
    // Fills against OTHER participants before the self-cross are real: they
    // must still count as an execution so the client sees the quantity and an
    // OCO leg that genuinely traded can still win. Only a zero-fill STP
    // termination reports CancelledBySTP.
    const OrderStatus st = (filled > 0) ? OrderStatus::PartiallyFilled
                                        : OrderStatus::CancelledBySTP;
    order->status = st;
    notifyOrderUpdate(order->id, st, filled, 0);
    orderLookup_.erase(order->id);
    orderPool_.deallocate(order);
    return true;
}

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
    // The mode is loop-invariant — incoming->participantId never changes across
    // the sweep — so resolve it once here instead of re-hashing it on every
    // self-match.
    //
    // NOTE: the mode is deliberately NOT folded into stpClear. Every mode,
    // including DefaultCancelIncoming, prevents the self-cross; there is no
    // "prevention off" setting to short-circuit on. Self-matching is prohibited
    // at essentially every regulated venue, so the unconfigured default
    // prevents too.
    const STPMode stpMode = getSTPMode(incoming->participantId);
    const bool stpClear = stpNoneResting(incoming->participantId);

    // P1-3/P1-4/P1-5: buffer per-fill side effects on the stack and flush them
    // AFTER the loop rather than firing virtual onTrade dispatches, audit logs
    // and O(1)-lookup erases interleaved with book mutation on every fill.
    //   pendingFills — batched trade events for onTrade + logTradeFill.
    //   toErase      — ids of fully-filled resting orders (their nodes are already
    //                  unlinked from their level and returned to the pool below;
    //                  matching never consults orderLookup_, so the id is dropped
    //                  in one batch instead of a hash erase per fill).
    auto stpCancelResting = [&](Order* victim, OrderList* lvl) {
        stpCancelRestingOrder(victim, lvl, opposite);
    };

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
        OB_PREFETCH_NEXT(bookOrder);

        // Past every break: this level is about to change, by a fill or an STP
        // removal. Recorded after the cross-check so a non-crossing order —
        // the common case — publishes nothing. The sweep walks levels in price
        // order, so comparing against the last entry is enough to dedupe.
        if (touchedCount == 0 || touchedPrices[touchedCount - 1] != bestPrice)
            touchedPrices[touchedCount++] = bestPrice;

        if (!stpClear && checkSMP(*incoming, *bookOrder)) [[unlikely]] {
            // Phase 4: mode-aware STP — action depends on participant config
            STPResult stp = SelfTradeProtection::check(
                incoming->participantId, bookOrder->participantId,
                stpMode, std::min(incoming->remainingQty,
                    (bookOrder->type == OrderType::Iceberg)
                    ? bookOrder->visibleQty : bookOrder->remainingQty));

            // NO `default:` — deliberately. The missing NoSelfTrade case fell
            // through to a default that silently zeroed the incoming order, and
            // that silence IS C1. With every action named, adding an
            // STPResult::Action is a -Wswitch -Werror compile error instead of
            // a new silent phantom fill.
            switch (stp.action) {
            case STPResult::Action::NoSelfTrade:
                // Same participant with no explicit mode configured. checkSMP
                // has already established same-participant, so this IS a real
                // self-cross — the incoming order is cancelled, the safe
                // default. What C1 changed is that it is now LABELLED as an STP
                // cancellation instead of reported as a full fill.
                // remainingQty is deliberately left alone; see CancelIncoming.
                incoming->status = OrderStatus::CancelledBySTP;
                break;
            case STPResult::Action::CancelIncoming:
                // NOTE: remainingQty is deliberately left ALONE. The pre-fix
                // code zeroed it to mean "stop matching", but remainingQty == 0
                // already means "fully filled", and post-match computes
                // filledQty as initialQty - remainingQty. Zeroing therefore
                // reported the ENTIRE order quantity as executed — that is
                // where C1's `filledQty == initialQty` came from, and it also
                // erased how much a partially-filled order had really traded.
                // The `break` after this switch exits the match loop on its
                // own, so the status alone is sufficient to stop and to tell
                // post-match not to rest the order.
                incoming->status = OrderStatus::CancelledBySTP;
                break;
            case STPResult::Action::CancelResting:
                stpCancelResting(bookOrder, level);
                continue;  // try next resting order
            case STPResult::Action::CancelBoth:
                incoming->status = OrderStatus::CancelledBySTP;  // qty untouched, see above
                stpCancelResting(bookOrder, level);
                break;
            case STPResult::Action::DecreaseResting:
                if (stp.decreaseAmount >= bookOrder->remainingQty) {
                    stpCancelResting(bookOrder, level);
                } else {
                    bookOrder->remainingQty -= stp.decreaseAmount;
                    // Preserve the iceberg invariant visibleQty <= remainingQty
                    // (no-op for non-iceberg, where visibleQty == 0). Without it
                    // a later fill underflows remainingQty to UINT64_MAX.
                    bookOrder->visibleQty =
                        std::min(bookOrder->visibleQty, bookOrder->remainingQty);
                }
                continue;  // try next resting order
            }
            break;  // STP acted: stop matching this order
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
        tradeHistory_.pushOverwrite(t);
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
            untrackOrder(bookOrder);
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

        // ── C1: STP pre-pass ────────────────────────────────────────────────
        // Resolve self-trade prevention BEFORE computing allocations. The
        // pre-fix code tested checkSMP inside the allocation loop and, on a
        // hit, zeroed remainingQty and returned — which (a) ignored the
        // configured mode entirely and always behaved CancelIncoming,
        // (b) produced the C1 phantom fill, since post-match reads
        // filledQty as initialQty - remainingQty, (c) abandoned the whole
        // sweep including levels holding no same-participant order, and
        // (d) fired even for a resting order due a zero pro-rata share.
        //
        // Doing it here also avoids invalidating the allocation walk: removing
        // a resting order mid-scan would free the node whose ->next the loop is
        // about to read.
        if (!stpNoneResting(incoming->participantId)) [[unlikely]] {
            const STPMode stpMode = getSTPMode(incoming->participantId);
            bool levelChanged = false;
            for (Order* o = level.front(); o; ) {
                Order* next = o->next;   // read before any teardown frees `o`
                if (!checkSMP(*incoming, *o)) { o = next; continue; }

                const Quantity avail = (o->type == OrderType::Iceberg)
                                       ? o->visibleQty : o->remainingQty;
                const STPResult stp = SelfTradeProtection::check(
                    incoming->participantId, o->participantId, stpMode,
                    std::min(incoming->remainingQty, avail));

                switch (stp.action) {
                case STPResult::Action::NoSelfTrade:
                case STPResult::Action::CancelIncoming:
                    // Status only — remainingQty is left intact so the caller's
                    // finalizeIfStpCancelled() reports what really executed.
                    incoming->status = OrderStatus::CancelledBySTP;
                    return;
                case STPResult::Action::CancelBoth:
                    incoming->status = OrderStatus::CancelledBySTP;
                    stpCancelRestingOrder(o, &level, opposite);
                    return;
                case STPResult::Action::CancelResting:
                    stpCancelRestingOrder(o, &level, opposite);
                    levelChanged = true;
                    break;
                case STPResult::Action::DecreaseResting:
                    if (stp.decreaseAmount >= o->remainingQty) {
                        stpCancelRestingOrder(o, &level, opposite);
                    } else {
                        o->remainingQty -= stp.decreaseAmount;
                        // Keep visibleQty <= remainingQty or a later fill
                        // underflows remainingQty to UINT64_MAX.
                        o->visibleQty = std::min(o->visibleQty, o->remainingQty);
                    }
                    levelChanged = true;
                    break;
                }
                o = next;
            }
            // Quantities at this level moved: restart the outer loop so the
            // level totals and allocations are computed from the new state.
            // The level may also be gone entirely, in which case this picks up
            // the next best price.
            if (levelChanged) continue;
        }

        // Calculate total quantity at this level
        Quantity totalLevelQty = 0;
        for (Order* o = level.front(); o; o = o->next) {
            OB_PREFETCH_NEXT(o);
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
            OB_PREFETCH_NEXT(o);
            // Self-crossing orders are already resolved by the pre-pass above.
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
            tradeHistory_.pushOverwrite(t);
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

// Precondition: bookLock_ held. See the declaration for why.
bool OrderBook::ownedBy(OrderId orderId, ParticipantId requester) const {
    if (requester == kAnyParticipant) return true;
    auto* p = orderLookup_.find(orderId);
    if (!p || !*p) return true;   // absent — let not-found handling report it
    return (*p)->participantId == requester;
}

void OrderBook::cancelOrder(OrderId orderId, ParticipantId requester) {
    std::unique_lock<std::mutex> lock(bookLock_);
    if (!ownedBy(orderId, requester)) return;
    cancelOrderImpl(orderId);
}

OrderBook::OrderExposure OrderBook::cancelOrderReleasing(OrderId orderId,
                                                         ParticipantId requester) {
    std::unique_lock<std::mutex> lock(bookLock_);
    OrderExposure e;
    if (!ownedBy(orderId, requester)) {
        e.denied = true;
        return e;   // found stays false: nothing cancelled, nothing to release
    }
    if (auto* p = orderLookup_.find(orderId); p && *p) {
        const Order* o = *p;
        e.participantId = o->participantId;
        e.side          = o->side;
        e.remainingQty  = o->remainingQty;
        e.found         = true;
    }
    cancelOrderImpl(orderId);
    return e;
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

// Drop `order` from every tracking list that holds a raw pointer to it.
//
// These lists outlive the orders they point at, so an entry left behind after
// the order returns to the pool is a dangling pointer — the next sweep over
// that list dereferences freed memory and frees it a second time. Cancel has
// always done this; the fill path did not, which is the bug this exists to
// stop from recurring: any site that returns an order to the pool calls this
// first, whether the order died by cancel, fill, expiry or uncross.
//
// Type-gated on purpose. erase_value is an O(n) scan of up to 16384 entries,
// which must not run on the matching hot path for the plain Limit orders that
// are never in any of these lists.
// Structural invariant: the price-level lists and orderLookup_ must describe
// exactly the same set of live orders. Every drift this engine has had showed
// up here first — a node linked into two levels (freed once, dangling in the
// other), or a node erased from the lookup but left linked (depth that
// overstates what the book can actually trade).
bool OrderBook::validateIntegrity(std::string* err) const {
    std::unique_lock<std::mutex> lock(bookLock_);

    std::set<OrderId> seen;
    std::string problem;

    auto checkSide = [&](const FlatPriceMap& book, Side side, const char* label) {
        book.forEachLevel([&](Price levelPrice, const OrderList& level) {
            for (Order* o = level.front(); o; o = o->next) {
                if (!problem.empty()) return;
                auto fail = [&](const std::string& why) {
                    problem = std::string(label) + " level " + std::to_string(levelPrice)
                            + ": order #" + (o ? std::to_string(o->id) : std::string("?"))
                            + " " + why;
                };
                if (!seen.insert(o->id).second) {
                    fail("appears in the book more than once (double-linked node)");
                    return;
                }
                auto* looked = orderLookup_.find(o->id);
                if (!looked) {
                    fail("is linked in the book but absent from orderLookup_");
                    return;
                }
                if (*looked != o) {
                    fail("is linked in the book but orderLookup_ maps its id elsewhere");
                    return;
                }
                if (!o->inBook) {
                    fail("is linked in the book with inBook == false");
                    return;
                }
                if (o->price != levelPrice) {
                    fail("sits at the wrong price level (order says "
                         + std::to_string(o->price) + ")");
                    return;
                }
                if (o->side != side) {
                    fail("is on the wrong side of the book");
                    return;
                }
            }
        });
    };

    checkSide(bids_, Side::Buy, "bid");
    if (problem.empty()) checkSide(asks_, Side::Sell, "ask");

    // The other direction: anything orderLookup_ calls resting must be linked.
    if (problem.empty()) {
        orderLookup_.forEach([&](OrderId id, Order* o) {
            if (!problem.empty() || !o) return;
            if (o->inBook && seen.find(id) == seen.end()) {
                problem = "order #" + std::to_string(id) +
                          " has inBook == true but is not linked into any price level";
            }
        });
    }

    if (!problem.empty()) {
        if (err) *err = problem;
        return false;
    }
    return true;
}

void OrderBook::untrackOrder(Order* order) {
    if (!order) return;
    switch (order->type) {
        case OrderType::Stop:
        case OrderType::StopLimit:
        case OrderType::MIT:
            stopOrders_.erase_value(order);
            break;
        case OrderType::TrailingStop:
            trailingStopOrders_.erase_value(order);
            break;
        case OrderType::Pegged:
            peggedOrders_.erase_value(order);
            break;
        case OrderType::MOC:
            // An MOC moves from onCloseOrders_ into auctionMarketOrders_ when
            // the closing cross releases it, so it can be sitting in either.
            onCloseOrders_.erase_value(order);
            locActiveIds_.erase_value(order->id);
            auctionMarketOrders_.erase_value(order);
            break;
        case OrderType::LOC:
            onCloseOrders_.erase_value(order);
            locActiveIds_.erase_value(order->id);
            break;
        case OrderType::Market:
            // A market order submitted during an auction parks here until the
            // uncross. Missing this was a live dangling pointer: cancelling a
            // parked market order freed it while auctionMarketOrders_ kept
            // pointing at the slot, the pool re-handed that slot to an
            // unrelated new order, and the next uncross() repriced THAT order
            // and addToBook'd it a second time — one node in two price levels.
            auctionMarketOrders_.erase_value(order);
            break;
        default:
            break;
    }
}

void OrderBook::cancelOrderImpl(OrderId orderId) {
    auto* orderPtr = orderLookup_.find(orderId);
    if (!orderPtr) [[unlikely]] return;

    Order* order = *orderPtr;
    if (!order) return;
    // No ordersSubmitted++ here: a cancel is not a submission. Counting it
    // inflated getOTR() (and the /otr admin endpoint) on every cancel, so a
    // participant that quotes and pulls normally reads as an OTR abuser.
    // Submissions are counted once, at admission (recordOrderSubmit).

    untrackOrder(order);

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

bool OrderBook::modifyOrder(OrderId orderId, Quantity newQty,
                            ParticipantId requester) {
    RejectReason ignored = RejectReason::None;
    return modifyOrder(orderId, newQty, ignored, requester);
}

bool OrderBook::modifyOrder(OrderId orderId, Quantity newQty, RejectReason& reason,
                            ParticipantId requester) {
    std::unique_lock<std::mutex> lock(bookLock_);
    reason = RejectReason::None;

    auto* orderPtr = orderLookup_.find(orderId);
    if (!orderPtr) [[unlikely]] { reason = RejectReason::OrderNotFound; return false; }

    Order* order = *orderPtr;
    if (!order) { reason = RejectReason::OrderNotFound; return false; }

    if (!ownedBy(orderId, requester)) [[unlikely]] {
        reason = RejectReason::NotOrderOwner;
        return false;
    }

    // Zero is arithmetically a reduction and is not a modify. Applied, it left
    // the order LINKED IN ITS LEVEL at zero quantity, and the two match
    // algorithms then failed differently: price-time published a TRADE OF
    // QUANTITY ZERO to the tape and to both participants before dropping the
    // order (so an aggressor expecting the displayed 50 received nothing), and
    // pro-rata hit `if (totalLevelQty == 0) { opposite.eraseBest(); continue; }`
    // — where eraseBest correctly declines to deactivate a level that still
    // holds orders, making it a no-op, so the matching thread spun forever on
    // the same zero total. One symbol permanently dead per malformed amendment.
    //
    // A client that means "remove it" has cancel. cancelReplace has refused this
    // same quantity since it was written; this path just never asked.
    if (newQty == 0) [[unlikely]] { reason = RejectReason::InvalidQuantity; return false; }

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

    // Found, but newQty is not a reduction. Growing in place would forfeit time
    // priority anyway, so that is a cancel-replace, not a modify — and
    // answering OrderNotFound told the client its live order had vanished.
    reason = RejectReason::InvalidQuantity;
    return false;
}

// ─── Cancel/Replace (full amendment, price change loses priority) ────────────

bool OrderBook::cancelReplace(OrderId orderId, Price newPrice, Quantity newQty,
                              ParticipantId requester) {
    RejectReason ignored = RejectReason::None;
    return cancelReplace(orderId, newPrice, newQty, ignored, requester);
}

bool OrderBook::cancelReplace(OrderId orderId, Price newPrice, Quantity newQty,
                              RejectReason& reason, ParticipantId requester) {
    std::unique_lock<std::mutex> lock(bookLock_);
    reason = RejectReason::OrderNotFound;

    auto* orderPtr = orderLookup_.find(orderId);
    if (!orderPtr) [[unlikely]] return false;

    Order* order = *orderPtr;
    if (!order) return false;

    if (!ownedBy(orderId, requester)) [[unlikely]] {
        reason = RejectReason::NotOrderOwner;
        return false;
    }

    if (newQty == 0) [[unlikely]] { reason = RejectReason::InvalidQuantity; return false; }
    if (newPrice <= 0) [[unlikely]] { reason = RejectReason::InvalidPrice; return false; }

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
        reason = RejectReason::OrderTypeNotAllowedInState;
        return false;
    }

    // A parked order is not a resting order. MOC, LOC-before-release and a
    // market order held for an auction all live in their own tracking list
    // with inBook == false, waiting to be released into the book later.
    // Repricing one used to run the resting-order path below — removeFromBook
    // (a silent no-op, it was never in the book) and then addToBook — which
    // put a parked order into the LIVE book while it was still queued for
    // release. releaseOnCloseOrders() then handed that same node to uncross(),
    // which addToBook'd it a second time: one node linked into two price
    // levels at once. The uncross freed it via one level and left the other
    // pointing at freed memory, so the next match() over that level freed it
    // again — a double free, and before that a book whose level totals
    // disagreed with their own contents.
    //
    // Gate on the invariant rather than extending the type list above: that
    // list has already drifted once (it never covered MOC/LOC), and inBook is
    // the property that actually decides whether the code below is valid.
    if (!order->inBook) [[unlikely]] {
        reason = RejectReason::OrderTypeNotAllowedInState;
        return false;
    }

    // H2: a replace is a fresh admission decision. Previously cancelReplace
    // ran only the qty/price sanity checks above, so it bypassed the trading
    // state gate, risk limits (hence arbitrary quantity increases), the LULD
    // price band, the circuit breaker, and — most seriously — the PostOnly
    // would-cross test, letting a repriced PostOnly match as an AGGRESSOR.
    // (Pool pressure is not in this list: cancelReplace reuses the existing
    // Order and never calls orderPool_.allocate(), so there is nothing to
    // shed.) Rejecting leaves the original order untouched and resting.
    if (const RejectReason admit =
            checkAdmission(order->participantId, order->side, newPrice, newQty,
                           order->type, /*riskChecksBypassed=*/false);
        admit != RejectReason::None) [[unlikely]] {
        // Was flattened to a bare false, so every admission failure — risk
        // limit, price band, breaker, post-only would-cross — reached the
        // client as "order not found".
        reason = admit;
        return false;
    }

    reason = RejectReason::None;
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

        // A repriced order acts as an aggressor, so it can self-cross too.
        if (finalizeIfStpCancelled(order)) return true;

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

    // Scan the live book. This used to consult a per-participant index
    // (participantOrders_), which was wrong three ways at once: it was a
    // FixedVector<OrderId, 4096> whose push_back return was ignored, so once
    // full it silently stopped recording; cancelOrderImpl never removed from
    // it, so it filled with the ids of long-dead orders; and the sweep copied
    // only its first 4096 entries before clearing the whole thing. For any
    // participant past 4096 lifetime orders in a book that combination
    // cancelled nothing — every id filtered out by contains() — while leaving
    // their live orders resting AND untracked, so a second call could not find
    // them either. An engaged kill switch that silently leaves orders working
    // is worse than one that fails loudly.
    //
    // orderLookup_ is the authoritative record, so ask it directly and drop the
    // index entirely. ponytail: O(lookup capacity) per sweep instead of O(live
    // orders for this participant). The kill switch is a rare control-plane
    // operation and correctness beats speed here; this is also exactly what the
    // OB_LEAN_MODE build already did.
    constexpr size_t kBatch = 4096;
    uint64_t count = 0;
    OrderId ids[kBatch];

    // Re-scan until a pass finds nothing: one pass caps out at kBatch, and
    // cancelOrderImpl mutates orderLookup_, so collect then cancel.
    for (;;) {
        size_t n = 0;
        orderLookup_.forEach([&](OrderId id, Order* order) {
            if (order && order->participantId == participantId && n < kBatch) {
                ids[n++] = id;
            }
        });
        if (n == 0) break;
        for (size_t i = 0; i < n; ++i) {
            if (orderLookup_.contains(ids[i])) {
                cancelOrderImpl(ids[i]);  // already holding the lock
                ++count;
            }
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

    // ponytail: the scan is O(all resting stops) on every sweep, ~1.8 ns each
    // here, so a full 16384-entry stopOrders_ costs ~30 us of pure scanning
    // whether or not anything is elected — and the sweep runs on every order
    // once the book has ever traded, not only on orders that print. That is a
    // SECOND latency term, independent of the execution bound below, and this
    // is its ceiling. Upgrade path if it matters: index stopOrders_ by trigger
    // price (two price-ordered buckets, buy-side and sell-side) so a sweep
    // touches only the stops the print actually reached.

    // Executions performed in THIS sweep, against kMaxStopExecutionsPerSweep.
    // Elections beyond the cap are latched, not dropped — see the constant.
    size_t executed = 0;

    // Index-based iteration with erase_swap (FixedVector compatible)
    size_t i = 0;
    while (i < stopOrders_.size()) {
        Order* order = stopOrders_[i];

        // Already elected by an earlier sweep that ran out of execution budget.
        // The election is NOT re-decided here: a stop elected by a print that
        // went through its level must fire even if the price has since come
        // back, or the bound would silently cancel it instead of deferring it.
        bool triggered = order->isStopTriggered;

        if (!triggered) {
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
        }

        if (triggered) {
            // Latch the election before deciding whether there is budget to act
            // on it, so the deferred ones are the SAME set this print elected.
            order->isStopTriggered = true;

            if (executed >= kMaxStopExecutionsPerSweep) [[unlikely]] {
                // Out of budget: leave it parked and latched. The next sweep
                // (every addOrder runs one once the book has traded) executes
                // it. It stays cancellable by its owner until then.
                ++i;
                continue;
            }
            ++executed;

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

            // A triggered stop becomes an aggressor and can self-cross.
            // erase_swap already moved the last element here, so `continue`
            // without incrementing i, exactly as the tail of this block does.
            if (finalizeIfStpCancelled(order)) continue;

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

    // Same fan-out bound as checkStopOrders: one print can elect every trailing
    // stop at once, and each election is a full match(). See
    // kMaxStopExecutionsPerSweep.
    size_t executed = 0;

    size_t i = 0;
    while (i < trailingStopOrders_.size()) {
        Order* order = trailingStopOrders_[i];

        // Latched by an earlier sweep that ran out of budget. Skipping the
        // ratchet as well as the trigger test is the point: a deferred election
        // whose trail kept following the price could stop qualifying, which
        // would cancel it rather than defer it.
        bool triggered = order->isStopTriggered;

        if (!triggered) {
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
        }

        if (triggered) {
            order->isStopTriggered = true;

            if (executed >= kMaxStopExecutionsPerSweep) [[unlikely]] {
                ++i;
                continue;
            }
            ++executed;

            order->type = OrderType::Limit;
            order->price = order->stopPrice;
            trailingStopOrders_.erase_swap(i);
            match(order);

            if (finalizeIfStpCancelled(order)) continue;

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

        // ── C1: mode-aware STP in the auction cross ─────────────────────────
        // The pre-fix code ignored stpModes_ entirely and always cancelled the
        // BUYER — an arbitrary side bias — reporting it as a client-initiated
        // Cancelled.
        //
        // An auction has no aggressor: both orders were resting before the
        // uncross, so CancelIncoming / CancelResting / DecreaseResting have no
        // literal referent. "Incoming" maps to the LATER-ARRIVING order by
        // timestamp: price-time is the auction's own ordering, and the newer
        // order is the one that created the self-cross. Ties (equal timestamps)
        // fall to the buyer, preserving the historical side for that case.
        //
        // Whose mode applies is unambiguous — checkSMP has already established
        // both orders belong to the same participant, so the two lookups are
        // the same lookup.
        //
        // KNOWN LIMITATION (pre-existing, widened by CancelBoth): bestUncrossPrice
        // and remainingVolume were discovered from a book that still contained
        // these orders. Removing them here means the printed price and volume
        // can exceed what the post-STP book supports. A correct fix filters
        // self-crossing pairs BEFORE price discovery; that is a larger change
        // than making this path mode-aware.
        if (checkSMP(*buyer, *seller)) {
            const STPMode stpMode = getSTPMode(buyer->participantId);
            const STPResult stp = SelfTradeProtection::check(
                buyer->participantId, seller->participantId, stpMode,
                std::min(buyer->remainingQty, seller->remainingQty));

            const bool buyerIsNewer = (buyer->timestamp >= seller->timestamp);
            Order*     newer     = buyerIsNewer ? buyer    : seller;
            OrderList* newerLvl  = buyerIsNewer ? bidLevel : askLevel;
            FlatPriceMap& newerBk = buyerIsNewer ? bids_   : asks_;
            Order*     older     = buyerIsNewer ? seller   : buyer;
            OrderList* olderLvl  = buyerIsNewer ? askLevel : bidLevel;
            FlatPriceMap& olderBk = buyerIsNewer ? asks_   : bids_;

            switch (stp.action) {
            case STPResult::Action::NoSelfTrade:
            case STPResult::Action::CancelIncoming:
                stpCancelRestingOrder(newer, newerLvl, newerBk);
                break;
            case STPResult::Action::CancelResting:
                stpCancelRestingOrder(older, olderLvl, olderBk);
                break;
            case STPResult::Action::CancelBoth:
                stpCancelRestingOrder(newer, newerLvl, newerBk);
                stpCancelRestingOrder(older, olderLvl, olderBk);
                break;
            case STPResult::Action::DecreaseResting:
                if (stp.decreaseAmount >= older->remainingQty) {
                    stpCancelRestingOrder(older, olderLvl, olderBk);
                } else {
                    older->remainingQty -= stp.decreaseAmount;
                    // Keep visibleQty <= remainingQty or a later fill
                    // underflows remainingQty to UINT64_MAX.
                    older->visibleQty = std::min(older->visibleQty, older->remainingQty);
                }
                break;
            }
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
        tradeHistory_.pushOverwrite(t);
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

        // Retiring a fully-filled order in the cross. The per-order removal
        // MUST be published: an auction fills against an order's full
        // remaining size, so an iceberg's slice is re-derived and re-announced
        // (Rest) after every fill. If the order is then exhausted and freed
        // silently, the last thing the feed heard about it was that fresh
        // slice — and every downstream book keeps displaying it forever, size
        // that no longer exists and can never trade. The level-scoped Modify
        // published below updates L2 aggregates but says nothing about the
        // order, so it cannot retire the entry either.
        auto retireFilled = [&](Order* o, OrderList* lvl, FlatPriceMap& book) {
            const bool    wasDisplayed = o->inBook && !o->isHidden;
            const OrderId id           = o->id;
            const Side    side         = o->side;
            const Price   px           = o->price;

            o->status = OrderStatus::Filled;
            notifyOrderUpdate(id, OrderStatus::Filled, o->initialQty, 0, bestUncrossPrice);
            stpNoteRemoved(o);
            untrackOrder(o);
            lvl->remove(o);
            orderLookup_.erase(id);
            orderPool_.deallocate(o);
            if (lvl->empty()) book.eraseBest();

            if (wasDisplayed)
                notifyBookVisible(BookVisibleUpdate::Action::Remove, id, side, px, 0);
        };

        if (buyer->remainingQty == 0) {
            retireFilled(buyer, bidLevel, bids_);
        }

        if (seller->remainingQty == 0) {
            retireFilled(seller, askLevel, asks_);
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
        // removeFromBook before freeing, exactly as uncross()'s own cancel
        // pass does. A parked market order is normally not in the book, but
        // "normally" is what the double free above relied on: if anything
        // ever rests one, freeing it here would leave its level pointing at
        // freed memory. Free nothing that is still linked.
        removeFromBook(m);
        untrackOrder(m);
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
    // H4: this was a range-for over locActiveIds_ while cancelOrderImpl
    // swap-erases from that same container (:1580), which broke two ways.
    //
    //  1. Skipping. erase_swap(i) moves the LAST element into slot i; the
    //     iterator then advances to i+1, so the relocated element is never
    //     visited. Roughly half the LOC orders were skipped.
    //  2. Stale tail reads. Range-for captures __end once, at the original
    //     size_, so the loop kept reading indices past the shrunken size_.
    //     Those slots are valid storage in a fixed T[16384] holding
    //     already-processed ids, so this is NOT out of bounds and no
    //     sanitizer reports it — it just silently does nothing.
    //
    // The survivors then stayed resting while the trailing clear() wiped the
    // tracking list, orphaning them: live in the book, invisible to every
    // later LOC sweep.
    //
    // Drain from the tail instead. Copying the id list first would be 128 KB
    // of stack for a 16384-entry FixedVector; taking the last element is O(1)
    // and immune to the swap, because the element erase_swap relocates is the
    // one we are already holding.
    while (!locActiveIds_.empty()) {
        const size_t last = locActiveIds_.size() - 1;
        const OrderId id = locActiveIds_[last];
        cancelOrderImpl(id);
        // cancelOrderImpl erases the id itself for a live LOC order. If the
        // order was ALREADY gone it early-returns on the orderLookup_ miss
        // without erasing, so drop the entry here or this loop never
        // terminates.
        if (!locActiveIds_.empty() &&
            locActiveIds_.size() - 1 == last &&
            locActiveIds_[last] == id) {
            locActiveIds_.erase_swap(last);
        }
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
    // EXCLUSIVE lock, not a shared one — this comment used to say "shared lock:
    // many concurrent readers" and that stopped being true when bookLock_ was
    // changed from std::shared_mutex to a plain std::mutex (the uncontended
    // write-lock of a shared_mutex costs more than a mutex, and every real
    // caller here writes). Readers serialise against each other and against
    // matching.
    //
    // Closes the torn-read window documented in spec/Snapshot.tla — a
    // side-flipping cancelReplace can no longer interleave between the bid
    // traversal and the ask traversal.
    //
    // SO AN ADMIN READ CAN STALL MATCHING, AND THE COST DEPENDS ENTIRELY ON
    // QUEUE DEPTH. /book and /audit reach this through
    // MatchingEngine::getSnapshot, so a dashboard polling them takes the same
    // lock addOrder needs.
    //
    // The cost is O(depth x ORDERS-PER-LEVEL), not O(book size) and not
    // O(levels): the loop above stops after maxDepth levels, but walks every
    // order inside each one. Measured hold (benchmarks/SnapshotContentionBenchmark):
    //
    //   orders/level   hold P50    duty at a 1,000 req/s poll
    //             1        42 ns    0.00%
    //            10       167 ns    0.02%
    //           100      2.79 us    0.28%
    //         1,000       124 us   12.39%
    //        10,000      15.9 ms   poll rate physically unreachable
    //
    // An earlier version of this comment said "P50 291 ns ... not worth a
    // seqlock". That number was taken at ~10 orders per level and did not
    // generalise: a book with a deep queue at the touch holds this lock for
    // MILLISECONDS. The conclusion is now conditional, and the condition is
    // queue depth at the touch, not book size.
    //
    // READ dP99 TOGETHER WITH DUTY CYCLE, OR IT WILL MISLEAD YOU. At a paced
    // 1,000 req/s the matching path's P99 barely moves even at 1,000
    // orders/level (+0 ns) — because a 12% duty cycle puts the stall at P88,
    // below where P99 looks. Flat-out scraping at that depth collapses
    // matching throughput to 0-1% of control with a 188 ms P99. Monitoring
    // cannot stall this engine at a sane poll rate; it can absolutely starve it
    // without one.
    //
    // A seqlock or double-buffered snapshot IS worth building if books here are
    // expected to carry deep queues. It is not worth it for shallow ones, which
    // is what this deployment has had so far. /metrics and /prometheus do NOT
    // come through here.
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
    // bb + ba overflows int64 for large prices, and signed overflow is UB that
    // wraps to a negative mid. Both are validated positive, so ba - bb cannot
    // overflow and this form is exact for every representable pair.
    return bb + (ba - bb) / 2;
}

} // namespace OrderMatcher
