#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

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

class PyTriangularEngine {
public:
    PyTriangularEngine(std::uint64_t latency_ns, double maker_fee_bps, double taker_fee_bps, bool verbose)
        : latency_ns_(latency_ns), maker_fee_bps_(maker_fee_bps), taker_fee_bps_(taker_fee_bps), verbose_(verbose) {}

    [[nodiscard]] PyBacktestResult run(const std::vector<std::string>& file_paths) const {
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

        return PyBacktestResult{engine.run()};
    }

private:
    std::uint64_t latency_ns_ {};
    double maker_fee_bps_ {};
    double taker_fee_bps_ {};
    bool verbose_ {};
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
        );

    module.def("run_triangular", &run_triangular,
               py::arg("file_paths"),
               py::arg("latency_ns") = 0,
               py::arg("maker_fee_bps") = 0.0,
               py::arg("taker_fee_bps") = 0.0,
               py::arg("verbose") = false,
               py::call_guard<py::gil_scoped_release>(),
               "Run a triangular arbitrage backtest.");
}
