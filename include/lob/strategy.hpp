#pragma once

#include <cstdint>

#include "lob/order_book.hpp"
#include "lob/order.hpp"
#include "lob/trade.hpp"

namespace lob {

struct MarketSnapshot {
    std::uint64_t timestamp {};
    double best_bid {};
    double best_ask {};
    double mid_price {};
};

struct StrategyFill {
    std::uint64_t order_id {};
    Side side {Side::Buy};
    double price {};
    std::uint64_t quantity {};
    std::uint64_t timestamp {};
};

class OrderGateway {
public:
    virtual ~OrderGateway() = default;

    [[nodiscard]] virtual std::uint64_t submit_order(
        Side side,
        double price,
        std::uint64_t quantity,
        std::uint64_t timestamp
    ) = 0;

    [[nodiscard]] virtual bool cancel_order(std::uint64_t order_id) = 0;

    [[nodiscard]] virtual std::uint64_t current_timestamp() const noexcept = 0;
};

class Strategy {
public:
    virtual ~Strategy() = default;

    virtual void on_start(OrderGateway& gateway) {
        (void)gateway;
    }

    virtual void on_tick(const OrderBook& book, OrderGateway& gateway) {
        (void)book;
        (void)gateway;
    }

    virtual void on_fill(const StrategyFill& fill, OrderGateway& gateway) {
        (void)fill;
        (void)gateway;
    }
};

}  // namespace lob
