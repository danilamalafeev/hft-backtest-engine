#!/usr/bin/env python3

import argparse
from pathlib import Path

import pandas as pd
import plotly.graph_objects as go
from plotly.subplots import make_subplots


def main() -> int:
    parser = argparse.ArgumentParser(description="Plot LOB backtest trace output.")
    parser.add_argument("trace_csv", nargs="?", default="trace_log.csv")
    parser.add_argument("--output", default="backtest_trace.html")
    args = parser.parse_args()

    trace_path = Path(args.trace_csv)
    output_path = Path(args.output)

    frame = pd.read_csv(trace_path)
    if frame.empty:
        raise RuntimeError(f"Trace file is empty: {trace_path}")

    frame["pnl"] = frame["equity"] - frame["equity"].iloc[0]
    bot_buys = frame[(frame["is_bot_trade"] == 1) & (frame["trade_side"] == "Buy")]
    bot_sells = frame[(frame["is_bot_trade"] == 1) & (frame["trade_side"] == "Sell")]

    figure = make_subplots(
        rows=2,
        cols=1,
        shared_xaxes=True,
        vertical_spacing=0.06,
        row_heights=[0.65, 0.35],
        subplot_titles=("Mid Price and Bot Trades", "Equity Curve / PnL"),
    )

    figure.add_trace(
        go.Scattergl(
            x=frame["timestamp"],
            y=frame["mid_price"],
            mode="lines",
            name="Mid Price",
            line={"color": "#2563eb", "width": 1},
        ),
        row=1,
        col=1,
    )

    figure.add_trace(
        go.Scattergl(
            x=bot_buys["timestamp"],
            y=bot_buys["mid_price"],
            mode="markers",
            name="Bot Buy",
            marker={"color": "#16a34a", "size": 7, "symbol": "triangle-up"},
        ),
        row=1,
        col=1,
    )

    figure.add_trace(
        go.Scattergl(
            x=bot_sells["timestamp"],
            y=bot_sells["mid_price"],
            mode="markers",
            name="Bot Sell",
            marker={"color": "#dc2626", "size": 7, "symbol": "triangle-down"},
        ),
        row=1,
        col=1,
    )

    figure.add_trace(
        go.Scattergl(
            x=frame["timestamp"],
            y=frame["pnl"],
            mode="lines",
            name="PnL",
            line={"color": "#111827", "width": 1.5},
        ),
        row=2,
        col=1,
    )

    figure.update_layout(
        title="Backtest Trace",
        template="plotly_white",
        hovermode="x unified",
        height=850,
        legend={"orientation": "h", "yanchor": "bottom", "y": 1.02, "xanchor": "right", "x": 1.0},
    )
    figure.update_xaxes(title_text="Virtual Timestamp (ns)", row=2, col=1)
    figure.update_yaxes(title_text="Mid Price", row=1, col=1)
    figure.update_yaxes(title_text="PnL", row=2, col=1)

    figure.write_html(output_path, include_plotlyjs="cdn")
    print(f"Wrote interactive trace chart: {output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
