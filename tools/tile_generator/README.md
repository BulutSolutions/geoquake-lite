# GeoQuake Lite Tile Generator

GeoQuake Lite's map tiles are drawn from **Natural Earth** data (public domain). If you change the map, keep using data
you may redistribute: most online tile services forbid bulk downloads and redistributing their tiles.

- **Tiles**: 256×256 grey QOI images, a dark look for the AMOLED (sea grey 38, land grey 9 in RGB565), Montserrat labels (SIL Open Font License, credited in THIRD_PARTY_NOTICES.md).
- **Map pack z1–z6** (5,460 tiles, 2,363 distinct payloads, about 8.04 MB): one pack file, placed on the device (see
  "How tiles reach a device").

## Make the tiles

```bash
# 1. Data (Natural Earth 10m/50m, public domain) and font (Montserrat, OFL), into any folder outside the repo
for p in physical/ne_10m_land physical/ne_50m_land physical/ne_10m_lakes physical/ne_50m_lakes \
         cultural/ne_10m_admin_0_boundary_lines_land cultural/ne_50m_admin_0_boundary_lines_land \
         cultural/ne_10m_admin_1_states_provinces_lines cultural/ne_10m_admin_0_countries; do
  n=$(basename $p); curl -sSfL -o $n.zip "https://naciscdn.org/naturalearth/10m/$p.zip" \
    || curl -sSfL -o $n.zip "https://naciscdn.org/naturalearth/50m/$p.zip"; unzip -qo $n.zip -d $n
done
curl -sSfL -o Montserrat.ttf "https://github.com/google/fonts/raw/main/ofl/montserrat/Montserrat%5Bwght%5D.ttf"

# 2. Render all six levels into one folder outside the repo (pip install -r requirements.txt)
python3 render_natural_earth.py <data dir> Montserrat.ttf <tiles dir> --zooms 1-6 --png <preview dir>

# 3. Pack them (defaults: --levels 6 --version 2, identical tiles stored once)
python3 pack_core_tiles.py <tiles dir> ../../data/GeoQuakeLite/tiles/core.pak
python3 test_pack_core_tiles.py
```

The renderer is deterministic: the same data and font give byte-identical tiles. `--only z/x/y,...` renders single
tiles while tuning the style.

## How tiles reach a device

The pack is `/GeoQuakeLite/tiles/core.pak` in the device's LittleFS. It is built on a computer (step 3 above) and
shipped in the filesystem image: one pack of about 8 MB in a 9 MiB LittleFS partition, so every device has all six
levels without a network request. `python3 tools/check_release.py` checks that the pack, the partition table and the
build outputs agree.

At start GeoQuake Lite checks the pack's header and table of contents, then reads tiles from it by offset. Every tile
of the shipped pack is decoded with the firmware's decoder in the host tests (`pio test -e native`,
`test/test_tile_pak.cpp`). One file instead of thousands also saves space, because LittleFS gives every file at least
one 4 KB block.

Loose z5/z6 files on an SD card are used only when the pack in use has fewer levels, for example an older four-level
(340-tile) pack. In that case the card looks like this:

```
/sdcard/GeoQuakeLite/tiles/
├── core.pak            z1-z4 (also present in LittleFS)
├── z5/<x>/<y>.qoi      optional
└── z6/<x>/<y>.qoi      optional
```

## Pack format (QPAK)

16-byte header (`QPAK`, u16 version 1|2, u16 tile count, 8 reserved bytes), then one 12-byte TOC entry per tile
(u8 z, u8 x, u8 y, u8 pad, u32 offset, u32 length, little-endian, z/x/y order), then the QOI payloads. The tile count
tells the levels, and only three counts are allowed: 340 = z1..z4, 1,364 = z1..z5, 5,460 = z1..z6.

v1 stores one payload per entry in TOC order. v2 stores identical payloads once (their entries share an offset) and
puts the payloads that no z1..z4 tile uses first and the ones z1..z4 tiles use last. LittleFS finds a position in a
file by walking back from its end, so this keeps the most used levels cheap to reach. Firmware parser:
`src/tile_pak_format.h` (unit-tested in `test/test_tile_pak.cpp`).

`convert_to_qoi.py` (PNG to QOI) and `convert_to_sjpg.py` (dormant SJPG path) are older helpers.
