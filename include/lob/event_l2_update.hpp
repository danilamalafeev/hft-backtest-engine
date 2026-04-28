#pragma once

#include <cstdint>

namespace lob {

struct L2UpdateEvent {
    std::uint64_t timestamp_ns {};
    bool is_snapshot {};
    bool is_bid {};
    double price {};
    double qty {};
};

}  // namespace lob
