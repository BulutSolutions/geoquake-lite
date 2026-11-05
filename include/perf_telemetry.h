#pragma once

#include <Arduino.h>
#include "esp_timer.h"
#include "esp_heap_caps.h"

/**
 * @file perf_telemetry.h
 * @brief High-Resolution Performance & Memory Telemetry for ESP32
 * 
 */

namespace Perf {

struct MemorySnapshot {
    uint32_t free_sram;
    uint32_t min_free_sram;
    uint32_t largest_sram_block;
    uint32_t free_psram;
    uint32_t min_free_psram;

    static inline MemorySnapshot capture() {
        MemorySnapshot snap;
        snap.free_sram           = esp_get_free_heap_size();
        snap.min_free_sram       = esp_get_minimum_free_heap_size();
        snap.largest_sram_block  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        snap.free_psram          = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        snap.min_free_psram      = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
        return snap;
    }

    inline void print(const char* label) const {
        Serial.printf("[PERF:MEM] %-16s | SRAM Free: %6u B | MinEver: %6u B | MaxBlock: %6u B | PSRAM Free: %7u B\n",
                      label, free_sram, min_free_sram, largest_sram_block, free_psram);
    }
};

class ScopeTimer {
public:
    inline explicit ScopeTimer(const char* tag) 
        : tag_(tag), start_us_(esp_timer_get_time()), start_mem_(MemorySnapshot::capture()) {}

    inline ~ScopeTimer() {
        int64_t elapsed_us = esp_timer_get_time() - start_us_;
        MemorySnapshot end_mem = MemorySnapshot::capture();
        int32_t sram_diff = (int32_t)end_mem.free_sram - (int32_t)start_mem_.free_sram;
        
        Serial.printf("[PERF:TIME] %-16s | %7.2f ms (%6lld us) | Heap Delta: %+5d B | LargestBlock: %6u B\n",
                      tag_, elapsed_us / 1000.0f, elapsed_us, sram_diff, end_mem.largest_sram_block);
    }

private:
    const char* tag_;
    int64_t start_us_;
    MemorySnapshot start_mem_;
};

class FpsTracker {
public:
    inline FpsTracker() : last_frame_us_(esp_timer_get_time()), frame_count_(0), fps_(0.0f) {}

    inline void on_frame() {
        int64_t now = esp_timer_get_time();
        int64_t delta_us = now - last_frame_us_;
        last_frame_us_ = now;
        
        frame_count_++;
        if (delta_us > 0) {
            float current_fps = 1000000.0f / (float)delta_us;
            fps_ = (fps_ * 0.9f) + (current_fps * 0.1f); // Smooth EMA
        }

        if (frame_count_ % 60 == 0) {
            Serial.printf("[PERF:FPS] Average FPS: %5.1f | Frame Interval: %5.2f ms\n",
                          fps_, (delta_us / 1000.0f));
        }
    }

    inline float get_fps() const { return fps_; }

private:
    int64_t last_frame_us_;
    uint32_t frame_count_;
    float fps_;
};

} // namespace Perf

// Convenience Macro: Wraps any scope with zero-overhead timing and heap tracking
#define PERF_SCOPE(name) Perf::ScopeTimer _perf_scope_timer_##__LINE__(name)
#define PERF_LOG_MEM(label) Perf::MemorySnapshot::capture().print(label)
