// engine_bindings.cpp
// pybind11 bridge exposing the TLA+-verified C++ MatchingEngine to Python.
//
// Design notes
// ------------
// * The engine runs in SYNCHRONOUS mode (start(), not startAsync()). In sync
//   mode submit_order() dispatches matching on the calling thread and the
//   book's EventListener fires inline, so draining trades immediately after a
//   submit yields exactly the fills that order produced. This keeps the bridge
//   deterministic and free of cross-thread/GIL concerns.
// * Fills are captured by a single TradeCollector per book, attached through
//   the same OrderBook::setEventListener slot the C++ MarketMaker uses (proven
//   in tests/TestSimulation.cpp). The collector records EVERY trade on the
//   book; Python attributes fills to agents by buyer_id / seller_id.
// * The engine has no notion of agent latency. The BCS latency/race mechanism
//   lives entirely in the Python scheduler above this layer.

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "MatchingEngine.h"
#include "OrderBook.h"
#include "EventListener.h"
#include "Types.h"

#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace py = pybind11;
using namespace OrderMatcher;

namespace {

// Buffers every trade on one book. drain() returns and clears the buffer.
class TradeCollector : public EventListener {
public:
    void onTrade(const Trade& t) override { trades_.push_back(t); }

    std::vector<Trade> drain() {
        std::vector<Trade> out;
        out.swap(trades_);
        return out;
    }

    std::size_t pending() const { return trades_.size(); }

private:
    std::vector<Trade> trades_;
};

// Owns a synchronous MatchingEngine plus one TradeCollector per symbol.
// Single Python entry point for the BCS harness.
class EngineHarness {
public:
    EngineHarness() = default;

    // addSymbol must precede start(): start() freezes the book registry, after
    // which addSymbol is a silent no-op.
    void addSymbol(SymbolId sym) {
        if (started_) {
            throw std::runtime_error("add_symbol() must be called before start()");
        }
        engine_.addSymbol(sym);
        symbols_.push_back(sym);
    }

    void start(bool disableCircuitBreaker) {
        if (started_) return;
        engine_.start();
        started_ = true;
        for (SymbolId sym : symbols_) {
            OrderBook* book = engine_.getOrderBook(sym);
            if (!book) continue;
            auto collector = std::make_unique<TradeCollector>();
            book->setEventListener(collector.get());
            if (disableCircuitBreaker) {
                book->setCircuitBreakerThreshold(1e18);  // disable for sim control
            }
            collectors_.emplace(sym, std::move(collector));
        }
    }

    void stop() {
        if (!started_) return;
        detachListeners();
        engine_.stop();
        started_ = false;
    }

    // The book holds a raw EventListener* into our TradeCollectors. Detach
    // before those collectors are freed (and before the engine tears down
    // resting orders), otherwise teardown calls through a dangling pointer.
    ~EngineHarness() {
        if (started_) {
            detachListeners();
            engine_.stop();
            started_ = false;
        }
    }

    // Returns (accepted: bool, reject_reason: RejectReason, sequence_id: int).
    py::tuple submitOrder(SymbolId sym, OrderId orderId, ParticipantId pid,
                          Side side, Price price, Quantity qty,
                          OrderType type, TimeInForce tif) {
        SubmitResult r = engine_.submitOrder(sym, orderId, pid, side, price, qty,
                                             type, /*stopPrice=*/0, /*displayQty=*/0,
                                             tif);
        return py::make_tuple(r.isAccepted(), r.rejectReason, r.sequenceId);
    }

    py::tuple submitCancel(SymbolId sym, OrderId orderId) {
        SubmitResult r = engine_.submitCancel(sym, orderId);
        return py::make_tuple(r.isAccepted(), r.rejectReason, r.sequenceId);
    }

    // Best bid; 0 when the bid side is empty.
    Price bestBid(SymbolId sym) {
        OrderBook* b = engine_.getOrderBook(sym);
        return b ? b->getBestBid() : 0;
    }

    // Best ask; normalized to 0 when the ask side is empty (the engine returns
    // INT64_MAX in that case).
    Price bestAsk(SymbolId sym) {
        OrderBook* b = engine_.getOrderBook(sym);
        if (!b) return 0;
        Price p = b->getBestAsk();
        return (p == std::numeric_limits<Price>::max()) ? 0 : p;
    }

    // Mid; 0 if either side is empty.
    Price mid(SymbolId sym) {
        Price bb = bestBid(sym);
        Price ba = bestAsk(sym);
        return (bb > 0 && ba > 0) ? (bb + ba) / 2 : 0;
    }

    std::vector<Trade> drainTrades(SymbolId sym) {
        auto it = collectors_.find(sym);
        return (it == collectors_.end()) ? std::vector<Trade>{} : it->second->drain();
    }

    MarketDataSnapshot snapshot(SymbolId sym, std::size_t depth) {
        OrderBook* b = engine_.getOrderBook(sym);
        return b ? b->getSnapshot(depth) : MarketDataSnapshot{};
    }

    double vwap(SymbolId sym) {
        OrderBook* b = engine_.getOrderBook(sym);
        return b ? b->getVWAP() : 0.0;
    }

    std::uint64_t tradeCount(SymbolId sym) {
        OrderBook* b = engine_.getOrderBook(sym);
        return b ? b->getTradeCount() : 0;
    }

    // Run one uniform-price call auction on the resting book. Exposed so the
    // batch-auction arm of Experiment 4 clears through the SAME model-checked
    // code path as the continuous arm, rather than a Python re-implementation
    // of the clearing rule. Trades land in the usual collector, so drain_trades
    // reports auction fills exactly as it reports continuous fills.
    void uncross(SymbolId sym) {
        if (OrderBook* b = engine_.getOrderBook(sym)) {
            b->uncross();
        }
    }

    // A call auction needs orders to ACCUMULATE rather than match on arrival, so
    // the batch arm must park the book in an auction state (AuctionOpen/Close,
    // PreOpen) before submitting and restore Continuous after uncross(). Calling
    // uncross() while the book is Continuous is a no-op: arrivals have already
    // matched pairwise at their own prices, which is precisely the uniform-price
    // property the batch arm exists to test.
    void setTradingState(SymbolId sym, TradingState s) {
        if (OrderBook* b = engine_.getOrderBook(sym)) {
            b->setTradingState(s);
        }
    }

    TradingState tradingState(SymbolId sym) {
        OrderBook* b = engine_.getOrderBook(sym);
        return b ? b->getTradingState() : TradingState::Continuous;
    }

private:
    // Point every book's listener slot back at the shared null listener.
    void detachListeners() {
        for (SymbolId sym : symbols_) {
            if (OrderBook* book = engine_.getOrderBook(sym)) {
                book->setEventListener(&nullListener());
            }
        }
    }

    // Declaration order matters: engine_ is declared last so it is destroyed
    // FIRST (members destruct in reverse order), while the TradeCollectors it
    // points at are still alive.
    std::map<SymbolId, std::unique_ptr<TradeCollector>> collectors_;
    std::vector<SymbolId> symbols_;
    bool started_ = false;
    MatchingEngine engine_;
};

}  // namespace

PYBIND11_MODULE(bcs_engine, m) {
    m.doc() = "pybind11 bridge to the TLA+-verified C++ MatchingEngine (BCS research)";
    m.attr("PRICE_PRECISION") = py::int_(PRICE_PRECISION);

    py::enum_<Side>(m, "Side")
        .value("Buy", Side::Buy)
        .value("Sell", Side::Sell);

    py::enum_<OrderType>(m, "OrderType")
        .value("Limit", OrderType::Limit)
        .value("Market", OrderType::Market)
        .value("IOC", OrderType::IOC)
        .value("FOK", OrderType::FOK)
        .value("PostOnly", OrderType::PostOnly);

    py::enum_<TradingState>(m, "TradingState")
        .value("Continuous", TradingState::Continuous)
        .value("Halted", TradingState::Halted)
        .value("AuctionOpen", TradingState::AuctionOpen)
        .value("AuctionClose", TradingState::AuctionClose)
        .value("PreOpen", TradingState::PreOpen)
        .value("PostClose", TradingState::PostClose)
        .value("VolatilityAuction", TradingState::VolatilityAuction);

    py::enum_<TimeInForce>(m, "TimeInForce")
        .value("GTC", TimeInForce::GTC)
        .value("GTD", TimeInForce::GTD)
        .value("DAY", TimeInForce::DAY);

    py::enum_<RejectReason>(m, "RejectReason")
        .value("None_", RejectReason::None)
        .value("PostOnlyWouldCross", RejectReason::PostOnlyWouldCross)
        .value("FOKInsufficientLiquidity", RejectReason::FOKInsufficientLiquidity)
        .value("InvalidPrice", RejectReason::InvalidPrice)
        .value("InvalidQuantity", RejectReason::InvalidQuantity)
        .value("SymbolNotFound", RejectReason::SymbolNotFound)
        .value("OrderNotFound", RejectReason::OrderNotFound)
        .value("MarketHalted", RejectReason::MarketHalted)
        .value("DuplicateOrderId", RejectReason::DuplicateOrderId)
        .value("EngineStopped", RejectReason::EngineStopped);

    py::class_<Trade>(m, "Trade")
        .def_readonly("trade_id", &Trade::tradeId)
        .def_readonly("buy_order_id", &Trade::buyOrderId)
        .def_readonly("sell_order_id", &Trade::sellOrderId)
        .def_readonly("buyer_id", &Trade::buyerId)
        .def_readonly("seller_id", &Trade::sellerId)
        .def_readonly("price", &Trade::price)
        .def_readonly("quantity", &Trade::quantity)
        .def_readonly("timestamp", &Trade::timestamp)
        .def_readonly("sequence_number", &Trade::sequenceNumber)
        .def_readonly("aggressor_side", &Trade::aggressorSide)
        .def("__repr__", [](const Trade& t) {
            return "<Trade px=" + std::to_string(t.price) +
                   " qty=" + std::to_string(t.quantity) +
                   " buyer=" + std::to_string(t.buyerId) +
                   " seller=" + std::to_string(t.sellerId) + ">";
        });

    py::class_<PriceLevel>(m, "PriceLevel")
        .def_readonly("price", &PriceLevel::price)
        .def_readonly("total_quantity", &PriceLevel::totalQuantity)
        .def_readonly("order_count", &PriceLevel::orderCount)
        .def("__repr__", [](const PriceLevel& l) {
            return "<PriceLevel px=" + std::to_string(l.price) +
                   " qty=" + std::to_string(l.totalQuantity) +
                   " n=" + std::to_string(l.orderCount) + ">";
        });

    py::class_<MarketDataSnapshot>(m, "MarketDataSnapshot")
        .def_property_readonly("bids", [](const MarketDataSnapshot& s) {
            return std::vector<PriceLevel>(s.bids, s.bids + s.bidCount);
        })
        .def_property_readonly("asks", [](const MarketDataSnapshot& s) {
            return std::vector<PriceLevel>(s.asks, s.asks + s.askCount);
        })
        .def_readonly("last_trade_price", &MarketDataSnapshot::lastTradePrice)
        .def_readonly("last_trade_qty", &MarketDataSnapshot::lastTradeQty)
        .def_readonly("timestamp", &MarketDataSnapshot::timestamp);

    py::class_<EngineHarness>(m, "EngineHarness")
        .def(py::init<>())
        .def("add_symbol", &EngineHarness::addSymbol, py::arg("symbol"))
        .def("start", &EngineHarness::start,
             py::arg("disable_circuit_breaker") = true)
        .def("stop", &EngineHarness::stop)
        .def("submit_order", &EngineHarness::submitOrder,
             py::arg("symbol"), py::arg("order_id"), py::arg("participant_id"),
             py::arg("side"), py::arg("price"), py::arg("quantity"),
             py::arg("order_type") = OrderType::Limit,
             py::arg("tif") = TimeInForce::GTC)
        .def("submit_cancel", &EngineHarness::submitCancel,
             py::arg("symbol"), py::arg("order_id"))
        .def("best_bid", &EngineHarness::bestBid, py::arg("symbol"))
        .def("best_ask", &EngineHarness::bestAsk, py::arg("symbol"))
        .def("mid", &EngineHarness::mid, py::arg("symbol"))
        .def("drain_trades", &EngineHarness::drainTrades, py::arg("symbol"))
        .def("snapshot", &EngineHarness::snapshot,
             py::arg("symbol"), py::arg("depth") = 10)
        .def("vwap", &EngineHarness::vwap, py::arg("symbol"))
        .def("trade_count", &EngineHarness::tradeCount, py::arg("symbol"))
        .def("uncross", &EngineHarness::uncross, py::arg("symbol"))
        .def("set_trading_state", &EngineHarness::setTradingState,
             py::arg("symbol"), py::arg("state"))
        .def("trading_state", &EngineHarness::tradingState, py::arg("symbol"));
}
