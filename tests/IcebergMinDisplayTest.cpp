// IcebergMinDisplayTest — an iceberg shows at least 1/kMaxIcebergSlices of its
// size (roadmap 1.5-E2, MATCH-19).
//
// WHAT WAS WRONG. Any display size of 1 or more was accepted. An iceberg of
// 1,000,000 showing 1 turned one aggressor for 1,000,000 into a million
// trades, a million slice refreshes and a million book events, all under the
// book lock: unbounded work per incoming order, chosen by the resting side.
//
// WHAT IS PINNED. Admission refuses displayQty < ceil(qty / kMaxIcebergSlices)
// with InvalidDisplayQty, so no iceberg is worked in more than
// kMaxIcebergSlices slices. A replace that grows the total past that bound
// (its display size is fixed) is refused the same way and leaves the order
// untouched.

#include "OrderBook.h"

#include <cassert>
#include <cstdio>
#include <variant>

using namespace OrderMatcher;

namespace {

constexpr Price kPx = 10000;
constexpr Quantity kSlices = OrderBook::kMaxIcebergSlices;

RejectReason rejectOf(const AddOrderResult& r) {
    return std::holds_alternative<RejectReason>(r) ? std::get<RejectReason>(r)
                                                   : RejectReason::None;
}

void admissionRefusesATooSmallDisplay() {
    OrderBook book(1);
    book.setCircuitBreakerThreshold(1e9);
    const Quantity total = 10 * kSlices + 1;  // needs a display of 11

    const auto tooSmall = book.addOrder(1, 1, Side::Sell, kPx, total,
                                        OrderType::Iceberg, 0, /*displayQty=*/10);
    if (rejectOf(tooSmall) != RejectReason::InvalidDisplayQty)
        std::printf("  FAIL iceberg %llu showing 10 (%llu slices) was admitted\n",
                    static_cast<unsigned long long>(total),
                    static_cast<unsigned long long>((total + 9) / 10));
    assert(rejectOf(tooSmall) == RejectReason::InvalidDisplayQty &&
           "an iceberg needing more than kMaxIcebergSlices slices is refused");
    assert(book.getOrder(1) == nullptr);

    const auto atBound = book.addOrder(2, 1, Side::Sell, kPx, total,
                                       OrderType::Iceberg, 0, /*displayQty=*/11);
    assert(std::holds_alternative<OrderId>(atBound) &&
           "a display of ceil(qty / kMaxIcebergSlices) is admitted");
    std::puts("  admission: display < ceil(qty / kMaxIcebergSlices) -> InvalidDisplayQty");
}

void replaceCannotGrowPastTheBound() {
    OrderBook book(1);
    book.setCircuitBreakerThreshold(1e9);
    book.addOrder(1, 1, Side::Sell, kPx, 100, OrderType::Iceberg, 0, /*displayQty=*/10);

    RejectReason reason = RejectReason::None;
    const bool grew = book.cancelReplace(1, kPx, 10 * kSlices + 1, reason, 1);
    if (grew)
        std::printf("  FAIL replace grew iceberg showing 10 to %llu\n",
                    static_cast<unsigned long long>(10 * kSlices + 1));
    assert(!grew && reason == RejectReason::InvalidDisplayQty &&
           "a replace past display * kMaxIcebergSlices is refused");
    assert(book.getOrder(1)->initialQty == 100 && book.getOrder(1)->remainingQty == 100 &&
           "a refused replace leaves the order untouched");

    assert(book.cancelReplace(1, kPx, 10 * kSlices, reason, 1) &&
           "a replace up to display * kMaxIcebergSlices is accepted");
    std::puts("  replace: total > display * kMaxIcebergSlices -> InvalidDisplayQty");
}

}  // namespace

int main() {
    admissionRefusesATooSmallDisplay();
    replaceCannotGrowPastTheBound();
    std::puts("IcebergMinDisplayTest passed");
    return 0;
}
