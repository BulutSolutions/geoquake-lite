#!/usr/bin/env python3
"""
Core Map Tile Packer for GeoQuake Lite
Packs zoom levels z1 to z4 QOI tiles into one 'core.pak' archive (QPAK).

    python3 pack_core_tiles.py <tiles_dir> <output.pak> [--version 1|2]

Archive Layout (little-endian):
- Magic: b'QPAK' (4 bytes)
- Version: uint16_t (1 or 2)
- Total Tiles: uint16_t (340)
- Reserved: 8 bytes zeros
- Table of Contents (340 * 12 bytes = 4,080 bytes), z1..z4, x, y order:
    struct TOCEntry {
        uint8_t z;
        uint8_t x;
        uint8_t y;
        uint8_t pad;
        uint32_t offset;  // Absolute byte offset in .pak
        uint32_t length;  // Length of QOI tile payload
    };
- QOI tile payloads

Version 1: one payload per TOC entry, in TOC order.
Version 2: identical payloads are stored once and their TOC entries share an offset. The firmware stores the archive
           as one file (/GeoQuakeLite/tiles/core.pak) and reads each tile by offset; it accepts both versions.
"""

import argparse
import struct
import sys
from pathlib import Path

MAGIC = b'QPAK'
HEADER_SIZE = 16
TOC_ENTRY_SIZE = 12


def collect_tiles(tiles_dir: Path):
    """[(z, x, y, data)] for every z1..z4 tile in TOC order, or None when one is missing."""
    tiles = []
    for z in range(1, 5):
        num_tiles = 2 ** z
        for x in range(num_tiles):
            for y in range(num_tiles):
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
    """The archive bytes. Version 2 stores each distinct payload once (first occurrence order)."""
    if version not in (1, 2):
        raise ValueError(f"unknown QPAK version {version}")
    data_start = HEADER_SIZE + len(tiles) * TOC_ENTRY_SIZE
    toc = bytearray()
    payloads = bytearray()
    seen = {}  # payload -> offset (version 2)
    for z, x, y, data in tiles:
        offset = seen.get(data) if version == 2 else None
        if offset is None:
            offset = data_start + len(payloads)
            payloads += data
            if version == 2:
                seen[data] = offset
        toc += struct.pack("<BBBBII", z, x, y, 0, offset, len(data))
    header = struct.pack("<4sHH8s", MAGIC, version, len(tiles), b"\x00" * 8)
    return bytes(header + toc + payloads)


def unpack_pak(blob: bytes):
    """{(z, x, y): data} from an archive (used by the round-trip check)."""
    magic, version, count, _ = struct.unpack_from("<4sHH8s", blob, 0)
    if magic != MAGIC or version not in (1, 2):
        raise ValueError("not a QPAK v1/v2 archive")
    out = {}
    for i in range(count):
        z, x, y, _, offset, length = struct.unpack_from("<BBBBII", blob, HEADER_SIZE + i * TOC_ENTRY_SIZE)
        out[(z, x, y)] = blob[offset:offset + length]
    return out


def pack_tiles(tiles_dir: Path, output_file: Path, version: int = 1) -> bool:
    tiles_dir, output_file = Path(tiles_dir), Path(output_file)
    print(f"[PACK] Searching for tiles in {tiles_dir} (expected 340 tiles across z1-z4)...")
    tiles = collect_tiles(tiles_dir)
    if tiles is None:
        return False
    blob = build_pak(tiles, version)
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
    ap.add_argument("--version", type=int, choices=(1, 2), default=1)
    a = ap.parse_args()
    sys.exit(0 if pack_tiles(a.tiles_dir, a.output, a.version) else 1)
