// ConfigValidationTest — P3-7 fail-fast startup config validation.
//
// Verifies validateStartupConfig() refuses every control OrderEngine would
// otherwise start without (a symbol's band, participants' position limits)
// unless waived, refuses keys nothing applies, and accepts a valid config. Uses Config::loadMap so no files are needed except for the
// journal-path writability check.

#include "Config.h"
#include "ConfigValidator.h"
#include "TempPath.h"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

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
// One symbol, banded; one participant, limited.
static void fillValidConfig(Config& cfg) {
    cfg.loadMap({
        {"instrument.0.min_price", "990000"},
        {"instrument.0.max_price", "1010000"},   // 20001 levels < 200001
        {"participants", "10"},
        {"participant.10.position_limit", "5000000"},
    });
}

static ConfigValidationResult check(const Config& cfg, ControlWaivers w = {},
                                    size_t capacity = FlatPriceMap::DEFAULT_CAPACITY) {
    return validateStartupConfig(cfg, /*numSymbols=*/1, w, capacity);
}

static void expectError(const ConfigValidationResult& r, const std::string& needle) {
    if (r.ok || !hasError(r, needle)) {
        std::cout << "\n    wanted an error with: " << needle;
        for (auto& e : r.errors) std::cout << "\n    got: " << e;
        assert(false);
    }
}

void test_valid_config_passes() {
    SECTION("Valid config passes");
    Config cfg; fillValidConfig(cfg);
    auto r = check(cfg);
    if (!r.ok) for (auto& e : r.errors) std::cout << "\n    unexpected: " << e;
    assert(r.ok);
    assert(r.errors.empty());
    PASS();
}

void test_empty_config_needs_waivers() {
    SECTION("Empty config: refused without waivers, passes with both");
    Config cfg;
    auto r = check(cfg);
    expectError(r, "--no-price-band");
    expectError(r, "--no-risk-limits");
    assert(check(cfg, {true, true}).ok);
    PASS();
}

void test_waiver_does_not_excuse_a_bad_band() {
    SECTION("A waiver allows an absent band, not a broken one");
    Config cfg; fillValidConfig(cfg);
    cfg.set("instrument.0.max_price", "1");
    expectError(check(cfg, {true, true}), "min_price");
    Config half; half.loadMap({{"instrument.0.min_price", "5"}});
    expectError(check(half, {true, true}), "set both");
    PASS();
}

void test_min_ge_max() {
    SECTION("min_price >= max_price, or min_price <= 0, rejected");
    Config cfg; fillValidConfig(cfg);
    cfg.set("instrument.0.min_price", "1010000");
    cfg.set("instrument.0.max_price", "1000000");
    expectError(check(cfg), "min_price");
    Config zero; fillValidConfig(zero);
    zero.set("instrument.0.min_price", "0");
    expectError(check(zero), "0 < min_price");
    PASS();
}

void test_unapplied_keys_refused() {
    SECTION("tick_size, lot_size and per-participant otr_limit are refused, not certified");
    for (const char* key : {"instrument.0.tick_size", "instrument.0.lot_size",
                            "participant.10.otr_limit"}) {
        Config cfg; fillValidConfig(cfg);
        cfg.set(key, "1");
        expectError(check(cfg), "is not applied");
    }
    PASS();
}

void test_price_range_exceeds_capacity() {
    SECTION("A band the price map cannot hold is refused, and the message says why");
    Config cfg; fillValidConfig(cfg);
    expectError(check(cfg, {}, /*capacity=*/100), "fixed tick of 0.0001");

    // The real map: 200,000 ticks fits, 200,001 does not.
    Config edge; fillValidConfig(edge);
    edge.set("instrument.0.min_price", "1");
    edge.set("instrument.0.max_price", "200001");
    assert(check(edge).ok);
    edge.set("instrument.0.max_price", "200002");
    const auto r = check(edge);
    expectError(r, "200002 price levels");
    expectError(r, "widest band is 200000 ticks");
    PASS();
}

void test_every_symbol_needs_a_band() {
    SECTION("Every symbol 0..N-1 needs a band");
    Config cfg; fillValidConfig(cfg);
    const auto r = validateStartupConfig(cfg, /*numSymbols=*/2, {});
    expectError(r, "symbol 1 has no price band");
    PASS();
}

void test_participant_limits() {
    SECTION("Participant ids and position limits");
    Config missing; missing.loadMap({{"participants", "20"}});
    expectError(check(missing, {true, false}), "position_limit not set");
    Config zero; zero.loadMap({{"participants", "20"}, {"participant.20.position_limit", "0"}});
    expectError(check(zero, {true, false}), "must be > 0");
    Config bad; bad.loadMap({{"participants", "x,1024"}});
    const auto r = check(bad, {true, false});
    expectError(r, "'x' is not a participant id");
    expectError(r, "'1024' is not a participant id");
    PASS();
}

void test_risk_coverage() {
    SECTION("A credentialed participant without a limit is refused unless waived");
    Config cfg; fillValidConfig(cfg);
    ConfigValidationResult r;
    validateRiskCoverage(cfg, {10, 11}, /*waived=*/false, r);
    expectError(r, "participant 11 can trade");
    assert(!hasError(r, "participant 10 "));
    ConfigValidationResult w;
    validateRiskCoverage(cfg, {10, 11}, /*waived=*/true, w);
    assert(w.ok);
    PASS();
}

void test_rate_limit() {
    SECTION("rate_limit: both > 0 or neither");
    Config both; fillValidConfig(both);
    both.loadMap({{"rate_limit.default_rate", "100"}, {"rate_limit.default_burst", "10"}});
    assert(check(both).ok);
    for (const char* bad : {"rate_limit.default_rate", "rate_limit.default_burst"}) {
        Config one; fillValidConfig(one);
        one.set(bad, "5");
        expectError(check(one), "must both be set and > 0");
    }
    Config zero; fillValidConfig(zero);
    zero.loadMap({{"rate_limit.default_rate", "0"}, {"rate_limit.default_burst", "10"}});
    expectError(check(zero), "must both be set and > 0");
    PASS();
}

void test_journal_path_writable() {
    SECTION("Journal path writability");
    assert(!isJournalPathWritable("/nonexistent_dir_p37_xyz/journal.wal"));
    assert(isJournalPathWritable(uniqueTempPath("cfgval_journal.wal")));
    PASS();
}

// worker_cores is checked against an explicit allowed set, so every case runs
// on every platform; {} stands for one with no affinity mask (macOS).
// `needle` nullptr: must pass and return `want`.
static void checkWorkerCores(const char* spec, size_t workers, const std::vector<int>& allowed,
                             const char* needle, const std::vector<int>& want = {}) {
    Config cfg;
    cfg.set("worker_cores", spec);
    ConfigValidationResult r;
    const std::vector<int> cores = validateWorkerCores(cfg, workers, allowed, r);
    if (needle == nullptr) {
        if (!r.ok) for (auto& e : r.errors) std::cout << "\n    unexpected: " << e;
        assert(r.ok && cores == want);
        return;
    }
    if (r.ok || !hasError(r, needle)) {
        std::cout << "\n    '" << spec << "' wanted an error with: " << needle;
        for (auto& e : r.errors) std::cout << "\n    got: " << e;
        assert(false);
    }
    assert(cores.empty());
}

void test_worker_cores() {
    SECTION("worker_cores: a bad list is rejected, never ignored");
    const std::vector<int> allowed = {2, 3, 4, 5};
    checkWorkerCores("2,3,4,5", 4, allowed, nullptr, {2, 3, 4, 5});
    checkWorkerCores(" 5 , 4 ", 2, allowed, nullptr, {5, 4});
    checkWorkerCores("2,3,4", 2, allowed, nullptr, {2, 3, 4});  // extras unused
    checkWorkerCores("2,3", 4, allowed, "names 2 CPU(s) for 4 worker threads");
    checkWorkerCores("", 1, allowed, "names 0 CPU(s) for 1 worker threads");
    checkWorkerCores("0,2", 2, allowed, "CPU 0 is not one this process may run on (2-5)");
    checkWorkerCores("2,3,9", 2, allowed, "CPU 9 is not one this process may run on");
    checkWorkerCores("2,2", 2, allowed, "CPU 2 is named twice");
    checkWorkerCores("2-5", 4, allowed, "'2-5' is not a CPU number");
    checkWorkerCores("2,x", 2, allowed, "'x' is not a CPU number");
    checkWorkerCores("-1", 1, allowed, "'-1' is not a CPU number");
    checkWorkerCores("2", 1, {}, "no CPU affinity mask");
    Config unset;
    ConfigValidationResult r;
    assert(validateWorkerCores(unset, 4, allowed, r).empty() && r.ok);
    PASS();
}

int main() {
    std::cout << "\n═══ Config Validation (P3-7) Tests ═══\n\n";

    test_valid_config_passes();
    test_empty_config_needs_waivers();
    test_waiver_does_not_excuse_a_bad_band();
    test_min_ge_max();
    test_unapplied_keys_refused();
    test_price_range_exceeds_capacity();
    test_every_symbol_needs_a_band();
    test_participant_limits();
    test_risk_coverage();
    test_rate_limit();
    test_journal_path_writable();
    test_worker_cores();

    std::cout << "\n─── Results: " << passed << " passed ───\n";
    std::cout << "\nAll config validation tests passed.\n\n";
    return 0;
}
