#pragma once

#include <array>
#include <cstdint>

#include "lob/event.hpp"

namespace lob {

struct L2Depth5Event {
    static constexpr std::size_t Depth = 5U;

    std::uint64_t timestamp {};
    AssetID pair_id {};
    std::array<double, Depth> bid_prices {};
    std::array<double, Depth> bid_qty {};
    std::array<double, Depth> ask_prices {};
    std::array<double, Depth> ask_qty {};
};

}  // namespace lob
