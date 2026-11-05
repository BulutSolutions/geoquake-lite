#pragma once

// Display: the CO5300 AMOLED + LVGL 9 display driver.
//
// What it does, in this order: QSPI bus + panel objects, delay(250) for the AXP2101 rails and the AMOLED charge pump,
// panel begin + black, lv_init + millis tick, draw buffers, LVGL display (RGB565, partial strips), the dual-axis
// 2-pixel rounder, and a flush that byte-swaps once (rgb565_swap_inplace: never also build with
// LV_COLOR_16_SWAP) and rotates in software for orientations 90/180/270.
//
// Threads: init, rotation and everything LVGL run on the app's LVGL thread. Panel commands (brightness, on/off,
// direct pixel writes) may come from any task: they share one panel lock with the flush, so a command never lands
// in the middle of a pixel transfer.

#include <stdint.h>
#include <lvgl.h>

class Arduino_GFX;

struct HandeDisplayFlushTiming {
    uint32_t bytes;
    uint32_t total_us;      // queued (or started) -> panel write done
    uint32_t lock_wait_us;  // waiting for the panel lock
    uint32_t cache_us;      // PSRAM cache write-back (0 for internal buffers)
    uint32_t rotate_us;     // software rotation
    uint32_t panel_us;      // QSPI transfer
    int64_t queued_at_us;
    uint32_t tag;           // HandeDisplayConfig::flush_tag at flush time
    bool last;              // last strip of the frame
};

struct HandeDisplayConfig {
    int32_t size = 480;       // square panel edge
    int32_t col_offset = 0;   // CO5300 column offset
    int32_t buffer_lines = 40;  // strip height; must divide size (a remainder strip left stale pixels on scroll)
    bool double_buffer = false;  // only useful with async_flush
    bool buffers_in_psram = false;  // default: internal DMA SRAM (fast, cache-coherent)
    bool software_rotation = true;  // allocates the rotation buffer; false = orientation 0 only
    lv_display_render_mode_t render_mode = LV_DISPLAY_RENDER_MODE_PARTIAL;

    // Async flush: the QSPI transfer runs on a separate task, so LVGL renders the next strip meanwhile
    // (needs double_buffer). Off = the transfer runs inside LVGL's flush callback.
    bool async_flush = false;
    uint8_t flush_task_core = 0;
    uint8_t flush_task_priority = 5;

    // Optional instrumentation (crash trace, cycle probes, frame trace). on_flush_start: start of
    // LVGL's flush callback (LVGL thread). flush_tag: read right after it. on_flush_done: after every strip, from the
    // task that wrote it (= LVGL thread unless async; keep it short, no LVGL calls). on_flush_wait_done: LVGL thread,
    // with the time LVGL waited for an async strip.
    void (*on_flush_start)() = nullptr;
    uint32_t (*flush_tag)() = nullptr;
    void (*on_flush_done)(const HandeDisplayFlushTiming&) = nullptr;
    void (*on_flush_wait_done)(uint32_t waited_us) = nullptr;
};

// Panel only (bus, gfx, delay(250), begin, black); safe to call twice. For screens drawn before LVGL exists.
bool hande_display_panel_begin(const HandeDisplayConfig& config);

// Panel (if not begun yet) + LVGL display. false = a buffer or the display could not be created; a missing rotation
// buffer is fatal (abort), per the AMOLED standard.
bool hande_display_init(const HandeDisplayConfig& config);

lv_display_t* hande_display_get();
Arduino_GFX* hande_display_gfx();  // direct Arduino_GFX access; take hande_display_panel_lock() around any use
int32_t hande_display_size();

// Orientation 0..3 = physical USB position (hande_display_policy.h). Rotates the UI and redraws it. LVGL thread.
void hande_display_set_orientation(uint8_t orientation);
uint8_t hande_display_get_orientation();

// Panel commands (any task).
void hande_display_set_brightness(uint8_t level);  // CO5300 0..255
uint8_t hande_display_get_brightness();            // last level written (the panel's power-on level until then)
void hande_display_panel_off();  // DISPOFF + SLPIN: blocks ~240 ms
void hande_display_panel_on();   // SLPOUT + DISPON: blocks ~240 ms (brightness is not changed)
bool hande_display_panel_is_off();

// Direct pixel writes for apps that draw outside LVGL (video). Big-endian RGB565, UPRIGHT (UI) coordinates like
// LVGL: when the UI is turned (orientation 90/180/270, software_rotation) they are rotated to the panel here.
// While an app draws directly it must not run lv_timer_handler (LVGL would paint over it); call
// hande_display_redraw_lvgl() when LVGL owns the panel again. hande_display_gfx() draws in PANEL coordinates
// (no rotation): an app that also rotates draws through these two instead while hande_display_rotated().
void hande_display_push_be(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t* pixels);
void hande_display_fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color);  // split per kMaxFillPixels
bool hande_display_rotated();  // the UI is turned right now (direct writes are being rotated)
void hande_display_redraw_lvgl();  // invalidate the active screen and render it now (LVGL thread)

// The panel lock (recursive) for Arduino_GFX calls an app makes itself through hande_display_gfx().
void hande_display_panel_lock();
void hande_display_panel_unlock();

struct HandeDisplayFlushStats {
    uint32_t count;
    uint32_t average_us;
    uint32_t max_us;
    uint32_t dropped;
    uint64_t bytes;
};
// reset = start a new measuring window.
HandeDisplayFlushStats hande_display_flush_stats(bool reset);
