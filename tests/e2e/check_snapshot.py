#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Checks a `rayomd-gui --snapshot` PNG: the theme's background and accent are where the main
window draws them, and the preview shows a page with text on it.

Usage: check_snapshot.py SNAPSHOT.png BACKGROUND ACCENT [--scale S] [--timings FILE]

BACKGROUND and ACCENT are hex colours (rrggbb). --scale is the device pixels per logical pixel
the snapshot was taken at (default 1). --timings names a file holding the app's stdout, whose
`snapshot:` line is printed with the result.
"""

import argparse
import pathlib
import struct
import sys
import zlib


def read_png(path):
    data = pathlib.Path(path).read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos = 8
    width = height = None
    colour = None
    idat = bytearray()
    while pos + 8 <= len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height, depth, colour, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or colour not in (2, 6) or interlace:
                raise ValueError(f"unsupported PNG: depth {depth}, colour type {colour}, interlace {interlace}")
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
        pos += 12 + length
    channels = 3 if colour == 2 else 4
    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    rows = []
    previous = bytearray(stride)
    offset = 0
    for _ in range(height):
        kind = raw[offset]
        line = bytearray(raw[offset + 1:offset + 1 + stride])
        offset += 1 + stride
        for i in range(stride):
            left = line[i - channels] if i >= channels else 0
            up = previous[i]
            corner = previous[i - channels] if i >= channels else 0
            if kind == 1:
                line[i] = (line[i] + left) & 0xFF
            elif kind == 2:
                line[i] = (line[i] + up) & 0xFF
            elif kind == 3:
                line[i] = (line[i] + (left + up) // 2) & 0xFF
            elif kind == 4:
                p = left + up - corner
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - corner)
                predictor = left if pa <= pb and pa <= pc else (up if pb <= pc else corner)
                line[i] = (line[i] + predictor) & 0xFF
        rows.append([tuple(line[x * channels:x * channels + 3]) for x in range(width)])
        previous = line
    return width, height, rows


def parse_hex(text):
    text = text.lstrip("#")
    return tuple(int(text[i:i + 2], 16) for i in (0, 2, 4))


def luminance(p):
    return (299 * p[0] + 587 * p[1] + 114 * p[2]) // 1000


def is_accent(p, bg, accent):
    """Along the background-to-accent line at least halfway, and within 20 of that line."""
    d = [accent[i] - bg[i] for i in range(3)]
    v = [p[i] - bg[i] for i in range(3)]
    dd = sum(x * x for x in d)
    if dd == 0:
        return False
    t = sum(v[i] * d[i] for i in range(3)) / dd
    off = [v[i] - t * d[i] for i in range(3)]
    return t >= 0.5 and sum(x * x for x in off) ** 0.5 <= 20


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("snapshot")
    parser.add_argument("background")
    parser.add_argument("accent")
    parser.add_argument("--scale", type=float, default=1.0)
    parser.add_argument("--timings")
    args = parser.parse_args()

    bg = parse_hex(args.background)
    accent = parse_hex(args.accent)
    width, height, rows = read_png(args.snapshot)
    scale = args.scale
    failures = []

    if width < 720 or height < 520:
        failures.append(f"size {width}x{height} is below 720x520")

    probe = rows[height // 2][int(8 * scale)]
    if probe != bg:
        failures.append(f"pixel at ({int(8 * scale)}, {height // 2}) is {probe}, not the background {bg}")

    half = width // 2
    pane = width - half
    page_row = None
    for y in range(height):
        whites = [x for x in range(half, width) if rows[y][x] == (255, 255, 255)]
        if len(whites) > 0.3 * pane:
            page_row = (y, whites[0], whites[-1])
            break
    if page_row is None:
        failures.append("no row in the right half is more than 30% white: no page")
    else:
        _, first, last = page_row
        dark = sum(1 for y in range(height) for x in range(first, last + 1) if luminance(rows[y][x]) < 100)
        if dark <= 500:
            failures.append(f"only {dark} dark pixels inside the page's columns {first}-{last}: no text")

    footer = int(32 * scale)
    left = int(220 * scale)
    accents = sum(1 for y in range(max(0, height - footer), height) for x in range(min(left, width))
                  if is_accent(rows[y][x], bg, accent))
    if accents <= 5:
        failures.append(f"only {accents} accent pixels in the footer's left {left} px")

    print(f"snapshot {args.snapshot}: {width}x{height}, page row {page_row}, footer accent pixels {accents}")
    if args.timings:
        for line in pathlib.Path(args.timings).read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("snapshot:"):
                print(line)
    for failure in failures:
        print(f"FAIL {failure}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
