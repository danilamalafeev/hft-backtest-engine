#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

#include "lob/event_l2_update.hpp"

namespace lob {

class L2UpdateCsvParser {
public:
    L2UpdateCsvParser() = default;
    explicit L2UpdateCsvParser(const std::filesystem::path& file_path);
    ~L2UpdateCsvParser();

    L2UpdateCsvParser(const L2UpdateCsvParser&) = delete;
    L2UpdateCsvParser& operator=(const L2UpdateCsvParser&) = delete;
    L2UpdateCsvParser(L2UpdateCsvParser&& other) noexcept;
    L2UpdateCsvParser& operator=(L2UpdateCsvParser&& other) noexcept;

    [[nodiscard]] bool has_next() const noexcept {
        return has_next_;
    }

    [[nodiscard]] std::uint64_t peek_time() const noexcept {
        return next_event_.timestamp_ns;
    }

    [[nodiscard]] const L2UpdateEvent& peek() const noexcept {
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
    L2UpdateEvent next_event_ {};
    bool has_next_ {};
};

}  // namespace lob
