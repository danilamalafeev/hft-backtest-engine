# YABE: Yet Another Backtest Engine

YABE is a high-performance C++20 limit order book (LOB), multi-asset mmap replay engine, and quantitative backtesting framework built for market microstructure research, N-leg graph arbitrage, and atomic execution strategies. The current research path supports raw L2 update replay with capped visible depth, dense or sparse graph lookup policies, and a `pybind11` Python module for Python/NumPy/Pandas workflows.

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
- `lob::GraphArbitrageEngine`: Default dense-lookup N-asset graph arbitrage engine for small graphs.
- `lob::GraphArbitrageEngineLarge`: Sparse-lookup graph arbitrage engine for larger sparse asset/product graphs.
- `lob::GraphArbitrageEngineT<LookupPolicy>`: Compile-time lookup-policy template behind both graph engines.
- `lob::L2OrderBook`: Capped top-N visible L2 execution view fed by raw `L2Update` rows.
- `lob::ReplaySequenceValidator` / venue replay primitives: Foundation types for future venue-normalized immutable replay logs.
- `lob::VenueManifest`: Foundation manifest types for assets, products, tick/lot scales, cost models, and settlement domains.
- `lob::OrderGateway` / `lob::oms`: Gateway interface defining atomic execution `OrderGroup` logic, FOK sweeps, and execution reports.
- `lob::TriangularArbitrageStrategy`: Strategy implementation modeling USDT -> BTC -> ETH -> USDT cycles with bottleneck volume calculation and post-fee threshold checks.
- `lob::AsyncLogger`: High-throughput background CSV writer for full event-level trace data.
- `lob::Wallet`: Fee-aware asset inventory state and risk management.
- `lob::DynamicWallet`: Runtime-sized wallet for arbitrary graph assets, exposed through `GraphResult.balances`.
- `yabe` Python module: Thin `pybind11` binding exposing `run_triangular`, `TriangularEngine`, `GraphEngine`, `GraphEngineLarge`, and result objects for research workflows.

## Strategy: Triangular Arbitrage

The `TriangularArbitrageStrategy` monitors 3 pairs (e.g., BTCUSDT, ETHUSDT, ETHBTC) and detects cyclic mispricings.

Features:
- **Bottleneck Sizing**: Dynamically calculates the maximum executable volume across the 3 order books based on available L2 depth.
- **Fee-Aware Sizing**: Anticipates taker fees deducted by the `Wallet` from the received asset. Sizes subsequent legs exactly to the post-fee received amount to completely eliminate dust accumulation.
- **Atomic Execution**: Submits all 3 legs via `OrderGroup`. If any leg fails or experiences severe slippage, the OMS triggers a panic reverse-sweep to close the partial exposure.
- **Intra-Leg Latency**: Leg execution is separated by simulated nanosecond-level latency and exponential/lognormal jitter.

## Dynamic Graph Arbitrage

`GraphEngine` is the default research path for N-leg arbitrage on small dense graphs. `GraphEngineLarge` exposes the same Python API but uses the sparse lookup policy, which is better for larger sparse product graphs and is also competitive on the current 3-asset triangle.

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
    max_book_levels_per_side=20,
)

engine.add_pair("BTC", "USDT", "data/tardis/BTCUSDT-l2update-2024-03-01.csv")
engine.add_pair("ETH", "USDT", "data/tardis/ETHUSDT-l2update-2024-03-01.csv")
engine.add_pair("ETH", "BTC", "data/tardis/ETHBTC-l2update-2024-03-01.csv")

result = engine.run()

print(engine.assets)
print(result.final_nav, result.inventory_risk, result.balances)
```

The graph engine currently supports:
- Bellman-Ford negative-cycle detection.
- Compile-time lookup policies:
  - `GraphEngine`: dense O(1) lookup tables, best for small graphs.
  - `GraphEngineLarge`: sparse adjacency/route lookup, best for large sparse graphs.
- Stable graph topology: directed edges are built once, then rates/capacities update in place.
- Raw L2Update replay into capped top-N visible books via `max_book_levels_per_side`.
- Dynamic cycle rotation so tradable cycles start/end in `USDT`.
- Fee-aware bottleneck sizing.
- Latency-delayed pending cycle execution.
- Current-depth execution revalidation before each pending leg.
- Non-mutating quote simulation at signal time; live book depletion happens only at execution time.
- Dynamic wallet balances for arbitrary assets.
- Pessimistic panic close with a penalty for size beyond visible top-of-book liquidity.
- Smart order router filters: adverse OBI, spread, visible depth, and minimum theoretical cycle edge after taker fees.

For CLI-style research runs:

```bash
PYTHONPATH=build-release python3 scripts/run_graph_arbitrage.py \
  --pair BTC/USDT:data/tardis/BTCUSDT-l2update-2024-03-01.csv \
  --pair ETH/USDT:data/tardis/ETHUSDT-l2update-2024-03-01.csv \
  --pair ETH/BTC:data/tardis/ETHBTC-l2update-2024-03-01.csv \
  --latency-ns 500000 \
  --taker-fee-bps 7.5 \
  --max-book-levels-per-side 20 \
  --summary-csv results/graph_run.csv
```

Use `--large-graph` to select the sparse lookup policy:

```bash
PYTHONPATH=build-release python3 scripts/run_graph_arbitrage.py \
  --large-graph \
  --pair BTC/USDT:data/tardis/BTCUSDT-l2update-2024-03-01.csv \
  --pair ETH/USDT:data/tardis/ETHUSDT-l2update-2024-03-01.csv \
  --pair ETH/BTC:data/tardis/ETHBTC-l2update-2024-03-01.csv \
  --max-book-levels-per-side 20
```

`GraphResult` exposes:
- `events_processed`
- `cycles_detected`
- `attempted_cycles`
- `completed_cycles`
- `panic_closes`
- `market_batches_processed`
- `cycle_searches`
- `final_usdt`
- `final_nav`
- `inventory_risk`
- `balances`
- `last_cycle`
- `cycle_snapshot_count`

Key execution filters:
- `--max-adverse-obi`: rejects buys when bid-side imbalance is too strong and rejects sells when ask-side pressure is too strong. `1.0` disables the filter.
- `--max-spread-bps`: rejects cycles touching pairs with wide spreads. `1000` is effectively permissive for most liquid markets.
- `--min-depth-usdt`: rejects cycles when visible top-of-book depth is too shallow in USDT terms.
- `--min-cycle-edge-bps`: requires a minimum theoretical cycle edge after taker fees before the cycle is sent to the latency heap.
- `--max-book-levels-per-side`: caps retained visible depth per side. Use `20` for fast iteration and `100` for sanity checks against deeper visible liquidity.
- `--large-graph`: uses `GraphEngineLarge` and the sparse lookup policy.

## Data Sources

YABE supports four data paths:

1. Trade replay for the older `TriangularEngine`.
2. L1/bookTicker-style replay for older graph experiments.
3. Depth-5 replay for compatibility with prior Tardis experiments.
4. Raw `L2Update` replay for the current graph research path.

The backward-compatible L1 CSV format is:

```csv
timestamp,bid_price,bid_qty,ask_price,ask_qty
```

The legacy Depth-5 CSV format is:

```csv
timestamp,b1_p,b1_q,b2_p,b2_q,b3_p,b3_q,b4_p,b4_q,b5_p,b5_q,a1_p,a1_q,a2_p,a2_q,a3_p,a3_q,a4_p,a4_q,a5_p,a5_q
```

The current preferred graph CSV format is `L2Update`:

```csv
timestamp,is_snapshot,is_bid,price,qty
```

Rows represent absolute price-level quantities. `qty <= 0` removes a level. The graph engine batches rows with the same timestamp, applies them to capped `L2OrderBook` views, then runs cycle detection once for that timestamp batch.

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

Convert diffs to YABE's current `L2Update` schema:

```bash
python3 scripts/tardis_to_l2update.py \
  data/tardis/tardis-binance-BTCUSDT-incremental_book_L2-2024-03-01.csv.gz \
  -o data/tardis/BTCUSDT-l2update-2024-03-01.csv

python3 scripts/tardis_to_l2update.py \
  data/tardis/tardis-binance-ETHUSDT-incremental_book_L2-2024-03-01.csv.gz \
  -o data/tardis/ETHUSDT-l2update-2024-03-01.csv

python3 scripts/tardis_to_l2update.py \
  data/tardis/tardis-binance-ETHBTC-incremental_book_L2-2024-03-01.csv.gz \
  -o data/tardis/ETHBTC-l2update-2024-03-01.csv
```

Then run the graph engine:

```bash
PYTHONPATH=build-release python3 scripts/run_graph_arbitrage.py \
  --pair BTC/USDT:data/tardis/BTCUSDT-l2update-2024-03-01.csv \
  --pair ETH/USDT:data/tardis/ETHUSDT-l2update-2024-03-01.csv \
  --pair ETH/BTC:data/tardis/ETHBTC-l2update-2024-03-01.csv \
  --latency-ns 500000 \
  --taker-fee-bps 7.5 \
  --max-book-levels-per-side 20
```

The converter preserves raw L2 price-level rows in YABE's replay schema. The C++ engine owns book mutation and caps the visible execution view with `max_book_levels_per_side`. The older `tardis_to_depth5.py` and `tardis_to_bookticker.py` converters are still available for compatibility and lightweight experiments.

Recent Tardis spot raw L2Update run:

```text
BTCUSDT: 23,766,561 L2Update rows
ETHUSDT: 20,236,011 L2Update rows
ETHBTC: 1,625,907 L2Update rows
Total processed events: 45,628,476
```

Current reference result on `2024-03-01` Tardis spot raw L2Update, cap 20:

```text
latency_ns=500000
taker_fee_bps=7.5
max_book_levels_per_side=20
PnL=+$40.3445859551
cycles=29
completed=24
panic=5
```

Run that configuration:

```bash
PYTHONPATH=build-release python3 scripts/run_graph_arbitrage.py \
  --pair BTC/USDT:data/tardis/BTCUSDT-l2update-2024-03-01.csv \
  --pair ETH/USDT:data/tardis/ETHUSDT-l2update-2024-03-01.csv \
  --pair ETH/BTC:data/tardis/ETHBTC-l2update-2024-03-01.csv \
  --latency-ns 500000 \
  --taker-fee-bps 7.5 \
  --max-book-levels-per-side 20 \
  --summary-csv results/tardis_l2update_graph_2024-03-01.csv
```

## Performance

Tested locally in a `Release` build on Apple Silicon.

Recent Graph Engine runs (Release build on Apple Silicon):

**Raw L2Update Tardis, cap 20 (45.63M events)**:
- `GraphEngine`: `6.51s`, `7.01M EPS`, PnL `+$40.3445859551 USDT`
- `GraphEngineLarge`: `6.06s`, `7.53M EPS`, PnL `+$40.3445859551 USDT`
- Cycles Detected: `29`
- Completed Cycles: `24`
- Panic Closes: `5`

**Raw L2Update Tardis, cap 100 (45.63M events)**:
- Wall Time: `7.95s`
- Throughput: `5.74M EPS`
- PnL: `+$40.3467098325 USDT`

**Legacy Depth-5 Tardis (1.77M events)**:
- Median Throughput: `2.28M EPS`
- PnL: `+$37.20 USDT`

**L1 bookTicker (9.00M events)**:
- Median Throughput: `2.95M EPS`
- Cycles Detected: `81,728`
- Panic Closes: `64,962`
- PnL: `+$9,205.05 USDT`

## Foundation Roadmap

The current foundation is ready for separate Hyperliquid, Polymarket, and ML/RL research branches. The next architecture work should stay additive:

- **Venue-normalized replay**: immutable replay logs with venue/product ids, exchange/receive timestamps, sequence ids, snapshot epochs, checksum/gap policy, absolute-vs-delta semantics, and integer ticks/lots.
- **Venue manifests**: assets, products, tick/lot scales, fee/cost models, collateral, settlement domains, and synthetic transform declarations.
- **Canonical full-depth books**: maintain full venue state separately from capped top-N execution views.
- **Typed executable edges**: order-book taker edges, static conversion edges, and Polymarket-style multi-input/multi-output transforms.
- **Feature export**: deterministic snapshots for spread, OBI, depth bands, microprice, cycle edge, latency outcome, and execution label generation.

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
