#!/usr/bin/env python3
"""Generate res/rain.ico (multi-size, 32-bit RGBA) for rain.exe.

usage:  python tools/make_icon.py [out.ico]
No third-party modules: the drawing is plain math with 4x4 supersampling,
the ICO is assembled by hand (BMP entries for small sizes, PNG for 256).
"""
import math
import os
import struct
import sys
import zlib

SIZES = (16, 20, 24, 32, 48, 64, 128, 256)
SS = 4                                    # supersampling factor per axis

BG_TOP = (46, 116, 158)                   # rain-blue gradient, top
BG_BOT = (13, 52, 78)                     # ... and bottom
BORDER = (9, 38, 58)
DROP = (232, 247, 255)
SHINE = (255, 255, 255)

R_SQUARE = 0.16                           # corner radius, unit square
INSET_BORDER = 0.028


def inside_round_rect(u, v, r, inset=0.0):
    if u < inset or u > 1.0 - inset or v < inset or v > 1.0 - inset:
        return False
    qx = max(inset + r - u, 0.0, u - (1.0 - inset - r))
    qy = max(inset + r - v, 0.0, v - (1.0 - inset - r))
    return qx * qx + qy * qy <= r * r


def point_in_triangle(p, a, b, c):
    def cross(o, x, y):
        return (x[0] - o[0]) * (y[1] - o[1]) - (x[1] - o[1]) * (y[0] - o[0])
    d1 = cross(a, b, p)
    d2 = cross(b, c, p)
    d3 = cross(c, a, p)
    return (d1 >= 0 and d2 >= 0 and d3 >= 0) or (d1 <= 0 and d2 <= 0 and d3 <= 0)


def drop_outline():
    """Circle plus the tangent triangle from an apex: a teardrop."""
    cx, cy, r = 0.5, 0.615, 0.205
    apex = (0.5, 0.200)
    dx, dy = apex[0] - cx, apex[1] - cy
    d = math.hypot(dx, dy)
    theta = math.acos(r / d)
    ang = math.atan2(dy, dx)
    t1 = (cx + r * math.cos(ang + theta), cy + r * math.sin(ang + theta))
    t2 = (cx + r * math.cos(ang - theta), cy + r * math.sin(ang - theta))
    return (cx, cy, r), apex, t1, t2


CIRCLE, APEX, T1, T2 = drop_outline()
SHINE_C = (0.428, 0.520)                  # tilted oval highlight, upper-left of the drop
SHINE_RX, SHINE_RY, SHINE_ANG = 0.038, 0.072, -0.55


def in_shine(u, v):
    dx, dy = u - SHINE_C[0], v - SHINE_C[1]
    ca, sa = math.cos(SHINE_ANG), math.sin(SHINE_ANG)
    xr = dx * ca + dy * sa
    yr = -dx * sa + dy * ca
    return (xr / SHINE_RX) ** 2 + (yr / SHINE_RY) ** 2 <= 1.0


def color_at(u, v):
    if not inside_round_rect(u, v, R_SQUARE):
        return None                       # transparent outside the badge
    if not inside_round_rect(u, v, R_SQUARE, INSET_BORDER):
        return BORDER
    cx, cy, r = CIRCLE
    in_drop = (u - cx) ** 2 + (v - cy) ** 2 <= r * r or point_in_triangle((u, v), APEX, T1, T2)
    if in_drop:
        if in_shine(u, v):
            return SHINE
        return DROP
    t = min(max(v, 0.0), 1.0)
    return tuple(int(BG_TOP[i] + (BG_BOT[i] - BG_TOP[i]) * t) for i in range(3))


def render(size):
    rows = []
    n = size * SS
    for py in range(size):
        row = []
        for px in range(size):
            sr = sg = sb = 0
            hit = 0
            for sy in range(SS):
                v = (py * SS + sy + 0.5) / n
                for sx in range(SS):
                    u = (px * SS + sx + 0.5) / n
                    c = color_at(u, v)
                    if c is not None:
                        sr += c[0]
                        sg += c[1]
                        sb += c[2]
                        hit += 1
            if hit:
                row.append((sr // hit, sg // hit, sb // hit, (255 * hit) // (SS * SS)))
            else:
                row.append((0, 0, 0, 0))
        rows.append(row)
    return rows


def encode_png(rows):
    size = len(rows)
    raw = b"".join(b"\x00" + bytes(ch for px in row for ch in px) for row in rows)

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) +
            chunk(b"IEND", b""))


def encode_bmp(rows):
    size = len(rows)
    hdr = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, size * size * 4, 0, 0, 0, 0)
    xor = bytearray()
    for y in range(size - 1, -1, -1):
        for (r, g, b, a) in rows[y]:
            xor += bytes((b, g, r, a))
    stride = ((size + 31) // 32) * 4
    return hdr + bytes(xor) + bytes(stride) * size       # AND mask all zero: alpha channel rules


def build_ico(path):
    images = []
    for s in SIZES:
        rows = render(s)
        data = encode_png(rows) if s >= 256 else encode_bmp(rows)
        images.append((s, data))
    offset = 6 + 16 * len(images)
    header = struct.pack("<HHH", 0, 1, len(images))
    entries = b""
    blob = b""
    for s, data in images:
        entries += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
        blob += data
    with open(path, "wb") as f:
        f.write(header + entries + blob)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "res", "rain.ico")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    build_ico(out)
    print("wrote %s (%d bytes)" % (out, os.path.getsize(out)))


if __name__ == "__main__":
    main()
