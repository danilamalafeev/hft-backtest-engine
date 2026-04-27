#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

#include "lob/event.hpp"

namespace lob {

class L2CsvParser {
public:
    L2CsvParser() = default;
    explicit L2CsvParser(const std::filesystem::path& file_path);
    ~L2CsvParser();

    L2CsvParser(const L2CsvParser&) = delete;
    L2CsvParser& operator=(const L2CsvParser&) = delete;
    L2CsvParser(L2CsvParser&& other) noexcept;
    L2CsvParser& operator=(L2CsvParser&& other) noexcept;

    [[nodiscard]] bool has_next() const noexcept {
        return has_next_;
    }

    [[nodiscard]] std::uint64_t peek_time() const noexcept {
        return next_event_.timestamp;
    }

    [[nodiscard]] L2BookEvent pop();

private:
    void map_file(const std::filesystem::path& file_path);
    void close_mapping() noexcept;
    void parse_next();

    int file_descriptor_ {-1};
    void* mapping_ {nullptr};
    std::size_t size_ {};
    const char* cursor_ {nullptr};
    const char* end_ {nullptr};
    L2BookEvent next_event_ {};
    bool has_next_ {};
};

}  // namespace lob
