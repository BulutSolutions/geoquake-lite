#include "test_utils.h"
#include "ui_button_dock.h"
#include "ui_display.h"
#include <string.h>

// Button ID mapping
lv_obj_t* test_get_button_by_id(const char* button_id) {
    if (strcmp(button_id, "settings") == 0) {
        return ui_dock_get_settings_button();
    }
    if (strcmp(button_id, "notifications") == 0) {
        return ui_dock_get_notifications_button();
    }

    // Sync button removed - refresh now via status label tap
    if (strcmp(button_id, "sync") == 0) return NULL;
    // Back button was removed in circular display optimization (replaced by confirm button)
    if (strcmp(button_id, "back") == 0) return NULL;

    return NULL;
}

// Button click by screen + index
lv_obj_t* test_get_button_by_index(const char* screen, int index) {
    // "settings" rows are drawn by the Settings screen: no per-index buttons exist.
    (void)screen;
    (void)index;
    // Unknown screen or invalid index
    return NULL;
}

// Simulate button click (send LVGL event)
bool test_click_button(lv_obj_t* btn) {
    if (!btn) return false;

    // Send CLICKED event to button
    lv_obj_send_event(btn, LV_EVENT_CLICKED, NULL);

    return true;
}

// State enum → string
const char* test_get_state_string() {
    UiState state = ui_get_state();

    switch (state) {
        case UI_STATE_BOOT_SPLASH: return "BOOT_SPLASH";
        case UI_STATE_STARTUP_LOADING: return "STARTUP_LOADING";
        case UI_STATE_WIFI_SETUP: return "WIFI_SETUP";
        case UI_STATE_FETCH_MANIFEST: return "FETCH_MANIFEST";
        case UI_STATE_MAIN: return "MAIN";
        case UI_STATE_SETTINGS: return "SETTINGS";
        case UI_STATE_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

// String → state enum
UiState test_parse_state(const char* state_str) {
    if (strcmp(state_str, "BOOT_SPLASH") == 0) return UI_STATE_BOOT_SPLASH;
    if (strcmp(state_str, "STARTUP_LOADING") == 0) return UI_STATE_STARTUP_LOADING;
    if (strcmp(state_str, "WIFI_SETUP") == 0) return UI_STATE_WIFI_SETUP;
    if (strcmp(state_str, "FETCH_MANIFEST") == 0) return UI_STATE_FETCH_MANIFEST;
    if (strcmp(state_str, "MAIN") == 0) return UI_STATE_MAIN;
    if (strcmp(state_str, "SETTINGS") == 0) return UI_STATE_SETTINGS;
    if (strcmp(state_str, "ERROR") == 0) return UI_STATE_ERROR;

    return UI_STATE_BOOT_SPLASH;  // Invalid → default
}
