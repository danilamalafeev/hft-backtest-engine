#!/usr/bin/env python3
"""Download Binance USD-M futures bookDepth and normalize it for YABE GraphEngine.

Binance futures bookDepth archives are aggregate depth buckets, not raw L2 or
bookTicker. The source CSV schema is:

timestamp,percentage,depth,notional

This script converts the closest negative bucket and closest positive bucket
for each timestamp into YABE's L1-style schema:

timestamp,bid_price,bid_qty,ask_price,ask_qty

where price is inferred as notional / depth. This is a depth-derived proxy, not
a true best bid/ask quote.
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import shutil
import sys
import time
import urllib.error
import urllib.request
import zipfile
from pathlib import Path


BASE_URL = "https://data.binance.vision/data/futures/um/daily/bookDepth"
MAX_ATTEMPTS = 3


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Download Binance USD-M futures bookDepth for YABE.")
    parser.add_argument("--date", required=True, help="Trading date in YYYY-MM-DD format")
    parser.add_argument("--symbols", nargs="+", required=True, help="Symbols such as BTCUSDT ETHUSDT ETHBTC")
    parser.add_argument("--output-dir", type=Path, default=Path("data"), help="Output directory")
    parser.add_argument(
        "--keep-raw",
        action="store_true",
        help="Keep extracted raw bookDepth CSV next to normalized output",
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
            if attempt == MAX_ATTEMPTS:
                raise RuntimeError(f"failed to download {url}: {exc}") from exc
            sleep_seconds = 2 ** attempt
            print(f"Retrying in {sleep_seconds}s after error: {exc}", file=sys.stderr)
            time.sleep(sleep_seconds)


def extract_csv(zip_path: Path, output_dir: Path) -> Path:
    with zipfile.ZipFile(zip_path) as archive:
        csv_names = [name for name in archive.namelist() if name.endswith(".csv")]
        if len(csv_names) != 1:
            raise RuntimeError(f"expected exactly one CSV in {zip_path}, found {len(csv_names)}")

        output_path = output_dir / Path(csv_names[0]).name
        with archive.open(csv_names[0]) as source, output_path.open("wb") as destination:
            shutil.copyfileobj(source, destination, length=1024 * 1024)
        return output_path


def timestamp_to_ms(value: str) -> int:
    text = value.strip()
    if text.isdigit():
        raw = int(text)
        return raw // 1_000_000 if raw > 10_000_000_000_000 else raw

    parsed = dt.datetime.strptime(text, "%Y-%m-%d %H:%M:%S")
    parsed = parsed.replace(tzinfo=dt.timezone.utc)
    return int(parsed.timestamp() * 1000)


def normalize_book_depth(raw_path: Path, clean_path: Path) -> None:
    with raw_path.open(newline="") as source, clean_path.open("w", newline="") as destination:
        reader = csv.DictReader(source)
        writer = csv.writer(destination)
        writer.writerow(["timestamp", "bid_price", "bid_qty", "ask_price", "ask_qty"])

        current_ts: str | None = None
        bid: tuple[int, float, float] | None = None
        ask: tuple[int, float, float] | None = None

        def flush() -> None:
            if current_ts is None or bid is None or ask is None:
                return
            ts_ms = timestamp_to_ms(current_ts)
            _, bid_qty, bid_notional = bid
            _, ask_qty, ask_notional = ask
            if bid_qty <= 0.0 or ask_qty <= 0.0:
                return
            writer.writerow(
                [
                    ts_ms,
                    f"{bid_notional / bid_qty:.12f}",
                    f"{bid_qty:.12f}",
                    f"{ask_notional / ask_qty:.12f}",
                    f"{ask_qty:.12f}",
                ]
            )

        for row in reader:
            ts = row["timestamp"]
            percentage = int(float(row["percentage"]))
            depth = float(row["depth"])
            notional = float(row["notional"])

            if current_ts is None:
                current_ts = ts
            elif ts != current_ts:
                flush()
                current_ts = ts
                bid = None
                ask = None

            if percentage < 0:
                if bid is None or percentage > bid[0]:
                    bid = (percentage, depth, notional)
            elif percentage > 0:
                if ask is None or percentage < ask[0]:
                    ask = (percentage, depth, notional)

        flush()


def main() -> int:
    args = parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)

    for symbol in args.symbols:
        archive_name = f"{symbol}-bookDepth-{args.date}.zip"
        url = f"{BASE_URL}/{symbol}/{archive_name}"
        zip_path = args.output_dir / archive_name
        clean_path = args.output_dir / f"{symbol}-futures-bookDepth-{args.date}.csv"

        download_file(url, zip_path)
        raw_path = extract_csv(zip_path, args.output_dir)
        normalize_book_depth(raw_path, clean_path)
        zip_path.unlink()
        if not args.keep_raw:
            raw_path.unlink()
        print(f"Wrote {clean_path}")

    print(
        "\nNote: normalized files are depth-derived BBO proxies using +/-1% buckets, "
        "not true top-of-book quotes.",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
