#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "lob/csv_parser.hpp"
#include "lob/order_book.hpp"

namespace {

[[nodiscard]] std::string FormatVirtualElapsed(std::uint64_t elapsed_milliseconds) {
    const std::uint64_t total_seconds = elapsed_milliseconds / 1'000U;
    const std::uint64_t milliseconds = elapsed_milliseconds % 1'000U;
    const std::uint64_t seconds = total_seconds % 60U;
    const std::uint64_t total_minutes = total_seconds / 60U;
    const std::uint64_t minutes = total_minutes % 60U;
    const std::uint64_t total_hours = total_minutes / 60U;
    const std::uint64_t hours = total_hours % 24U;
    const std::uint64_t days = total_hours / 24U;

    std::ostringstream stream {};
    if (days > 0U) {
        stream << days << "d ";
    }

    stream << std::setfill('0')
           << std::setw(2) << hours << ':'
           << std::setw(2) << minutes << ':'
           << std::setw(2) << seconds << '.'
           << std::setw(3) << milliseconds;
    return stream.str();
}

int RunBacktest(const std::filesystem::path& csv_path) {
    lob::CsvParser parser {};
    lob::OrderBook order_book {};

    const auto wall_start = std::chrono::steady_clock::now();
    const std::clock_t cpu_start = std::clock();
    const lob::CsvParser::ReplayStats replay_stats = parser.replay_file(csv_path, order_book);

    const std::clock_t cpu_end = std::clock();
    const auto wall_end = std::chrono::steady_clock::now();

    const double cpu_seconds = static_cast<double>(cpu_end - cpu_start) / static_cast<double>(CLOCKS_PER_SEC);
    const std::chrono::duration<double> wall_seconds = wall_end - wall_start;
    const double events_per_second = cpu_seconds > 0.0
        ? static_cast<double>(replay_stats.orders_processed) / cpu_seconds
        : 0.0;
    const std::uint64_t virtual_elapsed = replay_stats.last_timestamp >= replay_stats.first_timestamp
        ? replay_stats.last_timestamp - replay_stats.first_timestamp
        : 0U;

    std::cout << "Total Orders Processed: " << replay_stats.orders_processed << '\n';
    std::cout << "Generated Trades: " << replay_stats.generated_trades << '\n';
    std::cout << "Virtual Market Time Elapsed: " << FormatVirtualElapsed(virtual_elapsed) << '\n';
    std::cout << "Actual CPU Time Taken: " << std::fixed << std::setprecision(6) << cpu_seconds << " seconds\n";
    std::cout << "Wall Clock Time Taken: " << std::fixed << std::setprecision(6) << wall_seconds.count() << " seconds\n";
    std::cout << "EPS: " << std::fixed << std::setprecision(2) << events_per_second << '\n';

    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        if (argc != 2) {
            std::cerr << "Usage: lob_backtest <binance_trades.csv>" << '\n';
            return 1;
        }

        return RunBacktest(argv[1]);
    } catch (const std::exception& exception) {
        std::cerr << "Backtest failed: " << exception.what() << '\n';
        return 1;
    }
}
