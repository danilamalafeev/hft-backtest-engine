#!/usr/bin/env python3

from __future__ import annotations

import shutil
import sys
import time
import urllib.error
import urllib.request
import zipfile
from pathlib import Path


DATE = "2024-03-05"
SYMBOLS = ("BTCUSDT", "ETHUSDT", "ETHBTC")
BASE_URL = "https://data.binance.vision/data/spot/daily/trades"
DATA_DIR = Path("data")
MAX_ATTEMPTS = 3


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

        csv_name = csv_names[0]
        output_path = output_dir / Path(csv_name).name
        with archive.open(csv_name) as source, output_path.open("wb") as destination:
            shutil.copyfileobj(source, destination, length=1024 * 1024)
        return output_path


def main() -> int:
    DATA_DIR.mkdir(parents=True, exist_ok=True)

    extracted_paths: list[Path] = []
    for symbol in SYMBOLS:
        archive_name = f"{symbol}-trades-{DATE}.zip"
        url = f"{BASE_URL}/{symbol}/{archive_name}"
        zip_path = DATA_DIR / archive_name

        download_file(url, zip_path)
        csv_path = extract_csv(zip_path, DATA_DIR)
        zip_path.unlink()
        extracted_paths.append(csv_path)
        print(f"Extracted {csv_path}")

    print("\nRun triangular arbitrage backtest:")
    print(
        "./build-release/lob_backtest "
        + " ".join(str(path) for path in extracted_paths)
        + " --maker-fee-bps -1.0 --taker-fee-bps 7.5 --base-latency-ns 500000"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
