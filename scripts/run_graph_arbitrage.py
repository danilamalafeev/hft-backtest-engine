#!/usr/bin/env python3
"""Run YABE GraphEngine from Python.

Expected input files are clean bookTicker/L1 CSVs with columns:
timestamp,bid_price,bid_qty,ask_price,ask_qty

Examples:
  PYTHONPATH=build-release python3 scripts/run_graph_arbitrage.py \
    --pair BTC/USDT:data/BTCUSDT-bookTicker-2024-03-05.csv \
    --pair ETH/USDT:data/ETHUSDT-bookTicker-2024-03-05.csv \
    --pair ETH/BTC:data/ETHBTC-bookTicker-2024-03-05.csv

  python3 scripts/run_graph_arbitrage.py --auto-triangle --date 2024-03-05
"""

from __future__ import annotations

import argparse
import csv
import os
import sys
from dataclasses import dataclass
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BUILD_DIRS = [
    PROJECT_ROOT / "build-release",
    PROJECT_ROOT / "build",
]


@dataclass(frozen=True)
class PairSpec:
    base: str
    quote: str
    path: Path


def add_yabe_to_path(build_dir: str | None) -> None:
    candidates = [Path(build_dir).expanduser().resolve()] if build_dir else DEFAULT_BUILD_DIRS
    for candidate in candidates:
        if (candidate / "yabe.so").exists() or any(candidate.glob("yabe*.pyd")):
            sys.path.insert(0, str(candidate))
            return
    if build_dir:
        sys.path.insert(0, str(Path(build_dir).expanduser().resolve()))


def parse_pair_spec(value: str) -> PairSpec:
    try:
        pair, file_path = value.split(":", 1)
        base, quote = pair.split("/", 1)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("pair must look like BASE/QUOTE:path/to/file.csv") from exc

    path = Path(file_path).expanduser()
    if not path.is_absolute():
        path = PROJECT_ROOT / path
    if not path.exists():
        raise argparse.ArgumentTypeError(f"CSV file does not exist: {path}")

    return PairSpec(base=base.upper(), quote=quote.upper(), path=path)


def find_auto_triangle(data_dir: Path, date: str) -> list[PairSpec]:
    symbols = [
        ("BTC", "USDT", "BTCUSDT"),
        ("ETH", "USDT", "ETHUSDT"),
        ("ETH", "BTC", "ETHBTC"),
    ]

    specs: list[PairSpec] = []
    for base, quote, symbol in symbols:
        candidates = [
            data_dir / f"{symbol}-bookTicker-{date}.csv",
            data_dir / f"{symbol}-bookticker-{date}.csv",
            data_dir / f"tardis-binance-{symbol}-bbo-{date}.csv",
            data_dir / f"tardis-binance-futures-{symbol}-bbo-{date}.csv",
            data_dir / f"tardis-binance-{symbol}-depth5-{date}.csv",
            data_dir / f"tardis-binance-futures-{symbol}-depth5-{date}.csv",
            data_dir / "tardis" / f"tardis-binance-{symbol}-bbo-{date}.csv",
            data_dir / "tardis" / f"tardis-binance-futures-{symbol}-bbo-{date}.csv",
            data_dir / "tardis" / f"tardis-binance-{symbol}-depth5-{date}.csv",
            data_dir / "tardis" / f"tardis-binance-futures-{symbol}-depth5-{date}.csv",
            data_dir / f"{symbol}.csv",
        ]
        found = next((path for path in candidates if path.exists()), None)
        if found is None:
            raise FileNotFoundError(
                f"Could not find bookTicker CSV for {symbol}. "
                f"Run: python3 scripts/download_bookticker_data.py --date {date} --symbols BTCUSDT ETHUSDT ETHBTC"
            )
        specs.append(PairSpec(base=base, quote=quote, path=found))
    return specs


def write_summary_csv(path: Path, rows: list[dict[str, object]]) -> None:
    if not rows:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def run_once(yabe, args: argparse.Namespace, pair_specs: list[PairSpec]) -> dict[str, object]:
    engine = yabe.GraphEngine(
        initial_usdt=args.initial_usdt,
        latency_ns=args.latency_ns,
        intra_leg_latency_ns=args.intra_leg_latency_ns,
        taker_fee_bps=args.taker_fee_bps,
        max_cycle_notional_usdt=args.max_cycle_notional_usdt,
        max_adverse_obi=args.max_adverse_obi,
        max_spread_bps=args.max_spread_bps,
        min_depth_usdt=args.min_depth_usdt,
        min_cycle_edge_bps=args.min_cycle_edge_bps,
        cycle_snapshot_reserve=args.cycle_snapshot_reserve,
        quote_asset=args.quote_asset,
    )

    for spec in pair_specs:
        engine.add_pair(spec.base, spec.quote, str(spec.path))

    result = engine.run()
    assets = list(engine.assets)
    balances = list(result.balances)

    print("ASSETS:", ",".join(f"{idx}={name}" for idx, name in enumerate(assets)))
    print(
        "RESULT_GRAPH_CSV,"
        "events,cycles,attempts,completed,panic,latency_ns,taker_fee_bps,max_notional,"
        "max_adverse_obi,max_spread_bps,min_depth_usdt,min_cycle_edge_bps,cycle_snapshots,"
        "final_usdt,final_nav,inventory_risk"
    )
    print(
        "RESULT_GRAPH_CSV,"
        f"{result.events_processed},"
        f"{result.cycles_detected},"
        f"{result.attempted_cycles},"
        f"{result.completed_cycles},"
        f"{result.panic_closes},"
        f"{args.latency_ns},"
        f"{args.taker_fee_bps},"
        f"{args.max_cycle_notional_usdt},"
        f"{args.max_adverse_obi},"
        f"{args.max_spread_bps},"
        f"{args.min_depth_usdt},"
        f"{args.min_cycle_edge_bps},"
        f"{result.cycle_snapshot_count},"
        f"{result.final_usdt:.10f},"
        f"{result.final_nav:.10f},"
        f"{result.inventory_risk:.10f}"
    )

    print("BALANCES:")
    for asset, balance in zip(assets, balances):
        print(f"  {asset}: {balance:.12f}")

    return {
        "events": result.events_processed,
        "cycles": result.cycles_detected,
        "attempts": result.attempted_cycles,
        "completed": result.completed_cycles,
        "panic": result.panic_closes,
        "latency_ns": args.latency_ns,
        "intra_leg_latency_ns": args.intra_leg_latency_ns,
        "taker_fee_bps": args.taker_fee_bps,
        "max_notional": args.max_cycle_notional_usdt,
        "max_adverse_obi": args.max_adverse_obi,
        "max_spread_bps": args.max_spread_bps,
        "min_depth_usdt": args.min_depth_usdt,
        "min_cycle_edge_bps": args.min_cycle_edge_bps,
        "cycle_snapshots": result.cycle_snapshot_count,
        "final_usdt": result.final_usdt,
        "final_nav": result.final_nav,
        "inventory_risk": result.inventory_risk,
        "last_cycle": "->".join(str(asset) for asset in result.last_cycle),
    }


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Run YABE dynamic graph arbitrage backtest.")
    parser.add_argument("--build-dir", default=os.environ.get("YABE_BUILD_DIR"), help="Directory containing yabe.so")
    parser.add_argument("--pair", action="append", type=parse_pair_spec, help="Pair spec BASE/QUOTE:path.csv")
    parser.add_argument("--auto-triangle", action="store_true", help="Use BTCUSDT, ETHUSDT, ETHBTC from data dir")
    parser.add_argument("--data-dir", default=str(PROJECT_ROOT / "data"))
    parser.add_argument("--date", default="2024-03-05")
    parser.add_argument("--initial-usdt", type=float, default=100_000_000.0)
    parser.add_argument("--latency-ns", type=int, default=500_000)
    parser.add_argument("--intra-leg-latency-ns", type=int, default=75)
    parser.add_argument("--taker-fee-bps", type=float, default=7.5)
    parser.add_argument("--max-cycle-notional-usdt", type=float, default=1_000.0)
    parser.add_argument("--max-adverse-obi", type=float, default=1.0)
    parser.add_argument("--max-spread-bps", type=float, default=1_000.0)
    parser.add_argument("--min-depth-usdt", type=float, default=0.0)
    parser.add_argument("--min-cycle-edge-bps", type=float, default=0.0)
    parser.add_argument("--cycle-snapshot-reserve", type=int, default=100_000)
    parser.add_argument("--quote-asset", default="USDT")
    parser.add_argument("--summary-csv", default="", help="Optional path for one-row result CSV")
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    add_yabe_to_path(args.build_dir)
    import yabe  # pylint: disable=import-error,import-outside-toplevel

    if args.auto_triangle:
        pair_specs = find_auto_triangle(Path(args.data_dir).expanduser().resolve(), args.date)
    else:
        pair_specs = args.pair or []

    if len(pair_specs) < 2:
        parser.error("Provide at least two --pair entries, or use --auto-triangle")

    row = run_once(yabe, args, pair_specs)
    if args.summary_csv:
        write_summary_csv(Path(args.summary_csv).expanduser(), [row])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
