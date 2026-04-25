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

struct PriceLevelInfo {
    double price {};
    double total_qty {};
};

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
    void process_order(const Order& order, std::vector<Trade>& trades_out);

    // Cancels an order by its unique identifier.
    [[nodiscard]] bool cancel_order(std::uint64_t order_id);

    [[nodiscard]] double get_best_bid() const noexcept;
    [[nodiscard]] double get_best_ask() const noexcept;
    void get_l2_snapshot(
        std::vector<PriceLevelInfo>& bids_out,
        std::vector<PriceLevelInfo>& asks_out,
        int depth = 5
    ) const;

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
