// FixValidationTest — boundary-validation tests for the FIX input path.
//
// These pin six "trust the bytes" gaps that previously let malformed or
// out-of-protocol FIX messages reach the matching engine:
//   1. Missing MsgSeqNum(34) was silently accepted.
//   2. Application messages (35=D/F/G) were dispatched before a Logon(35=A).
//   3. NewOrderSingle required fields (11/55/54/38/40) were coerced to 0/'\0'
//      with valid=true instead of being rejected.
//   4. BodyLength(9) was not cross-checked against the measured body size.
//   5. TargetCompID(56) was never validated against the venue's CompID.
//   6. Unknown OrdType(40)/TimeInForce(59) values silently defaulted to
//      Limit/GTC instead of being rejected.
//
// Validate at the boundary; fail fast; never trust external bytes.

#include "FIXParser.h"
#include "FixFramer.h"
#include "FixSession.h"
#include "MatchingEngine.h"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

using namespace OrderMatcher;

namespace {

// Build a complete, framed FIX 4.2 message with a correctly-measured
// BodyLength and CheckSum. `tags` are the body fields after MsgType(35).
std::string makeFix(const char* msgType,
                    const std::vector<std::pair<int, std::string>>& tags) {
    std::string body;
    body += "35="; body += msgType; body += FIX_SOH;
    for (auto& [tag, val] : tags) {
        body += std::to_string(tag); body += "="; body += val; body += FIX_SOH;
    }
    std::string pre = "8=FIX.4.2";
    pre += FIX_SOH;
    pre += "9=" + std::to_string(body.size());
    pre += FIX_SOH;
    pre += body;
    uint32_t sum = 0;
    for (char c : pre) sum += static_cast<uint8_t>(c);
    char cs[4]; std::snprintf(cs, sizeof(cs), "%03d", sum % 256);
    return pre + "10=" + cs + FIX_SOH;
}

// Build a framed message whose declared BodyLength(9) is `declaredLen`
// (which may deliberately differ from the true body size). The CheckSum is
// computed over the bytes actually emitted, so it stays internally valid —
// only the BodyLength is wrong.
std::string makeFixWithBodyLen(const char* msgType,
                               const std::vector<std::pair<int, std::string>>& tags,
                               size_t declaredLen) {
    std::string body;
    body += "35="; body += msgType; body += FIX_SOH;
    for (auto& [tag, val] : tags) {
        body += std::to_string(tag); body += "="; body += val; body += FIX_SOH;
    }
    std::string pre = "8=FIX.4.2";
    pre += FIX_SOH;
    pre += "9=" + std::to_string(declaredLen);
    pre += FIX_SOH;
    pre += body;
    uint32_t sum = 0;
    for (char c : pre) sum += static_cast<uint8_t>(c);
    char cs[4]; std::snprintf(cs, sizeof(cs), "%03d", sum % 256);
    return pre + "10=" + cs + FIX_SOH;
}

// Build a raw (unframed) NewOrderSingle for direct fixToOrderParams tests.
std::string makeRawNewOrder(const std::vector<std::pair<int, std::string>>& tags) {
    std::string raw = "35=D";
    raw += FIX_SOH;
    for (auto& [tag, val] : tags) {
        raw += std::to_string(tag); raw += "="; raw += val; raw += FIX_SOH;
    }
    return raw;
}

// Drive a Logon(35=A) through the session so subsequent app messages pass
// the logon gate; clears whatever the Logon ack wrote to `sent`.
void doLogon(FixSession& s, std::string& sent, uint64_t seq = 1,
             const std::string& targetCompId = "") {
    std::vector<std::pair<int, std::string>> tags = {
        {FixTag::MsgSeqNum, std::to_string(seq)},
        {FixTag::HeartBtInt, "30"},
    };
    if (!targetCompId.empty()) {
        tags.push_back({FixTag::TargetCompID, targetCompId});
    }
    auto m = makeFix("A", tags);
    s.feed(m.data(), m.size());
    sent.clear();
}

// A well-formed NewOrderSingle field set (all required fields present).
std::vector<std::pair<int, std::string>> goodOrderTags(
    uint64_t seq, OrderId clOrdId, SymbolId sym) {
    return {
        {FixTag::MsgSeqNum,    std::to_string(seq)},
        {FixTag::SenderCompID, "100"},
        {FixTag::ClOrdID,      std::to_string(clOrdId)},
        {FixTag::Symbol,       std::to_string(sym)},
        {FixTag::Side,         "1"},
        {FixTag::OrderQty,     "50"},
        {FixTag::Price,        "1000"},
        {FixTag::OrdType,      "2"},
    };
}

}  // namespace

// ─── Fix 1: Missing MsgSeqNum(34) is rejected ───────────────────────────────

TEST(FixValidation, MissingMsgSeqNumIsRejected) {
    MatchingEngine engine;
    engine.addSymbol(7);
    engine.start();

    std::string sent;
    FixSession session(engine, [&](std::string_view b) { sent.append(b); });
    doLogon(session, sent);

    // NewOrderSingle with every required field but NO MsgSeqNum(34).
    auto msg = makeFix("D", {
        {FixTag::SenderCompID, "100"},
        {FixTag::ClOrdID,      "1001"},
        {FixTag::Symbol,       "7"},
        {FixTag::Side,         "1"},
        {FixTag::OrderQty,     "50"},
        {FixTag::Price,        "1000"},
        {FixTag::OrdType,      "2"},
    });
    ASSERT_TRUE(session.feed(msg.data(), msg.size()));

    EXPECT_EQ(session.ordersAccepted(), 0u) << "unsequenced message must not be admitted";
    auto* book = engine.getOrderBook(7);
    ASSERT_NE(book, nullptr);
    EXPECT_EQ(book->getOrder(1001), nullptr) << "unsequenced order must NOT reach the engine";

    ASSERT_FALSE(sent.empty());
    FixMessage resp;
    ASSERT_TRUE(resp.parse(sent.data(), sent.size()));
    EXPECT_EQ(resp.getChar(FixTag::ExecType), '8');
}

// ─── Fix 2: App message before Logon is rejected ────────────────────────────

TEST(FixValidation, NewOrderBeforeLogonIsRejected) {
    MatchingEngine engine;
    engine.addSymbol(7);
    engine.start();

    std::string sent;
    FixSession session(engine, [&](std::string_view b) { sent.append(b); });

    // A fully valid, sequenced NewOrderSingle — but no Logon yet.
    auto msg = makeFix("D", goodOrderTags(/*seq=*/1, /*clOrdId=*/2001, /*sym=*/7));
    ASSERT_TRUE(session.feed(msg.data(), msg.size()));

    EXPECT_EQ(session.ordersAccepted(), 0u);
    EXPECT_FALSE(session.loggedOn());
    auto* book = engine.getOrderBook(7);
    ASSERT_NE(book, nullptr);
    EXPECT_EQ(book->getOrder(2001), nullptr)
        << "app message before Logon must NEVER reach the engine";

    ASSERT_FALSE(sent.empty());
    FixMessage resp;
    ASSERT_TRUE(resp.parse(sent.data(), sent.size()));
    EXPECT_EQ(resp.getChar(FixTag::ExecType), '8');
}

TEST(FixValidation, NewOrderAfterLogonStillFlows) {
    // Regression guard: the stricter checks must not block well-formed,
    // logged-on, sequenced order flow.
    MatchingEngine engine;
    engine.addSymbol(7);
    engine.start();

    std::string sent;
    FixSession session(engine, [&](std::string_view b) { sent.append(b); });
    doLogon(session, sent, /*seq=*/1);

    auto msg = makeFix("D", goodOrderTags(/*seq=*/2, /*clOrdId=*/3001, /*sym=*/7));
    ASSERT_TRUE(session.feed(msg.data(), msg.size()));

    EXPECT_EQ(session.ordersAccepted(), 1u);
    EXPECT_EQ(session.ordersRejected(), 0u);
    auto* book = engine.getOrderBook(7);
    ASSERT_NE(book, nullptr);
    ASSERT_NE(book->getOrder(3001), nullptr);

    ASSERT_FALSE(sent.empty());
    FixMessage resp;
    ASSERT_TRUE(resp.parse(sent.data(), sent.size()));
    EXPECT_TRUE(resp.validateChecksum());
    EXPECT_EQ(resp.getString(FixTag::MsgType), "8");
    EXPECT_EQ(resp.getChar(FixTag::ExecType), '0');
    EXPECT_EQ(resp.getUint64(FixTag::OrderID), 3001u);
}

// ─── Fix 3: NewOrderSingle required fields ──────────────────────────────────

TEST(FixValidation, ParamsRejectMissingOrderQty) {
    auto raw = makeRawNewOrder({
        {FixTag::ClOrdID, "1"}, {FixTag::Symbol, "7"},
        {FixTag::Side, "1"},    {FixTag::Price, "1000"},
        {FixTag::OrdType, "2"},
        // Deliberately no OrderQty(38).
    });
    FixMessage msg;
    ASSERT_TRUE(msg.parse(raw.data(), raw.size()));
    auto params = fixToOrderParams(msg);
    EXPECT_FALSE(params.valid) << "missing OrderQty must not produce a valid order";
    EXPECT_NE(params.error.find("OrderQty"), std::string::npos)
        << "reason should name the missing field; got: " << params.error;
}

TEST(FixValidation, ParamsRejectMissingSide) {
    auto raw = makeRawNewOrder({
        {FixTag::ClOrdID, "1"}, {FixTag::Symbol, "7"},
        {FixTag::OrderQty, "50"}, {FixTag::Price, "1000"},
        {FixTag::OrdType, "2"},
        // Deliberately no Side(54).
    });
    FixMessage msg;
    ASSERT_TRUE(msg.parse(raw.data(), raw.size()));
    auto params = fixToOrderParams(msg);
    EXPECT_FALSE(params.valid);
    EXPECT_NE(params.error.find("Side"), std::string::npos)
        << "reason should name the missing field; got: " << params.error;
}

TEST(FixValidation, ParamsRejectZeroOrderQty) {
    auto raw = makeRawNewOrder({
        {FixTag::ClOrdID, "1"}, {FixTag::Symbol, "7"},
        {FixTag::Side, "1"}, {FixTag::OrderQty, "0"},
        {FixTag::Price, "1000"}, {FixTag::OrdType, "2"},
    });
    FixMessage msg;
    ASSERT_TRUE(msg.parse(raw.data(), raw.size()));
    auto params = fixToOrderParams(msg);
    EXPECT_FALSE(params.valid) << "qty=0 is not a tradeable order";
}

TEST(FixValidation, ParamsAcceptCompleteNewOrder) {
    // Regression guard: a complete order still parses as valid.
    auto raw = makeRawNewOrder({
        {FixTag::SenderCompID, "5"}, {FixTag::ClOrdID, "42"},
        {FixTag::Symbol, "7"}, {FixTag::Side, "1"},
        {FixTag::OrderQty, "50"}, {FixTag::Price, "1000"},
        {FixTag::OrdType, "2"}, {FixTag::TimeInForce, "1"},
    });
    FixMessage msg;
    ASSERT_TRUE(msg.parse(raw.data(), raw.size()));
    auto params = fixToOrderParams(msg);
    EXPECT_TRUE(params.valid);
    EXPECT_EQ(params.orderId, 42u);
    EXPECT_EQ(params.qty, 50u);
    EXPECT_EQ(params.side, Side::Buy);
    EXPECT_EQ(params.orderType, OrderType::Limit);
}

TEST(FixValidation, MissingFieldRejectedAtSessionWithFieldReason) {
    // The session must surface a field-level reason and never call the
    // engine for a NewOrderSingle missing OrderQty.
    MatchingEngine engine;
    engine.addSymbol(7);
    engine.start();

    std::string sent;
    FixSession session(engine, [&](std::string_view b) { sent.append(b); });
    doLogon(session, sent, /*seq=*/1);

    auto msg = makeFix("D", {
        {FixTag::MsgSeqNum, "2"},
        {FixTag::SenderCompID, "100"},
        {FixTag::ClOrdID, "777"},
        {FixTag::Symbol, "7"},
        {FixTag::Side, "1"},
        {FixTag::Price, "1000"},
        {FixTag::OrdType, "2"},
        // No OrderQty(38).
    });
    ASSERT_TRUE(session.feed(msg.data(), msg.size()));

    EXPECT_EQ(session.ordersAccepted(), 0u);
    EXPECT_EQ(session.ordersRejected(), 1u);
    EXPECT_EQ(engine.getOrderBook(7)->getOrder(777), nullptr);

    ASSERT_FALSE(sent.empty());
    FixMessage resp;
    ASSERT_TRUE(resp.parse(sent.data(), sent.size()));
    EXPECT_EQ(resp.getChar(FixTag::ExecType), '8');
    EXPECT_NE(resp.getString(FixTag::Text).find("OrderQty"), std::string_view::npos)
        << "client should get a field-level reason, not a generic/engine reject; got: "
        << resp.getString(FixTag::Text);
    EXPECT_EQ(resp.getUint64(FixTag::OrderID), 777u)
        << "ClOrdID echoed back so the client can correlate the reject";
}

// ─── Fix 4: BodyLength(9) cross-check ────────────────────────────────────────

TEST(FixValidation, WrongBodyLengthIsRejected) {
    // A NewOrderSingle whose declared BodyLength is wrong, but whose
    // CheckSum is internally consistent. The legacy presence-only checks
    // (isWellFormed + validateChecksum) accept it; the length cross-check
    // must reject it.
    auto good = makeFix("D", {
        {FixTag::ClOrdID, "1"}, {FixTag::Symbol, "7"},
        {FixTag::Side, "1"}, {FixTag::OrderQty, "50"},
        {FixTag::Price, "1000"}, {FixTag::OrdType, "2"},
    });
    FixMessage goodMsg;
    ASSERT_TRUE(goodMsg.parse(good.data(), good.size()));
    EXPECT_TRUE(goodMsg.validateBodyLength()) << "correct BodyLength must pass";

    auto bad = makeFixWithBodyLen("D", {
        {FixTag::ClOrdID, "1"}, {FixTag::Symbol, "7"},
        {FixTag::Side, "1"}, {FixTag::OrderQty, "50"},
        {FixTag::Price, "1000"}, {FixTag::OrdType, "2"},
    }, /*declaredLen=*/9999);

    FixMessage badMsg;
    ASSERT_TRUE(badMsg.parse(bad.data(), bad.size()));
    // The old gate would have admitted this:
    EXPECT_TRUE(badMsg.isWellFormed());
    EXPECT_TRUE(badMsg.validateChecksum());
    // The new cross-check catches it:
    EXPECT_FALSE(badMsg.validateBodyLength())
        << "declared BodyLength must equal the measured body byte count";
}

// ─── Fix 5: TargetCompID(56) validation ──────────────────────────────────────

TEST(FixValidation, WrongTargetCompIDIsRejected) {
    MatchingEngine engine;
    engine.addSymbol(7);
    engine.start();

    std::string sent;
    FixSession session(engine, [&](std::string_view b) { sent.append(b); });
    session.setExpectedTargetCompID("VENUE");
    doLogon(session, sent, /*seq=*/1, /*targetCompId=*/"VENUE");

    // Order addressed to the WRONG CompID.
    auto tags = goodOrderTags(/*seq=*/2, /*clOrdId=*/4001, /*sym=*/7);
    tags.push_back({FixTag::TargetCompID, "SOMEONE_ELSE"});
    auto msg = makeFix("D", tags);
    ASSERT_TRUE(session.feed(msg.data(), msg.size()));

    EXPECT_EQ(session.ordersAccepted(), 0u);
    EXPECT_EQ(engine.getOrderBook(7)->getOrder(4001), nullptr)
        << "message for another CompID must NOT reach the engine";

    ASSERT_FALSE(sent.empty());
    FixMessage resp;
    ASSERT_TRUE(resp.parse(sent.data(), sent.size()));
    EXPECT_EQ(resp.getChar(FixTag::ExecType), '8');
}

TEST(FixValidation, CorrectTargetCompIDIsAccepted) {
    MatchingEngine engine;
    engine.addSymbol(7);
    engine.start();

    std::string sent;
    FixSession session(engine, [&](std::string_view b) { sent.append(b); });
    session.setExpectedTargetCompID("VENUE");
    doLogon(session, sent, /*seq=*/1, /*targetCompId=*/"VENUE");

    auto tags = goodOrderTags(/*seq=*/2, /*clOrdId=*/4002, /*sym=*/7);
    tags.push_back({FixTag::TargetCompID, "VENUE"});
    auto msg = makeFix("D", tags);
    ASSERT_TRUE(session.feed(msg.data(), msg.size()));

    EXPECT_EQ(session.ordersAccepted(), 1u);
    ASSERT_NE(engine.getOrderBook(7)->getOrder(4002), nullptr);
}

// ─── Fix 6: Unknown OrdType(40) / TimeInForce(59) ───────────────────────────

TEST(FixValidation, ParamsRejectUnknownOrdType) {
    auto raw = makeRawNewOrder({
        {FixTag::ClOrdID, "1"}, {FixTag::Symbol, "7"},
        {FixTag::Side, "1"}, {FixTag::OrderQty, "50"},
        {FixTag::Price, "1000"},
        {FixTag::OrdType, "9"},  // not 1/2/3/4
    });
    FixMessage msg;
    ASSERT_TRUE(msg.parse(raw.data(), raw.size()));
    auto params = fixToOrderParams(msg);
    EXPECT_FALSE(params.valid) << "unknown OrdType must not silently default to Limit";
    EXPECT_NE(params.error.find("OrdType"), std::string::npos)
        << "reason should name OrdType; got: " << params.error;
}

TEST(FixValidation, ParamsRejectUnknownTimeInForce) {
    auto raw = makeRawNewOrder({
        {FixTag::ClOrdID, "1"}, {FixTag::Symbol, "7"},
        {FixTag::Side, "1"}, {FixTag::OrderQty, "50"},
        {FixTag::Price, "1000"}, {FixTag::OrdType, "2"},
        {FixTag::TimeInForce, "Z"},  // not 0/1/3/4
    });
    FixMessage msg;
    ASSERT_TRUE(msg.parse(raw.data(), raw.size()));
    auto params = fixToOrderParams(msg);
    EXPECT_FALSE(params.valid) << "unknown TimeInForce must not silently default to GTC";
    EXPECT_NE(params.error.find("TimeInForce"), std::string::npos)
        << "reason should name TimeInForce; got: " << params.error;
}

TEST(FixValidation, ParamsAcceptAbsentTimeInForce) {
    // TimeInForce is optional; absence must remain valid (defaults to GTC).
    auto raw = makeRawNewOrder({
        {FixTag::ClOrdID, "1"}, {FixTag::Symbol, "7"},
        {FixTag::Side, "1"}, {FixTag::OrderQty, "50"},
        {FixTag::Price, "1000"}, {FixTag::OrdType, "2"},
        // No TimeInForce(59).
    });
    FixMessage msg;
    ASSERT_TRUE(msg.parse(raw.data(), raw.size()));
    auto params = fixToOrderParams(msg);
    EXPECT_TRUE(params.valid);
    EXPECT_EQ(params.tif, TimeInForce::GTC);
}
