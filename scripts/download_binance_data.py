#!/usr/bin/env python3

import argparse
import csv
import io
import urllib.request
import zipfile
from pathlib import Path


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Download and extract a daily Binance trades archive.")
    parser.add_argument("--symbol", default="BTCUSDT", help="Trading pair symbol.")
    parser.add_argument("--date", default="2024-03-01", help="UTC date in YYYY-MM-DD format.")
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("binance_trades.csv"),
        help="Output CSV path for the extracted trades.",
    )
    return parser


def build_url(symbol: str, day: str) -> str:
    return (
        f"https://data.binance.vision/data/spot/daily/trades/"
        f"{symbol}/{symbol}-trades-{day}.zip"
    )


def main() -> int:
    args = build_parser().parse_args()
    url = build_url(args.symbol, args.date)

    with urllib.request.urlopen(url) as response:
        archive_bytes = response.read()

    args.output.parent.mkdir(parents=True, exist_ok=True)

    with zipfile.ZipFile(io.BytesIO(archive_bytes)) as archive:
        csv_members = [name for name in archive.namelist() if name.endswith(".csv")]
        if not csv_members:
            raise RuntimeError("Downloaded Binance archive does not contain a CSV file.")

        with archive.open(csv_members[0], "r") as source, args.output.open("w", newline="", encoding="utf-8") as target:
            text_source = io.TextIOWrapper(source, encoding="utf-8")
            writer = csv.writer(target)
            for row in csv.reader(text_source):
                writer.writerow(row)

    print(f"Downloaded {url}")
    print(f"Saved extracted CSV to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
