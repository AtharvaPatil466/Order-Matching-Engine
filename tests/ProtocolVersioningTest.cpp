// ProtocolVersioningTest — codifies the wire-protocol VERSIONING CONTRACT
// across the four codecs the engine speaks (FIX, SBE, ITCH, OUCH) as
// executable assertions. The roadmap calls out that "SBE forward-compat
// [is] proven by test ... FIX/OUCH/ITCH versioning not yet codified as a
// contract"; this file closes that gap.
//
// For each protocol the comments mark whether a rule is ENFORCED (the
// running parser/codec rejects or transforms on its own) or
// DOCUMENTED-ONLY (the intended rule is asserted against what the code
// actually does today, with the gap noted). Centralized constants /
// accept-list predicates live in the new header-only ProtocolVersion.h
// and are cross-checked here against the live parsers.
//
// This test uses ONLY existing public parser/codec APIs — it does not
// modify any parser behavior.
//
// Coverage:
//   FIX (ENFORCED by FixSession)
//     1.  Unsupported BeginString (FIX.4.0) → UnsupportedFixVersion reject,
//         no engine dispatch.
//     2.  Default-accepted FIX.4.2 NewOrder parses and reaches the engine.
//     3.  FIX.4.4 is opt-in: rejected until setAcceptedVersions widens the
//         set, then accepted.
//     4.  parseFixVersion contract + ProtocolVersion.h accept-list
//         predicates agree with FixSession's defaults.
//     5.  fixToOrderParams version-independence (pure parse layer).
//
//   SBE (ENFORCED by SbeProtocol.h)
//     6.  v1 reader reads a v2 message's common prefix UNCHANGED (the
//         headline forward-compat property, asserted directly).
//     7.  v2 reader reads a v1 message, filling documented defaults.
//     8.  schemaId is the hard ABI boundary; a foreign schemaId is
//         detectable from the header (documented + asserted).
//
//   ITCH (DOCUMENTED-ONLY: encode-only codec, no wire version)
//     9.  Type-code → fixed-size mapping is stable (wire-breaking if
//         changed); distinct type codes.
//
//   OUCH (ENFORCED by OuchProtocol decoders / OuchSession framing)
//    10.  ouchInboundFrameSize: known types → exact size, unknown → 0
//         (the session drops the connection on 0).
//    11.  Decoders reject a wrong-type or wrong-length frame.

#include "FIXParser.h"
#include "FixSession.h"
#include "ItchProtocol.h"
#include "MatchingEngine.h"
#include "OuchProtocol.h"
#include "ProtocolVersion.h"
#include "SbeProtocol.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

using namespace OrderMatcher;
namespace pv = OrderMatcher::protocol_version;

static int passed = 0;
#define TEST(name) std::cout << "  " << #name << "..." << std::flush
#define PASS() do { ++passed; std::cout << " PASS\n"; } while (0)

// ─── FIX message builder ─────────────────────────────────────────────────────
// Builds a well-formed FIX NewOrderSingle (35=D) with a valid BodyLength
// and checksum for an arbitrary BeginString, so the FixFramer accepts the
// frame and the version check is exercised on its merits (not rejected for
// being malformed). Mirrors makeNewOrder() in FixTcpGatewayTest.
static std::string makeNewOrder(std::string_view beginString, OrderId clOrdId,
                                SymbolId sym, Quantity qty, Price price,
                                bool withTransactTime = false) {
    std::string body;
    body += "35=D"; body += FIX_SOH;
    body += "49=100"; body += FIX_SOH;                       // SenderCompID
    body += "11=" + std::to_string(clOrdId); body += FIX_SOH;
    body += "55=" + std::to_string(sym);     body += FIX_SOH;
    body += "54=1"; body += FIX_SOH;                         // Buy
    body += "38=" + std::to_string(qty);     body += FIX_SOH;
    body += "44=" + std::to_string(price);   body += FIX_SOH;
    body += "40=2"; body += FIX_SOH;                         // Limit
    body += "59=1"; body += FIX_SOH;                         // GTC
    if (withTransactTime) {
        body += "60=20240101-00:00:00.000"; body += FIX_SOH; // required on 4.4
    }
    std::string head = "8=" + std::string(beginString);
    head += FIX_SOH;
    head += "9=" + std::to_string(body.size());
    head += FIX_SOH;
    std::string pre = head + body;
    uint32_t sum = 0;
    for (char c : pre) sum += static_cast<uint8_t>(c);
    char cs[4];
    std::snprintf(cs, sizeof(cs), "%03d", sum % 256);
    return pre + "10=" + cs + FIX_SOH;
}

// Parse the most recent captured FIX response.
static FixMessage parseResp(const std::string& raw) {
    FixMessage m;
    assert(m.parse(raw.data(), raw.size()) && "response failed to parse");
    return m;
}

// ─── FIX: ENFORCED contract via FixSession ───────────────────────────────────

void test_fix_unsupported_begin_string_rejected() {
    TEST(FixUnsupportedBeginStringRejected);

    constexpr SymbolId kSym = 7;
    MatchingEngine engine;
    engine.addSymbol(kSym);
    engine.start();

    std::vector<std::string> sent;
    FixSession session(engine, [&](std::string_view bytes) {
        sent.emplace_back(bytes);
    });
    // Default accept-list is {FIX.4.2}. FIX.4.0 is not in it.

    auto frame = makeNewOrder("FIX.4.0", /*clOrdId=*/1, kSym, /*qty=*/10,
                              /*price=*/1000);
    assert(session.feed(frame.data(), frame.size()));

    // The order must have been rejected BEFORE the engine saw it.
    assert(session.ordersRejected() == 1);
    assert(session.ordersAccepted() == 0);
    auto* book = engine.getOrderBook(kSym);
    assert(book && book->getOrder(1) == nullptr &&
           "unsupported-version order must NOT reach the book");

    // The wire reject must carry ExecType=8 and OrdRejReason=99 (the
    // FIX standard "Other" code FIXParser maps UnsupportedFixVersion to).
    assert(sent.size() == 1);
    auto resp = parseResp(sent.back());
    assert(resp.getString(FixTag::MsgType) == "8");
    assert(resp.getChar(FixTag::ExecType) == '8');
    assert(resp.getInt(FixTag::OrdRejReason) ==
           ordRejReasonCode(RejectReason::UnsupportedFixVersion));
    assert(ordRejReasonCode(RejectReason::UnsupportedFixVersion) == 99);

    PASS();
}

void test_fix_supported_version_parses() {
    TEST(FixSupportedVersionParses);

    constexpr SymbolId kSym = 7;
    MatchingEngine engine;
    engine.addSymbol(kSym);
    engine.start();

    std::vector<std::string> sent;
    FixSession session(engine, [&](std::string_view bytes) {
        sent.emplace_back(bytes);
    });

    // FIX.4.2 is the default-accepted version → order reaches the engine.
    auto frame = makeNewOrder("FIX.4.2", /*clOrdId=*/42, kSym, /*qty=*/25,
                              /*price=*/1000);
    assert(session.feed(frame.data(), frame.size()));

    assert(session.ordersAccepted() == 1);
    assert(session.ordersRejected() == 0);
    auto* book = engine.getOrderBook(kSym);
    assert(book && book->getOrder(42) != nullptr &&
           "supported-version order must reach the book");

    // The ack echoes back under the accepted BeginString.
    auto resp = parseResp(sent.back());
    assert(resp.getString(FixTag::BeginString) == "FIX.4.2");
    assert(resp.getChar(FixTag::ExecType) == '0');  // New

    PASS();
}

void test_fix_4_4_is_opt_in() {
    TEST(Fix44IsOptIn);

    constexpr SymbolId kSym = 7;

    // (a) A fresh session rejects FIX.4.4 — it is NOT in the default set.
    {
        MatchingEngine engine;
        engine.addSymbol(kSym);
        engine.start();
        std::vector<std::string> sent;
        FixSession session(engine, [&](std::string_view b) { sent.emplace_back(b); });

        auto frame = makeNewOrder("FIX.4.4", /*clOrdId=*/100, kSym, /*qty=*/10,
                                  /*price=*/1000, /*withTransactTime=*/true);
        assert(session.feed(frame.data(), frame.size()));
        assert(session.ordersRejected() == 1 && session.ordersAccepted() == 0);
        auto* book = engine.getOrderBook(kSym);
        assert(book && book->getOrder(100) == nullptr);
        auto resp = parseResp(sent.back());
        assert(resp.getInt(FixTag::OrdRejReason) ==
               ordRejReasonCode(RejectReason::UnsupportedFixVersion));
    }

    // (b) After opting in via setAcceptedVersions, the SAME frame parses.
    {
        MatchingEngine engine;
        engine.addSymbol(kSym);
        engine.start();
        std::vector<std::string> sent;
        FixSession session(engine, [&](std::string_view b) { sent.emplace_back(b); });
        session.setAcceptedVersions({"FIX.4.2", "FIX.4.4"});

        auto frame = makeNewOrder("FIX.4.4", /*clOrdId=*/101, kSym, /*qty=*/10,
                                  /*price=*/1000, /*withTransactTime=*/true);
        assert(session.feed(frame.data(), frame.size()));
        assert(session.ordersAccepted() == 1 && session.ordersRejected() == 0);
        auto* book = engine.getOrderBook(kSym);
        assert(book && book->getOrder(101) != nullptr);
        // 4.4 ack echoes the 4.4 BeginString.
        auto resp = parseResp(sent.back());
        assert(resp.getString(FixTag::BeginString) == "FIX.4.4");
    }

    PASS();
}

void test_fix_4_4_missing_transact_time_rejected() {
    TEST(Fix44MissingTransactTimeRejected);

    // ENFORCED: FIX 4.4 requires TransactTime (60) on D/F/G. A 4.4
    // NewOrder lacking it is rejected with MissingRequiredField even
    // though the version itself is accepted. (4.2 leaves it optional —
    // covered implicitly by the 4.2 tests above, which omit tag 60.)
    constexpr SymbolId kSym = 7;
    MatchingEngine engine;
    engine.addSymbol(kSym);
    engine.start();
    std::vector<std::string> sent;
    FixSession session(engine, [&](std::string_view b) { sent.emplace_back(b); });
    session.setAcceptedVersions({"FIX.4.2", "FIX.4.4"});

    auto frame = makeNewOrder("FIX.4.4", /*clOrdId=*/200, kSym, /*qty=*/10,
                              /*price=*/1000, /*withTransactTime=*/false);
    assert(session.feed(frame.data(), frame.size()));
    assert(session.ordersRejected() == 1 && session.ordersAccepted() == 0);
    auto resp = parseResp(sent.back());
    assert(resp.getInt(FixTag::OrdRejReason) ==
           ordRejReasonCode(RejectReason::MissingRequiredField));

    PASS();
}

void test_fix_parse_version_and_accept_lists_agree() {
    TEST(FixParseVersionAndAcceptListsAgree);

    // parseFixVersion contract (the codec-layer mapping):
    assert(parseFixVersion("FIX.4.4") == FixVersion::FIX_4_4);
    assert(parseFixVersion("FIX.4.2") == FixVersion::FIX_4_2);
    // Anything the session has opted into but isn't 4.4 falls back to 4.2.
    assert(parseFixVersion("FIX.4.0") == FixVersion::FIX_4_2);
    assert(parseFixVersion("") == FixVersion::FIX_4_2);

    // ProtocolVersion.h centralized predicates must agree with the live
    // FixSession default accept-set. Cross-check the header against a
    // freshly constructed session so the two cannot silently diverge.
    MatchingEngine engine;
    FixSession session(engine, [](std::string_view) {});
    const std::unordered_set<std::string>& accepted = session.acceptedVersions();

    assert(accepted.count("FIX.4.2") == 1 && "FIX.4.2 must be default-accepted");
    assert(accepted.count("FIX.4.4") == 0 && "FIX.4.4 must be opt-in");
    assert(accepted.size() == 1 && "default accept-list is exactly {FIX.4.2}");

    // The header predicate mirrors that exact default.
    assert(pv::isDefaultAcceptedFixVersion("FIX.4.2"));
    assert(!pv::isDefaultAcceptedFixVersion("FIX.4.4"));
    assert(!pv::isDefaultAcceptedFixVersion("FIX.4.0"));
    for (const std::string& v : accepted) {
        assert(pv::isDefaultAcceptedFixVersion(v) &&
               "header default predicate must accept every live default");
    }

    // The set of versions the codec can serialize distinctly.
    assert(pv::isKnownFixVersion("FIX.4.2"));
    assert(pv::isKnownFixVersion("FIX.4.4"));
    assert(!pv::isKnownFixVersion("FIX.4.0"));
    assert(!pv::isKnownFixVersion("FIX.5.0"));

    PASS();
}

void test_fix_to_order_params_is_version_independent() {
    TEST(FixToOrderParamsIsVersionIndependent);

    // The pure parse layer (fixToOrderParams) keys off MsgType, not
    // version — by design the version gate lives in the session above
    // it. Confirm a D message decodes identically regardless of the
    // BeginString carried, so the ONLY thing standing between a wrong-
    // version client and the engine is FixSession's accept-list check.
    auto check = [](std::string_view beginString) {
        auto raw = makeNewOrder(beginString, /*clOrdId=*/5, /*sym=*/3,
                                /*qty=*/77, /*price=*/1234);
        FixMessage m;
        assert(m.parse(raw.data(), raw.size()));
        auto p = fixToOrderParams(m);
        assert(p.valid);
        assert(p.action == FixOrderParams::Action::NewOrder);
        assert(p.orderId == 5);
        assert(p.symbolId == 3);
        assert(p.qty == 77);
        assert(p.price == 1234);
    };
    check("FIX.4.2");
    check("FIX.4.4");
    check("FIX.4.0");  // would parse here; the session is what rejects it

    PASS();
}

// ─── SBE: ENFORCED forward-compat contract ───────────────────────────────────

void test_sbe_v1_reader_reads_v2_prefix_unchanged() {
    TEST(SbeV1ReaderReadsV2PrefixUnchanged);

    // The roadmap's headline property, asserted from the v1 reader's
    // point of view: a v2 message (with appended fields) is encoded on
    // the wire, and an UNMODIFIED v1 reader decodes the v1 fields from
    // it byte-for-byte, oblivious to the trailing v2-only bytes.
    SbeNewOrderV2 v2{};
    v2.orderId       = 0xDEADBEEFULL;
    v2.price         = -98765;          // sign must survive
    v2.quantity      = 4242;
    v2.side          = 2;               // Sell
    v2.orderType     = 3;               // IOC
    v2.participantId = 0xABCD;          // v2-only
    v2.timeInForce   = 1;               // v2-only (DAY)

    uint8_t buf[SBE_MESSAGE_HEADER_BYTES + SBE_NEW_ORDER_V2_BLOCK_BYTES];
    size_t n = encodeSbeNewOrderV2(buf, v2);
    assert(n == sizeof(buf));

    // Header advertises the v2 schema version + the longer block.
    auto h = readSbeMessageHeader(buf);
    assert(h.schemaId == SBE_SCHEMA_ID);
    assert(h.schemaId == pv::kSbeSchemaId);
    assert(h.templateId == SBE_TEMPLATE_NEW_ORDER);
    assert(h.version == SBE_NEW_ORDER_V2);
    assert(h.version == pv::kSbeNewOrderV2Version);
    assert(h.blockLength == SBE_NEW_ORDER_V2_BLOCK_BYTES);

    // A v1 reader — knowing ONLY the v1 layout — reads the v2 buffer's
    // block. It must recover every v1 field exactly. It reads only the
    // first SBE_NEW_ORDER_V1_BLOCK_BYTES and ignores the rest, which is
    // safe because blockLength tells it the real block is longer.
    assert(h.blockLength >= SBE_NEW_ORDER_V1_BLOCK_BYTES);
    SbeNewOrderV1 v1out{};
    readSbeNewOrderV1Block(buf + SBE_MESSAGE_HEADER_BYTES, v1out);
    assert(v1out.orderId   == 0xDEADBEEFULL);
    assert(v1out.price     == -98765);
    assert(v1out.quantity  == 4242);
    assert(v1out.side      == 2);
    assert(v1out.orderType == 3);

    // And byte-exact prefix: encoding the same v1 fields as a pure v1
    // message yields a block whose bytes equal the v2 message's first
    // v1-block bytes. This is the structural guarantee underpinning the
    // decode above.
    SbeNewOrderV1 v1{};
    static_cast<SbeNewOrderV1&>(v1) = static_cast<const SbeNewOrderV1&>(v2);
    uint8_t bufV1[SBE_MESSAGE_HEADER_BYTES + SBE_NEW_ORDER_V1_BLOCK_BYTES];
    encodeSbeNewOrderV1(bufV1, v1);
    assert(std::memcmp(bufV1 + SBE_MESSAGE_HEADER_BYTES,
                       buf + SBE_MESSAGE_HEADER_BYTES,
                       SBE_NEW_ORDER_V1_BLOCK_BYTES) == 0 &&
           "v1 block must be a byte-exact prefix of the v2 block");

    PASS();
}

void test_sbe_v2_reader_reads_v1_with_defaults() {
    TEST(SbeV2ReaderReadsV1WithDefaults);

    // The reverse direction (backward read): a v2 reader decodes a v1
    // message and fills the v2-only fields with their documented
    // "absent" defaults, driven by the header's (shorter) blockLength.
    SbeNewOrderV1 v1{};
    v1.orderId = 11; v1.price = 555; v1.quantity = 9;
    v1.side = 1; v1.orderType = 1;

    uint8_t buf[SBE_MESSAGE_HEADER_BYTES + SBE_NEW_ORDER_V1_BLOCK_BYTES];
    encodeSbeNewOrderV1(buf, v1);
    auto h = readSbeMessageHeader(buf);
    assert(h.version == SBE_NEW_ORDER_V1);
    assert(h.version == pv::kSbeNewOrderV1Version);
    assert(h.blockLength == SBE_NEW_ORDER_V1_BLOCK_BYTES);

    SbeNewOrderV2 v2out{};
    assert(readSbeNewOrderV2Block(buf + SBE_MESSAGE_HEADER_BYTES,
                                  h.blockLength, v2out));
    assert(v2out.orderId == 11 && v2out.price == 555 && v2out.quantity == 9);
    assert(v2out.participantId == 0 && "absent v2 field defaults to 0");
    assert(v2out.timeInForce == 0 && "absent v2 field defaults to GTC");

    // A block shorter than v1's minimum is undecodable.
    uint8_t tooShort[SBE_NEW_ORDER_V1_BLOCK_BYTES - 1] = {0};
    SbeNewOrderV2 dummy{};
    assert(!readSbeNewOrderV2Block(
               tooShort, SBE_NEW_ORDER_V1_BLOCK_BYTES - 1, dummy) &&
           "below v1 min block size must fail to decode");

    PASS();
}

void test_sbe_schema_id_is_abi_boundary() {
    TEST(SbeSchemaIdIsAbiBoundary);

    // DOCUMENTED + ASSERTED: schemaId is the hard ABI boundary. SBE
    // forward-compat applies only WITHIN a schemaId; a different schema
    // is a non-decodable break. The codec does not embed a foreign-
    // schema guard (a real reader checks schemaId before dispatching to
    // a codec), so the contract here is that the header surfaces the
    // schemaId so the reader CAN reject a mismatch.
    SbeNewOrderV1 v1{};
    v1.orderId = 1; v1.price = 1; v1.quantity = 1; v1.side = 1; v1.orderType = 1;
    uint8_t buf[SBE_MESSAGE_HEADER_BYTES + SBE_NEW_ORDER_V1_BLOCK_BYTES];
    encodeSbeNewOrderV1(buf, v1);

    auto h = readSbeMessageHeader(buf);
    assert(h.schemaId == pv::kSbeSchemaId);

    // Simulate a reader that only accepts its own schema: a foreign
    // schemaId in the header is detectable and must be treated as
    // undecodable rather than reinterpreted under the local schema.
    const uint16_t kForeignSchema = pv::kSbeSchemaId + 1;
    SbeMessageHeader foreign = h;
    foreign.schemaId = kForeignSchema;
    uint8_t fbuf[SBE_MESSAGE_HEADER_BYTES];
    writeSbeMessageHeader(fbuf, foreign);
    auto fh = readSbeMessageHeader(fbuf);
    assert(fh.schemaId != pv::kSbeSchemaId &&
           "foreign schemaId must be distinguishable from the local schema");

    PASS();
}

// ─── ITCH: DOCUMENTED-ONLY (no wire version; type-code stability) ─────────────

void test_itch_type_code_size_mapping_stable() {
    TEST(ItchTypeCodeSizeMappingStable);

    // ITCH has NO version field on the wire — like OUCH, a message-type
    // byte maps to one fixed layout forever, and any change is a wire-
    // breaking republish. There is no inbound decoder in this repo to
    // reject an unknown type (subscribers are out of scope), so the
    // enforceable contract is the STABILITY of the type-code → size
    // mapping and the distinctness of the codes. Pin them here so a
    // change to either fails loudly.
    struct Row { uint8_t type; size_t size; };
    const Row rows[] = {
        {ITCH_MT_SYSTEM_EVENT,      ITCH_SIZE_SYSTEM_EVENT},
        {ITCH_MT_STOCK_DIRECTORY,   ITCH_SIZE_STOCK_DIRECTORY},
        {ITCH_MT_TRADING_ACTION,    ITCH_SIZE_TRADING_ACTION},
        {ITCH_MT_ADD_ORDER,         ITCH_SIZE_ADD_ORDER},
        {ITCH_MT_ORDER_EXECUTED,    ITCH_SIZE_ORDER_EXECUTED},
        {ITCH_MT_ORDER_EXECUTED_PX, ITCH_SIZE_ORDER_EXECUTED_PX},
        {ITCH_MT_ORDER_CANCEL,      ITCH_SIZE_ORDER_CANCEL},
        {ITCH_MT_ORDER_DELETE,      ITCH_SIZE_ORDER_DELETE},
        {ITCH_MT_TRADE,             ITCH_SIZE_TRADE},
        {ITCH_MT_CROSS_TRADE,       ITCH_SIZE_CROSS_TRADE},
    };
    constexpr size_t kRows = sizeof(rows) / sizeof(rows[0]);

    // Pin the ITCH 5.0 type-code letters and sizes (changing any one is
    // a backward-incompatible feed change).
    assert(ITCH_MT_SYSTEM_EVENT == 'S' && ITCH_SIZE_SYSTEM_EVENT == 12);
    assert(ITCH_MT_ADD_ORDER == 'A' && ITCH_SIZE_ADD_ORDER == 36);
    assert(ITCH_MT_ORDER_EXECUTED == 'E' && ITCH_SIZE_ORDER_EXECUTED == 31);
    assert(ITCH_MT_ORDER_EXECUTED_PX == 'C' && ITCH_SIZE_ORDER_EXECUTED_PX == 36);
    assert(ITCH_MT_ORDER_CANCEL == 'X' && ITCH_SIZE_ORDER_CANCEL == 23);
    assert(ITCH_MT_ORDER_DELETE == 'D' && ITCH_SIZE_ORDER_DELETE == 19);
    assert(ITCH_MT_TRADE == 'P' && ITCH_SIZE_TRADE == 44);

    // Every type code is distinct (no two messages share a wire tag).
    for (size_t i = 0; i < kRows; ++i) {
        for (size_t j = i + 1; j < kRows; ++j) {
            assert(rows[i].type != rows[j].type &&
                   "ITCH message-type codes must be unique on the wire");
        }
        assert(rows[i].size > 0);
    }

    // An encoder writes exactly its declared size and stamps the type
    // byte — confirm for a representative message so "the size constant
    // IS the wire size" is asserted, not just declared.
    uint8_t out[64];
    size_t w = encodeAddOrder(out, /*stockLocate=*/1, /*trackingNumber=*/0,
                              /*timestampNs=*/123, /*orderRefNumber=*/9,
                              Side::Buy, /*shares=*/100, /*stock=*/5,
                              /*price=*/1000);
    assert(w == ITCH_SIZE_ADD_ORDER);
    assert(out[0] == ITCH_MT_ADD_ORDER);

    PASS();
}

// ─── OUCH: ENFORCED (type-byte framing; no wire version) ──────────────────────

void test_ouch_frame_size_known_vs_unknown() {
    TEST(OuchFrameSizeKnownVsUnknown);

    // ENFORCED: OUCH has no wire version. The inbound message-type byte
    // selects a fixed frame length; ouchInboundFrameSize returns 0 for
    // any type the codec does not implement, and OuchSession::feed drops
    // the connection on a 0 (unrecognized type at a frame boundary).
    assert(ouchInboundFrameSize(OUCH_MT_ENTER_ORDER)   == OUCH_SIZE_ENTER_ORDER);
    assert(ouchInboundFrameSize(OUCH_MT_CANCEL_ORDER)  == OUCH_SIZE_CANCEL_ORDER);
    assert(ouchInboundFrameSize(OUCH_MT_REPLACE_ORDER) == OUCH_SIZE_REPLACE_ORDER);

    // Unknown / outbound-only types are not framable inbound → 0.
    assert(ouchInboundFrameSize('Z') == 0 && "unknown type → 0 (session drops)");
    assert(ouchInboundFrameSize(0)   == 0);
    assert(ouchInboundFrameSize(OUCH_MT_ORDER_ACCEPTED) == 0 &&
           "outbound-only 'A' is not a valid inbound frame");
    assert(ouchInboundFrameSize(OUCH_MT_ORDER_EXECUTED) == 0);

    PASS();
}

void test_ouch_decoders_reject_wrong_type_or_length() {
    TEST(OuchDecodersRejectWrongTypeOrLength);

    // ENFORCED: each decoder re-checks both the message-type byte and
    // the exact frame length, so a frame of the wrong size for its type
    // (or the wrong type byte for its decoder) is rejected — defense in
    // depth behind the framing layer.

    // A well-formed EnterOrder decodes; corrupting the type byte or
    // length makes the same bytes undecodable.
    uint8_t enter[OUCH_SIZE_ENTER_ORDER];
    std::memset(enter, ' ', sizeof(enter));
    enter[0] = OUCH_MT_ENTER_ORDER;
    // OrderToken (14B ASCII decimal) at offset 1 — must be all-digits.
    std::memset(enter + 1, '0', 14);
    enter[14] = '1';                 // token = "00000000000001"
    enter[15] = 'B';                 // side = Buy
    writeU32BE(enter + 16, 100);     // shares
    std::memset(enter + 20, '0', 8);
    enter[27] = '5';                 // stock = "00000005"
    writeU32BE(enter + 28, 1000);    // price
    writeU32BE(enter + 32, OUCH_TIF_DAY);
    std::memset(enter + 36, '0', 4);
    enter[39] = '1';                 // firm = "0001"
    enter[40] = 'Y';                 // display
    enter[41] = 'O';                 // capacity
    enter[42] = 'N';                 // intermarket sweep
    writeU32BE(enter + 43, 0);       // min qty
    enter[47] = 'N';                 // cross type
    enter[48] = 'R';                 // customer type

    OuchEnterOrder eo{};
    assert(decodeEnterOrder(enter, sizeof(enter), eo) &&
           "well-formed EnterOrder must decode");
    assert(eo.orderToken == 1 && eo.side == Side::Buy && eo.shares == 100);

    // Wrong length for the type → reject (no version field to negotiate
    // a different size; the size IS the contract).
    assert(!decodeEnterOrder(enter, sizeof(enter) - 1, eo) &&
           "EnterOrder of wrong length must be rejected");
    assert(!decodeEnterOrder(enter, OUCH_SIZE_CANCEL_ORDER, eo));

    // Wrong type byte for this decoder → reject.
    uint8_t wrongType[OUCH_SIZE_ENTER_ORDER];
    std::memcpy(wrongType, enter, sizeof(enter));
    wrongType[0] = OUCH_MT_CANCEL_ORDER;
    assert(!decodeEnterOrder(wrongType, sizeof(wrongType), eo) &&
           "EnterOrder decoder must reject a non-'O' type byte");

    // Cross-decoder confusion: feeding an EnterOrder-sized buffer to the
    // CancelOrder decoder fails on the length check.
    OuchCancelOrder co{};
    assert(!decodeCancelOrder(enter, sizeof(enter), co) &&
           "CancelOrder decoder must reject an EnterOrder-sized frame");

    PASS();
}

int main() {
    std::cout << "Running ProtocolVersioningTest\n";

    // FIX
    test_fix_unsupported_begin_string_rejected();
    test_fix_supported_version_parses();
    test_fix_4_4_is_opt_in();
    test_fix_4_4_missing_transact_time_rejected();
    test_fix_parse_version_and_accept_lists_agree();
    test_fix_to_order_params_is_version_independent();

    // SBE
    test_sbe_v1_reader_reads_v2_prefix_unchanged();
    test_sbe_v2_reader_reads_v1_with_defaults();
    test_sbe_schema_id_is_abi_boundary();

    // ITCH
    test_itch_type_code_size_mapping_stable();

    // OUCH
    test_ouch_frame_size_known_vs_unknown();
    test_ouch_decoders_reject_wrong_type_or_length();

    std::cout << "\n" << passed << " sections passed\n";
    return 0;
}
