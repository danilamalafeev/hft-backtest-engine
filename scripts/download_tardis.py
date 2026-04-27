#!/usr/bin/env python3
"""Download free first-of-month Tardis.dev CSV datasets.

Tardis downloadable CSV files are public for the first day of each month.
This script downloads gzip-compressed incremental L2 book updates that can be
converted to YABE BBO CSVs with scripts/tardis_to_bookticker.py.

Example:
  python3 scripts/download_tardis.py \
    --exchange binance \
    --date 2024-03-01 \
    --symbols BTCUSDT ETHUSDT ETHBTC
"""

from __future__ import annotations

import argparse
import shutil
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path


BASE_URL = "https://datasets.tardis.dev/v1"
MAX_ATTEMPTS = 3


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Download free Tardis.dev incremental L2 CSV datasets.")
    parser.add_argument("--exchange", default="binance", help="Tardis exchange id, e.g. binance or binance-futures")
    parser.add_argument("--date", default="2024-03-01", help="Date in YYYY-MM-DD format. First day is free.")
    parser.add_argument("--symbols", nargs="+", default=["BTCUSDT", "ETHUSDT", "ETHBTC"])
    parser.add_argument("--data-type", default="incremental_book_L2")
    parser.add_argument("--output-dir", type=Path, default=Path("data/tardis"))
    parser.add_argument("--force", action="store_true", help="Re-download existing files")
    return parser.parse_args()


def dataset_url(exchange: str, data_type: str, date: str, symbol: str) -> str:
    year, month, day = date.split("-")
    return f"{BASE_URL}/{exchange}/{data_type}/{year}/{month}/{day}/{symbol.upper()}.csv.gz"


def output_path(output_dir: Path, exchange: str, data_type: str, date: str, symbol: str) -> Path:
    return output_dir / f"tardis-{exchange}-{symbol.upper()}-{data_type}-{date}.csv.gz"


def download_file(url: str, destination: Path, force: bool) -> None:
    if destination.exists() and destination.stat().st_size > 0 and not force:
        print(f"Exists {destination}")
        return

    tmp_path = destination.with_suffix(destination.suffix + ".tmp")
    for attempt in range(1, MAX_ATTEMPTS + 1):
        try:
            print(f"Downloading {url}")
            request = urllib.request.Request(url, headers={"User-Agent": "YABE/1.0"})
            with urllib.request.urlopen(request, timeout=120) as response:
                with tmp_path.open("wb") as output:
                    shutil.copyfileobj(response, output, length=1024 * 1024)
            tmp_path.replace(destination)
            print(f"Wrote {destination} ({destination.stat().st_size:,} bytes)")
            return
        except (urllib.error.URLError, TimeoutError) as exc:
            if tmp_path.exists():
                tmp_path.unlink()
            if attempt == MAX_ATTEMPTS:
                raise RuntimeError(f"failed to download {url}: {exc}") from exc
            sleep_seconds = 2 ** attempt
            print(f"Retrying in {sleep_seconds}s after error: {exc}", file=sys.stderr)
            time.sleep(sleep_seconds)


def main() -> int:
    args = parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)

    for symbol in args.symbols:
        url = dataset_url(args.exchange, args.data_type, args.date, symbol)
        path = output_path(args.output_dir, args.exchange, args.data_type, args.date, symbol)
        download_file(url, path, args.force)

    print("\nConvert to YABE BBO CSV:")
    for symbol in args.symbols:
        source = output_path(args.output_dir, args.exchange, args.data_type, args.date, symbol)
        target = args.output_dir / f"tardis-{args.exchange}-{symbol.upper()}-bbo-{args.date}.csv"
        print(f"  python3 scripts/tardis_to_bookticker.py {source} -o {target}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
