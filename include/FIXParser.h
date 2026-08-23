#pragma once

#include "OrderBook.h"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>

namespace OrderMatcher {

// ─── FIX Protocol Constants ─────────────────────────────────────────────────
// SOH (Start of Header) = ASCII 0x01, the standard FIX delimiter
constexpr char FIX_SOH = '\x01';

// Negotiated session-protocol version. The session translates an inbound
// BeginString into one of these and routes serialization accordingly. Tags
// the engine cares about overlap between FIX 4.2 and 4.4, but ExecType
// fill encoding, the requirement of TransactTime, and the use of
// OrdRejReason on rejects all differ — the version flag drives those.
enum class FixVersion : uint8_t {
    FIX_4_2,
    FIX_4_4,
};

// Best-effort BeginString → FixVersion. Defaults to 4.2 for any string the
// session has accepted but isn't 4.4: the only way an unknown version gets
// here is through setAcceptedVersions(), so the caller has already opted in.
inline FixVersion parseFixVersion(std::string_view beginString) {
    if (beginString == "FIX.4.4") return FixVersion::FIX_4_4;
    return FixVersion::FIX_4_2;
}

// Standard FIX tags used in order entry
namespace FixTag {
    constexpr int BeginString   = 8;    // FIX.4.2
    constexpr int BeginSeqNo    = 7;
    constexpr int BodyLength    = 9;
    constexpr int MsgType       = 35;
    constexpr int MsgSeqNum     = 34;
    constexpr int NewSeqNo      = 36;
    constexpr int PossDupFlag   = 43;
    constexpr int SenderCompID  = 49;
    constexpr int TargetCompID  = 56;
    constexpr int ClOrdID       = 11;   // Client Order ID
    constexpr int OrigClOrdID   = 41;   // Original Client Order ID (for cancel/replace)
    constexpr int Symbol        = 55;
    constexpr int Side          = 54;   // 1=Buy, 2=Sell
    constexpr int OrderQty      = 38;
    constexpr int Price         = 44;
    constexpr int OrdType       = 40;   // 1=Market, 2=Limit, 3=Stop, 4=StopLimit
    constexpr int TimeInForce   = 59;   // 0=Day, 1=GTC, 3=IOC, 4=FOK
    constexpr int ExecType      = 150;  // 0=New, 1=PartialFill, 2=Fill, 4=Cancel, 8=Reject
    constexpr int OrdStatus     = 39;   // Same encoding as ExecType
    constexpr int LeavesQty     = 151;
    constexpr int CumQty        = 14;
    constexpr int AvgPx         = 6;
    constexpr int LastPx        = 31;
    constexpr int LastQty       = 32;
    constexpr int OrderID       = 37;
    constexpr int ExecID        = 17;
    constexpr int Text          = 58;
    constexpr int CheckSum      = 10;
    constexpr int EndSeqNo      = 16;
    constexpr int SendingTime   = 52;
    constexpr int TransactTime  = 60;   // Required on FIX 4.4 D + 8 messages
    constexpr int OrdRejReason  = 103;  // Numeric reject code on ExecType=8
    constexpr int HeartBtInt    = 108;
    constexpr int TestReqID     = 112;
    constexpr int GapFillFlag   = 123;
}

// Standard FIX OrdRejReason (tag 103) values per the FIX 4.4 spec.
// Internal RejectReason values are mapped to these; -1 means "do not emit
// tag 103" (used for non-reject ExecutionReports).
constexpr int kOrdRejReasonOmit = -1;

inline int ordRejReasonCode(RejectReason r) {
    switch (r) {
    case RejectReason::SymbolNotFound:              return 1;   // Unknown symbol
    case RejectReason::MarketHalted:                return 2;   // Exchange closed
    case RejectReason::MarketClosed:                return 2;
    case RejectReason::VolatilityCircuitBreaker:    return 2;
    case RejectReason::OutOfPriceRange:             return 3;   // Order exceeds limit
    case RejectReason::OutsidePriceBand:            return 3;
    case RejectReason::RateLimitExceeded:           return 3;
    case RejectReason::RiskLimitBreached:           return 3;
    case RejectReason::OrderNotFound:               return 5;   // Unknown order
    case RejectReason::DuplicateOrderId:            return 6;   // Duplicate Order
    case RejectReason::FOKInsufficientLiquidity:    return 11;  // Unsupported order characteristic
    case RejectReason::PostOnlyWouldCross:          return 11;
    case RejectReason::OrderTypeNotAllowedInState:  return 11;
    case RejectReason::InvalidQuantity:             return 13;  // Incorrect quantity
    case RejectReason::InvalidDisplayQty:           return 13;  // Incorrect quantity (display)
    case RejectReason::InvalidPrice:                return 18;  // Invalid price
    case RejectReason::MissingRequiredField:        return 1;   // Required tag missing
                                                                // (FIX session-level reject code,
                                                                // closest standard value here)
    case RejectReason::CapacityExhausted:           return 0;   // Broker / Exchange option
    case RejectReason::QueueBackpressure:           return 0;
    case RejectReason::EngineStopped:               return 0;
    case RejectReason::UnsupportedFixVersion:       return 99;  // Other
    case RejectReason::None:                        return kOrdRejReasonOmit;
    case RejectReason::KillSwitchActive:            return 2;   // Exchange closed
    case RejectReason::PositionLimitExceeded:       return 3;   // Order exceeds limit
    case RejectReason::FatFingerReject:             return 3;
    case RejectReason::OrderToTradeRatioExceeded:   return 3;
    case RejectReason::PoolCapacityExceeded:        return 0;   // Broker / Exchange option
    }
    return kOrdRejReasonOmit;
}

// FIX UTC timestamp in the canonical "YYYYMMDD-HH:MM:SS.sss" form used by
// TransactTime (60) and SendingTime (52). Uses gmtime_r so it's independent
// of the host's timezone — FIX strings are always UTC.
inline std::string nowFixUTCTime() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto secs = time_point_cast<seconds>(now);
    auto millis = duration_cast<milliseconds>(now - secs).count();
    std::time_t t = system_clock::to_time_t(secs);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::snprintf(buf, sizeof(buf),
                  "%04d%02d%02d-%02d:%02d:%02d.%03lld",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<long long>(millis));
    return std::string(buf);
}

// ─── FIX Message Parser (Zero-Copy) ─────────────────────────────────────────
// Parses raw FIX tag=value|SOH byte streams into a tag map.

class FixMessage {
public:
    // Parse a raw FIX message. Returns false if malformed.
    bool parse(const char* data, size_t len) {
        tags_.clear();
        rawData_ = std::string_view(data, len);

        size_t pos = 0;
        while (pos < len) {
            // Find '='
            size_t eqPos = rawData_.find('=', pos);
            if (eqPos == std::string_view::npos) break;

            // Find SOH (or end of message)
            size_t sohPos = rawData_.find(FIX_SOH, eqPos + 1);
            if (sohPos == std::string_view::npos) sohPos = len;

            // FIX tags are positive integers; the highest standard tag is in
            // the low six figures. Cap to guard against integer overflow on
            // adversarial input and to reject obviously malformed tags.
            constexpr int kMaxTag = 999999;
            if (eqPos == pos) return false;       // empty tag
            int tag = 0;
            for (size_t i = pos; i < eqPos; ++i) {
                if (rawData_[i] < '0' || rawData_[i] > '9') return false;
                tag = tag * 10 + (rawData_[i] - '0');
                if (tag > kMaxTag) return false;
            }

            tags_[tag] = rawData_.substr(eqPos + 1, sohPos - eqPos - 1);
            pos = sohPos + 1;
        }

        return !tags_.empty();
    }

    bool hasTag(int tag) const { return tags_.count(tag) > 0; }

    std::string_view getString(int tag) const {
        auto it = tags_.find(tag);
        return (it != tags_.end()) ? it->second : std::string_view{};
    }

    int64_t getInt(int tag) const {
        auto sv = getString(tag);
        if (sv.empty()) return 0;
        bool neg = false;
        size_t i = 0;
        if (sv[0] == '-') { neg = true; i = 1; }
        // Accumulate in int64 with overflow checks. On overflow or non-digit
        // input we clamp to INT64_MAX / INT64_MIN — callers downstream coerce
        // to unsigned size types, where a saturated value is preferable to UB.
        int64_t val = 0;
        for (; i < sv.size(); ++i) {
            char c = sv[i];
            if (c < '0' || c > '9') {
                return neg ? INT64_MIN : INT64_MAX;
            }
            int64_t digit = c - '0';
            if (__builtin_mul_overflow(val, int64_t{10}, &val) ||
                __builtin_add_overflow(val, digit, &val)) {
                return neg ? INT64_MIN : INT64_MAX;
            }
        }
        return neg ? -val : val;
    }

    // Unsigned 64-bit accessor for tags whose semantic type is unsigned
    // (OrderId, Quantity, etc.). Saturates to UINT64_MAX on overflow or
    // non-digit input; returns 0 on a leading minus sign.
    uint64_t getUint64(int tag) const {
        auto sv = getString(tag);
        if (sv.empty()) return 0;
        if (sv[0] == '-') return 0;
        uint64_t val = 0;
        for (char c : sv) {
            if (c < '0' || c > '9') return UINT64_MAX;
            uint64_t digit = static_cast<uint64_t>(c - '0');
            if (__builtin_mul_overflow(val, uint64_t{10}, &val) ||
                __builtin_add_overflow(val, digit, &val)) {
                return UINT64_MAX;
            }
        }
        return val;
    }

    char getChar(int tag) const {
        auto sv = getString(tag);
        return sv.empty() ? '\0' : sv[0];
    }

    // Validate FIX checksum (tag 10).
    // Anchored on SOH+"10=" so a value containing "10=" cannot displace the
    // trailer position. The trailer must also be the last field in the message.
    bool validateChecksum() const {
        if (!hasTag(FixTag::CheckSum)) return false;

        // Trailer must be exactly "...<SOH>10=NNN<SOH>" at end of message.
        // Length: 8 = strlen("\x01" "10=NNN" "\x01")
        if (rawData_.size() < 8) return false;
        size_t end = rawData_.size();
        if (rawData_[end - 1] != FIX_SOH) return false;
        size_t csPos = end - 8;  // points at the leading SOH
        if (rawData_[csPos] != FIX_SOH) return false;
        if (rawData_[csPos + 1] != '1' || rawData_[csPos + 2] != '0' ||
            rawData_[csPos + 3] != '=') return false;

        // Sum is over bytes up to (not including) the "10=" — i.e. up to and
        // including the SOH that precedes the trailer.
        size_t sumEnd = csPos + 1;  // include the SOH preceding "10="
        uint32_t sum = 0;
        for (size_t i = 0; i < sumEnd; ++i) {
            sum += static_cast<uint8_t>(rawData_[i]);
        }

        int expected = static_cast<int>(getInt(FixTag::CheckSum));
        return (static_cast<int>(sum % 256) == expected);
    }

    // Cross-check BodyLength(9) against the actual measured body size.
    // FIX defines BodyLength as the number of bytes from the character
    // immediately following the BodyLength field's SOH up to and including
    // the SOH immediately preceding the CheckSum(10) field. The framer
    // trusts tag 9 to locate the trailer; this lets any caller holding a
    // complete frame verify the declared length matches reality and reject
    // a forged/garbled header. Returns false on any structural problem or
    // on a mismatch — never trust the declared length.
    bool validateBodyLength() const {
        if (!hasTag(FixTag::BodyLength)) return false;

        // Frame must start "8=...<SOH>9=NNN<SOH>". Find the SOH that ends
        // BeginString(8); BodyLength(9) must immediately follow.
        size_t firstSoh = rawData_.find(FIX_SOH);
        if (firstSoh == std::string_view::npos) return false;
        size_t blPos = firstSoh + 1;
        if (blPos + 2 > rawData_.size()) return false;
        if (rawData_[blPos] != '9' || rawData_[blPos + 1] != '=') return false;
        size_t blSoh = rawData_.find(FIX_SOH, blPos + 2);
        if (blSoh == std::string_view::npos) return false;
        size_t bodyStart = blSoh + 1;  // first body byte

        // Trailer must be exactly "...<SOH>10=NNN<SOH>" (8 bytes) at the end.
        size_t end = rawData_.size();
        if (end < 8) return false;
        if (rawData_[end - 1] != FIX_SOH) return false;
        size_t csPos = end - 8;  // SOH immediately preceding "10="
        if (rawData_[csPos] != FIX_SOH) return false;
        if (rawData_[csPos + 1] != '1' || rawData_[csPos + 2] != '0' ||
            rawData_[csPos + 3] != '=') return false;
        if (bodyStart > csPos + 1) return false;  // header overlaps trailer

        // Measured body spans [bodyStart, csPos] inclusive — FIX counts the
        // SOH that precedes "10=".
        size_t measured = csPos - bodyStart + 1;
        int64_t declared = getInt(FixTag::BodyLength);
        return declared >= 0 &&
               static_cast<size_t>(declared) == measured;
    }

    // Structural framing check per FIX 4.2: BeginString(8), BodyLength(9),
    // MsgType(35), CheckSum(10) must all be present. This is intentionally
    // separate from parse() to keep the lenient zero-copy path available; the
    // gateway should call parse() && isWellFormed() && validateChecksum().
    bool isWellFormed() const {
        return hasTag(FixTag::BeginString) &&
               hasTag(FixTag::BodyLength) &&
               hasTag(FixTag::MsgType) &&
               hasTag(FixTag::CheckSum);
    }

private:
    std::unordered_map<int, std::string_view> tags_;
    std::string_view rawData_;
};

// ─── FIX → Engine Parameter Converter ───────────────────────────────────────
// Standalone struct — no dependency on OrderRequest (which lives in MatchingEngine.h)

struct FixOrderParams {
    enum class Action : uint8_t { NewOrder, Cancel, CancelReplace, Unknown };

    bool valid{false};
    Action action{Action::Unknown};
    OrderId orderId{0};
    SymbolId symbolId{0};
    ParticipantId participantId{0};
    Side side{Side::Buy};
    Price price{0};
    Quantity qty{0};
    OrderType orderType{OrderType::Limit};
    TimeInForce tif{TimeInForce::GTC};
    // For CancelReplace:
    Price newPrice{0};
    Quantity newQty{0};
    std::string error;
};

inline FixOrderParams fixToOrderParams(const FixMessage& msg) {
    FixOrderParams result;

    auto msgType = msg.getString(FixTag::MsgType);
    if (msgType.empty()) {
        result.error = "Missing MsgType (35)";
        return result;
    }

    if (msgType == "D") {
        // NewOrderSingle
        result.action = FixOrderParams::Action::NewOrder;

        // Required fields. The previous code read every tag unconditionally;
        // an absent tag coerced to 0/'\0' yet still set valid=true, admitting
        // orders with id=0, qty=0, or an unintended side. Reject at the
        // boundary with a field-level reason — never build an order from
        // missing data. result.valid stays false on every early return.
        if (!msg.hasTag(FixTag::ClOrdID)) {
            result.error = "Missing required field ClOrdID (11)"; return result;
        }
        if (!msg.hasTag(FixTag::Symbol)) {
            result.error = "Missing required field Symbol (55)"; return result;
        }
        if (!msg.hasTag(FixTag::Side)) {
            result.error = "Missing required field Side (54)"; return result;
        }
        if (!msg.hasTag(FixTag::OrderQty)) {
            result.error = "Missing required field OrderQty (38)"; return result;
        }
        if (!msg.hasTag(FixTag::OrdType)) {
            result.error = "Missing required field OrdType (40)"; return result;
        }

        result.orderId = static_cast<OrderId>(msg.getInt(FixTag::ClOrdID));
        result.symbolId = static_cast<SymbolId>(msg.getInt(FixTag::Symbol));
        result.price = static_cast<Price>(msg.getInt(FixTag::Price));
        result.qty = static_cast<Quantity>(msg.getInt(FixTag::OrderQty));

        // OrderQty must be strictly positive — an explicit 0 is not a
        // tradeable quantity and must not be admitted as a live order.
        if (result.qty == 0) {
            result.error = "Invalid OrderQty (38): must be > 0"; return result;
        }

        // Side: 1=Buy, 2=Sell. Any other value is a malformed enum and must
        // be rejected, not silently defaulted to Sell.
        char side = msg.getChar(FixTag::Side);
        if (side == '1') {
            result.side = Side::Buy;
        } else if (side == '2') {
            result.side = Side::Sell;
        } else {
            result.error = "Unknown Side (54): " + std::string(1, side);
            return result;
        }

        // OrdType: 1=Market, 2=Limit, 3=Stop, 4=StopLimit. An unknown value
        // must be rejected, not silently reinterpreted as Limit.
        char ordType = msg.getChar(FixTag::OrdType);
        switch (ordType) {
            case '1': result.orderType = OrderType::Market; break;
            case '2': result.orderType = OrderType::Limit; break;
            case '3': result.orderType = OrderType::Stop; break;
            case '4': result.orderType = OrderType::StopLimit; break;
            default:
                result.error = "Unknown OrdType (40): " + std::string(1, ordType);
                return result;
        }

        // TimeInForce: 0=Day, 1=GTC, 3=IOC, 4=FOK. Optional (absent means the
        // session default, GTC), but a PRESENT value outside the known set is
        // rejected rather than silently defaulted.
        if (msg.hasTag(FixTag::TimeInForce)) {
            char tifChar = msg.getChar(FixTag::TimeInForce);
            switch (tifChar) {
                case '0': result.tif = TimeInForce::DAY; break;
                case '1': result.tif = TimeInForce::GTC; break;
                case '3':
                    result.orderType = OrderType::IOC;
                    result.tif = TimeInForce::GTC;
                    break;
                case '4':
                    result.orderType = OrderType::FOK;
                    result.tif = TimeInForce::GTC;
                    break;
                default:
                    result.error = "Unknown TimeInForce (59): " + std::string(1, tifChar);
                    return result;
            }
        } else {
            result.tif = TimeInForce::GTC;
        }

        result.participantId = static_cast<ParticipantId>(msg.getInt(FixTag::SenderCompID));
        result.valid = true;

    } else if (msgType == "F") {
        // OrderCancelRequest
        result.action = FixOrderParams::Action::Cancel;
        result.orderId = static_cast<OrderId>(msg.getInt(FixTag::OrigClOrdID));
        result.symbolId = static_cast<SymbolId>(msg.getInt(FixTag::Symbol));
        result.valid = true;

    } else if (msgType == "G") {
        // OrderCancelReplaceRequest
        result.action = FixOrderParams::Action::CancelReplace;
        result.orderId = static_cast<OrderId>(msg.getInt(FixTag::OrigClOrdID));
        result.symbolId = static_cast<SymbolId>(msg.getInt(FixTag::Symbol));
        result.newPrice = static_cast<Price>(msg.getInt(FixTag::Price));
        result.newQty = static_cast<Quantity>(msg.getInt(FixTag::OrderQty));
        result.valid = true;

    } else {
        result.error = "Unsupported MsgType: " + std::string(msgType);
    }

    return result;
}

// ─── ExecutionReport (35=8) Serializer ──────────────────────────────────────

class FixSerializer {
public:
    // Build a FIX ExecutionReport.
    //
    // execType is the wire value the caller intends. Per FIX 4.4 the
    // partial-fill ('1') and full-fill ('2') codes from FIX 4.2 are
    // deprecated in favor of 'F' (Trade) — when version=FIX_4_4 the
    // serializer rewrites '1'/'2' to 'F' automatically. ordStatus is
    // unaffected; the spec keeps the 4.2-era ordStatus codes.
    //
    // TransactTime (60) is always emitted (required in 4.4, common
    // practice in 4.2). Pass an explicit transactTime to make output
    // deterministic in tests; empty means "now in UTC".
    //
    // ordRejReason is the FIX standard OrdRejReason (103) numeric code
    // and is only emitted when execType='8'. Pass kOrdRejReasonOmit
    // (the default) for non-reject reports.
    static std::string buildExecutionReport(
        OrderId orderId,
        OrderId execId,
        char execType,       // '0'=New, '1'=PartialFill, '2'=Fill, '4'=Cancel, '8'=Reject
        char ordStatus,
        Side side,
        Price price,
        Quantity orderQty,
        Quantity cumQty,
        Quantity leavesQty,
        Price lastPx = 0,
        Quantity lastQty = 0,
        const std::string& text = "",
        std::string_view beginString = "FIX.4.2",
        uint64_t msgSeqNum = 0,
        FixVersion version = FixVersion::FIX_4_2,
        int ordRejReason = kOrdRejReasonOmit,
        std::string_view transactTime = {})
    {
        // FIX 4.4 deprecates the per-fill ExecType codes in favor of 'F'.
        // ordStatus (39) intentionally keeps its 4.2 encoding — that's
        // still the spec for status, only the *event* type tag changed.
        char wireExecType = execType;
        if (version == FixVersion::FIX_4_4 &&
            (wireExecType == '1' || wireExecType == '2')) {
            wireExecType = 'F';
        }

        std::ostringstream body;
        body << "35=8" << FIX_SOH;
        if (msgSeqNum > 0) body << "34=" << msgSeqNum << FIX_SOH;
        body << "37=" << orderId << FIX_SOH;
        body << "17=" << execId << FIX_SOH;
        body << "150=" << wireExecType << FIX_SOH;
        body << "39=" << ordStatus << FIX_SOH;
        body << "54=" << (side == Side::Buy ? '1' : '2') << FIX_SOH;
        body << "44=" << price << FIX_SOH;
        body << "38=" << orderQty << FIX_SOH;
        body << "14=" << cumQty << FIX_SOH;
        body << "151=" << leavesQty << FIX_SOH;

        // TransactTime: required on 4.4, harmless on 4.2.
        if (transactTime.empty()) {
            body << "60=" << nowFixUTCTime() << FIX_SOH;
        } else {
            body << "60=" << transactTime << FIX_SOH;
        }

        // OrdRejReason: only meaningful when this is actually a reject.
        if (execType == '8' && ordRejReason != kOrdRejReasonOmit) {
            body << "103=" << ordRejReason << FIX_SOH;
        }

        if (lastPx > 0) body << "31=" << lastPx << FIX_SOH;
        if (lastQty > 0) body << "32=" << lastQty << FIX_SOH;
        if (!text.empty()) body << "58=" << text << FIX_SOH;

        std::string bodyStr = body.str();

        // Build full message: header + body + checksum
        std::ostringstream msg;
        msg << "8=" << beginString << FIX_SOH;
        msg << "9=" << bodyStr.size() << FIX_SOH;
        msg << bodyStr;

        std::string raw = msg.str();

        // Calculate checksum
        uint32_t sum = 0;
        for (char c : raw) sum += static_cast<uint8_t>(c);

        char cs[4];
        snprintf(cs, sizeof(cs), "%03d", static_cast<int>(sum % 256));
        raw += "10=";
        raw += cs;
        raw += FIX_SOH;

        return raw;
    }
};

} // namespace OrderMatcher
