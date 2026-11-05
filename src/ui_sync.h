#ifndef UI_SYNC_H
#define UI_SYNC_H
/**
 * UI Synchronization Layer
 *
 * This module provides thread-safe UI operations for ESP32 multi-core architecture.
 *
 * Problem: LVGL is NOT thread-safe. Direct calls from background tasks (Core 0)
 * while the main loop (Core 1) runs lv_timer_handler() causes memory corruption.
 *
 * Solution:
 * - Phase 1: Mutex protection for all LVGL operations
 * - Phase 2: Queue-based message passing (background → UI)
 *
 * Usage:
 *   // In main.cpp setup():
 *   ui_sync_init();
 *
 *   // In main loop:
 *   ui_sync_lock();
 *   lv_timer_handler();
 *   ui_process_queue();
 *   ui_sync_unlock();
 *
 *   // From background tasks (safe UI updates):
 *   ui_queue_status_update("Loading...");
 *   ui_queue_loading_text("Downloading frame 1...");
 */

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include <atomic>
#include "config_constants.h"
#include "ui_state.h"

// ============================================================================
// MUTEX PROTECTION
// ============================================================================

/**
 * Initialize the UI synchronization system (call once in setup())
 */
void ui_sync_init();

/**
 * Lock LVGL mutex - call before any LVGL operations
 * @param timeout_ms Maximum time to wait (0 = no wait, portMAX_DELAY = forever)
 * @return true if lock acquired, false if timeout
 */
bool ui_sync_lock(uint32_t timeout_ms = 100);

/**
 * Unlock LVGL mutex - call after LVGL operations complete
 */
void ui_sync_unlock();

/**
 * Check if mutex is currently held by this task
 */
bool ui_sync_is_locked();

// ============================================================================
// FIX #3: LVGL THREAD SAFETY ASSERTIONS
// ============================================================================

/**
 * Check if current execution is on the LVGL-safe core (Core 1)
 * @return true if on Core 1, false if on Core 0
 */
bool ui_sync_is_lvgl_core();

/**
 * Assert macro for debug mode - verifies LVGL calls are on correct core
 * In release builds, this is a no-op for performance.
 * Usage: UI_ASSERT_LVGL_THREAD(); before any lv_* call in non-UI tasks
 */
#ifdef DEBUG
    #define UI_ASSERT_LVGL_THREAD() do { \
        if (!ui_sync_is_lvgl_core()) { \
            Serial.printf("[LVGL] ERROR: Cross-thread LVGL call from Core %d!\n", xPortGetCoreID()); \
            configASSERT(0); \
        } \
    } while(0)
#else
    #define UI_ASSERT_LVGL_THREAD() ((void)0)
#endif

// ============================================================================
// EVENT QUEUE (Phase 2)
// ============================================================================

// Event types for UI updates from background tasks
enum UI_EventType {
    UI_EVT_NONE = 0,
    UI_EVT_SET_STATE,         // Set UI state machine state (value = UiState)
    UI_EVT_LOADING_TEXT,      // Update loading screen text
    UI_EVT_STATUS_UPDATE,     // Update status label (status message)

    // Background Task Control
    UI_EVT_ABORT_DOWNLOADS,   // Signal the earthquake loader to abort the current fetch (async, no wait)

    // Hardware Control (must be handled outside LVGL callbacks to avoid blocking)
    UI_EVT_SET_BRIGHTNESS,    // Apply brightness change (value = brightness 0-255)

    // Earthquake Filter Changes
    UI_EVT_RELOAD_EARTHQUAKES,  // Reload earthquakes with new filter settings
    UI_EVT_MANUAL_REFRESH,      // Manual earthquake refresh from sync button
    UI_EVT_STATUS_TEXT,         // Update status text type ("connecting", "syncing", "restore")

    // Auto-Hide Events (from Core 0 loop() to Core 1 LVGL task)
    UI_EVT_AUTO_HIDE_SHOW,        // Show all components (touch detected)
    UI_EVT_AUTO_HIDE_HIDE,        // Hide components (timeout reached)

    // Clock refresh (from background timezone sync)
    UI_EVT_CLOCK_REFRESH,         // Refresh time labels on the LVGL task

    // Tile import completion (from Core 0 background task)
    UI_EVT_ACTIVATE_TILE_MODE     // Safe Core 1 tile mode activation after the import
};

// Maximum text length for queued messages (see config_constants.h for value)

// Event structure - fixed size for queue compatibility
struct UI_Event {
    UI_EventType type;
    char text[UI_EVT_TEXT_MAX];
    int32_t value;            // For progress percentage, error codes, etc.
};

/**
 * Process all pending UI events from the queue
 * Call this inside the mutex lock in main loop
 */
void ui_process_queue();

// ============================================================================
// STATE MACHINE (thread-safe state transitions)
// ============================================================================

/**
 * Queue a UI state transition (thread-safe, non-blocking)
 * This is the ONLY safe way to change UI state from background tasks.
 * The actual state change happens in ui_process_queue() inside the mutex.
 * @param state The target UI state
 */
void ui_queue_set_state(UiState state);

// ============================================================================
// SAFE UI UPDATE FUNCTIONS (for background tasks)
// ============================================================================

/**
 * Queue a loading text update (thread-safe, non-blocking)
 * @param text The text to display on loading screen
 */
void ui_queue_loading_text(const char* text);

/**
 * Queue a status update (thread-safe, non-blocking)
 * @param text The status message to show
 */
void ui_queue_status_update(const char* text);
// A status type for ui_update_status_text ("connecting", "syncing", "restore"), not text to show.
void ui_queue_status_text(const char* status_type);

/**
 * Queue abort downloads command (thread-safe, non-blocking)
 * Signals the earthquake loader to abort its fetch without blocking.
 * Safe to call from LVGL callbacks - no vTaskDelay().
 */
void ui_queue_abort_downloads();

/**
 * Queue brightness change (thread-safe, non-blocking)
 * Applies hardware brightness change outside LVGL callback context.
 * Safe to call from LVGL callbacks - no blocking SPI/I2C operations.
 * @param level Brightness level 0-255
 */
void ui_queue_set_brightness(int level);

/**
 * Queue earthquake reload (thread-safe, non-blocking)
 * Triggers refresh_quakes_now() when filter settings change.
 * Safe to call from LVGL callbacks.
 */
void ui_queue_reload_earthquakes();

/**
 * Queue manual refresh (thread-safe, non-blocking)
 * Triggers WiFi-managed earthquake fetch with loading UI.
 * Safe to call from LVGL callbacks (sync button).
 */
void ui_queue_manual_refresh();

// ============================================================================
// AUTO-HIDE THREAD-SAFE QUEUE FUNCTIONS
// ============================================================================

/**
 * Queue auto-hide show (thread-safe, non-blocking)
 * Shows all HUD components when touch is detected.
 * Safe to call from loop() on Core 0.
 */
void ui_queue_auto_hide_show();

/**
 * Queue auto-hide hide (thread-safe, non-blocking)
 * Hides HUD components after timeout.
 * Safe to call from loop() on Core 0.
 */
void ui_queue_auto_hide_hide();

/**
 * Queue clock label refresh (thread-safe, non-blocking).
 * Safe to call from background timezone sync tasks.
 */
void ui_queue_clock_refresh();

/**
 * Queue tile mode activation (thread-safe, non-blocking).
 * Called from the Core 0 background tile task when the tiles are ready.
 * Executes map_tiles_refresh_availability() and map_set_tile_mode(true) on Core 1 UI task.
 */
void ui_queue_activate_tile_mode();

#endif // UI_SYNC_H
