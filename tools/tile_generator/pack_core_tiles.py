#!/usr/bin/env python3
"""
Core Map Tile Packer for GeoQuake Lite
Packs zoom levels z1 to z4, z5 or z6 QOI tiles into one 'core.pak' archive (QPAK).

    python3 pack_core_tiles.py <tiles_dir> <output.pak> [--levels 4|5|6] [--version 1|2]

Defaults: --levels 6 (5,460 tiles) and --version 2.

Archive Layout (little-endian):
- Magic: b'QPAK' (4 bytes)
- Version: uint16_t (1 or 2)
- Total Tiles: uint16_t (340, 1364 or 5460 for z1..z4, z1..z5 or z1..z6)
- Reserved: 8 bytes zeros
- Table of Contents (count * 12 bytes), z1..zN, x, y order:
    struct TOCEntry {
        uint8_t z;
        uint8_t x;
        uint8_t y;
        uint8_t pad;
        uint32_t offset;  // Absolute byte offset in .pak
        uint32_t length;  // Length of QOI tile payload
    };
- QOI tile payloads, each 22 to 65,536 bytes (the upper limit is tile_pak::kMaxTileBytes)

Version 1: one payload per TOC entry, in TOC order.
Version 2: identical payloads are stored once and their TOC entries share an offset. The firmware stores the archive
           as one file (/GeoQuakeLite/tiles/core.pak) and reads each tile by offset; it accepts both versions.
           Payloads used only by z5/z6 tiles come first, then payloads used by any z1..z4 tile, each group in TOC
           order of first use. LittleFS finds a position by walking back from the end of the file, so the low
           zoom levels (used most) stay cheap to reach. A z1..z4 pack keeps plain first-use order.
"""

import argparse
import struct
import sys
from pathlib import Path

MAGIC = b'QPAK'
HEADER_SIZE = 16
TOC_ENTRY_SIZE = 12
LEVEL_CHOICES = (4, 5, 6)
LOW_LEVELS = 4  # z1..z4 payloads go last in a version 2 pack
MAX_TILE_BYTES = 64 * 1024  # firmware buffer, tile_pak::kMaxTileBytes
MIN_TILE_BYTES = 22  # QOI header (14) plus end marker (8)


def tile_count(levels: int) -> int:
    """Number of tiles in z1..z{levels}."""
    return sum(4 ** z for z in range(1, levels + 1))


def tile_keys(levels: int):
    """[(z, x, y)] for z1..z{levels} in TOC order."""
    return [(z, x, y) for z in range(1, levels + 1) for x in range(2 ** z) for y in range(2 ** z)]


def levels_for_count(count: int) -> int:
    """The level count whose pack holds `count` tiles, or ValueError."""
    for levels in LEVEL_CHOICES:
        if tile_count(levels) == count:
            return levels
    raise ValueError(f"tile count {count} is not one of {[tile_count(n) for n in LEVEL_CHOICES]}")


def collect_tiles(tiles_dir: Path, levels: int = 6):
    """[(z, x, y, data)] for every z1..z{levels} tile in TOC order, or None when one is missing."""
    tiles = []
    for z, x, y in tile_keys(levels):
        tile_path = tiles_dir / f"z{z}" / f"{x}_{y}.qoi"
        if not tile_path.exists():
            alt_path = tiles_dir / f"z{z}" / f"{x}" / f"{y}.qoi"
            if not alt_path.exists():
                print(f"❌ Missing tile: z={z}, x={x}, y={y} (checked {tile_path})")
                return None
            tile_path = alt_path
        tiles.append((z, x, y, tile_path.read_bytes()))
    return tiles


def build_pak(tiles, version: int) -> bytes:
    """The archive bytes. Version 2 stores each distinct payload once, z5/z6-only payloads first."""
    if version not in (1, 2):
        raise ValueError(f"unknown QPAK version {version}")
    levels = levels_for_count(len(tiles))
    if [(z, x, y) for z, x, y, _ in tiles] != tile_keys(levels):
        raise ValueError(f"tiles are not exactly z1..z{levels} in z, x, y order")
    for z, x, y, data in tiles:
        if not MIN_TILE_BYTES <= len(data) <= MAX_TILE_BYTES:
            raise ValueError(f"tile z={z} x={x} y={y} is {len(data)} bytes, "
                             f"outside {MIN_TILE_BYTES}..{MAX_TILE_BYTES}")
    if version == 1:
        order = [data for *_, data in tiles]
    else:
        first_use = list(dict.fromkeys(data for *_, data in tiles))
        low = {data for z, *_, data in tiles if z <= LOW_LEVELS}
        order = [d for d in first_use if d not in low] + [d for d in first_use if d in low]
    data_start = HEADER_SIZE + len(tiles) * TOC_ENTRY_SIZE
    payloads = bytearray()
    offsets = []  # offset of each entry of `order`
    for data in order:
        offsets.append(data_start + len(payloads))
        payloads += data
    shared = dict(zip(order, offsets))  # payload -> offset (version 2)
    toc = bytearray()
    for i, (z, x, y, data) in enumerate(tiles):
        offset = offsets[i] if version == 1 else shared[data]
        toc += struct.pack("<BBBBII", z, x, y, 0, offset, len(data))
    header = struct.pack("<4sHH8s", MAGIC, version, len(tiles), b"\x00" * 8)
    return bytes(header + toc + payloads)


def unpack_pak(blob: bytes):
    """{(z, x, y): data} from an archive (used by the round-trip check)."""
    magic, version, count, _ = struct.unpack_from("<4sHH8s", blob, 0)
    if magic != MAGIC or version not in (1, 2):
        raise ValueError("not a QPAK v1/v2 archive")
    levels_for_count(count)
    out = {}
    for i in range(count):
        z, x, y, _, offset, length = struct.unpack_from("<BBBBII", blob, HEADER_SIZE + i * TOC_ENTRY_SIZE)
        out[(z, x, y)] = blob[offset:offset + length]
    return out


def pack_tiles(tiles_dir: Path, output_file: Path, version: int = 2, levels: int = 6) -> bool:
    tiles_dir, output_file = Path(tiles_dir), Path(output_file)
    print(f"[PACK] Searching for tiles in {tiles_dir} "
          f"(expected {tile_count(levels)} tiles across z1-z{levels})...")
    tiles = collect_tiles(tiles_dir, levels)
    if tiles is None:
        return False
    try:
        blob = build_pak(tiles, version)
    except ValueError as e:
        print(f"❌ {e}")
        return False
    # Round trip: every tile comes back byte for byte.
    back = unpack_pak(blob)
    if len(back) != len(tiles) or any(back[(z, x, y)] != data for z, x, y, data in tiles):
        print("❌ Round-trip check failed")
        return False
    output_file.parent.mkdir(parents=True, exist_ok=True)
    output_file.write_bytes(blob)
    data_bytes = len(blob) - HEADER_SIZE - len(tiles) * TOC_ENTRY_SIZE
    distinct = len({d for *_, d in tiles})
    print(f"✅ Created {output_file} (QPAK v{version})")
    print(f"   Total Size: {len(blob)} bytes ({len(blob) / (1024 * 1024):.2f} MB)")
    print(f"   Tiles: {len(tiles)} ({distinct} distinct), tile data: {data_bytes} bytes")
    return True


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tiles_dir", type=Path)
    ap.add_argument("output", type=Path)
    ap.add_argument("--levels", type=int, choices=LEVEL_CHOICES, default=6)
    ap.add_argument("--version", type=int, choices=(1, 2), default=2)
    a = ap.parse_args()
    sys.exit(0 if pack_tiles(a.tiles_dir, a.output, a.version, a.levels) else 1)
