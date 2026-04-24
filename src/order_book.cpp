#include "lob/order_book.hpp"

#include <iterator>
#include <stdexcept>
#include <utility>
#include <vector>

namespace lob {

void OrderBook::add_resting_order(const Order& order) {
    const auto [existing_order_it, inserted] = order_index_.try_emplace(order.id, OrderIterator {});
    if (!inserted) {
        throw std::invalid_argument("Duplicate order id");
    }

    try {
        if (order.side == Side::Buy) {
            auto [level_it, level_inserted] = bids_.try_emplace(order.price);
            if (level_inserted) {
                level_it->second.price = order.price;
            }

            auto& order_queue = level_it->second.orders;
            order_queue.push_back(order);
            existing_order_it->second = std::prev(order_queue.end());
            return;
        }

        auto [level_it, level_inserted] = asks_.try_emplace(order.price);
        if (level_inserted) {
            level_it->second.price = order.price;
        }

        auto& order_queue = level_it->second.orders;
        order_queue.push_back(order);
        existing_order_it->second = std::prev(order_queue.end());
    } catch (...) {
        order_index_.erase(existing_order_it);
        throw;
    }
}

std::vector<Trade> OrderBook::process_order(const Order& order) {
    if (order.quantity == 0U) {
        return {};
    }

    if (order_index_.contains(order.id)) {
        throw std::invalid_argument("Duplicate order id");
    }

    std::vector<Trade> trades {};
    Order incoming_order {order};

    if (incoming_order.side == Side::Buy) {
        while (incoming_order.quantity > 0U && !asks_.empty()) {
            auto best_ask_it = asks_.begin();
            if (best_ask_it->first > incoming_order.price) {
                break;
            }

            auto& resting_queue = best_ask_it->second.orders;
            auto resting_order_it = resting_queue.begin();

            const std::uint64_t executed_quantity = std::min(incoming_order.quantity, resting_order_it->quantity);
            trades.push_back(Trade {
                .buyer_id = incoming_order.id,
                .seller_id = resting_order_it->id,
                .price = resting_order_it->price,
                .quantity = executed_quantity,
            });

            incoming_order.quantity -= executed_quantity;
            resting_order_it->quantity -= executed_quantity;

            if (resting_order_it->quantity == 0U) {
                order_index_.erase(resting_order_it->id);
                resting_queue.erase(resting_order_it);
                if (resting_queue.empty()) {
                    asks_.erase(best_ask_it);
                }
            }
        }
    } else {
        while (incoming_order.quantity > 0U && !bids_.empty()) {
            auto best_bid_it = bids_.begin();
            if (best_bid_it->first < incoming_order.price) {
                break;
            }

            auto& resting_queue = best_bid_it->second.orders;
            auto resting_order_it = resting_queue.begin();

            const std::uint64_t executed_quantity = std::min(incoming_order.quantity, resting_order_it->quantity);
            trades.push_back(Trade {
                .buyer_id = resting_order_it->id,
                .seller_id = incoming_order.id,
                .price = resting_order_it->price,
                .quantity = executed_quantity,
            });

            incoming_order.quantity -= executed_quantity;
            resting_order_it->quantity -= executed_quantity;

            if (resting_order_it->quantity == 0U) {
                order_index_.erase(resting_order_it->id);
                resting_queue.erase(resting_order_it);
                if (resting_queue.empty()) {
                    bids_.erase(best_bid_it);
                }
            }
        }
    }

    if (incoming_order.quantity > 0U) {
        add_resting_order(incoming_order);
    }

    return trades;
}

bool OrderBook::cancel_order(std::uint64_t order_id) {
    const auto index_it = order_index_.find(order_id);
    if (index_it == order_index_.end()) {
        return false;
    }

    const OrderIterator order_it = index_it->second;
    const Side side = order_it->side;
    const double price = order_it->price;

    if (side == Side::Buy) {
        const auto level_it = bids_.find(price);
        if (level_it == bids_.end()) {
            throw std::logic_error("Order index references missing bid price level");
        }

        level_it->second.orders.erase(order_it);
        if (level_it->second.orders.empty()) {
            bids_.erase(level_it);
        }
    } else {
        const auto level_it = asks_.find(price);
        if (level_it == asks_.end()) {
            throw std::logic_error("Order index references missing ask price level");
        }

        level_it->second.orders.erase(order_it);
        if (level_it->second.orders.empty()) {
            asks_.erase(level_it);
        }
    }

    order_index_.erase(index_it);
    return true;
}

}  // namespace lob
