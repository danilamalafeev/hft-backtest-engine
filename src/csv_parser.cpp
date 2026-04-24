#include "lob/csv_parser.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "lob/order_book.hpp"

namespace lob {
namespace {

constexpr std::uint64_t kQuantityScale = 100'000'000U;

[[nodiscard]] std::string_view Trim(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r')) {
        value.remove_prefix(1);
    }

    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) {
        value.remove_suffix(1);
    }

    return value;
}

[[nodiscard]] std::uint64_t ParseUnsigned(std::string_view token, std::uint64_t line_number, const char* field_name) {
    token = Trim(token);
    std::uint64_t value = 0U;
    const auto* begin = token.data();
    const auto* end = token.data() + token.size();
    const auto [parse_end, error_code] = std::from_chars(begin, end, value);
    if (error_code != std::errc {} || parse_end != end) {
        throw std::runtime_error(std::string("Invalid ") + field_name + " at line " + std::to_string(line_number));
    }

    return value;
}

[[nodiscard]] double ParseDouble(std::string_view token, std::uint64_t line_number, const char* field_name) {
    token = Trim(token);
    const std::string buffer {token};
    std::size_t parsed_characters = 0U;
    const double value = std::stod(buffer, &parsed_characters);
    if (parsed_characters != buffer.size()) {
        throw std::runtime_error(std::string("Invalid ") + field_name + " at line " + std::to_string(line_number));
    }

    return value;
}

[[nodiscard]] std::uint64_t ParseScaledQuantity(std::string_view token, std::uint64_t line_number) {
    const double quantity = ParseDouble(token, line_number, "qty");
    if (quantity < 0.0) {
        throw std::runtime_error("Negative qty at line " + std::to_string(line_number));
    }

    return static_cast<std::uint64_t>(std::llround(quantity * static_cast<double>(kQuantityScale)));
}

[[nodiscard]] Side ParseIncomingSide(std::string_view token, std::uint64_t line_number) {
    token = Trim(token);
    if (token == "true" || token == "True" || token == "TRUE" || token == "1") {
        return Side::Sell;
    }

    if (token == "false" || token == "False" || token == "FALSE" || token == "0") {
        return Side::Buy;
    }

    throw std::runtime_error("Invalid is_buyer_maker value at line " + std::to_string(line_number));
}

[[nodiscard]] Order ParseBinanceTradeLine(const std::string& line, std::uint64_t line_number) {
    const auto first_delimiter = line.find(',');
    const auto second_delimiter = line.find(',', first_delimiter == std::string::npos ? first_delimiter : first_delimiter + 1U);
    const auto third_delimiter = line.find(',', second_delimiter == std::string::npos ? second_delimiter : second_delimiter + 1U);
    const auto fourth_delimiter = line.find(',', third_delimiter == std::string::npos ? third_delimiter : third_delimiter + 1U);
    const auto fifth_delimiter = line.find(',', fourth_delimiter == std::string::npos ? fourth_delimiter : fourth_delimiter + 1U);
    const auto sixth_delimiter = line.find(',', fifth_delimiter == std::string::npos ? fifth_delimiter : fifth_delimiter + 1U);

    if (first_delimiter == std::string::npos || second_delimiter == std::string::npos ||
        third_delimiter == std::string::npos || fourth_delimiter == std::string::npos ||
        fifth_delimiter == std::string::npos || sixth_delimiter == std::string::npos) {
        throw std::runtime_error("Malformed Binance CSV row at line " + std::to_string(line_number));
    }

    const std::string_view trade_id_token {line.data(), first_delimiter};
    const std::string_view price_token {line.data() + first_delimiter + 1U, second_delimiter - first_delimiter - 1U};
    const std::string_view qty_token {line.data() + second_delimiter + 1U, third_delimiter - second_delimiter - 1U};
    const std::string_view time_token {line.data() + fourth_delimiter + 1U, fifth_delimiter - fourth_delimiter - 1U};
    const std::string_view is_buyer_maker_token {line.data() + fifth_delimiter + 1U, sixth_delimiter - fifth_delimiter - 1U};

    Order order {};
    order.id = ParseUnsigned(trade_id_token, line_number, "trade_id");
    order.price = ParseDouble(price_token, line_number, "price");
    order.quantity = ParseScaledQuantity(qty_token, line_number);
    order.side = ParseIncomingSide(is_buyer_maker_token, line_number);
    order.timestamp = ParseUnsigned(time_token, line_number, "time");
    return order;
}

}  // namespace

std::uint64_t CsvParser::parse_file(const std::filesystem::path& file_path, const OrderHandler& handler) const {
    if (!handler) {
        throw std::invalid_argument("CSV parser requires a valid order handler");
    }

    std::ifstream input_stream {file_path};
    if (!input_stream.is_open()) {
        throw std::runtime_error("Unable to open CSV file: " + file_path.string());
    }

    std::string line {};
    std::uint64_t parsed_orders = 0U;
    std::uint64_t file_line_number = 0U;

    while (std::getline(input_stream, line)) {
        ++file_line_number;
        if (line.empty()) {
            continue;
        }

        handler(ParseBinanceTradeLine(line, file_line_number));
        ++parsed_orders;
    }

    return parsed_orders;
}

std::uint64_t CsvParser::process_file(const std::filesystem::path& file_path, OrderBook& order_book) const {
    return parse_file(file_path, [&order_book](const Order& order) {
        const auto trades = order_book.process_order(order);
        (void)trades;
    });
}

}  // namespace lob
