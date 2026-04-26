#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <queue>
#include <stdexcept>
#include <utility>
#include <vector>

#include "lob/csv_parser.hpp"
#include "lob/event.hpp"

namespace lob {

template <typename Parser>
concept StreamingParser = requires(Parser parser, const Parser& const_parser) {
    { const_parser.has_next() } noexcept -> std::same_as<bool>;
    { const_parser.peek_time() } noexcept -> std::same_as<std::uint64_t>;
    { parser.pop() } -> std::same_as<Event>;
};

template <std::size_t N, typename Parser = CsvParser>
requires StreamingParser<Parser>
class EventMerger {
    static_assert(N > 0U, "EventMerger requires at least one stream");
    static_assert(N <= static_cast<std::size_t>(std::numeric_limits<AssetID>::max()) + 1U,
                  "EventMerger asset_id does not fit in AssetID");

public:
    using ParserArray = std::array<Parser, N>;

    EventMerger() : EventMerger(ParserArray {}) {}

    explicit EventMerger(ParserArray parsers)
        : parsers_(std::move(parsers)),
          heap_(HeapCompare {}, MakeReservedHeapStorage()) {
        if constexpr (N > 4U) {
            InitializeHeap();
        }
    }

    [[nodiscard]] inline bool has_next() const noexcept {
        if constexpr (N <= 4U) {
            for (const Parser& parser : parsers_) {
                if (parser.has_next()) {
                    return true;
                }
            }
            return false;
        } else {
            return !heap_.empty();
        }
    }

    [[nodiscard]] inline Event get_next() {
        if constexpr (N <= 4U) {
            return GetNextFastPath();
        } else {
            return next_dynamic();
        }
    }

private:
    struct HeapNode {
        std::uint64_t next_timestamp {};
        AssetID asset_id {};
    };

    struct HeapCompare {
        [[nodiscard]] inline bool operator()(const HeapNode& lhs, const HeapNode& rhs) const noexcept {
            if (lhs.next_timestamp != rhs.next_timestamp) {
                return lhs.next_timestamp > rhs.next_timestamp;
            }
            return lhs.asset_id > rhs.asset_id;
        }
    };

    using HeapStorage = std::vector<HeapNode>;
    using Heap = std::priority_queue<HeapNode, HeapStorage, HeapCompare>;

    [[nodiscard]] static HeapStorage MakeReservedHeapStorage() {
        HeapStorage storage {};
        storage.reserve(N);
        return storage;
    }

    inline void InitializeHeap() {
        for (std::size_t index = 0U; index < N; ++index) {
            if (parsers_[index].has_next()) [[likely]] {
                heap_.push(HeapNode {
                    .next_timestamp = parsers_[index].peek_time(),
                    .asset_id = static_cast<AssetID>(index),
                });
            }
        }
    }

    [[nodiscard]] inline Event GetNextFastPath() {
        std::uint64_t best_timestamp = std::numeric_limits<std::uint64_t>::max();
        std::size_t best_index = N;

        for (std::size_t index = 0U; index < N; ++index) {
            Parser& parser = parsers_[index];
            if (!parser.has_next()) [[unlikely]] {
                continue;
            }

            const std::uint64_t timestamp = parser.peek_time();
            if (timestamp < best_timestamp) {
                best_timestamp = timestamp;
                best_index = index;
            }
        }

        if (best_index == N) {
            throw std::out_of_range("EventMerger::get_next called with no remaining events");
        }

        Event event = parsers_[best_index].pop();
        event.asset_id = static_cast<AssetID>(best_index);
        return event;
    }

    [[nodiscard]] inline Event next_dynamic() {
        if (heap_.empty()) [[unlikely]] {
            throw std::out_of_range("EventMerger::get_next called with no remaining events");
        }

        const HeapNode node = heap_.top();
        heap_.pop();

        Parser& parser = parsers_[node.asset_id];
        Event event = parser.pop();
        event.asset_id = node.asset_id;

        if (parser.has_next()) [[likely]] {
            heap_.push(HeapNode {
                .next_timestamp = parser.peek_time(),
                .asset_id = node.asset_id,
            });
        }

        return event;
    }

    ParserArray parsers_ {};
    Heap heap_;
};

}  // namespace lob
