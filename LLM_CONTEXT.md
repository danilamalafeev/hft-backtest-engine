# YABE (Yet Another Backtest Engine) - LLM Context Guide

Welcome to the YABE codebase. This document is written specifically to onboard LLM agents and human developers quickly. It provides a technical blueprint of the architecture, algorithmic invariants, and memory constraints that define this project.

## 1. Core Philosophy

YABE is a production-grade, low-latency, multi-asset quantitative backtesting framework built in **C++20**. Its primary goal is to simulate high-frequency (HFT) triangular and N-way arbitrage with absolute mathematical determinism.

**Strict Invariants:**
1. **Zero-Allocation Hot Path:** Heap allocations (`new`, `std::vector::push_back`, `std::make_shared`) are strictly forbidden during the main event loop. All memory is pre-allocated via `reserve()` or fixed-size structures (e.g., Ring Buffers) during `prepare_runtime()`.
2. **Zero-Copy Ingestion:** Data is ingested via POSIX `mmap` and streamed via custom parsers. Parsers return `const T&` (e.g., `peek()`) to avoid copying structs across the boundary.
3. **Deterministic State Machine:** Wall-clock time does not exist. The engine is a pure event-driven simulator strictly governed by the chronological processing of timestamps (`release_time_ns` and `event.timestamp`).

## 2. Dual Engine Architecture

The codebase houses two primary simulation architectures:

### A. `MultiAssetBacktestEngine` (Legacy / Microstructure)
Located in `multi_asset_backtest_engine.hpp`. 
- Built around `EventMerger` which multiplexes multiple Binance historical trade streams.
- Hooks into `OrderGateway` / `oms` to simulate FOK sweeps and atomic `OrderGroup` execution.
- Designed primarily for the hardcoded `TriangularArbitrageStrategy` (BTC/ETH/USDT).
- Used for `Trade` event generation and high-throughput logging via the lock-free SPSC `AsyncLogger`.

### B. `GraphArbitrageEngine` (State-of-the-Art / Production)
Located in `graph_arbitrage_engine.hpp`. This is the core focus of recent development.
- **Data Model:** Processes `L2Depth5Event` (Top 5 levels of the limit order book) directly.
- **Dynamic Assets:** Arbitrary pairs are added dynamically at runtime (e.g., via Python bindings).
- **Unified Event Loop:** The `run()` method contains a master `while` loop that multiplexes incoming market events (from CSV `peek()`) and internal simulation events (from `pending_cycles_` heap) chronologically. 

## 3. Algorithmic Deep Dive (`GraphArbitrageEngine`)

### Cycle Detection (SPFA)
The engine utilizes the **Shortest Path Faster Algorithm (SPFA)** to find negative cycles (arbitrage opportunities).
- **Multiplicative Rate Accumulator:** We do NOT use `-std::log()` due to precision loss over deep books. Instead, `std::vector<double> rates_` is initialized to `1.0` for all nodes. 
- The relaxation step maximizes the rate: `rates_[edge.from] * edge.effective_rate`.
- Cycles are detected when the product exceeds `1.0` relative to the starting point, circumventing floating-point degradation.
- **Adjacency List:** The graph maintains an `adjacency_edges_` list mapping `AssetID -> vector<EdgeIndex>` to guarantee $O(\text{out-degree})$ edge lookups rather than $O(V \cdot E)$.

### Order Management (OMS) & Interleaved Execution
When a cycle is found, it is **not executed instantly**.
1. **Bottleneck Calculation:** Checks theoretical slippage and calculates the `calculate_fee_aware_bottleneck()` size.
2. **Latency Queuing:** If executable, the first leg is queued into the `PendingMinHeap` with `release_time_ns = current_time_ns + latency_ns`.
3. **Interleaved Dispatch:** The `execute_pending_leg` function processes *exactly one leg* of the cycle. It computes fill prices based on the order book AT THAT EXACT MOMENT in simulation time.
4. **Hop Latency:** If more legs remain, the cycle is pushed *back* into the heap delayed by `intra_leg_latency_ns`. This ensures the engine continues processing market updates while the order routes to the next exchange/pair.

### Risk Management
- **`DynamicWallet`:** A robust capital tracking class. During cycle dispatch, capital is locked using `reserve_balance()`. It is later resolved using `consume_reserved()` upon execution or `release_reserved()` during failures.
- **Panic Close:** If a leg executes but the *next* leg fails (due to latency-induced slippage or depleted books), the `panic_close_to_quote` protocol is triggered. This forces a pessimistic market dump of the partial inventory back to `USDT` to ensure exactly $0.00 inventory risk at the end of the simulation.
- **Depletion Ledger:** Tracks how much liquidity has been consumed from the visible L2 snapshots between actual market updates, preventing the engine from executing virtual fills against the same visible order twice.

## 4. Memory Profiling Implementations

- **`CycleSnapshotRing`:** Records every attempted execution. Instead of `std::vector`, this is a custom pre-allocated ring buffer. It silently overwrites the oldest traces if `cycle_snapshot_reserve` is exceeded, exposing the overrun count to Python while keeping the C++ hot-path strictly allocation-free.

## 5. Python Interop (`pybind11`)

The `src/bindings.cpp` file provides the `yabe` Python module.
- `PyGraphEngine` wraps the C++ implementation.
- Python is solely used for configuration, initiating runs, and data analysis.
- The C++ engine strictly runs with `py::gil_scoped_release` to ensure Python garbage collection does not block the nanosecond simulation loop.
- The `get_cycle_snapshots_dataframe` and `get_microstructure_dataframe` methods efficiently map C++ structs into column-wise `py::dict`s containing NumPy arrays for instantaneous zero-copy ingestion into Pandas.

## 6. Next Steps / TODO (Context for Agents)

The codebase is mathematically stable and heavily optimized. The primary ongoing architectural shift is:

- **Data Abstraction (CLOB Refactor):** Currently, `GraphArbitrageEngine` is tightly coupled to `L2Depth5Event` (fixed size array arrays for bids/asks). The next step is to introduce a generic, event-driven `OrderBook` component inside the graph engine capable of ingesting arbitrary continuous L2 deltas (`limit_add`, `limit_cancel`, `trade`) instead of relying on depth snapshots and `depletion_ledger_` approximation.
