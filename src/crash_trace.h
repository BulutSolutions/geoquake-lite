#pragma once

#include <stdint.h>

// Breadcrumbs kept in RTC no-init memory, which survives a task-watchdog or
// panic reset. The next boot prints where the LVGL task was and which task
// held the storage locks, so a watchdog reset names its cause instead of only
// showing the idle task that the watchdog interrupt happened to land on.

enum LvglPhase : uint8_t {
    LVGL_PHASE_IDLE = 0,
    LVGL_PHASE_QUEUE,
    LVGL_PHASE_SERIAL,
    LVGL_PHASE_ORIENTATION,
    LVGL_PHASE_TICK,
    LVGL_PHASE_SAVES,
    LVGL_PHASE_HUD_CLOCK,
    LVGL_PHASE_HUD_BATTERY,
    LVGL_PHASE_HUD_STATUS,
    LVGL_PHASE_TOUCH_READ,   // inside lv_timer_handler: indev read_cb
    LVGL_PHASE_FLUSH,        // inside lv_timer_handler: display flush_cb
    LVGL_PHASE_MAP_DOTS,     // inside lv_timer_handler: quake dots draw
    LVGL_PHASE_GESTURE,      // inside lv_timer_handler: map gesture event
    LVGL_PHASE_COUNT
};

enum TraceLock : uint8_t {
    TRACE_LOCK_FLASH = 0,
    TRACE_LOCK_SD,
    TRACE_LOCK_COUNT
};

void crash_trace_lvgl_phase(uint8_t phase);

// Marks a callback that LVGL runs inside lv_timer_handler; restores the
// lv_timer_handler phase when the callback returns.
struct CrashTraceScope {
    explicit CrashTraceScope(uint8_t phase) { crash_trace_lvgl_phase(phase); }
    ~CrashTraceScope() { crash_trace_lvgl_phase(LVGL_PHASE_TICK); }
};
void crash_trace_lock_taken(uint8_t lock);
void crash_trace_lock_released(uint8_t lock);

// Call once, early in setup(): prints the breadcrumbs after a watchdog or
// panic reset, then clears them.
void crash_trace_report_and_reset();
