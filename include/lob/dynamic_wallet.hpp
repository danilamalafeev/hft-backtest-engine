#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "lob/event.hpp"

namespace lob {

class DynamicWallet {
public:
    DynamicWallet() = default;

    void reset(std::size_t asset_count, AssetID quote_currency_id, double quote_balance) {
        balances_.assign(asset_count, 0.0);
        reserved_.assign(asset_count, 0.0);
        quote_currency_id_ = quote_currency_id;
        if (quote_currency_id_ >= balances_.size()) {
            throw std::out_of_range("DynamicWallet quote currency id is out of range");
        }
        balances_[quote_currency_id_] = quote_balance;
    }

    [[nodiscard]] double balance(AssetID asset) const {
        return balances_.at(asset);
    }

    [[nodiscard]] double reserved(AssetID asset) const {
        return reserved_.at(asset);
    }

    [[nodiscard]] double free_balance(AssetID asset) const {
        return balances_.at(asset) - reserved_.at(asset);
    }

    [[nodiscard]] const std::vector<double>& balances() const noexcept {
        return balances_;
    }

    [[nodiscard]] const std::vector<double>& reserved_balances() const noexcept {
        return reserved_;
    }

    [[nodiscard]] AssetID quote_currency_id() const noexcept {
        return quote_currency_id_;
    }

    void add_balance(AssetID asset, double quantity) noexcept {
        balances_[asset] += quantity;
    }

    void sub_balance(AssetID asset, double quantity) noexcept {
        balances_[asset] -= quantity;
    }

    [[nodiscard]] bool reserve_balance(AssetID asset, double quantity) noexcept {
        if (asset >= balances_.size() || quantity < 0.0) {
            return false;
        }
        if (free_balance(asset) + kEpsilon < quantity) {
            return false;
        }
        reserved_[asset] += quantity;
        return true;
    }

    void release_reserved(AssetID asset, double quantity) noexcept {
        if (asset >= reserved_.size() || quantity <= 0.0) {
            return;
        }
        reserved_[asset] -= quantity;
        if (reserved_[asset] < kEpsilon) {
            reserved_[asset] = 0.0;
        }
    }

    [[nodiscard]] bool consume_reserved(AssetID asset, double quantity) noexcept {
        if (asset >= balances_.size() || quantity < 0.0) {
            return false;
        }
        if (reserved_[asset] + kEpsilon < quantity || balances_[asset] + kEpsilon < quantity) {
            return false;
        }
        reserved_[asset] -= quantity;
        balances_[asset] -= quantity;
        if (reserved_[asset] < kEpsilon) {
            reserved_[asset] = 0.0;
        }
        return true;
    }

    [[nodiscard]] double apply_fill(AssetID asset, double quantity, double taker_fee_bps) noexcept {
        const double net_quantity = quantity * fee_multiplier(taker_fee_bps);
        add_balance(asset, net_quantity);
        return net_quantity;
    }

    template <typename EdgeContainer>
    [[nodiscard]] double get_total_inventory_risk(const EdgeContainer& edges, double taker_fee_bps) const noexcept {
        double risk = 0.0;
        for (AssetID asset = 0U; asset < balances_.size(); ++asset) {
            if (asset == quote_currency_id_) {
                continue;
            }

            const double quantity = std::abs(balances_[asset]);
            if (quantity <= 0.0) {
                continue;
            }

            risk += liquidation_value_to_quote(asset, quantity, edges, taker_fee_bps);
        }
        return risk;
    }

    template <typename EdgeContainer>
    [[nodiscard]] double mark_to_market_nav(const EdgeContainer& edges, double taker_fee_bps) const noexcept {
        double nav = quote_currency_id_ < balances_.size() ? balances_[quote_currency_id_] : 0.0;
        for (AssetID asset = 0U; asset < balances_.size(); ++asset) {
            if (asset == quote_currency_id_) {
                continue;
            }

            const double quantity = balances_[asset];
            if (quantity == 0.0) {
                continue;
            }

            const double value = liquidation_value_to_quote(asset, std::abs(quantity), edges, taker_fee_bps);
            nav += quantity > 0.0 ? value : -value;
        }
        return nav;
    }

private:
    static constexpr double kEpsilon = 1e-12;

    [[nodiscard]] static double fee_multiplier(double taker_fee_bps) noexcept {
        return 1.0 - (taker_fee_bps * 0.0001);
    }

    template <typename EdgeContainer>
    [[nodiscard]] double liquidation_value_to_quote(
        AssetID asset,
        double quantity,
        const EdgeContainer& edges,
        double taker_fee_bps
    ) const noexcept {
        if (asset == quote_currency_id_) {
            return quantity;
        }

        const double multiplier = fee_multiplier(taker_fee_bps);
        for (const auto& edge : edges) {
            if (edge.from == asset && edge.to == quote_currency_id_) {
                return quantity * edge.rate * multiplier;
            }
        }

        for (const auto& first : edges) {
            if (first.from != asset) {
                continue;
            }
            for (const auto& second : edges) {
                if (second.from == first.to && second.to == quote_currency_id_) {
                    return quantity * first.rate * multiplier * second.rate * multiplier;
                }
            }
        }

        return 0.0;
    }

    std::vector<double> balances_ {};
    std::vector<double> reserved_ {};
    AssetID quote_currency_id_ {};
};

}  // namespace lob
