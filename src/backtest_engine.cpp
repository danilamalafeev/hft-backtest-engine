#include "lob/backtest_engine.hpp"

#include <utility>

namespace lob {
namespace {

constexpr double kQuantityScale = 100'000'000.0;

[[nodiscard]] double QuantityToUnits(std::uint64_t quantity) noexcept {
    return static_cast<double>(quantity) / kQuantityScale;
}

[[nodiscard]] double PositionToUnits(std::int64_t position) noexcept {
    return static_cast<double>(position) / kQuantityScale;
}

}  // namespace

BacktestEngine::BacktestEngine(Strategy& strategy)
    : BacktestEngine(strategy, Config {}) {}

BacktestEngine::BacktestEngine(Strategy& strategy, Config config)
    : strategy_(strategy),
      config_(config),
      cash_(config.initial_cash) {
    trade_buffer_.reserve(16U);
    pending_fills_.reserve(8U);
    own_orders_.reserve(256U);
}

BacktestEngine::Result BacktestEngine::run(const std::filesystem::path& file_path) {
    Result result {};
    result.initial_cash = config_.initial_cash;
    order_book_ = OrderBook {};
    own_orders_.clear();
    snapshot_ = MarketSnapshot {};
    next_strategy_order_id_ = 1ULL << 63U;
    next_equity_sample_timestamp_ = 0U;
    cash_ = config_.initial_cash;
    position_ = 0;
    equity_curve_.clear();
    equity_curve_.reserve(config_.equity_curve_reserve);

    strategy_.on_start(*this);

    result.replay_stats.orders_processed = parser_.parse_file(file_path, [this, &result](const Order& order) {
        if (result.replay_stats.first_timestamp == 0U) {
            result.replay_stats.first_timestamp = order.timestamp;
            next_equity_sample_timestamp_ = order.timestamp;
        }

        result.replay_stats.last_timestamp = order.timestamp;
        process_market_order(order);
        result.replay_stats.generated_trades += static_cast<std::uint64_t>(trade_buffer_.size());

        update_snapshot(order.timestamp, order.price);
        strategy_.on_tick(order_book_, *this);
        sample_equity(order.timestamp);
    });

    if (snapshot_.mid_price > 0.0) {
        equity_curve_.push_back(current_equity());
    }

    result.final_cash = cash_;
    result.final_position = position_;
    result.final_mid_price = snapshot_.mid_price;
    result.equity_curve = std::move(equity_curve_);
    return result;
}

std::uint64_t BacktestEngine::submit_order(
    Side side,
    double price,
    std::uint64_t quantity,
    std::uint64_t timestamp
) {
    const std::uint64_t order_id = next_strategy_order_id_++;
    if (quantity == 0U) {
        return order_id;
    }

    own_orders_.emplace(order_id, OwnOrderState {
        .side = side,
        .remaining_quantity = quantity,
    });

    order_book_.process_order(Order {
        .id = order_id,
        .price = price,
        .quantity = quantity,
        .side = side,
        .timestamp = timestamp,
    }, trade_buffer_);

    route_trades(trade_buffer_);

    const auto own_order_it = own_orders_.find(order_id);
    if (own_order_it != own_orders_.end() && own_order_it->second.remaining_quantity == 0U) {
        own_orders_.erase(own_order_it);
    }

    return order_id;
}

bool BacktestEngine::cancel_order(std::uint64_t order_id) {
    const bool canceled = order_book_.cancel_order(order_id);
    if (canceled) {
        own_orders_.erase(order_id);
    }
    return canceled;
}

std::uint64_t BacktestEngine::current_timestamp() const noexcept {
    return snapshot_.timestamp;
}

void BacktestEngine::process_market_order(const Order& order) {
    order_book_.process_order(order, trade_buffer_);
    route_trades(trade_buffer_);
}

void BacktestEngine::route_trades(const std::vector<Trade>& trades) {
    pending_fills_.clear();

    for (const Trade& trade : trades) {
        const bool own_buy = is_strategy_order(trade.buyer_id);
        const bool own_sell = is_strategy_order(trade.seller_id);

        if (own_buy) {
            pending_fills_.push_back(StrategyFill {
                .order_id = trade.buyer_id,
                .side = Side::Buy,
                .price = trade.price,
                .quantity = trade.quantity,
                .timestamp = trade.timestamp,
            });
        }

        if (own_sell) {
            pending_fills_.push_back(StrategyFill {
                .order_id = trade.seller_id,
                .side = Side::Sell,
                .price = trade.price,
                .quantity = trade.quantity,
                .timestamp = trade.timestamp,
            });
        }
    }

    for (const StrategyFill& fill : pending_fills_) {
        route_strategy_fill(fill);
    }
}

void BacktestEngine::route_strategy_fill(const StrategyFill& fill) {
    const double notional = fill.price * QuantityToUnits(fill.quantity);

    auto own_order_it = own_orders_.find(fill.order_id);
    if (own_order_it != own_orders_.end()) {
        own_order_it->second.remaining_quantity =
            own_order_it->second.remaining_quantity > fill.quantity
                ? own_order_it->second.remaining_quantity - fill.quantity
                : 0U;

        if (own_order_it->second.remaining_quantity == 0U) {
            own_orders_.erase(own_order_it);
        }
    }

    if (fill.side == Side::Buy) {
        cash_ -= notional;
        position_ += static_cast<std::int64_t>(fill.quantity);
    } else {
        cash_ += notional;
        position_ -= static_cast<std::int64_t>(fill.quantity);
    }

    strategy_.on_fill(fill, *this);
}

void BacktestEngine::sample_equity(std::uint64_t timestamp) {
    if (snapshot_.mid_price <= 0.0 || config_.equity_sample_interval_ms == 0U) {
        return;
    }

    while (timestamp >= next_equity_sample_timestamp_) {
        equity_curve_.push_back(current_equity());
        next_equity_sample_timestamp_ += config_.equity_sample_interval_ms;
    }
}

void BacktestEngine::update_snapshot(std::uint64_t timestamp, double fallback_price) {
    snapshot_.timestamp = timestamp;

    snapshot_.best_bid = order_book_.get_best_bid();
    snapshot_.best_ask = order_book_.get_best_ask();

    if (snapshot_.best_bid > 0.0 && snapshot_.best_ask > 0.0) {
        snapshot_.mid_price = (snapshot_.best_bid + snapshot_.best_ask) * 0.5;
    } else if (snapshot_.best_bid > 0.0) {
        snapshot_.mid_price = snapshot_.best_bid;
    } else if (snapshot_.best_ask > 0.0) {
        snapshot_.mid_price = snapshot_.best_ask;
    } else {
        snapshot_.mid_price = fallback_price;
    }
}

double BacktestEngine::current_equity() const noexcept {
    return cash_ + (PositionToUnits(position_) * snapshot_.mid_price);
}

bool BacktestEngine::is_strategy_order(std::uint64_t order_id) const noexcept {
    return own_orders_.find(order_id) != own_orders_.end();
}

}  // namespace lob
