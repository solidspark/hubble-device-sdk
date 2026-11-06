#!/usr/bin/env python3
# Copyright (c) 2026 Hubble Network, Inc.
# SPDX-License-Identifier: Apache-2.0
"""Plot crystal offset, die temperature and TX events from drift_capture.py.

    python3 plot_drift.py run1
    python3 plot_drift.py run1 --rx receiver.csv --out run1/drift.png

The receiver CSV needs rs_corrections and decoded (0|1) columns, plus seq
or timestamp. With seq, rows join to TX lines by sequence number. With
timestamp (unix seconds or ISO 8601), each row joins to the nearest TX by
the host's wall clock at capture time.
"""

import argparse
import os
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import pandas as pd  # noqa: E402


def load_capture(path):
    drift = pd.read_csv(os.path.join(path, "drift.csv"))
    tx = pd.read_csv(os.path.join(path, "tx.csv"))
    for df in (drift, tx):
        df["t_s"] = df["ms_since_boot"] / 1000.0
    drift["nosof"] = drift["frames"].astype(str) == "nosof"
    for col in ("frames", "ticks", "ppm", "die_temp_c", "valid"):
        drift[col] = pd.to_numeric(drift[col], errors="coerce")
    drift["valid"] = drift["valid"].fillna(0).astype(int)
    return drift, tx


def to_unix(series):
    num = pd.to_numeric(series, errors="coerce")
    if num.notna().all():
        return num.astype(float)
    return pd.to_datetime(series, utc=True).astype("int64") / 1e9


def load_rx(path, tx, tolerance_s):
    rx = pd.read_csv(path)
    missing = {"rs_corrections", "decoded"} - set(rx.columns)
    if missing:
        sys.exit(f"{path}: missing columns {sorted(missing)}")

    if "seq" in rx.columns:
        rx = rx.merge(tx[["seq", "t_s"]], on="seq", how="left")
    elif "timestamp" in rx.columns:
        rx["host_unix"] = to_unix(rx["timestamp"])
        rx = pd.merge_asof(rx.sort_values("host_unix"),
                           tx[["host_unix", "t_s"]].sort_values("host_unix"),
                           on="host_unix", direction="nearest",
                           tolerance=tolerance_s)
    else:
        sys.exit(f"{path}: needs a seq or timestamp column")

    unmatched = rx["t_s"].isna().sum()
    if unmatched:
        print(f"{unmatched} receiver rows did not match a TX line",
              file=sys.stderr)
    return rx.dropna(subset=["t_s"])


def summarize(drift, tx):
    ok = drift[drift["valid"] == 1]
    print(f"windows: {len(drift)} ({len(ok)} valid, "
          f"{int(drift['nosof'].sum())} nosof), TX: {len(tx)}")
    if len(ok):
        print(f"ppm: first {ok['ppm'].iloc[0]:+.2f}, last {ok['ppm'].iloc[-1]:+.2f}, "
              f"min {ok['ppm'].min():+.2f}, max {ok['ppm'].max():+.2f}")
    temp = drift["die_temp_c"].dropna()
    if len(temp):
        print(f"die temp: {temp.iloc[0]:.1f} -> {temp.iloc[-1]:.1f} C")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("capture", help="directory written by drift_capture.py")
    ap.add_argument("--rx", help="receiver log CSV")
    ap.add_argument("--rx-tolerance", type=float, default=10.0,
                    help="max seconds between a receiver row and its TX")
    ap.add_argument("--out", help="output image (default <capture>/drift.png)")
    args = ap.parse_args()

    drift, tx = load_capture(args.capture)
    summarize(drift, tx)
    rx = load_rx(args.rx, tx, args.rx_tolerance) if args.rx else None

    rows = 3 if rx is not None else 2
    fig, axes = plt.subplots(rows, 1, sharex=True, figsize=(11, 3 * rows + 1))

    ok = drift[drift["valid"] == 1]
    bad = drift[(drift["valid"] == 0) & ~drift["nosof"]]
    ax = axes[0]
    ax.plot(ok["t_s"], ok["ppm"], ".-", color="tab:blue", label="valid window")
    if len(bad):
        y = ok["ppm"].min() if len(ok) else 0
        ax.plot(bad["t_s"], [y] * len(bad), "x", color="tab:red",
                label="invalid window")
    nosof = drift[drift["nosof"]]
    if len(nosof):
        ax.plot(nosof["t_s"], [ok["ppm"].min() if len(ok) else 0] * len(nosof),
                "|", color="grey", label="no USB SOF")
    ax.set_ylabel("crystal vs host (ppm)")
    ax.legend(loc="best", fontsize="small")

    ax = axes[1]
    ax.plot(drift["t_s"], drift["die_temp_c"], "-", color="tab:orange")
    ax.set_ylabel("die temp (C)")

    if rx is not None:
        ax = axes[2]
        good = rx[rx["decoded"] == 1]
        fail = rx[rx["decoded"] == 0]
        ax.stem(good["t_s"], good["rs_corrections"], basefmt=" ",
                label="RS corrections")
        if len(fail):
            ax.plot(fail["t_s"], [0] * len(fail), "v", color="tab:red",
                    markersize=8, label="failed decode")
        ax.set_ylabel("RS corrections")
        ax.legend(loc="best", fontsize="small")

    for ax in axes:
        for t in tx["t_s"]:
            ax.axvline(t, color="tab:green", alpha=0.25, linewidth=0.8)
        ax.grid(True, alpha=0.3)
    axes[0].set_title("Clock drift vs USB SOF (green lines: satellite TX)")
    axes[-1].set_xlabel("time since boot (s)")

    out = args.out or os.path.join(args.capture, "drift.png")
    fig.tight_layout()
    fig.savefig(out, dpi=120)
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
