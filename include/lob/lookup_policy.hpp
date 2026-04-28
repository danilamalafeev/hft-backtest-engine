#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "lob/event.hpp"

namespace lob {

struct GraphRoute {
    AssetID pair_id {};
    bool use_bid {};
    bool valid {};
};

class DenseLookupPolicy {
public:
    void init(std::size_t asset_count, std::size_t pair_count) {
        (void)pair_count;
        edge_matrix_.assign(asset_count, std::vector<std::size_t>(asset_count, invalid_edge_index()));
        route_matrix_.assign(asset_count, std::vector<GraphRoute>(asset_count, GraphRoute {}));
    }

    void clear_edges() noexcept {
        for (auto& row : edge_matrix_) {
            std::fill(row.begin(), row.end(), invalid_edge_index());
        }
    }

    void set_edge(AssetID from, AssetID to, std::size_t edge_index) noexcept {
        if (
            from >= edge_matrix_.size() ||
            to >= edge_matrix_[from].size() ||
            edge_matrix_[from][to] != invalid_edge_index()
        ) {
            return;
        }
        edge_matrix_[from][to] = edge_index;
    }

    template <typename AdjacencyEdges, typename Edges>
    [[nodiscard]] std::size_t find_edge(
        AssetID from,
        AssetID to,
        const AdjacencyEdges& adjacency_edges,
        const Edges& edges
    ) const noexcept {
        (void)adjacency_edges;
        (void)edges;
        if (from >= edge_matrix_.size() || to >= edge_matrix_[from].size()) {
            return invalid_edge_index();
        }
        return edge_matrix_[from][to];
    }

    void set_route(AssetID from, AssetID to, AssetID pair_id, bool use_bid) noexcept {
        if (from >= route_matrix_.size() || to >= route_matrix_[from].size() || route_matrix_[from][to].valid) {
            return;
        }
        route_matrix_[from][to] = GraphRoute {
            .pair_id = pair_id,
            .use_bid = use_bid,
            .valid = true,
        };
    }

    void finalize_routes() noexcept {}

    [[nodiscard]] bool find_route(AssetID from, AssetID to, AssetID& pair_id_out, bool& use_bid_out) const noexcept {
        if (from >= route_matrix_.size() || to >= route_matrix_[from].size()) {
            return false;
        }
        const GraphRoute route = route_matrix_[from][to];
        if (!route.valid) {
            return false;
        }
        pair_id_out = route.pair_id;
        use_bid_out = route.use_bid;
        return true;
    }

private:
    [[nodiscard]] static constexpr std::size_t invalid_edge_index() noexcept {
        return std::numeric_limits<std::size_t>::max();
    }

    std::vector<std::vector<std::size_t>> edge_matrix_ {};
    std::vector<std::vector<GraphRoute>> route_matrix_ {};
};

class SparseLookupPolicy {
public:
    void init(std::size_t asset_count, std::size_t pair_count) {
        (void)asset_count;
        route_entries_.clear();
        route_entries_.reserve(pair_count * 2U);
    }

    void clear_edges() noexcept {}

    void set_edge(AssetID from, AssetID to, std::size_t edge_index) noexcept {
        (void)from;
        (void)to;
        (void)edge_index;
    }

    template <typename AdjacencyEdges, typename Edges>
    [[nodiscard]] std::size_t find_edge(
        AssetID from,
        AssetID to,
        const AdjacencyEdges& adjacency_edges,
        const Edges& edges
    ) const noexcept {
        if (from >= adjacency_edges.size()) {
            return invalid_edge_index();
        }
        for (const std::size_t edge_index : adjacency_edges[from]) {
            if (edge_index < edges.size() && edges[edge_index].to == to) {
                return edge_index;
            }
        }
        return invalid_edge_index();
    }

    void set_route(AssetID from, AssetID to, AssetID pair_id, bool use_bid) {
        const std::uint64_t route_key = key(from, to);
        for (const RouteEntry& entry : route_entries_) {
            if (entry.key == route_key) {
                return;
            }
        }
        route_entries_.push_back(RouteEntry {
            .key = route_key,
            .route = GraphRoute {
                .pair_id = pair_id,
                .use_bid = use_bid,
                .valid = true,
            },
        });
    }

    void finalize_routes() {
        std::sort(
            route_entries_.begin(),
            route_entries_.end(),
            [](const RouteEntry& lhs, const RouteEntry& rhs) noexcept {
                return lhs.key < rhs.key;
            }
        );
    }

    [[nodiscard]] bool find_route(AssetID from, AssetID to, AssetID& pair_id_out, bool& use_bid_out) const noexcept {
        const std::uint64_t route_key = key(from, to);
        const auto found = std::lower_bound(
            route_entries_.begin(),
            route_entries_.end(),
            route_key,
            [](const RouteEntry& entry, std::uint64_t value) noexcept {
                return entry.key < value;
            }
        );
        if (found == route_entries_.end() || found->key != route_key || !found->route.valid) {
            return false;
        }
        pair_id_out = found->route.pair_id;
        use_bid_out = found->route.use_bid;
        return true;
    }

private:
    struct RouteEntry {
        std::uint64_t key {};
        GraphRoute route {};
    };

    [[nodiscard]] static constexpr std::size_t invalid_edge_index() noexcept {
        return std::numeric_limits<std::size_t>::max();
    }

    [[nodiscard]] static constexpr std::uint64_t key(AssetID from, AssetID to) noexcept {
        return (static_cast<std::uint64_t>(from) << 32U) | static_cast<std::uint64_t>(to);
    }

    std::vector<RouteEntry> route_entries_ {};
};

}  // namespace lob
