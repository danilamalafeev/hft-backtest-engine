#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>

#include "lob/order.hpp"

namespace lob {

class OrderBook;

class CsvParser {
public:
    using OrderHandler = std::function<void(const Order&)>;

    struct ReplayStats {
        std::uint64_t orders_processed {};
        std::uint64_t generated_trades {};
        std::uint64_t first_timestamp {};
        std::uint64_t last_timestamp {};
    };

    CsvParser() = default;
    ~CsvParser() = default;

    [[nodiscard]] std::uint64_t parse_file(const std::filesystem::path& file_path, const OrderHandler& handler) const;
    [[nodiscard]] std::uint64_t process_file(const std::filesystem::path& file_path, OrderBook& order_book) const;
    [[nodiscard]] ReplayStats replay_file(const std::filesystem::path& file_path, OrderBook& order_book) const;
};

}  // namespace lob
