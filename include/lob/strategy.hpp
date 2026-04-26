#pragma once

#include <cstdint>

#include "lob/event_merger.hpp"
#include "lob/order_gateway.hpp"
#include "lob/order_book.hpp"
#include "lob/order.hpp"
#include "lob/trade.hpp"

namespace lob {

class Strategy {
public:
    virtual ~Strategy() = default;

    virtual void on_start(OrderGateway& gateway) {
        (void)gateway;
    }

    virtual void on_tick(AssetID asset_id, const OrderBook& book, OrderGateway& gateway) {
        (void)asset_id;
        on_tick(book, gateway);
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
