#pragma once

#include <cmath>
#include <cstdint>

#include "lob/event.hpp"
#include "lob/order.hpp"

namespace lob {

enum class Currency : std::uint8_t {
    USDT,
    BTC,
    ETH
};

struct Wallet {
    double usdt {};
    double btc {};
    double eth {};

    void add_balance(Currency currency, double delta) noexcept {
        switch (currency) {
            case Currency::USDT:
                usdt += delta;
                break;
            case Currency::BTC:
                btc += delta;
                break;
            case Currency::ETH:
                eth += delta;
                break;
        }
    }

    [[nodiscard]] double balance(Currency currency) const noexcept {
        switch (currency) {
            case Currency::USDT:
                return usdt;
            case Currency::BTC:
                return btc;
            case Currency::ETH:
                return eth;
        }
        return 0.0;
    }

    void apply_spot_fill(
        AssetID asset_id,
        Side side,
        double base_quantity,
        double price,
        double fee_bps
    ) noexcept {
        const Currency base_currency = base_currency_for_asset(asset_id);
        const Currency quote_currency = quote_currency_for_asset(asset_id);
        const double quote_notional = base_quantity * price;
        const double fee_rate = fee_bps * 0.0001;

        if (side == Side::Buy) {
            add_balance(base_currency, base_quantity - (base_quantity * fee_rate));
            add_balance(quote_currency, -quote_notional);
            return;
        }

        add_balance(base_currency, -base_quantity);
        add_balance(quote_currency, quote_notional - (quote_notional * fee_rate));
    }

    [[nodiscard]] double get_total_inventory_risk(double btc_usdt_mid, double eth_usdt_mid) const noexcept {
        return std::abs(btc) * btc_usdt_mid + std::abs(eth) * eth_usdt_mid;
    }

    [[nodiscard]] double mark_to_market_nav(double btc_usdt_mid, double eth_usdt_mid) const noexcept {
        return usdt + (btc * btc_usdt_mid) + (eth * eth_usdt_mid);
    }

    [[nodiscard]] static Currency base_currency_for_asset(AssetID asset_id) noexcept {
        switch (asset_id) {
            case 0U:
                return Currency::BTC;
            case 1U:
            case 2U:
                return Currency::ETH;
            default:
                return Currency::USDT;
        }
    }

    [[nodiscard]] static Currency quote_currency_for_asset(AssetID asset_id) noexcept {
        switch (asset_id) {
            case 0U:
            case 1U:
                return Currency::USDT;
            case 2U:
                return Currency::BTC;
            default:
                return Currency::USDT;
        }
    }
};

}  // namespace lob
