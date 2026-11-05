#pragma once

#include <lvgl.h>

#include "tile_pak_format.h"

#ifndef GEOQUAKE_MAP_STORAGE_TEST
#define GEOQUAKE_MAP_STORAGE_TEST 0
#endif

// LVGL Filesystem Driver for the LittleFS partition (drive 'F') and GeoQuake Lite's map pack in it.

void lv_fs_flash_init();
bool lv_fs_flash_ready();
// The LittleFS partition holds a valid core map pack (/GeoQuakeLite/tiles/core.pak, tile_pak.h): fills *out.
bool flash_load_core_pak(tile_pak::Index* out);

#if GEOQUAKE_MAP_STORAGE_TEST
// Map storage check only: remove the pack and a half-written part file (the next boot must start empty).
bool flash_map_test_remove_pack();
#endif

// A LittleFS File held open outside the LVGL driver (the tile downloader's pack file): counted so that
// lv_fs_flash_quiesce() waits for it to close before the filesystem is unmounted.
void lv_fs_flash_handle_opened();
void lv_fs_flash_handle_closed();

void flash_lock();
void flash_unlock();

// Quiesce: block all new flash operations and drain in-flight ones.
// After this returns, it is safe to unmount LittleFS.
// Caller must later call lv_fs_flash_unquiesce() to re-enable.
void lv_fs_flash_quiesce();
void lv_fs_flash_unquiesce();
bool lv_fs_flash_is_quiesced();

// Unmount LittleFS and reset internal mount state so next
// lv_fs_flash_ready() / flash_mount() actually re-initializes.
void flash_unmount();

// F3: Set the maximum time (ms) to wait for open handles to close during quiesce.
// Default is 5000ms. Call before lv_fs_flash_quiesce() if a different timeout is needed.
void lv_fs_flash_set_quiesce_timeout(uint32_t timeout_ms);
