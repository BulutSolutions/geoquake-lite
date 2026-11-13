#include "ui_touch.h"
#include "crash_trace.h"
#include "config.h"
#include "board_profile.h"
#include "map_renderer.h"
#include "ui_sync.h"
#include "ui_state.h"
#include "ui_button_dock.h"
#include "ui_notifications_panel.h"
#include "ui_quake_detail.h"
#include "i2c_mutex.h"
#include "settings_manager.h"
#include "hande_touch.h"
#include <math.h>
#include "ui_display.h"  // For UiState enum

// GeoQuake's touch over the touch pipeline in hande_touch.cpp. It reads the CST92xx in LVGL's input callback under
// the shared I2C lock and filters it (3 busy reads held, then a forced release; release on the 2nd no-contact read);
// on_sample() below is GeoQuake's own logic: panels get raw points through LVGL, the map gets pan / pinch on visual
// points (200 px jump clamp), dock buttons stay LVGL presses, and the first touch on an idle-sleeping screen only
// wakes it.

// External declarations from ui_display.cpp (will be moved later)
extern UiState ui_get_state();
extern bool ui_is_panel_open();
extern lv_obj_t* ui_ImgContainer;
extern unsigned long last_interaction_millis;

// Touch input device
static lv_indev_t* touch_indev = NULL;
static lv_timer_t* input_enable_timer = NULL;

// Gesture state
static bool is_pinching = false;
static bool is_single_touch_panning = false;

// Touch tracking
static bool last_touch_state = false;
static int16_t last_touch_x = 0, last_touch_y = 0;  // last visual point on the map (jump clamp)
static bool wake_touch_swallowed = false;            // the press that woke the screen does nothing else

// Jump detection moved into per-frame delta clamp (simpler approach)

// Median filter removed for performance - replaced with simpler per-frame delta clamp

// Timing state
static uint32_t last_touch_millis = 0;

// Auto-hide component show pending flag
static volatile bool show_components_pending = false;

// Idle sleep state tracking
static bool idle_sleep_active = false;
static int user_brightness_before_sleep = 200;

// Auto-hide state
static bool auto_hide_active = false;
static uint32_t last_ui_interaction_millis = 0;

// Idle throttle removed - IRQ early exit handles this case

// Forward declarations
static void on_sample(const HandeTouchSample& s, HandeTouchLvglInput& lvgl);
static uint32_t lock_timeout_for_read();
static bool may_skip_idle_read();
static void end_map_gestures();
static void reenable_input_cb(lv_timer_t* timer);

// --- Public API Implementation ---

void ui_touch_init(lv_display_t* disp) {
    // Same controller setup as before: GPIO 40 reset pulse, Wire.begin(15, 14), 400 kHz, board mirror flags.
    HandeTouchConfig config;
    config.size = (int16_t)g_board.screen_size;
    config.mirror_x = g_board.touch_mirror_x;
    config.mirror_y = g_board.touch_mirror_y;
    config.reset_pin = true;
    config.begin_wire = true;
    config.i2c_clock_hz = 400000;  // 400kHz Fast-Mode I2C for snappy touch response
    config.wire_timeout_ms = 0;
    config.report_delay_us = 200;
    config.filter.hold_on_bus_busy = 3;          // a busy bus never ends a drag early; the 4th busy read releases
    config.filter.release_after_no_contact = 2;  // release on 2 consecutive zero reads
    config.sampler_period_ms = 0;                // read in LVGL's input callback (LV_DEF_REFR_PERIOD)
    config.display = disp;
    config.lock_timeout_for_read = lock_timeout_for_read;
    config.may_skip_idle_read = may_skip_idle_read;
    config.on_sample = on_sample;
    if (!hande_touch_init(config)) {
        Serial.println("[UI_TOUCH] Touch init FAILED!");
    } else {
        Serial.println("[UI_TOUCH] Touch init OK (CST92xx)");
    }
    Serial.flush();

    touch_indev = hande_touch_indev();
    Serial.printf("[UI_TOUCH] Created indev %p\n", (void*)touch_indev);

    // Initialize timing
    last_touch_millis = millis();
    last_ui_interaction_millis = millis();
}

// --- INPUT DEVICE HEALTH MONITORING ---

void ui_touch_ensure_active() {
    if (!touch_indev) return;
    lv_indev_enable(touch_indev, true);
}

// --- EXTERNAL API ---
lv_indev_t* ui_touch_get_indev() {
    return touch_indev;
}

void ui_touch_set_indev(lv_indev_t* indev) {
    touch_indev = indev;
}

bool ui_touch_is_pressed() {
    return last_touch_state;
}

void ui_disable_input_temporarily(uint32_t delay_ms) {
    if (!touch_indev) return;

    lv_indev_enable(touch_indev, false);
    lv_indev_reset(touch_indev, NULL);

    if (input_enable_timer) {
        lv_timer_del(input_enable_timer);
        input_enable_timer = NULL;
    }
    input_enable_timer = lv_timer_create(reenable_input_cb, delay_ms, NULL);
    lv_timer_set_repeat_count(input_enable_timer, 1);
}

void ui_touch_reinit(const char* reason) {
    Serial.printf("[UI_TOUCH] Reinit requested (%s) - skipped\n", reason ? reason : "unknown");
    if (touch_indev) {
        lv_indev_reset(touch_indev, NULL);
        lv_indev_enable(touch_indev, true);
    }
}

void ui_touch_throttle(uint32_t interval_ms, uint32_t duration_ms) {
    // Throttle removed - IRQ early exit handles this case
    (void)interval_ms;
    (void)duration_ms;
}

bool ui_touch_is_panning() {
    return is_single_touch_panning;
}

bool ui_touch_is_pinching() {
    return is_pinching;
}

void ui_touch_end_gestures() {
    end_map_gestures();
}

uint32_t ui_touch_get_last_touch_millis() {
    return last_touch_millis;
}

void ui_touch_update_interaction_time() {
    last_ui_interaction_millis = millis();
}

void ui_touch_note_physical_interaction() {
    const uint32_t now = millis();
    last_touch_millis = now;
    last_ui_interaction_millis = now;
    last_interaction_millis = now;
    if (idle_sleep_active) {
        idle_sleep_active = false;
        const int brightness = user_brightness_before_sleep;
        ui_queue_set_brightness(brightness < 50 ? 50 : brightness);
    }
}

bool ui_touch_has_pending_show_components() {
    return show_components_pending;
}

void ui_touch_clear_pending_show_components() {
    show_components_pending = false;
}

// Access idle sleep state
bool ui_touch_is_idle_sleep_active() {
    return idle_sleep_active;
}

void ui_touch_set_idle_sleep_active(bool active) {
    idle_sleep_active = active;
}

int ui_touch_get_brightness_before_sleep() {
    return user_brightness_before_sleep;
}

void ui_touch_set_brightness_before_sleep(int brightness) {
    user_brightness_before_sleep = brightness;
}

// Access auto-hide state
bool ui_touch_is_auto_hide_active() {
    return auto_hide_active;
}

void ui_touch_set_auto_hide_active(bool active) {
    auto_hide_active = active;
}

uint32_t ui_touch_get_last_ui_interaction_millis() {
    return last_ui_interaction_millis;
}

// --- Private Implementation ---

static void reenable_input_cb(lv_timer_t* timer) {
    LV_UNUSED(timer);
    if (touch_indev) {
        lv_indev_enable(touch_indev, true);
        Serial.println("[UI_TOUCH] Re-enabled touch");
    }
    input_enable_timer = NULL;
}

static bool is_panel_active() {
    return (ui_get_state() != UI_STATE_MAIN) || ui_is_panel_open();
}

// I2C lock timeout per read: 20 ms on panels/lists, 5 ms on the map. Core calls this right before it reads the
// controller, so it also marks the touch-read phase for the crash breadcrumbs (on_sample restores LVGL_PHASE_TICK).
static uint32_t lock_timeout_for_read() {
    crash_trace_lvgl_phase(LVGL_PHASE_TOUCH_READ);
    return is_panel_active() ? 20 : 5;
}

// INT early exit (no I2C read while INT is high) only on the plain map. It is asked only while no press is in
// progress: never during a gesture (stale coords reversed the pan once) and never on other screens (their lists
// could not scroll).
static bool may_skip_idle_read() {
    return ui_get_state() == UI_STATE_MAIN && !ui_is_panel_open();
}

static void end_map_gestures() {
    if (is_single_touch_panning) {
        is_single_touch_panning = false;
        map_on_touch_end();
    }
    if (is_pinching) {
        is_pinching = false;
        map_on_pinch_end();
    }
}

// LVGL thread, once per LVGL read. LVGL always receives RAW points (it rotates them itself); lvgl.point already
// holds the latest raw point.
static void on_sample(const HandeTouchSample& s, HandeTouchLvglInput& lvgl) {
    CrashTraceScope trace(LVGL_PHASE_TOUCH_READ);
    const uint32_t now = s.ms;

    // Released: 2 zero reads in a row, or the bus stayed busy for 3 reads (forced). End any gesture the way a
    // real release does, so map state (dots, HUD) matches what LVGL sees.
    if (s.release_edge) {
        end_map_gestures();
        last_touch_state = false;
        wake_touch_swallowed = false;
        lvgl.pressed = false;
        return;
    }
    if (!s.pressed) {
        lvgl.pressed = false;
        return;
    }

    // Held (bus busy or a single zero read): keep the previous state at the last raw point, no gesture update.
    if (!s.fresh) {
        lvgl.pressed = last_touch_state;
        return;
    }

    // Wake from idle sleep on touch (first-touch wake-only guard): the press that wakes the screen does nothing
    // else until it is released.
    if (wake_touch_swallowed) {
        lvgl.pressed = false;
        return;
    }
    if (idle_sleep_active) {
        idle_sleep_active = false;
        ui_queue_set_brightness(user_brightness_before_sleep);
        last_touch_millis = now;
        last_interaction_millis = now;
        last_ui_interaction_millis = now;
        wake_touch_swallowed = true;
        last_touch_state = false;
        lvgl.pressed = false;
        return;
    }

    if (is_panel_active()) {
        if (is_single_touch_panning || is_pinching) {
            ui_touch_end_gestures();
        }
        // A touch on a panel (Settings, the quake list) is use of the device too: the screen-off timer, which
        // stands still while a panel is open, counts from the last touch there, not from the last one on the map.
        last_touch_millis = now;
        lvgl.pressed = true;
        last_touch_state = true;
        return;
    }

    // Visual screen coordinates for custom map gestures and dock hits (hande_display orientation; same mapping as
    // orientation_transform_point).
    int16_t screen_x[2] = {s.visual[0].x, s.visual[1].x};
    int16_t screen_y[2] = {s.visual[0].y, s.visual[1].y};

    // Per-frame delta clamp on visual coordinates to reject noise spikes
    if (last_touch_state) {
        int16_t dx = screen_x[0] - last_touch_x;
        int16_t dy = screen_y[0] - last_touch_y;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dx > 200 || dy > 200) {
            screen_x[0] = last_touch_x;
            screen_y[0] = last_touch_y;
        }
    }

    last_touch_x = screen_x[0];
    last_touch_y = screen_y[0];
    last_touch_millis = now;
    last_interaction_millis = now;
    last_ui_interaction_millis = now;

    // Show hidden components on touch (only the main map reaches here)
    if (auto_hide_active) {
        show_components_pending = true;
    }

    // 2-finger pinch (visual screen coordinates)
    if (s.count == 2) {
        float dx = screen_x[1] - screen_x[0];
        float dy = screen_y[1] - screen_y[0];
        float distance = sqrtf(dx * dx + dy * dy);
        if (distance > 30.0f) {
            if (is_single_touch_panning) {
                is_single_touch_panning = false;
                map_on_touch_end();
            }
            int16_t mid_x = (screen_x[0] + screen_x[1]) / 2;
            int16_t mid_y = (screen_y[0] + screen_y[1]) / 2;
            if (!is_pinching) {
                is_pinching = true;
                map_on_pinch_begin(distance, mid_x, mid_y);
            } else {
                map_on_pinch_update(distance, mid_x, mid_y);
            }
        }
        lvgl.pressed = false;
    }
    // 1-finger pan / dock button
    else {
        if (is_pinching) {
            is_pinching = false;
            map_on_pinch_end();
        }

        // Check dock button only at gesture START (dock buttons work through LVGL, no pan)
        if (!is_single_touch_panning) {
            if (ui_dock_is_touch_on_dock(screen_x[0], screen_y[0])) {
                lvgl.pressed = true;
                last_touch_state = true;
                return;
            }
            is_single_touch_panning = true;
            map_on_touch_begin(screen_x[0], screen_y[0]);
        } else {
            map_on_touch_move(screen_x[0], screen_y[0]);
        }
        // LVGL gets the raw press so map_gesture_event_cb tap detection works
        lvgl.pressed = true;
    }
    last_touch_state = true;
}
