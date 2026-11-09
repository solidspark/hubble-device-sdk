#!/usr/bin/env python3
# Copyright (c) 2026 Hubble Network, Inc.
# SPDX-License-Identifier: Apache-2.0
"""Capture DRIFT and TX lines from a CONFIG_DIAG_CLOCK_DRIFT build.

Writes drift.csv, tx.csv, hop.csv and the full console log to the output directory.
Each row gets the host's wall-clock and monotonic time, so device rows can
be joined to receiver logs by time.

    python3 drift_capture.py /dev/cu.usbmodem101 --out run1 --duration 1800
"""

import argparse
import csv
import os
import sys
import time

import serial

DRIFT_COLS = ["frames", "ticks", "ppm", "die_temp_c", "valid"]
TX_COLS = ["seq", "die_temp_c"]
HOP_COLS = ["seq", "hop", "channel", "mhz", "symbols", "offsets"]


def open_port(port, baud, wait_s, reset):
    deadline = time.monotonic() + wait_s
    while True:
        try:
            ser = serial.Serial()
            ser.port = port
            ser.baudrate = baud
            ser.timeout = 0.5
            # The C6 USB Serial/JTAG resets the chip on RTS high, DTR low.
            ser.dtr = False
            ser.rts = False
            ser.open()
            break
        except (serial.SerialException, OSError):
            if time.monotonic() > deadline:
                raise
            time.sleep(0.1)
    if reset:
        ser.rts = True
        time.sleep(0.1)
        ser.rts = False
    return ser


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("port")
    ap.add_argument("--baud", type=int, default=115200,
                    help="ignored by USB Serial/JTAG, kept for UART boards")
    ap.add_argument("--out", default="drift-capture", help="output directory")
    ap.add_argument("--duration", type=float, default=600,
                    help="seconds to capture (0 = until Ctrl-C)")
    ap.add_argument("--wait", type=float, default=60,
                    help="seconds to wait for the port to appear")
    ap.add_argument("--reset", action="store_true",
                    help="reset the chip after opening, to capture from boot")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    ser = open_port(args.port, args.baud, args.wait, args.reset)

    with open(os.path.join(args.out, "drift.csv"), "w", newline="") as fd, \
         open(os.path.join(args.out, "tx.csv"), "w", newline="") as ft, \
         open(os.path.join(args.out, "hop.csv"), "w", newline="") as fh, \
         open(os.path.join(args.out, "console.log"), "w", buffering=1) as fl:
        drift = csv.writer(fd)
        tx = csv.writer(ft)
        hop = csv.writer(fh)
        drift.writerow(["host_unix", "host_mono", "ms_since_boot"] + DRIFT_COLS)
        tx.writerow(["host_unix", "host_mono", "ms_since_boot"] + TX_COLS)
        hop.writerow(["host_unix", "host_mono", "ms_since_boot"] + HOP_COLS)

        start = time.monotonic()
        counts = {"DRIFT": 0, "TX": 0, "HOP": 0}
        buf = b""
        try:
            while args.duration <= 0 or time.monotonic() - start < args.duration:
                try:
                    chunk = ser.read(ser.in_waiting or 1)
                except serial.SerialException as e:
                    print(f"port lost: {e}", file=sys.stderr)
                    break
                if not chunk:
                    continue
                buf += chunk
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    line = raw.decode("utf-8", "replace").strip()
                    now_unix, now_mono = time.time(), time.monotonic()
                    fl.write(f"{now_unix:.3f} {line}\n")
                    fields = line.split(",")
                    if fields[0] == "DRIFT" and len(fields) == 7:
                        drift.writerow([f"{now_unix:.3f}", f"{now_mono:.3f}"]
                                       + fields[1:])
                        fd.flush()
                    elif fields[0] == "TX" and len(fields) == 4:
                        tx.writerow([f"{now_unix:.3f}", f"{now_mono:.3f}"]
                                    + fields[1:])
                        ft.flush()
                    elif fields[0] == "HOP" and len(fields) == 8:
                        hop.writerow([f"{now_unix:.3f}", f"{now_mono:.3f}"]
                                     + fields[1:])
                        fh.flush()
                    else:
                        continue
                    counts[fields[0]] += 1
                    print(line, flush=True)
        except KeyboardInterrupt:
            pass

    print(f"{counts['DRIFT']} DRIFT, {counts['TX']} TX and {counts['HOP']} HOP "
          f"lines in {args.out}",
          file=sys.stderr)


if __name__ == "__main__":
    main()
