#pragma once

// Touch: the CST92xx touch pipeline.
//
//   controller read (shared I2C lock)  ->  filter (hande_touch_policy.h)  ->  delivery
//
// Reading happens either inside LVGL's input callback (sampler_period_ms = 0: the LVGL thread reads) or on a sampler
// task (sampler_period_ms > 0: reads are timestamped and independent of LVGL load). With LVGL input, gestures are
// recognised as LVGL consumes the samples (in order with on_sample); without it, as they are read. Delivery, any mix
// of:
//   - LVGL input device: RAW panel points (LVGL rotates them itself). From a sampler task every press and release
//     reaches LVGL in order (no quick tap is lost between two LVGL reads).
//   - on_sample hook on the LVGL thread: the caller sees raw + VISUAL (upright) points and may change what LVGL
//     gets (custom gestures such as map pan and pinch).
//   - gesture events (tap, swipes, long press) when a GestureConfig is given.
//   - a sample queue + current sample for builds whose LVGL is render-only.

#include <stdint.h>
#include <lvgl.h>

#include "hande_touch_policy.h"

// Builds whose LVGL never takes input define HANDE_TOUCH_LVGL=0 so LVGL's input-device code is not linked at
// all.
#ifndef HANDE_TOUCH_LVGL
#define HANDE_TOUCH_LVGL 1
#endif

struct HandeTouchSample {
    bool pressed;
    uint8_t count;                           // fingers, 0 when released
    hande_touch_policy::Point raw[2];        // panel coordinates
    hande_touch_policy::Point visual[2];     // upright UI coordinates (hande_display orientation)
    bool press_edge;
    bool release_edge;
    bool forced_release;                     // the bus stayed busy: end any gesture
    bool fresh;                              // points read now (false = held or released)
    uint32_t ms;
};

// What LVGL is told for one read; on_sample may change it.
struct HandeTouchLvglInput {
    bool pressed;
    hande_touch_policy::Point point;  // RAW
};

struct HandeTouchGesture {
    hande_touch_policy::GestureType type;
    hande_touch_policy::Point start;  // visual
    hande_touch_policy::Point end;    // visual
    uint32_t ms;                      // when it was recognised
};

struct HandeTouchConfig {
    // Controller and bus.
    int16_t size = 480;
    bool mirror_x = false;
    bool mirror_y = false;
    bool reset_pin = true;          // pulse GPIO 40 (10 ms low, 50 ms settle) at init
    bool begin_wire = true;         // Wire.begin(15, 14) here (false = the app did it)
    uint32_t i2c_clock_hz = 400000; // global for every device on the bus
    uint16_t wire_timeout_ms = 0;   // Wire.setTimeOut, 0 = leave it
    uint32_t lock_timeout_ms = 20;  // per read
    uint32_t report_delay_us = 200; // between the register write and the report read

    hande_touch_policy::FilterConfig filter;

    // 0 = read in LVGL's input callback; otherwise a sampler task reading every period (woken early by the
    // touch interrupt when sampler_wake_on_int).
    uint16_t sampler_period_ms = 0;
    uint8_t sampler_core = 0;
    uint8_t sampler_priority = 2;
    uint16_t sampler_stack = 4096;
    bool sampler_wake_on_int = false;

    bool lvgl_indev = true;
    lv_display_t* display = nullptr;  // indev display (nullptr = default display)

    // Skip the I2C read while released and the INT line is high (no new report). Never used during a press:
    // INT is a pulse, not a level. nullptr = never skip.
    bool (*may_skip_idle_read)() = nullptr;
    // Lock timeout per read when it depends on the screen (GeoQuake: 5 ms on the map, 20 ms on panels).
    uint32_t (*lock_timeout_for_read)() = nullptr;

    // LVGL thread, once per sample LVGL consumes.
    void (*on_sample)(const HandeTouchSample& sample, HandeTouchLvglInput& lvgl) = nullptr;

    // Gesture events (visual coordinates); nullptr = none.
    const hande_touch_policy::GestureConfig* gestures = nullptr;

    // Keep a sample queue for hande_touch_pop_sample() (render-only LVGL apps).
    bool sample_queue = false;
};

bool hande_touch_init(const HandeTouchConfig& config);
lv_indev_t* hande_touch_indev();

bool hande_touch_is_pressed();
HandeTouchSample hande_touch_current();          // latest filtered sample
bool hande_touch_pop_sample(HandeTouchSample* out);
bool hande_touch_pop_gesture(HandeTouchGesture* out);
void hande_touch_flush();                        // drop queued samples and gestures (after a modal screen);
                                                 // with LVGL input, call it on the thread that runs LVGL
void hande_touch_cancel_gesture();               // the current press produces no gesture (it was consumed)
bool hande_touch_wait_ready(uint32_t timeout_ms);  // controller answers its address (startup health gate)
uint32_t hande_touch_last_activity_ms();         // last read with a finger down

struct HandeTouchStats {
    uint32_t reads;
    uint32_t bus_busy;
    uint32_t sample_overruns;
    uint32_t gesture_overruns;
    uint32_t bus_errors;     // report reads that failed (not waits for the shared bus); information only
    uint32_t not_answering;  // times the controller ignored its address 3 idle pings in a row (sampler task only)
};
HandeTouchStats hande_touch_stats();
uint32_t hande_touch_sampler_stack_headroom_bytes();  // 0 without a sampler task
