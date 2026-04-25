#include "lob/order_book.hpp"

#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <utility>
#include <vector>

namespace lob {

double OrderBook::get_best_bid() const noexcept {
    return bids_.empty() ? 0.0 : bids_.begin()->first;
}

double OrderBook::get_best_ask() const noexcept {
    return asks_.empty() ? 0.0 : asks_.begin()->first;
}

std::uint64_t OrderBook::get_total_quantity_at_price(Side side, double price) const noexcept {
    std::uint64_t total_quantity = 0U;

    if (side == Side::Buy) {
        const auto level_it = bids_.find(price);
        if (level_it == bids_.end()) {
            return 0U;
        }

        for (const Order& order : level_it->second.orders) {
            total_quantity += order.quantity;
        }
        return total_quantity;
    }

    const auto level_it = asks_.find(price);
    if (level_it == asks_.end()) {
        return 0U;
    }

    for (const Order& order : level_it->second.orders) {
        total_quantity += order.quantity;
    }
    return total_quantity;
}

void OrderBook::get_l2_snapshot(
    std::vector<PriceLevelInfo>& bids_out,
    std::vector<PriceLevelInfo>& asks_out,
    int depth
) const {
    bids_out.clear();
    asks_out.clear();

    if (depth <= 0) {
        return;
    }

    int bid_levels = 0;
    for (auto bid_it = bids_.begin(); bid_it != bids_.end() && bid_levels < depth; ++bid_it, ++bid_levels) {
        double total_quantity = 0.0;
        for (const Order& order : bid_it->second.orders) {
            total_quantity += static_cast<double>(order.quantity);
        }

        bids_out.push_back(PriceLevelInfo {
            .price = bid_it->first,
            .total_qty = total_quantity,
        });
    }

    int ask_levels = 0;
    for (auto ask_it = asks_.begin(); ask_it != asks_.end() && ask_levels < depth; ++ask_it, ++ask_levels) {
        double total_quantity = 0.0;
        for (const Order& order : ask_it->second.orders) {
            total_quantity += static_cast<double>(order.quantity);
        }

        asks_out.push_back(PriceLevelInfo {
            .price = ask_it->first,
            .total_qty = total_quantity,
        });
    }
}

void OrderBook::add_resting_order(const Order& order) {
    if (order_index_.contains(order.id)) {
        throw std::invalid_argument("Duplicate order id");
    }

    if (order.side == Side::Buy) {
        auto [level_it, level_inserted] = bids_.try_emplace(order.price);
        if (level_inserted) {
            level_it->second.price = order.price;
        }

        auto& order_queue = level_it->second.orders;
        try {
            order_queue.push_back(order);
            const auto order_it = std::prev(order_queue.end());
            order_index_.emplace(order.id, OrderLocation {
                .order_iterator = order_it,
                .side = Side::Buy,
                .bid_level_iterator = level_it,
            });
        } catch (...) {
            if (!order_queue.empty()) {
                order_queue.pop_back();
            }
            if (order_queue.empty()) {
                bids_.erase(level_it);
            }
            throw;
        }
        return;
    }

    auto [level_it, level_inserted] = asks_.try_emplace(order.price);
    if (level_inserted) {
        level_it->second.price = order.price;
    }

    auto& order_queue = level_it->second.orders;
    try {
        order_queue.push_back(order);
        const auto order_it = std::prev(order_queue.end());
        order_index_.emplace(order.id, OrderLocation {
            .order_iterator = order_it,
            .side = Side::Sell,
            .ask_level_iterator = level_it,
        });
    } catch (...) {
        if (!order_queue.empty()) {
            order_queue.pop_back();
        }
        if (order_queue.empty()) {
            asks_.erase(level_it);
        }
        throw;
    }
}

std::vector<Trade> OrderBook::process_order(const Order& order) {
    std::vector<Trade> trades {};
    process_order(order, trades);
    return trades;
}

void OrderBook::process_order(const Order& order, std::vector<Trade>& trades) {
    trades.clear();

    if (order.quantity == 0U) {
        return;
    }

    if (order_index_.contains(order.id)) {
        throw std::invalid_argument("Duplicate order id");
    }

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
                .taker_order_id = incoming_order.id,
                .price = resting_order_it->price,
                .quantity = executed_quantity,
                .timestamp = incoming_order.timestamp,
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
                .taker_order_id = incoming_order.id,
                .price = resting_order_it->price,
                .quantity = executed_quantity,
                .timestamp = incoming_order.timestamp,
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
}

bool OrderBook::cancel_order(std::uint64_t order_id) {
    const auto index_it = order_index_.find(order_id);
    if (index_it == order_index_.end()) {
        return false;
    }

    const OrderLocation& order_location = index_it->second;
    if (order_location.side == Side::Buy) {
        auto& order_queue = order_location.bid_level_iterator->second.orders;
        order_queue.erase(order_location.order_iterator);
        if (order_queue.empty()) {
            bids_.erase(order_location.bid_level_iterator);
        }
    } else {
        auto& order_queue = order_location.ask_level_iterator->second.orders;
        order_queue.erase(order_location.order_iterator);
        if (order_queue.empty()) {
            asks_.erase(order_location.ask_level_iterator);
        }
    }

    order_index_.erase(index_it);
    return true;
}

}  // namespace lob
