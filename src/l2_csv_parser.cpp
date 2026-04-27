#include "lob/l2_csv_parser.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace lob {
namespace {

[[nodiscard]] inline bool IsDigit(char value) noexcept {
    return value >= '0' && value <= '9';
}

[[nodiscard]] inline bool LooksLikeHeader(const char* cursor, const char* end) noexcept {
    return cursor < end && !IsDigit(*cursor);
}

inline void SkipField(const char*& cursor, const char* end) noexcept {
    while (cursor < end && *cursor != ',' && *cursor != '\n' && *cursor != '\r') {
        ++cursor;
    }
}

inline void SkipLine(const char*& cursor, const char* end) noexcept {
    while (cursor < end && *cursor != '\n') {
        ++cursor;
    }
    if (cursor < end && *cursor == '\n') {
        ++cursor;
    }
}

inline void ExpectComma(const char*& cursor, const char* end) {
    if (cursor >= end || *cursor != ',') {
        throw std::runtime_error("Malformed L2 CSV row: expected comma delimiter");
    }
    ++cursor;
}

inline void ConsumeRecordEnd(const char*& cursor, const char* end) {
    if (cursor < end && *cursor == '\r') {
        ++cursor;
    }
    if (cursor < end && *cursor == '\n') {
        ++cursor;
    } else if (cursor < end) {
        throw std::runtime_error("Malformed L2 CSV row: expected newline");
    }
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

    std::uint64_t fractional_part = 0U;
    std::uint64_t fractional_scale = 1U;
    if (cursor < end && *cursor == '.') {
        ++cursor;
        while (cursor < end && IsDigit(*cursor)) {
            fractional_part = (fractional_part * 10U) + static_cast<std::uint64_t>(*cursor - '0');
            fractional_scale *= 10U;
            ++cursor;
        }
    }

    return static_cast<double>(integer_part) +
           (static_cast<double>(fractional_part) / static_cast<double>(fractional_scale));
}

}  // namespace

L2CsvParser::L2CsvParser(const std::filesystem::path& file_path) {
    map_file(file_path);
    parse_next();
}

L2CsvParser::~L2CsvParser() {
    close_mapping();
}

L2CsvParser::L2CsvParser(L2CsvParser&& other) noexcept
    : file_descriptor_(other.file_descriptor_),
      mapping_(other.mapping_),
      size_(other.size_),
      cursor_(other.cursor_),
      end_(other.end_),
      next_event_(other.next_event_),
      has_next_(other.has_next_) {
    other.file_descriptor_ = -1;
    other.mapping_ = nullptr;
    other.size_ = 0U;
    other.cursor_ = nullptr;
    other.end_ = nullptr;
    other.next_event_ = L2BookEvent {};
    other.has_next_ = false;
}

L2CsvParser& L2CsvParser::operator=(L2CsvParser&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    close_mapping();
    file_descriptor_ = other.file_descriptor_;
    mapping_ = other.mapping_;
    size_ = other.size_;
    cursor_ = other.cursor_;
    end_ = other.end_;
    next_event_ = other.next_event_;
    has_next_ = other.has_next_;

    other.file_descriptor_ = -1;
    other.mapping_ = nullptr;
    other.size_ = 0U;
    other.cursor_ = nullptr;
    other.end_ = nullptr;
    other.next_event_ = L2BookEvent {};
    other.has_next_ = false;
    return *this;
}

L2BookEvent L2CsvParser::pop() {
    if (!has_next_) {
        throw std::out_of_range("L2CsvParser::pop called with no remaining events");
    }

    L2BookEvent event = next_event_;
    parse_next();
    return event;
}

void L2CsvParser::map_file(const std::filesystem::path& file_path) {
    file_descriptor_ = ::open(file_path.c_str(), O_RDONLY);
    if (file_descriptor_ == -1) {
        throw std::runtime_error("Unable to open L2 CSV file: " + file_path.string());
    }

    struct stat file_stat {};
    if (::fstat(file_descriptor_, &file_stat) == -1) {
        const int saved_errno = errno;
        close_mapping();
        throw std::runtime_error("Unable to stat L2 CSV file: " + std::to_string(saved_errno));
    }

    size_ = static_cast<std::size_t>(file_stat.st_size);
    if (size_ == 0U) {
        cursor_ = nullptr;
        end_ = nullptr;
        return;
    }

    mapping_ = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, file_descriptor_, 0);
    if (mapping_ == MAP_FAILED) {
        const int saved_errno = errno;
        mapping_ = nullptr;
        close_mapping();
        throw std::runtime_error("Unable to mmap L2 CSV file: " + std::to_string(saved_errno));
    }

    cursor_ = static_cast<const char*>(mapping_);
    end_ = cursor_ + size_;
    (void)::madvise(mapping_, size_, MADV_SEQUENTIAL);

    if (LooksLikeHeader(cursor_, end_)) {
        SkipLine(cursor_, end_);
    }
}

void L2CsvParser::close_mapping() noexcept {
    if (mapping_ != nullptr) {
        ::munmap(mapping_, size_);
        mapping_ = nullptr;
    }
    if (file_descriptor_ != -1) {
        ::close(file_descriptor_);
        file_descriptor_ = -1;
    }
    size_ = 0U;
    cursor_ = nullptr;
    end_ = nullptr;
    has_next_ = false;
}

void L2CsvParser::parse_next() {
    has_next_ = false;

    while (cursor_ != nullptr && cursor_ < end_) {
        if (*cursor_ == '\n' || *cursor_ == '\r') {
            SkipLine(cursor_, end_);
            continue;
        }

        L2BookEvent event {};
        event.timestamp = ParseUnsignedField(cursor_, end_);
        ExpectComma(cursor_, end_);
        event.bid_price = ParseDoubleField(cursor_, end_);
        ExpectComma(cursor_, end_);
        event.bid_qty = ParseDoubleField(cursor_, end_);
        ExpectComma(cursor_, end_);
        event.ask_price = ParseDoubleField(cursor_, end_);
        ExpectComma(cursor_, end_);
        event.ask_qty = ParseDoubleField(cursor_, end_);
        SkipField(cursor_, end_);
        ConsumeRecordEnd(cursor_, end_);

        next_event_ = event;
        has_next_ = true;
        return;
    }
}

}  // namespace lob
