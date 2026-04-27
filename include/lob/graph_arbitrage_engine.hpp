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
#include "lob/l2_depth5_csv_parser.hpp"

namespace lob {

class GraphArbitrageEngine {
public:
    static constexpr std::size_t kMaxGraphLegs = 16U;

    struct Config {
        double initial_usdt {100'000'000.0};
        std::string quote_asset {"USDT"};
        std::uint64_t latency_ns {0U};
        std::uint64_t intra_leg_latency_ns {75U};
        double taker_fee_bps {0.0};
        double max_cycle_notional_usdt {1'000.0};
        double max_adverse_obi {1.0};
        double max_spread_bps {1'000.0};
        double min_depth_usdt {0.0};
        double min_cycle_edge_bps {0.0};
        std::size_t cycle_snapshot_reserve {100'000U};
    };

    struct PairConfig {
        std::string base_asset {};
        std::string quote_asset {};
        std::filesystem::path csv_file_path {};
    };

    struct PairState {
        AssetID base_id {};
        AssetID quote_id {};
        std::array<double, L2Depth5Event::Depth> bid_prices {};
        std::array<double, L2Depth5Event::Depth> bid_qty {};
        std::array<double, L2Depth5Event::Depth> ask_prices {};
        std::array<double, L2Depth5Event::Depth> ask_qty {};
        double obi {};
        double micro_price {};
        double spread_bps {};
    };

    struct CycleSnapshot {
        std::uint64_t timestamp_ns {};
        std::uint8_t leg_count {};
        std::array<AssetID, kMaxGraphLegs + 1U> cycle_path {};
        std::array<double, kMaxGraphLegs> leg_obis {};
        std::array<double, kMaxGraphLegs> leg_spreads_bps {};
        bool executed {};
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
        std::vector<CycleSnapshot> cycle_snapshots {};
        std::uint64_t cycle_snapshots_overwritten {};
    };

    GraphArbitrageEngine()
        : config_(Config {}) {}

    explicit GraphArbitrageEngine(Config config)
        : config_(config) {}

    GraphArbitrageEngine(
        double initial_usdt,
        std::uint64_t latency_ns,
        std::uint64_t intra_leg_latency_ns,
        double taker_fee_bps,
        double max_cycle_notional_usdt
    )
        : config_(Config {
              .initial_usdt = initial_usdt,
              .latency_ns = latency_ns,
              .intra_leg_latency_ns = intra_leg_latency_ns,
              .taker_fee_bps = taker_fee_bps,
              .max_cycle_notional_usdt = max_cycle_notional_usdt,
          }) {}

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
        result.initial_usdt = config_.initial_usdt;

        while (has_next_event() || has_pending_cycle()) {
            if (should_process_pending_cycle()) {
                process_next_pending_cycle(result);
                continue;
            }

            process_next_market_event(result);
        }

        rebuild_edges();
        result.final_usdt = wallet_.balance(quote_asset_id_);
        result.final_nav = wallet_.mark_to_market_nav(edges_, config_.taker_fee_bps);
        result.inventory_risk = wallet_.get_total_inventory_risk(edges_, config_.taker_fee_bps);
        result.balances = wallet_.balances();
        result.last_cycle = cycle_;
        result.cycle_snapshots = cycle_snapshots_.to_vector();
        result.cycle_snapshots_overwritten = cycle_snapshots_.overwritten_count();
        running_ = false;
        return result;
    }

    [[nodiscard]] const std::vector<std::string>& assets() const noexcept {
        return asset_names_;
    }

private:
    static constexpr std::size_t kPendingCycleCapacity = 16'384U;
    static constexpr double kEpsilon = 1e-12;

    struct Edge {
        AssetID from {};
        AssetID to {};
        AssetID pair_id {};
        bool use_bid {};
        double rate {};
        double available_from_qty {};
        double effective_rate {};
    };

    struct DepletionState {
        std::uint64_t last_update_ns {};
        std::array<double, L2Depth5Event::Depth> bid_depleted_qty {};
        std::array<double, L2Depth5Event::Depth> ask_depleted_qty {};
    };

    struct PendingCycle {
        std::uint64_t release_time_ns {};
        std::uint8_t leg_count {};
        std::uint8_t current_leg {};
        double current_quantity {};
        AssetID current_asset {};
        double initial_quantity {};
        std::array<AssetID, kMaxGraphLegs + 1U> assets {};
    };

    struct ParserEvent {
        std::uint64_t release_time_ns {};
        AssetID pair_id {};
    };

    struct ParserEventGreater {
        [[nodiscard]] bool operator()(const ParserEvent& lhs, const ParserEvent& rhs) const noexcept {
            if (lhs.release_time_ns != rhs.release_time_ns) {
                return lhs.release_time_ns > rhs.release_time_ns;
            }
            return lhs.pair_id > rhs.pair_id;
        }
    };

    class CycleSnapshotRing {
    public:
        void reset(std::size_t capacity) {
            storage_.assign(capacity, CycleSnapshot {});
            next_index_ = 0U;
            count_ = 0U;
            overwritten_count_ = 0U;
        }

        void push(const CycleSnapshot& snapshot) noexcept {
            if (storage_.empty()) {
                ++overwritten_count_;
                return;
            }

            storage_[next_index_] = snapshot;
            next_index_ = (next_index_ + 1U) % storage_.size();
            if (count_ < storage_.size()) {
                ++count_;
            } else {
                ++overwritten_count_;
            }
        }

        [[nodiscard]] std::uint64_t overwritten_count() const noexcept {
            return overwritten_count_;
        }

        [[nodiscard]] std::vector<CycleSnapshot> to_vector() const {
            std::vector<CycleSnapshot> snapshots {};
            snapshots.reserve(count_);
            if (count_ == 0U) {
                return snapshots;
            }

            const std::size_t first_index = count_ == storage_.size() ? next_index_ : 0U;
            for (std::size_t offset = 0U; offset < count_; ++offset) {
                snapshots.push_back(storage_[(first_index + offset) % storage_.size()]);
            }
            return snapshots;
        }

    private:
        std::vector<CycleSnapshot> storage_ {};
        std::size_t next_index_ {};
        std::size_t count_ {};
        std::uint64_t overwritten_count_ {};
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

        quote_asset_id_ = asset_id_for(config_.quote_asset);
        const std::size_t asset_count = asset_names_.size();
        rates_.assign(asset_count, 1.0);
        predecessors_.assign(asset_count, 0U);
        predecessor_edges_.assign(asset_count, 0U);
        relax_counts_.assign(asset_count, 0U);
        in_queue_.assign(asset_count, 0U);
        spfa_queue_.assign(asset_count == 0U ? 1U : asset_count, 0U);
        adjacency_edges_.assign(asset_count, {});
        for (auto& adjacent_edges : adjacency_edges_) {
            adjacent_edges.reserve(pairs_.size() * 2U);
        }
        cycle_.clear();
        cycle_.reserve(asset_count + 1U);
        wallet_.reset(asset_count, quote_asset_id_, config_.initial_usdt);
        edges_.clear();
        edges_.reserve(pairs_.size() * 2U);
        depletion_ledger_.assign(pair_states_.size(), DepletionState {});
        pending_cycles_ = PendingMinHeap<PendingCycle, kPendingCycleCapacity> {};
        cycle_snapshots_.reset(config_.cycle_snapshot_reserve);
        parser_events_.clear();
        parser_events_.reserve(parsers_.size());
        for (std::size_t index = 0U; index < parsers_.size(); ++index) {
            push_parser_event_if_available(static_cast<AssetID>(index));
        }
        current_time_ns_ = 0U;
        running_ = true;
    }

    [[nodiscard]] static std::uint64_t normalize_timestamp_ns(std::uint64_t timestamp) noexcept {
        return timestamp < 10'000'000'000'000ULL ? timestamp * 1'000'000ULL : timestamp;
    }

    [[nodiscard]] bool has_next_event() const noexcept {
        return !parser_events_.empty();
    }

    [[nodiscard]] bool has_pending_cycle() const noexcept {
        return !pending_cycles_.empty();
    }

    [[nodiscard]] bool should_process_pending_cycle() const noexcept {
        if (!has_pending_cycle()) {
            return false;
        }
        return !has_next_event() || pending_cycles_.top().release_time_ns <= parser_events_.front().release_time_ns;
    }

    [[nodiscard]] AssetID next_pair_index() const {
        if (parser_events_.empty()) {
            throw std::out_of_range("GraphArbitrageEngine has no remaining events");
        }
        return parser_events_.front().pair_id;
    }

    [[nodiscard]] std::uint64_t next_event_time() const {
        if (parser_events_.empty()) {
            throw std::out_of_range("GraphArbitrageEngine has no remaining events");
        }
        return parser_events_.front().release_time_ns;
    }

    void push_parser_event_if_available(AssetID pair_id) {
        L2Depth5CsvParser& parser = parsers_[pair_id];
        if (!parser.has_next()) {
            return;
        }
        parser_events_.push_back(ParserEvent {
            .release_time_ns = normalize_timestamp_ns(parser.peek_time()),
            .pair_id = pair_id,
        });
        std::push_heap(parser_events_.begin(), parser_events_.end(), ParserEventGreater {});
    }

    [[nodiscard]] ParserEvent pop_parser_event() {
        if (parser_events_.empty()) {
            throw std::out_of_range("GraphArbitrageEngine has no remaining events");
        }
        std::pop_heap(parser_events_.begin(), parser_events_.end(), ParserEventGreater {});
        ParserEvent event = parser_events_.back();
        parser_events_.pop_back();
        return event;
    }

    void process_next_market_event(Result& result) {
        const ParserEvent parser_event = pop_parser_event();
        L2Depth5CsvParser& parser = parsers_[parser_event.pair_id];
        const L2Depth5Event& event = parser.peek();
        current_time_ns_ = normalize_timestamp_ns(event.timestamp);
        apply_event(parser_event.pair_id, event);
        ++result.events_processed;
        parser.advance();
        push_parser_event_if_available(parser_event.pair_id);

        if (find_negative_cycle()) {
            ++result.cycles_detected;
            enqueue_cycle(result);
        }
    }

    void apply_event(AssetID pair_id, const L2Depth5Event& event) noexcept {
        PairState& state = pair_states_[pair_id];
        DepletionState& depletion = depletion_ledger_[pair_id];
        reconcile_depletion(state, event, depletion);

        state.bid_prices = event.bid_prices;
        state.bid_qty = event.bid_qty;
        state.ask_prices = event.ask_prices;
        state.ask_qty = event.ask_qty;

        double bid_depth = 0.0;
        double ask_depth = 0.0;
        double bid_notional = 0.0;
        double ask_notional = 0.0;
        for (std::size_t level = 0U; level < L2Depth5Event::Depth; ++level) {
            if (state.bid_prices[level] > 0.0 && state.bid_qty[level] > 0.0) {
                bid_depth += state.bid_qty[level];
                bid_notional += state.bid_prices[level] * state.bid_qty[level];
            }
            if (state.ask_prices[level] > 0.0 && state.ask_qty[level] > 0.0) {
                ask_depth += state.ask_qty[level];
                ask_notional += state.ask_prices[level] * state.ask_qty[level];
            }
        }

        const double depth_sum = bid_depth + ask_depth;
        const double best_bid = state.bid_prices[0U];
        const double best_ask = state.ask_prices[0U];
        if (depth_sum > kEpsilon && best_bid > 0.0 && best_ask > 0.0) {
            state.obi = (bid_depth - ask_depth) / depth_sum;
            const double bid_vwap = bid_depth > kEpsilon ? bid_notional / bid_depth : best_bid;
            const double ask_vwap = ask_depth > kEpsilon ? ask_notional / ask_depth : best_ask;
            state.micro_price = ((bid_vwap * ask_depth) + (ask_vwap * bid_depth)) / depth_sum;
            state.spread_bps = ((best_ask - best_bid) / best_bid) * 10'000.0;
        } else {
            state.obi = 0.0;
            state.micro_price = 0.0;
            state.spread_bps = 0.0;
        }

        depletion.last_update_ns = current_time_ns_;
    }

    void reconcile_depletion(
        const PairState& previous_state,
        const L2Depth5Event& event,
        DepletionState& depletion
    ) noexcept {
        std::array<double, L2Depth5Event::Depth> next_bid_depletion {};
        std::array<double, L2Depth5Event::Depth> next_ask_depletion {};

        for (std::size_t new_level = 0U; new_level < L2Depth5Event::Depth; ++new_level) {
            const double bid_price = event.bid_prices[new_level];
            if (bid_price > 0.0) {
                for (std::size_t old_level = 0U; old_level < L2Depth5Event::Depth; ++old_level) {
                    if (previous_state.bid_prices[old_level] == bid_price) {
                        const double retained = depletion.bid_depleted_qty[old_level];
                        next_bid_depletion[new_level] = retained < event.bid_qty[new_level] ? retained : event.bid_qty[new_level];
                        break;
                    }
                }
            }

            const double ask_price = event.ask_prices[new_level];
            if (ask_price > 0.0) {
                for (std::size_t old_level = 0U; old_level < L2Depth5Event::Depth; ++old_level) {
                    if (previous_state.ask_prices[old_level] == ask_price) {
                        const double retained = depletion.ask_depleted_qty[old_level];
                        next_ask_depletion[new_level] = retained < event.ask_qty[new_level] ? retained : event.ask_qty[new_level];
                        break;
                    }
                }
            }
        }

        depletion.bid_depleted_qty = next_bid_depletion;
        depletion.ask_depleted_qty = next_ask_depletion;
    }

    void rebuild_edges() {
        edges_.clear();
        for (auto& adjacent_edges : adjacency_edges_) {
            adjacent_edges.clear();
        }
        for (AssetID pair_id = 0U; pair_id < pair_states_.size(); ++pair_id) {
            const PairState& pair = pair_states_[pair_id];
            const double bid_capacity = side_input_capacity(pair_id, true);
            const double ask_capacity = side_input_capacity(pair_id, false);
            if (pair.bid_prices[0U] > 0.0 && bid_capacity > 0.0) {
                const double rate = pair.bid_prices[0U];
                const double effective_rate = rate * taker_fee_multiplier();
                add_edge(Edge {
                    .from = pair.base_id,
                    .to = pair.quote_id,
                    .pair_id = pair_id,
                    .use_bid = true,
                    .rate = rate,
                    .available_from_qty = bid_capacity,
                    .effective_rate = effective_rate,
                });
            }
            if (pair.ask_prices[0U] > 0.0 && ask_capacity > 0.0) {
                const double rate = 1.0 / pair.ask_prices[0U];
                const double effective_rate = rate * taker_fee_multiplier();
                add_edge(Edge {
                    .from = pair.quote_id,
                    .to = pair.base_id,
                    .pair_id = pair_id,
                    .use_bid = false,
                    .rate = rate,
                    .available_from_qty = ask_capacity,
                    .effective_rate = effective_rate,
                });
            }
        }
    }

    void add_edge(const Edge& edge) {
        const std::size_t edge_index = edges_.size();
        edges_.push_back(edge);
        if (edge.from < adjacency_edges_.size()) {
            adjacency_edges_[edge.from].push_back(edge_index);
        }
    }

    [[nodiscard]] bool find_negative_cycle() {
        rebuild_edges();
        const std::size_t asset_count = asset_names_.size();
        if (asset_count == 0U || edges_.empty()) {
            return false;
        }

        std::fill(rates_.begin(), rates_.end(), 1.0);
        std::fill(relax_counts_.begin(), relax_counts_.end(), 0U);
        std::fill(in_queue_.begin(), in_queue_.end(), 0U);

        std::size_t head = 0U;
        std::size_t tail = 0U;
        std::size_t queued = 0U;
        for (AssetID vertex = 0U; vertex < asset_count; ++vertex) {
            enqueue_spfa_vertex(vertex, head, tail, queued);
        }

        while (queued > 0U) {
            const AssetID vertex = spfa_queue_[head];
            head = (head + 1U) % spfa_queue_.size();
            --queued;
            in_queue_[vertex] = 0U;

            for (const std::size_t edge_index : adjacency_edges_[vertex]) {
                const Edge& edge = edges_[edge_index];
                const double candidate = rates_[edge.from] * edge.effective_rate;
                if (candidate <= rates_[edge.to] * (1.0 + kEpsilon)) {
                    continue;
                }

                rates_[edge.to] = candidate;
                predecessors_[edge.to] = edge.from;
                predecessor_edges_[edge.to] = edge_index;
                ++relax_counts_[edge.to];
                if (relax_counts_[edge.to] >= asset_count) {
                    return reconstruct_negative_cycle(edge.to, asset_count);
                }
                enqueue_spfa_vertex(edge.to, head, tail, queued);
            }
        }

        return false;
    }

    void enqueue_spfa_vertex(AssetID vertex, std::size_t& head, std::size_t& tail, std::size_t& queued) noexcept {
        (void)head;
        if (vertex >= in_queue_.size() || in_queue_[vertex] != 0U || queued >= spfa_queue_.size()) {
            return;
        }
        spfa_queue_[tail] = vertex;
        tail = (tail + 1U) % spfa_queue_.size();
        ++queued;
        in_queue_[vertex] = 1U;
    }

    [[nodiscard]] bool reconstruct_negative_cycle(AssetID updated_vertex, std::size_t asset_count) {
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
        return cycle_.size() > 2U && rotate_cycle_to_quote();
    }

    [[nodiscard]] bool rotate_cycle_to_quote() {
        if (cycle_.size() < 3U) {
            return false;
        }

        auto quote_it = std::find(cycle_.begin(), cycle_.end() - 1, quote_asset_id_);
        if (quote_it == cycle_.end() - 1) {
            return false;
        }

        std::rotate(cycle_.begin(), quote_it, cycle_.end() - 1);
        cycle_.back() = cycle_.front();
        return true;
    }

    void enqueue_cycle(Result& result) {
        ++result.attempted_cycles;
        if (cycle_.size() < 3U || cycle_.size() > kMaxGraphLegs + 1U || cycle_.front() != quote_asset_id_) {
            return;
        }

        const double start_quantity = calculate_fee_aware_bottleneck();
        const bool executable = start_quantity > 0.0 && cycle_passes_execution_filters() && cycle_edge_bps() >= config_.min_cycle_edge_bps;
        record_cycle_snapshot(executable);
        if (!executable) {
            return;
        }
        if (!wallet_.reserve_balance(quote_asset_id_, start_quantity)) {
            return;
        }

        PendingCycle pending {};
        pending.release_time_ns = current_time_ns_ + config_.latency_ns;
        pending.leg_count = static_cast<std::uint8_t>(cycle_.size() - 1U);
        pending.current_leg = 0U;
        pending.current_quantity = start_quantity;
        pending.current_asset = cycle_.front();
        pending.initial_quantity = start_quantity;
        for (std::size_t index = 0U; index < cycle_.size(); ++index) {
            pending.assets[index] = cycle_[index];
        }

        if (!pending_cycles_.push(pending)) {
            wallet_.release_reserved(quote_asset_id_, start_quantity);
            ++result.panic_closes;
            return;
        }
    }

    [[nodiscard]] double calculate_fee_aware_bottleneck() const noexcept {
        double cumulative_input = 1.0;
        const double available_quote = wallet_.free_balance(quote_asset_id_);
        double max_start = config_.max_cycle_notional_usdt > 0.0 ? config_.max_cycle_notional_usdt : available_quote;
        if (available_quote < max_start) {
            max_start = available_quote;
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

    void process_next_pending_cycle(Result& result) {
        PendingCycle pending = pending_cycles_.top();
        pending_cycles_.pop();
        current_time_ns_ = std::max(current_time_ns_, pending.release_time_ns);
        execute_pending_leg(pending, result);
    }

    void execute_pending_leg(PendingCycle& pending, Result& result) {
        if (pending.current_leg >= pending.leg_count) {
            if (pending.current_asset == quote_asset_id_) {
                ++result.completed_cycles;
                return;
            }
            panic_close_to_quote(pending.current_asset, pending.current_quantity, pending.initial_quantity, result);
            return;
        }

        rebuild_edges();
        const AssetID next_asset = pending.assets[pending.current_leg + 1U];
        const Edge* edge = edge_for(pending.current_asset, next_asset);
        const bool spending_reserved_quote = pending.current_leg == 0U && pending.current_asset == quote_asset_id_;
        const double spendable_balance = spending_reserved_quote
            ? wallet_.reserved(pending.current_asset)
            : wallet_.balance(pending.current_asset);
        if (
            edge == nullptr ||
            edge->available_from_qty + kEpsilon < pending.current_quantity ||
            spendable_balance + kEpsilon < pending.current_quantity
        ) {
            if (spending_reserved_quote) {
                wallet_.release_reserved(pending.current_asset, pending.initial_quantity);
            }
            panic_close_to_quote(pending.current_asset, pending.current_quantity, pending.initial_quantity, result);
            return;
        }

        if (spending_reserved_quote) {
            if (!wallet_.consume_reserved(pending.current_asset, pending.current_quantity)) {
                wallet_.release_reserved(pending.current_asset, pending.initial_quantity);
                panic_close_to_quote(pending.current_asset, pending.current_quantity, pending.initial_quantity, result);
                return;
            }
        } else {
            wallet_.sub_balance(pending.current_asset, pending.current_quantity);
        }

        const double gross_output = sweep_visible_depth(*edge, pending.current_quantity);
        pending.current_quantity = wallet_.apply_fill(next_asset, gross_output, config_.taker_fee_bps);
        pending.current_asset = next_asset;
        ++pending.current_leg;

        if (pending.current_leg >= pending.leg_count) {
            if (pending.current_asset == quote_asset_id_) {
                ++result.completed_cycles;
                return;
            }
            panic_close_to_quote(pending.current_asset, pending.current_quantity, pending.initial_quantity, result);
            return;
        }

        pending.release_time_ns += config_.intra_leg_latency_ns;
        if (!pending_cycles_.push(pending)) {
            panic_close_to_quote(pending.current_asset, pending.current_quantity, pending.initial_quantity, result);
        }
    }

    void panic_close_to_quote(AssetID asset_id, double quantity, double initial_quote, Result& result) {
        ++result.panic_closes;
        (void)initial_quote;
        if (asset_id == quote_asset_id_ || quantity <= 0.0) {
            return;
        }

        const double close_quantity = wallet_.balance(asset_id) < quantity ? wallet_.balance(asset_id) : quantity;
        if (close_quantity <= 0.0) {
            return;
        }

        const double recovered_quote = convert_to_quote(asset_id, close_quantity);
        if (recovered_quote <= 0.0) {
            return;
        }
        wallet_.sub_balance(asset_id, close_quantity);
        (void)wallet_.apply_fill(quote_asset_id_, recovered_quote, config_.taker_fee_bps);
    }

    [[nodiscard]] double convert_to_quote(AssetID asset_id, double quantity) noexcept {
        if (asset_id == quote_asset_id_) {
            return quantity;
        }

        if (const Edge* direct = edge_for(asset_id, quote_asset_id_)) {
            return execute_pessimistic_liquidation(*direct, quantity);
        }

        for (const Edge& first : edges_) {
            if (first.from != asset_id) {
                continue;
            }
            if (const Edge* second = edge_for(first.to, quote_asset_id_)) {
                const double intermediate = execute_pessimistic_liquidation(first, quantity);
                return execute_pessimistic_liquidation(*second, intermediate * taker_fee_multiplier());
            }
        }

        return 0.0;
    }

    [[nodiscard]] double execute_pessimistic_liquidation(const Edge& edge, double input_quantity) noexcept {
        static constexpr double kPanicSlippagePenalty = 0.005;
        const double visible_quantity = input_quantity < edge.available_from_qty ? input_quantity : edge.available_from_qty;
        const double excess_quantity = input_quantity - visible_quantity;
        const double visible_output = sweep_visible_depth(edge, visible_quantity);
        const double penalized_output = excess_quantity > 0.0
            ? excess_quantity * edge.rate * (1.0 - kPanicSlippagePenalty)
            : 0.0;
        deplete_unobserved_tail(edge, excess_quantity);
        return visible_output + penalized_output;
    }

    [[nodiscard]] double side_input_capacity(AssetID pair_id, bool use_bid) const noexcept {
        const PairState& pair = pair_states_[pair_id];
        const DepletionState& depletion = depletion_ledger_[pair_id];
        double capacity = 0.0;
        for (std::size_t level = 0U; level < L2Depth5Event::Depth; ++level) {
            if (use_bid) {
                const double effective_qty = pair.bid_qty[level] - depletion.bid_depleted_qty[level];
                if (pair.bid_prices[level] > 0.0 && effective_qty > 0.0) {
                    capacity += effective_qty;
                }
            } else {
                const double effective_qty = pair.ask_qty[level] - depletion.ask_depleted_qty[level];
                if (pair.ask_prices[level] > 0.0 && effective_qty > 0.0) {
                    capacity += pair.ask_prices[level] * effective_qty;
                }
            }
        }
        return capacity;
    }

    [[nodiscard]] double sweep_visible_depth(const Edge& edge, double input_quantity) noexcept {
        DepletionState& depletion = depletion_ledger_[edge.pair_id];
        const PairState& pair = pair_states_[edge.pair_id];
        double remaining = input_quantity;
        double output = 0.0;
        for (std::size_t level = 0U; level < L2Depth5Event::Depth && remaining > kEpsilon; ++level) {
            if (edge.use_bid) {
                const double effective_qty = pair.bid_qty[level] - depletion.bid_depleted_qty[level];
                if (pair.bid_prices[level] <= 0.0 || effective_qty <= 0.0) {
                    continue;
                }
                const double consumed = remaining < effective_qty ? remaining : effective_qty;
                depletion.bid_depleted_qty[level] += consumed;
                remaining -= consumed;
                output += consumed * pair.bid_prices[level];
            } else {
                const double effective_qty = pair.ask_qty[level] - depletion.ask_depleted_qty[level];
                if (pair.ask_prices[level] <= 0.0 || effective_qty <= 0.0) {
                    continue;
                }
                const double level_notional = pair.ask_prices[level] * effective_qty;
                const double consumed_quote = remaining < level_notional ? remaining : level_notional;
                const double consumed_base = consumed_quote / pair.ask_prices[level];
                depletion.ask_depleted_qty[level] += consumed_base;
                remaining -= consumed_quote;
                output += consumed_base;
            }
        }
        return output;
    }

    void deplete_unobserved_tail(const Edge& edge, double input_quantity) noexcept {
        if (input_quantity <= 0.0) {
            return;
        }
        DepletionState& depletion = depletion_ledger_[edge.pair_id];
        if (edge.use_bid) {
            depletion.bid_depleted_qty[L2Depth5Event::Depth - 1U] += input_quantity;
            return;
        }

        const PairState& pair = pair_states_[edge.pair_id];
        const double tail_price = pair.ask_prices[L2Depth5Event::Depth - 1U] > 0.0
            ? pair.ask_prices[L2Depth5Event::Depth - 1U]
            : pair.ask_prices[0U];
        if (tail_price > 0.0) {
            depletion.ask_depleted_qty[L2Depth5Event::Depth - 1U] += input_quantity / tail_price;
        }
    }

    [[nodiscard]] double taker_fee_multiplier() const noexcept {
        return 1.0 - (config_.taker_fee_bps * 0.0001);
    }

    [[nodiscard]] const Edge* edge_for(AssetID from, AssetID to) const noexcept {
        for (const Edge& edge : edges_) {
            if (edge.from == from && edge.to == to) {
                return &edge;
            }
        }
        return nullptr;
    }

    [[nodiscard]] bool cycle_passes_execution_filters() const noexcept {
        for (std::size_t index = 0U; index + 1U < cycle_.size(); ++index) {
            const Edge* edge = edge_for(cycle_[index], cycle_[index + 1U]);
            if (edge == nullptr) {
                return false;
            }

            const PairState& state = pair_states_[edge->pair_id];
            if (state.spread_bps > config_.max_spread_bps) {
                return false;
            }
            if (config_.min_depth_usdt > 0.0 && depth_to_quote(*edge) < config_.min_depth_usdt) {
                return false;
            }
            if (!obi_is_acceptable(*edge, state)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] double cycle_edge_bps() const noexcept {
        double gross_multiplier = 1.0;
        for (std::size_t index = 0U; index + 1U < cycle_.size(); ++index) {
            const Edge* edge = edge_for(cycle_[index], cycle_[index + 1U]);
            if (edge == nullptr || edge->rate <= 0.0) {
                return -std::numeric_limits<double>::infinity();
            }
            gross_multiplier *= edge->effective_rate;
        }
        return (gross_multiplier - 1.0) * 10'000.0;
    }

    [[nodiscard]] bool obi_is_acceptable(const Edge& edge, const PairState& state) const noexcept {
        if (config_.max_adverse_obi >= 1.0) {
            return true;
        }
        if (!edge.use_bid && state.obi > config_.max_adverse_obi) {
            return false;
        }
        if (edge.use_bid && state.obi < -config_.max_adverse_obi) {
            return false;
        }
        return true;
    }

    [[nodiscard]] double depth_to_quote(const Edge& edge) const noexcept {
        if (edge.from == quote_asset_id_) {
            return edge.available_from_qty;
        }
        if (edge.to == quote_asset_id_) {
            return edge.available_from_qty * edge.rate;
        }
        if (const Edge* to_quote = edge_for(edge.to, quote_asset_id_)) {
            return edge.available_from_qty * edge.rate * to_quote->rate;
        }
        if (const Edge* from_quote = edge_for(edge.from, quote_asset_id_)) {
            return edge.available_from_qty * from_quote->rate;
        }
        return 0.0;
    }

    void record_cycle_snapshot(bool executed) {
        CycleSnapshot snapshot {};
        snapshot.timestamp_ns = current_time_ns_;
        snapshot.executed = executed;
        snapshot.leg_count = static_cast<std::uint8_t>(cycle_.size() > 0U ? cycle_.size() - 1U : 0U);
        for (std::size_t index = 0U; index < cycle_.size() && index < snapshot.cycle_path.size(); ++index) {
            snapshot.cycle_path[index] = cycle_[index];
        }
        for (std::size_t index = 0U; index < snapshot.leg_count; ++index) {
            const Edge* edge = edge_for(cycle_[index], cycle_[index + 1U]);
            if (edge == nullptr) {
                snapshot.leg_obis[index] = 0.0;
                snapshot.leg_spreads_bps[index] = 0.0;
                continue;
            }

            const PairState& state = pair_states_[edge->pair_id];
            snapshot.leg_obis[index] = state.obi;
            snapshot.leg_spreads_bps[index] = state.spread_bps;
        }
        cycle_snapshots_.push(snapshot);
    }

    Config config_ {};
    AssetID quote_asset_id_ {};
    std::uint64_t current_time_ns_ {};
    bool running_ {};
    std::vector<PairConfig> pairs_ {};
    std::vector<PairState> pair_states_ {};
    std::vector<L2Depth5CsvParser> parsers_ {};
    std::vector<std::string> asset_names_ {};
    std::unordered_map<std::string, AssetID> asset_index_ {};
    std::vector<Edge> edges_ {};
    std::vector<std::vector<std::size_t>> adjacency_edges_ {};
    std::vector<double> rates_ {};
    std::vector<AssetID> predecessors_ {};
    std::vector<std::size_t> predecessor_edges_ {};
    std::vector<std::uint16_t> relax_counts_ {};
    std::vector<std::uint8_t> in_queue_ {};
    std::vector<AssetID> spfa_queue_ {};
    std::vector<AssetID> cycle_ {};
    std::vector<DepletionState> depletion_ledger_ {};
    std::vector<ParserEvent> parser_events_ {};
    CycleSnapshotRing cycle_snapshots_ {};
    DynamicWallet wallet_ {};
    PendingMinHeap<PendingCycle, kPendingCycleCapacity> pending_cycles_ {};
};

}  // namespace lob
