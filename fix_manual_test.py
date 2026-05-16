import re

with open("tests/ManualTest.cpp", "r") as f:
    content = f.read()

# 1. Replace getTradeHistory().back() and [size-x] with a custom EventListener that records trades.
# We will insert a TestListener struct at the top of ManualTest.cpp
listener_code = """
struct TestListener : public OrderMatcher::EventListener {
    std::vector<OrderMatcher::Trade> trades;
    std::vector<OrderMatcher::OrderUpdate> updates;
    void onTrade(const OrderMatcher::Trade& t) override { trades.push_back(t); }
    void onOrderUpdate(const OrderMatcher::OrderUpdate& u) override { updates.push_back(u); }
    void onMarketData(const OrderMatcher::MarketDataUpdate&) override {}
};
"""
# Insert it after the includes
content = re.sub(r'(#include "Utils.h"\n)', r'\1\n' + listener_code + '\n', content)

# Replace all OrderBook declarations to also attach the listener
content = re.sub(r'OrderBook book;', r'OrderBook book;\n    TestListener listener;\n    book.setEventListener(&listener);', content)

# 2. Fix the getTradeHistory().back() accesses
content = re.sub(r'book\.getTradeHistory\(\)\.back\(\)', r'listener.trades.back()', content)
content = re.sub(r'book\.getTradeHistory\(\)\[book\.getTradeHistory\(\)\.size\(\)-2\]', r'listener.trades[listener.trades.size()-2]', content)
content = re.sub(r'book\.getTradeHistory\(\)\[book\.getTradeHistory\(\)\.size\(\)-1\]', r'listener.trades.back()', content)

# 3. For auto history = book.getTradeHistory() usage
content = re.sub(r'auto history = book\.getTradeHistory\(\);', r'auto history = listener.trades;', content)

# 4. For book.getTradeCount() equivalents
content = re.sub(r'book\.getTradeCount\(\)', r'listener.trades.size()', content)

# 5. For size checks like book.getTradeHistory().size()
content = re.sub(r'book\.getTradeHistory\(\)\.size\(\)', r'listener.trades.size()', content)

# 6. For index access like book.getTradeHistory()[i]
content = re.sub(r'book\.getTradeHistory\(\)\[([^\]]+)\]', r'listener.trades[\1]', content)

# 7. Array to count on market data
content = re.sub(r'snap\.bids\.empty\(\)', r'snap.bidCount == 0', content)
content = re.sub(r'snap\.asks\.empty\(\)', r'snap.askCount == 0', content)
content = re.sub(r'snap\.bids\.size\(\)', r'snap.bidCount', content)
content = re.sub(r'snap\.asks\.size\(\)', r'snap.askCount', content)

# 8. testOrderStatusCallbacks used std::function. Replace with existing listener updates.
content = re.sub(r'std::vector<OrderUpdate> updates;\n\s*book\.setOrderUpdateCallback\(\[&\]\(const OrderUpdate& u\) \{\n\s*updates\.push_back\(u\);\n\s*\}\);', r'auto& updates = listener.updates;', content)

with open("tests/ManualTest.cpp", "w") as f:
    f.write(content)

print("Patch applied.")
