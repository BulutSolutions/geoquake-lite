#!/usr/bin/env python3
"""Render GeoQuake's dark map tiles from Natural Earth data (public domain).

    render_natural_earth.py <natural-earth dir> <Montserrat.ttf> <out dir> [--zooms 1-4] [--only z/x/y,..] [--png DIR]

Writes <out>/z{z}/{x}_{y}.qoi (z1-z4) and <out>/z{z}/{x}/{y}.qoi (z5-z6), the paths tile_manager.cpp reads: 256x256
grey QOI tiles for the AMOLED (sea 38, land 9 in RGB565).
Labels are placed once per zoom on the whole world, then drawn into every tile they touch, so they never break at
tile edges. Data and font: see README.md ("Natural Earth tiles"). Pack the core set with pack_core_tiles.py.
"""
import argparse
import glob
import math
import struct
from pathlib import Path

import shapefile
from PIL import Image, ImageDraw, ImageFont

TILE = 256
SS = 4  # supersampling factor

# Grey levels (0-255) tuned for the AMOLED screen.
WATER = 38
LAND = 9
BORDER = 40
ADMIN1 = 25
LABEL = 66
CONTINENT = 70
OCEAN_LABEL = 54
# Grey levels a tile may use (smaller QOI files). Anchored on the old tiles' sea (38) and land (9): the display's
# RGB565 turns 40 into a visibly lighter sea than 38.
LEVELS = [9, 13, 17, 21, 25, 29, 33, 38, 42, 46, 50, 54, 58, 62, 66, 70, 74, 78, 82]
SNAP = [min(LEVELS, key=lambda l: abs(l - v)) for v in range(256)]
CONTINENTS = [  # (name, lon, lat): label anchor points
    ("NORTH\nAMERICA", -100, 45), ("SOUTH\nAMERICA", -58, -12), ("EUROPE", 15, 51), ("AFRICA", 20, 7),
    ("ASIA", 90, 46), ("AUSTRALIA", 134, -25), ("ANTARCTICA", 0, -80),
]
OCEANS = [("PACIFIC\nOCEAN", -150, 5), ("ATLANTIC\nOCEAN", -38, 22), ("INDIAN\nOCEAN", 80, -20)]
# Natural Earth's short NAME, with a few made friendlier for a small screen.
NAME_FIX = {"United States of America": "United States", "Dem. Rep. Congo": "DR Congo", "N. Macedonia": "North Macedonia",
            "S. Sudan": "South Sudan", "Solomon Is.": "Solomon Islands", "Côte d'Ivoire": "Ivory Coast",
            "eSwatini": "Eswatini"}
SKIP = {"Vatican City", "Monaco", "Liechtenstein", "San Marino", "Turkish Republic of Northern Cyprus", "Somaliland"}


def merc(lon, lat):
    lat = max(-85.05112878, min(85.05112878, lat))
    s = math.sin(math.radians(lat))
    return (lon + 180.0) / 360.0, 0.5 - math.log((1 + s) / (1 - s)) / (4 * math.pi)


def signed_area(pts):
    return sum(x0 * y1 - x1 * y0 for (x0, y0), (x1, y1) in zip(pts, pts[1:] + pts[:1])) / 2


class Layer:
    """Shapefile parts projected to Web Mercator unit square, each with its bounding box."""

    def __init__(self, path, keep=lambda rec: True, polygon=False):
        r = shapefile.Reader(glob.glob(str(path) + "/*.shp")[0])
        self.parts = []  # (bbox, points, is_hole)
        for sr in r.iterShapeRecords():
            if not keep(sr.record):
                continue
            pts, idx = sr.shape.points, list(sr.shape.parts) + [len(sr.shape.points)]
            for a, b in zip(idx, idx[1:]):
                p = [merc(x, y) for x, y in pts[a:b]]
                if len(p) < 2:
                    continue
                xs, ys = [q[0] for q in p], [q[1] for q in p]
                # shapefile outer rings are clockwise (negative area in a y-down frame is counter-clockwise)
                hole = polygon and signed_area(p) < 0
                self.parts.append(((min(xs), min(ys), max(xs), max(ys)), p, hole))

    def visible(self, box):
        x0, y0, x1, y1 = box
        return [(p, h) for (bx0, by0, bx1, by1), p, h in self.parts
                if bx1 >= x0 and bx0 <= x1 and by1 >= y0 and by0 <= y1]


class Font:
    def __init__(self, path):
        self.path, self.cache = path, {}

    def get(self, size, weight):
        key = (size, weight)
        if key not in self.cache:
            f = ImageFont.truetype(self.path, size)
            f.set_variation_by_name(weight)
            self.cache[key] = f
        return self.cache[key]


def text_lines(font, text, spacing):
    """Returns [(line, width)], total width, line height for letter-spaced text."""
    out = []
    for line in text.split("\n"):
        w = sum(font.getlength(ch) for ch in line) + spacing * (len(line) - 1)
        out.append((line, w))
    asc, desc = font.getmetrics()
    return out, max(w for _, w in out), asc + desc


def wrap(name, limit=12):
    words = name.split()
    if len(name) <= limit or len(words) == 1:
        return name
    best = min(range(1, len(words)), key=lambda i: abs(len(" ".join(words[:i])) - len(" ".join(words[i:]))))
    return " ".join(words[:best]) + "\n" + " ".join(words[best:])


def place_labels(z, cands, font):
    """Greedy collision-free placement in world pixels (at SS). cands: (priority, text, x, y, size, weight, grey, sp)."""
    world = TILE * (2 ** z) * SS
    placed, boxes = [], []
    for _, text, ux, uy, size, weight, grey, sp in sorted(cands, key=lambda c: c[0]):
        f = font.get(size * SS, weight)
        lines, w, lh = text_lines(f, text, sp * SS)
        h = lh * len(lines)
        for dx, dy in ((0, 0), (0, 0.7 * h), (0, -0.7 * h), (-0.4 * w, 0), (0.4 * w, 0)):  # nudge a blocked label
            cx, cy = ux * world + dx, uy * world + dy
            box = (cx - w / 2 - 2 * SS, cy - h / 2, cx + w / 2 + 2 * SS, cy + h / 2)
            if box[1] >= 0 and box[3] <= world and not any(
                    not (box[2] < b[0] or box[0] > b[2] or box[3] < b[1] or box[1] > b[3]) for b in boxes):
                break
        else:
            continue
        boxes.append(box)
        placed.append((box, lines, cx, cy, lh, f, grey, sp * SS))
    return placed


def draw_label(d, lab, ox, oy):
    box, lines, cx, cy, lh, f, grey, sp = lab
    y = cy - lh * len(lines) / 2 - oy
    for line, w in lines:
        x = cx - w / 2 - ox
        for ch in line:
            d.text((x, y), ch, font=f, fill=(grey, grey, grey))
            x += f.getlength(ch) + sp
        y += lh


def encode_qoi(img):
    """QOI encoder (RGB, 3 channels), same format as tools/tile_generator/convert_to_qoi.py."""
    w, h = img.size
    px_data = img.tobytes()
    out = bytearray(b"qoif" + struct.pack(">IIBB", w, h, 3, 0))
    index = [(0, 0, 0, 0)] * 64
    prev, run = (0, 0, 0, 255), 0
    n = w * h
    for i in range(n):
        r, g, b = px_data[3 * i:3 * i + 3]
        px = (r, g, b, 255)
        if px == prev:
            run += 1
            if run == 62 or i == n - 1:
                out.append(0xC0 | (run - 1))
                run = 0
            continue
        if run:
            out.append(0xC0 | (run - 1))
            run = 0
        h_ = (r * 3 + g * 5 + b * 7 + 255 * 11) % 64
        if index[h_] == px:
            out.append(h_)
        else:
            index[h_] = px
            dr, dg, db = (r - prev[0] + 128) % 256 - 128, (g - prev[1] + 128) % 256 - 128, (b - prev[2] + 128) % 256 - 128
            if -2 <= dr <= 1 and -2 <= dg <= 1 and -2 <= db <= 1:
                out.append(0x40 | (dr + 2) << 4 | (dg + 2) << 2 | (db + 2))
            elif -32 <= dg <= 31 and -8 <= dr - dg <= 7 and -8 <= db - dg <= 7:
                out += bytes([0x80 | (dg + 32), (dr - dg + 8) << 4 | (db - dg + 8)])
            else:
                out += bytes([0xFE, r, g, b])
        prev = px
    return bytes(out + b"\x00" * 7 + b"\x01")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ne"), ap.add_argument("font"), ap.add_argument("out")
    ap.add_argument("--zooms", default="1-4")
    ap.add_argument("--only", default="", help="render only these tiles: z/x/y,z/x/y")
    ap.add_argument("--png", help="also write PNG previews under this folder")
    a = ap.parse_args()
    ne, out = Path(a.ne), Path(a.out)
    z0, z1 = map(int, a.zooms.split("-"))
    only = {tuple(map(int, t.replace("/", " ").split())) for t in a.only.split(",") if t}
    font = Font(a.font)

    print("loading data ...")
    land = {50: Layer(ne / "ne_50m_land", polygon=True), 10: Layer(ne / "ne_10m_land", polygon=True)}
    lakes = {50: Layer(ne / "ne_50m_lakes", lambda r: r["scalerank"] <= 2, polygon=True),
             10: Layer(ne / "ne_10m_lakes", lambda r: r["scalerank"] <= 4, polygon=True)}
    borders = {50: Layer(ne / "ne_50m_admin_0_boundary_lines_land"),
               10: Layer(ne / "ne_10m_admin_0_boundary_lines_land")}
    admin1 = Layer(ne / "ne_10m_admin_1_states_provinces_lines", lambda r: r["MIN_ZOOM"] <= 9.0)
    countries = shapefile.Reader(glob.glob(str(ne / "ne_10m_admin_0_countries") + "/*.shp")[0]).records()

    for z in range(z0, z1 + 1):
        res = 50 if z <= 3 else 10
        n = 2 ** z
        cands = []
        if z <= 2:
            cands += [(0, t, *merc(lo, la), 11 if z == 1 else 12, "Bold", CONTINENT, 1.2) for t, lo, la in CONTINENTS]
            cands += [(1, t, *merc(lo, la), 9 if z == 1 else 10, "SemiBold", OCEAN_LABEL, 1.5) for t, lo, la in OCEANS]
        else:
            for r in countries:
                if r["MIN_LABEL"] > (4.0 if z == 3 else 5.0) or r["NAME_EN"] in SKIP:
                    continue
                if r["TYPE"] == "Dependency" or r["NAME"] in ("Hong Kong", "Macao"):
                    continue
                if r["TYPE"] in ("Lease", "Indeterminate") and r["NAME_EN"] != "Antarctica":
                    continue
                name = NAME_FIX.get(r["NAME"], r["NAME"]).upper()
                size = 11 if r["LABELRANK"] <= 2 else 10 if r["LABELRANK"] <= 4 else 8
                cands.append((r["MIN_LABEL"] + r["LABELRANK"] / 10, wrap(name), *merc(r["LABEL_X"], r["LABEL_Y"]),
                              size, "Bold", LABEL, 0.9))
            if z == 3:
                cands += [(9, t, *merc(lo, la), 10, "SemiBold", OCEAN_LABEL, 1.5) for t, lo, la in OCEANS]
        labels = place_labels(z, cands, font)
        print(f"z{z}: {n * n} tiles, {len(labels)} labels")

        for tx in range(n):
            for ty in range(n):
                if only and (z, tx, ty) not in only:
                    continue
                scale = TILE * n * SS
                ox, oy = tx * TILE * SS, ty * TILE * SS
                m = 0.02 / n  # margin in unit coords
                box = (tx / n - m, ty / n - m, (tx + 1) / n + m, (ty + 1) / n + m)
                img = Image.new("L", (TILE * SS, TILE * SS), WATER)
                d = ImageDraw.Draw(img)
                proj = lambda p: [(x * scale - ox, y * scale - oy) for x, y in p]
                for layer, outer, hole in ((land[res], LAND, WATER), (lakes[res], WATER, LAND)):
                    vis = layer.visible(box)
                    for p, h in vis:
                        if not h and len(p) > 2:
                            d.polygon(proj(p), fill=outer)
                    for p, h in vis:
                        if h and len(p) > 2:
                            d.polygon(proj(p), fill=hole)
                if z >= 4:
                    for p, _ in admin1.visible(box):
                        d.line(proj(p), fill=ADMIN1, width=3)
                bw = 0.8 if z <= 2 else 1.0
                for p, _ in borders[res].visible(box):
                    d.line(proj(p), fill=BORDER, width=max(1, round(bw * SS)), joint="curve")
                img = img.convert("RGB")
                d = ImageDraw.Draw(img)
                for lab in labels:
                    b = lab[0]
                    if b[2] >= ox and b[0] <= ox + TILE * SS and b[3] >= oy and b[1] <= oy + TILE * SS:
                        draw_label(d, lab, ox, oy)
                tile = img.resize((TILE, TILE), Image.LANCZOS).point(SNAP * 3)
                rel = Path(f"z{z}") / (f"{tx}_{ty}" if z <= 4 else f"{tx}/{ty}")  # tile_manager.cpp build_tile_path
                (out / rel).parent.mkdir(parents=True, exist_ok=True)
                (out / rel).with_suffix(".qoi").write_bytes(encode_qoi(tile))
                if a.png:
                    (Path(a.png) / rel).parent.mkdir(parents=True, exist_ok=True)
                    tile.save((Path(a.png) / rel).with_suffix(".png"))


if __name__ == "__main__":
    main()
