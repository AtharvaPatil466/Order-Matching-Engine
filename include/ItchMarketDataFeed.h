#pragma once

// ItchMarketDataFeed — the integrated market-data publish stack for
// one OrderBook.
//
// Composition:
//   OrderBook
//     ↓ EventListener (onOrderUpdate, onTrade)
//   ItchPublisher       — translates engine events to ITCH 5.0 frames
//     ↓ frame bytes
//   ItchMarketDataFeed::onItchFrame
//     ↓     ↘
//   journal  udp
//   .record  .publish + .flush
//     ↓     ↘
//   replay   UDP multicast
//   path     (MoldUDP64 packets, sequence-numbered)
//
// The journal records each ITCH frame keyed by its MoldUDP64
// sequence number, so a separately-running ItchRetransmissionService
// (constructed externally with the journal as its data source) can
// replay any missed frames to a subscriber that detects a gap.
//
// Flush policy: one ITCH frame per UDP datagram. Real venues batch
// frames into MTU-sized packets for throughput; this implementation
// flushes per-frame for predictable testing and lossless
// observability. Override the policy by calling setBatchPolicy()
// (not currently exposed — extension point for later if needed).

#include "EventListener.h"
#include "ItchProtocol.h"
#include "ItchPublisher.h"
#include "ItchUdpTransport.h"
#include "MoldPacketJournal.h"
#include "OrderBook.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace OrderMatcher {

class ItchMarketDataFeed : public EventListener {
public:
    ItchMarketDataFeed(OrderBook& book, std::string session,
                       size_t journalCapacity = 4096)
        : journal_(journalCapacity),
          udp_(std::move(session)),
          // Construct ItchPublisher with a sink that fans out to the
          // UDP publisher AND the journal. `this` is captured to
          // call the dispatch helper — safe because the lambda's
          // lifetime is bounded by *this.
          itch_(book, [this](std::string_view bytes) {
              dispatchFrame(bytes);
          }) {}

    ~ItchMarketDataFeed() { stop(); }

    ItchMarketDataFeed(const ItchMarketDataFeed&) = delete;
    ItchMarketDataFeed& operator=(const ItchMarketDataFeed&) = delete;

    // Open the UDP socket toward destIp:destPort and start
    // publishing. Multicast TTL defaults to 1 (link-local). Returns
    // false on bind/connect failure.
    bool start(const std::string& destIp, uint16_t destPort,
               uint8_t multicastTtl = 1) {
        return udp_.start(destIp, destPort, multicastTtl);
    }

    void stop() { udp_.stop(); }

    // Subscribers expect a 'S' SystemEvent at start and end of
    // session; callers wire these to their own session boundaries.
    void publishSystemEvent(char eventCode) {
        itch_.publishSystemEvent(eventCode);
    }

    // Announce the symbol via 'R' at session start.
    void publishStockDirectory(char marketCategory = 'Q',
                               char financialStatus = 'N',
                               uint32_t roundLotSize = 100) {
        itch_.publishStockDirectory(marketCategory, financialStatus,
                                    roundLotSize);
    }

    // Notify subscribers of a TradingState transition (halt/resume).
    void publishTradingAction(const char reason4[4] = "    ") {
        itch_.publishTradingAction(reason4);
    }

    // Auction-uncross print.
    void publishCrossTrade(uint64_t shares, Price crossPrice,
                           uint64_t matchNumber, char crossType) {
        itch_.publishCrossTrade(shares, crossPrice, matchNumber, crossType);
    }

    // The publish path's source of truth for retransmission: a
    // separately-constructed ItchRetransmissionService holds a
    // reference to this journal and replays from it.
    MoldPacketJournal& journal() { return journal_; }

    // Observability.
    uint64_t addsEmitted()      const { return itch_.addsEmitted(); }
    uint64_t executedEmitted()  const { return itch_.executedEmitted(); }
    uint64_t deletesEmitted()   const { return itch_.deletesEmitted(); }
    uint64_t messagesEmitted()  const { return itch_.messagesEmitted(); }
    uint64_t datagramsSent()    const { return udp_.datagramsSent(); }
    uint64_t framesJournaled()  const { return framesJournaled_; }
    size_t   liveOrderCount()   const { return itch_.liveOrderCount(); }

    // EventListener overrides — forward to the inner ITCH publisher,
    // which encodes and emits via the sink we configured at
    // construction.
    void onOrderUpdate(const OrderUpdate& u) override {
        itch_.onOrderUpdate(u);
    }
    void onTrade(const Trade& t) override { itch_.onTrade(t); }
    void onMarketData(const MarketDataUpdate& u) override {
        itch_.onMarketData(u);
    }

private:
    void dispatchFrame(std::string_view bytes) {
        // 1. Enqueue into MoldUDP64; receive the assigned seqnum.
        uint64_t seq = udp_.publish(bytes.data(),
                                    static_cast<uint16_t>(bytes.size()));
        // 2. Record into the journal so retransmission can replay.
        //    The journal is the durable side-channel that backs
        //    re-requests; without this entry, a gap-recovering
        //    subscriber would never see this frame again.
        journal_.record(seq, bytes.data(),
                        static_cast<uint16_t>(bytes.size()));
        ++framesJournaled_;
        // 3. Per-frame flush: one UDP datagram per ITCH event.
        udp_.flush();
    }

    MoldPacketJournal       journal_;
    ItchUdpPublisher        udp_;
    ItchPublisher           itch_;
    uint64_t                framesJournaled_{0};
};

}  // namespace OrderMatcher
