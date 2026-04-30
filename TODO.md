# TODO

## Done

- [x] **Generic L2 Graph Path**: Decoupled `GraphArbitrageEngine` from fixed `L2Depth5Event` rows and added raw `L2Update` replay into a capped dynamic `L2OrderBook`.
- [x] **Capped Visible Depth**: Added `max_book_levels_per_side` and wired it through C++, pybind, and `scripts/run_graph_arbitrage.py`.
- [x] **Graph Lookup Policies**: Added `GraphArbitrageEngineT<LookupPolicy>`, dense default `GraphEngine`, and sparse `GraphEngineLarge`.
- [x] **Stable Edge Topology**: Build directed graph edges once and update rates/capacities in place instead of rebuilding the whole edge set every cycle search.
- [x] **Execution Correctness Fixes**: Restored full Bellman-Ford correctness, non-mutating signal-time quote simulation, current-depth revalidation at execution, and chronological pending-leg processing.
- [x] **Foundation Replay Types**: Added venue-normalized replay envelope primitives and sequence validation helpers.
- [x] **Foundation Manifest Types**: Added manifest structs for assets, products, tick/lot scales, fee models, and settlement domains.
- [x] **Single-Asset L2 Replay Path**: Added `L2BacktestEngine`, native `L2Strategy`, native `L2MarketMakerStrategy`, latency-delayed visible-depth fills, feature rows, and a GIL-released Python runner for L2 market-making experiments.

## Next Core Work

- [ ] **Venue-Normalized Replay Log**: Implement an immutable replay file format around the current replay primitives. The normalized envelope should include venue/product ids, exchange and receive timestamps, monotonic sequence/update ids, snapshot epoch, checksum or gap policy, absolute-vs-delta semantics, side, integer price ticks, and integer quantity lots.
- [ ] **Replay Parser and Writer**: Add C++ parsers/writers for the normalized replay format and converters from existing Tardis/Binance L2Update CSVs.
- [ ] **Venue Manifests**: Replace Binance-triangle auto-discovery with manifest-driven configuration for assets, products, tick/lot scales, collateral/settlement domains, fee models, and synthetic transform edges.
- [ ] **Canonical Full-Depth Book**: Maintain full venue book state separately from the capped top-N execution view. The current capped vector book is correct for bounded research replay, but it is not a canonical raw L2 reconstruction store.
- [ ] **Integer Tick/Lot Book Path**: Move price identity and level mutation to venue-scaled integer ticks/lots before serious Hyperliquid or Polymarket research.
- [ ] **Typed Edge Cost Models**: Replace the single global taker fee multiplier with per-edge cost models covering proportional fees, fixed fees, gas, funding, collateral currency, and contract multipliers.
- [ ] **Typed Executable Edges**: Add order-book taker edges, static conversion edges, and synthetic transform edges for Polymarket-style YES/NO bundles and settlement transforms.
- [ ] **Incremental Cycle Detection Research**: Keep full Bellman-Ford as the correctness verifier, but investigate dirty-edge or affected-region relaxation for larger product graphs.
- [ ] **Richer Single-Asset Labels**: Extend `L2BacktestEngine` feature export with realized forward returns, queue-position approximations, adverse selection labels, fill probability labels, and latency outcome labels.

## Research Branches To Cut From Foundation

- [ ] **Hyperliquid Research**: Build a Hyperliquid adapter/converter into the normalized replay format, including venue sequencing and product metadata.
- [ ] **Polymarket CLOB Arbitrage**: Model outcome tokens, complement bundles, settlement domains, mint/redeem transforms, gas/fixed costs, and exact cent arithmetic.
- [ ] **ML/RL Feature Export**: Extend deterministic feature snapshots beyond the current L2 spread/OBI/depth/NAV rows to include depth bands, microprice, cycle edge bps, latency outcome, panic close outcome, and realized execution label generation.
