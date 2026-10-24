#!/usr/bin/env python3
# Copyright (c) 2026 Hubble Network, Inc.
# SPDX-License-Identifier: Apache-2.0
"""Rasterize the Hubble logomark SVG path into an LVGL A8 image (main/logo.c).

Pure Python so it runs without extra packages:
    python3 tools/gen_logo.py --width 24 > main/logo.c
"""

import argparse
import re

# hubble-dashboard/src/assets/images/h-hubble-logo.svg, viewBox 0 0 38 32
VIEW_W, VIEW_H = 38.0, 32.0
PATH = (
    "M5.22879 19.9533C5.19678 19.8184 5.16281 19.6847 5.13369 19.5479C5.08607 19.3242 5.05083 19.1432 5.03493 18.9958C5.09861 19.3502 5.16486 19.6706 5.22879 19.9533Z"
    "M5.22879 19.9533C5.2552 20.0699 5.2813 20.1818 5.30651 20.2843C5.27876 20.1752 5.25498 20.0635 5.22879 19.9533Z"
    "M10.096 13.0639C10.1404 12.9583 10.1759 12.849 10.2018 12.7373C10.3257 12.2279 10.48 11.7266 10.6639 11.2359C10.4156 11.825 10.2254 12.4373 10.096 13.0639Z"
    "M31.4125 9.50437e-05C30.3262 1.10944 29.247 2.21172 28.1589 3.32103C30.201 5.99237 31.6223 8.37619 32.3453 10.3801C32.377 10.4511 32.4088 10.5239 32.4388 10.6002C34.3803 15.3554 32.4388 21.2481 32.4388 21.2481L32.4212 21.2464C31.7527 23.4653 30.6614 25.532 29.2082 27.331C28.8238 27.8067 28.4287 28.2735 28.1625 28.5931C29.2946 29.7646 30.3685 30.8775 31.4424 31.9886C40.9562 22.6647 39.3726 7.05208 31.4125 9.50437e-05Z"
    "M5.57293 21.2483H5.57116C5.57116 21.2483 5.49887 21.0247 5.39659 20.636C5.38601 20.6005 5.37705 20.5667 5.36839 20.533C5.35972 20.4993 5.35249 20.4655 5.34194 20.4301C5.3296 20.3821 5.31709 20.334 5.30651 20.2843C5.24479 20.0411 5.18661 19.7964 5.13369 19.5479C5.08607 19.3242 5.05083 19.1432 5.03493 18.9958C4.65061 16.8446 4.38438 13.5112 5.57116 10.6001C5.57293 10.6001 5.57293 10.6001 5.57293 10.5983C6.26947 8.55179 7.71728 6.0899 9.83511 3.32093C8.74712 2.21158 7.6679 1.10931 6.58159 0C-1.37854 7.05199 -2.9621 22.6646 6.55163 31.9886C7.62556 30.8774 8.6995 29.7645 9.82987 28.593C9.56531 28.2736 9.1703 27.8068 8.7859 27.3309C7.33305 25.5323 6.24177 23.4664 5.57293 21.2483Z"
    "M27.9298 13.1296C27.8687 13.0048 27.8225 12.8731 27.7922 12.7373C27.2573 10.5211 26.1388 8.49094 24.5545 6.86047C23.3977 8.0142 22.3026 9.10759 21.2004 10.208C22.2091 11.4255 22.9248 12.8608 23.2918 14.4024H14.7004C15.0681 12.8607 15.7844 11.4254 16.7936 10.208C15.6915 9.10759 14.5963 8.0142 13.4396 6.86047C12.2283 8.1103 11.2848 9.59704 10.6674 11.2286L10.6639 11.2359C10.48 11.7266 10.3257 12.2279 10.2018 12.7373C10.1759 12.849 10.1404 12.9583 10.096 13.0639C9.5948 15.3657 9.70741 17.7601 10.4222 20.004L10.4275 20.02C10.4769 20.1762 10.5299 20.3324 10.5862 20.4904C10.5898 20.5028 10.595 20.5133 10.5986 20.524C10.6568 20.6839 10.7168 20.8453 10.782 21.0069C11.0782 21.7522 11.4467 22.4662 11.8824 23.1386C12.313 23.8033 12.8151 24.4182 13.3795 24.9722C14.5258 23.8592 15.6314 22.7855 16.7724 21.6778C15.9235 20.6493 15.2772 19.4674 14.8678 18.1953H23.1261C22.7161 19.4674 22.0691 20.6494 21.2197 21.6778C22.3607 22.7854 23.4682 23.8592 24.6144 24.9722C26.2043 23.3633 27.3148 21.3386 27.8203 19.1272C27.8368 19.0562 27.8575 18.9862 27.8822 18.9178C28.3193 17.0152 28.3356 15.0391 27.9298 13.1296Z"
)

TOKEN = re.compile(r"[MCLHZ]|-?\d*\.?\d+(?:e-?\d+)?")
CURVE_STEPS = 16
SUPERSAMPLE = 8


def polygons(d):
    """Flatten the path (absolute M/C/L/H/Z only) into closed polygons."""
    tokens = TOKEN.findall(d)
    i, cmd, cur, polys, poly = 0, None, (0.0, 0.0), [], []

    def num():
        nonlocal i
        i += 1
        return float(tokens[i - 1])

    while i < len(tokens):
        if tokens[i] in "MCLHZ":
            cmd = tokens[i]
            i += 1
        if cmd == "M":
            if poly:
                polys.append(poly)
            cur = (num(), num())
            poly = [cur]
        elif cmd == "L":
            cur = (num(), num())
            poly.append(cur)
        elif cmd == "H":
            cur = (num(), cur[1])
            poly.append(cur)
        elif cmd == "C":
            p1, p2, p3 = (num(), num()), (num(), num()), (num(), num())
            p0 = cur
            for s in range(1, CURVE_STEPS + 1):
                t = s / CURVE_STEPS
                u = 1 - t
                poly.append(tuple(
                    u**3 * p0[k] + 3 * u * u * t * p1[k] + 3 * u * t * t * p2[k] + t**3 * p3[k]
                    for k in (0, 1)))
            cur = p3
        elif cmd == "Z":
            if poly:
                polys.append(poly)
            poly = []
    if poly:
        polys.append(poly)
    return polys


def rasterize(polys, width):
    scale = width / VIEW_W
    height = round(VIEW_H * scale)
    sw, sh = width * SUPERSAMPLE, height * SUPERSAMPLE
    ss = scale * SUPERSAMPLE
    edges = []
    for poly in polys:
        pts = [(x * ss, y * ss) for x, y in poly]
        for a, b in zip(pts, pts[1:] + pts[:1]):
            if a[1] != b[1]:
                edges.append((a, b))

    coverage = [[0] * width for _ in range(height)]
    for row in range(sh):
        y = row + 0.5
        crossings = []
        for (x0, y0), (x1, y1) in edges:
            if (y0 <= y < y1) or (y1 <= y < y0):
                x = x0 + (y - y0) * (x1 - x0) / (y1 - y0)
                crossings.append((x, 1 if y1 > y0 else -1))
        crossings.sort()
        winding = 0
        for (xa, w), (xb, _) in zip(crossings, crossings[1:]):
            winding += w
            if winding == 0:
                continue
            for col in range(max(0, int(xa + 0.5)), min(sw, int(xb + 0.5))):
                coverage[row // SUPERSAMPLE][col // SUPERSAMPLE] += 1
    full = SUPERSAMPLE * SUPERSAMPLE
    return height, [[min(255, c * 255 // full) for c in r] for r in coverage]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--width", type=int, default=24)
    args = parser.parse_args()
    height, rows = rasterize(polygons(PATH), args.width)

    print("/*\n * Copyright (c) 2026 Hubble Network, Inc.\n *\n"
          " * SPDX-License-Identifier: Apache-2.0\n *\n"
          " * Generated by tools/gen_logo.py. Do not edit.\n */\n")
    print('#include "lvgl.h"\n')
    print(f"static const uint8_t _logo_map[{args.width * height}] = {{")
    for r in rows:
        print("\t" + ", ".join(f"0x{v:02x}" for v in r) + ",")
    print("};\n")
    print("const lv_image_dsc_t hubble_logo = {")
    print("\t.header = {")
    print("\t\t.magic = LV_IMAGE_HEADER_MAGIC,")
    print("\t\t.cf = LV_COLOR_FORMAT_A8,")
    print(f"\t\t.w = {args.width},")
    print(f"\t\t.h = {height},")
    print(f"\t\t.stride = {args.width},")
    print("\t},")
    print("\t.data_size = sizeof(_logo_map),")
    print("\t.data = _logo_map,")
    print("};")


if __name__ == "__main__":
    main()
