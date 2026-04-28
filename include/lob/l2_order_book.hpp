#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

#include "lob/event_l2_update.hpp"

namespace lob {

class L2OrderBook {
public:
    static constexpr std::size_t kDefaultMaxLevelsPerSide = 4096U;

    struct Level {
        double price {};
        double qty {};
        double depleted_qty {};

        [[nodiscard]] double effective_qty() const noexcept {
            return qty > depleted_qty ? qty - depleted_qty : 0.0;
        }
    };
    using LevelIterator = std::vector<Level>::iterator;
    using ConstLevelIterator = std::vector<Level>::const_iterator;

    explicit L2OrderBook(std::size_t max_levels_per_side = kDefaultMaxLevelsPerSide)
        : max_levels_per_side_(max_levels_per_side) {
        bids_.reserve(max_levels_per_side_);
        asks_.reserve(max_levels_per_side_);
    }

    void reserve(std::size_t max_levels_per_side) {
        max_levels_per_side_ = max_levels_per_side;
        bids_.reserve(max_levels_per_side_);
        asks_.reserve(max_levels_per_side_);
        trim_to_capacity(true, bids_);
        trim_to_capacity(false, asks_);
    }

    void clear() noexcept {
        bids_.clear();
        asks_.clear();
        bid_total_qty_ = 0.0;
        ask_total_qty_ = 0.0;
        bid_total_notional_ = 0.0;
        ask_total_notional_ = 0.0;
        bid_depleted_qty_ = 0.0;
        ask_depleted_qty_ = 0.0;
        bid_depleted_notional_ = 0.0;
        ask_depleted_notional_ = 0.0;
    }

    void apply_update(const L2UpdateEvent& event) {
        if (event.is_snapshot) {
            clear();
        }
        update_level(event.is_bid, event.price, event.qty);
    }

    void update_level(bool is_bid, double price, double qty) {
        if (price <= 0.0) {
            return;
        }

        std::vector<Level>& side = levels_for(is_bid);
        auto level_it = find_level(side, is_bid, price);
        const bool found = level_it != side.end() && level_it->price == price;

        if (qty <= 0.0) {
            if (found) {
                acc_subtract(is_bid, *level_it);
                side.erase(level_it);
            }
            return;
        }

        if (found) {
            acc_subtract(is_bid, *level_it);
            level_it->qty = qty;
            if (level_it->depleted_qty > qty) {
                level_it->depleted_qty = qty;
            }
            acc_add(is_bid, *level_it);
            return;
        }

        if (side.size() >= max_levels_per_side_) {
            if (side.empty() || is_deeper_or_equal(is_bid, price, side.back().price)) {
                return;
            }
            acc_subtract(is_bid, side.back());
            side.pop_back();
            level_it = find_level(side, is_bid, price);
        }

        Level new_level {
            .price = price,
            .qty = qty,
            .depleted_qty = 0.0,
        };
        acc_add(is_bid, new_level);
        side.insert(level_it, new_level);
    }

    void deplete_level(bool is_bid, double price, double consumed_qty) noexcept {
        if (price <= 0.0 || consumed_qty <= 0.0) {
            return;
        }

        std::vector<Level>& side = levels_for(is_bid);
        auto level_it = find_level(side, is_bid, price);
        if (level_it == side.end() || level_it->price != price) {
            return;
        }

        level_it->depleted_qty += consumed_qty;
        if (is_bid) {
            bid_depleted_qty_ += consumed_qty;
            bid_depleted_notional_ += consumed_qty * price;
        } else {
            ask_depleted_qty_ += consumed_qty;
            ask_depleted_notional_ += consumed_qty * price;
        }
    }

    template <typename PriceContainer>
    void remove_levels_not_in(bool is_bid, const PriceContainer& prices) {
        std::vector<Level>& side = levels_for(is_bid);
        auto level_it = side.begin();
        while (level_it != side.end()) {
            bool found = false;
            for (const double price : prices) {
                if (price > 0.0 && price == level_it->price) {
                    found = true;
                    break;
                }
            }

            if (found) {
                ++level_it;
            } else {
                acc_subtract(is_bid, *level_it);
                level_it = side.erase(level_it);
            }
        }
    }

    [[nodiscard]] double effective_qty(bool is_bid, double price) const noexcept {
        if (price <= 0.0) {
            return 0.0;
        }

        const std::vector<Level>& side = levels_for(is_bid);
        auto level_it = find_level(side, is_bid, price);
        if (level_it == side.end() || level_it->price != price) {
            return 0.0;
        }
        return level_it->effective_qty();
    }

    [[nodiscard]] const std::vector<Level>& bids() const noexcept {
        return bids_;
    }

    [[nodiscard]] const std::vector<Level>& asks() const noexcept {
        return asks_;
    }

    [[nodiscard]] std::size_t max_levels_per_side() const noexcept {
        return max_levels_per_side_;
    }

    [[nodiscard]] double best_bid() const noexcept {
        return bids_.empty() ? 0.0 : bids_.front().price;
    }

    [[nodiscard]] double best_ask() const noexcept {
        return asks_.empty() ? 0.0 : asks_.front().price;
    }

    // --- O(1) accumulator accessors ---

    [[nodiscard]] double bid_total_qty() const noexcept { return bid_total_qty_; }
    [[nodiscard]] double ask_total_qty() const noexcept { return ask_total_qty_; }
    [[nodiscard]] double bid_total_notional() const noexcept { return bid_total_notional_; }
    [[nodiscard]] double ask_total_notional() const noexcept { return ask_total_notional_; }

    [[nodiscard]] double bid_effective_qty() const noexcept {
        const double v = bid_total_qty_ - bid_depleted_qty_;
        return v > 0.0 ? v : 0.0;
    }
    [[nodiscard]] double ask_effective_qty() const noexcept {
        const double v = ask_total_qty_ - ask_depleted_qty_;
        return v > 0.0 ? v : 0.0;
    }
    [[nodiscard]] double bid_effective_notional() const noexcept {
        const double v = bid_total_notional_ - bid_depleted_notional_;
        return v > 0.0 ? v : 0.0;
    }
    [[nodiscard]] double ask_effective_notional() const noexcept {
        const double v = ask_total_notional_ - ask_depleted_notional_;
        return v > 0.0 ? v : 0.0;
    }

private:
    // --- Accumulator helpers ---

    void acc_add(bool is_bid, const Level& level) noexcept {
        if (is_bid) {
            bid_total_qty_ += level.qty;
            bid_total_notional_ += level.price * level.qty;
            bid_depleted_qty_ += level.depleted_qty;
            bid_depleted_notional_ += level.price * level.depleted_qty;
        } else {
            ask_total_qty_ += level.qty;
            ask_total_notional_ += level.price * level.qty;
            ask_depleted_qty_ += level.depleted_qty;
            ask_depleted_notional_ += level.price * level.depleted_qty;
        }
    }

    void acc_subtract(bool is_bid, const Level& level) noexcept {
        if (is_bid) {
            bid_total_qty_ -= level.qty;
            bid_total_notional_ -= level.price * level.qty;
            bid_depleted_qty_ -= level.depleted_qty;
            bid_depleted_notional_ -= level.price * level.depleted_qty;
        } else {
            ask_total_qty_ -= level.qty;
            ask_total_notional_ -= level.price * level.qty;
            ask_depleted_qty_ -= level.depleted_qty;
            ask_depleted_notional_ -= level.price * level.depleted_qty;
        }
    }

    [[nodiscard]] std::vector<Level>& levels_for(bool is_bid) noexcept {
        return is_bid ? bids_ : asks_;
    }

    [[nodiscard]] const std::vector<Level>& levels_for(bool is_bid) const noexcept {
        return is_bid ? bids_ : asks_;
    }

    [[nodiscard]] static LevelIterator find_level(std::vector<Level>& side, bool is_bid, double price) noexcept {
        if (is_bid) {
            return std::lower_bound(
                side.begin(),
                side.end(),
                price,
                [](const Level& level, double target_price) noexcept {
                    return level.price > target_price;
                }
            );
        }

        return std::lower_bound(
            side.begin(),
            side.end(),
            price,
            [](const Level& level, double target_price) noexcept {
                return level.price < target_price;
            }
        );
    }

    [[nodiscard]] static ConstLevelIterator find_level(
        const std::vector<Level>& side,
        bool is_bid,
        double price
    ) noexcept {
        if (is_bid) {
            return std::lower_bound(
                side.begin(),
                side.end(),
                price,
                [](const Level& level, double target_price) noexcept {
                    return level.price > target_price;
                }
            );
        }

        return std::lower_bound(
            side.begin(),
            side.end(),
            price,
            [](const Level& level, double target_price) noexcept {
                return level.price < target_price;
            }
        );
    }

    [[nodiscard]] static bool is_deeper_or_equal(bool is_bid, double price, double tail_price) noexcept {
        return is_bid ? price <= tail_price : price >= tail_price;
    }

    void trim_to_capacity(bool is_bid, std::vector<Level>& side) {
        while (side.size() > max_levels_per_side_) {
            acc_subtract(is_bid, side.back());
            side.pop_back();
        }
    }

    std::size_t max_levels_per_side_ {};
    std::vector<Level> bids_ {};
    std::vector<Level> asks_ {};

    // Incremental accumulators — maintained in O(1) per update
    double bid_total_qty_ {};
    double ask_total_qty_ {};
    double bid_total_notional_ {};
    double ask_total_notional_ {};
    double bid_depleted_qty_ {};
    double ask_depleted_qty_ {};
    double bid_depleted_notional_ {};
    double ask_depleted_notional_ {};
};

}  // namespace lob
