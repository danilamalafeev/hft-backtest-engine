# Event-Driven Limit Order Book & Replay Engine

A lightweight, C++20 Limit Order Book (LOB) matching engine and historical replay tool. Designed for market microstructure research and strategy backtesting rather than live production trading.

## Overview

This project implements a standard Price-Time Priority matching engine. While it does not utilize extreme HFT techniques (like kernel bypass or FPGA offloading), it employs robust C++ engineering principles to achieve high throughput on consumer hardware.

Key technical choices:
- Strict FIFO execution inside each price level.
- $O(1)$ cancellation via direct hash-to-iterator mapping.
- $O(\log N)$ insertion using standard associative containers.
- Zero-copy historical data ingestion via POSIX `mmap`.
- Event-driven virtual clock for accurate temporal backtesting without wall-clock pacing.

## Performance

Tested locally in a `Release` build on an ARM64 CPU (Apple Silicon).

### Historical Replay Throughput
Replaying a full day of Binance spot `BTCUSDT` trades (~140MB CSV, zero-copy parser):
- **Total orders processed**: `1,947,444`
- **Virtual market time**: `24 Hours`
- **Actual CPU time taken**: `0.28 seconds`
- **Throughput**: `~6.9 Million EPS`

### Microbenchmarks (Google Benchmark)
Time complexity validation for core LOB operations:

| Operation | 100 Orders | 10k Orders | Big O |
| :--- | :--- | :--- | :--- |
| `Resting Insert` | `1.13 us` | `1.63 us` | $O(\log N)$ |
| `Aggressive Match`| `1.14 us` | `1.75 us` | $O(\log N)$ |
| `Cancel Order` | `1.10 us` | `1.49 us` | $O(1)$ |

## Quick Start

```bash
# Build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Run Tests & Benchmarks
cd build
ctest --output-on-failure
./lob_benchmarks

# Run Binance Backtester (Requires real data)
python3 ../scripts/download_binance_data.py
./lob_backtest binance_trades.csv
```

## Next Steps

- Implement a basic `Strategy` interface (`on_orderbook_update`, `on_trade`).
- Add a baseline Market Maker bot to simulate liquidity provision.
- Track virtual PnL and position sizing.