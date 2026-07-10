// ConfigValidationTest — P3-7 fail-fast startup config validation.
//
// Verifies validateStartupConfig() catches every declared-config invariant
// violation (instruments, participants, journal path) and accepts a valid
// config. Uses Config::loadMap so no files are needed except for the
// journal-path writability check.

#include "Config.h"
#include "ConfigValidator.h"

#include <cassert>
#include <iostream>
#include <string>

using namespace OrderMatcher;

static int passed = 0;
#define SECTION(name) std::cout << "  " << name << "..." << std::flush
#define PASS() do { ++passed; std::cout << " PASS\n"; } while (0)

// True iff any error mentions `needle`.
static bool hasError(const ConfigValidationResult& r, const std::string& needle) {
    for (const auto& e : r.errors)
        if (e.find(needle) != std::string::npos) return true;
    return false;
}

// Config is non-copyable (holds a mutex), so fill a caller-owned instance.
static void fillValidConfig(Config& cfg) {
    cfg.loadMap({
        {"instruments", "1"},
        {"instrument.1.min_price", "990000"},
        {"instrument.1.max_price", "1010000"},   // span 20000 < 200001
        {"instrument.1.tick_size", "100"},
        {"instrument.1.lot_size", "1"},
        {"participants", "10"},
        {"participant.10.position_limit", "5000000"},
        {"participant.10.otr_limit", "100"},
        {"journal_path", "/tmp/ob_cfgval_ok.wal"},
    });
}

void test_valid_config_passes() {
    SECTION("Valid config passes");
    Config cfg; fillValidConfig(cfg);
    auto r = validateStartupConfig(cfg);
    if (!r.ok) for (auto& e : r.errors) std::cout << "\n    unexpected: " << e;
    assert(r.ok);
    assert(r.errors.empty());
    PASS();
}

void test_empty_config_passes() {
    SECTION("Empty config (nothing declared) passes");
    Config cfg;  // no instruments/participants/journal
    auto r = validateStartupConfig(cfg);
    assert(r.ok);
    PASS();
}

void test_min_ge_max() {
    SECTION("min_price >= max_price rejected");
    Config cfg; fillValidConfig(cfg);
    cfg.set("instrument.1.min_price", "1010000");
    cfg.set("instrument.1.max_price", "1000000");
    auto r = validateStartupConfig(cfg);
    assert(!r.ok);
    assert(hasError(r, "min_price"));
    PASS();
}

void test_tick_not_positive() {
    SECTION("tick_size <= 0 rejected");
    Config cfg; fillValidConfig(cfg);
    cfg.set("instrument.1.tick_size", "0");
    auto r = validateStartupConfig(cfg);
    assert(!r.ok);
    assert(hasError(r, "tick_size"));
    PASS();
}

void test_lot_not_positive() {
    SECTION("lot_size <= 0 rejected");
    Config cfg; fillValidConfig(cfg);
    cfg.set("instrument.1.lot_size", "-5");
    auto r = validateStartupConfig(cfg);
    assert(!r.ok);
    assert(hasError(r, "lot_size"));
    PASS();
}

void test_price_range_exceeds_capacity() {
    SECTION("price range exceeding FlatPriceMap capacity rejected");
    Config cfg; fillValidConfig(cfg);
    // Override capacity to a tiny value so span 20000 overflows it.
    auto r = validateStartupConfig(cfg, /*priceMapCapacity=*/100);
    assert(!r.ok);
    assert(hasError(r, "capacity"));

    // And with the real default capacity, a genuinely oversized band fails too.
    Config big; fillValidConfig(big);
    big.set("instrument.1.min_price", "0");
    big.set("instrument.1.max_price", "500000");   // span 500000 >= 200001
    auto r2 = validateStartupConfig(big);
    assert(!r2.ok);
    assert(hasError(r2, "capacity"));
    PASS();
}

void test_instrument_missing_fields() {
    SECTION("Instrument with missing fields rejected");
    Config cfg;
    cfg.loadMap({
        {"instruments", "7"},
        {"instrument.7.min_price", "1000"},
        // max_price, tick_size, lot_size all missing
    });
    auto r = validateStartupConfig(cfg);
    assert(!r.ok);
    assert(hasError(r, "max_price"));
    assert(hasError(r, "tick_size"));
    assert(hasError(r, "lot_size"));
    PASS();
}

void test_participant_missing_limits() {
    SECTION("Participant missing position/OTR limits rejected");
    Config cfg;
    cfg.loadMap({
        {"participants", "20"},
        // neither position_limit nor otr_limit set
    });
    auto r = validateStartupConfig(cfg);
    assert(!r.ok);
    assert(hasError(r, "position_limit"));
    assert(hasError(r, "otr_limit"));
    PASS();
}

void test_participant_nonpositive_limit() {
    SECTION("Participant with non-positive limits rejected");
    Config cfg;
    cfg.loadMap({
        {"participants", "20"},
        {"participant.20.position_limit", "0"},
        {"participant.20.otr_limit", "0"},
    });
    auto r = validateStartupConfig(cfg);
    assert(!r.ok);
    assert(hasError(r, "position_limit"));
    assert(hasError(r, "otr_limit"));
    PASS();
}

void test_journal_path_unwritable() {
    SECTION("Unwritable journal path rejected");
    Config cfg;
    cfg.loadMap({
        {"journal_path", "/nonexistent_dir_p37_xyz/journal.wal"},
    });
    auto r = validateStartupConfig(cfg);
    assert(!r.ok);
    assert(hasError(r, "journal_path"));

    // A writable path (/tmp exists and is writable) passes.
    Config ok;
    ok.loadMap({{"journal_path", "/tmp/ob_cfgval_journal.wal"}});
    auto r2 = validateStartupConfig(ok);
    assert(r2.ok);
    PASS();
}

void test_multiple_errors_accumulate() {
    SECTION("Multiple violations are all reported");
    Config cfg;
    cfg.loadMap({
        {"instruments", "1"},
        {"instrument.1.min_price", "2000"},
        {"instrument.1.max_price", "1000"},   // min >= max
        {"instrument.1.tick_size", "0"},      // bad tick
        {"instrument.1.lot_size", "0"},       // bad lot
        {"participants", "5"},                // participant with no limits
    });
    auto r = validateStartupConfig(cfg);
    assert(!r.ok);
    assert(r.errors.size() >= 5);
    PASS();
}

int main() {
    std::cout << "\n═══ Config Validation (P3-7) Tests ═══\n\n";

    test_valid_config_passes();
    test_empty_config_passes();
    test_min_ge_max();
    test_tick_not_positive();
    test_lot_not_positive();
    test_price_range_exceeds_capacity();
    test_instrument_missing_fields();
    test_participant_missing_limits();
    test_participant_nonpositive_limit();
    test_journal_path_unwritable();
    test_multiple_errors_accumulate();

    std::cout << "\n─── Results: " << passed << " passed ───\n";
    std::cout << "\nAll config validation tests passed.\n\n";
    return 0;
}
