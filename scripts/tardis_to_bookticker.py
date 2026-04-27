#!/usr/bin/env python3
"""Convert Tardis incremental_book_L2 CSVs to YABE BBO CSV format.

Input schema:
exchange,symbol,timestamp,local_timestamp,is_snapshot,side,price,amount

Output schema:
timestamp,bid_price,bid_qty,ask_price,ask_qty

Timestamps are emitted in nanoseconds so YABE's C++ L2CsvParser can consume
them without further conversion.
"""

from __future__ import annotations

import argparse
import csv
import gzip
import heapq
import sys
from pathlib import Path
from typing import Iterable


try:
    from sortedcontainers import SortedDict  # type: ignore
except ImportError:  # pragma: no cover - exercised only when dependency is absent
    SortedDict = None


class HeapBookSide:
    """Dependency-free fallback for best-price access with lazy heap cleanup."""

    def __init__(self, is_bid: bool) -> None:
        self.is_bid = is_bid
        self.levels: dict[float, float] = {}
        self.heap: list[float] = []

    def clear(self) -> None:
        self.levels.clear()
        self.heap.clear()

    def set(self, price: float, amount: float) -> None:
        if amount <= 0.0:
            self.levels.pop(price, None)
            return
        self.levels[price] = amount
        heapq.heappush(self.heap, -price if self.is_bid else price)

    def best(self) -> tuple[float, float] | None:
        while self.heap:
            raw = self.heap[0]
            price = -raw if self.is_bid else raw
            amount = self.levels.get(price)
            if amount is not None and amount > 0.0:
                return price, amount
            heapq.heappop(self.heap)
        return None


class SortedBookSide:
    def __init__(self, is_bid: bool) -> None:
        self.is_bid = is_bid
        self.levels = SortedDict()

    def clear(self) -> None:
        self.levels.clear()

    def set(self, price: float, amount: float) -> None:
        if amount <= 0.0:
            self.levels.pop(price, None)
            return
        self.levels[price] = amount

    def best(self) -> tuple[float, float] | None:
        if not self.levels:
            return None
        index = -1 if self.is_bid else 0
        price = self.levels.keys()[index]
        return price, self.levels[price]


def make_book_side(is_bid: bool, prefer_sortedcontainers: bool):
    if prefer_sortedcontainers and SortedDict is not None:
        return SortedBookSide(is_bid)
    return HeapBookSide(is_bid)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build YABE BBO CSV from Tardis incremental L2 diffs.")
    parser.add_argument("input", type=Path, help="Tardis incremental_book_L2 .csv or .csv.gz")
    parser.add_argument("-o", "--output", type=Path, required=True, help="Output BBO CSV")
    parser.add_argument(
        "--timestamp-source",
        choices=("local_timestamp", "timestamp"),
        default="local_timestamp",
        help="Tardis timestamp column to emit",
    )
    parser.add_argument(
        "--use-heap-book",
        action="store_true",
        help="Force dependency-free heap book instead of sortedcontainers.SortedDict",
    )
    parser.add_argument(
        "--max-rows",
        type=int,
        default=0,
        help="Optional row limit for quick smoke tests",
    )
    parser.add_argument(
        "--progress-every",
        type=int,
        default=1_000_000,
        help="Print progress every N input rows. 0 disables progress.",
    )
    return parser.parse_args()


def open_text(path: Path):
    if path.suffix == ".gz":
        return gzip.open(path, "rt", newline="")
    return path.open(newline="")


def truthy(value: str) -> bool:
    return value.strip().lower() in {"true", "1", "t", "yes"}


def micros_to_nanos(value: str) -> int:
    raw = int(value)
    # Tardis downloadable CSV timestamps are microseconds since epoch.
    return raw * 1_000


def apply_batch(rows: list[dict[str, str]], bids, asks) -> None:
    if any(truthy(row["is_snapshot"]) for row in rows):
        bids.clear()
        asks.clear()

    for row in rows:
        price = float(row["price"])
        amount = float(row["amount"])
        side = row["side"].strip().lower()
        if side == "bid":
            bids.set(price, amount)
        elif side == "ask":
            asks.set(price, amount)


def current_bbo(bids, asks) -> tuple[float, float, float, float] | None:
    bid = bids.best()
    ask = asks.best()
    if bid is None or ask is None:
        return None
    bid_price, bid_qty = bid
    ask_price, ask_qty = ask
    if bid_price <= 0.0 or ask_price <= 0.0 or bid_qty <= 0.0 or ask_qty <= 0.0:
        return None
    return bid_price, bid_qty, ask_price, ask_qty


def should_emit(
    bbo: tuple[float, float, float, float],
    last_bbo: tuple[float, float, float, float] | None,
) -> bool:
    if last_bbo is None:
        return True
    return bbo != last_bbo


def convert(input_path: Path, output_path: Path, args: argparse.Namespace) -> tuple[int, int]:
    prefer_sortedcontainers = not args.use_heap_book
    if prefer_sortedcontainers and SortedDict is None:
        print("sortedcontainers is not installed; using heap-backed fallback book.", file=sys.stderr)

    bids = make_book_side(is_bid=True, prefer_sortedcontainers=prefer_sortedcontainers)
    asks = make_book_side(is_bid=False, prefer_sortedcontainers=prefer_sortedcontainers)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    input_rows = 0
    output_rows = 0
    initialized = False
    last_bbo: tuple[float, float, float, float] | None = None

    with open_text(input_path) as source, output_path.open("w", newline="") as destination:
        reader = csv.DictReader(source)
        required = {"exchange", "symbol", "timestamp", "local_timestamp", "is_snapshot", "side", "price", "amount"}
        missing = required.difference(reader.fieldnames or [])
        if missing:
            raise RuntimeError(f"{input_path} is missing required Tardis columns: {sorted(missing)}")

        writer = csv.writer(destination)
        writer.writerow(["timestamp", "bid_price", "bid_qty", "ask_price", "ask_qty"])

        batch_ts: str | None = None
        batch_rows: list[dict[str, str]] = []

        def flush_batch() -> None:
            nonlocal output_rows, initialized, last_bbo
            if not batch_rows or batch_ts is None:
                return

            has_snapshot = any(truthy(row["is_snapshot"]) for row in batch_rows)
            if not initialized and not has_snapshot:
                return

            apply_batch(batch_rows, bids, asks)
            if has_snapshot:
                initialized = True

            bbo = current_bbo(bids, asks)
            if bbo is None or not should_emit(bbo, last_bbo):
                return

            bid_price, bid_qty, ask_price, ask_qty = bbo
            writer.writerow(
                [
                    micros_to_nanos(batch_ts),
                    f"{bid_price:.12f}",
                    f"{bid_qty:.12f}",
                    f"{ask_price:.12f}",
                    f"{ask_qty:.12f}",
                ]
            )
            output_rows += 1
            last_bbo = bbo

        for row in reader:
            input_rows += 1
            timestamp = row[args.timestamp_source]
            if batch_ts is None:
                batch_ts = timestamp
            elif timestamp != batch_ts:
                flush_batch()
                batch_rows.clear()
                batch_ts = timestamp

            batch_rows.append(row)

            if args.progress_every and input_rows % args.progress_every == 0:
                print(f"{input_path.name}: read {input_rows:,} rows, wrote {output_rows:,} BBO rows", file=sys.stderr)
            if args.max_rows and input_rows >= args.max_rows:
                break

        flush_batch()

    return input_rows, output_rows


def main() -> int:
    args = parse_args()
    input_rows, output_rows = convert(args.input, args.output, args)
    print(f"Read {input_rows:,} rows")
    print(f"Wrote {output_rows:,} BBO rows to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
