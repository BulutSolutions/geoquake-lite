#pragma once

/**
 * UI State Machine
 *
 * Deterministic state flow:
 * 1. BOOT_SPLASH      - Initial splash screen
 * 2. STARTUP_LOADING  - System initialization
 * 3. WIFI_SETUP       - WiFi configuration (if no saved creds)
 * 4. FETCH_MANIFEST   - Downloading earthquake data
 * 5. MAIN             - Normal operation with earthquake map view
 * 6. SETTINGS         - Settings/configuration screen
 * 7. ERROR            - Error state with retry option
 */

typedef enum {
    UI_STATE_BOOT_SPLASH = 0,
    UI_STATE_STARTUP_LOADING,
    UI_STATE_WIFI_SETUP,
    UI_STATE_FETCH_MANIFEST,
    UI_STATE_MAIN,
    UI_STATE_SETTINGS,
    UI_STATE_ERROR
} UiState;

// State name strings for debugging
static inline const char* ui_state_name(UiState state) {
    switch (state) {
        case UI_STATE_BOOT_SPLASH:      return "BOOT_SPLASH";
        case UI_STATE_STARTUP_LOADING:  return "STARTUP_LOADING";
        case UI_STATE_WIFI_SETUP:       return "WIFI_SETUP";
        case UI_STATE_FETCH_MANIFEST:   return "FETCH_MANIFEST";
        case UI_STATE_MAIN:             return "MAIN";
        case UI_STATE_SETTINGS:         return "SETTINGS";
        case UI_STATE_ERROR:            return "ERROR";
        default:                        return "UNKNOWN";
    }
}
