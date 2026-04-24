#pragma once

#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <unordered_map>

#include "lob/order.hpp"
#include "lob/price_level.hpp"

namespace lob {

class OrderBook {
public:
    using OrderIterator = std::list<Order>::iterator;
    using BidBook = std::map<double, PriceLevel, std::greater<double>>;
    using AskBook = std::map<double, PriceLevel>;
    // Direct iterators enable O(1) access to an order node during cancellation.
    using OrderIndex = std::unordered_map<std::uint64_t, OrderIterator>;

    OrderBook() = default;
    ~OrderBook() = default;

    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) = default;
    OrderBook& operator=(OrderBook&&) = default;

    // Inserts a new order into the relevant side of the book.
    void add_order(const Order& order);

    // Cancels an order by its unique identifier.
    [[nodiscard]] bool cancel_order(std::uint64_t order_id);

    [[nodiscard]] const BidBook& bids() const noexcept {
        return bids_;
    }

    [[nodiscard]] const AskBook& asks() const noexcept {
        return asks_;
    }

    [[nodiscard]] const OrderIndex& order_index() const noexcept {
        return order_index_;
    }

private:
    BidBook bids_ {};
    AskBook asks_ {};
    OrderIndex order_index_ {};
};

}  // namespace lob
