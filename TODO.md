# TODO

- [ ] **Data Abstraction (Arch)**: Decouple the `GraphArbitrageEngine` from the hardcoded `L2Depth5Event` struct by introducing a generic `OrderBook` component to handle raw L2 tick deltas (event-driven depth). Replace the fixed 5-level arrays in `PairState` with a dynamic representation (e.g., `std::map` or a flat `std::vector` structure) that can accurately handle `limit_add`, `limit_cancel`, and `trade` events.
