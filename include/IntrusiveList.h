#pragma once

#include "Order.h"

namespace OrderMatcher {

// Intrusive List specifically for Order struct. It is also the price level, so
// it carries the level's displayed totals: the visible quantity and the count
// of displayed (non-hidden) orders, the two numbers an L2 update publishes.
// They are maintained on every change rather than recomputed by walking the
// level, which made each add, fill and cancel cost O(orders at the price) as
// soon as a listener was attached (PERF-1).
//
// The rule that keeps them right: the totals always hold displayQuantity() of
// every linked non-hidden order AS IT IS NOW. push_back adds an order's
// current display, remove subtracts it, and whoever changes a linked order's
// remainingQty or visibleQty in place calls restate() with what it displayed
// before. OrderBook::validateIntegrity checks them against a full walk.
class OrderList {
public:
    OrderList() : head(nullptr), tail(nullptr) {}

    void push_back(Order* order) {
        if (!order->isHidden) {
            visibleQty_ += displayQuantity(*order);
            ++visibleCount_;
        }
        order->next = nullptr;
        order->prev = tail;
        if (tail) {
            tail->next = order;
        } else {
            head = order;
        }
        tail = order;
    }

    void remove(Order* order) {
        if (!order->isHidden) {
            visibleQty_ -= displayQuantity(*order);
            --visibleCount_;
        }
        if (order->prev) {
            order->prev->next = order->next;
        } else {
            head = order->next;
        }

        if (order->next) {
            order->next->prev = order->prev;
        } else {
            tail = order->prev;
        }
        
        order->next = nullptr;
        order->prev = nullptr;
    }

    // A linked order's displayed size changed in place (a fill, a reduction,
    // an iceberg slice refreshed or clamped); `displayBefore` is what it
    // displayed before the change.
    void restate(const Order& order, Quantity displayBefore) {
        if (!order.isHidden) visibleQty_ += displayQuantity(order) - displayBefore;
    }

    Quantity visibleQty() const { return visibleQty_; }
    uint32_t visibleCount() const { return visibleCount_; }

    Order* front() const {
        return head;
    }

    bool empty() const {
        return head == nullptr;
    }

    // Iterator support for range-based loops
    class Iterator {
    public:
        Iterator(Order* ptr) : m_ptr(ptr) {}
        Order* operator*() const { return m_ptr; }
        Iterator& operator++() { m_ptr = m_ptr->next; return *this; }
        bool operator!=(const Iterator& other) const { return m_ptr != other.m_ptr; }
    private:
        Order* m_ptr;
    };

    Iterator begin() const { return Iterator(head); }
    Iterator end() const { return Iterator(nullptr); }

private:
    Order* head;
    Order* tail;
    Quantity visibleQty_ = 0;
    uint32_t visibleCount_ = 0;
};

} // namespace OrderMatcher
