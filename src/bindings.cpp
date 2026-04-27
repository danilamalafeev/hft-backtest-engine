#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "lob/graph_arbitrage_engine.hpp"
#include "lob/multi_asset_backtest_engine.hpp"
#include "lob/triangular_arbitrage_strategy.hpp"
#include "lob/wallet.hpp"

namespace py = pybind11;

namespace {

class PyWallet {
public:
    PyWallet() = default;

    PyWallet(lob::Wallet wallet, double btc_usdt_mid, double eth_usdt_mid)
        : wallet_(wallet),
          btc_usdt_mid_(btc_usdt_mid),
          eth_usdt_mid_(eth_usdt_mid) {}

    [[nodiscard]] double usdt() const noexcept {
        return wallet_.usdt;
    }

    [[nodiscard]] double btc() const noexcept {
        return wallet_.btc;
    }

    [[nodiscard]] double eth() const noexcept {
        return wallet_.eth;
    }

    [[nodiscard]] double get_nav() const noexcept {
        return wallet_.mark_to_market_nav(btc_usdt_mid_, eth_usdt_mid_);
    }

    [[nodiscard]] double get_total_inventory_risk() const noexcept {
        return wallet_.get_total_inventory_risk(btc_usdt_mid_, eth_usdt_mid_);
    }

    [[nodiscard]] double balance(lob::AssetID asset_id) const {
        switch (asset_id) {
            case 0U:
                return wallet_.usdt;
            case 1U:
                return wallet_.btc;
            case 2U:
                return wallet_.eth;
            default:
                throw std::out_of_range("wallet asset_id must be 0=USDT, 1=BTC, or 2=ETH");
        }
    }

private:
    lob::Wallet wallet_ {};
    double btc_usdt_mid_ {};
    double eth_usdt_mid_ {};
};

class PyBacktestResult {
public:
    PyBacktestResult(const lob::MultiAssetBacktestEngine<3U>::Result& result)
        : wallet_(result.final_portfolio, result.btc_usdt_mid, result.eth_usdt_mid),
          nav_(result.final_mtm_nav_usdt),
          inventory_risk_(result.inventory_risk_usdt),
          events_processed_(result.events_processed),
          taker_notional_(result.execution.taker_notional),
          dropped_orders_(result.dropped_pending_orders) {}

    const PyWallet& wallet() const { return wallet_; }
    double nav() const { return nav_; }
    double inventory_risk() const { return inventory_risk_; }
    std::uint64_t events_processed() const { return events_processed_; }
    double taker_notional() const { return taker_notional_; }
    std::uint64_t dropped_orders() const { return dropped_orders_; }

private:
    PyWallet wallet_;
    double nav_;
    double inventory_risk_;
    std::uint64_t events_processed_;
    double taker_notional_;
    std::uint64_t dropped_orders_;
};

class PyGraphResult {
public:
    explicit PyGraphResult(lob::GraphArbitrageEngine::Result result)
        : result_(std::move(result)) {}

    std::uint64_t events_processed() const noexcept { return result_.events_processed; }
    std::uint64_t cycles_detected() const noexcept { return result_.cycles_detected; }
    std::uint64_t attempted_cycles() const noexcept { return result_.attempted_cycles; }
    std::uint64_t completed_cycles() const noexcept { return result_.completed_cycles; }
    std::uint64_t panic_closes() const noexcept { return result_.panic_closes; }
    double final_usdt() const noexcept { return result_.final_usdt; }
    double final_nav() const noexcept { return result_.final_nav; }
    double inventory_risk() const noexcept { return result_.inventory_risk; }
    const std::vector<double>& balances() const noexcept { return result_.balances; }
    const std::vector<lob::AssetID>& last_cycle() const noexcept { return result_.last_cycle; }

private:
    lob::GraphArbitrageEngine::Result result_ {};
};

class PyGraphEngine {
public:
    PyGraphEngine(
        double initial_usdt,
        std::uint64_t latency_ns,
        std::uint64_t intra_leg_latency_ns,
        double taker_fee_bps,
        double max_cycle_notional_usdt
    )
        : engine_(initial_usdt, latency_ns, intra_leg_latency_ns, taker_fee_bps, max_cycle_notional_usdt) {}

    lob::AssetID add_pair(const std::string& base_asset, const std::string& quote_asset, const std::string& csv_file_path) {
        return engine_.add_pair(base_asset, quote_asset, csv_file_path);
    }

    [[nodiscard]] PyGraphResult run() {
        return PyGraphResult {engine_.run()};
    }

    [[nodiscard]] const std::vector<std::string>& assets() const noexcept {
        return engine_.assets();
    }

private:
    lob::GraphArbitrageEngine engine_;
};

class PyTriangularEngine {
public:
    PyTriangularEngine(std::uint64_t latency_ns, double maker_fee_bps, double taker_fee_bps, bool verbose)
        : latency_ns_(latency_ns), maker_fee_bps_(maker_fee_bps), taker_fee_bps_(taker_fee_bps), verbose_(verbose) {}

    [[nodiscard]] PyBacktestResult run(const std::vector<std::string>& file_paths) {
        if (file_paths.size() != 3U) {
            throw std::invalid_argument("TriangularEngine.run expects exactly 3 CSV file paths");
        }

        using Engine = lob::MultiAssetBacktestEngine<3U>;

        lob::TriangularArbitrageStrategy strategy {lob::TriangularArbitrageStrategy::Config {
            .taker_fee_bps = taker_fee_bps_,
            .verbose = verbose_,
        }};

        Engine engine {strategy, Engine::PathArray {
            file_paths[0],
            file_paths[1],
            file_paths[2],
        }, Engine::Config {
            .maker_fee_bps = maker_fee_bps_,
            .taker_fee_bps = taker_fee_bps_,
            .latency = Engine::LatencyConfig {
                .base_latency_ns = latency_ns_,
            },
        }};

        if (record_microstructure_) {
            engine.enable_microstructure_recording(true, sampling_interval_ns_);
        }

        PyBacktestResult result {engine.run()};
        if (record_microstructure_) {
            snapshots_ = engine.take_microstructure_snapshots();
        } else {
            snapshots_.clear();
        }
        return result;
    }

    void enable_microstructure_recording(bool enable, std::uint64_t sampling_interval_ns = 0U) {
        record_microstructure_ = enable;
        sampling_interval_ns_ = sampling_interval_ns;
        snapshots_.clear();
    }

    [[nodiscard]] py::dict get_microstructure_dataframe() const {
        const py::ssize_t size = static_cast<py::ssize_t>(snapshots_.size());
        py::array_t<std::uint64_t> timestamps {size};
        py::array_t<std::uint32_t> asset_ids {size};
        py::array_t<double> bid_prices {size};
        py::array_t<double> bid_quantities {size};
        py::array_t<double> ask_prices {size};
        py::array_t<double> ask_quantities {size};

        auto* timestamp_ptr = timestamps.mutable_data();
        auto* asset_id_ptr = asset_ids.mutable_data();
        auto* bid_price_ptr = bid_prices.mutable_data();
        auto* bid_quantity_ptr = bid_quantities.mutable_data();
        auto* ask_price_ptr = ask_prices.mutable_data();
        auto* ask_quantity_ptr = ask_quantities.mutable_data();

        for (py::ssize_t index = 0; index < size; ++index) {
            const lob::MarketSnapshot& snapshot = snapshots_[static_cast<std::size_t>(index)];
            timestamp_ptr[index] = snapshot.ts;
            asset_id_ptr[index] = snapshot.asset;
            bid_price_ptr[index] = snapshot.bid_p;
            bid_quantity_ptr[index] = snapshot.bid_q;
            ask_price_ptr[index] = snapshot.ask_p;
            ask_quantity_ptr[index] = snapshot.ask_q;
        }

        py::dict frame {};
        frame["timestamp"] = std::move(timestamps);
        frame["asset_id"] = std::move(asset_ids);
        frame["bid_price"] = std::move(bid_prices);
        frame["bid_qty"] = std::move(bid_quantities);
        frame["ask_price"] = std::move(ask_prices);
        frame["ask_qty"] = std::move(ask_quantities);
        return frame;
    }

private:
    std::uint64_t latency_ns_ {};
    double maker_fee_bps_ {};
    double taker_fee_bps_ {};
    bool verbose_ {};
    bool record_microstructure_ {};
    std::uint64_t sampling_interval_ns_ {};
    std::vector<lob::MarketSnapshot> snapshots_ {};
};

PyBacktestResult run_triangular(
    const std::vector<std::string>& file_paths,
    std::uint64_t latency_ns,
    double maker_fee_bps,
    double taker_fee_bps,
    bool verbose
) {
    PyTriangularEngine engine(latency_ns, maker_fee_bps, taker_fee_bps, verbose);
    return engine.run(file_paths);
}

}  // namespace

PYBIND11_MODULE(yabe, module) {
    module.doc() = "YABE: Yet Another Backtest Engine Python bindings";

    py::class_<PyWallet>(module, "Wallet")
        .def(py::init<>())
        .def_property_readonly("usdt", &PyWallet::usdt)
        .def_property_readonly("btc", &PyWallet::btc)
        .def_property_readonly("eth", &PyWallet::eth)
        .def("balance", &PyWallet::balance, py::arg("asset_id"))
        .def("get_nav", &PyWallet::get_nav)
        .def("get_total_inventory_risk", &PyWallet::get_total_inventory_risk);

    py::class_<PyBacktestResult>(module, "BacktestResult")
        .def_property_readonly("wallet", &PyBacktestResult::wallet)
        .def_property_readonly("nav", &PyBacktestResult::nav)
        .def_property_readonly("inventory_risk", &PyBacktestResult::inventory_risk)
        .def_property_readonly("events_processed", &PyBacktestResult::events_processed)
        .def_property_readonly("taker_notional", &PyBacktestResult::taker_notional)
        .def_property_readonly("dropped_orders", &PyBacktestResult::dropped_orders);

    py::class_<PyGraphResult>(module, "GraphResult")
        .def_property_readonly("events_processed", &PyGraphResult::events_processed)
        .def_property_readonly("cycles_detected", &PyGraphResult::cycles_detected)
        .def_property_readonly("attempted_cycles", &PyGraphResult::attempted_cycles)
        .def_property_readonly("completed_cycles", &PyGraphResult::completed_cycles)
        .def_property_readonly("panic_closes", &PyGraphResult::panic_closes)
        .def_property_readonly("final_usdt", &PyGraphResult::final_usdt)
        .def_property_readonly("final_nav", &PyGraphResult::final_nav)
        .def_property_readonly("inventory_risk", &PyGraphResult::inventory_risk)
        .def_property_readonly("balances", &PyGraphResult::balances)
        .def_property_readonly("last_cycle", &PyGraphResult::last_cycle);

    py::class_<PyGraphEngine>(module, "GraphEngine")
        .def(
            py::init<double, std::uint64_t, std::uint64_t, double, double>(),
            py::arg("initial_usdt") = 100'000'000.0,
            py::arg("latency_ns") = 0,
            py::arg("intra_leg_latency_ns") = 75,
            py::arg("taker_fee_bps") = 0.0,
            py::arg("max_cycle_notional_usdt") = 1'000.0
        )
        .def("add_pair", &PyGraphEngine::add_pair, py::arg("base_asset"), py::arg("quote_asset"), py::arg("csv_file_path"))
        .def_property_readonly("assets", &PyGraphEngine::assets)
        .def("run", &PyGraphEngine::run, py::call_guard<py::gil_scoped_release>());

    py::class_<PyTriangularEngine>(module, "TriangularEngine")
        .def(py::init<std::uint64_t, double, double, bool>(),
             py::arg("latency_ns") = 0,
             py::arg("maker_fee_bps") = 0.0,
             py::arg("taker_fee_bps") = 0.0,
             py::arg("verbose") = false)
        .def(
            "run",
            &PyTriangularEngine::run,
            py::arg("file_paths"),
            py::call_guard<py::gil_scoped_release>()
        )
        .def(
            "enable_microstructure_recording",
            &PyTriangularEngine::enable_microstructure_recording,
            py::arg("enable"),
            py::arg("sampling_interval_ns") = 0
        )
        .def("get_microstructure_dataframe", &PyTriangularEngine::get_microstructure_dataframe);

    module.def("run_triangular", &run_triangular,
               py::arg("file_paths"),
               py::arg("latency_ns") = 0,
               py::arg("maker_fee_bps") = 0.0,
               py::arg("taker_fee_bps") = 0.0,
               py::arg("verbose") = false,
               py::call_guard<py::gil_scoped_release>(),
               "Run a triangular arbitrage backtest.");
}
