#ifndef UI_POWER_H
#define UI_POWER_H

#include <Arduino.h>
#include <lvgl.h>

// --- POWER MANAGEMENT UI ---
// Handles auto-dim, idle sleep, and auto-hide functionality

// Initialize power management (loads settings from NVS)
void ui_power_init();

// --- Auto-Dim (Night Mode) ---
// Check and apply auto-dim based on current time
void ui_apply_auto_dim();

// Auto-dim getters
bool ui_auto_dim_is_enabled();
bool ui_auto_dim_is_active();
int ui_auto_dim_get_start_hour();
int ui_auto_dim_get_end_hour();

// Auto-dim setters (marks save as pending)
void ui_auto_dim_set_enabled(bool enabled);
void ui_auto_dim_set_schedule(int start_hour, int end_hour);

// --- Idle Sleep ---
// Check and apply idle sleep based on last touch time
void ui_check_idle_sleep();

// --- Auto-Hide HUD ---
// Check and apply auto-hide based on last interaction time
void ui_check_auto_hide();

// Show all hidden HUD components (called on touch)
void ui_show_all_components();

// Reset auto-hide timer without showing components
void ui_reset_auto_hide_timer();

// --- Deferred Saves ---
// Process pending NVS writes (call from main loop)
void ui_check_deferred_saves();

// --- Internal LVGL Functions (called from ui_sync.cpp queue handler) ---
// These are thread-safe because they're invoked from the LVGL task via queue
void ui_auto_hide_show_internal();
void ui_auto_hide_hide_internal();

// Map drag: the HUD steps aside while a finger pans the map (LVGL task only)
void ui_hud_drag_begin();
void ui_hud_drag_end();

#endif // UI_POWER_H
