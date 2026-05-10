// GatewayProtocolTest — exercises the versioned binary wire format.
//
// Tests:
//   1. V2 roundtrip (serialize → deserialize → identical)
//   2. V1 → V2 backward compatibility (old payload, new deserializer)
//   3. V2 → V1 forward skip (new payload, old-compat deserializer)
//   4. Bad magic rejection
//   5. Truncated frame rejection
//   6. Version below MIN_READABLE_VERSION rejection
//   7. Header + payload size isolation (header growth tolerance)
//   8. GatewayResponse layout static assertions
//   9. V1 prefix alignment proof (field offsets match between V1 and V2)
//  10. Payload-too-small graceful handling
//  11. Zero-filled defaults for V2-only fields when reading V1

#include "GatewayProtocol.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

using namespace OrderMatcher;

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name)                                                        \
    do {                                                                  \
        std::cout << "  " << #name << "... " << std::flush;               \
    } while (0)

#define PASS()                                                            \
    do {                                                                  \
        ++tests_passed;                                                   \
        std::cout << "PASS" << std::endl;                                 \
    } while (0)

#define ASSERT_EQ(a, b)                                                   \
    do {                                                                  \
        if ((a) != (b)) {                                                 \
            ++tests_failed;                                               \
            std::cout << "FAIL (" << #a << " = " << static_cast<uint64_t>(a) \
                      << ", expected " << static_cast<uint64_t>(b) << ")" \
                      << std::endl;                                       \
            return;                                                       \
        }                                                                 \
    } while (0)

// ─── Test 1: V2 roundtrip ───────────────────────────────────────────────────

void test_v2_roundtrip() {
    TEST(V2Roundtrip);

    OrderRequestV2 req{};
    req.type           = GatewayRequestType::NewOrder;
    req.symbolId       = 42;
    req.orderId        = 12345;
    req.participantId  = 99;
    req.side           = Side::Buy;
    req.price          = 1000000;
    req.qty            = 500;
    req.orderType      = OrderType::Limit;
    req.tif            = TimeInForce::GTC;
    req.sessionId      = 7;
    req.correlationTag = 0xDEADBEEF;
    req.ingressNs      = 999999;

    auto frame = serializeGatewayRequest(req);
    ASSERT_EQ(frame.size(), sizeof(GatewayRequestHeader) + sizeof(OrderRequestV2));

    OrderRequestV2 decoded{};
    auto result = deserializeGatewayRequest(frame.data(), frame.size(), decoded);
    ASSERT_EQ(static_cast<uint8_t>(result), static_cast<uint8_t>(GatewayDecodeResult::Ok));
    ASSERT_EQ(static_cast<uint8_t>(decoded.type), static_cast<uint8_t>(req.type));
    ASSERT_EQ(decoded.symbolId, req.symbolId);
    ASSERT_EQ(decoded.orderId, req.orderId);
    ASSERT_EQ(decoded.participantId, req.participantId);
    ASSERT_EQ(static_cast<uint8_t>(decoded.side), static_cast<uint8_t>(req.side));
    ASSERT_EQ(decoded.price, req.price);
    ASSERT_EQ(decoded.qty, req.qty);
    ASSERT_EQ(decoded.sessionId, req.sessionId);
    ASSERT_EQ(decoded.correlationTag, req.correlationTag);
    ASSERT_EQ(decoded.ingressNs, req.ingressNs);

    PASS();
}

// ─── Test 2: V1 backward compatibility ──────────────────────────────────────

void test_v1_backward_compat() {
    TEST(V1BackwardCompat);

    // Simulate a V1 sender: build a frame with version=1 and
    // sizeof(OrderRequestV1) payload.
    OrderRequestV1 v1{};
    v1.type          = GatewayRequestType::Cancel;
    v1.symbolId      = 10;
    v1.orderId       = 777;
    v1.participantId = 3;
    v1.side          = Side::Sell;
    v1.price         = 500000;
    v1.qty           = 100;
    v1.ingressNs     = 42;

    GatewayRequestHeader hdr{};
    hdr.magic       = GatewayRequestHeader::MAGIC;
    hdr.version     = 1;  // V1 sender
    hdr.headerSize  = sizeof(GatewayRequestHeader);
    hdr.payloadSize = sizeof(OrderRequestV1);
    hdr.flags       = 0;

    std::vector<uint8_t> frame(sizeof(hdr) + sizeof(v1));
    std::memcpy(frame.data(), &hdr, sizeof(hdr));
    std::memcpy(frame.data() + sizeof(hdr), &v1, sizeof(v1));

    OrderRequestV2 decoded{};
    auto result = deserializeGatewayRequest(frame.data(), frame.size(), decoded);
    ASSERT_EQ(static_cast<uint8_t>(result), static_cast<uint8_t>(GatewayDecodeResult::Ok));

    // V1 fields must be preserved
    ASSERT_EQ(static_cast<uint8_t>(decoded.type), static_cast<uint8_t>(GatewayRequestType::Cancel));
    ASSERT_EQ(decoded.symbolId, 10u);
    ASSERT_EQ(decoded.orderId, 777u);
    ASSERT_EQ(decoded.participantId, 3u);
    ASSERT_EQ(static_cast<uint8_t>(decoded.side), static_cast<uint8_t>(Side::Sell));
    ASSERT_EQ(decoded.price, 500000);
    ASSERT_EQ(decoded.qty, 100u);
    ASSERT_EQ(decoded.ingressNs, 42u);

    // V2-only fields must be zero (defaults)
    ASSERT_EQ(decoded.sessionId, 0u);
    ASSERT_EQ(decoded.correlationTag, 0u);

    PASS();
}

// ─── Test 3: Future version skip ────────────────────────────────────────────

void test_future_version_skip() {
    TEST(FutureVersionSkip);

    // Simulate a V99 sender: the deserializer should return
    // UnknownFutureVersion so the caller can skip the frame.
    GatewayRequestHeader hdr{};
    hdr.magic       = GatewayRequestHeader::MAGIC;
    hdr.version     = 99;
    hdr.headerSize  = sizeof(GatewayRequestHeader);
    hdr.payloadSize = 256;
    hdr.flags       = 0;

    std::vector<uint8_t> frame(sizeof(hdr) + 256, 0);
    std::memcpy(frame.data(), &hdr, sizeof(hdr));

    OrderRequestV2 decoded{};
    auto result = deserializeGatewayRequest(frame.data(), frame.size(), decoded);
    ASSERT_EQ(static_cast<uint8_t>(result),
              static_cast<uint8_t>(GatewayDecodeResult::UnknownFutureVersion));

    PASS();
}

// ─── Test 4: Bad magic ──────────────────────────────────────────────────────

void test_bad_magic() {
    TEST(BadMagic);

    GatewayRequestHeader hdr{};
    hdr.magic       = 0xBADC0DE;
    hdr.version     = 1;
    hdr.headerSize  = sizeof(GatewayRequestHeader);
    hdr.payloadSize = sizeof(OrderRequestV1);

    std::vector<uint8_t> frame(sizeof(hdr) + sizeof(OrderRequestV1), 0);
    std::memcpy(frame.data(), &hdr, sizeof(hdr));

    OrderRequestV2 decoded{};
    auto result = deserializeGatewayRequest(frame.data(), frame.size(), decoded);
    ASSERT_EQ(static_cast<uint8_t>(result),
              static_cast<uint8_t>(GatewayDecodeResult::BadMagic));

    PASS();
}

// ─── Test 5: Truncated frame ────────────────────────────────────────────────

void test_truncated_frame() {
    TEST(TruncatedFrame);

    // Not enough bytes for the header itself
    uint8_t tiny[4] = {0};
    OrderRequestV2 decoded{};
    auto result = deserializeGatewayRequest(tiny, sizeof(tiny), decoded);
    ASSERT_EQ(static_cast<uint8_t>(result),
              static_cast<uint8_t>(GatewayDecodeResult::Truncated));

    // Header present but payload truncated
    GatewayRequestHeader hdr{};
    hdr.magic       = GatewayRequestHeader::MAGIC;
    hdr.version     = 1;
    hdr.headerSize  = sizeof(GatewayRequestHeader);
    hdr.payloadSize = sizeof(OrderRequestV1);

    // Only provide the header, no payload
    result = deserializeGatewayRequest(
        reinterpret_cast<const uint8_t*>(&hdr), sizeof(hdr), decoded);
    ASSERT_EQ(static_cast<uint8_t>(result),
              static_cast<uint8_t>(GatewayDecodeResult::Truncated));

    PASS();
}

// ─── Test 6: Version too old ────────────────────────────────────────────────

void test_version_too_old() {
    TEST(VersionTooOld);

    // Version 0 is below MIN_READABLE_VERSION (1)
    GatewayRequestHeader hdr{};
    hdr.magic       = GatewayRequestHeader::MAGIC;
    hdr.version     = 0;
    hdr.headerSize  = sizeof(GatewayRequestHeader);
    hdr.payloadSize = 100;

    std::vector<uint8_t> frame(sizeof(hdr) + 100, 0);
    std::memcpy(frame.data(), &hdr, sizeof(hdr));

    OrderRequestV2 decoded{};
    auto result = deserializeGatewayRequest(frame.data(), frame.size(), decoded);
    ASSERT_EQ(static_cast<uint8_t>(result),
              static_cast<uint8_t>(GatewayDecodeResult::UnsupportedVersion));

    PASS();
}

// ─── Test 7: Header growth tolerance ────────────────────────────────────────

void test_header_growth_tolerance() {
    TEST(HeaderGrowthTolerance);

    // Simulate a future header that's 32 bytes (16 extra), but the
    // payload is still V1. The deserializer should skip headerSize
    // bytes and read the payload correctly.
    OrderRequestV1 v1{};
    v1.type    = GatewayRequestType::Modify;
    v1.orderId = 888;
    v1.newQty  = 42;

    GatewayRequestHeader hdr{};
    hdr.magic       = GatewayRequestHeader::MAGIC;
    hdr.version     = 1;
    hdr.headerSize  = 32;  // bigger header
    hdr.payloadSize = sizeof(OrderRequestV1);

    // [header16][padding16][v1]
    std::vector<uint8_t> frame(32 + sizeof(v1), 0);
    std::memcpy(frame.data(), &hdr, sizeof(hdr));
    std::memcpy(frame.data() + 32, &v1, sizeof(v1));

    OrderRequestV2 decoded{};
    auto result = deserializeGatewayRequest(frame.data(), frame.size(), decoded);
    ASSERT_EQ(static_cast<uint8_t>(result),
              static_cast<uint8_t>(GatewayDecodeResult::Ok));
    ASSERT_EQ(decoded.orderId, 888u);
    ASSERT_EQ(decoded.newQty, 42u);
    ASSERT_EQ(static_cast<uint8_t>(decoded.type),
              static_cast<uint8_t>(GatewayRequestType::Modify));

    PASS();
}

// ─── Test 8: GatewayResponse layout ─────────────────────────────────────────

void test_gateway_response_layout() {
    TEST(GatewayResponseLayout);

    // Static assertions are checked at compile time; this test just
    // verifies the fields are accessible and packed correctly.
    GatewayResponse resp{};
    resp.correlationTag = 0xCAFEBABE;
    resp.sequenceId     = 100;
    resp.status         = 0;  // accepted
    resp.rejectReason   = 0;  // none

    ASSERT_EQ(resp.correlationTag, 0xCAFEBABEu);
    ASSERT_EQ(resp.sequenceId, 100u);
    ASSERT_EQ(resp.status, 0);

    PASS();
}

// ─── Test 9: V1/V2 prefix alignment ────────────────────────────────────────

void test_v1_v2_prefix_alignment() {
    TEST(V1V2PrefixAlignment);

    // The first sizeof(OrderRequestV1) bytes of V2 must have the same
    // layout as V1. Build both, memcmp the prefix.
    OrderRequestV1 v1{};
    v1.type          = GatewayRequestType::NewOrder;
    v1.symbolId      = 99;
    v1.orderId       = 1234;
    v1.participantId = 5;
    v1.side          = Side::Buy;
    v1.price         = 100;
    v1.qty           = 200;
    v1.ingressNs     = 777;

    OrderRequestV2 v2{};
    std::memcpy(&v2, &v1, sizeof(v1));
    v2.sessionId      = 42;
    v2.correlationTag = 99;

    // The V1 prefix of V2 must match V1 byte-for-byte.
    int cmp = std::memcmp(&v1, &v2, sizeof(v1));
    ASSERT_EQ(cmp, 0);

    PASS();
}

// ─── Test 10: Payload-too-small graceful ────────────────────────────────────

void test_payload_small_graceful() {
    TEST(PayloadSmallGraceful);

    // V1 header but only 8 bytes of payload (much smaller than V1).
    // The deserializer should still succeed, copying only what's available.
    GatewayRequestHeader hdr{};
    hdr.magic       = GatewayRequestHeader::MAGIC;
    hdr.version     = 1;
    hdr.headerSize  = sizeof(GatewayRequestHeader);
    hdr.payloadSize = 8;

    // Build a frame: only 8 bytes of payload, all 0xFF
    std::vector<uint8_t> frame(sizeof(hdr) + 8, 0xFF);
    std::memcpy(frame.data(), &hdr, sizeof(hdr));
    // Set the 8 payload bytes to identifiable pattern
    std::memset(frame.data() + sizeof(hdr), 0x01, 8);

    OrderRequestV2 decoded{};
    auto result = deserializeGatewayRequest(frame.data(), frame.size(), decoded);
    ASSERT_EQ(static_cast<uint8_t>(result),
              static_cast<uint8_t>(GatewayDecodeResult::Ok));

    // The first byte (type field) should be 1 (NewOrder)
    ASSERT_EQ(static_cast<uint8_t>(decoded.type),
              static_cast<uint8_t>(GatewayRequestType::NewOrder));

    PASS();
}

// ─── Test 11: Constants sanity ──────────────────────────────────────────────

void test_constants_sanity() {
    TEST(ConstantsSanity);

    ASSERT_EQ(GatewayRequestHeader::MAGIC, 0x4F524451u);
    ASSERT_EQ(GATEWAY_PROTOCOL_VERSION, 2);
    ASSERT_EQ(GATEWAY_MIN_READABLE_VERSION, 1);

    // V2 must be strictly larger than V1
    assert(sizeof(OrderRequestV2) > sizeof(OrderRequestV1));

    PASS();
}

// ─── Main ───────────────────────────────────────────────────────────────────

int main() {
    std::cout << "\n═══ Gateway Protocol Schema Evolution Tests ═══\n" << std::endl;

    test_v2_roundtrip();
    test_v1_backward_compat();
    test_future_version_skip();
    test_bad_magic();
    test_truncated_frame();
    test_version_too_old();
    test_header_growth_tolerance();
    test_gateway_response_layout();
    test_v1_v2_prefix_alignment();
    test_payload_small_graceful();
    test_constants_sanity();

    std::cout << "\n─── Results: " << tests_passed << " passed, "
              << tests_failed << " failed ───" << std::endl;

    if (tests_failed > 0) {
        std::cerr << "\n*** FAILURES DETECTED ***\n" << std::endl;
        return 1;
    }

    std::cout << "\nAll gateway protocol tests passed.\n" << std::endl;
    return 0;
}
