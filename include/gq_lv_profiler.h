#pragma once

// Backend for LVGL's LV_PROFILER_BEGIN/END hooks ([env:perf_profile] only): per
// function inclusive and self cycles on the LVGL task, fixed tables, no allocation.
// LVGL's C sources include this through LV_PROFILER_INCLUDE, from inside their own
// extern "C" blocks, so every declaration here has C linkage.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void gq_prof_begin(const char* tag);
void gq_prof_end(const char* tag);

// Measurement side (LVGL task, outside any LVGL call so the call stack starts empty).
void gq_prof_start(void);  // reset tables, record calls made by the current task
void gq_prof_stop(void);
// Top entries by self time, one line each through out(fmt, ...).
void gq_prof_report(void (*out)(const char* fmt, ...), const char* variant, int frames);

#ifdef __cplusplus
}
#endif
