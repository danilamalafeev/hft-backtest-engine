#include "lob/csv_parser.hpp"

#include <cstddef>
#include <cstdint>
#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "lob/order_book.hpp"

namespace lob {
namespace {

constexpr std::uint64_t kQuantityScale = 100'000'000U;

class MappedFile {
public:
    explicit MappedFile(const std::filesystem::path& file_path) {
        file_descriptor_ = ::open(file_path.c_str(), O_RDONLY);
        if (file_descriptor_ == -1) {
            throw std::runtime_error("Unable to open CSV file: " + file_path.string());
        }

        struct stat file_stat {};
        if (::fstat(file_descriptor_, &file_stat) == -1) {
            const int saved_errno = errno;
            ::close(file_descriptor_);
            file_descriptor_ = -1;
            throw std::runtime_error("Unable to stat CSV file: " + std::to_string(saved_errno));
        }

        size_ = static_cast<std::size_t>(file_stat.st_size);
        if (size_ == 0U) {
            return;
        }

        mapping_ = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, file_descriptor_, 0);
        if (mapping_ == MAP_FAILED) {
            const int saved_errno = errno;
            mapping_ = nullptr;
            ::close(file_descriptor_);
            file_descriptor_ = -1;
            throw std::runtime_error("Unable to mmap CSV file: " + std::to_string(saved_errno));
        }
    }

    ~MappedFile() {
        if (mapping_ != nullptr) {
            ::munmap(mapping_, size_);
        }

        if (file_descriptor_ != -1) {
            ::close(file_descriptor_);
        }
    }

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    MappedFile(MappedFile&&) = delete;
    MappedFile& operator=(MappedFile&&) = delete;

    [[nodiscard]] const char* data() const noexcept {
        return static_cast<const char*>(mapping_);
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

private:
    int file_descriptor_ {-1};
    void* mapping_ {nullptr};
    std::size_t size_ {0U};
};

[[nodiscard]] inline bool IsDigit(const char value) noexcept {
    return value >= '0' && value <= '9';
}

[[nodiscard]] inline std::uint64_t ParseUnsignedField(const char*& cursor, const char* end) {
    std::uint64_t value = 0U;
    while (cursor < end && IsDigit(*cursor)) {
        value = (value * 10U) + static_cast<std::uint64_t>(*cursor - '0');
        ++cursor;
    }

    return value;
}

[[nodiscard]] inline double ParseDoubleField(const char*& cursor, const char* end) {
    std::uint64_t integer_part = 0U;
    while (cursor < end && IsDigit(*cursor)) {
        integer_part = (integer_part * 10U) + static_cast<std::uint64_t>(*cursor - '0');
        ++cursor;
    }

    double value = static_cast<double>(integer_part);
    if (cursor < end && *cursor == '.') {
        ++cursor;

        double fractional_scale = 0.1;
        while (cursor < end && IsDigit(*cursor)) {
            value += static_cast<double>(*cursor - '0') * fractional_scale;
            fractional_scale *= 0.1;
            ++cursor;
        }
    }

    return value;
}

[[nodiscard]] inline std::uint64_t ParseScaledQuantityField(const char*& cursor, const char* end) {
    std::uint64_t integer_part = 0U;
    while (cursor < end && IsDigit(*cursor)) {
        integer_part = (integer_part * 10U) + static_cast<std::uint64_t>(*cursor - '0');
        ++cursor;
    }

    std::uint64_t scaled_quantity = integer_part * kQuantityScale;
    if (cursor < end && *cursor == '.') {
        ++cursor;

        std::uint64_t multiplier = kQuantityScale / 10U;
        while (cursor < end && IsDigit(*cursor) && multiplier > 0U) {
            scaled_quantity += static_cast<std::uint64_t>(*cursor - '0') * multiplier;
            multiplier /= 10U;
            ++cursor;
        }

        while (cursor < end && IsDigit(*cursor)) {
            ++cursor;
        }
    }

    return scaled_quantity;
}

[[nodiscard]] inline Side ParseIncomingSideField(const char*& cursor, const char* end) {
    if (cursor >= end) {
        throw std::runtime_error("Unexpected end of file while parsing side");
    }

    const char first_character = *cursor;
    while (cursor < end && *cursor != ',' && *cursor != '\n' && *cursor != '\r') {
        ++cursor;
    }

    if (first_character == 't' || first_character == 'T' || first_character == '1') {
        return Side::Sell;
    }

    if (first_character == 'f' || first_character == 'F' || first_character == '0') {
        return Side::Buy;
    }

    throw std::runtime_error("Invalid is_buyer_maker field");
}

inline void ExpectComma(const char*& cursor, const char* end) {
    if (cursor >= end || *cursor != ',') {
        throw std::runtime_error("Malformed CSV row: expected comma delimiter");
    }

    ++cursor;
}

inline void SkipField(const char*& cursor, const char* end) noexcept {
    while (cursor < end && *cursor != ',' && *cursor != '\n' && *cursor != '\r') {
        ++cursor;
    }
}

inline void ConsumeRecordEnd(const char*& cursor, const char* end) {
    if (cursor < end && *cursor == '\r') {
        ++cursor;
    }

    if (cursor < end && *cursor == '\n') {
        ++cursor;
    } else if (cursor < end) {
        throw std::runtime_error("Malformed CSV row: expected newline");
    }
}

inline void SkipEmptyLine(const char*& cursor, const char* end) noexcept {
    if (cursor < end && *cursor == '\r') {
        ++cursor;
    }

    if (cursor < end && *cursor == '\n') {
        ++cursor;
    }
}

template <typename Handler>
std::uint64_t ParseMappedBinanceFile(const std::filesystem::path& file_path, Handler&& handler) {
    MappedFile mapped_file {file_path};
    if (mapped_file.size() == 0U) {
        return 0U;
    }

    const char* cursor = mapped_file.data();
    const char* const end = cursor + mapped_file.size();

    std::uint64_t parsed_orders = 0U;

    while (cursor < end) {
        if (*cursor == '\n' || *cursor == '\r') {
            SkipEmptyLine(cursor, end);
            continue;
        }

        Order order {};
        order.id = ParseUnsignedField(cursor, end);
        ExpectComma(cursor, end);
        order.price = ParseDoubleField(cursor, end);
        ExpectComma(cursor, end);
        order.quantity = ParseScaledQuantityField(cursor, end);
        ExpectComma(cursor, end);
        SkipField(cursor, end);
        ExpectComma(cursor, end);
        order.timestamp = ParseUnsignedField(cursor, end);
        ExpectComma(cursor, end);
        order.side = ParseIncomingSideField(cursor, end);
        ExpectComma(cursor, end);
        SkipField(cursor, end);
        ConsumeRecordEnd(cursor, end);

        handler(order);
        ++parsed_orders;
    }

    return parsed_orders;
}

}  // namespace

std::uint64_t CsvParser::parse_file(const std::filesystem::path& file_path, const OrderHandler& handler) const {
    if (!handler) {
        throw std::invalid_argument("CSV parser requires a valid order handler");
    }

    return ParseMappedBinanceFile(file_path, [&handler](const Order& order) {
        handler(order);
    });
}

std::uint64_t CsvParser::process_file(const std::filesystem::path& file_path, OrderBook& order_book) const {
    return ParseMappedBinanceFile(file_path, [&order_book](const Order& order) {
        const auto trades = order_book.process_order(order);
        (void)trades;
    });
}

CsvParser::ReplayStats CsvParser::replay_file(const std::filesystem::path& file_path, OrderBook& order_book) const {
    ReplayStats replay_stats {};

    replay_stats.orders_processed = ParseMappedBinanceFile(file_path, [&order_book, &replay_stats](const Order& order) {
        if (replay_stats.first_timestamp == 0U) {
            replay_stats.first_timestamp = order.timestamp;
        }

        replay_stats.last_timestamp = order.timestamp;
        replay_stats.generated_trades += static_cast<std::uint64_t>(order_book.process_order(order).size());
    });

    return replay_stats;
}

}  // namespace lob
