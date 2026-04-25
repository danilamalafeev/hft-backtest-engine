# Event-Driven Limit Order Book Backtesting Framework

A lightweight C++20 limit order book, mmap replay engine, and quantitative backtesting framework for market microstructure research. The current setup is designed to keep the simulation hot path allocation-light while moving analytics and parameter research to the cold path.

## Overview

The project implements a price-time-priority matching engine and a strategy-driven backtest loop over Binance trade CSV data.

Key technical choices:
- Strict FIFO execution inside each price level.
- O(1) cancellation via direct hash-to-iterator mapping.
- O(1) best bid / best ask access.
- Caller-buffered L2 snapshots to avoid allocations in strategy code.
- Zero-copy historical data ingestion via POSIX `mmap`.
- Event-driven virtual clock for backtests without wall-clock pacing.
- Cold-path analytics for PnL, Sharpe ratio, and max drawdown.

## Architecture

Core modules:
- `lob::OrderBook`: matching engine, best bid/ask getters, and depth-limited L2 snapshots.
- `lob::Strategy`: abstract strategy interface with `on_tick(const OrderBook&, OrderGateway&)` and `on_fill(...)`.
- `lob::BacktestEngine`: owns the LOB, feeds parsed events, routes fills, tracks cash/position, and samples the equity curve.
- `lob::BacktestAnalytics`: post-run tear-sheet calculations.
- `lob::InventorySkewStrategy`: inventory-aware market maker with linear skew and L2 imbalance protection.

The hot path avoids heavy math and per-event analytics. Strategy L2 buffers are pre-reserved and reused.

## Strategy

`InventorySkewStrategy` quotes around true L1 mid-price:

```text
mid = (best_bid + best_ask) / 2
skew = (position - target_position) * gamma
bid = mid - base_spread / 2 - skew
ask = mid + base_spread / 2 - skew
```

Risk controls:
- Stops bidding when inventory is at or above `max_position`.
- Stops asking when inventory is at or below `-max_position`.
- Pulls the bid when depth-5 ask volume is greater than bid volume times `imbalance_threshold`.
- Pulls the ask when depth-5 bid volume is greater than ask volume times `imbalance_threshold`.

## Performance

Tested locally in a `Release` build on Apple Silicon with a full day of Binance spot `BTCUSDT` trades.

Recent full backtest:
- Total orders processed: `1,947,444`
- Virtual market time: `23:59:59.999`
- Throughput: `~5.5M-6.3M EPS`, depending on strategy parameters and local run variance

The old replay-only path can be faster because it does not run strategy logic, fill accounting, L2 imbalance checks, or equity sampling.

## Quick Start

```bash
# Build
cmake -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release

# Run tests
ctest --test-dir build-release --output-on-failure

# Optional: run microbenchmarks
./build-release/lob_benchmarks

# Download Binance data if needed
python3 scripts/download_binance_data.py

# Run backtest with defaults: spread=10.0, gamma=2.0, imbalance=3.0
./build-release/lob_backtest binance_trades.csv

# Run backtest with explicit parameters
./build-release/lob_backtest binance_trades.csv 20.0 0.1 2.0
```

The backtester prints a human-readable tear sheet and a machine-readable final line:

```text
RESULT_CSV,spread,gamma,imbalance,pnl,sharpe,max_dd
```

## Parameter Optimization

Run the grid search script:

```bash
python3 scripts/optimize_grid.py binance_trades.csv
```

The script runs `build-release/lob_backtest` when available, otherwise falls back to `build/lob_backtest`. It searches:

```python
spread_grid = [2.0, 5.0, 10.0, 20.0]
gamma_grid = [0.1, 0.5, 1.0, 3.0]
imbalance_grid = [2.0, 5.0, 10.0, 999.0]
```

Results are parsed from `RESULT_CSV`, loaded into pandas, sorted by Sharpe ratio, and printed as the top 10 parameter combinations.

## Notes

- CSV quantity values are parsed into fixed-point integer units scaled by `1e8`; PnL and displayed positions convert back to base units.
- `test_orders.csv` is a simple unit-test style CSV and is not in the Binance replay format expected by the mmap parser.
- The framework is research-oriented and does not model fees, latency, queue position probability, or exchange-specific order acknowledgements yet.
