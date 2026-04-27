#!/usr/bin/env python3
"""Convert Tardis incremental_book_L2 CSVs to fixed Depth-5 YABE CSVs.

Output schema:
timestamp,b1_p,b1_q,b2_p,b2_q,b3_p,b3_q,b4_p,b4_q,b5_p,b5_q,
a1_p,a1_q,a2_p,a2_q,a3_p,a3_q,a4_p,a4_q,a5_p,a5_q
"""

from __future__ import annotations

import argparse
import csv
import gzip
import heapq
import sys
from pathlib import Path


try:
    from sortedcontainers import SortedDict  # type: ignore
except ImportError:  # pragma: no cover
    SortedDict = None


DEPTH = 5


class HeapBookSide:
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

    def top(self, depth: int) -> list[tuple[float, float]]:
        prices = sorted(self.levels.keys(), reverse=self.is_bid)[:depth]
        return [(price, self.levels[price]) for price in prices if self.levels[price] > 0.0]


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

    def top(self, depth: int) -> list[tuple[float, float]]:
        if not self.levels:
            return []
        keys = reversed(self.levels.keys()) if self.is_bid else iter(self.levels.keys())
        out: list[tuple[float, float]] = []
        for price in keys:
            amount = self.levels[price]
            if amount > 0.0:
                out.append((price, amount))
                if len(out) == depth:
                    break
        return out


def make_book_side(is_bid: bool, prefer_sortedcontainers: bool):
    if prefer_sortedcontainers and SortedDict is not None:
        return SortedBookSide(is_bid)
    return HeapBookSide(is_bid)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build YABE Depth-5 CSV from Tardis incremental L2 diffs.")
    parser.add_argument("input", type=Path)
    parser.add_argument("-o", "--output", type=Path, required=True)
    parser.add_argument("--timestamp-source", choices=("local_timestamp", "timestamp"), default="local_timestamp")
    parser.add_argument("--use-heap-book", action="store_true")
    parser.add_argument("--max-rows", type=int, default=0)
    parser.add_argument("--progress-every", type=int, default=1_000_000)
    return parser.parse_args()


def open_text(path: Path):
    if path.suffix == ".gz":
        return gzip.open(path, "rt", newline="")
    return path.open(newline="")


def truthy(value: str) -> bool:
    return value.strip().lower() in {"true", "1", "t", "yes"}


def micros_to_nanos(value: str) -> int:
    return int(value) * 1_000


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


def padded(levels: list[tuple[float, float]]) -> tuple[tuple[float, float], ...]:
    padded_levels = levels[:DEPTH] + [(0.0, 0.0)] * (DEPTH - len(levels))
    return tuple(padded_levels)


def snapshot_depth5(bids, asks) -> tuple[tuple[float, float], tuple[float, float]] | None:
    bid_levels = padded(bids.top(DEPTH))
    ask_levels = padded(asks.top(DEPTH))
    if bid_levels[0][0] <= 0.0 or bid_levels[0][1] <= 0.0 or ask_levels[0][0] <= 0.0 or ask_levels[0][1] <= 0.0:
        return None
    return bid_levels, ask_levels


def header() -> list[str]:
    columns = ["timestamp"]
    for level in range(1, DEPTH + 1):
        columns.extend([f"b{level}_p", f"b{level}_q"])
    for level in range(1, DEPTH + 1):
        columns.extend([f"a{level}_p", f"a{level}_q"])
    return columns


def flatten(timestamp: str, bid_levels, ask_levels) -> list[str]:
    row = [str(micros_to_nanos(timestamp))]
    for price, qty in bid_levels:
        row.extend([f"{price:.12f}", f"{qty:.12f}"])
    for price, qty in ask_levels:
        row.extend([f"{price:.12f}", f"{qty:.12f}"])
    return row


def convert(input_path: Path, output_path: Path, args: argparse.Namespace) -> tuple[int, int]:
    prefer_sortedcontainers = not args.use_heap_book
    if prefer_sortedcontainers and SortedDict is None:
        print("sortedcontainers is not installed; using sorted dict fallback.", file=sys.stderr)

    bids = make_book_side(True, prefer_sortedcontainers)
    asks = make_book_side(False, prefer_sortedcontainers)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    input_rows = 0
    output_rows = 0
    initialized = False
    last_snapshot = None

    with open_text(input_path) as source, output_path.open("w", newline="") as destination:
        reader = csv.DictReader(source)
        required = {"exchange", "symbol", "timestamp", "local_timestamp", "is_snapshot", "side", "price", "amount"}
        missing = required.difference(reader.fieldnames or [])
        if missing:
            raise RuntimeError(f"{input_path} missing required Tardis columns: {sorted(missing)}")

        writer = csv.writer(destination)
        writer.writerow(header())

        batch_ts: str | None = None
        batch_rows: list[dict[str, str]] = []

        def flush_batch() -> None:
            nonlocal initialized, last_snapshot, output_rows
            if not batch_rows or batch_ts is None:
                return
            has_snapshot = any(truthy(row["is_snapshot"]) for row in batch_rows)
            if not initialized and not has_snapshot:
                return

            apply_batch(batch_rows, bids, asks)
            if has_snapshot:
                initialized = True

            current = snapshot_depth5(bids, asks)
            if current is None or current == last_snapshot:
                return

            writer.writerow(flatten(batch_ts, current[0], current[1]))
            last_snapshot = current
            output_rows += 1

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
                print(f"{input_path.name}: read {input_rows:,} rows, wrote {output_rows:,} depth5 rows", file=sys.stderr)
            if args.max_rows and input_rows >= args.max_rows:
                break

        flush_batch()

    return input_rows, output_rows


def main() -> int:
    args = parse_args()
    input_rows, output_rows = convert(args.input, args.output, args)
    print(f"Read {input_rows:,} rows")
    print(f"Wrote {output_rows:,} depth5 rows to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
