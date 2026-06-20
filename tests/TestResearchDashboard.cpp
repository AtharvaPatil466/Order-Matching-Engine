// TestResearchDashboard.cpp — Google Test suite for ResearchDashboard.h
//
// Tests:
//   1. buildReport with zero-value components → all numeric fields = 0, no crash
//   2. toJson() contains "trades_analyzed" key
//   3. toCsv(true) first line is header containing "effective_spread"
//   4. toCsv(false) has no header
//   5. fromJournal with >= 20 trades → tradesAnalyzed >= 5, vpin >= 0

#include <gtest/gtest.h>

#include "ResearchDashboard.h"
#include "CalibrationPipeline.h"
#include "Journal.h"
#include "MatchingEngine.h"
#include "MicrostructureMetrics.h"
#include "OrderFlowAnalytics.h"
#include "SignalGenerator.h"
#include "SpreadDecomposition.h"
#include "Types.h"
#include "VPINCalculator.h"

#include <cstdio>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using namespace OrderMatcher;

// ── Test 1: buildReport with zero-value components ───────────────────────────

TEST(ResearchDashboardTest, BuildReport_ZeroComponents_NoFields_NoCrash) {
    MicrostructureMetrics   metrics;
    OrderFlowAnalytics      flow;
    VPINCalculator          vpinCalc;
    SpreadDecomposition_GH  spreadDecomp{};
    CalibrationResult       calib{};
    MicrostructureSnapshot  snap{};

    calib.params.eta   = 0.0;
    calib.params.gamma = 0.0;
    calib.valid        = false;

    MicrostructureReport r = ResearchDashboard::buildReport(
        metrics, flow, vpinCalc, spreadDecomp, calib, snap, 0.0, 0u);

    EXPECT_EQ(r.tradesAnalyzed, 0u);
    EXPECT_DOUBLE_EQ(r.adv,                     0.0);
    EXPECT_DOUBLE_EQ(r.effectiveSpread,          0.0);
    EXPECT_DOUBLE_EQ(r.realizedSpread,           0.0);
    EXPECT_DOUBLE_EQ(r.rollSpread,               0.0);
    EXPECT_DOUBLE_EQ(r.adverseSelectionFraction, 0.0);
    EXPECT_DOUBLE_EQ(r.orderFlowImbalance,       0.0);
    EXPECT_DOUBLE_EQ(r.kylesLambda,              0.0);
    EXPECT_DOUBLE_EQ(r.amihudRatio,              0.0);
    EXPECT_DOUBLE_EQ(r.vpin,                     0.0);
    EXPECT_DOUBLE_EQ(r.orderToTradeRatio,        0.0);
    EXPECT_DOUBLE_EQ(r.acEta,                    0.0);
    EXPECT_DOUBLE_EQ(r.acGamma,                  0.0);
    // compositeSignal/signalConfidence may be 0 from empty snapshot
    EXPECT_GE(r.signalConfidence, 0.0);
}

// ── Test 2: toJson() contains "trades_analyzed" key ──────────────────────────

TEST(ResearchDashboardTest, ToJson_ContainsTradesAnalyzedKey) {
    MicrostructureReport r;
    r.tradesAnalyzed = 42u;
    r.adv            = 1000.0;

    std::string json = r.toJson();

    EXPECT_NE(json.find("\"trades_analyzed\""), std::string::npos)
        << "JSON output: " << json;
    EXPECT_NE(json.find("42"), std::string::npos)
        << "JSON output: " << json;
}

// ── Test 3: toCsv(true) first line contains "effective_spread" ───────────────

TEST(ResearchDashboardTest, ToCsv_WithHeader_FirstLineIsHeader) {
    MicrostructureReport r;
    std::string csv = r.toCsv(true);

    // Split on first newline to get the header row.
    auto nl = csv.find('\n');
    ASSERT_NE(nl, std::string::npos) << "toCsv(true) must contain a newline";
    std::string headerRow = csv.substr(0, nl);

    EXPECT_NE(headerRow.find("effective_spread"), std::string::npos)
        << "Header row: " << headerRow;
}

// ── Test 4: toCsv(false) has no header ───────────────────────────────────────

TEST(ResearchDashboardTest, ToCsv_NoHeader_NoHeaderLine) {
    MicrostructureReport r;
    std::string csv = r.toCsv(false);

    // Should not contain the header keyword.
    EXPECT_EQ(csv.find("effective_spread"), std::string::npos)
        << "toCsv(false) should not contain header, got: " << csv;
    // Should still have numeric content (no newline needed for single-row).
    EXPECT_FALSE(csv.empty());
}

// ── Test 5: fromJournal with >= 20 trades ────────────────────────────────────

class ResearchDashboardJournalTest : public ::testing::Test {
protected:
    static constexpr SymbolId      kSym    = 1;
    static constexpr ParticipantId kBuyer  = 10;
    static constexpr ParticipantId kSeller = 20;

    std::string journalPath_;

    void SetUp() override {
        journalPath_ = (fs::temp_directory_path() /
                        ("dashboard_test_" + std::to_string(::getpid()) + "_" +
                         std::to_string(reinterpret_cast<uintptr_t>(this)) +
                         ".log")).string();
        fs::remove(journalPath_);
        writeJournal();
    }

    void TearDown() override {
        fs::remove(journalPath_);
    }

    // Write 20 crossing pairs → 20 trades, with varied prices for metrics.
    void writeJournal() {
        Journal j(journalPath_, Journal::SyncPolicy::Immediate, 1);
        OrderId id = 1;
        for (int i = 0; i < 20; ++i) {
            Price sellPrice = toPrice(100.0 + i * 0.01);
            Price buyPrice  = sellPrice;   // aggressive buyer crosses the spread

            j.logAddOrder(id++, kSeller, kSym, Side::Sell,
                          sellPrice, 10, OrderType::Limit);
            j.logAddOrder(id++, kBuyer,  kSym, Side::Buy,
                          buyPrice,  10, OrderType::Limit);
        }
        j.flush();
    }
};

TEST_F(ResearchDashboardJournalTest, FromJournal_TradesAndVPINValid) {
    MicrostructureReport r = ResearchDashboard::fromJournal(
        journalPath_, kSym, /*adv=*/0.0);

    EXPECT_GE(r.tradesAnalyzed, 5u)
        << "Expected at least 5 trades from a 20-pair journal";
    EXPECT_GE(r.vpin, 0.0)
        << "VPIN must be non-negative";
    EXPECT_LE(r.vpin, 1.0 + 1e-9)
        << "VPIN must not exceed 1";
}
