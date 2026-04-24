#!/usr/bin/env python3

import argparse
import csv
import random
from pathlib import Path


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Generate synthetic limit-order CSV data.")
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("test_orders.csv"),
        help="Output CSV path.",
    )
    parser.add_argument(
        "--count",
        type=int,
        default=1_000_000,
        help="Number of orders to generate.",
    )
    parser.add_argument(
        "--seed",
        type=int,
        default=42,
        help="Random seed for reproducibility.",
    )
    return parser


def main() -> int:
    args = build_parser().parse_args()
    random.seed(args.seed)

    args.output.parent.mkdir(parents=True, exist_ok=True)

    mid_price = 100.0
    quantities = [1, 5, 10, 25, 50, 100, 250]

    with args.output.open("w", newline="", encoding="utf-8") as output_file:
        writer = csv.writer(output_file)
        writer.writerow(["id", "price", "quantity", "side"])

        for order_id in range(1, args.count + 1):
            mid_price += random.uniform(-0.05, 0.05)
            mid_price = max(90.0, min(110.0, mid_price))

            order_price = round(mid_price + random.uniform(-0.25, 0.25), 2)
            order_quantity = random.choice(quantities)
            order_side = random.choice(["Buy", "Sell"])

            writer.writerow([order_id, f"{order_price:.2f}", order_quantity, order_side])

    print(f"Generated {args.count} orders into {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
