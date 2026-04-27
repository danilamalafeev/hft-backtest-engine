#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

#include "lob/event_depth5.hpp"

namespace lob {

class L2Depth5CsvParser {
public:
    L2Depth5CsvParser() = default;
    explicit L2Depth5CsvParser(const std::filesystem::path& file_path);
    ~L2Depth5CsvParser();

    L2Depth5CsvParser(const L2Depth5CsvParser&) = delete;
    L2Depth5CsvParser& operator=(const L2Depth5CsvParser&) = delete;
    L2Depth5CsvParser(L2Depth5CsvParser&& other) noexcept;
    L2Depth5CsvParser& operator=(L2Depth5CsvParser&& other) noexcept;

    [[nodiscard]] bool has_next() const noexcept {
        return has_next_;
    }

    [[nodiscard]] std::uint64_t peek_time() const noexcept {
        return next_event_.timestamp;
    }

    [[nodiscard]] const L2Depth5Event& peek() const noexcept {
        return next_event_;
    }

    void advance();

private:
    void map_file(const std::filesystem::path& file_path);
    void close_mapping() noexcept;
    void parse_next();

    int file_descriptor_ {-1};
    void* mapping_ {nullptr};
    std::size_t size_ {};
    const char* cursor_ {nullptr};
    const char* end_ {nullptr};
    L2Depth5Event next_event_ {};
    bool has_next_ {};
    bool depth5_format_ {};
};

}  // namespace lob
