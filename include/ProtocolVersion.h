#pragma once

// ProtocolVersion — centralized, header-only declaration of the wire-
// protocol VERSIONING CONTRACT across every order-entry / market-data
// codec the engine speaks (FIX, OUCH, ITCH, SBE).
//
// PURPOSE
//   The individual protocol headers (FIXParser.h, OuchProtocol.h,
//   ItchProtocol.h, SbeProtocol.h) each encode their own version
//   handling inline. That works, but the *rules* — which versions are
//   accepted, what "compatible" means for each format — are scattered
//   and only implicitly asserted. This header collects the contract in
//   one place so a regression test (tests/ProtocolVersioningTest.cpp)
//   can pin it down: a future change that silently broadens an accept-
//   list, removes a supported version, or breaks SBE's append-only
//   forward-compat rule will then fail a test instead of shipping.
//
// SCOPE / NON-GOALS
//   This is documentation-as-code plus a couple of helper predicates.
//   It is deliberately NOT wired into the live parsers — FixSession
//   still owns its own runtime accept-set (settable per-session via
//   setAcceptedVersions), OuchSession still frames off
//   ouchInboundFrameSize, etc. Centralizing those call sites onto these
//   constants is a deliberate follow-up; doing it here would change
//   parser behavior, which the contract test must not do.
//
//   The constants below therefore describe the *default / intended*
//   contract. Where a rule is enforced in code today the test asserts
//   it directly against the parser; where a rule is only intended, the
//   test asserts what the code actually does and references the
//   constant here as the documented target.

#include <string_view>

namespace OrderMatcher::protocol_version {

// ─── FIX ────────────────────────────────────────────────────────────────────
// FIX carries its version in BeginString (tag 8). The session enforces
// an accept-list BEFORE any engine dispatch (FixSession::dispatch):
// a BeginString outside the set is rejected with
// RejectReason::UnsupportedFixVersion and never reaches the engine.
//
// ENFORCED (FixSession): default-accepted set is exactly {FIX.4.2};
//   FIX.4.4 is opt-in via setAcceptedVersions. parseFixVersion maps
//   "FIX.4.4" → FIX_4_4 and every other accepted string → FIX_4_2.
constexpr std::string_view kFixBeginString_4_2 = "FIX.4.2";
constexpr std::string_view kFixBeginString_4_4 = "FIX.4.4";

// The two versions the codec layer (FixSerializer / parseFixVersion)
// actually knows how to serialize differently. Anything else, even if
// a session were configured to accept its BeginString, would fall
// through parseFixVersion to the 4.2 wire semantics.
inline bool isKnownFixVersion(std::string_view beginString) {
    return beginString == kFixBeginString_4_2 ||
           beginString == kFixBeginString_4_4;
}

// The session's out-of-the-box accept-list. A brand-new FixSession
// rejects everything except this until setAcceptedVersions widens it.
// (Mirrors FixSession's constructor default; the test asserts the two
// stay in agreement so a change to one without the other is caught.)
inline bool isDefaultAcceptedFixVersion(std::string_view beginString) {
    return beginString == kFixBeginString_4_2;
}

// ─── SBE ──────────────────────────────────────────────────────────────────--
// SBE versioning is forward-compatible BY CONSTRUCTION: every message
// carries an 8-byte MessageHeader (blockLength, templateId, schemaId,
// version). New schema versions append fields at the end of the fixed
// block and bump `version`; `schemaId` is the hard ABI boundary.
//
// CONTRACT (enforced by SbeProtocol.h's encode/decode + the test):
//   * A reader at version N decodes a message of version > N by reading
//     the prefix it understands and skipping the trailing bytes it does
//     not (using blockLength). The leading bytes of vN+1 are a byte-
//     exact copy of vN's layout.
//   * schemaId mismatch is NOT decodable — different schema == hard break.
constexpr uint16_t kSbeSchemaId             = 100;  // SBE_SCHEMA_ID
constexpr uint16_t kSbeNewOrderTemplateId   = 1;    // SBE_TEMPLATE_NEW_ORDER
constexpr uint16_t kSbeNewOrderV1Version    = 1;
constexpr uint16_t kSbeNewOrderV2Version    = 2;

// ─── OUCH ─────────────────────────────────────────────────────────────────--
// OUCH (like ITCH) has NO version field on the wire. One message type
// byte == one fixed layout, forever. "Versioning" means republishing a
// whole new spec and rebuilding every counterparty.
//
// ENFORCED (OuchSession + decoders): the message-type byte selects a
//   fixed frame length via ouchInboundFrameSize(); an unknown type
//   returns 0 and the session drops the connection. Each decoder also
//   re-checks both the type byte and the exact length, so a frame of
//   the wrong size for its type is rejected.
//
// ─── ITCH ─────────────────────────────────────────────────────────────────--
// Same paradigm as OUCH: fixed-width, type-byte-keyed, no wire version.
// The codec in this repo is encode-only (publisher side), so the
// "enforced" surface is the stability of the type-code → size mapping.
// Any change to a type code or a message size is a wire-breaking change
// that every subscriber must be rebuilt for — the test pins the
// constants so such a change is loud.
//
// DOCUMENTED-ONLY (not enforced in this repo's ITCH code): there is no
//   inbound ITCH decoder that would *reject* an unknown type code on
//   the subscriber side, because subscribers are out of scope here.
//   A production subscriber MUST skip/flag unknown 'message-type' bytes
//   rather than misparse; that rule lives in the consuming app.

}  // namespace OrderMatcher::protocol_version
