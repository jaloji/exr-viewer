"""Writes uncompressed OpenEXR test files together with the expected 8-bit images.

The expected images follow the documented pipeline (premultiplied alpha, chromaticities to sRGB,
HDR detection on the 99.9th percentile of the brightest channel, highlight shoulder, sRGB curve),
computed independently of the C++ code and of the OpenEXR library.

Usage:
  python make_testdata.py <out_dir>            write the .exr files and the expected .png files
  python make_testdata.py <out_dir> --check    compare <name>.out.png (from exr2png) with <name>.png
"""
import os
import struct
import sys

import numpy as np
from PIL import Image

W, H = 64, 48
HALF, FLOAT = 1, 2


def attr(name, typ, value):
    return name.encode() + b"\0" + typ.encode() + b"\0" + struct.pack("<i", len(value)) + value


def chlist(channels):
    out = b""
    for name, ptype in sorted(channels):
        out += name.encode() + b"\0" + struct.pack("<iB3xii", ptype, 0, 1, 1)
    return out + b"\0"


def write_exr(path, planes, ptype=HALF, chroma=None, tiled=False, data=None):
    """planes: {channel name: float array}. Uncompressed scanlines or 16x16 tiles.
    data: (x0, y0, x1, y1) data window when it differs from the W x H display window."""
    names = sorted(planes)
    x0, y0, x1, y1 = data or (0, 0, W - 1, H - 1)
    dtype = np.float16 if ptype == HALF else np.float32
    hdr = b""
    hdr += attr("channels", "chlist", chlist([(n, ptype) for n in names]))
    hdr += attr("compression", "compression", b"\0")
    hdr += attr("dataWindow", "box2i", struct.pack("<4i", x0, y0, x1, y1))
    hdr += attr("displayWindow", "box2i", struct.pack("<4i", 0, 0, W - 1, H - 1))
    hdr += attr("lineOrder", "lineOrder", b"\0")
    hdr += attr("pixelAspectRatio", "float", struct.pack("<f", 1.0))
    hdr += attr("screenWindowCenter", "v2f", struct.pack("<2f", 0, 0))
    hdr += attr("screenWindowWidth", "float", struct.pack("<f", 1.0))
    if chroma:
        hdr += attr("chromaticities", "chromaticities", struct.pack("<8f", *[v for p in chroma for v in p]))
    if tiled:
        hdr += attr("tiles", "tiledesc", struct.pack("<IIB", 16, 16, 0))
    hdr += b"\0"
    version = 2 | (0x200 if tiled else 0)
    head = b"\x76\x2f\x31\x01" + struct.pack("<i", version) + hdr

    chunks = []
    if tiled:
        for ty in range(0, H, 16):
            for tx in range(0, W, 16):
                data = b"".join(planes[n][y, tx:tx + 16].astype(dtype).tobytes()
                                for y in range(ty, ty + 16) for n in names)
                chunks.append(struct.pack("<4ii", tx // 16, ty // 16, 0, 0, len(data)) + data)
    else:
        for y in range(y1 - y0 + 1):
            line = b"".join(planes[n][y].astype(dtype).tobytes() for n in names)
            chunks.append(struct.pack("<ii", y0 + y, len(line)) + line)
    offset = len(head) + 8 * len(chunks)
    table = b""
    for c in chunks:
        table += struct.pack("<Q", offset)
        offset += len(c)
    with open(path, "wb") as f:
        f.write(head + table + b"".join(chunks))


def write_deep(path, samples, alpha=True):
    """samples: H x W lists of (r, g, b, a, z) tuples, in file order. Uncompressed deep scanlines,
    R, G, B, A as half and Z as float."""
    names = (["A"] if alpha else []) + ["B", "G", "R", "Z"]
    types = {"A": HALF, "B": HALF, "G": HALF, "R": HALF, "Z": FLOAT}
    index = {"R": 0, "G": 1, "B": 2, "A": 3, "Z": 4}
    hdr = b""
    hdr += attr("channels", "chlist", chlist([(n, types[n]) for n in names]))
    hdr += attr("compression", "compression", b"\0")
    box = struct.pack("<4i", 0, 0, W - 1, H - 1)
    hdr += attr("dataWindow", "box2i", box)
    hdr += attr("displayWindow", "box2i", box)
    hdr += attr("lineOrder", "lineOrder", b"\0")
    hdr += attr("pixelAspectRatio", "float", struct.pack("<f", 1.0))
    hdr += attr("screenWindowCenter", "v2f", struct.pack("<2f", 0, 0))
    hdr += attr("screenWindowWidth", "float", struct.pack("<f", 1.0))
    hdr += attr("type", "string", b"deepscanline")
    hdr += attr("version", "int", struct.pack("<i", 1))
    hdr += attr("maxSamplesPerPixel", "int", struct.pack("<i", max(len(p) for row in samples for p in row)))
    hdr += b"\0"
    head = b"\x76\x2f\x31\x01" + struct.pack("<i", 2 | 0x800) + hdr
    chunks = []
    for y in range(H):
        table, total = b"", 0
        for x in range(W):
            total += len(samples[y][x])
            table += struct.pack("<i", total)
        data = b""
        for n in names:
            dtype = np.float16 if types[n] == HALF else np.float32
            values = [smp[index[n]] for x in range(W) for smp in samples[y][x]]
            data += np.asarray(values, dtype).tobytes()
        chunks.append(struct.pack("<iQQQ", y, len(table), len(data), len(data)) + table + data)
    offset = len(head) + 8 * len(chunks)
    offsets = b""
    for c in chunks:
        offsets += struct.pack("<Q", offset)
        offset += len(c)
    with open(path, "wb") as f:
        f.write(head + offsets + b"".join(chunks))


def flatten(samples, alpha=True):
    """Front-to-back compositing of premultiplied samples sorted by Z: flat premultiplied planes."""
    q = lambda v: float(np.float16(v))
    out = np.zeros((4, H, W))
    for y in range(H):
        for x in range(W):
            acc = [0.0, 0.0, 0.0, 0.0]
            for r, g, b, a, z in sorted(samples[y][x], key=lambda s: np.float32(s[4])):
                sa = min(max(q(a), 0), 1) if alpha else 1.0
                t = 1 - acc[3]
                acc = [acc[0] + t * q(r), acc[1] + t * q(g), acc[2] + t * q(b), acc[3] + t * sa]
            out[:, y, x] = acc
    return out


# ---- Reference pipeline --------------------------------------------------------------------

REC709 = [(0.64, 0.33), (0.30, 0.60), (0.15, 0.06), (0.3127, 0.3290)]
ACES_AP0 = [(0.7347, 0.2653), (0.0, 1.0), (0.0001, -0.0770), (0.32168, 0.33767)]
BRADFORD = np.array([[0.8951, 0.2664, -0.1614], [-0.7502, 1.7135, 0.0367], [0.0389, -0.0685, 1.0296]])


def xyz(p):
    return np.array([p[0] / p[1], 1.0, (1 - p[0] - p[1]) / p[1]])


def rgb_to_xyz(c):
    prim = np.array([xyz(p) for p in c[:3]]).T
    return prim * np.linalg.solve(prim, xyz(c[3]))


def to_srgb_matrix(c):
    adapt = np.linalg.inv(BRADFORD) @ np.diag((BRADFORD @ xyz(REC709[3])) / (BRADFORD @ xyz(c[3]))) @ BRADFORD
    return np.linalg.inv(rgb_to_xyz(REC709)) @ adapt @ rgb_to_xyz(c)


def srgb8(x):
    x = np.clip(x, 0, 1)
    s = np.where(x <= 0.0031308, 12.92 * x, 1.055 * np.power(x, 1 / 2.4) - 0.055)
    return np.round(s * 255).astype(np.uint8)


def expected(r, g, b, a, chroma=None, straight=False):
    q = lambda v: np.asarray(v, np.float16).astype(np.float64)  # stored as half
    r, g, b, a = q(r), q(g), q(b), np.clip(q(a), 0, 1)
    if not straight:
        semi = (a > 1e-4) & (a < 1)
        r, g, b = [np.where(semi, c / np.where(semi, a, 1), c) for c in (r, g, b)]
    if chroma:
        m = to_srgb_matrix(chroma)
        r, g, b = (m[i, 0] * r + m[i, 1] * g + m[i, 2] * b for i in range(3))
    r, g, b = [np.maximum(c, 0) for c in (r, g, b)]
    peak = np.maximum(np.maximum(r, g), b)
    opaque = not (a > 0).any() and (peak > 0).any()
    if opaque:
        a = np.ones_like(a)
    white = np.quantile(peak[a > 0], 0.999, method="inverted_cdf")
    if white > 1.1:
        knee, room = 0.5, 0.5
        x = np.maximum(peak - knee, 0)
        c = (white - knee) ** 2 / room
        md = np.minimum(np.where(peak > knee, knee + x * (1 + x / c) / (1 + x / room), peak), 1)
        s = np.where(peak > 0, md / np.where(peak > 0, peak, 1), 0)
        r, g, b = r * s, g * s, b * s
    return np.dstack([srgb8(r), srgb8(g), srgb8(b), np.round(a * 255).astype(np.uint8)])


# ---- Test cases -----------------------------------------------------------------------------

def cases():
    y, x = np.mgrid[0:H, 0:W].astype(np.float64)
    u, v = x / (W - 1), y / (H - 1)
    one = np.ones((H, W))
    sdr = (u, v, 1 - u, one)
    # HDR: a sky gradient up to ~3 with a small "sun" at 40 and a saturated red lamp.
    hr, hg, hb = 0.2 + 2.0 * v, 0.3 + 2.2 * v, 0.5 + 2.5 * v
    sun = (x - 48) ** 2 + (y - 10) ** 2 < 9
    lamp = (x - 12) ** 2 + (y - 36) ** 2 < 6
    hr, hg, hb = np.where(sun, 40.0, hr), np.where(sun, 38.0, hg), np.where(sun, 30.0, hb)
    hr, hg, hb = np.where(lamp, 12.0, hr), np.where(lamp, 0.5, hg), np.where(lamp, 0.2, hb)
    hdr = (hr, hg, hb, one)
    # Semi-transparent disc, premultiplied (colour = straight colour * alpha).
    alpha = np.clip(1.2 - np.hypot(x - 32, y - 24) / 20, 0, 1)
    sr, sg, sb = 0.9 * one, 0.6 * u, 0.2 + 0.5 * v

    out = []
    out.append(("sdr_rgb", {"R": sdr[0], "G": sdr[1], "B": sdr[2]}, {}, expected(*sdr)))
    out.append(("sdr_float", {"R": sdr[0], "G": sdr[1], "B": sdr[2]}, {"ptype": FLOAT}, None))
    out.append(("hdr_rgb", {"R": hr, "G": hg, "B": hb}, {}, expected(*hdr)))
    out.append(("hdr_aces", {"R": hr, "G": hg, "B": hb}, {"chroma": ACES_AP0}, expected(*hdr, chroma=ACES_AP0)))
    out.append(("alpha_premultiplied", {"R": sr * alpha, "G": sg * alpha, "B": sb * alpha, "A": alpha}, {},
                expected(sr * alpha, sg * alpha, sb * alpha, alpha)))
    out.append(("alpha_straight", {"R": sr, "G": sg, "B": sb, "A": alpha}, {},
                expected(sr, sg, sb, alpha, straight=True)))
    out.append(("alpha_zero", {"R": sdr[0], "G": sdr[1], "B": sdr[2], "A": 0 * one}, {}, expected(*sdr[:3], 0 * one)))
    out.append(("red_only", {"R": u * v}, {}, expected(u * v, u * v, u * v, one)))
    out.append(("luminance_y", {"Y": hg}, {}, expected(hg, hg, hg, one)))
    out.append(("layer_combined", {"ViewLayer.Combined.R": hr, "ViewLayer.Combined.G": hg, "ViewLayer.Combined.B": hb,
                                   "ViewLayer.Combined.A": one, "ViewLayer.Depth.Z": 5 * u}, {}, expected(*hdr)))
    out.append(("tiled", {"R": sdr[0], "G": sdr[1], "B": sdr[2]}, {"tiled": True}, expected(*sdr)))
    # Data window inside the display window (the rest is transparent) and one overflowing it (cropped).
    inner = (8, 6, 47, 35)
    ir, ig, ib = (c[6:36, 8:48] for c in sdr[:3])
    exp = np.zeros((H, W, 4), np.uint8)
    exp[6:36, 8:48] = expected(ir, ig, ib, one[6:36, 8:48])
    out.append(("display_window_inner", {"R": ir, "G": ig, "B": ib}, {"data": inner}, exp))
    big_y, big_x = np.mgrid[-10:H + 10, -10:W + 10].astype(np.float64)
    br, bg, bb = (big_x + 10) / (W + 19), (big_y + 10) / (H + 19), 0.5 + 0 * big_x
    out.append(("display_window_crop", {"R": br, "G": bg, "B": bb}, {"data": (-10, -10, W + 9, H + 9)},
                expected(br[10:-10, 10:-10], bg[10:-10, 10:-10], bb[10:-10, 10:-10], one)))
    out.append(("green_only", {"G": u * v}, {}, expected(u * v, u * v, u * v, one)))
    # Deep: a far opaque gradient and a nearer half-transparent disc stored after it (the decoder must sort
    # by Z), an empty corner (transparent), and a variant without alpha where the nearest sample wins.
    deep = [[[] for _ in range(W)] for _ in range(H)]
    for yy in range(H):
        for xx in range(W):
            if xx < 8 and yy < 8:
                continue
            deep[yy][xx].append((0.8 * u[yy, xx], 0.3, 0.2 + 0.6 * v[yy, xx], 1.0, 10.0))
            if (xx - 32) ** 2 + (yy - 24) ** 2 < 225:
                deep[yy][xx].append((0.05, 0.2, 0.45, 0.5, 2.0 + 0.01 * xx))
    planes = flatten(deep)
    out.append(("deep_overlap", deep, {"deep": True}, expected(*planes)))
    planes = flatten(deep, alpha=False)
    out.append(("deep_no_alpha", deep, {"deep": True, "alpha": False}, expected(*planes[:3], planes[3])))
    out.append(("depth_only", {"Z": u}, {}, None))  # no colour: the codec must decline it
    return out


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    folder = sys.argv[1]
    os.makedirs(folder, exist_ok=True)
    check = "--check" in sys.argv
    failures = 0
    for name, planes, opts, image in cases():
        exr_path = os.path.join(folder, name + ".exr")
        ref = image if image is not None or name != "sdr_float" else cases()[0][3]
        if not check:
            if opts.get("deep"):
                write_deep(exr_path, planes, alpha=opts.get("alpha", True))
            else:
                write_exr(exr_path, planes, **opts)
            if ref is not None:
                Image.fromarray(ref, "RGBA").save(os.path.join(folder, name + ".png"))
            continue
        out_path = os.path.join(folder, name + ".out.png")
        if ref is None:
            ok = not os.path.exists(out_path)
            print(f"{name}: {'declined as expected' if ok else 'decoded but should be declined'}")
            failures += not ok
            continue
        if not os.path.exists(out_path):
            print(f"{name}: no output")
            failures += 1
            continue
        got = np.asarray(Image.open(out_path).convert("RGBA")).astype(int)
        diff = np.abs(got - ref.astype(int))
        worst = int(diff.max())
        print(f"{name}: max difference {worst}" + ("" if worst <= 2 else "  FAIL"))
        failures += worst > 2
    if check:
        print("all good" if failures == 0 else f"{failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
