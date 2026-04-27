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


BOOK_TICKER_URL = "https://data.binance.vision/data/spot/daily/bookTicker"
TRADES_URL = "https://data.binance.vision/data/spot/daily/trades"
MAX_ATTEMPTS = 3


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Download Binance Vision spot bookTicker L1 data.")
    parser.add_argument("--date", required=True, help="Trading date in YYYY-MM-DD format")
    parser.add_argument("--symbols", nargs="+", required=True, help="Symbols such as BTCUSDT ETHUSDT ETHBTC")
    parser.add_argument("--output-dir", type=Path, default=Path("data"), help="Output directory")
    parser.add_argument(
        "--fallback",
        choices=("synthetic-trades", "none"),
        default="synthetic-trades",
        help="Fallback when spot bookTicker archives are unavailable on Binance Vision",
    )
    parser.add_argument(
        "--synthetic-spread-bps",
        type=float,
        default=2.0,
        help="Bid/ask spread used when synthesizing BBO from spot trades",
    )
    parser.add_argument(
        "--synthetic-qty-multiplier",
        type=float,
        default=10.0,
        help="Quantity multiplier used when synthesizing BBO size from trade quantity",
    )
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
            if isinstance(exc, urllib.error.HTTPError) and exc.code == 404:
                raise FileNotFoundError(url) from exc
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


def extract_raw_csv(zip_path: Path, output_dir: Path, stem: str) -> Path:
    with zipfile.ZipFile(zip_path) as archive:
        csv_names = [name for name in archive.namelist() if name.endswith(".csv")]
        if len(csv_names) != 1:
            raise RuntimeError(f"expected exactly one CSV in {zip_path}, found {len(csv_names)}")

        raw_path = output_dir / f"{stem}.raw.csv"
        with archive.open(csv_names[0]) as source, raw_path.open("wb") as destination:
            shutil.copyfileobj(source, destination, length=1024 * 1024)
        return raw_path


def trade_row_to_clean_bbo(
    row: list[str],
    spread_fraction: float,
    qty_multiplier: float,
) -> tuple[str, str, str, str, str]:
    # Binance spot trades archives:
    # trade_id,price,qty,quote_qty,time,is_buyer_maker,is_best_match
    if len(row) < 5:
        raise RuntimeError(f"unsupported trades row with {len(row)} columns: {row}")

    price = float(row[1])
    qty = max(float(row[2]) * qty_multiplier, 1e-12)
    timestamp = row[4].strip()
    is_buyer_maker = len(row) > 5 and row[5].strip().lower() == "true"

    if is_buyer_maker:
        bid_price = price
        ask_price = price * (1.0 + spread_fraction)
    else:
        bid_price = price * (1.0 - spread_fraction)
        ask_price = price

    return (
        timestamp,
        f"{bid_price:.12f}",
        f"{qty:.12f}",
        f"{ask_price:.12f}",
        f"{qty:.12f}",
    )


def synthesize_bookticker_from_trades(
    raw_trades_path: Path,
    clean_path: Path,
    spread_bps: float,
    qty_multiplier: float,
) -> None:
    spread_fraction = spread_bps * 0.0001
    with raw_trades_path.open(newline="") as source, clean_path.open("w", newline="") as destination:
        reader = csv.reader(source)
        writer = csv.writer(destination)
        writer.writerow(["timestamp", "bid_price", "bid_qty", "ask_price", "ask_qty"])

        for row in reader:
            if not row:
                continue
            if row[0].strip() and not row[0].strip()[0].isdigit():
                continue
            writer.writerow(trade_row_to_clean_bbo(row, spread_fraction, qty_multiplier))


def main() -> int:
    args = parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)

    for symbol in args.symbols:
        archive_name = f"{symbol}-bookTicker-{args.date}.zip"
        url = f"{BOOK_TICKER_URL}/{symbol}/{archive_name}"
        zip_path = args.output_dir / archive_name
        clean_path = args.output_dir / f"{symbol}-bookTicker-{args.date}.csv"

        try:
            download_file(url, zip_path)
            raw_path = first_csv_from_zip(zip_path, args.output_dir)
            normalize_csv(raw_path, clean_path)
            raw_path.unlink()
            zip_path.unlink()
        except FileNotFoundError:
            if args.fallback == "none":
                raise

            print(
                f"Spot bookTicker archive not found for {symbol}; "
                "falling back to synthetic BBO from spot trades.",
                file=sys.stderr,
            )
            trades_archive_name = f"{symbol}-trades-{args.date}.zip"
            trades_url = f"{TRADES_URL}/{symbol}/{trades_archive_name}"
            trades_zip_path = args.output_dir / trades_archive_name
            existing_trades_path = args.output_dir / f"{symbol}-trades-{args.date}.csv"
            if existing_trades_path.exists():
                raw_path = existing_trades_path
                cleanup_raw = False
                cleanup_zip = False
                print(f"Using existing trades CSV {existing_trades_path}")
            else:
                download_file(trades_url, trades_zip_path)
                raw_path = extract_raw_csv(trades_zip_path, args.output_dir, f"{symbol}-trades-{args.date}")
                cleanup_raw = True
                cleanup_zip = True
            synthesize_bookticker_from_trades(
                raw_path,
                clean_path,
                args.synthetic_spread_bps,
                args.synthetic_qty_multiplier,
            )
            if cleanup_raw:
                raw_path.unlink()
            if cleanup_zip:
                trades_zip_path.unlink()
            print(
                "WARNING: wrote synthetic BBO, not real historical L2. "
                "Use it for pipeline testing only.",
                file=sys.stderr,
            )
        print(f"Wrote {clean_path}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
