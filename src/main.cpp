#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include "config.h"
#include "board_profile.h"
#include "crash_trace.h"

// Global flag for Watchdog Recovery Toast
bool watchdog_recovered = false;

#include "ui_display.h"
#include "ui_settings.h"
#include "quake_loader.h"
#include "wifi_manager.h"
#include "icons/icons.h"
#include "theme.h"
#include "settings_manager.h"
#include "ui_sync.h"
#include "battery.h"
#include "sd_card.h"
#include "lv_fs_sd.h"
#include "serial_commands.h"
#include "i2c_mutex.h"
#include "hande_time.h"
#include "audio_manager.h"
#include "version.h"
#include "orientation_manager.h"
#include "map_tiles.h"
#include "splash_screen.h"
#include "perf_probe.h"
#include "geoquake_system_menu.h"

// Refresh timing
#define REFRESH_INTERVAL_MS     600000 // 10 minutes
unsigned long last_quake_update = 0;

// WiFi Power Management
static bool wifi_power_save_enabled = true;

// CPU Governor state
static int current_cpu_freq = 240;
static TaskHandle_t loop_task_handle = nullptr;
static TaskHandle_t lvgl_task_handle = nullptr;
static uint32_t local_ui_ready_ms = 0;

static size_t task_stack_headroom_bytes(TaskHandle_t task = nullptr) {
    return static_cast<size_t>(uxTaskGetStackHighWaterMark(task)) *
           sizeof(StackType_t);
}

// --- CPU GOVERNOR ---
void update_cpu_governor() {
    unsigned long now = millis();
    int target_freq = 240;

    if (is_fetching_quakes || (now - last_interaction_millis < 5000)) {
        target_freq = 240;
    } else {
        target_freq = 160;
    }

    if (target_freq != current_cpu_freq) {
        setCpuFrequencyMhz(target_freq);
        current_cpu_freq = target_freq;
    }
}

// --- BACKGROUND TASKS ---
// Periodic refresh task
static void refresh_quakes_task(void* param) {
    const int MAX_RETRIES = 2;
    static int retry_count = 0;
    const uint32_t boot_ms = millis();

    for (;;) {
        // Wait for next refresh interval (use settings or fallback to default).
        // First fetch: TLS needs the date, so while the clock is not set retry
        // every 5 s for the first minute after boot instead of a whole interval.
        AppSettings* settings = settings_get();
        uint32_t interval_ms = settings ? (settings->sync_interval_minutes * 60 * 1000) : REFRESH_INTERVAL_MS;
        const bool clock_pending = !hande_time_valid() && (millis() - boot_ms) < 60000UL;
        vTaskDelay(pdMS_TO_TICKS(clock_pending ? 5000 : interval_ms));

        if (ui_get_state() == UI_STATE_MAIN) {
            Serial.println("[MAIN] Background refresh started");

            QuakeFetchResult result = FETCH_SUCCESS;
            WiFiOperationResult op_res = wifi_managed_operation("Background Refresh", [&result]() {
                result = fetch_quakes_with_result();
                return (result == FETCH_SUCCESS);
            });

            if (op_res == WIFI_OP_SUCCESS) {
                retry_count = 0;
                Serial.println("[RefreshTask] Fetch successful");
            } else if (op_res == WIFI_OP_WIFI_FAILED) {
                quake_note_connect_failed();
                if (retry_count < MAX_RETRIES) {
                    retry_count++;
                    vTaskDelay(pdMS_TO_TICKS(retry_count * 5000));
                    continue;
                }
                retry_count = 0;
            } else {
                Serial.println("[RefreshTask] Refresh cycle completed with cached fallback");
            }
            Serial.println("[MAIN] Background refresh complete, WiFi OFF");
        }
    }
}

static void post_wifi_task(void* param) {
    Serial.println("[POST_WIFI] Starting earthquake data sync (skipping manifest)");

    // Initialize and fetch earthquake data
    init_quake_loader();
    ui_queue_loading_text("Loading Earthquakes...");
    fetch_quakes();

    ui_queue_set_state(UI_STATE_MAIN);

    Serial.printf("[PERF] task=PostWiFi stack_headroom=%uB\n",
                  static_cast<unsigned>(
                      uxTaskGetStackHighWaterMark(nullptr) *
                      sizeof(StackType_t)));
    vTaskDelete(NULL);
}

static void startup_task(void* param) {
    ui_queue_loading_text("Connecting WiFi...");
    if (wifi_has_saved_credentials() && wifi_try_saved_credentials()) {
        ui_queue_set_state(UI_STATE_FETCH_MANIFEST);
        xTaskCreatePinnedToCore(post_wifi_task, "PostWiFi", 8192, NULL, 1, NULL, 0);
    } else {
        ui_queue_set_state(UI_STATE_WIFI_SETUP);
    }
    Serial.printf("[PERF] task=Startup stack_headroom=%uB\n",
                  static_cast<unsigned>(
                      uxTaskGetStackHighWaterMark(nullptr) *
                      sizeof(StackType_t)));
    vTaskDelete(NULL);
}

void memory_monitor_task(void *param) {
    for (;;) {
        size_t free_heap = ESP.getFreeHeap();
        size_t free_psram = ESP.getFreePsram();
        size_t largest_internal_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        size_t largest_psram_block = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        size_t min_free_heap =
            heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        size_t min_free_psram = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);

        const size_t loop_headroom =
            loop_task_handle ? task_stack_headroom_bytes(loop_task_handle) : 0;
        const size_t lvgl_headroom =
            lvgl_task_handle ? task_stack_headroom_bytes(lvgl_task_handle) : 0;
        const float psram_fragmentation =
            free_psram
                ? 100.0f *
                      (1.0f -
                       static_cast<float>(largest_psram_block) /
                           static_cast<float>(free_psram))
                : 0.0f;
        Serial.printf(
            "[PERF] uptime_ms=%lu local_ui_ms=%lu internal_free=%uKB "
            "internal_largest=%uKB "
            "internal_min=%uKB psram_total=%uKB psram_free=%uKB "
            "psram_largest=%uKB psram_min=%uKB psram_fragmentation=%.1f%% "
            "loop_stack_headroom=%uB lvgl_stack_headroom=%uB "
            "memmon_stack_headroom=%uB\n",
            static_cast<unsigned long>(millis()),
            static_cast<unsigned long>(local_ui_ready_ms),
            static_cast<unsigned>(free_heap / 1024),
            static_cast<unsigned>(largest_internal_block / 1024),
            static_cast<unsigned>(min_free_heap / 1024),
            static_cast<unsigned>(ESP.getPsramSize() / 1024),
            static_cast<unsigned>(free_psram / 1024),
            static_cast<unsigned>(largest_psram_block / 1024),
            static_cast<unsigned>(min_free_psram / 1024),
            psram_fragmentation,
            static_cast<unsigned>(loop_headroom),
            static_cast<unsigned>(lvgl_headroom),
            static_cast<unsigned>(task_stack_headroom_bytes()));

        // Warning thresholds
        if (largest_psram_block < 512 * 1024) {
            Serial.println("║ [!] WARNING: Low contiguous PSRAM!         ║");
        }
        if (free_psram < 1024 * 1024) {
            Serial.println("║ [X] CRITICAL: PSRAM below 1MB!             ║");
        }
        vTaskDelay(pdMS_TO_TICKS(30000));  // Every 30 seconds (reduces blocking during panning)
    }
}

// PHASE 3 OPTIMIZATION: Dedicated LVGL task on Core 1
// Eliminates UI freezes during WiFi/networking operations
void lvgl_task(void *param) {
    Serial.println("[LVGL_TASK] Starting on Core 1");

    // Add this task to watchdog - prevents system hang if LVGL task deadlocks
    esp_task_wdt_add(NULL);
    Serial.println("[LVGL_TASK] Added to watchdog");

    // FREEZE DETECTION: Track loop timing to identify blocks
    uint32_t last_loop_ms = 0;
    uint32_t freeze_count = 0;

    // The System Menu (hold BOOT 5 s). Built here, after setup() has the display,
    // LVGL, the HUD and the splash up, so it sits above everything on lv_layer_top().
    if (ui_sync_lock(portMAX_DELAY)) {
        geoquake_system_menu_init();
        ui_sync_unlock();
    }

    for (;;) {
        // Feed watchdog at start of each iteration
        esp_task_wdt_reset();
        uint32_t loop_start = millis();
        uint32_t idle_ms = 1;  // set from LVGL's timer list while the lock is held

        // FREEZE DETECTION: Detect slow iterations (>100ms = freeze candidate)
        if (last_loop_ms > 0 && (loop_start - last_loop_ms) > 100) {
            freeze_count++;
            Serial.printf("[FREEZE_DETECT] LVGL loop gap: %lu ms! (count=%lu)\n",
                         loop_start - last_loop_ms, freeze_count);
        }

        // Acquire LVGL mutex
        uint32_t pre_lock = millis();
        if (ui_sync_lock(pdMS_TO_TICKS(10))) {
            uint32_t lock_time = millis();

            // Process queued UI events from background tasks
            crash_trace_lvgl_phase(LVGL_PHASE_QUEUE);
            ui_process_queue();
            esp_task_wdt_reset();  // Reset WDT after potentially heavy queue processing
            uint32_t queue_time = millis();

            // Process serial command queue (automated testing)
            crash_trace_lvgl_phase(LVGL_PHASE_SERIAL);
            serial_process_command_queue();
            uint32_t serial_time = millis();

            // Check device orientation on flat edges (gentle 1.2s settling)
            crash_trace_lvgl_phase(LVGL_PHASE_ORIENTATION);
            orientation_tick();

            // System Menu: reads BOOT itself; must run before lv_timer_handler()
            crash_trace_lvgl_phase(LVGL_PHASE_TICK);
            geoquake_system_menu_tick();

            // Call LVGL timer handler (rendering, animations, touch)
            crash_trace_lvgl_phase(LVGL_PHASE_TICK);
            ui_tick();  // This calls lv_timer_handler()
            esp_task_wdt_reset();  // Reset WDT after potentially heavy LVGL tick
            uint32_t tick_time = millis();

            // Check for deferred settings saves (500ms debounced)
            crash_trace_lvgl_phase(LVGL_PHASE_SAVES);
            ui_settings_check_deferred_saves();
            uint32_t save_time = millis();

            // Update HUD elements at 1 Hz (ONLY on main screen: the HUD widgets exist only there)
            static unsigned long last_hud_update = 0;
            static uint32_t seen_time_changes = 0;
            const uint32_t time_changes = hande_time_changes();  // clock set / zone learned: redraw now
            if ((millis() - last_hud_update > 1000 || time_changes != seen_time_changes) &&
                ui_get_state() == UI_STATE_MAIN) {
                seen_time_changes = time_changes;
                crash_trace_lvgl_phase(LVGL_PHASE_HUD_CLOCK);
                ui_update_clock();
                crash_trace_lvgl_phase(LVGL_PHASE_HUD_BATTERY);
                ui_update_battery();
                crash_trace_lvgl_phase(LVGL_PHASE_HUD_STATUS);
                ui_update_status_sync();
                last_hud_update = millis();
            }
            uint32_t hud_time = millis();

            // FREEZE DETECTION: Rate-limited warnings with consecutive frame tracking
            // Avoids log spam while still detecting persistent slowness
            static uint8_t consecutive_slow_frames = 0;
            static uint32_t last_slow_log_ms = 0;
            uint32_t total_time = hud_time - lock_time;

            if (total_time > 50) {
                consecutive_slow_frames++;
                // Only log every 5 seconds OR if 10+ consecutive slow frames
                uint32_t now_ms = millis();
                if (now_ms - last_slow_log_ms > 5000 || consecutive_slow_frames >= 10) {
                    Serial.printf("[FREEZE_DETECT] Slow LVGL: lock=%lu queue=%lu serial=%lu tick=%lu save=%lu hud=%lu TOTAL=%lu ms (consecutive=%u)\n",
                        lock_time - pre_lock,
                        queue_time - lock_time,
                        serial_time - queue_time,
                        tick_time - serial_time,
                        save_time - tick_time,
                        hud_time - save_time,
                        total_time,
                        consecutive_slow_frames);
                    last_slow_log_ms = now_ms;
                    consecutive_slow_frames = 0;
                }
            } else {
                consecutive_slow_frames = 0;
            }

            // Time until LVGL's next timer is due (display refresh, indev read, animations)
            idle_ms = lv_timer_get_time_until_next();

            // Release mutex
            crash_trace_lvgl_phase(LVGL_PHASE_IDLE);
            ui_sync_unlock();
        } else {
            // FREEZE DETECTION: Log mutex acquisition failures
            static uint32_t last_mutex_fail_log = 0;
            if (millis() - last_mutex_fail_log > 1000) {
                Serial.printf("[FREEZE_DETECT] LVGL mutex timeout at %lu ms\n", millis());
                last_mutex_fail_log = millis();
            }
        }

        hande_time_loop();  // shared time: RTC write after an SNTP answer (takes i2c_mutex itself)

        last_loop_ms = millis();

        // Sleep until LVGL's next timer is due instead of a fixed 16 ms, so a refresh
        // (LV_DEF_REFR_PERIOD) is never pushed back by a whole loop period. At most
        // 16 ms keeps the queues above serviced at >= 60 Hz; at least 1 tick lets
        // lower-priority tasks on this core run.
        if (idle_ms > 16) idle_ms = 16;
        const TickType_t idle_ticks = pdMS_TO_TICKS(idle_ms);
        vTaskDelay(idle_ticks > 0 ? idle_ticks : 1);
    }
}

// Set to true to skip all menus and show map directly (for testing)
#define MAP_TEST_MODE false

void setup() {
    const uint32_t boot_started_ms = millis();
    Serial.begin(115200);
    PERF_PROBE_INIT();
    loop_task_handle = xTaskGetCurrentTaskHandle();
    Serial.printf("[BUILD] GeoQuake Lite %s %s\n", __DATE__, __TIME__);
    crash_trace_report_and_reset();

    board_profile_load();

    // Initialize serial command interface for automated testing
    serial_commands_init();

    ui_sync_init();
    // Before any task that can fetch quakes exists: "Use Offline" skips the
    // startup task's init, and a later refresh would then take a NULL mutex.
    init_quake_loader();
    if (esp_reset_reason() == ESP_RST_TASK_WDT) watchdog_recovered = true;

    // Initialize SD card for tile rendering
    if (sd_init()) {
        Serial.println("[MAIN] SD card initialized");
    } else {
        Serial.println("[MAIN] SD card not available - using vector map");
    }

    theme_init();
    settings_init();  // Initialize settings manager before UI (loads theme from NVS)
    ui_display_create_gfx();
    ui_init();
    local_ui_ready_ms = millis() - boot_started_ms;
    Serial.printf(
        "[PERF] event=local_ui_ready elapsed_ms=%lu psram_total=%uKB "
        "psram_free=%uKB\n",
        static_cast<unsigned long>(local_ui_ready_ms),
        static_cast<unsigned>(ESP.getPsramSize() / 1024),
        static_cast<unsigned>(ESP.getFreePsram() / 1024));
    i2c_mutex_init();  // Initialize I2C mutex before battery (shares bus with touch)
    audio_init();      // Initialize ES8311 codec on the shared I2C bus with mutex protection
    battery_init();    // Initialize AXP2101 battery monitoring
    orientation_init(); // Initialize QMI8658 6-axis auto-rotation

    // Time service (hande_time.h): RTC + the board's stored zone, before any task reads the time.
    hande_time_begin("GeoQuake-Lite/" FIRMWARE_VERSION_STRING);

#if MAP_TEST_MODE
    // Skip everything and show map directly with test earthquakes
    Serial.println("[TEST] Map test mode - loading test earthquakes");
    init_quake_loader();
    load_test_earthquakes();  // Load NYC, Istanbul, Tokyo, Sydney, São Paulo, Reykjavik
    ui_queue_set_state(UI_STATE_MAIN);
    const bool startup_tasks_ready = true;
#else
    splash_screen_show();
    splash_screen_update("Starting GeoQuake Lite...", 15);
    wifi_init();

    // Boot-time shared-network retries are synchronous and can legitimately
    // run longer than the steady-state watchdog window when the AP is unreachable.
    // Start task watchdog coverage after boot WiFi init returns so loopTask does
    // not reset during a normal connection timeout.
    esp_task_wdt_init(15, true);
    esp_task_wdt_add(NULL);

    // Ensure loopTask can preempt the UI task on Core 1 to reset the watchdog,
    // even during heavy LVGL rendering (e.g. Appearance screen creation/redraw).
    vTaskPrioritySet(NULL, 3);

    const BaseType_t startup_task_result =
        // 6 KB: the Wi-Fi connect in this task left 76-84 B of 4 KB free (measured 2026-10-12). Freed on exit.
        xTaskCreatePinnedToCore(startup_task, "Startup", 6144, NULL, 1, NULL, 0);
    const BaseType_t refresh_task_result =
        xTaskCreatePinnedToCore(
            refresh_quakes_task, "RefreshQuakes", 4096, NULL, 1, NULL, 0);
    const BaseType_t memory_task_result =
        xTaskCreate(memory_monitor_task, "MemMon", 4608, NULL, 1, NULL);
    // Increased from 2048 to 4096 bytes for verbose logging with Serial.printf

    // PHASE 3 OPTIMIZATION: Create dedicated LVGL task on Core 1
    // This separates UI rendering from networking operations for better responsiveness
    const BaseType_t lvgl_task_result = xTaskCreatePinnedToCore(
        lvgl_task,           // Task function
        "LVGL",              // Task name
        12288,               // Stack size: 8 KB overflowed with the Settings hub open while quakes arrived
                             // (measured 2026-10-13: "Stack canary watchpoint triggered (LVGL)")
        NULL,                // Parameters
        2,                   // Priority (higher than networking tasks)
        &lvgl_task_handle,   // Task handle for stack telemetry
        1                    // Core 1 (dedicated UI core)
    );
    const bool startup_tasks_ready =
        startup_task_result == pdPASS &&
        refresh_task_result == pdPASS &&
        memory_task_result == pdPASS &&
        lvgl_task_result == pdPASS;
    if (startup_tasks_ready) {
        Serial.println("[MAIN] Required tasks started successfully");
    } else {
        Serial.printf(
            "[MAIN] ERROR: Required task creation failed "
            "(startup=%ld refresh=%ld memory=%ld lvgl=%ld)\n",
            static_cast<long>(startup_task_result),
            static_cast<long>(refresh_task_result),
            static_cast<long>(memory_task_result),
            static_cast<long>(lvgl_task_result));
    }
#endif  // MAP_TEST_MODE
}

// PHASE 3 OPTIMIZATION: loop() refactored for Core 0 (networking only)
// UI operations moved to dedicated lvgl_task on Core 1
// Battery health (battery.cpp thresholds): checked every 30 s; at
// LOW_BATTERY_WARN_PCT battery_check_health() logs a warning (the HUD battery turns red,
// ui_status_bar.cpp); at CRITICAL_BATTERY_PCT the app shows "Low Battery / Shutting down..."
// and powers off 3 s later. USB power or charging counts as safe and cancels the countdown.
static void battery_health_tick() {
    static unsigned long last_check_ms = 0;
    static unsigned long shutdown_start_ms = 0;
    static bool shutdown_pending = false;
    const unsigned long now = millis();

    if (shutdown_pending) {
        if (battery_is_usb_connected() || battery_is_charging()) {
            Serial.println("[MAIN] External power detected - cancelling battery shutdown");
            shutdown_pending = false;
            if (ui_sync_lock(portMAX_DELAY)) {
                ui_hide_loading();
                ui_sync_unlock();
            }
        } else if (now - shutdown_start_ms >= 3000) {
            battery_shutdown();  // does not return
        }
        return;
    }

    if (now - last_check_ms < 30000) return;
    last_check_ms = now;
    if (battery_check_health() != 2) return;

    Serial.println("[MAIN] CRITICAL BATTERY - Initiating shutdown...");
    if (ui_sync_lock(portMAX_DELAY)) {
        ui_show_loading("Low Battery\nShutting down...", false);
        ui_sync_unlock();
    }
    shutdown_pending = true;
    shutdown_start_ms = now;
}

void loop() {
    esp_task_wdt_reset();
    wifi_process();

    // LVGL now runs on Core 1 independently - only non-UI operations here

    // WiFi credential check (non-blocking)
    static unsigned long last_wifi_cache = 0;
    if (millis() - last_wifi_cache > 1000) {
        ui_update_wifi_cache();
        last_wifi_cache = millis();
    }

    // WiFi setup completion check
    if (ui_get_state() == UI_STATE_WIFI_SETUP && wifi_is_connected()) {
        ui_queue_set_state(UI_STATE_FETCH_MANIFEST);
        xTaskCreatePinnedToCore(post_wifi_task, "PostWiFi", 8192, NULL, 1, NULL, 0);
    }

    // Watchdog recovery check
    if (ui_get_state() == UI_STATE_MAIN) {
        if (watchdog_recovered) {
            ui_queue_status_update("System Recovered");
            watchdog_recovered = false;
        }
    }

    // System management (non-UI)
    update_cpu_governor();
    quake_dispatch_pending_audio();
    ui_apply_auto_dim();
    ui_check_auto_hide();
    ui_check_idle_sleep();
    ui_check_deferred_saves();
    battery_health_tick();

    // Light polling (Core 0 can now handle networking without blocking UI on Core 1)
    vTaskDelay(pdMS_TO_TICKS(50));
}
