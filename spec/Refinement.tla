-------------------------------- MODULE Refinement --------------------------------
\* Refinement Mapping: C++ Implementation → Abstract TLA+ Specification
\*
\* Roadmap Phase 2, Week 7
\*
\* This module defines how the concrete C++ MatchingEngine state maps to the
\* abstract MatchingEngine.tla specification. Every implementation action must
\* correspond to either a valid abstract action or a stuttering step.
\*
\* Assumptions (documented per roadmap requirement):
\*   A1: Memory is treated as sequentially consistent for the purpose of this
\*       mapping. The actual C++ code uses acquire/release atomics which are
\*       strictly weaker, but the observable behavior at API boundaries is SC.
\*   A2: ObjectPool allocation/deallocation is invisible (stuttering) because
\*       the abstract spec models orders as logical entities, not memory.
\*   A3: Journal I/O is a stuttering step; it does not change book state.
\*   A4: Network transport (FIX parsing, TCP) is below the abstraction level.
\*
\* Refinement function Abs maps:
\*   C++ OrderBook::bids_/asks_ (FlatPriceMap)  →  Abstract bids/asks (functions)
\*   C++ OrderBook::orderLookup_ (FlatHashMap)   →  Abstract orders (function)
\*   C++ OrderBook::tradeHistory_ (RingBuffer)   →  Abstract trades (sequence)
\*   C++ MatchingEngine::nextSubmitSequence_      →  Abstract sequence_number
\*
\* Each C++ public method maps to one abstract action:
\*   OrderBook::addOrder(Limit)    →  PlaceLimit
\*   OrderBook::addOrder(Market)   →  PlaceMarket
\*   OrderBook::addOrder(IOC)      →  PlaceIOC
\*   OrderBook::addOrder(FOK)      →  PlaceFOK
\*   OrderBook::addOrder(GTD)      →  PlaceGTD (PlaceLimit with expiry)
\*   OrderBook::cancelOrder        →  CancelOrder
\*   OrderBook::expireOrders       →  ExpireGTD
\*   OrderBook::match (internal)   →  Stuttering (sub-step of Place*)
\*   Journal::logAddOrder          →  Stuttering (persistence, not state)
\*   ObjectPool::allocate          →  Stuttering (memory management)
\*   EventListener::onTrade        →  Stuttering (notification, not state)
\*
\* Invariant preservation proof sketch:
\*   I1: NoNegativeQuantity — Order::remainingQty is Quantity (uint64_t),
\*       unsigned by construction. Every decrement is guarded by
\*       `min(incoming.remainingQty, bookOrder.remainingQty)`.
\*   I2: FIFO_Preservation — IntrusiveList maintains insertion order.
\*       match() always calls level->front() and never reorders.
\*       Price-time: new orders appended via push_back().
\*   I3: Quantity_Conservation — Every fill produces a Trade with fillQty.
\*       incoming.remainingQty -= fillQty and bookOrder.remainingQty -= fillQty
\*       happen atomically under bookLock_. Cancelled orders have their
\*       remaining qty zeroed and are removed from the book.
\*   I4: GTD_Expiry_Correctness — expireOrders() iterates all orders,
\*       checks currentTime >= order.expiryTime, and cancels. The expiry
\*       timer thread calls this periodically. Between calls, expired orders
\*       may exist briefly (bounded by timer interval).
--------------------------------------------------------------------------------

EXTENDS MatchingEngine, TLC

\* The refinement mapping function
Abs(implState) ==
    [bids      |-> ImplBidsToAbstract(implState.bids_),
     asks      |-> ImplAsksToAbstract(implState.asks_),
     orders    |-> ImplOrdersToAbstract(implState.orderLookup_),
     trades    |-> ImplTradesToAbstract(implState.tradeHistory_),
     seqno     |-> implState.nextSubmitSequence_]

\* Implementation bids → Abstract bids
\* FlatPriceMap stores price → IntrusiveList<Order>
\* Abstract bids is a function [Price → Seq(Order)]
ImplBidsToAbstract(flatPriceMap) ==
    [p \in DOMAIN flatPriceMap |->
        IntrusiveListToSeq(flatPriceMap[p])]

\* Implementation asks → Abstract asks (same structure)
ImplAsksToAbstract(flatPriceMap) ==
    [p \in DOMAIN flatPriceMap |->
        IntrusiveListToSeq(flatPriceMap[p])]

\* Implementation orders → Abstract orders
\* FlatHashMap<OrderId, Order*> → function [OrderId → OrderRecord]
ImplOrdersToAbstract(hashMap) ==
    [id \in DOMAIN hashMap |->
        [price     |-> hashMap[id].price,
         qty       |-> hashMap[id].remainingQty,
         side      |-> hashMap[id].side,
         pid       |-> hashMap[id].participantId,
         type      |-> hashMap[id].type,
         timestamp |-> hashMap[id].timestamp]]

\* Implementation trades → Abstract trades
\* RingBuffer<Trade> → Sequence of TradeRecord
ImplTradesToAbstract(ringBuffer) ==
    SubSeq(ringBuffer.buffer,
           ringBuffer.head,
           ringBuffer.tail - 1)

\* Stuttering actions: these change implementation state but not abstract state
StutteringActions == {
    "ObjectPool::allocate",
    "ObjectPool::deallocate",
    "Journal::logAddOrder",
    "Journal::logCancelOrder",
    "Journal::logModifyOrder",
    "Journal::flush",
    "EventListener::onTrade",
    "EventListener::onOrderUpdate",
    "EventListener::onMarketData",
    "RateLimiter::check",
    "LatencyTracker::record",
    "FIXParser::parse",
    "AdminServer::handleRequest"
}

================================================================================
