// Round-trip fuzzer: derive ExecutionReport fields from fuzzer bytes, build
// with FixSerializer, parse back with FixMessage, assert structural and
// semantic round-trip equality. Catches serializer bugs and parser/serializer
// drift (e.g. checksum mismatch, escaping issues, integer formatting bugs).

#include "FIXParser.h"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace OrderMatcher;

namespace {

// Cheap, deterministic byte-stream consumer.
struct Cursor {
    const uint8_t* p; size_t n;
    template <typename T>
    bool take(T& out) {
        if (n < sizeof(T)) return false;
        std::memcpy(&out, p, sizeof(T));
        p += sizeof(T); n -= sizeof(T);
        return true;
    }
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    Cursor c{data, size};

    uint64_t orderId = 0, execId = 0;
    int64_t  price = 0, lastPx = 0;
    uint64_t orderQty = 0, cumQty = 0, leavesQty = 0, lastQty = 0;
    uint8_t execType = 0, ordStatus = 0, sideByte = 0, textLen = 0;

    if (!c.take(orderId)) return 0;
    if (!c.take(execId))  return 0;
    if (!c.take(price))   return 0;
    if (!c.take(orderQty)) return 0;
    if (!c.take(cumQty))   return 0;
    if (!c.take(leavesQty)) return 0;
    if (!c.take(lastPx))    return 0;
    if (!c.take(lastQty))   return 0;
    if (!c.take(execType))  return 0;
    if (!c.take(ordStatus)) return 0;
    if (!c.take(sideByte))  return 0;
    if (!c.take(textLen))   return 0;

    // Constrain text length and content. We exclude SOH and '=' from the text
    // intentionally — those are framing characters; the serializer doesn't
    // currently escape them, and feeding them in would be a separate bug
    // class (worth a dedicated target later).
    std::string text;
    size_t want = std::min<size_t>(textLen, c.n);
    text.reserve(want);
    for (size_t i = 0; i < want; ++i) {
        char ch = static_cast<char>(c.p[i]);
        if (ch == FIX_SOH || ch == '=' || ch == '\0') continue;
        text.push_back(ch);
    }

    // Map exec type / status to printable digits the serializer accepts.
    char execTypeCh = "012348"[execType % 6];
    char ordStatusCh = "012348"[ordStatus % 6];
    Side side = (sideByte & 1) ? Side::Buy : Side::Sell;

    std::string raw = FixSerializer::buildExecutionReport(
        orderId, execId, execTypeCh, ordStatusCh, side,
        price, orderQty, cumQty, leavesQty, lastPx, lastQty, text);

    FixMessage msg;
    bool parsed = msg.parse(raw.c_str(), raw.size());
    assert(parsed && "serializer output failed to parse");

    // The serializer must always produce a well-formed, checksum-valid frame.
    assert(msg.isWellFormed());
    assert(msg.validateChecksum());

    // Spot-check a few fields round-trip. getInt saturates on overflow, which
    // is fine for this assertion: orderId/qty are int64-representable on the
    // serializer side, but the wire is unsigned. We compare via the wire path.
    assert(msg.getString(FixTag::MsgType) == "8");
    assert(msg.getUint64(FixTag::OrderID) == orderId);
    assert(msg.getUint64(FixTag::ExecID)  == execId);
    assert(msg.getChar(FixTag::ExecType) == execTypeCh);
    assert(msg.getChar(FixTag::OrdStatus) == ordStatusCh);
    assert(msg.getChar(FixTag::Side) == (side == Side::Buy ? '1' : '2'));
    assert(msg.getInt(FixTag::Price) == price);
    assert(msg.getUint64(FixTag::OrderQty)  == orderQty);
    assert(msg.getUint64(FixTag::CumQty)    == cumQty);
    assert(msg.getUint64(FixTag::LeavesQty) == leavesQty);

    return 0;
}
