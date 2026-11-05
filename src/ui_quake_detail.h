#ifndef UI_QUAKE_DETAIL_H
#define UI_QUAKE_DETAIL_H

#include <Arduino.h>
#include <lvgl.h>

// --- EARTHQUAKE DETAIL SHEET ---
// Shows detailed info when user taps on an earthquake dot

// Find the nearest earthquake to the given tap coordinates
// Returns index in quakes list, or -1 if none found within threshold
int ui_find_nearest_quake(int16_t tap_x, int16_t tap_y);

// Show bottom sheet with earthquake details
void ui_show_quake_detail_sheet(int quake_idx);

// Close the detail sheet if open
void ui_close_quake_detail_sheet();

// Check if detail sheet is currently open
bool ui_quake_detail_is_open();

#endif // UI_QUAKE_DETAIL_H
