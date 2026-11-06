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

With hop.csv, it also checks each frame against the SatNet v1 plan: the
synthesizer step (from the preamble's 63 and 0 symbols), that every symbol in
a hop sits on one bin-0 grid, the bin-0 error against the spec channel, the
on-air error with the measured ppm, and that channels follow one of the four
hopping sequences. Results go to hop_check.csv.
"""

import argparse
import os
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import pandas as pd  # noqa: E402

# SatNet v1: channel 0 center, channel spacing, and the four hop sequences.
SPEC_CH0_HZ = 2482208625.0
SPEC_SPACING_HZ = 25750.0
HOP_SEQUENCES = [
    [3, 14, 5, 6, 9, 2, 12, 8, 15, 4, 11, 13, 17, 10, 1, 7, 0, 18, 16],
    [10, 3, 15, 5, 0, 17, 13, 6, 11, 4, 8, 18, 9, 14, 1, 12, 7, 16, 2],
    [14, 5, 11, 3, 8, 2, 18, 4, 10, 13, 9, 1, 16, 17, 0, 6, 15, 12, 7],
    [7, 0, 11, 18, 4, 2, 13, 5, 10, 17, 3, 9, 16, 14, 8, 12, 1, 6, 15],
]
PREAMBLE = [63, 0, 63, 0, 63, 0, 63, 63]
# ESP32-C6 phy_set_freq offsets are in 100 Hz units (the SDK's 400 Hz step
# is 4 of them).
PHY_UNIT_HZ = 100.0


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


def load_hops(path):
    f = os.path.join(path, "hop.csv")
    if not os.path.exists(f):
        return None
    hops = pd.read_csv(f)
    if hops.empty:
        return None
    hops["t_s"] = hops["ms_since_boot"] / 1000.0
    hops["sym"] = hops["symbols"].astype(str).str.split().apply(
        lambda v: [int(x) for x in v])
    hops["off"] = hops["offsets"].astype(str).str.split().apply(
        lambda v: [int(x) for x in v])
    return hops.sort_values(["seq", "hop"]).reset_index(drop=True)


def _step_units(hops):
    steps = []
    for _, h in hops[hops["hop"] == 0].iterrows():
        pre = list(zip(h["sym"][:len(PREAMBLE)], h["off"][:len(PREAMBLE)]))
        if [s for s, _ in pre] != PREAMBLE:
            continue
        hi = [o for s, o in pre if s == 63]
        lo = [o for s, o in pre if s == 0]
        if len(set(hi)) == 1 and len(set(lo)) == 1:
            steps.append((hi[0] - lo[0]) / 63.0)
    return pd.Series(steps).median() if steps else None


def _sequence_check(channels):
    best = None
    for i, seq in enumerate(HOP_SEQUENCES):
        nxt = {seq[k]: seq[(k + 1) % len(seq)] for k in range(len(seq))}
        breaks = sum(1 for a, b in zip(channels, channels[1:]) if nxt[a] != b)
        if best is None or breaks < best[1]:
            best = (i + 1, breaks)
    return best


def check_hops(hops, drift, out_csv):
    step_units = _step_units(hops)
    if step_units is None:
        print("hops: no clean preamble, cannot measure the step")
        return None
    step_hz = step_units * PHY_UNIT_HZ

    rows = []
    for _, h in hops.iterrows():
        bins = {o - step_units * s for s, o in zip(h["sym"], h["off"])}
        bin0_units = min(bins)
        bin0_hz = h["mhz"] * 1e6 + bin0_units * PHY_UNIT_HZ
        spec_bin0 = (SPEC_CH0_HZ + h["channel"] * SPEC_SPACING_HZ -
                     31.5 * step_hz)
        implied = round((bin0_hz + 31.5 * step_hz - SPEC_CH0_HZ) /
                        SPEC_SPACING_HZ)
        rows.append({
            "t_s": h["t_s"], "seq": h["seq"], "hop": h["hop"],
            "channel": h["channel"], "nsym": len(h["sym"]),
            "on_grid": len(bins) == 1, "implied_channel": implied,
            "bin0_hz": bin0_hz, "spec_bin0_hz": spec_bin0,
            "plan_err_hz": bin0_hz - spec_bin0,
        })
    res = pd.DataFrame(rows)

    ok = drift[drift["valid"] == 1][["t_s", "ppm"]].sort_values("t_s")
    if len(ok):
        res = pd.merge_asof(res.sort_values("t_s"), ok, on="t_s",
                            direction="nearest")
        res["onair_err_hz"] = (res["bin0_hz"] * (1 + res["ppm"] * 1e-6) -
                               res["spec_bin0_hz"])
    res = res.sort_values(["seq", "hop"]).reset_index(drop=True)
    res.to_csv(out_csv, index=False)

    seq_id, breaks = _sequence_check(list(res["channel"]))
    shapes = res.groupby("seq")["nsym"].apply(
        lambda v: "+".join(str(n) for n in v)).value_counts()
    print(f"hops: {len(res)} in {res['seq'].nunique()} packets, "
          f"step {step_hz:.1f} Hz")
    print(f"  symbols per packet: " +
          ", ".join(f"{k} (x{v})" for k, v in shapes.items()))
    print(f"  off the bin grid: {int((~res['on_grid']).sum())}, "
          f"channel mismatch: "
          f"{int((res['implied_channel'] != res['channel']).sum())}")
    print(f"  hopping: sequence {seq_id}, {breaks} breaks over "
          f"{len(res) - 1} hops (first channel {res['channel'].iloc[0]})")
    per_ch = res.groupby("channel")["plan_err_hz"].first()
    print("  bin-0 error vs spec by channel (Hz): " + ", ".join(
        f"{c}:{e:+.0f}" for c, e in per_ch.items()))
    if "onair_err_hz" in res:
        print(f"  on-air bin-0 error with measured ppm: "
              f"{res['onair_err_hz'].min():+.0f} to "
              f"{res['onair_err_hz'].max():+.0f} Hz "
              f"({res['onair_err_hz'].min() / step_hz:+.1f} to "
              f"{res['onair_err_hz'].max() / step_hz:+.1f} steps)")
    res.attrs["step_hz"] = step_hz
    return res


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
    hops = load_hops(args.capture)
    hopres = (check_hops(hops, drift,
                         os.path.join(args.capture, "hop_check.csv"))
              if hops is not None else None)

    rows = 2 + (rx is not None) + (hopres is not None)
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

    if hopres is not None:
        ax = axes[-1]
        step_hz = hopres.attrs["step_hz"]
        ax.plot(hopres["t_s"], hopres["plan_err_hz"], "o", color="tab:grey",
                markersize=4, label="configured (channel grid)")
        if "onair_err_hz" in hopres:
            ax.plot(hopres["t_s"], hopres["onair_err_hz"], ".",
                    color="tab:purple", label="on air, with measured ppm")
        ax.set_ylabel("bin-0 vs spec (Hz)")
        sec = ax.secondary_yaxis("right", functions=(lambda hz: hz / step_hz,
                                                     lambda st: st * step_hz))
        sec.set_ylabel(f"steps of {step_hz:.0f} Hz")
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
