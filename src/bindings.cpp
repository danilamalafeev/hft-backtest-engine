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

class PyTriangularEngine {
public:
    PyTriangularEngine() = default;

    void set_latency_ns(std::uint64_t latency_ns) noexcept {
        latency_ns_ = latency_ns;
    }

    void set_fee_rate(double fee_rate) noexcept {
        fee_rate_bps_ = fee_rate;
    }

    void set_verbose(bool verbose) noexcept {
        verbose_ = verbose;
    }

    [[nodiscard]] PyWallet run(const std::vector<std::string>& file_paths) const {
        if (file_paths.size() != 3U) {
            throw std::invalid_argument("TriangularEngine.run expects exactly 3 CSV file paths");
        }

        using Engine = lob::MultiAssetBacktestEngine<3U>;

        lob::TriangularArbitrageStrategy strategy {lob::TriangularArbitrageStrategy::Config {
            .taker_fee_bps = fee_rate_bps_,
            .verbose = verbose_,
        }};

        Engine engine {strategy, Engine::PathArray {
            file_paths[0],
            file_paths[1],
            file_paths[2],
        }, Engine::Config {
            .maker_fee_bps = fee_rate_bps_,
            .taker_fee_bps = fee_rate_bps_,
            .latency = Engine::LatencyConfig {
                .base_latency_ns = latency_ns_,
            },
        }};

        const Engine::Result result = engine.run();
        return PyWallet {result.final_portfolio, result.btc_usdt_mid, result.eth_usdt_mid};
    }

private:
    std::uint64_t latency_ns_ {};
    double fee_rate_bps_ {};
    bool verbose_ {};
};

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

    py::class_<PyTriangularEngine>(module, "TriangularEngine")
        .def(py::init<>())
        .def("set_latency_ns", &PyTriangularEngine::set_latency_ns, py::arg("ns"))
        .def("set_fee_rate", &PyTriangularEngine::set_fee_rate, py::arg("rate"))
        .def("set_verbose", &PyTriangularEngine::set_verbose, py::arg("verbose"))
        .def(
            "run",
            &PyTriangularEngine::run,
            py::arg("file_paths"),
            py::call_guard<py::gil_scoped_release>()
        );
}
