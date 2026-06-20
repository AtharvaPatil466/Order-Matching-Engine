// TestResearchSerializer — Google Test suite for ResearchSerializer.
//
// Test coverage:
//  1.  tickHistoryToCsv  with 2 snapshots → correct header + 2 data rows
//  2.  tickHistoryToJsonl with 2 snapshots → 2 valid JSON objects
//  3.  tickHistoryToCsv  with empty history → header only
//  4.  tradeEventsToCsv  with 1 event → correct columns
//  5.  fillRecordsToCsv  with 2 fills → correct columns and values

#include <gtest/gtest.h>

#include "ResearchSerializer.h"
#include "Types.h"

#include <sstream>
#include <string>
#include <vector>

using namespace OrderMatcher;

// ── Helper: split a string by delimiter ───────────────────────────────────────
static std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> lines;
    std::istringstream ss(s);
    std::string line;
    while (std::getline(ss, line))
        lines.push_back(line);
    return lines;
}

static std::vector<std::string> splitCols(const std::string& s, char delim = ',') {
    std::vector<std::string> cols;
    std::istringstream ss(s);
    std::string col;
    while (std::getline(ss, col, delim))
        cols.push_back(col);
    return cols;
}

// ─────────────────────────────────────────────────────────────────────────────
// 1. tickHistoryToCsv with 2 snapshots
// ─────────────────────────────────────────────────────────────────────────────
TEST(ResearchSerializer, TickHistoryCsvTwoSnapshots) {
    std::vector<SimulationDriver::TickSnapshot> history;

    SimulationDriver::TickSnapshot s1{};
    s1.tick            = 1;
    s1.bestBid         = 9990;
    s1.bestAsk         = 10010;
    s1.lastTradePrice  = 10000;
    s1.bidDepth        = 500;
    s1.askDepth        = 300;
    s1.volume          = 100;
    s1.makerNetPosition = 50;

    SimulationDriver::TickSnapshot s2{};
    s2.tick            = 2;
    s2.bestBid         = 9980;
    s2.bestAsk         = 10020;
    s2.lastTradePrice  = 10005;
    s2.bidDepth        = 400;
    s2.askDepth        = 200;
    s2.volume          = 200;
    s2.makerNetPosition = -10;

    history.push_back(s1);
    history.push_back(s2);

    std::string csv = tickHistoryToCsv(history);
    auto lines = splitLines(csv);

    // Header + 2 data rows = 3 lines total
    ASSERT_EQ(lines.size(), 3u);

    // Verify header
    EXPECT_EQ(lines[0],
              "tick,bestBid,bestAsk,lastTradePrice,bidDepth,askDepth,volume,makerNetPosition");

    // Verify row 1
    auto cols1 = splitCols(lines[1]);
    ASSERT_EQ(cols1.size(), 8u);
    EXPECT_EQ(cols1[0], "1");      // tick
    EXPECT_EQ(cols1[1], "9990");   // bestBid
    EXPECT_EQ(cols1[2], "10010");  // bestAsk
    EXPECT_EQ(cols1[3], "10000");  // lastTradePrice
    EXPECT_EQ(cols1[4], "500");    // bidDepth
    EXPECT_EQ(cols1[5], "300");    // askDepth
    EXPECT_EQ(cols1[6], "100");    // volume
    EXPECT_EQ(cols1[7], "50");     // makerNetPosition

    // Verify row 2
    auto cols2 = splitCols(lines[2]);
    ASSERT_EQ(cols2.size(), 8u);
    EXPECT_EQ(cols2[0], "2");
    EXPECT_EQ(cols2[7], "-10");    // negative makerNetPosition
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. tickHistoryToJsonl with 2 snapshots → 2 valid JSON objects
// ─────────────────────────────────────────────────────────────────────────────
TEST(ResearchSerializer, TickHistoryJsonlTwoSnapshots) {
    std::vector<SimulationDriver::TickSnapshot> history;

    SimulationDriver::TickSnapshot s1{};
    s1.tick            = 7;
    s1.bestBid         = 1000;
    s1.bestAsk         = 1010;
    s1.lastTradePrice  = 1005;
    s1.bidDepth        = 50;
    s1.askDepth        = 60;
    s1.volume          = 10;
    s1.makerNetPosition = 5;

    SimulationDriver::TickSnapshot s2{};
    s2.tick            = 8;
    s2.bestBid         = 1001;
    s2.bestAsk         = 1011;
    s2.lastTradePrice  = 1006;
    s2.bidDepth        = 55;
    s2.askDepth        = 65;
    s2.volume          = 20;
    s2.makerNetPosition = -3;

    history.push_back(s1);
    history.push_back(s2);

    std::string jsonl = tickHistoryToJsonl(history);
    auto lines = splitLines(jsonl);

    // Exactly 2 lines, no trailing newline
    ASSERT_EQ(lines.size(), 2u);

    // Each line must contain all expected field keys
    for (const auto& line : lines) {
        EXPECT_NE(line.find("\"tick\""),            std::string::npos);
        EXPECT_NE(line.find("\"bestBid\""),         std::string::npos);
        EXPECT_NE(line.find("\"bestAsk\""),         std::string::npos);
        EXPECT_NE(line.find("\"lastTradePrice\""),  std::string::npos);
        EXPECT_NE(line.find("\"bidDepth\""),        std::string::npos);
        EXPECT_NE(line.find("\"askDepth\""),        std::string::npos);
        EXPECT_NE(line.find("\"volume\""),          std::string::npos);
        EXPECT_NE(line.find("\"makerNetPosition\""),std::string::npos);
    }

    // Line 1 should contain tick=7, line 2 tick=8
    EXPECT_NE(lines[0].find("\"tick\":7"),  std::string::npos);
    EXPECT_NE(lines[1].find("\"tick\":8"),  std::string::npos);

    // Negative makerNetPosition should appear correctly
    EXPECT_NE(lines[1].find("\"makerNetPosition\":-3"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. tickHistoryToCsv with empty history → header only, no trailing newline
// ─────────────────────────────────────────────────────────────────────────────
TEST(ResearchSerializer, TickHistoryCsvEmptyHistory) {
    std::vector<SimulationDriver::TickSnapshot> history;
    std::string csv = tickHistoryToCsv(history);

    // Should be exactly the header line with no trailing newline
    EXPECT_EQ(csv,
              "tick,bestBid,bestAsk,lastTradePrice,bidDepth,askDepth,volume,makerNetPosition");

    auto lines = splitLines(csv);
    ASSERT_EQ(lines.size(), 1u);
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. tradeEventsToCsv with 1 event → correct columns
// ─────────────────────────────────────────────────────────────────────────────
TEST(ResearchSerializer, TradeEventsCsvOneEvent) {
    MicrostructureMetrics::TradeEvent e{};
    e.price       = 10050;
    e.side        = Side::Buy;
    e.qty         = 200;
    e.midpoint    = 10025;
    e.timestampNs = 0;

    std::vector<MicrostructureMetrics::TradeEvent> events{e};
    std::string csv = tradeEventsToCsv(events);

    auto lines = splitLines(csv);
    ASSERT_EQ(lines.size(), 2u);

    // Header
    EXPECT_EQ(lines[0], "seqNum,price,qty,side,midpoint");

    // Data row
    auto cols = splitCols(lines[1]);
    ASSERT_EQ(cols.size(), 5u);
    EXPECT_EQ(cols[0], "0");        // seqNum = 0
    EXPECT_EQ(cols[1], "10050");    // price
    EXPECT_EQ(cols[2], "200");      // qty
    EXPECT_EQ(cols[3], "Buy");      // side
    EXPECT_EQ(cols[4], "10025");    // midpoint
}

TEST(ResearchSerializer, TradeEventsCsvSellSide) {
    MicrostructureMetrics::TradeEvent e{};
    e.price    = 9990;
    e.side     = Side::Sell;
    e.qty      = 50;
    e.midpoint = 10000;

    std::vector<MicrostructureMetrics::TradeEvent> events{e};
    std::string csv = tradeEventsToCsv(events);
    auto lines = splitLines(csv);
    ASSERT_EQ(lines.size(), 2u);

    auto cols = splitCols(lines[1]);
    ASSERT_EQ(cols.size(), 5u);
    EXPECT_EQ(cols[3], "Sell");
}

TEST(ResearchSerializer, TradeEventsCsvEmpty) {
    std::vector<MicrostructureMetrics::TradeEvent> events;
    std::string csv = tradeEventsToCsv(events);
    EXPECT_EQ(csv, "seqNum,price,qty,side,midpoint");
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. fillRecordsToCsv with 2 fills → correct columns and values
// ─────────────────────────────────────────────────────────────────────────────
TEST(ResearchSerializer, FillRecordsCsvTwoFills) {
    FillRecord f1{100.50, 10.0, 1};
    FillRecord f2{99.25, -5.0, 2};

    std::vector<FillRecord> fills{f1, f2};
    std::string csv = fillRecordsToCsv(fills);

    auto lines = splitLines(csv);
    ASSERT_EQ(lines.size(), 3u);

    // Header
    EXPECT_EQ(lines[0], "seqNum,price,qty");

    // Row 1: seqNum=1, price=100.50, qty=10.0
    auto cols1 = splitCols(lines[1]);
    ASSERT_EQ(cols1.size(), 3u);
    EXPECT_EQ(cols1[0], "1");               // seqNum from FillRecord
    EXPECT_EQ(cols1[1].substr(0, 5), "100.5"); // price starts with 100.5
    EXPECT_EQ(cols1[2].substr(0, 5), "10.00"); // qty starts with 10.00

    // Row 2: seqNum=2, price=99.25, qty=-5.0 (signed)
    auto cols2 = splitCols(lines[2]);
    ASSERT_EQ(cols2.size(), 3u);
    EXPECT_EQ(cols2[0], "2");
    // Verify negative qty is preserved
    EXPECT_NE(cols2[2].find('-'), std::string::npos);
}

TEST(ResearchSerializer, FillRecordsCsvEmpty) {
    std::vector<FillRecord> fills;
    std::string csv = fillRecordsToCsv(fills);
    EXPECT_EQ(csv, "seqNum,price,qty");
}

// Additional: verify JSONL with empty history → empty string
TEST(ResearchSerializer, TickHistoryJsonlEmpty) {
    std::vector<SimulationDriver::TickSnapshot> history;
    std::string jsonl = tickHistoryToJsonl(history);
    EXPECT_EQ(jsonl, "");
}

// Additional: tradeEventsToCsv with 2 events → seqNums are sequential (0, 1)
TEST(ResearchSerializer, TradeEventsCsvSeqNumIncrement) {
    MicrostructureMetrics::TradeEvent e{};
    e.price = 10000; e.side = Side::Buy; e.qty = 100; e.midpoint = 9995;
    std::vector<MicrostructureMetrics::TradeEvent> events{e, e};
    std::string csv = tradeEventsToCsv(events);
    auto lines = splitLines(csv);
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_EQ(splitCols(lines[1])[0], "0");
    EXPECT_EQ(splitCols(lines[2])[0], "1");
}
