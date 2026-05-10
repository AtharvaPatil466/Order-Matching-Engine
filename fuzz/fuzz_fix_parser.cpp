// Fuzz target for the FIX 4.2 parser, checksum validator, and FixOrderParams converter.
//
// Build with libFuzzer: cmake -DBUILD_FUZZERS=ON -DCMAKE_CXX_COMPILER=clang++ ..
// Replay a corpus without libFuzzer: ./fuzz_fix_parser_replay corpus/*

#include "FIXParser.h"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Cap input size; real FIX messages over a gateway are bounded by frame length.
    // The gateway should reject oversize frames before reaching the parser, so the
    // fuzzer only needs to explore the bounded regime.
    if (size > 64 * 1024) return 0;

    OrderMatcher::FixMessage msg;
    bool parsed = msg.parse(reinterpret_cast<const char*>(data), size);

    if (!parsed) return 0;

    // Exercise all read accessors on every tag the parser may have populated.
    // Some accessors (getInt) currently do no digit validation — that is intentionally
    // exposed to the fuzzer so UBSan/ASan can flag misbehavior on adversarial input.
    for (int tag = 0; tag < 1024; ++tag) {
        if (!msg.hasTag(tag)) continue;
        (void)msg.getString(tag);
        (void)msg.getInt(tag);
        (void)msg.getChar(tag);
    }

    (void)msg.validateChecksum();

    // Drive the engine-facing converter; this dispatches on MsgType and pulls
    // a wider set of tags through getInt/getChar.
    auto params = OrderMatcher::fixToOrderParams(msg);
    (void)params;

    return 0;
}
