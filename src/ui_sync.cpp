/**
 * UI Synchronization Layer - Implementation
 *
 * Provides thread-safe LVGL operations via mutex + queue architecture.
 */

#include "ui_sync.h"
#include "ui_display.h"
#include "ui_power.h"
#include "quake_loader.h"
#include "wifi_manager.h"
#include "map_tiles.h"
#include "map_renderer.h"
#include <string.h>
#include "hande_display.h"
#include <atomic>

// ============================================================================
// PRIVATE STATE
// ============================================================================

static SemaphoreHandle_t ui_mutex = NULL;
static QueueHandle_t ui_event_queue = NULL;
static TaskHandle_t mutex_owner = NULL;

// Queue size - enough to buffer burst of updates
#define UI_QUEUE_SIZE 20

// Re-entrancy gate - prevents multiple simultaneous manual refreshes
// Using std::atomic for proper thread-safe check-and-set across cores
static std::atomic<bool> manual_refresh_in_progress{false};
// A refresh that must not be lost (ui_queue_manual_refresh_kept) could not start: one was running, the event queue
// was full or its task could not be created. It stays pending here and ui_process_queue() starts it, not before
// the given time (a failed task creation is tried again after 5 s).
static std::atomic<bool> manual_refresh_again{false};
static std::atomic<uint32_t> manual_refresh_again_from_ms{0};

static void keep_manual_refresh(uint32_t wait_ms) {
    manual_refresh_again_from_ms.store(millis() + wait_ms);
    manual_refresh_again.store(true);
}

// ============================================================================
// MUTEX IMPLEMENTATION
// ============================================================================

void ui_sync_init() {
    // Create recursive mutex (allows same task to lock multiple times)
    ui_mutex = xSemaphoreCreateRecursiveMutex();
    if (!ui_mutex) {
        Serial.println("[UI_SYNC] ERROR: Failed to create mutex!");
        return;
    }

    // Create event queue
    ui_event_queue = xQueueCreate(UI_QUEUE_SIZE, sizeof(UI_Event));
    if (!ui_event_queue) {
        Serial.println("[UI_SYNC] ERROR: Failed to create event queue!");
        return;
    }

    Serial.println("[UI_SYNC] Initialized - Mutex + Queue ready");
}

bool ui_sync_lock(uint32_t timeout_ms) {
    if (!ui_mutex) return false;

    TickType_t ticks = (timeout_ms == portMAX_DELAY) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);

    if (xSemaphoreTakeRecursive(ui_mutex, ticks) == pdTRUE) {
        mutex_owner = xTaskGetCurrentTaskHandle();
        return true;
    }
    return false;
}

void ui_sync_unlock() {
    if (!ui_mutex) return;
    xSemaphoreGiveRecursive(ui_mutex);
    mutex_owner = NULL;
}

bool ui_sync_is_locked() {
    return (mutex_owner == xTaskGetCurrentTaskHandle());
}

// FIX #3: Check if we're on the LVGL-safe core
// LVGL runs on Core 1 (Arduino default task), decode tasks run on Core 0
bool ui_sync_is_lvgl_core() {
    return (xPortGetCoreID() == 1);
}

// ============================================================================
// QUEUE IMPLEMENTATION
// ============================================================================

// Internal: Send event to queue (non-blocking)
static bool queue_event(UI_EventType type, const char* text, int32_t value) {
    if (!ui_event_queue) return false;

    UI_Event evt;
    evt.type = type;
    evt.value = value;

    if (text) {
        strncpy(evt.text, text, UI_EVT_TEXT_MAX - 1);
        evt.text[UI_EVT_TEXT_MAX - 1] = '\0';
    } else {
        evt.text[0] = '\0';
    }

    // Non-blocking send - if queue full, drop the message
    if (xQueueSend(ui_event_queue, &evt, 0) != pdTRUE) {
        Serial.println("[UI_SYNC] Warning: Event queue full, dropping message");
        return false;
    }
    return true;
}

// ============================================================================
// BACKGROUND TASK FOR ASYNC OPERATIONS
// ============================================================================

// Background task for manual refresh (runs on Core 0, doesn't block LVGL on Core 1)
static void manual_refresh_task(void* param) {
    Serial.println("[REFRESH] Background task started on Core 0");

    WiFiOperationResult result = wifi_managed_operation("Manual Refresh", []() {
        // Post "Syncing..." status to UI queue (processed by LVGL task)
        queue_event(UI_EVT_STATUS_TEXT, "syncing", 0);

        QuakeFetchResult fetch_result = fetch_quakes_with_result();
        return (fetch_result == FETCH_SUCCESS);
    });

    // Post completion status
    if (result == WIFI_OP_SUCCESS) {
        Serial.println("[REFRESH] Completed successfully");
    } else {
        Serial.printf("[REFRESH] Failed with result: %d\n", result);
        if (result == WIFI_OP_WIFI_FAILED) quake_note_connect_failed();
    }

    // Restore normal display via queue
    queue_event(UI_EVT_STATUS_TEXT, "restore", 0);

    manual_refresh_in_progress.store(false);
    vTaskDelete(NULL);  // Clean up task
}

// Background task for earthquake reload (filter changes)
static void reload_earthquakes_task(void* param) {
    Serial.println("[RELOAD] Background task started on Core 0");

    WiFiOperationResult result = wifi_managed_operation("Settings Filter Change", []() {
        // Post "Syncing..." status to UI queue
        queue_event(UI_EVT_STATUS_TEXT, "syncing", 0);

        QuakeFetchResult fetch_result = fetch_quakes_with_result();
        return (fetch_result == FETCH_SUCCESS);
    });

    if (result == WIFI_OP_SUCCESS) {
        Serial.println("[RELOAD] Completed successfully");
    } else {
        Serial.printf("[RELOAD] Failed with result: %d\n", result);
        if (result == WIFI_OP_WIFI_FAILED) quake_note_connect_failed();
    }

    // Restore normal display via queue
    queue_event(UI_EVT_STATUS_TEXT, "restore", 0);

    manual_refresh_in_progress.store(false);  // Reuse same gate
    vTaskDelete(NULL);
}

// LVGL task. Starts the manual refresh task. A kept request that cannot start now stays pending.
static void start_manual_refresh(bool kept) {
    // Gate: atomic check-and-set prevents multiple simultaneous refreshes
    if (manual_refresh_in_progress.exchange(true)) {
        if (kept) {
            keep_manual_refresh(0);
            Serial.println("[UI_SYNC] Refresh already in progress, one more follows");
        } else {
            Serial.println("[UI_SYNC] Refresh already in progress, ignoring");
        }
        return;
    }

    Serial.println("[UI_SYNC] Manual refresh triggered - spawning background task");

    // Show "Connecting..." immediately (renders next frame)
    ui_update_status_text("connecting");

    // Spawn background task on Core 0 (doesn't block LVGL on Core 1)
    if (xTaskCreatePinnedToCore(
        manual_refresh_task,   // Task function
        "ManualRefresh",       // Task name
        8192,                  // Stack size (WiFi + HTTP needs space)
        NULL,                  // No parameters
        1,                     // Priority
        NULL,                  // Don't need handle
        0                      // Core 0 (background/networking)
    ) != pdPASS) {
        Serial.println("[UI_SYNC] ERROR: Could not create refresh task!");
        ui_update_status_text("restore");
        manual_refresh_in_progress.store(false);
        if (kept) keep_manual_refresh(5000);
    }
}

void ui_process_queue() {
    if (!ui_event_queue) return;

    // A kept refresh that could not start earlier: start it as soon as no other refresh runs.
    if (manual_refresh_again.load() && !manual_refresh_in_progress.load() &&
        static_cast<int32_t>(millis() - manual_refresh_again_from_ms.load()) >= 0) {
        manual_refresh_again.store(false);
        start_manual_refresh(true);
    }

    UI_Event evt;

    // Drain all pending events
    while (xQueueReceive(ui_event_queue, &evt, 0) == pdTRUE) {
        switch (evt.type) {
            case UI_EVT_SET_STATE:
                // State machine transition - this is the ONLY safe place to change UI state
                ui_set_state((UiState)evt.value);
                break;

            case UI_EVT_LOADING_TEXT:
                ui_update_loading_text(evt.text);
                break;

            case UI_EVT_STATUS_UPDATE:
                ui_update_status(evt.text);
                break;

            case UI_EVT_ABORT_DOWNLOADS:
                // Signal quake_loader to abort
                abort_quake_fetch();
                break;

            case UI_EVT_SET_BRIGHTNESS:
                // Apply brightness change & deep AMOLED sleep/wake
                {
                    static bool s_display_asleep = false;

                    if (evt.value == 0) {
                        // Enter deep display sleep & low-power 80MHz CPU mode
                        hande_display_set_brightness(0);
                        hande_display_panel_off();
                        setCpuFrequencyMhz(80); // Cut ESP32-S3 active core power in half
                        s_display_asleep = true;
                        Serial.println("[POWER] 💤 Entered deep AMOLED sleep & 80MHz CPU");
                    } else {
                        // Wake up display & restore CPU frequency
                        if (s_display_asleep) {
                            setCpuFrequencyMhz(160);
                            hande_display_panel_on();
                            s_display_asleep = false;
                            Serial.println("[POWER] ⚡ Woke AMOLED from sleep to 160MHz CPU");
                        }
                        hande_display_set_brightness(evt.value);
                        lv_obj_invalidate(lv_screen_active());
                    }
                    Serial.printf("[UI_SYNC] Brightness applied: %d\n", evt.value);
                }
                break;

            case UI_EVT_RELOAD_EARTHQUAKES:
                // Reload earthquakes with new filter settings - spawn async task
                {
                    // Gate: atomic check-and-set prevents multiple simultaneous refreshes
                    if (manual_refresh_in_progress.exchange(true)) {
                        Serial.println("[UI_SYNC] Refresh already in progress, ignoring reload");
                        break;
                    }

                    Serial.println("[UI_SYNC] Settings filter change - spawning background task");

                    // Show "Connecting..." immediately (renders next frame)
                    ui_update_status_text("connecting");

                    // Spawn background task on Core 0 (doesn't block LVGL on Core 1)
                    if (xTaskCreatePinnedToCore(
                        reload_earthquakes_task,  // Task function
                        "ReloadQuakes",           // Task name
                        8192,                     // Stack size (WiFi + HTTP needs space)
                        NULL,                     // No parameters
                        1,                        // Priority
                        NULL,                     // Don't need handle
                        0                         // Core 0 (background/networking)
                    ) != pdPASS) {
                        Serial.println("[UI_SYNC] ERROR: Could not create reload task!");
                        ui_update_status_text("restore");
                        manual_refresh_in_progress.store(false);
                    }
                }
                break;

            case UI_EVT_MANUAL_REFRESH:
                // Manual refresh from the sync button, or a kept one (value 1) from Settings
                start_manual_refresh(evt.value == 1);
                break;

            case UI_EVT_STATUS_TEXT:
                // Handle status text updates from background tasks
                ui_update_status_text(evt.text);
                break;

            case UI_EVT_AUTO_HIDE_SHOW:
                // Show all HUD components (called from LVGL task - thread safe)
                ui_auto_hide_show_internal();
                break;

            case UI_EVT_AUTO_HIDE_HIDE:
                // Hide HUD components (called from LVGL task - thread safe)
                ui_auto_hide_hide_internal();
                break;

    case UI_EVT_CLOCK_REFRESH:
        ui_update_clock();
        break;

    // Safe Core 1 tile mode activation after background download completes
    case UI_EVT_ACTIVATE_TILE_MODE: {
        map_tiles_refresh_availability();
        if (map_tiles_available()) {
            map_set_tile_mode(true);
            Serial.println("[UI_SYNC] High-resolution tile mode activated on Core 1");
        }
        break;
    }

    default:
        break;
        }
    }
}

// ============================================================================
// SAFE UI UPDATE FUNCTIONS (for background tasks)
// ============================================================================

void ui_queue_loading_text(const char* text) {
    queue_event(UI_EVT_LOADING_TEXT, text, 0);
}

void ui_queue_status_update(const char* text) {
    queue_event(UI_EVT_STATUS_UPDATE, text, 0);
}

void ui_queue_status_text(const char* status_type) {
    queue_event(UI_EVT_STATUS_TEXT, status_type, 0);
}

void ui_queue_abort_downloads() {
    queue_event(UI_EVT_ABORT_DOWNLOADS, NULL, 0);
}

void ui_queue_set_brightness(int level) {
    queue_event(UI_EVT_SET_BRIGHTNESS, NULL, level);
}

void ui_queue_reload_earthquakes() {
    queue_event(UI_EVT_RELOAD_EARTHQUAKES, NULL, 0);
}

void ui_queue_manual_refresh() {
    queue_event(UI_EVT_MANUAL_REFRESH, NULL, 0);
}

void ui_queue_manual_refresh_kept() {
    if (!queue_event(UI_EVT_MANUAL_REFRESH, NULL, 1)) keep_manual_refresh(0);  // queue full: still not lost
}

// ============================================================================
// STATE MACHINE
// ============================================================================

void ui_queue_set_state(UiState state) {
    Serial.printf("[UI_SYNC] Queuing state transition to %s\n", ui_state_name(state));
    queue_event(UI_EVT_SET_STATE, NULL, (int32_t)state);
}

// ============================================================================
// AUTO-HIDE THREAD-SAFE QUEUE FUNCTIONS
// ============================================================================

void ui_queue_auto_hide_show() {
    queue_event(UI_EVT_AUTO_HIDE_SHOW, NULL, 0);
}

void ui_queue_auto_hide_hide() {
    queue_event(UI_EVT_AUTO_HIDE_HIDE, NULL, 0);
}

void ui_queue_clock_refresh() {
    queue_event(UI_EVT_CLOCK_REFRESH, NULL, 0);
}

void ui_queue_activate_tile_mode() {
    queue_event(UI_EVT_ACTIVATE_TILE_MODE, NULL, 0);
}
