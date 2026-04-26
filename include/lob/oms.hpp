#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "lob/event.hpp"
#include "lob/order.hpp"

namespace lob {

static constexpr std::size_t kOrderGroupLegCount = 3U;

struct OrderRequest {
    AssetID asset_id {};
    Side side {Side::Buy};
    double price {};
    std::uint64_t quantity {};
    std::uint64_t timestamp {};
    double expected_price {};
    double slippage_tolerance {};
};

struct OrderExecutionReport {
    AssetID asset_id {};
    Side side {Side::Buy};
    double expected_price {};
    double vwap_price {};
    std::uint64_t requested_quantity {};
    std::uint64_t filled_quantity {};
    bool fully_filled {};
    bool slippage_breached {};
};

struct OrderGroup {
    std::uint64_t group_id {};
    std::uint64_t timestamp {};
    std::uint64_t intra_leg_latency_ns {75U};
    std::uint64_t intra_leg_jitter_ns {25U};
    double slippage_tolerance {0.0002};
    std::array<OrderRequest, kOrderGroupLegCount> legs {};
};

struct OrderGroupResult {
    std::uint64_t group_id {};
    std::array<OrderExecutionReport, kOrderGroupLegCount> reports {};
    bool completed {};
    bool panic_triggered {};
};

}  // namespace lob
