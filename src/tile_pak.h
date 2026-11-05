#pragma once

// The core map pack on storage (format: tile_pak_format.h). GeoQuake keeps the 340 z1-z4 tiles as ONE file,
// TILE_PAK_PATH, on the SD card or in LittleFS (one file instead of 340 saves ~0.9 MB, because LittleFS gives
// every file at least one 4 KB block).
//
// tile_pak_load_index expects the caller to hold that filesystem's lock (sd_lock / flash_lock); the others take it
// per call through the given callbacks.

#include <FS.h>
#include "tile_pak_format.h"

#define TILE_PAK_PATH "/GeoQuakeLite/tiles/core.pak"
#define TILE_PAK_PART_PATH "/GeoQuakeLite/tiles/core.pak.part"
#define TILE_PAK_DIR "/GeoQuakeLite/tiles"

// Header and TOC of the pack at `path` (size, structure, bounds): fills *out. Cheap (one 4 KB read): used at start.
bool tile_pak_load_index(fs::FS& fs, const char* path, tile_pak::Index* out);

// Every distinct payload of an already loaded index decodes completely as a 256x256 QOI image (qoi_decoder.h, the
// decoder the map uses). Run once on a downloaded pack before it is published. lock/unlock wrap each tile read
// (lock() returning false ends the check: not valid); decoding runs unlocked. Uses 64 KB + 128 KB of PSRAM.
bool tile_pak_check_payloads(fs::FS& fs, const char* path, const tile_pak::Index& index, bool (*lock)(),
                             void (*unlock)());
