#pragma once

#include <cmath>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "lob/event.hpp"
#include "lob/dynamic_wallet.hpp"
#include "lob/l2_csv_parser.hpp"

namespace lob {

class GraphArbitrageEngine {
public:
    struct PairConfig {
        std::string base_asset {};
        std::string quote_asset {};
        std::filesystem::path csv_file_path {};
    };

    struct PairState {
        AssetID base_id {};
        AssetID quote_id {};
        double bid_price {};
        double bid_qty {};
        double ask_price {};
        double ask_qty {};
    };

    struct Result {
        std::uint64_t events_processed {};
        std::uint64_t cycles_detected {};
        std::uint64_t attempted_cycles {};
        std::uint64_t completed_cycles {};
        std::uint64_t panic_closes {};
        double initial_usdt {100'000'000.0};
        double final_usdt {100'000'000.0};
        double final_nav {100'000'000.0};
        double inventory_risk {};
        std::vector<double> balances {};
        std::vector<AssetID> last_cycle {};
    };

    explicit GraphArbitrageEngine(
        double initial_usdt = 100'000'000.0,
        std::uint64_t latency_ns = 0U,
        std::uint64_t intra_leg_latency_ns = 75U,
        double taker_fee_bps = 0.0,
        double max_cycle_notional_usdt = 1'000.0
    )
        : initial_usdt_(initial_usdt),
          latency_ns_(latency_ns),
          intra_leg_latency_ns_(intra_leg_latency_ns),
          taker_fee_bps_(taker_fee_bps),
          max_cycle_notional_usdt_(max_cycle_notional_usdt) {}

    AssetID add_pair(std::string base_asset, std::string quote_asset, std::filesystem::path csv_file_path) {
        if (running_) {
            throw std::runtime_error("Cannot add pairs while GraphArbitrageEngine is running");
        }

        const AssetID base_id = asset_id_for(std::move(base_asset));
        const AssetID quote_id = asset_id_for(std::move(quote_asset));
        const AssetID pair_id = static_cast<AssetID>(pairs_.size());
        pairs_.push_back(PairConfig {
            .base_asset = asset_names_[base_id],
            .quote_asset = asset_names_[quote_id],
            .csv_file_path = std::move(csv_file_path),
        });
        pair_states_.push_back(PairState {
            .base_id = base_id,
            .quote_id = quote_id,
        });
        return pair_id;
    }

    [[nodiscard]] Result run() {
        prepare_runtime();
        Result result {};
        result.initial_usdt = initial_usdt_;

        while (has_next_event()) {
            const AssetID pair_id = next_pair_index();
            L2BookEvent event = parsers_[pair_id].pop();
            event.pair_id = pair_id;
            current_time_ns_ = normalize_timestamp_ns(event.timestamp);
            release_pending_cycles(result, current_time_ns_);
            apply_event(event);
            ++result.events_processed;

            if (find_negative_cycle()) {
                ++result.cycles_detected;
                enqueue_cycle(result);
            }
            release_pending_cycles(result, current_time_ns_);
        }

        release_pending_cycles(result, std::numeric_limits<std::uint64_t>::max());
        rebuild_edges();
        result.final_usdt = wallet_.balance(usdt_asset_id_);
        result.final_nav = wallet_.mark_to_market_nav(edges_, taker_fee_bps_);
        result.inventory_risk = wallet_.get_total_inventory_risk(edges_, taker_fee_bps_);
        result.balances = wallet_.balances();
        result.last_cycle = cycle_;
        running_ = false;
        return result;
    }

    [[nodiscard]] const std::vector<std::string>& assets() const noexcept {
        return asset_names_;
    }

private:
    static constexpr std::size_t kMaxGraphLegs = 16U;
    static constexpr std::size_t kPendingCycleCapacity = 16'384U;
    static constexpr double kEpsilon = 1e-12;

    struct Edge {
        AssetID from {};
        AssetID to {};
        AssetID pair_id {};
        bool use_bid {};
        double rate {};
        double available_from_qty {};
        double weight {};
    };

    struct DepletionState {
        std::uint64_t last_update_ns {};
        double bid_depleted_qty {};
        double ask_depleted_qty {};
    };

    struct PendingCycle {
        std::uint64_t release_time_ns {};
        std::uint8_t leg_count {};
        double start_quantity {};
        std::array<AssetID, kMaxGraphLegs + 1U> assets {};
    };

    template <typename T, std::size_t Capacity>
    class PendingMinHeap {
    public:
        [[nodiscard]] bool empty() const noexcept {
            return size_ == 0U;
        }

        [[nodiscard]] const T& top() const noexcept {
            return heap_[0];
        }

        bool push(const T& item) noexcept {
            if (size_ == Capacity) {
                return false;
            }

            std::size_t index = size_++;
            heap_[index] = item;
            while (index > 0U) {
                const std::size_t parent = (index - 1U) / 2U;
                if (heap_[parent].release_time_ns <= heap_[index].release_time_ns) {
                    break;
                }
                std::swap(heap_[parent], heap_[index]);
                index = parent;
            }
            return true;
        }

        void pop() noexcept {
            heap_[0] = heap_[--size_];
            std::size_t index = 0U;
            while (true) {
                const std::size_t left = (index * 2U) + 1U;
                const std::size_t right = left + 1U;
                if (left >= size_) {
                    break;
                }

                std::size_t smallest = left;
                if (right < size_ && heap_[right].release_time_ns < heap_[left].release_time_ns) {
                    smallest = right;
                }
                if (heap_[index].release_time_ns <= heap_[smallest].release_time_ns) {
                    break;
                }
                std::swap(heap_[index], heap_[smallest]);
                index = smallest;
            }
        }

    private:
        std::array<T, Capacity> heap_ {};
        std::size_t size_ {};
    };

    [[nodiscard]] AssetID asset_id_for(std::string asset) {
        const auto found = asset_index_.find(asset);
        if (found != asset_index_.end()) {
            return found->second;
        }

        const AssetID id = static_cast<AssetID>(asset_names_.size());
        asset_names_.push_back(asset);
        asset_index_.emplace(std::move(asset), id);
        return id;
    }

    void prepare_runtime() {
        parsers_.clear();
        parsers_.reserve(pairs_.size());
        for (const PairConfig& pair : pairs_) {
            parsers_.emplace_back(pair.csv_file_path);
        }

        const std::size_t asset_count = asset_names_.size();
        distances_.assign(asset_count, 0.0);
        predecessors_.assign(asset_count, 0U);
        predecessor_edges_.assign(asset_count, 0U);
        cycle_.clear();
        cycle_.reserve(asset_count + 1U);
        const auto usdt_it = asset_index_.find("USDT");
        usdt_asset_id_ = usdt_it != asset_index_.end() ? usdt_it->second : 0U;
        wallet_.reset(asset_count, usdt_asset_id_, initial_usdt_);
        edges_.clear();
        edges_.reserve(pairs_.size() * 2U);
        depletion_ledger_.assign(pair_states_.size(), DepletionState {});
        pending_cycles_ = PendingMinHeap<PendingCycle, kPendingCycleCapacity> {};
        current_time_ns_ = 0U;
        running_ = true;
    }

    [[nodiscard]] static std::uint64_t normalize_timestamp_ns(std::uint64_t timestamp) noexcept {
        return timestamp < 10'000'000'000'000ULL ? timestamp * 1'000'000ULL : timestamp;
    }

    [[nodiscard]] bool has_next_event() const noexcept {
        for (const L2CsvParser& parser : parsers_) {
            if (parser.has_next()) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] AssetID next_pair_index() const {
        std::uint64_t best_time = std::numeric_limits<std::uint64_t>::max();
        AssetID best_pair = std::numeric_limits<AssetID>::max();
        for (std::size_t index = 0U; index < parsers_.size(); ++index) {
            const L2CsvParser& parser = parsers_[index];
            if (!parser.has_next()) {
                continue;
            }
            const std::uint64_t time = parser.peek_time();
            if (time < best_time) {
                best_time = time;
                best_pair = static_cast<AssetID>(index);
            }
        }
        if (best_pair == std::numeric_limits<AssetID>::max()) {
            throw std::out_of_range("GraphArbitrageEngine has no remaining events");
        }
        return best_pair;
    }

    void apply_event(const L2BookEvent& event) noexcept {
        PairState& state = pair_states_[event.pair_id];
        state.bid_price = event.bid_price;
        state.bid_qty = event.bid_qty;
        state.ask_price = event.ask_price;
        state.ask_qty = event.ask_qty;

        DepletionState& depletion = depletion_ledger_[event.pair_id];
        depletion.last_update_ns = current_time_ns_;
        depletion.bid_depleted_qty = 0.0;
        depletion.ask_depleted_qty = 0.0;
    }

    void rebuild_edges() {
        edges_.clear();
        for (AssetID pair_id = 0U; pair_id < pair_states_.size(); ++pair_id) {
            const PairState& pair = pair_states_[pair_id];
            const DepletionState& depletion = depletion_ledger_[pair_id];
            const double effective_bid_qty = pair.bid_qty - depletion.bid_depleted_qty;
            const double effective_ask_qty = pair.ask_qty - depletion.ask_depleted_qty;
            if (pair.bid_price > 0.0 && effective_bid_qty > 0.0) {
                const double rate = pair.bid_price;
                edges_.push_back(Edge {
                    .from = pair.base_id,
                    .to = pair.quote_id,
                    .pair_id = pair_id,
                    .use_bid = true,
                    .rate = rate,
                    .available_from_qty = effective_bid_qty,
                    .weight = -std::log(rate),
                });
            }
            if (pair.ask_price > 0.0 && effective_ask_qty > 0.0) {
                const double rate = 1.0 / pair.ask_price;
                edges_.push_back(Edge {
                    .from = pair.quote_id,
                    .to = pair.base_id,
                    .pair_id = pair_id,
                    .use_bid = false,
                    .rate = rate,
                    .available_from_qty = pair.ask_price * effective_ask_qty,
                    .weight = -std::log(rate),
                });
            }
        }
    }

    [[nodiscard]] bool find_negative_cycle() {
        rebuild_edges();
        const std::size_t asset_count = asset_names_.size();
        if (asset_count == 0U || edges_.empty()) {
            return false;
        }

        std::fill(distances_.begin(), distances_.end(), 0.0);
        AssetID updated_vertex = std::numeric_limits<AssetID>::max();

        for (std::size_t iteration = 0U; iteration < asset_count; ++iteration) {
            updated_vertex = std::numeric_limits<AssetID>::max();
            for (std::size_t edge_index = 0U; edge_index < edges_.size(); ++edge_index) {
                const Edge& edge = edges_[edge_index];
                const double candidate = distances_[edge.from] + edge.weight;
                if (candidate + kEpsilon < distances_[edge.to]) {
                    distances_[edge.to] = candidate;
                    predecessors_[edge.to] = edge.from;
                    predecessor_edges_[edge.to] = edge_index;
                    updated_vertex = edge.to;
                }
            }
            if (updated_vertex == std::numeric_limits<AssetID>::max()) {
                return false;
            }
        }

        for (std::size_t index = 0U; index < asset_count; ++index) {
            updated_vertex = predecessors_[updated_vertex];
        }

        cycle_.clear();
        AssetID current = updated_vertex;
        do {
            cycle_.push_back(current);
            current = predecessors_[current];
        } while (current != updated_vertex && cycle_.size() <= asset_count + 1U);
        cycle_.push_back(updated_vertex);
        std::reverse(cycle_.begin(), cycle_.end());
        return cycle_.size() > 2U && rotate_cycle_to_usdt();
    }

    [[nodiscard]] bool rotate_cycle_to_usdt() {
        if (cycle_.size() < 3U) {
            return false;
        }

        auto usdt_it = std::find(cycle_.begin(), cycle_.end() - 1, usdt_asset_id_);
        if (usdt_it == cycle_.end() - 1) {
            return false;
        }

        std::rotate(cycle_.begin(), usdt_it, cycle_.end() - 1);
        cycle_.back() = cycle_.front();
        return true;
    }

    void enqueue_cycle(Result& result) {
        ++result.attempted_cycles;
        if (cycle_.size() < 3U || cycle_.size() > kMaxGraphLegs + 1U || cycle_.front() != usdt_asset_id_) {
            return;
        }

        const double start_quantity = calculate_fee_aware_bottleneck();
        if (start_quantity <= 0.0) {
            return;
        }

        PendingCycle pending {};
        pending.release_time_ns = current_time_ns_ + latency_ns_;
        pending.leg_count = static_cast<std::uint8_t>(cycle_.size() - 1U);
        pending.start_quantity = start_quantity;
        for (std::size_t index = 0U; index < cycle_.size(); ++index) {
            pending.assets[index] = cycle_[index];
        }

        if (!pending_cycles_.push(pending)) {
            ++result.panic_closes;
            return;
        }
    }

    [[nodiscard]] double calculate_fee_aware_bottleneck() const noexcept {
        double cumulative_input = 1.0;
        const double available_usdt = wallet_.balance(usdt_asset_id_);
        double max_start = max_cycle_notional_usdt_ > 0.0 ? max_cycle_notional_usdt_ : available_usdt;
        if (available_usdt < max_start) {
            max_start = available_usdt;
        }

        const double fee_multiplier = taker_fee_multiplier();
        for (std::size_t index = 0U; index + 1U < cycle_.size(); ++index) {
            const Edge* edge = edge_for(cycle_[index], cycle_[index + 1U]);
            if (edge == nullptr || edge->rate <= 0.0 || edge->available_from_qty <= 0.0 || cumulative_input <= 0.0) {
                return 0.0;
            }

            const double constrained_start = edge->available_from_qty / cumulative_input;
            if (constrained_start < max_start) {
                max_start = constrained_start;
            }
            cumulative_input *= edge->rate * fee_multiplier;
        }

        return max_start > 0.0 ? max_start : 0.0;
    }

    void release_pending_cycles(Result& result, std::uint64_t now_ns) {
        while (!pending_cycles_.empty() && pending_cycles_.top().release_time_ns <= now_ns) {
            const PendingCycle pending = pending_cycles_.top();
            pending_cycles_.pop();
            execute_pending_cycle(pending, result);
        }
    }

    void execute_pending_cycle(const PendingCycle& pending, Result& result) {
        double quantity = pending.start_quantity;
        AssetID current_asset = pending.assets[0U];
        const double initial_quantity = quantity;

        for (std::size_t leg = 0U; leg < pending.leg_count; ++leg) {
            (void)(pending.release_time_ns + (leg + 1U) * intra_leg_latency_ns_);
            rebuild_edges();
            const AssetID next_asset = pending.assets[leg + 1U];
            const Edge* edge = edge_for(current_asset, next_asset);
            if (
                edge == nullptr ||
                edge->available_from_qty + kEpsilon < quantity ||
                wallet_.balance(current_asset) + kEpsilon < quantity
            ) {
                panic_close_to_usdt(current_asset, quantity, initial_quantity, result);
                return;
            }

            wallet_.sub_balance(current_asset, quantity);
            apply_depletion(*edge, quantity);
            quantity = wallet_.apply_fill(next_asset, quantity * edge->rate, taker_fee_bps_);
            current_asset = next_asset;
        }

        if (current_asset == usdt_asset_id_) {
            ++result.completed_cycles;
            return;
        }

        panic_close_to_usdt(current_asset, quantity, initial_quantity, result);
    }

    void panic_close_to_usdt(AssetID asset_id, double quantity, double initial_usdt, Result& result) {
        ++result.panic_closes;
        (void)initial_usdt;
        if (asset_id == usdt_asset_id_ || quantity <= 0.0) {
            return;
        }

        const double close_quantity = wallet_.balance(asset_id) < quantity ? wallet_.balance(asset_id) : quantity;
        if (close_quantity <= 0.0) {
            return;
        }

        const double recovered_usdt = convert_to_usdt(asset_id, close_quantity);
        wallet_.sub_balance(asset_id, close_quantity);
        (void)wallet_.apply_fill(usdt_asset_id_, recovered_usdt, taker_fee_bps_);
    }

    [[nodiscard]] double convert_to_usdt(AssetID asset_id, double quantity) noexcept {
        if (asset_id == usdt_asset_id_) {
            return quantity;
        }

        if (const Edge* direct = edge_for(asset_id, usdt_asset_id_)) {
            return execute_pessimistic_liquidation(*direct, quantity);
        }

        for (const Edge& first : edges_) {
            if (first.from != asset_id) {
                continue;
            }
            if (const Edge* second = edge_for(first.to, usdt_asset_id_)) {
                const double intermediate = execute_pessimistic_liquidation(first, quantity);
                return execute_pessimistic_liquidation(*second, intermediate * taker_fee_multiplier());
            }
        }

        return 0.0;
    }

    [[nodiscard]] double execute_pessimistic_liquidation(const Edge& edge, double input_quantity) noexcept {
        static constexpr double kPanicSlippagePenalty = 0.005;
        const double top_quantity = input_quantity < edge.available_from_qty ? input_quantity : edge.available_from_qty;
        const double excess_quantity = input_quantity - top_quantity;
        const double top_output = top_quantity * edge.rate;
        const double penalized_output = excess_quantity > 0.0
            ? excess_quantity * edge.rate * (1.0 - kPanicSlippagePenalty)
            : 0.0;
        apply_depletion(edge, input_quantity);
        return top_output + penalized_output;
    }

    void apply_depletion(const Edge& edge, double input_quantity) noexcept {
        DepletionState& depletion = depletion_ledger_[edge.pair_id];
        if (edge.use_bid) {
            depletion.bid_depleted_qty += input_quantity;
            return;
        }

        const PairState& pair = pair_states_[edge.pair_id];
        if (pair.ask_price > 0.0) {
            depletion.ask_depleted_qty += input_quantity / pair.ask_price;
        }
    }

    [[nodiscard]] double taker_fee_multiplier() const noexcept {
        return 1.0 - (taker_fee_bps_ * 0.0001);
    }

    [[nodiscard]] const Edge* edge_for(AssetID from, AssetID to) const noexcept {
        for (const Edge& edge : edges_) {
            if (edge.from == from && edge.to == to) {
                return &edge;
            }
        }
        return nullptr;
    }

    double initial_usdt_ {};
    std::uint64_t latency_ns_ {};
    std::uint64_t intra_leg_latency_ns_ {75U};
    double taker_fee_bps_ {};
    double max_cycle_notional_usdt_ {1'000.0};
    AssetID usdt_asset_id_ {};
    std::uint64_t current_time_ns_ {};
    bool running_ {};
    std::vector<PairConfig> pairs_ {};
    std::vector<PairState> pair_states_ {};
    std::vector<L2CsvParser> parsers_ {};
    std::vector<std::string> asset_names_ {};
    std::unordered_map<std::string, AssetID> asset_index_ {};
    std::vector<Edge> edges_ {};
    std::vector<double> distances_ {};
    std::vector<AssetID> predecessors_ {};
    std::vector<std::size_t> predecessor_edges_ {};
    std::vector<AssetID> cycle_ {};
    std::vector<DepletionState> depletion_ledger_ {};
    DynamicWallet wallet_ {};
    PendingMinHeap<PendingCycle, kPendingCycleCapacity> pending_cycles_ {};
};

}  // namespace lob
