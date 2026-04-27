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
- `lob::GraphArbitrageEngine`: Dynamic N-asset graph arbitrage engine over L1/bookTicker style data, using Bellman-Ford negative-cycle detection.
- `lob::OrderGateway` / `lob::oms`: Gateway interface defining atomic execution `OrderGroup` logic, FOK sweeps, and execution reports.
- `lob::TriangularArbitrageStrategy`: Strategy implementation modeling USDT -> BTC -> ETH -> USDT cycles with bottleneck volume calculation and post-fee threshold checks.
- `lob::AsyncLogger`: High-throughput background CSV writer for full event-level trace data.
- `lob::Wallet`: Fee-aware asset inventory state and risk management.
- `lob::DynamicWallet`: Runtime-sized wallet for arbitrary graph assets, exposed through `GraphResult.balances`.
- `yabe` Python module: Thin `pybind11` binding exposing `run_triangular`, `TriangularEngine`, `GraphEngine`, and result objects for research workflows.

## Strategy: Triangular Arbitrage

The `TriangularArbitrageStrategy` monitors 3 pairs (e.g., BTCUSDT, ETHUSDT, ETHBTC) and detects cyclic mispricings.

Features:
- **Bottleneck Sizing**: Dynamically calculates the maximum executable volume across the 3 order books based on available L2 depth.
- **Fee-Aware Sizing**: Anticipates taker fees deducted by the `Wallet` from the received asset. Sizes subsequent legs exactly to the post-fee received amount to completely eliminate dust accumulation.
- **Atomic Execution**: Submits all 3 legs via `OrderGroup`. If any leg fails or experiences severe slippage, the OMS triggers a panic reverse-sweep to close the partial exposure.
- **Intra-Leg Latency**: Leg execution is separated by simulated nanosecond-level latency and exponential/lognormal jitter.

## Dynamic Graph Arbitrage

`GraphEngine` is the research path for N-leg arbitrage. Instead of hardcoding BTC/ETH/USDT, pairs are added dynamically from Python:

```python
import yabe

engine = yabe.GraphEngine(
    initial_usdt=100_000_000,
    latency_ns=500_000,
    intra_leg_latency_ns=75,
    taker_fee_bps=7.5,
    max_cycle_notional_usdt=1_000,
    max_adverse_obi=0.2,
    min_cycle_edge_bps=0.5,
)

engine.add_pair("BTC", "USDT", "data/BTCUSDT-bookTicker-2024-03-05.csv")
engine.add_pair("ETH", "USDT", "data/ETHUSDT-bookTicker-2024-03-05.csv")
engine.add_pair("ETH", "BTC", "data/ETHBTC-bookTicker-2024-03-05.csv")

result = engine.run()

print(engine.assets)
print(result.final_nav, result.inventory_risk, result.balances)
```

The graph engine currently supports:
- Bellman-Ford negative-cycle detection.
- Dynamic cycle rotation so tradable cycles start/end in `USDT`.
- Fee-aware bottleneck sizing.
- Latency-delayed pending cycle execution.
- Per-snapshot liquidity depletion ledger, so virtual fills cannot repeatedly consume the same visible BBO size.
- Dynamic wallet balances for arbitrary assets.
- Pessimistic panic close with a penalty for size beyond visible top-of-book liquidity.
- Smart order router filters: adverse OBI, spread, visible depth, and minimum theoretical cycle edge after taker fees.

For CLI-style research runs:

```bash
PYTHONPATH=build-release python3 scripts/run_graph_arbitrage.py \
  --auto-triangle \
  --date 2024-03-05 \
  --latency-ns 500000 \
  --taker-fee-bps 7.5 \
  --max-adverse-obi 0.2 \
  --min-cycle-edge-bps 0.5 \
  --summary-csv results/graph_run.csv
```

`GraphResult` exposes:
- `events_processed`
- `cycles_detected`
- `attempted_cycles`
- `completed_cycles`
- `panic_closes`
- `final_usdt`
- `final_nav`
- `inventory_risk`
- `balances`
- `last_cycle`

Key execution filters:
- `--max-adverse-obi`: rejects buys when bid-side imbalance is too strong and rejects sells when ask-side pressure is too strong. `1.0` disables the filter.
- `--max-spread-bps`: rejects cycles touching pairs with wide spreads. `1000` is effectively permissive for most liquid markets.
- `--min-depth-usdt`: rejects cycles when visible top-of-book depth is too shallow in USDT terms.
- `--min-cycle-edge-bps`: requires a minimum theoretical cycle edge after taker fees before the cycle is sent to the latency heap.

## Data Sources

YABE supports two data paths:

1. Trade replay for the older `TriangularEngine`.
2. L1/bookTicker-style replay for the newer `GraphEngine`.

The `GraphEngine` L1 CSV format is:

```csv
timestamp,bid_price,bid_qty,ask_price,ask_qty
```

### Spot Data Caveat

Binance Vision provides spot daily `trades`, but spot daily `bookTicker` archives are not available for the BTCUSDT/ETHUSDT/ETHBTC path used in examples. The helper script therefore tries real spot `bookTicker` first and, if it receives `404`, falls back to a synthetic BBO generated from spot trades:

```bash
python3 scripts/download_bookticker_data.py \
  --date 2024-03-05 \
  --symbols BTCUSDT ETHUSDT ETHBTC
```

This writes:

```text
data/BTCUSDT-bookTicker-2024-03-05.csv
data/ETHUSDT-bookTicker-2024-03-05.csv
data/ETHBTC-bookTicker-2024-03-05.csv
```

The fallback is useful for pipeline testing, Python API work, and stress-testing the OMS. It is not a real historical L2 dataset and should not be used for final quantitative conclusions.

### Futures and External L2 Data

Binance Vision does provide `bookTicker` archives for some futures markets, for example under:

```text
data/futures/um/daily/bookTicker/
```

Those files can be normalized to the same `timestamp,bid_price,bid_qty,ask_price,ask_qty` schema and fed into `GraphEngine`. External feeds such as Hyperliquid data should use the same schema for plug-and-play replay. Once the CSV is normalized, add pairs manually:

```bash
PYTHONPATH=build-release python3 scripts/run_graph_arbitrage.py \
  --pair BTC/USDT:data/BTCUSDT-bookTicker-2024-03-05.csv \
  --pair ETH/USDT:data/ETHUSDT-bookTicker-2024-03-05.csv \
  --pair ETH/BTC:data/ETHBTC-bookTicker-2024-03-05.csv
```

### Tardis.dev Tick L2

For higher fidelity research, Tardis.dev exposes free historical CSV datasets for the first day of each month. Download incremental L2 diffs:

```bash
python3 scripts/download_tardis.py \
  --exchange binance \
  --date 2024-03-01 \
  --symbols BTCUSDT ETHUSDT ETHBTC
```

Convert diffs to YABE's BBO schema:

```bash
python3 scripts/tardis_to_bookticker.py \
  data/tardis/tardis-binance-BTCUSDT-incremental_book_L2-2024-03-01.csv.gz \
  -o data/tardis/tardis-binance-BTCUSDT-bbo-2024-03-01.csv

python3 scripts/tardis_to_bookticker.py \
  data/tardis/tardis-binance-ETHUSDT-incremental_book_L2-2024-03-01.csv.gz \
  -o data/tardis/tardis-binance-ETHUSDT-bbo-2024-03-01.csv

python3 scripts/tardis_to_bookticker.py \
  data/tardis/tardis-binance-ETHBTC-incremental_book_L2-2024-03-01.csv.gz \
  -o data/tardis/tardis-binance-ETHBTC-bbo-2024-03-01.csv
```

Then run the graph engine:

```bash
PYTHONPATH=build-release python3 scripts/run_graph_arbitrage.py \
  --pair BTC/USDT:data/tardis/tardis-binance-BTCUSDT-bbo-2024-03-01.csv \
  --pair ETH/USDT:data/tardis/tardis-binance-ETHUSDT-bbo-2024-03-01.csv \
  --pair ETH/BTC:data/tardis/tardis-binance-ETHBTC-bbo-2024-03-01.csv \
  --latency-ns 500000 \
  --taker-fee-bps 7.5 \
  --max-adverse-obi 0.2 \
  --min-cycle-edge-bps 0.5
```

The converter reconstructs the local book from `incremental_book_L2` rows, skips pre-snapshot diffs, batches rows by `local_timestamp`, and emits a BBO row only when best price or size changes.

Recent Tardis spot run:

```text
BTCUSDT: 23,766,560 diff rows -> 667,419 BBO rows
ETHUSDT: 20,236,010 diff rows -> 606,780 BBO rows
ETHBTC: 1,625,906 diff rows -> 225,029 BBO rows
GraphEngine replay: 1,499,228 events at ~3M-4M EPS
```

Current best small-grid result on `2024-03-01` Tardis spot BBO:

```text
latency_ns=500000
taker_fee_bps=1.0
max_adverse_obi=0.2
min_cycle_edge_bps=0.5
PnL=+$106.45
completed=410
panic=5
```

Run that configuration:

```bash
PYTHONPATH=build-release python3 scripts/run_graph_arbitrage.py \
  --pair BTC/USDT:data/tardis/tardis-binance-BTCUSDT-bbo-2024-03-01.csv \
  --pair ETH/USDT:data/tardis/tardis-binance-ETHUSDT-bbo-2024-03-01.csv \
  --pair ETH/BTC:data/tardis/tardis-binance-ETHBTC-bbo-2024-03-01.csv \
  --latency-ns 500000 \
  --taker-fee-bps 1.0 \
  --max-adverse-obi 0.2 \
  --min-cycle-edge-bps 0.5 \
  --summary-csv results/tardis_best_graph_2024-03-01.csv
```

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

Use it from Python by adding the build directory to `PYTHONPATH`. The high-level research API is `run_triangular(...)`, which behaves like a pure function: input files plus parameters produce a `BacktestResult`.

```bash
PYTHONPATH=build-release python3 - <<'PY'
import yabe

paths = [
    "data/BTCUSDT-trades-2024-03-05.csv",
    "data/ETHUSDT-trades-2024-03-05.csv",
    "data/ETHBTC-trades-2024-03-05.csv",
]

result = yabe.run_triangular(
    paths,
    latency_ns=500_000,
    maker_fee_bps=-1.0,
    taker_fee_bps=7.5,
    verbose=False,
)

wallet = result.wallet

print("USDT:", wallet.usdt)
print("BTC:", wallet.btc)
print("ETH:", wallet.eth)
print("NAV:", result.nav)
print("Inventory risk:", result.inventory_risk)
print("Events:", result.events_processed)
PY
```

`BacktestResult` exposes:
- `wallet`
- `nav`
- `inventory_risk`
- `events_processed`
- `taker_notional`
- `dropped_orders`

`Wallet.balance(asset_id)` uses the Python-facing wallet ids:
- `0 = USDT`
- `1 = BTC`
- `2 = ETH`

For advanced use, `TriangularEngine` can also be configured immutably via its constructor:

```python
engine = yabe.TriangularEngine(
    latency_ns=500_000,
    maker_fee_bps=-1.0,
    taker_fee_bps=7.5,
    verbose=False,
)

result = engine.run(paths)
```

Both `run_triangular(...)` and `TriangularEngine.run(...)` are quiet by default and release the Python GIL while the C++ replay loop is running. This makes threaded parameter sweeps possible, though large CSV replays are also limited by memory bandwidth and page-cache pressure.

### Microstructure Snapshots

For L1 research features such as OFI, BBO imbalance, and short-horizon momentum, use `TriangularEngine` and enable microstructure recording before `run(...)`:

```python
import pandas as pd
import yabe

engine = yabe.TriangularEngine(
    latency_ns=500_000,
    maker_fee_bps=-1.0,
    taker_fee_bps=7.5,
)

# 0 records every update. Use a positive interval to control RAM usage.
engine.enable_microstructure_recording(True, sampling_interval_ns=1_000_000_000)
result = engine.run(paths)

arrays = engine.get_microstructure_dataframe()
df = pd.DataFrame(arrays)

df["mid"] = (df["bid_price"] + df["ask_price"]) * 0.5
df["imbalance"] = (df["bid_qty"] - df["ask_qty"]) / (df["bid_qty"] + df["ask_qty"])
```

`get_microstructure_dataframe()` returns a dictionary of NumPy arrays, not Python objects:
- `timestamp`
- `asset_id`
- `bid_price`
- `bid_qty`
- `ask_price`
- `ask_qty`

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
