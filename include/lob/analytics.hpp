#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace lob {

struct BacktestAnalytics {
    double total_realized_pnl {};
    double sharpe_ratio {};
    double max_drawdown {};
    double max_drawdown_pct {};

    [[nodiscard]] static BacktestAnalytics analyze(
        const std::vector<double>& equity_curve,
        std::uint64_t sample_interval_ms = 1'000U
    ) {
        BacktestAnalytics analytics {};
        if (equity_curve.empty()) {
            return analytics;
        }

        analytics.total_realized_pnl = equity_curve.back() - equity_curve.front();

        double peak = equity_curve.front();
        for (const double equity : equity_curve) {
            if (equity > peak) {
                peak = equity;
            }

            const double drawdown = peak - equity;
            if (drawdown > analytics.max_drawdown) {
                analytics.max_drawdown = drawdown;
                analytics.max_drawdown_pct = peak > 0.0 ? drawdown / peak : 0.0;
            }
        }

        if (equity_curve.size() < 2U || sample_interval_ms == 0U) {
            return analytics;
        }

        double sum_returns = 0.0;
        double sum_squared_returns = 0.0;
        std::size_t return_count = 0U;

        for (std::size_t index = 1U; index < equity_curve.size(); ++index) {
            const double previous_equity = equity_curve[index - 1U];
            if (previous_equity == 0.0) {
                continue;
            }

            const double simple_return = (equity_curve[index] - previous_equity) / previous_equity;
            sum_returns += simple_return;
            sum_squared_returns += simple_return * simple_return;
            ++return_count;
        }

        if (return_count < 2U) {
            return analytics;
        }

        const double count = static_cast<double>(return_count);
        const double mean_return = sum_returns / count;
        const double variance = (sum_squared_returns - (sum_returns * sum_returns / count)) / (count - 1.0);
        if (variance <= 0.0) {
            return analytics;
        }

        constexpr double kMillisecondsPerYear = 365.0 * 24.0 * 60.0 * 60.0 * 1'000.0;
        const double samples_per_year = kMillisecondsPerYear / static_cast<double>(sample_interval_ms);
        analytics.sharpe_ratio = (mean_return / std::sqrt(variance)) * std::sqrt(samples_per_year);
        return analytics;
    }
};

}  // namespace lob
