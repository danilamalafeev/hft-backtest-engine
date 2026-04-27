#!/usr/bin/env python3

from __future__ import annotations

import argparse
import csv
import shutil
import sys
import time
import urllib.error
import urllib.request
import zipfile
from pathlib import Path


BASE_URL = "https://data.binance.vision/data/spot/daily/bookTicker"
MAX_ATTEMPTS = 3


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Download Binance Vision spot bookTicker L1 data.")
    parser.add_argument("--date", required=True, help="Trading date in YYYY-MM-DD format")
    parser.add_argument("--symbols", nargs="+", required=True, help="Symbols such as BTCUSDT ETHUSDT ETHBTC")
    parser.add_argument("--output-dir", type=Path, default=Path("data"), help="Output directory")
    return parser.parse_args()


def download_file(url: str, destination: Path) -> None:
    for attempt in range(1, MAX_ATTEMPTS + 1):
        try:
            print(f"Downloading {url}")
            with urllib.request.urlopen(url, timeout=60) as response:
                with destination.open("wb") as output:
                    shutil.copyfileobj(response, output, length=1024 * 1024)
            return
        except (urllib.error.URLError, TimeoutError) as exc:
            if attempt == MAX_ATTEMPTS:
                raise RuntimeError(f"failed to download {url}: {exc}") from exc
            sleep_seconds = 2 ** attempt
            print(f"Retrying in {sleep_seconds}s after error: {exc}", file=sys.stderr)
            time.sleep(sleep_seconds)


def first_csv_from_zip(zip_path: Path, output_dir: Path) -> Path:
    with zipfile.ZipFile(zip_path) as archive:
        csv_names = [name for name in archive.namelist() if name.endswith(".csv")]
        if len(csv_names) != 1:
            raise RuntimeError(f"expected exactly one CSV in {zip_path}, found {len(csv_names)}")

        raw_path = output_dir / f"{zip_path.stem}.raw.csv"
        with archive.open(csv_names[0]) as source, raw_path.open("wb") as destination:
            shutil.copyfileobj(source, destination, length=1024 * 1024)
        return raw_path


def row_to_clean(row: list[str], header: list[str] | None) -> tuple[str, str, str, str, str]:
    if header:
        values = {name.strip(): value.strip() for name, value in zip(header, row)}
        timestamp = (
            values.get("timestamp")
            or values.get("transaction_time")
            or values.get("event_time")
            or values.get("T")
            or values.get("E")
        )
        bid_price = values.get("bid_price") or values.get("best_bid_price") or values.get("bidPrice") or values.get("b")
        bid_qty = values.get("bid_qty") or values.get("best_bid_qty") or values.get("bidQty") or values.get("B")
        ask_price = values.get("ask_price") or values.get("best_ask_price") or values.get("askPrice") or values.get("a")
        ask_qty = values.get("ask_qty") or values.get("best_ask_qty") or values.get("askQty") or values.get("A")
        if timestamp and bid_price and bid_qty and ask_price and ask_qty:
            return timestamp, bid_price, bid_qty, ask_price, ask_qty

    # Binance bookTicker archives are commonly:
    # update_id,symbol,bid_price,bid_qty,ask_price,ask_qty,transaction_time,event_time
    if len(row) >= 8:
        return row[6].strip(), row[2].strip(), row[3].strip(), row[4].strip(), row[5].strip()
    if len(row) >= 5:
        return row[0].strip(), row[1].strip(), row[2].strip(), row[3].strip(), row[4].strip()
    raise RuntimeError(f"unsupported bookTicker row with {len(row)} columns: {row}")


def normalize_csv(raw_path: Path, clean_path: Path) -> None:
    with raw_path.open(newline="") as source, clean_path.open("w", newline="") as destination:
        reader = csv.reader(source)
        writer = csv.writer(destination)
        writer.writerow(["timestamp", "bid_price", "bid_qty", "ask_price", "ask_qty"])

        first = next(reader, None)
        if first is None:
            return

        header: list[str] | None = None
        if first and not first[0].strip().isdigit():
            header = first
        else:
            writer.writerow(row_to_clean(first, None))

        for row in reader:
            if not row:
                continue
            writer.writerow(row_to_clean(row, header))


def main() -> int:
    args = parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)

    for symbol in args.symbols:
        archive_name = f"{symbol}-bookTicker-{args.date}.zip"
        url = f"{BASE_URL}/{symbol}/{archive_name}"
        zip_path = args.output_dir / archive_name
        clean_path = args.output_dir / f"{symbol}-bookTicker-{args.date}.csv"

        download_file(url, zip_path)
        raw_path = first_csv_from_zip(zip_path, args.output_dir)
        normalize_csv(raw_path, clean_path)
        raw_path.unlink()
        zip_path.unlink()
        print(f"Wrote {clean_path}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
