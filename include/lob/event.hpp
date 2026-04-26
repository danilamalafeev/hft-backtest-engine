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

}  // namespace lob
