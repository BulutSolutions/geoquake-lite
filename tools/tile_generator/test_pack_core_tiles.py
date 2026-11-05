#!/usr/bin/env python3
"""pack_core_tiles.py: v1 stores one payload per entry in TOC order; v2 stores identical tiles once.

    python3 tools/tile_generator/test_pack_core_tiles.py
"""
import struct
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import pack_core_tiles as pack  # noqa: E402

TILES = HERE.parents[1] / "data" / "GeoQuakeLite" / "tiles"


def old_firmware_read(blob):
    """v1 reader: header, TOC, then payloads back to back in TOC order."""
    magic, version, count, _ = struct.unpack_from("<4sHH8s", blob, 0)
    assert magic == b"QPAK" and version == 1 and count > 0, "not a v1 pack"
    pos = pack.HEADER_SIZE
    toc = []
    for _ in range(count):
        z, x, y, _, _, length = struct.unpack_from("<BBBBII", blob, pos)
        toc.append((z, x, y, length))
        pos += pack.TOC_ENTRY_SIZE
    out = {}
    for z, x, y, length in toc:
        out[(z, x, y)] = blob[pos:pos + length]
        pos += length
    assert pos == len(blob), "reader would read past or stop short of the end"
    return out


class PackTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tiles = pack.collect_tiles(TILES)
        assert cls.tiles is not None and len(cls.tiles) == 340

    def test_v1_reads_in_toc_order(self):
        got = old_firmware_read(pack.build_pak(self.tiles, 1))
        self.assertEqual(got, {(z, x, y): d for z, x, y, d in self.tiles})

    def test_v2_round_trip_and_dedupe(self):
        v1, v2 = pack.build_pak(self.tiles, 1), pack.build_pak(self.tiles, 2)
        self.assertEqual(pack.unpack_pak(v2), {(z, x, y): d for z, x, y, d in self.tiles})
        distinct = sum(len(d) for d in {d for *_, d in self.tiles})
        self.assertEqual(len(v2), pack.HEADER_SIZE + 340 * pack.TOC_ENTRY_SIZE + distinct)
        self.assertLess(len(v2), len(v1))

    def test_v2_is_not_readable_as_v1(self):
        with self.assertRaises(AssertionError):
            old_firmware_read(pack.build_pak(self.tiles, 2))

    def test_shipped_pack_matches_the_tiles(self):
        self.assertEqual((TILES / "core.pak").read_bytes(), pack.build_pak(self.tiles, 2))

    def test_tiles_fit_the_firmware_buffer(self):
        self.assertTrue(all(len(d) <= 64 * 1024 for *_, d in self.tiles))  # tile_pak::kMaxTileBytes


if __name__ == "__main__":
    unittest.main()
