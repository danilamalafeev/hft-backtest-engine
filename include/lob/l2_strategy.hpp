#pragma once

#include "lob/l2_order_book.hpp"
#include "lob/order_gateway.hpp"
#include "lob/order.hpp"

namespace lob {

class L2Strategy {
public:
    virtual ~L2Strategy() = default;

    virtual void on_start(OrderGateway& gateway) {
        (void)gateway;
    }

    virtual void on_tick(AssetID asset_id, const L2OrderBook& book, OrderGateway& gateway) {
        (void)asset_id;
        (void)book;
        (void)gateway;
    }

    virtual void on_fill(const StrategyFill& fill, OrderGateway& gateway) {
        (void)fill;
        (void)gateway;
    }
};

}  // namespace lob
