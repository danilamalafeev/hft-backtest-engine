# YABE: Yet Another Backtest Engine

YABE is a high-performance C++20 limit order book (LOB), multi-asset mmap replay engine, and quantitative backtesting framework built for market microstructure research and atomic execution strategies like Triangular Arbitrage. The engine is strictly allocation-free on the hot path and now ships with a `pybind11` Python module for research workflows.

## Overview

The engine merges multiple Binance historical trade streams in real-time, matching orders with price-time priority. It supports complex OMS features like atomic group execution, intra-leg latency simulation, fee-aware sizing, and lock-free asynchronous logging.

Key technical highlights:
- **Zero-Copy Ingestion**: POSIX `mmap` combined with custom C++20 `StreamingParser` concepts for maximum speed.
- **Allocation-Free Event Merger**: Uses a fast-path flat scan for `N <= 4` assets and a `std::priority_queue` backed by a pre-reserved `std::vector` for dynamic scaling (`N > 4`).
- **Asynchronous Lock-Free Logging**: SPSC (Single-Producer Single-Consumer) ring buffer without syscalls on the hot path. Background thread batches disk writes and supports CPU core pinning.
- **Multi-Currency Wallet**: Native tracking of USDT, BTC, ETH, spot fee deduction (from the received asset), and Mark-to-Market (MTM) NAV calculation.
- **Advanced OMS**: Supports Fill-Or-Kill (FOK), `OrderGroup` atomic execution, slippage limits, and `panic_close_group` (reverse market sweeps for failed legs).

## Architecture

Core modules:
- `lob::MultiAssetBacktestEngine`: Orchestrates the order books, event merger, order execution, latency simulation, and portfolio tracking.
- `lob::OrderGateway` / `lob::oms`: Gateway interface defining atomic execution `OrderGroup` logic, FOK sweeps, and execution reports.
- `lob::TriangularArbitrageStrategy`: Strategy implementation modeling USDT -> BTC -> ETH -> USDT cycles with bottleneck volume calculation and post-fee threshold checks.
- `lob::AsyncLogger`: High-throughput background CSV writer for full event-level trace data.
- `lob::Wallet`: Fee-aware asset inventory state and risk management.
- `yabe` Python module: Thin `pybind11` binding exposing `TriangularEngine` and final `Wallet` state.

## Strategy: Triangular Arbitrage

The `TriangularArbitrageStrategy` monitors 3 pairs (e.g., BTCUSDT, ETHUSDT, ETHBTC) and detects cyclic mispricings.

Features:
- **Bottleneck Sizing**: Dynamically calculates the maximum executable volume across the 3 order books based on available L2 depth.
- **Fee-Aware Sizing**: Anticipates taker fees deducted by the `Wallet` from the received asset. Sizes subsequent legs exactly to the post-fee received amount to completely eliminate dust accumulation.
- **Atomic Execution**: Submits all 3 legs via `OrderGroup`. If any leg fails or experiences severe slippage, the OMS triggers a panic reverse-sweep to close the partial exposure.
- **Intra-Leg Latency**: Leg execution is separated by simulated nanosecond-level latency and exponential/lognormal jitter.

## Performance

Tested locally in a `Release` build on Apple Silicon.

Recent Triangular Arbitrage run (3 assets):
- Events Processed: `~9,000,000`
- Atomic Group Attempts: `~56,000`
- Throughput: `> 4.5M EPS` (Events Per Second) WITH full asynchronous logging active.
- Async Dropped Records: `0`
- Final Inventory Risk (Dust): `< $10.00` (Proof of correct fee-aware math).

## Quick Start

```bash
# Build the project
cmake -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release

# Run unit tests
ctest --test-dir build-release --output-on-failure

# Run backtest with async logging enabled
./build-release/lob_backtest \
    BTCUSDT-trades.csv \
    ETHUSDT-trades.csv \
    ETHBTC-trades.csv \
    --async-log /tmp/async_log.csv
```

The backtester will output a human-readable summary, execution diagnostics, per-asset breakdown, and a final machine-readable `RESULT_TRI_CSV`.

## Python API

Build the Python extension:

```bash
cmake -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target yabe
```

The module is emitted as:

```bash
build-release/yabe.so
```

Use it from Python by adding the build directory to `PYTHONPATH`:

```bash
PYTHONPATH=build-release python3 - <<'PY'
import yabe

engine = yabe.TriangularEngine()
engine.set_latency_ns(500_000)
engine.set_fee_rate(7.5)  # bps, same convention as the C++ CLI
# engine.set_verbose(True)  # Optional: prints every detected arbitrage window.

wallet = engine.run([
    "data/BTCUSDT-trades-2024-03-05.csv",
    "data/ETHUSDT-trades-2024-03-05.csv",
    "data/ETHBTC-trades-2024-03-05.csv",
])

print("USDT:", wallet.usdt)
print("BTC:", wallet.btc)
print("ETH:", wallet.eth)
print("NAV:", wallet.get_nav())
print("Inventory risk:", wallet.get_total_inventory_risk())
PY
```

`Wallet.balance(asset_id)` uses the Python-facing wallet ids:
- `0 = USDT`
- `1 = BTC`
- `2 = ETH`

`TriangularEngine.run(...)` is quiet by default and releases the Python GIL while the C++ replay loop is running, so Python threads are not blocked by the hot C++ simulation.

## Latency and OMS Simulation

The engine realistically models network and matching engine conditions:

- `--maker-fee-bps` / `--taker-fee-bps`: Exchange spot fees.
- `--base-latency-ns`: Deterministic base latency added to network requests.
- `--latency-dist`: Distributions (`none`, `exponential`, `lognormal`) for simulating network jitter.
- Atomic triangular execution also applies a nanosecond-scale intra-leg latency and jitter between legs inside the C++ OMS.

Orders and groups are held in a static `PendingOrderMinHeap` and released when virtual time catches up.

## Visualization Dashboard

The backtest outputs an asynchronous log containing every fill, panic close, and MTM NAV update. You can build a 3-panel Plotly dashboard:

```bash
python3 scripts/analyze_results.py /tmp/async_log.csv -o /tmp/hft_dashboard.html
```

The generated `hft_dashboard.html` features:
1. **Total Equity / NAV**: Mark-to-Market portfolio value over virtual time.
2. **Prices & Trade Markers**: Mid-prices with color-coded BUY/SELL `go.Scattergl` markers.
3. **Inventory Trace**: Cumulative position for each asset, validating the zero-dust fee-aware sizing.
