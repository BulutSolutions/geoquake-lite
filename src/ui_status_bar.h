#ifndef UI_STATUS_BAR_H
#define UI_STATUS_BAR_H

#include <Arduino.h>
#include <lvgl.h>

// --- STATUS BAR UI ---
// Handles battery indicator, status text, and clock display

// Update battery indicator (icon, percentage, charging status)
void ui_update_battery();

// Auto-hide HUD state (ui_power.cpp): while hidden, a low battery (red) stays on screen
void ui_battery_set_hud_hidden(bool hidden);

// Update status text (e.g., "40/24h" for quake count/time range)
void ui_update_status(const char* text);

// Update status with sync info (quake count / time range)
void ui_update_status_sync();

// Update status text for specific states
// status_type: "connecting", "syncing", "restore"
void ui_update_status_text(const char* status_type);

// Update clock display (time, date, AM/PM)
void ui_update_clock();

// Apply theme colors to status bar elements
void ui_apply_main_theme();

#endif // UI_STATUS_BAR_H
