# GeoQuake Lite Tile Generator

GeoQuake Lite's map tiles are drawn from **Natural Earth** data (public domain). If you change the map, keep using data
you may redistribute: most online tile services forbid bulk downloads and redistributing their tiles.

- **Tiles**: 256×256 grey QOI images, a dark look for the AMOLED (sea grey 38, land grey 9 in RGB565), Montserrat labels (SIL Open Font License, credited in THIRD_PARTY_NOTICES.md).
- **Core set z1–z4** (340 tiles): one pack file, placed on the device (see "How tiles reach a device").
- **Detail set z5–z6** (1,024 + 4,096 tiles): optional, copied by hand to an SD card.

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

# 2. Render (pip install -r requirements.txt): z1-z4 in ~25 s, z5-z6 in ~5 min
python3 render_natural_earth.py <data dir> Montserrat.ttf ../../data/GeoQuakeLite/tiles --zooms 1-4
python3 render_natural_earth.py <data dir> Montserrat.ttf <detail dir> --zooms 1-6 --png <preview dir>

# 3. Pack the core set (v2 stores identical tiles once)
python3 pack_core_tiles.py ../../data/GeoQuakeLite/tiles ../../data/GeoQuakeLite/tiles/core.pak --version 2
python3 test_pack_core_tiles.py
```

The renderer is deterministic: the same data and font give byte-identical tiles. `--only z/x/y,...` renders single
tiles while tuning the style.

## How tiles reach a device

The core pack is `/GeoQuakeLite/tiles/core.pak` in the device's LittleFS. It is built on a computer (step 3 above) and
shipped in the filesystem image, so every device has the core tiles without a network request.

GeoQuake Lite checks every tile in the pack before using it and reads tiles from it by offset. One file instead of
340 saves ~0.9 MB (LittleFS gives every file at least one 4 KB block).

Detail tiles are optional files on an SD card, read when present:

```
/sdcard/GeoQuakeLite/tiles/
├── core.pak            z1-z4 (also present in LittleFS)
├── z5/<x>/<y>.qoi      optional
└── z6/<x>/<y>.qoi      optional
```

## Pack format (QPAK)

16-byte header (`QPAK`, u16 version 1|2, u16 tile count 340, 8 reserved bytes), then 340 × 12-byte TOC entries
(u8 z, u8 x, u8 y, u8 pad, u32 offset, u32 length, little-endian, z1..z4/x/y order), then the QOI payloads. v1 stores
one payload per entry in TOC order; v2 lets entries share an offset. Firmware parser: `src/tile_pak_format.h`
(unit-tested in `test/test_tile_pak.cpp`).

`convert_to_qoi.py` (PNG to QOI) and `convert_to_sjpg.py` (dormant SJPG path) are older helpers.
