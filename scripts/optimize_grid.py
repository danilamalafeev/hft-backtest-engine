#!/usr/bin/env python3

import argparse
import itertools
import subprocess
from pathlib import Path
from typing import Optional

import pandas as pd


SPREAD_GRID = [2.0, 5.0, 10.0, 20.0]
GAMMA_GRID = [0.1, 0.5, 1.0, 3.0]
IMBALANCE_GRID = [2.0, 5.0, 10.0, 999.0]


def find_backtest_executable(repo_root: Path, requested: Optional[str]) -> Path:
    if requested is not None:
        executable = Path(requested)
        if not executable.is_absolute():
            executable = repo_root / executable
        return executable

    for candidate in (repo_root / "build-release" / "lob_backtest", repo_root / "build" / "lob_backtest"):
        if candidate.exists():
            return candidate

    return repo_root / "build-release" / "lob_backtest"


def parse_result_csv(stdout: str) -> dict[str, float]:
    for line in stdout.splitlines():
        if not line.startswith("RESULT_CSV,"):
            continue

        _, spread, gamma, imbalance, pnl, sharpe, max_dd = line.split(",")
        return {
            "spread": float(spread),
            "gamma": float(gamma),
            "imbalance": float(imbalance),
            "pnl": float(pnl),
            "sharpe": float(sharpe),
            "max_dd": float(max_dd),
        }

    raise RuntimeError("Backtest output did not contain RESULT_CSV line")


def run_backtest(executable: Path, csv_path: Path, spread: float, gamma: float, imbalance: float) -> dict[str, float]:
    completed = subprocess.run(
        [
            str(executable),
            str(csv_path),
            str(spread),
            str(gamma),
            str(imbalance),
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    return parse_result_csv(completed.stdout)


def main() -> int:
    repo_root = Path(__file__).resolve().parents[1]

    parser = argparse.ArgumentParser(description="Grid-search InventorySkewStrategy parameters.")
    parser.add_argument("csv_path", nargs="?", default=str(repo_root / "binance_trades.csv"))
    parser.add_argument("--exe", default=None, help="Path to lob_backtest executable")
    args = parser.parse_args()

    csv_path = Path(args.csv_path)
    if not csv_path.is_absolute():
        csv_path = repo_root / csv_path

    executable = find_backtest_executable(repo_root, args.exe)
    if not executable.exists():
        raise FileNotFoundError(f"Backtest executable not found: {executable}")

    rows: list[dict[str, float]] = []
    combinations = list(itertools.product(SPREAD_GRID, GAMMA_GRID, IMBALANCE_GRID))
    for index, (spread, gamma, imbalance) in enumerate(combinations, start=1):
        print(
            f"[{index:02d}/{len(combinations)}] spread={spread} gamma={gamma} imbalance={imbalance}",
            flush=True,
        )
        rows.append(run_backtest(executable, csv_path, spread, gamma, imbalance))

    frame = pd.DataFrame(rows)
    frame = frame.sort_values("sharpe", ascending=False)

    print("\nTop 10 parameter combinations by Sharpe:")
    print(frame.head(10).to_string(index=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
