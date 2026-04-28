#!/usr/bin/env python3
"""Convert Tardis incremental_book_L2 CSVs to YABE L2Update CSV format.

Input:  exchange,symbol,timestamp,local_timestamp,is_snapshot,side,price,amount
Output: timestamp,is_snapshot,is_bid,price,qty

Timestamps are converted from microseconds to nanoseconds.
"""

import argparse
import csv
import gzip
import sys
from pathlib import Path


def truthy(value: str) -> bool:
    return value.strip().lower() in {"true", "1", "t", "yes"}


def open_text(path: Path):
    if path.suffix == ".gz":
        return gzip.open(path, "rt", newline="")
    return path.open(newline="")


def parse_args():
    parser = argparse.ArgumentParser(description="Convert Tardis incremental_book_L2 to YABE L2Update CSV.")
    parser.add_argument("input", type=Path, help="Tardis incremental_book_L2 .csv or .csv.gz")
    parser.add_argument("-o", "--output", type=Path, required=True, help="Output L2Update CSV")
    parser.add_argument("--timestamp-source", choices=("local_timestamp", "timestamp"), default="local_timestamp")
    parser.add_argument("--max-rows", type=int, default=0)
    parser.add_argument("--progress-every", type=int, default=5_000_000)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)

    input_rows = 0
    output_rows = 0

    with open_text(args.input) as source, args.output.open("w", newline="") as dest:
        reader = csv.DictReader(source)
        required = {"timestamp", "local_timestamp", "is_snapshot", "side", "price", "amount"}
        missing = required.difference(reader.fieldnames or [])
        if missing:
            print(f"ERROR: {args.input} missing columns: {sorted(missing)}", file=sys.stderr)
            return 1

        writer = csv.writer(dest)
        writer.writerow(["timestamp", "is_snapshot", "is_bid", "price", "qty"])

        for row in reader:
            input_rows += 1
            ts_us = int(row[args.timestamp_source])
            ts_ns = ts_us * 1_000  # microseconds -> nanoseconds
            is_snap = 1 if truthy(row["is_snapshot"]) else 0
            side = row["side"].strip().lower()
            is_bid = 1 if side == "bid" else 0
            price = row["price"]
            qty = row["amount"]

            writer.writerow([ts_ns, is_snap, is_bid, price, qty])
            output_rows += 1

            if args.progress_every and input_rows % args.progress_every == 0:
                print(f"{args.input.name}: {input_rows:,} rows -> {output_rows:,} written", file=sys.stderr)
            if args.max_rows and input_rows >= args.max_rows:
                break

    print(f"Read {input_rows:,} rows, wrote {output_rows:,} L2Update rows to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
