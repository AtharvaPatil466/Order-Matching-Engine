// SbeSessionTest — exercises the SBE order-entry session.
//
// Coverage:
//   OrderAck codec
//     1. Ack v1 roundtrip
//     2. Ack v1 layout (byte-exact)
//
//   Session basics
//     3. NewOrder v1 → engine submitOrder + Accepted ack
//     4. NewOrder v2 → engine submitOrder (participantId honored) + ack
//     5. Reject for invalid symbol (no symbol seeded)
//
//   SBE forward-compat at the session boundary
//     6. v1 message arriving at v2-aware session decodes with
//        defaults — engine still receives a valid order
//
//   Framing
//     7. Byte-at-a-time fragmentation reassembles correctly
//     8. Two messages in one feed both dispatch in order
//     9. Schema-id mismatch returns false (hard error)
//    10. Block-length absurdly large returns false (hard error)
//
//   Unknown message type
//    11. Unknown templateId is skipped without crashing the session

#include "MatchingEngine.h"
#include "SbeProtocol.h"
#include "SbeSession.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace OrderMatcher;

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name)                                                        \
    std::cout << "  " << #name << "... " << std::flush;                   \
    try
#define END                                                               \
    catch (const std::exception& e) {                                     \
        std::cout << "FAIL: " << e.what() << "\n";                        \
        ++tests_failed;                                                   \
        return;                                                           \
    }                                                                     \
    std::cout << "ok\n";                                                  \
    ++tests_passed;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            throw std::runtime_error("CHECK failed: " #cond);             \
        }                                                                 \
    } while (0)

// ─── OrderAck codec tests ───────────────────────────────────────────────────

void test_AckRoundtrip() {
    TEST(AckRoundtrip) {
        SbeOrderAckV1 in;
        in.orderId      = 0xDEADBEEFULL;
        in.status       = SBE_ORDER_ACK_STATUS_REJECTED;
        in.rejectReason = static_cast<uint8_t>(RejectReason::InvalidPrice);

        uint8_t buf[SBE_MESSAGE_HEADER_BYTES + SBE_ORDER_ACK_V1_BLOCK_BYTES];
        size_t n = encodeSbeOrderAckV1(buf, in);
        CHECK(n == sizeof(buf));

        auto h = readSbeMessageHeader(buf);
        CHECK(h.templateId == SBE_TEMPLATE_ORDER_ACK);
        CHECK(h.blockLength == SBE_ORDER_ACK_V1_BLOCK_BYTES);

        SbeOrderAckV1 out;
        readSbeOrderAckV1Block(buf + SBE_MESSAGE_HEADER_BYTES, out);
        CHECK(out.orderId == 0xDEADBEEFULL);
        CHECK(out.status == SBE_ORDER_ACK_STATUS_REJECTED);
        CHECK(out.rejectReason ==
              static_cast<uint8_t>(RejectReason::InvalidPrice));
    } END
}

void test_AckLayout() {
    TEST(AckLayout) {
        SbeOrderAckV1 in;
        in.orderId      = 0x0102030405060708ULL;
        in.status       = SBE_ORDER_ACK_STATUS_ACCEPTED;
        in.rejectReason = 0;
        uint8_t buf[SBE_MESSAGE_HEADER_BYTES + SBE_ORDER_ACK_V1_BLOCK_BYTES];
        encodeSbeOrderAckV1(buf, in);
        const uint8_t* blk = buf + SBE_MESSAGE_HEADER_BYTES;
        CHECK(blk[0] == 0x08);   // LE
        CHECK(blk[7] == 0x01);
        CHECK(blk[8] == SBE_ORDER_ACK_STATUS_ACCEPTED);
        CHECK(blk[9] == 0);
    } END
}

// ─── Session tests ──────────────────────────────────────────────────────────

namespace {

std::vector<uint8_t> buildNewOrderV1Wire(uint64_t orderId, Price price,
                                         Quantity qty, uint8_t side = 1,
                                         uint8_t type = 1) {
    SbeNewOrderV1 m;
    m.orderId   = orderId;
    m.price     = price;
    m.quantity  = qty;
    m.side      = side;
    m.orderType = type;
    std::vector<uint8_t> buf(SBE_MESSAGE_HEADER_BYTES +
                              SBE_NEW_ORDER_V1_BLOCK_BYTES);
    encodeSbeNewOrderV1(buf.data(), m);
    return buf;
}

std::vector<uint8_t> buildNewOrderV2Wire(uint64_t orderId, Price price,
                                         Quantity qty, uint32_t pid,
                                         uint8_t tif,
                                         uint8_t side = 1, uint8_t type = 1) {
    SbeNewOrderV2 m{};
    m.orderId       = orderId;
    m.price         = price;
    m.quantity      = qty;
    m.side          = side;
    m.orderType     = type;
    m.participantId = pid;
    m.timeInForce   = tif;
    std::vector<uint8_t> buf(SBE_MESSAGE_HEADER_BYTES +
                              SBE_NEW_ORDER_V2_BLOCK_BYTES);
    encodeSbeNewOrderV2(buf.data(), m);
    return buf;
}

}  // namespace

void test_SessionNewOrderV1Accepted() {
    TEST(SessionNewOrderV1Accepted) {
        MatchingEngine engine;
        engine.addSymbol(0);  // session routes to symbol 0
        engine.start();

        std::string sent;
        SbeSession sess(engine, [&](std::string_view b) { sent.append(b); });

        auto wire = buildNewOrderV1Wire(/*id=*/1001, /*price=*/1000,
                                        /*qty=*/50);
        CHECK(sess.feed(reinterpret_cast<const char*>(wire.data()),
                        wire.size()));
        CHECK(sess.ordersAccepted() == 1);
        CHECK(sess.ordersRejected() == 0);

        auto* book = engine.getOrderBook(0);
        CHECK(book != nullptr);
        CHECK(book->getOrder(1001) != nullptr);

        // Wire response: an SBE OrderAck with status=Accepted.
        CHECK(sent.size() == SBE_MESSAGE_HEADER_BYTES +
                              SBE_ORDER_ACK_V1_BLOCK_BYTES);
        const auto* p = reinterpret_cast<const uint8_t*>(sent.data());
        auto h = readSbeMessageHeader(p);
        CHECK(h.templateId == SBE_TEMPLATE_ORDER_ACK);
        SbeOrderAckV1 ack;
        readSbeOrderAckV1Block(p + SBE_MESSAGE_HEADER_BYTES, ack);
        CHECK(ack.orderId == 1001);
        CHECK(ack.status == SBE_ORDER_ACK_STATUS_ACCEPTED);
    } END
}

void test_SessionNewOrderV2HonorsParticipant() {
    TEST(SessionNewOrderV2HonorsParticipant) {
        MatchingEngine engine;
        engine.addSymbol(0);
        engine.start();

        SbeSession sess(engine, [](std::string_view) {});
        auto wire = buildNewOrderV2Wire(/*id=*/2002, /*price=*/500,
                                         /*qty=*/10, /*pid=*/777,
                                         /*tif=*/1);
        CHECK(sess.feed(reinterpret_cast<const char*>(wire.data()),
                        wire.size()));
        CHECK(sess.ordersAccepted() == 1);

        auto* book = engine.getOrderBook(0);
        const Order* o = book->getOrder(2002);
        CHECK(o != nullptr);
        CHECK(o->participantId == 777);
    } END
}

void test_SessionForwardsEngineRejectReason() {
    TEST(SessionForwardsEngineRejectReason) {
        // The engine auto-registers symbol 0 in its constructor
        // (ensureDefaultSymbol), so we can't trigger SymbolNotFound
        // via the session's default routing. Use EngineStopped
        // instead — don't call start(). submitOrder rejects with
        // RejectReason::EngineStopped, which must arrive verbatim
        // in the ack's rejectReason byte.
        MatchingEngine engine;
        // Deliberately NOT calling engine.start().

        std::string sent;
        SbeSession sess(engine, [&](std::string_view b) { sent.append(b); });
        auto wire = buildNewOrderV1Wire(3003, 1000, 10);
        CHECK(sess.feed(reinterpret_cast<const char*>(wire.data()),
                        wire.size()));
        CHECK(sess.ordersAccepted() == 0);
        CHECK(sess.ordersRejected() == 1);

        const auto* p = reinterpret_cast<const uint8_t*>(sent.data());
        SbeOrderAckV1 ack;
        readSbeOrderAckV1Block(p + SBE_MESSAGE_HEADER_BYTES, ack);
        CHECK(ack.status == SBE_ORDER_ACK_STATUS_REJECTED);
        CHECK(ack.rejectReason ==
              static_cast<uint8_t>(RejectReason::EngineStopped));
    } END
}

// ─── SBE forward-compat at the session boundary ─────────────────────────────

void test_SessionDecodesV1MessageWithDefaults() {
    TEST(SessionDecodesV1MessageWithDefaults) {
        // The session always uses the v2 reader. A v1 message
        // arriving at this session still works: the decoder fills
        // participantId=0 and timeInForce=GTC. The engine receives
        // a valid order.
        MatchingEngine engine;
        engine.addSymbol(0);
        engine.start();

        SbeSession sess(engine, [](std::string_view) {});
        auto wire = buildNewOrderV1Wire(/*id=*/4004, /*price=*/750,
                                        /*qty=*/20);
        CHECK(sess.feed(reinterpret_cast<const char*>(wire.data()),
                        wire.size()));
        CHECK(sess.ordersAccepted() == 1);
        auto* book = engine.getOrderBook(0);
        const Order* o = book->getOrder(4004);
        CHECK(o != nullptr);
        CHECK(o->participantId == 0
              && "v1 didn't carry participantId — default 0 honored");
    } END
}

// ─── Framing ────────────────────────────────────────────────────────────────

void test_SessionByteFragmentation() {
    TEST(SessionByteFragmentation) {
        MatchingEngine engine;
        engine.addSymbol(0);
        engine.start();

        SbeSession sess(engine, [](std::string_view) {});
        auto wire = buildNewOrderV1Wire(5005, 100, 5);
        for (size_t i = 0; i < wire.size(); ++i) {
            CHECK(sess.feed(reinterpret_cast<const char*>(wire.data() + i), 1));
            if (i + 1 < wire.size()) {
                CHECK(sess.ordersAccepted() == 0
                      && "must NOT dispatch on a partial message");
            }
        }
        CHECK(sess.ordersAccepted() == 1);
    } END
}

void test_SessionBatchedMessages() {
    TEST(SessionBatchedMessages) {
        MatchingEngine engine;
        engine.addSymbol(0);
        engine.start();

        SbeSession sess(engine, [](std::string_view) {});
        auto w1 = buildNewOrderV1Wire(6001, 100, 5);
        auto w2 = buildNewOrderV1Wire(6002, 100, 5);
        std::vector<uint8_t> joined(w1.begin(), w1.end());
        joined.insert(joined.end(), w2.begin(), w2.end());

        CHECK(sess.feed(reinterpret_cast<const char*>(joined.data()),
                        joined.size()));
        CHECK(sess.messagesProcessed() == 2);
        CHECK(sess.ordersAccepted() == 2);
    } END
}

void test_SessionRejectsBadSchemaId() {
    TEST(SessionRejectsBadSchemaId) {
        MatchingEngine engine; engine.start();
        SbeSession sess(engine, [](std::string_view) {});

        // Build a header with a wrong schemaId; followed by 24
        // bytes of "block" so the framer treats it as a complete
        // packet.
        uint8_t buf[SBE_MESSAGE_HEADER_BYTES +
                    SBE_NEW_ORDER_V1_BLOCK_BYTES] = {0};
        SbeMessageHeader bad{SBE_NEW_ORDER_V1_BLOCK_BYTES,
                              SBE_TEMPLATE_NEW_ORDER,
                              /*schemaId=*/999,  // wrong
                              SBE_NEW_ORDER_V1};
        writeSbeMessageHeader(buf, bad);
        CHECK(!sess.feed(reinterpret_cast<const char*>(buf), sizeof(buf)));
    } END
}

void test_SessionRejectsAbsurdBlockLength() {
    TEST(SessionRejectsAbsurdBlockLength) {
        MatchingEngine engine; engine.start();
        SbeSession sess(engine, [](std::string_view) {});

        uint8_t buf[SBE_MESSAGE_HEADER_BYTES] = {0};
        SbeMessageHeader h{/*blockLen=*/0xFFFF,  // 65535 — too big
                            SBE_TEMPLATE_NEW_ORDER,
                            SBE_SCHEMA_ID, 1};
        writeSbeMessageHeader(buf, h);
        CHECK(!sess.feed(reinterpret_cast<const char*>(buf), sizeof(buf)));
    } END
}

void test_SessionSkipsUnknownTemplate() {
    TEST(SessionSkipsUnknownTemplate) {
        MatchingEngine engine; engine.start();
        SbeSession sess(engine, [](std::string_view) {});

        uint8_t buf[SBE_MESSAGE_HEADER_BYTES + 4] = {0};
        SbeMessageHeader h{/*blockLen=*/4,
                            /*templateId=*/999,  // unknown
                            SBE_SCHEMA_ID, 1};
        writeSbeMessageHeader(buf, h);
        // Returns true — recoverable; the session just skips and
        // counts the unknown.
        CHECK(sess.feed(reinterpret_cast<const char*>(buf), sizeof(buf)));
        CHECK(sess.messagesSkipped() == 1);
        CHECK(sess.messagesProcessed() == 1
              && "framer still counts it as processed");
    } END
}

int main() {
    std::cout << "Running SbeSessionTest\n";

    test_AckRoundtrip();
    test_AckLayout();

    test_SessionNewOrderV1Accepted();
    test_SessionNewOrderV2HonorsParticipant();
    test_SessionForwardsEngineRejectReason();
    test_SessionDecodesV1MessageWithDefaults();

    test_SessionByteFragmentation();
    test_SessionBatchedMessages();
    test_SessionRejectsBadSchemaId();
    test_SessionRejectsAbsurdBlockLength();
    test_SessionSkipsUnknownTemplate();

    std::cout << "\n" << tests_passed << " passed, "
              << tests_failed << " failed\n";
    return tests_failed > 0 ? 1 : 0;
}
