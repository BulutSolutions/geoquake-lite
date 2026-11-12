#!/usr/bin/env python3
"""pack_core_tiles.py: v1 stores one payload per entry in TOC order; v2 stores identical tiles once.

Self-contained: synthetic tiles for levels 4, 5 and 6, plus checks on the shipped core.pak.

    python3 tools/tile_generator/test_pack_core_tiles.py
"""
import hashlib
import struct
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import pack_core_tiles as pack  # noqa: E402

TILES = HERE.parents[1] / "data" / "GeoQuakeLite" / "tiles"
QOI_END = b"\x00" * 7 + b"\x01"
# core.pak of release 1.0.1 (z1..z4, version 2, 1,277,905 bytes): the reference for the low levels of later packs.
RELEASED_1_0_1_PACK_SHA256 = "e5684388371083ec7f9d10a21097a3a28701970d77ef304cd2707c43258b2154"


def qoi_payload(filler: bytes) -> bytes:
    """A valid-looking 256x256 QOI payload around `filler`."""
    return b"qoif" + struct.pack(">II", 256, 256) + bytes((3, 0)) + filler + QOI_END


SEA = qoi_payload(b"sea")  # used at every level, so shared between z1..z4 and z5/z6
DEEP = qoi_payload(b"deep-z5z6")  # used only at z5/z6, many times


def synthetic_tiles(levels: int):
    """[(z, x, y, data)] for z1..z{levels} in TOC order, with some payloads shared."""
    tiles = []
    for z, x, y in pack.tile_keys(levels):
        if (x + y) % 3 == 0:
            data = SEA
        elif z >= 5 and (x + y) % 3 == 1:
            data = DEEP
        else:
            data = qoi_payload(f"{z}/{x}/{y}".encode() * (1 + x % 4))
        tiles.append((z, x, y, data))
    return tiles


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


def toc_entries(blob):
    """[(z, x, y, offset, length)] from a pack."""
    count = struct.unpack_from("<H", blob, 6)[0]
    entries = []
    for i in range(count):
        z, x, y, _, offset, length = struct.unpack_from("<BBBBII", blob, pack.HEADER_SIZE + i * pack.TOC_ENTRY_SIZE)
        entries.append((z, x, y, offset, length))
    return entries


class LevelTests(unittest.TestCase):
    def test_tile_counts(self):
        self.assertEqual([pack.tile_count(n) for n in pack.LEVEL_CHOICES], [340, 1364, 5460])

    def test_each_level(self):
        for levels in pack.LEVEL_CHOICES:
            with self.subTest(levels=levels):
                tiles = synthetic_tiles(levels)
                want = {(z, x, y): d for z, x, y, d in tiles}
                v1, v2 = pack.build_pak(tiles, 1), pack.build_pak(tiles, 2)
                self.assertEqual(old_firmware_read(v1), want)
                self.assertEqual(pack.unpack_pak(v2), want)
                distinct = sum(len(d) for d in {d for *_, d in tiles})
                self.assertEqual(len(v2), pack.HEADER_SIZE + len(tiles) * pack.TOC_ENTRY_SIZE + distinct)
                self.assertLess(len(v2), len(v1))
                with self.assertRaises(AssertionError):
                    old_firmware_read(v2)

    def test_v2_low_levels_go_last(self):
        blob = pack.build_pak(synthetic_tiles(6), 2)
        entries = toc_entries(blob)
        low = {off for z, _, _, off, _ in entries if z <= 4}
        high_only = {off for *_, off, _ in entries} - low
        self.assertTrue(high_only and low)
        self.assertGreater(min(low), max(high_only))
        # SEA is shared across z1..z4 and z5/z6, so it sits in the low group.
        sea_offsets = {off for z, x, y, off, _ in entries if (x + y) % 3 == 0}
        self.assertEqual(len(sea_offsets), 1)
        self.assertTrue(sea_offsets <= low)

    def test_340_tiles_keep_first_use_order(self):
        tiles = synthetic_tiles(4)
        data_start = pack.HEADER_SIZE + len(tiles) * pack.TOC_ENTRY_SIZE
        seen, toc, payloads = {}, b"", b""
        for z, x, y, data in tiles:
            if data not in seen:
                seen[data] = data_start + len(payloads)
                payloads += data
            toc += struct.pack("<BBBBII", z, x, y, 0, seen[data], len(data))
        expected = b"QPAK" + struct.pack("<HH", 2, len(tiles)) + b"\x00" * 8 + toc + payloads
        self.assertEqual(pack.build_pak(tiles, 2), expected)

    def test_deterministic(self):
        tiles = synthetic_tiles(5)
        self.assertEqual(pack.build_pak(tiles, 2), pack.build_pak(list(tiles), 2))


class RefusalTests(unittest.TestCase):
    def setUp(self):
        self.tiles = synthetic_tiles(4)

    def test_wrong_count(self):
        with self.assertRaises(ValueError):
            pack.build_pak(self.tiles[:-1], 2)
        with self.assertRaises(ValueError):
            pack.build_pak(self.tiles + [(5, 0, 0, SEA)], 2)

    def test_out_of_order(self):
        tiles = list(self.tiles)
        tiles[1], tiles[2] = tiles[2], tiles[1]
        with self.assertRaises(ValueError):
            pack.build_pak(tiles, 2)

    def test_payload_size_limits(self):
        for size in (pack.MAX_TILE_BYTES + 1, pack.MIN_TILE_BYTES - 1):
            with self.subTest(size=size):
                tiles = list(self.tiles)
                z, x, y, _ = tiles[5]
                tiles[5] = (z, x, y, b"q" * size)
                with self.assertRaises(ValueError):
                    pack.build_pak(tiles, 1)
        self.assertEqual(pack.MAX_TILE_BYTES, 64 * 1024)  # tile_pak::kMaxTileBytes
        self.assertEqual(pack.MIN_TILE_BYTES, 22)

    def test_unknown_version(self):
        with self.assertRaises(ValueError):
            pack.build_pak(self.tiles, 3)

    def test_unpack_wrong_count(self):
        blob = bytearray(pack.build_pak(self.tiles, 2))
        struct.pack_into("<H", blob, 6, 341)
        with self.assertRaises(ValueError):
            pack.unpack_pak(bytes(blob))


class ShippedPackTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.blob = (TILES / "core.pak").read_bytes()
        cls.tiles_by_key = pack.unpack_pak(cls.blob)

    def test_header(self):
        magic, version, count = struct.unpack_from("<4sHH", self.blob, 0)
        self.assertEqual((magic, version), (b"QPAK", 2))
        self.assertIn(count, [pack.tile_count(n) for n in pack.LEVEL_CHOICES])
        self.assertEqual(len(self.tiles_by_key), count)

    def test_rebuild_is_byte_identical(self):
        tiles = [(z, x, y, d) for (z, x, y), d in sorted(self.tiles_by_key.items())]
        self.assertEqual(pack.build_pak(tiles, 2), self.blob)

    def test_payloads_are_256_qoi(self):
        for key, data in self.tiles_by_key.items():
            self.assertEqual(data[:4], b"qoif", key)
            self.assertEqual(struct.unpack_from(">II", data, 4), (256, 256), key)

    def test_low_levels_are_the_released_tiles(self):
        """The z1..z4 tiles of the shipped pack, packed alone, are the pack released with 1.0.1 byte for byte."""
        low = [(z, x, y, d) for (z, x, y), d in sorted(self.tiles_by_key.items()) if z <= 4]
        self.assertEqual(hashlib.sha256(pack.build_pak(low, 2)).hexdigest(), RELEASED_1_0_1_PACK_SHA256)

    @unittest.skipUnless((TILES / "z1" / "0_0.qoi").exists(), "no loose tiles next to core.pak")
    def test_loose_tiles_match_pack(self):
        levels = pack.levels_for_count(len(self.tiles_by_key))
        tiles = pack.collect_tiles(TILES, levels)
        self.assertIsNotNone(tiles)
        self.assertEqual(pack.build_pak(tiles, 2), self.blob)


if __name__ == "__main__":
    unittest.main()
