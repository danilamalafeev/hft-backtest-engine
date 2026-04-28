#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "lob/event.hpp"
#include "lob/venue_replay.hpp"

namespace lob {

using CostModelID = std::uint16_t;
using SettlementDomainID = std::uint16_t;

enum class ProductKind : std::uint8_t {
    Spot,
    Perp,
    PredictionMarketOutcome,
    SyntheticTransform,
};

enum class CostModelKind : std::uint8_t {
    Proportional,
    Fixed,
    ProportionalPlusFixed,
};

struct AssetManifestEntry {
    AssetID asset_id {};
    std::string symbol {};
    SettlementDomainID settlement_domain_id {};
};

struct CostModelManifestEntry {
    CostModelID cost_model_id {};
    CostModelKind kind {CostModelKind::Proportional};
    double proportional_fee_bps {};
    std::int64_t fixed_fee_lots {};
};

struct ProductManifestEntry {
    ProductID product_id {};
    VenueID venue_id {};
    ProductKind kind {ProductKind::Spot};
    std::string symbol {};
    AssetID base_asset_id {};
    AssetID quote_asset_id {};
    std::int64_t price_tick_scale {1};
    std::int64_t qty_lot_scale {1};
    CostModelID cost_model_id {};
    SettlementDomainID settlement_domain_id {};
};

struct VenueManifest {
    std::vector<AssetManifestEntry> assets {};
    std::vector<CostModelManifestEntry> cost_models {};
    std::vector<ProductManifestEntry> products {};
};

}  // namespace lob
