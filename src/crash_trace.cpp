#include "crash_trace.h"

#include <Arduino.h>
#include <esp_attr.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

struct CrashTrace {
    uint32_t magic;
    uint32_t lvgl_phase;
    uint32_t lvgl_phase_ms;
    char holder[TRACE_LOCK_COUNT][16];
    uint32_t holder_ms[TRACE_LOCK_COUNT];
};

RTC_NOINIT_ATTR static CrashTrace s_trace;
static const uint32_t kTraceMagic = 0x47515452;  // "GQTR"

static const char* const kPhaseNames[LVGL_PHASE_COUNT] = {
    "idle", "ui queue", "serial commands", "orientation", "lv_timer_handler",
    "deferred saves", "HUD clock", "HUD battery", "HUD sync status",
    "touch read", "display flush", "quake dots draw", "map gesture",
};
static const char* const kLockNames[TRACE_LOCK_COUNT] = {"flash", "SD"};

void crash_trace_lvgl_phase(uint8_t phase) {
    s_trace.lvgl_phase = phase;
    s_trace.lvgl_phase_ms = millis();
}

void crash_trace_lock_taken(uint8_t lock) {
    if (lock >= TRACE_LOCK_COUNT) return;
    strncpy(s_trace.holder[lock], pcTaskGetName(nullptr), sizeof(s_trace.holder[lock]) - 1);
    s_trace.holder[lock][sizeof(s_trace.holder[lock]) - 1] = '\0';
    s_trace.holder_ms[lock] = millis();
}

void crash_trace_lock_released(uint8_t lock) {
    if (lock >= TRACE_LOCK_COUNT) return;
    s_trace.holder[lock][0] = '\0';
}

void crash_trace_report_and_reset() {
    const esp_reset_reason_t reason = esp_reset_reason();
    const bool crashed = reason == ESP_RST_TASK_WDT || reason == ESP_RST_INT_WDT ||
                         reason == ESP_RST_WDT || reason == ESP_RST_PANIC;
    if (crashed && s_trace.magic == kTraceMagic) {
        const uint32_t phase = s_trace.lvgl_phase;
        Serial.printf("[CRASH] reset reason %d; LVGL task was in '%s' since %lu ms\n",
                      static_cast<int>(reason),
                      phase < LVGL_PHASE_COUNT ? kPhaseNames[phase] : "?",
                      static_cast<unsigned long>(s_trace.lvgl_phase_ms));
        for (int lock = 0; lock < TRACE_LOCK_COUNT; ++lock) {
            s_trace.holder[lock][sizeof(s_trace.holder[lock]) - 1] = '\0';
            if (s_trace.holder[lock][0]) {
                Serial.printf("[CRASH] %s lock held by task '%s' since %lu ms\n",
                              kLockNames[lock], s_trace.holder[lock],
                              static_cast<unsigned long>(s_trace.holder_ms[lock]));
            } else {
                Serial.printf("[CRASH] %s lock was free\n", kLockNames[lock]);
            }
        }
    }
    memset(&s_trace, 0, sizeof(s_trace));
    s_trace.magic = kTraceMagic;
}
