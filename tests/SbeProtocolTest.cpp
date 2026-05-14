// SbeProtocolTest — exercises the Simple Binary Encoding codec
// including the schema-versioning forward-compat guarantee that's
// SBE's defining feature.
//
// Coverage:
//   Byte-order helpers
//     1. U16 little-endian roundtrip + layout
//     2. U32 little-endian roundtrip
//     3. U64 little-endian roundtrip
//     4. I64 sign-preserving roundtrip
//
//   Message header
//     5. Header roundtrip
//     6. Header layout (byte-exact)
//
//   NewOrder v1
//     7. V1 encode + decode roundtrip
//     8. V1 wire layout (byte-exact)
//
//   NewOrder v2
//     9. V2 encode + decode roundtrip
//    10. V2 carries the new participantId + timeInForce fields
//
//   The SBE forward-compat guarantee
//    11. v2 reader decodes a v1 message (gets defaults for new fields)
//    12. The first 24 bytes of v1 and v2 blocks are byte-identical
//        given the same v1 field values — proves v1 readers on v2
//        messages read the original fields unchanged
//    13. v2 reader rejects a block shorter than v1's minimum

#include "SbeProtocol.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>

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

// ─── Byte-order tests ───────────────────────────────────────────────────────

void test_U16Little() {
    TEST(U16Little) {
        uint8_t buf[2];
        writeU16LE(buf, 0x1234);
        // LE: low byte first.
        CHECK(buf[0] == 0x34);
        CHECK(buf[1] == 0x12);
        CHECK(readU16LE(buf) == 0x1234);
    } END
}

void test_U32Little() {
    TEST(U32Little) {
        uint8_t buf[4];
        writeU32LE(buf, 0xAABBCCDD);
        CHECK(buf[0] == 0xDD);
        CHECK(buf[1] == 0xCC);
        CHECK(buf[2] == 0xBB);
        CHECK(buf[3] == 0xAA);
        CHECK(readU32LE(buf) == 0xAABBCCDD);
    } END
}

void test_U64Little() {
    TEST(U64Little) {
        uint8_t buf[8];
        writeU64LE(buf, 0x0102030405060708ULL);
        CHECK(buf[0] == 0x08 && buf[7] == 0x01);
        CHECK(readU64LE(buf) == 0x0102030405060708ULL);
    } END
}

void test_I64SignPreserving() {
    TEST(I64SignPreserving) {
        uint8_t buf[8];
        // Negative price (e.g. for a spread instrument's intrinsic
        // value) must round-trip without losing the sign.
        int64_t v = -1234567890LL;
        writeI64LE(buf, v);
        CHECK(readI64LE(buf) == v);
    } END
}

// ─── MessageHeader tests ────────────────────────────────────────────────────

void test_HeaderRoundtrip() {
    TEST(HeaderRoundtrip) {
        SbeMessageHeader h{32, 1, 100, 2};
        uint8_t buf[SBE_MESSAGE_HEADER_BYTES];
        writeSbeMessageHeader(buf, h);
        auto h2 = readSbeMessageHeader(buf);
        CHECK(h2.blockLength == 32);
        CHECK(h2.templateId == 1);
        CHECK(h2.schemaId == 100);
        CHECK(h2.version == 2);
    } END
}

void test_HeaderLayout() {
    TEST(HeaderLayout) {
        // Pin the layout to spec: 4 little-endian uint16 fields, 8
        // bytes total, in the order [blockLength, templateId,
        // schemaId, version].
        SbeMessageHeader h{0x1234, 0x5678, 0x9ABC, 0xDEF0};
        uint8_t buf[SBE_MESSAGE_HEADER_BYTES];
        writeSbeMessageHeader(buf, h);
        CHECK(buf[0] == 0x34); CHECK(buf[1] == 0x12);  // blockLength
        CHECK(buf[2] == 0x78); CHECK(buf[3] == 0x56);  // templateId
        CHECK(buf[4] == 0xBC); CHECK(buf[5] == 0x9A);  // schemaId
        CHECK(buf[6] == 0xF0); CHECK(buf[7] == 0xDE);  // version
    } END
}

// ─── NewOrder v1 tests ──────────────────────────────────────────────────────

void test_NewOrderV1Roundtrip() {
    TEST(NewOrderV1Roundtrip) {
        SbeNewOrderV1 in;
        in.orderId   = 42ULL;
        in.price     = 100000;
        in.quantity  = 500;
        in.side      = 1;  // Buy
        in.orderType = 1;  // Limit

        uint8_t buf[SBE_MESSAGE_HEADER_BYTES + SBE_NEW_ORDER_V1_BLOCK_BYTES];
        size_t n = encodeSbeNewOrderV1(buf, in);
        CHECK(n == sizeof(buf));

        auto h = readSbeMessageHeader(buf);
        CHECK(h.blockLength == SBE_NEW_ORDER_V1_BLOCK_BYTES);
        CHECK(h.templateId == SBE_TEMPLATE_NEW_ORDER);
        CHECK(h.schemaId == SBE_SCHEMA_ID);
        CHECK(h.version == SBE_NEW_ORDER_V1);

        SbeNewOrderV1 out;
        readSbeNewOrderV1Block(buf + SBE_MESSAGE_HEADER_BYTES, out);
        CHECK(out.orderId == 42ULL);
        CHECK(out.price == 100000);
        CHECK(out.quantity == 500);
        CHECK(out.side == 1);
        CHECK(out.orderType == 1);
    } END
}

void test_NewOrderV1Layout() {
    TEST(NewOrderV1Layout) {
        // orderId=0x0102030405060708, price=0x11..18, qty=0xAABBCCDD,
        // side=1, orderType=2 (Market).
        SbeNewOrderV1 in;
        in.orderId   = 0x0102030405060708ULL;
        in.price     = 0x1112131415161718LL;
        in.quantity  = 0xAABBCCDD;
        in.side      = 1;
        in.orderType = 2;

        uint8_t buf[SBE_MESSAGE_HEADER_BYTES + SBE_NEW_ORDER_V1_BLOCK_BYTES];
        encodeSbeNewOrderV1(buf, in);
        const uint8_t* blk = buf + SBE_MESSAGE_HEADER_BYTES;

        // orderId at offset 0, little-endian
        CHECK(blk[0] == 0x08 && blk[7] == 0x01);
        // price at offset 8
        CHECK(blk[8] == 0x18 && blk[15] == 0x11);
        // quantity at offset 16
        CHECK(blk[16] == 0xDD && blk[19] == 0xAA);
        CHECK(blk[20] == 1);   // side
        CHECK(blk[21] == 2);   // orderType
        CHECK(blk[22] == 0 && blk[23] == 0);  // padding
    } END
}

// ─── NewOrder v2 tests ──────────────────────────────────────────────────────

void test_NewOrderV2Roundtrip() {
    TEST(NewOrderV2Roundtrip) {
        SbeNewOrderV2 in;
        in.orderId       = 7ULL;
        in.price         = 200000;
        in.quantity      = 100;
        in.side          = 2;  // Sell
        in.orderType     = 1;
        in.participantId = 999;
        in.timeInForce   = 1;  // DAY

        uint8_t buf[SBE_MESSAGE_HEADER_BYTES + SBE_NEW_ORDER_V2_BLOCK_BYTES];
        size_t n = encodeSbeNewOrderV2(buf, in);
        CHECK(n == sizeof(buf));

        auto h = readSbeMessageHeader(buf);
        CHECK(h.blockLength == SBE_NEW_ORDER_V2_BLOCK_BYTES);
        CHECK(h.version == SBE_NEW_ORDER_V2);

        SbeNewOrderV2 out;
        CHECK(readSbeNewOrderV2Block(buf + SBE_MESSAGE_HEADER_BYTES,
                                     h.blockLength, out));
        CHECK(out.orderId == 7ULL);
        CHECK(out.price == 200000);
        CHECK(out.quantity == 100);
        CHECK(out.side == 2);
        CHECK(out.participantId == 999);
        CHECK(out.timeInForce == 1);
    } END
}

void test_NewOrderV2CarriesNewFields() {
    TEST(NewOrderV2CarriesNewFields) {
        SbeNewOrderV2 in{};
        in.orderId = 1;
        in.participantId = 0xCAFEU;
        in.timeInForce = 1;
        uint8_t buf[SBE_MESSAGE_HEADER_BYTES + SBE_NEW_ORDER_V2_BLOCK_BYTES];
        encodeSbeNewOrderV2(buf, in);

        // participantId at offset 24 inside the block (little-endian
        // 0xCAFE = 0xFE, 0xCA, 0x00, 0x00).
        const uint8_t* blk = buf + SBE_MESSAGE_HEADER_BYTES;
        CHECK(blk[24] == 0xFE);
        CHECK(blk[25] == 0xCA);
        CHECK(blk[28] == 1);     // timeInForce = DAY
    } END
}

// ─── The headline test: forward-compatible decode ───────────────────────────

void test_V2ReaderDecodesV1Message() {
    TEST(V2ReaderDecodesV1Message) {
        // A v1 publisher emits an order. A v2 subscriber reads it.
        // The new fields aren't on the wire — the v2 reader gets
        // the documented defaults instead.
        SbeNewOrderV1 v1in;
        v1in.orderId   = 100;
        v1in.price     = 500000;
        v1in.quantity  = 50;
        v1in.side      = 1;
        v1in.orderType = 1;

        uint8_t buf[SBE_MESSAGE_HEADER_BYTES + SBE_NEW_ORDER_V1_BLOCK_BYTES];
        encodeSbeNewOrderV1(buf, v1in);

        // v2 reader: it sees the v1 message and decodes WITHOUT
        // any code change in the v2 codec — it uses the header's
        // blockLength to decide whether to read the new fields.
        auto h = readSbeMessageHeader(buf);
        SbeNewOrderV2 v2out;
        CHECK(readSbeNewOrderV2Block(buf + SBE_MESSAGE_HEADER_BYTES,
                                     h.blockLength, v2out));
        CHECK(v2out.orderId == 100);
        CHECK(v2out.price == 500000);
        CHECK(v2out.quantity == 50);
        CHECK(v2out.side == 1);
        CHECK(v2out.orderType == 1);
        // v1 didn't carry these — v2 reader gets the documented
        // defaults instead of UB on uninitialized memory.
        CHECK(v2out.participantId == 0
              && "v1 message has no participantId — default to 0");
        CHECK(v2out.timeInForce == 0
              && "v1 message has no timeInForce — default to GTC");
    } END
}

void test_V1AndV2BlockPrefixIsByteIdentical() {
    TEST(V1AndV2BlockPrefixIsByteIdentical) {
        // The SBE forward-compat guarantee in its strongest form:
        // for any field set valid in v1, encoding it as v2 (with
        // defaults in the new fields) produces a byte sequence
        // whose first 24 bytes match the v1 encoding exactly. This
        // means a v1 reader looking at a v2 message reads the v1
        // fields unchanged — no codec branch, no version dispatch.
        SbeNewOrderV1 v1{};
        v1.orderId = 0xABCDEF; v1.price = -1234; v1.quantity = 7;
        v1.side = 1; v1.orderType = 3;

        SbeNewOrderV2 v2{};
        static_cast<SbeNewOrderV1&>(v2) = v1;
        v2.participantId = 0; v2.timeInForce = 0;

        uint8_t bufV1[SBE_MESSAGE_HEADER_BYTES + SBE_NEW_ORDER_V1_BLOCK_BYTES];
        uint8_t bufV2[SBE_MESSAGE_HEADER_BYTES + SBE_NEW_ORDER_V2_BLOCK_BYTES];
        encodeSbeNewOrderV1(bufV1, v1);
        encodeSbeNewOrderV2(bufV2, v2);

        // Block starts after the 8-byte header. The first 24 bytes
        // of each block must match exactly.
        CHECK(std::memcmp(bufV1 + SBE_MESSAGE_HEADER_BYTES,
                          bufV2 + SBE_MESSAGE_HEADER_BYTES,
                          SBE_NEW_ORDER_V1_BLOCK_BYTES) == 0
              && "v1 block must be a byte-exact prefix of v2 block");
    } END
}

void test_V2ReaderRejectsTooShortBlock() {
    TEST(V2ReaderRejectsTooShortBlock) {
        uint8_t buf[10] = {0};
        SbeNewOrderV2 out;
        CHECK(!readSbeNewOrderV2Block(buf, /*actualBlockBytes=*/10, out)
              && "below v1 min block size must fail to decode");
    } END
}

int main() {
    std::cout << "Running SbeProtocolTest\n";

    test_U16Little();
    test_U32Little();
    test_U64Little();
    test_I64SignPreserving();

    test_HeaderRoundtrip();
    test_HeaderLayout();

    test_NewOrderV1Roundtrip();
    test_NewOrderV1Layout();

    test_NewOrderV2Roundtrip();
    test_NewOrderV2CarriesNewFields();

    test_V2ReaderDecodesV1Message();
    test_V1AndV2BlockPrefixIsByteIdentical();
    test_V2ReaderRejectsTooShortBlock();

    std::cout << "\n" << tests_passed << " passed, "
              << tests_failed << " failed\n";
    return tests_failed > 0 ? 1 : 0;
}
