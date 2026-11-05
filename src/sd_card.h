#pragma once

#include "tile_pak_format.h"

#include <stdint.h>
#include <stddef.h>

// SD Card Interface for GeoQuake Tile System
// Uses SPI mode with high-speed transfer (20MHz+)

// Initialize SD card
// Returns true if successful
bool sd_init();

// Check if SD card is present and initialized
bool sd_card_ready();

// Check if a file exists on the SD card
bool sd_file_exists(const char* path);

// Read entire file into buffer
// Returns number of bytes read, or 0 on failure
size_t sd_read_file(const char* path, uint8_t* buffer, size_t max_size);

// Get file size
// Returns file size in bytes, or 0 if file doesn't exist
size_t sd_get_file_size(const char* path);

// The SD card holds a valid core map pack (/GeoQuakeLite/tiles/core.pak, tile_pak.h): fills *out with its index.
bool sd_load_core_pak(tile_pak::Index* out);
// Highest level of the hand-copied detail tiles on the SD card: 6 or 5 when that folder exists, else 4.
uint8_t sd_detail_tile_level();

// Debug/test-only: force sd_load_core_pak() false until reboot.
void sd_set_simulate_no_tiles(bool enabled);
bool sd_simulate_no_tiles_enabled();

// Get SD card info for debugging
void sd_print_info();

// Thread-safe SD access for multi-core decode tasks
// Call sd_lock() before direct SD operations, sd_unlock() after
void sd_lock();
void sd_unlock();
