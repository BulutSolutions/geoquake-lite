#pragma once

// The map pack on storage (format: tile_pak_format.h). GeoQuake Lite keeps the tiles of z1..z4, z1..z5 or z1..z6 as
// ONE file, TILE_PAK_PATH, in LittleFS or on the SD card (one file instead of thousands: LittleFS gives every file
// at least one 4 KB block).
//
// tile_pak_load_index expects the caller to hold that filesystem's lock (sd_lock / flash_lock).

#include <FS.h>
#include "tile_pak_format.h"

#define TILE_PAK_PATH "/GeoQuakeLite/tiles/core.pak"
#define TILE_PAK_PART_PATH "/GeoQuakeLite/tiles/core.pak.part"
#define TILE_PAK_DIR "/GeoQuakeLite/tiles"

// A zeroed index in PSRAM (43.7 KB), nullptr when there is none to spare. Release it with tile_pak_free_index.
// The index never goes on a task stack or into static data: internal RAM is the scarce resource.
tile_pak::Index* tile_pak_alloc_index();
void tile_pak_free_index(tile_pak::Index* index);

// Header and TOC of the pack at `path` (size, structure, bounds): fills *out, whose `levels` then tells the pack's
// highest level. Reads the 16-byte header, then the TOC it announces (4 KB for four levels, 64 KB for six) into a
// temporary PSRAM buffer. Used at start; the tiles themselves are decoded in the host tests, not here.
bool tile_pak_load_index(fs::FS& fs, const char* path, tile_pak::Index* out);
