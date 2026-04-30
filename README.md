# YABE

YABE, "Yet Another Backtest Engine", is a C++20 backtesting engine for limit-order-book replay and graph-based arbitrage research.

The current production research path is `GraphEngine`: it replays normalized L2 update CSV files through mmap-backed parsers, reconstructs per-pair books, builds an asset graph, and searches for executable arbitrage cycles with latency-aware execution.

## Current Status

YABE has five engine-facing paths:

- `GraphEngine`: the active dense-lookup N-asset graph arbitrage engine. This is the default path for small graphs.
- `GraphEngineLarge`: the same graph engine with sparse lookup policy. Use it for larger sparse product graphs or when testing graph-scale behavior.
- `L2BacktestEngine`: single-asset replay over external normalized L2 updates, intended for market-making and ML/RL research.
- `BacktestEngine`: single-asset replay and strategy backtesting over the core LOB/OMS path.
- `TriangularEngine`: fixed BTC/ETH/USDT triangular-arbitrage backtesting over the multi-asset OMS path.

The graph engine currently supports:

- mmap-backed CSV replay.
- Raw L2 update rows: snapshot/update flag, side, price, quantity.
- Per-pair local book reconstruction.
- Dynamic asset registration from Python.
- Bellman-Ford cycle detection.
- Stable graph topology with in-place rate and capacity updates.
- Dense and sparse lookup policies.
- Fee-aware sizing.
- Latency-delayed leg execution.
- Visible-depth depletion so simulated fills cannot repeatedly consume the same displayed liquidity.
- Python bindings through `pybind11`, with the replay loop running outside the Python GIL.

Known limitations are listed near the end of this file. Read them before treating results as production-quality venue simulation.

## Repository Layout

```text
include/lob/                  C++ headers
src/                          C++ implementation and Python bindings
tests/                        GoogleTest suite
benchmarks/                   Microbenchmarks
scripts/run_graph_arbitrage.py GraphEngine CLI helper
scripts/tardis_to_l2update.py  Tardis incremental L2 -> YABE update CSV
scripts/tardis_to_depth5.py    Legacy Tardis incremental L2 -> depth-5 CSV
include/lob/l2_backtest_engine.hpp Single-asset external L2 replay engine
include/lob/backtest_engine.hpp Single-asset strategy backtest engine
include/lob/market_maker_strategy.hpp Basic market-making strategy
include/lob/inventory_skew_strategy.hpp Inventory-skew market-making strategy
include/lob/venue_replay.hpp   Venue replay envelope and sequence validation foundations
include/lob/venue_manifest.hpp Venue/product manifest foundations
```

## Build

```bash
cmake -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
ctest --test-dir build-release --output-on-failure
```

Build only the Python extension:

```bash
cmake --build build-release --target yabe
```

The Python module is emitted into the build directory, usually:

```text
build-release/yabe.so
```

## GraphEngine Input Format

`GraphEngine` expects one CSV file per trading pair.

Preferred normalized L2 update format:

```csv
timestamp,is_snapshot,is_bid,price,qty
1000000000,1,1,49900.0,10.0
1000000000,1,0,50000.0,10.0
1000000100,0,1,49910.0,4.0
1000000200,0,0,50000.0,0.0
```

Field meaning:

- `timestamp`: event time. Nanoseconds are used as-is; smaller millisecond-like values are normalized internally.
- `is_snapshot`: `1` for snapshot rows, `0` for incremental updates.
- `is_bid`: `1` for bid side, `0` for ask side.
- `price`: price level.
- `qty`: absolute quantity at that level. `0` removes the level.

The parser supports this format through `L2UpdateCsvParser`.

## Running GraphEngine

Manual pair configuration:

```bash
PYTHONPATH=build-release python3 scripts/run_graph_arbitrage.py \
  --pair BTC/USDT:data/BTCUSDT-l2update.csv \
  --pair ETH/USDT:data/ETHUSDT-l2update.csv \
  --pair ETH/BTC:data/ETHBTC-l2update.csv \
  --latency-ns 500000 \
  --taker-fee-bps 1.0 \
  --max-cycle-notional-usdt 1000 \
  --min-cycle-edge-bps 0.5
```

The script prints:

- asset id mapping,
- event counts,
- detected and completed cycles,
- panic closes,
- final quote balance,
- mark-to-market NAV,
- final balances.

Use the sparse graph engine:

```bash
PYTHONPATH=build-release python3 scripts/run_graph_arbitrage.py \
  --large-graph \
  --pair BTC/USDT:data/BTCUSDT-l2update.csv \
  --pair ETH/USDT:data/ETHUSDT-l2update.csv \
  --pair ETH/BTC:data/ETHBTC-l2update.csv
```

## Python API

```python
import yabe

engine = yabe.GraphEngine(
    initial_usdt=100_000_000,
    latency_ns=500_000,
    intra_leg_latency_ns=75,
    taker_fee_bps=1.0,
    max_cycle_notional_usdt=1_000,
    max_adverse_obi=1.0,
    max_spread_bps=1_000,
    min_depth_usdt=0,
    min_cycle_edge_bps=0.5,
    cycle_snapshot_reserve=100_000,
    quote_asset="USDT",
    max_book_levels_per_side=100,
)

engine.add_pair("BTC", "USDT", "data/BTCUSDT-l2update.csv")
engine.add_pair("ETH", "USDT", "data/ETHUSDT-l2update.csv")
engine.add_pair("ETH", "BTC", "data/ETHBTC-l2update.csv")

result = engine.run()

print(engine.assets)
print(result.events_processed)
print(result.completed_cycles)
print(result.final_nav)
print(result.balances)
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
- `cycle_snapshots_overwritten`
- `get_cycle_snapshots_dataframe()`

`yabe.GraphEngineLarge` exposes the same constructor and methods as `yabe.GraphEngine`.

## Tardis Data Conversion

Convert Tardis `incremental_book_L2` CSV or CSV.GZ files into YABE's raw L2 update schema:

```bash
python3 scripts/tardis_to_l2update.py \
  data/tardis/tardis-binance-BTCUSDT-incremental_book_L2-2024-03-01.csv.gz \
  -o data/tardis/tardis-binance-BTCUSDT-l2update-2024-03-01.csv
```

Then pass the generated file to `scripts/run_graph_arbitrage.py` with `--pair`.

The older `scripts/tardis_to_depth5.py` converter emits fixed top-five snapshots. That format is useful for older experiments but loses raw delta fidelity.

## Single-Asset Backtesting

YABE now has two separate single-asset paths. They intentionally use different parsers and event models.

`BacktestEngine` replays one trade/order stream into a local matching `OrderBook`, routes events through a `Strategy`, simulates latency, tracks fills, cash, position, equity, and maker/taker diagnostics. It uses `CsvParser`, which parses trade/order-style rows into `Order` events.

`L2BacktestEngine` replays one external L2 update stream through `L2UpdateCsvParser` and maintains an `L2OrderBook` directly from normalized exchange depth rows:

```csv
timestamp,is_snapshot,is_bid,price,qty
```

Use `L2BacktestEngine` for Hyperliquid-style market-making research where the input is already a captured market book stream rather than a sequence of historical orders.

The L2 path does the following:

- batches all L2 rows with the same timestamp,
- applies snapshots and incremental level updates to `L2OrderBook`,
- calls native `L2Strategy::on_tick(...)` once per timestamp batch after both sides exist,
- keeps a legacy `Strategy` adapter mode for existing `OrderBook`-based strategies,
- releases submitted strategy orders after `latency_ns`,
- sweeps visible L2 depth for aggressive taker fills,
- lets passive orders fill as maker when later L2 updates cross their prices,
- tracks cash, position, NAV/equity, maker/taker fill diagnostics, active orders, and dropped pending orders,
- can record timestamped feature rows for ML datasets without calling back into Python during replay.

The native path avoids rebuilding an `OrderBook` adapter every timestamp batch. New market-making research strategies should implement `L2Strategy` and consume `L2OrderBook` directly. The adapter `OrderBook` exists only so current strategies can consume BBO and L2 snapshots through the existing `Strategy` interface. The authoritative replay state remains the `L2OrderBook`; the external L2 rows are not shoehorned into `CsvParser`.

The repository includes basic strategy implementations:

- `L2MarketMakerStrategy`: native L2 market maker that reads effective visible BBO directly from `L2OrderBook`.
- `MarketMakerStrategy`: legacy adapter strategy that periodically cancels and replaces symmetric bid/ask quotes around the mid.
- `InventorySkewStrategy`: market-making with inventory skew and simple internal-book L2 imbalance filtering.

The `lob_backtest` binary uses `InventorySkewStrategy` for the single-file path:

```bash
./build-release/lob_backtest data/BTCUSDT-trades.csv \
  --maker-fee-bps -1.0 \
  --taker-fee-bps 7.5 \
  --base-latency-ns 500000
```

Optional positional parameters tune the inventory-skew strategy:

```bash
./build-release/lob_backtest data/BTCUSDT-trades.csv 10.0 2.0 3.0
```

Those values are:

- base spread,
- inventory risk-aversion gamma,
- L2 imbalance threshold.

`MarketMakerStrategy` is available as a C++ strategy class for users embedding `BacktestEngine` directly.

Minimal C++ usage for the L2 path:

```cpp
#include "lob/l2_backtest_engine.hpp"
#include "lob/l2_market_maker_strategy.hpp"

lob::L2MarketMakerStrategy strategy {lob::L2MarketMakerStrategy::Config {
    .quote_offset = 0.5,
    .quote_quantity = 1'000'000,
    .refresh_interval_ns = 1'000'000'000ULL,
}};

lob::L2BacktestEngine engine {strategy, lob::L2BacktestEngine::Config {
    .initial_cash = 100'000'000.0,
    .maker_fee_bps = 0.0,
    .taker_fee_bps = 7.5,
    .latency_ns = 500'000,
    .max_book_levels_per_side = 20,
    .record_features = true,
}};

lob::L2BacktestEngine::Result result = engine.run("data/HYPEUSDC-l2update.csv");
```

Python can run the native C++ L2 market-maker path with the GIL released for the full replay loop:

```python
import yabe

engine = yabe.L2MarketMakerBacktest(
    max_book_levels_per_side=20,
    quote_offset=0.5,
    quote_quantity=1_000_000,
    latency_ns=500_000,
    record_features=True,
)

result = engine.run("data/tardis/BTCUSDT-l2update-2024-03-01.csv")
features = result.get_features_dataframe()
print(result.events_processed, result.pnl, len(features["timestamp"]))
```

## TriangularEngine

`TriangularEngine` models a fixed three-pair BTC/ETH/USDT strategy over the older multi-asset backtest path. It includes OMS behavior such as latency, order groups, panic close, and fee-aware sizing.

Example:

```bash
PYTHONPATH=build-release python3 - <<'PY'
import yabe

result = yabe.run_triangular(
    [
        "data/BTCUSDT-trades.csv",
        "data/ETHUSDT-trades.csv",
        "data/ETHBTC-trades.csv",
    ],
    latency_ns=500_000,
    maker_fee_bps=-1.0,
    taker_fee_bps=7.5,
    verbose=False,
)

print(result.events_processed)
print(result.nav)
PY
```

Use this path for multi-asset OMS research and regression coverage. Use `GraphEngine` or `GraphEngineLarge` for new dynamic graph work.

## Performance Notes

The graph replay loop is designed to avoid Python involvement during execution. `GraphEngine.run()` is exposed with `py::gil_scoped_release`, so Python threads are not blocked by the C++ replay loop.

Hot-path design goals:

- mmap-backed input parsing,
- preallocated runtime vectors,
- no per-row Python objects,
- static graph edge topology after runtime preparation,
- fixed-capacity pending-cycle heap,
- bounded cycle snapshot ring.

Current performance claims should be treated as local benchmarks, not universal guarantees. Throughput depends heavily on event density, number of pairs, graph size, page-cache state, compiler settings, and whether raw L2 deltas cause one graph search per update.

## Known Limitations

The current graph model is not yet a full venue-neutral HFT simulator.

Important gaps:

- Raw L2 events do not include sequence ids, checksums, or gap-recovery metadata.
- Book prices and quantities are stored as floating-point values, not venue-scaled integers.
- `max_book_levels_per_side` caps retained levels, so the local book can lose deeper state.
- Bellman-Ford runs over the full graph after each timestamp batch.
- Fees are modeled as a single global taker fee.
- Edge semantics assume simple two-asset order-book conversions.
- Synthetic Polymarket-style mint/redeem edges are not represented.
- Venue-specific concepts such as funding, collateral, gas, settlement, contract multipliers, and tick-size rules are not first-class configuration.

These are architectural constraints, not just missing parameters. Hyperliquid and Polymarket integration should start by introducing venue/product manifests, fixed-point market metadata, sequence-aware feed envelopes, and typed graph edges.

The repository already has early foundation types for manifests and replay validation in `include/lob/venue_manifest.hpp` and `include/lob/venue_replay.hpp`. They are not yet wired into `GraphEngine` ingestion.

## Development Notes

Useful commands:

```bash
cmake -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
ctest --test-dir build-release --output-on-failure
```

Run benchmarks:

```bash
cmake --build build-release --target lob_benchmarks
./build-release/lob_benchmarks
```

Run the graph helper:

```bash
PYTHONPATH=build-release python3 scripts/run_graph_arbitrage.py --help
```
