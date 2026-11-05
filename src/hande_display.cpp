#include "hande_display.h"

#include "hande_display_policy.h"
#include "rgb565_swap.h"

#include <Arduino.h>
#include <algorithm>
#include <Arduino_GFX_Library.h>
#include <draw/sw/lv_draw_sw_utils.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <soc/soc_memory_types.h>

#include <rom/cache.h>

namespace policy = hande_display_policy;

namespace {

// Waveshare ESP32-S3-Touch-AMOLED-2.16: CO5300 on QSPI (CS 12, SCK 38, D0-D3 4-7), reset 39.
constexpr int8_t kPinCs = 12;
constexpr int8_t kPinSck = 38;
constexpr int8_t kPinD0 = 4;
constexpr int8_t kPinD1 = 5;
constexpr int8_t kPinD2 = 6;
constexpr int8_t kPinD3 = 7;
constexpr int8_t kPinReset = 39;
constexpr uint8_t kPowerOnBrightness = 0xD0;  // what Arduino_CO5300's init table writes

HandeDisplayConfig s_config;
Arduino_DataBus* s_bus = nullptr;
Arduino_CO5300* s_panel = nullptr;
lv_display_t* s_display = nullptr;
uint8_t* s_rotated = nullptr;
uint8_t s_orientation = policy::kOrient0;
uint8_t s_brightness = kPowerOnBrightness;
bool s_panel_off = false;

SemaphoreHandle_t s_panel_mutex = nullptr;  // recursive: flush, panel commands, direct writes

struct FlushJob {
    lv_area_t area;
    uint8_t* pixels;
    lv_display_rotation_t rotation;
    int64_t queued_at_us;
    uint32_t tag;
    bool last;
};
QueueHandle_t s_flush_queue = nullptr;
SemaphoreHandle_t s_flush_done = nullptr;

portMUX_TYPE s_stats_mux = portMUX_INITIALIZER_UNLOCKED;
uint32_t s_flush_count = 0;
uint64_t s_flush_total_us = 0;
uint32_t s_flush_max_us = 0;
uint32_t s_flush_dropped = 0;
uint64_t s_flush_bytes = 0;

void panel_lock() { xSemaphoreTakeRecursive(s_panel_mutex, portMAX_DELAY); }
void panel_unlock() { xSemaphoreGiveRecursive(s_panel_mutex); }

void write_back_if_psram(const void* pixels, uint32_t bytes, uint32_t& elapsed_us) {
    if (!esp_ptr_external_ram(pixels)) return;
    const int64_t started = esp_timer_get_time();
    Cache_WriteBack_Addr(reinterpret_cast<uint32_t>(pixels), bytes);
    elapsed_us = static_cast<uint32_t>(esp_timer_get_time() - started);
}

// Draws upright pixels. The QSPI DMA cannot read PSRAM here (IDF 4.4), so for PSRAM pixels the SPI driver borrows a
// 2 KB internal DMA buffer for every chunk; when that borrow fails (a TLS handshake had used the internal RAM up),
// Arduino_ESP32QSPI ignores the error and spi_device_polling_end() asserts. PSRAM pixels go through s_rotated
// (internal DMA) in strips instead.
// Call under the panel lock (it guards s_rotated).
void draw_upright(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t* pixels) {
    if (!s_rotated || !esp_ptr_external_ram(pixels) || w > s_config.size) {  // a row must fit the strip buffer
        s_panel->draw16bitBeRGBBitmap(x, y, const_cast<uint16_t*>(pixels), w, h);
        return;
    }
    const int32_t rows_per_strip = std::max<int32_t>(1, s_config.size * s_config.buffer_lines / w);
    for (int32_t row = 0; row < h; row += rows_per_strip) {
        const int32_t rows = std::min<int32_t>(rows_per_strip, h - row);
        memcpy(s_rotated, pixels + row * w, static_cast<size_t>(rows) * w * sizeof(uint16_t));
        s_panel->draw16bitBeRGBBitmap(x, y + row, reinterpret_cast<uint16_t*>(s_rotated), w, rows);
    }
}

// Sends one rendered (already byte-swapped) strip to the panel. Runs in LVGL's flush callback or the flush task.
void write_strip(const FlushJob& job) {
    HandeDisplayFlushTiming timing{};
    timing.queued_at_us = job.queued_at_us;
    timing.tag = job.tag;
    timing.last = job.last;

    const policy::Area area{job.area.x1, job.area.y1, job.area.x2, job.area.y2};
    const int32_t w = area.x2 - area.x1 + 1;
    const int32_t h = area.y2 - area.y1 + 1;
    timing.bytes = static_cast<uint32_t>(w * h * sizeof(uint16_t));

    const int64_t lock_started = esp_timer_get_time();
    panel_lock();
    timing.lock_wait_us = static_cast<uint32_t>(esp_timer_get_time() - lock_started);

    const uint8_t* source = job.pixels;
    policy::PhysicalBlit blit{area.x1, area.y1, w, h};
    if (job.rotation != LV_DISPLAY_ROTATION_0 && s_rotated) {
        const auto rotation = static_cast<policy::Rotation>(job.rotation);
        blit = policy::physical_blit(area, rotation, s_config.size);
        const int64_t rotate_started = esp_timer_get_time();
        lv_draw_sw_rotate(job.pixels, s_rotated, w, h, w * sizeof(uint16_t), blit.w * sizeof(uint16_t),
                          job.rotation, LV_COLOR_FORMAT_RGB565);
        timing.rotate_us = static_cast<uint32_t>(esp_timer_get_time() - rotate_started);
        source = s_rotated;
    } else if (!s_rotated) {
        write_back_if_psram(source, timing.bytes, timing.cache_us);
    }

    const int64_t panel_started = esp_timer_get_time();
    if (source == s_rotated) {
        s_panel->draw16bitBeRGBBitmap(blit.x, blit.y, reinterpret_cast<uint16_t*>(s_rotated), blit.w, blit.h);
    } else {
        draw_upright(blit.x, blit.y, blit.w, blit.h, reinterpret_cast<const uint16_t*>(source));
    }
    const int64_t done = esp_timer_get_time();
    timing.panel_us = static_cast<uint32_t>(done - panel_started);
    panel_unlock();

    timing.total_us = static_cast<uint32_t>(done - job.queued_at_us);
    portENTER_CRITICAL(&s_stats_mux);
    s_flush_count++;
    s_flush_total_us += timing.total_us;
    s_flush_bytes += timing.bytes;
    if (timing.total_us > s_flush_max_us) s_flush_max_us = timing.total_us;
    portEXIT_CRITICAL(&s_stats_mux);

    if (s_config.on_flush_done) s_config.on_flush_done(timing);
}

void flush_task(void*) {
    FlushJob job{};
    for (;;) {
        if (xQueueReceive(s_flush_queue, &job, portMAX_DELAY) != pdTRUE) continue;
        write_strip(job);
        // LVGL's flush-wait callback takes this on the LVGL thread; no LVGL state is touched from this task.
        xSemaphoreGive(s_flush_done);
    }
}

void flush_wait_cb(lv_display_t*) {
    const int64_t started = esp_timer_get_time();
    xSemaphoreTake(s_flush_done, portMAX_DELAY);
    if (s_config.on_flush_wait_done) {
        s_config.on_flush_wait_done(static_cast<uint32_t>(esp_timer_get_time() - started));
    }
}

void flush_cb(lv_display_t* display, const lv_area_t* area, uint8_t* px_map) {
    if (s_config.on_flush_start) s_config.on_flush_start();
    // The CO5300 wants big-endian RGB565; LVGL renders little-endian. One SIMD swap, on the LVGL thread
    // (the PIE registers belong to this thread).
    rgb565_swap_inplace(reinterpret_cast<uint16_t*>(px_map), lv_area_get_size(area));

    const FlushJob job{*area,
                       px_map,
                       lv_display_get_rotation(display),
                       esp_timer_get_time(),
                       s_config.flush_tag ? s_config.flush_tag() : 0,
                       lv_display_flush_is_last(display)};

    if (!s_config.async_flush) {
        write_strip(job);
        lv_display_flush_ready(display);
        return;
    }
    if (xQueueSend(s_flush_queue, &job, portMAX_DELAY) != pdTRUE) {
        portENTER_CRITICAL(&s_stats_mux);
        s_flush_dropped++;
        portEXIT_CRITICAL(&s_stats_mux);
        xSemaphoreGive(s_flush_done);  // never leave LVGL waiting
    }
}

void rounder_cb(lv_event_t* e) {
    auto* area = static_cast<lv_area_t*>(lv_event_get_param(e));
    policy::Area a{area->x1, area->y1, area->x2, area->y2};
    policy::round_area(a, s_config.size, s_config.size);
    area->x1 = a.x1;
    area->y1 = a.y1;
    area->x2 = a.x2;
    area->y2 = a.y2;
}

void* alloc_buffer(size_t bytes, bool psram) {
    const uint32_t caps = psram ? (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : (MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    return heap_caps_aligned_alloc(16, bytes, caps);
}

}  // namespace

bool hande_display_panel_begin(const HandeDisplayConfig& config) {
    if (s_panel) return true;
    s_config = config;
    if (!s_panel_mutex) s_panel_mutex = xSemaphoreCreateRecursiveMutex();
    if (!s_panel_mutex) return false;

    s_bus = new Arduino_ESP32QSPI(kPinCs, kPinSck, kPinD0, kPinD1, kPinD2, kPinD3);
    s_panel = new Arduino_CO5300(s_bus, kPinReset, 0, false, config.size, config.size, config.col_offset, 0, 0, 0);

    // Let the AXP2101 rails and the AMOLED charge pump settle: without it cold boots show diagonal warping.
    delay(250);
    if (!s_panel->begin()) {
        Serial.println("[DISPLAY] panel begin failed");
        return false;
    }
    s_panel->fillScreen(0x0000);
    s_brightness = kPowerOnBrightness;
    s_panel_off = false;
    return true;
}

bool hande_display_init(const HandeDisplayConfig& config) {
    if (s_display) return true;
    if (!hande_display_panel_begin(config)) return false;
    s_config = config;

    lv_init();
    lv_tick_set_cb([]() -> uint32_t { return millis(); });

    const size_t bytes = static_cast<size_t>(config.size) * config.buffer_lines * sizeof(uint16_t);
    void* buf1 = alloc_buffer(bytes, config.buffers_in_psram);
    void* buf2 = config.double_buffer ? alloc_buffer(bytes, config.buffers_in_psram) : nullptr;
    if (!buf1 || (config.double_buffer && !buf2)) {
        Serial.printf("[DISPLAY] draw buffer allocation failed (%u bytes%s)\n", static_cast<unsigned>(bytes),
                      config.buffers_in_psram ? ", PSRAM" : ", internal");
        heap_caps_free(buf1);
        heap_caps_free(buf2);
        return false;
    }

    if (config.software_rotation || config.buffers_in_psram) {  // rotation, and the DMA copy of PSRAM pixels
        s_rotated = static_cast<uint8_t*>(alloc_buffer(bytes, false));
        if (!s_rotated) {
            Serial.println("[DISPLAY] FATAL: rotation DMA buffer allocation failed");
            abort();
        }
    }

    if (config.async_flush) {
        s_flush_queue = xQueueCreate(1, sizeof(FlushJob));
        s_flush_done = xSemaphoreCreateBinary();
        if (!s_flush_queue || !s_flush_done ||
            xTaskCreatePinnedToCore(flush_task, "DisplayFlush", 3072, nullptr, config.flush_task_priority, nullptr,
                                    config.flush_task_core) != pdPASS) {
            Serial.println("[DISPLAY] FATAL: async flush task could not start");
            abort();
        }
    }

    s_display = lv_display_create(config.size, config.size);
    if (!s_display) {
        heap_caps_free(buf1);
        heap_caps_free(buf2);
        return false;
    }
    lv_display_set_color_format(s_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_display, buf1, buf2, bytes, config.render_mode);
    lv_display_set_flush_cb(s_display, flush_cb);
    if (config.async_flush) lv_display_set_flush_wait_cb(s_display, flush_wait_cb);
    lv_display_add_event_cb(s_display, rounder_cb, LV_EVENT_INVALIDATE_AREA, nullptr);
    return true;
}

lv_display_t* hande_display_get() { return s_display; }
Arduino_GFX* hande_display_gfx() { return s_panel; }
int32_t hande_display_size() { return s_config.size; }

void hande_display_set_orientation(uint8_t orientation) {
    s_orientation = orientation <= policy::kOrient270 ? orientation : static_cast<uint8_t>(policy::kOrient0);
    if (!s_display) return;
    const policy::Rotation rotation = s_rotated ? policy::rotation_for(s_orientation) : policy::kRot0;
    lv_display_set_rotation(s_display, static_cast<lv_display_rotation_t>(rotation));
    lv_obj_invalidate(lv_screen_active());
}

uint8_t hande_display_get_orientation() { return s_orientation; }

void hande_display_set_brightness(uint8_t level) {
    if (!s_panel) return;
    panel_lock();
    s_panel->setBrightness(level);
    s_brightness = level;
    panel_unlock();
}

uint8_t hande_display_get_brightness() { return s_brightness; }

void hande_display_panel_off() {
    if (!s_panel) return;
    panel_lock();
    s_panel->displayOff();
    s_panel_off = true;
    panel_unlock();
}

void hande_display_panel_on() {
    if (!s_panel) return;
    panel_lock();
    s_panel->displayOn();
    s_panel_off = false;
    panel_unlock();
}

bool hande_display_panel_is_off() { return s_panel_off; }

namespace {
// The display's software rotation when the UI is turned (orientation 90/180/270 and a rotation buffer), else 0.
lv_display_rotation_t active_rotation() {
    if (!s_display || !s_rotated) return LV_DISPLAY_ROTATION_0;
    return lv_display_get_rotation(s_display);
}
}  // namespace

bool hande_display_rotated() { return active_rotation() != LV_DISPLAY_ROTATION_0; }

// Upright (UI) coordinates, like LVGL. Turned: rotated in strips through the flush's rotation buffer (under the panel
// lock, which the flush holds while it uses that buffer), each strip landing where physical_blit() puts it.
void hande_display_push_be(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t* pixels) {
    if (!s_panel || !pixels || w <= 0 || h <= 0) return;
    const lv_display_rotation_t rotation = active_rotation();
    panel_lock();
    if (rotation == LV_DISPLAY_ROTATION_0) {
        draw_upright(x, y, w, h, pixels);
        panel_unlock();
        return;
    }
    const int32_t buffer_pixels = s_config.size * s_config.buffer_lines;
    const int32_t rows_per_strip = std::max<int32_t>(1, buffer_pixels / w);
    for (int32_t row = 0; row < h; row += rows_per_strip) {
        const int32_t rows = std::min<int32_t>(rows_per_strip, h - row);
        const policy::Area area{x, y + row, x + w - 1, y + row + rows - 1};
        const policy::PhysicalBlit blit = policy::physical_blit(area, static_cast<policy::Rotation>(rotation), s_config.size);
        lv_draw_sw_rotate(pixels + row * w, s_rotated, w, rows, w * sizeof(uint16_t), blit.w * sizeof(uint16_t),
                          rotation, LV_COLOR_FORMAT_RGB565);
        s_panel->draw16bitBeRGBBitmap(blit.x, blit.y, reinterpret_cast<uint16_t*>(s_rotated), blit.w, blit.h);
    }
    panel_unlock();
}

void hande_display_fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color) {
    if (!s_panel || w <= 0 || h <= 0) return;
    const lv_display_rotation_t rotation = active_rotation();
    if (rotation != LV_DISPLAY_ROTATION_0) {  // a rectangle stays a rectangle: only where it lands changes
        const policy::PhysicalBlit blit = policy::physical_blit(policy::Area{x, y, x + w - 1, y + h - 1},
                                                                static_cast<policy::Rotation>(rotation), s_config.size);
        x = blit.x;
        y = blit.y;
        w = blit.w;
        h = blit.h;
    }
    const int32_t rows_per_write = std::max<int32_t>(1, policy::kMaxFillPixels / w);
    panel_lock();
    for (int32_t row = 0; row < h; row += rows_per_write) {
        const int32_t rows = std::min<int32_t>(rows_per_write, h - row);
        s_panel->fillRect(x, y + row, w, rows, color);
    }
    panel_unlock();
}

void hande_display_redraw_lvgl() {
    if (!s_display) return;
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(s_display);
}

void hande_display_panel_lock() {
    if (s_panel_mutex) panel_lock();
}

void hande_display_panel_unlock() {
    if (s_panel_mutex) panel_unlock();
}

HandeDisplayFlushStats hande_display_flush_stats(bool reset) {
    HandeDisplayFlushStats stats{};
    portENTER_CRITICAL(&s_stats_mux);
    stats.count = s_flush_count;
    stats.average_us = s_flush_count ? static_cast<uint32_t>(s_flush_total_us / s_flush_count) : 0;
    stats.max_us = s_flush_max_us;
    stats.dropped = s_flush_dropped;
    stats.bytes = s_flush_bytes;
    if (reset) {
        s_flush_count = 0;
        s_flush_total_us = 0;
        s_flush_max_us = 0;
        s_flush_dropped = 0;
        s_flush_bytes = 0;
    }
    portEXIT_CRITICAL(&s_stats_mux);
    return stats;
}
