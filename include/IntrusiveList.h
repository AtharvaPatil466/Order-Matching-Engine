#pragma once

#include "Order.h"

namespace OrderMatcher {

// Intrusive List specifically for Order struct
class OrderList {
public:
    OrderList() : head(nullptr), tail(nullptr) {}

    void push_back(Order* order) {
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
};

} // namespace OrderMatcher
