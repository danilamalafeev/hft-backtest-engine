#!/usr/bin/env python3

import argparse
from pathlib import Path

import pandas as pd
import plotly.graph_objects as go
from plotly.subplots import make_subplots


REQUIRED_COLUMNS = {
    "asset_id",
    "event_type",
    "price",
    "qty",
    "pnl_impact",
    "current_nav",
}


def normalize_columns(frame: pd.DataFrame) -> pd.DataFrame:
    frame = frame.rename(
        columns={
            "nanoseconds": "timestamp_ns",
            "timestamp": "timestamp_ns",
        }
    )

    required = REQUIRED_COLUMNS | {"timestamp_ns"}
    missing = required - set(frame.columns)
    if missing:
        raise ValueError(f"Missing required columns: {sorted(missing)}")

    frame["timestamp"] = pd.to_datetime(frame["timestamp_ns"], unit="ns", utc=True)
    frame["asset_id"] = pd.to_numeric(frame["asset_id"], errors="raise").astype("int16", copy=False)
    frame["event_type"] = frame["event_type"].astype(str)
    frame["price"] = pd.to_numeric(frame["price"], errors="coerce")
    frame["qty"] = pd.to_numeric(frame["qty"], errors="coerce").fillna(0.0)
    frame["pnl_impact"] = pd.to_numeric(frame["pnl_impact"], errors="coerce").fillna(0.0)
    frame["current_nav"] = pd.to_numeric(frame["current_nav"], errors="coerce")

    if "side" in frame.columns:
        frame["side"] = frame["side"].astype(str).str.upper()
    else:
        event_text = frame["event_type"].astype(str).str.upper()
        frame["side"] = "TRADE"
        frame.loc[event_text.str.contains("BUY"), "side"] = "BUY"
        frame.loc[event_text.str.contains("SELL"), "side"] = "SELL"

    return frame.sort_values("timestamp", kind="mergesort")


def downsample(frame: pd.DataFrame, max_points: int) -> pd.DataFrame:
    if len(frame) <= max_points:
        return frame
    step = max(1, len(frame) // max_points)
    return frame.iloc[::step].copy()


def build_inventory(frame: pd.DataFrame) -> pd.DataFrame:
    inventory = frame[["timestamp", "asset_id", "qty", "side"]].copy()
    inventory["signed_qty"] = inventory["qty"]

    known_side = inventory["side"].isin(["BUY", "SELL"]).any()
    if known_side:
        absolute_qty = inventory["qty"].abs()
        inventory["signed_qty"] = 0.0
        inventory.loc[inventory["side"] == "BUY", "signed_qty"] = absolute_qty
        inventory.loc[inventory["side"] == "SELL", "signed_qty"] = -absolute_qty

    inventory["position"] = inventory.groupby("asset_id", sort=False)["signed_qty"].cumsum()
    return inventory


def add_trade_markers(
    figure: go.Figure,
    asset_frame: pd.DataFrame,
    asset_id: int,
    color: str,
    max_points_per_trace: int,
) -> None:
    buys = asset_frame[asset_frame["side"] == "BUY"]
    sells = asset_frame[asset_frame["side"] == "SELL"]

    if not buys.empty:
        buys = downsample(buys[["timestamp", "price", "qty", "pnl_impact"]].dropna(), max_points_per_trace)
        figure.add_trace(
            go.Scattergl(
                x=buys["timestamp"],
                y=buys["price"],
                mode="markers",
                name=f"Asset {asset_id} BUY",
                marker={"symbol": "triangle-up", "size": 7, "color": "#16a34a"},
                customdata=buys[["qty", "pnl_impact"]],
                hovertemplate=(
                    "BUY<br>time=%{x}<br>price=%{y}<br>"
                    "qty=%{customdata[0]:.8f}<br>"
                    "pnl_impact=%{customdata[1]:.6f}<extra></extra>"
                ),
            ),
            row=2,
            col=1,
        )

    if not sells.empty:
        sells = downsample(sells[["timestamp", "price", "qty", "pnl_impact"]].dropna(), max_points_per_trace)
        figure.add_trace(
            go.Scattergl(
                x=sells["timestamp"],
                y=sells["price"],
                mode="markers",
                name=f"Asset {asset_id} SELL",
                marker={"symbol": "triangle-down", "size": 7, "color": "#dc2626"},
                customdata=sells[["qty", "pnl_impact"]],
                hovertemplate=(
                    "SELL<br>time=%{x}<br>price=%{y}<br>"
                    "qty=%{customdata[0]:.8f}<br>"
                    "pnl_impact=%{customdata[1]:.6f}<extra></extra>"
                ),
            ),
            row=2,
            col=1,
        )

    if buys.empty and sells.empty:
        trades = downsample(asset_frame[["timestamp", "price", "qty", "pnl_impact"]].dropna(), max_points_per_trace)
        figure.add_trace(
            go.Scattergl(
                x=trades["timestamp"],
                y=trades["price"],
                mode="markers",
                name=f"Asset {asset_id} Trades",
                marker={"symbol": "circle", "size": 5, "color": color, "opacity": 0.55},
                customdata=trades[["qty", "pnl_impact"]],
                hovertemplate=(
                    "TRADE<br>time=%{x}<br>price=%{y}<br>"
                    "qty=%{customdata[0]:.8f}<br>"
                    "pnl_impact=%{customdata[1]:.6f}<extra></extra>"
                ),
            ),
            row=2,
            col=1,
        )


def make_dashboard(input_csv: Path, output_html: Path, max_points_per_trace: int) -> None:
    frame = pd.read_csv(input_csv)
    if frame.empty:
        raise RuntimeError(f"Input CSV is empty: {input_csv}")

    frame = normalize_columns(frame)
    inventory = build_inventory(frame)
    asset_ids = sorted(frame["asset_id"].dropna().unique())

    figure = make_subplots(
        rows=3,
        cols=1,
        shared_xaxes=True,
        vertical_spacing=0.045,
        row_heights=[0.34, 0.38, 0.28],
        subplot_titles=(
            "Total Equity / NAV",
            "Asset Prices and Trade Markers",
            "Inventory / Position by Asset",
        ),
    )

    nav = frame[["timestamp", "current_nav"]].dropna().drop_duplicates("timestamp", keep="last")
    nav = downsample(nav, max_points_per_trace)
    figure.add_trace(
        go.Scattergl(
            x=nav["timestamp"],
            y=nav["current_nav"],
            mode="lines",
            name="Current NAV",
            line={"width": 1.5, "color": "#2563eb"},
        ),
        row=1,
        col=1,
    )

    colors = ["#2563eb", "#16a34a", "#dc2626", "#9333ea", "#ea580c", "#0891b2", "#4f46e5", "#be123c"]

    for index, asset_id in enumerate(asset_ids):
        color = colors[index % len(colors)]
        asset_frame = frame[frame["asset_id"] == asset_id]
        prices = downsample(asset_frame[["timestamp", "price"]].dropna(), max_points_per_trace)

        figure.add_trace(
            go.Scattergl(
                x=prices["timestamp"],
                y=prices["price"],
                mode="lines",
                name=f"Asset {asset_id} Price",
                line={"width": 1.1, "color": color},
            ),
            row=2,
            col=1,
        )

        add_trade_markers(figure, asset_frame, int(asset_id), color, max_points_per_trace)

        asset_inventory = inventory[inventory["asset_id"] == asset_id][["timestamp", "position"]]
        asset_inventory = downsample(asset_inventory, max_points_per_trace)
        figure.add_trace(
            go.Scattergl(
                x=asset_inventory["timestamp"],
                y=asset_inventory["position"],
                mode="lines",
                name=f"Asset {asset_id} Position",
                line={"width": 1.0, "color": color},
                fill="tozeroy",
                opacity=0.55,
            ),
            row=3,
            col=1,
        )

    figure.update_layout(
        title=f"HFT Backtest Dashboard - {input_csv.name}",
        template="plotly_white",
        height=1050,
        hovermode="x unified",
        legend={"orientation": "h", "yanchor": "bottom", "y": 1.01, "xanchor": "left", "x": 0.0},
        margin={"l": 60, "r": 30, "t": 90, "b": 45},
    )
    figure.update_xaxes(matches="x")
    figure.update_xaxes(title_text="Time", row=3, col=1)
    figure.update_yaxes(title_text="NAV", row=1, col=1)
    figure.update_yaxes(title_text="Price", row=2, col=1)
    figure.update_yaxes(title_text="Position", row=3, col=1)

    figure.write_html(output_html, include_plotlyjs="cdn", full_html=True)


def main() -> int:
    parser = argparse.ArgumentParser(description="Visualize HFT backtest async logger output.")
    parser.add_argument("csv", type=Path, help="Async log CSV from --async-log")
    parser.add_argument("-o", "--output", type=Path, default=Path("hft_dashboard.html"))
    parser.add_argument("--max-points-per-trace", type=int, default=500_000)
    args = parser.parse_args()

    make_dashboard(args.csv, args.output, args.max_points_per_trace)
    print(f"Wrote interactive dashboard: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
