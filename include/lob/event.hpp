#pragma once

#include <cstdint>

#include "lob/order.hpp"

namespace lob {

using AssetID = std::uint16_t;

struct Event {
    std::uint64_t timestamp {};
    AssetID asset_id {};
    Order order {};
};

struct L2BookEvent {
    std::uint64_t timestamp {};
    AssetID pair_id {};
    double bid_price {};
    double bid_qty {};
    double ask_price {};
    double ask_qty {};
};

}  // namespace lob
