#pragma once

#include <lvgl.h>

// LVGL Filesystem Driver for SD Card
// Registers 'S' drive letter for SD card access
// Usage: lv_image_set_src(img, "S:/GeoQuakeLite/tiles/z0/0_0.sjpg");

// Initialize and register the SD filesystem driver with LVGL
// Call this AFTER sd_init() and lv_init()
void lv_fs_sd_init();

// Check if filesystem is ready
bool lv_fs_sd_ready();
