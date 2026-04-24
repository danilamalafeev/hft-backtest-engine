#pragma once

#include <cstddef>
#include <list>

#include "lob/order.hpp"

namespace lob {

// Each price level owns a FIFO queue of orders at an identical price.
struct PriceLevel {
    using OrderQueue = std::list<Order>;
    using iterator = OrderQueue::iterator;
    using const_iterator = OrderQueue::const_iterator;

    double price {};
    OrderQueue orders {};

    [[nodiscard]] bool empty() const noexcept {
        return orders.empty();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return orders.size();
    }
};

}  // namespace lob
