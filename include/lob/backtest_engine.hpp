#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <vector>

#include "lob/csv_parser.hpp"
#include "lob/order_book.hpp"
#include "lob/strategy.hpp"

namespace lob {

class BacktestEngine final : public OrderGateway {
public:
    struct Config {
        double initial_cash {100'000'000.0};
        std::uint64_t equity_sample_interval_ms {1'000U};
        std::size_t equity_curve_reserve {100'000U};
        bool trace_enabled {};
        std::filesystem::path trace_path {"trace_log.csv"};
    };

    struct Result {
        CsvParser::ReplayStats replay_stats {};
        double initial_cash {};
        double final_cash {};
        std::int64_t final_position {};
        double final_mid_price {};
        std::vector<double> equity_curve {};
    };

    explicit BacktestEngine(Strategy& strategy);
    BacktestEngine(Strategy& strategy, Config config);

    [[nodiscard]] Result run(const std::filesystem::path& file_path);

    [[nodiscard]] std::uint64_t submit_order(
        Side side,
        double price,
        std::uint64_t quantity,
        std::uint64_t timestamp
    ) override;

    [[nodiscard]] bool cancel_order(std::uint64_t order_id) override;
    [[nodiscard]] std::uint64_t current_timestamp() const noexcept override;

private:
    struct OwnOrderState {
        Side side {Side::Buy};
        std::uint64_t remaining_quantity {};
    };

    void process_market_order(const Order& order);
    void route_trades(const std::vector<Trade>& trades);
    void route_strategy_fill(const StrategyFill& fill);
    void sample_equity(std::uint64_t timestamp);
    void update_snapshot(std::uint64_t timestamp, double fallback_price);
    void open_trace_log();
    void close_trace_log();
    void write_trace_row(std::uint64_t timestamp, int is_bot_trade, const char* trade_side);

    [[nodiscard]] double current_equity() const noexcept;
    [[nodiscard]] bool is_strategy_order(std::uint64_t order_id) const noexcept;

    Strategy& strategy_;
    Config config_ {};
    CsvParser parser_ {};
    OrderBook order_book_ {};
    MarketSnapshot snapshot_ {};
    std::vector<Trade> trade_buffer_ {};
    std::unordered_map<std::uint64_t, OwnOrderState> own_orders_ {};
    std::vector<StrategyFill> pending_fills_ {};
    std::vector<double> equity_curve_ {};
    std::ofstream trace_log_ {};
    std::uint64_t next_strategy_order_id_ {1ULL << 63U};
    std::uint64_t next_equity_sample_timestamp_ {};
    double cash_ {};
    std::int64_t position_ {};
};

}  // namespace lob
