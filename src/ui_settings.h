#ifndef UI_SETTINGS_H
#define UI_SETTINGS_H

#include <lvgl.h>
#include <stdint.h>

/**
 * GeoQuake Settings = the Settings screen (hande_settings.cpp).
 *
 * ui_settings.cpp only describes GeoQuake's rows (static tables) and wires them to the existing storage
 * (settings_manager AppSettings + NVS namespace "gql_cfg", gql_auto_dim namespace via ui_power). No settings UI is
 * built here. Opened by the gear (ui_button_dock.cpp) through UI_STATE_SETTINGS in ui_display.cpp.
 * LVGL thread only.
 */

// Opens the shared screen (called by ui_set_state when entering UI_STATE_SETTINGS).
void ui_settings_show();

// Closes it without the on_close bounce (called by ui_set_state when leaving UI_STATE_SETTINGS).
void ui_settings_hide();

bool ui_settings_is_visible();

// Current theme accent as 0xRRGGBB (the shared screen's accent).
uint32_t ui_settings_theme_accent();

/**
 * Deferred save flag (checked in main loop)
 * Set to true when settings change, cleared after NVS save
 */
extern bool ui_settings_save_pending;

/**
 * Check and save pending settings changes (deferred save with 500ms debounce)
 * Call from main loop to avoid blocking LVGL callbacks with NVS writes
 */
void ui_settings_check_deferred_saves();

#endif // UI_SETTINGS_H
