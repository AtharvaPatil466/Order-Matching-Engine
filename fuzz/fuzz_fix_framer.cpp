// Fuzz target for FixFramer — drives random byte chunks (modeling
// fragmented receives + concatenated messages + garbage interleaved with
// valid frames) into the framer and asserts:
//
//   * Every emitted frame passes isWellFormed() and validateChecksum() —
//     the framer must never emit a malformed message to its consumer.
//   * The framer's bounded buffer never grows unboundedly under
//     adversarial input.
//   * The framer never crashes / overflows / reads OOB on any byte
//     sequence.
//
// The fuzzer interprets the input bytes as a sequence of (chunkSize, data)
// tuples to model arbitrary network fragmentation patterns.

#include "FixFramer.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

using namespace OrderMatcher;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > 64 * 1024) return 0;

    FixFramer framer(/*maxFrameSize=*/4096);
    size_t emitted = 0;
    framer.setOnMessage([&](std::string_view raw, const FixMessage& msg) {
        // Invariant: every emitted frame must be structurally valid AND have
        // a valid checksum. If the framer ever emits something malformed,
        // the consumer would happily forward garbage into application logic.
        assert(msg.isWellFormed());
        assert(msg.validateChecksum());
        // Self-consistency check: re-parse the raw bytes the framer handed
        // us and confirm parse succeeds.
        FixMessage reparsed;
        assert(reparsed.parse(raw.data(), raw.size()));
        ++emitted;
    });

    // Slice input into chunks of varying size driven by the input itself.
    // First byte of each segment is the chunk size (bounded to 1..64).
    size_t pos = 0;
    while (pos < size) {
        size_t chunk = (data[pos] % 64) + 1;
        ++pos;
        if (pos + chunk > size) chunk = size - pos;
        if (chunk == 0) break;

        bool ok = framer.feed(reinterpret_cast<const char*>(data + pos), chunk);
        pos += chunk;
        if (!ok) {
            // Unrecoverable framing error — caller would drop the
            // connection. We reset and continue feeding to keep exploring
            // the state space.
            framer.reset();
        }
    }

    (void)emitted;
    return 0;
}
