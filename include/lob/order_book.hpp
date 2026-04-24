#pragma once

#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <unordered_map>
#include <vector>

#include "lob/order.hpp"
#include "lob/price_level.hpp"
#include "lob/trade.hpp"

namespace lob {

class OrderBook {
public:
    using OrderIterator = std::list<Order>::iterator;
    using BidBook = std::map<double, PriceLevel, std::greater<double>>;
    using AskBook = std::map<double, PriceLevel>;

    struct OrderLocation {
        OrderIterator order_iterator;
        Side side;
        BidBook::iterator bid_level_iterator {};
        AskBook::iterator ask_level_iterator {};
    };

    // Direct order and price-level iterators remove extra tree lookups during cancellation.
    using OrderIndex = std::unordered_map<std::uint64_t, OrderLocation>;

    OrderBook() = default;
    ~OrderBook() = default;

    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) = default;
    OrderBook& operator=(OrderBook&&) = default;

    // Matches the incoming order against the opposite side, then rests any residual.
    [[nodiscard]] std::vector<Trade> process_order(const Order& order);

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
    void add_resting_order(const Order& order);

    BidBook bids_ {};
    AskBook asks_ {};
    OrderIndex order_index_ {};
};

}  // namespace lob
