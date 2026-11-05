#pragma once

/**
 * @file perf_probe.h
 * @brief Zero-cost-when-disabled cycle probes for profiling the map engine.
 *
 * Compiled in only with -D GEOQUAKE_PERF_PROBE=1. In every other build
 * all PERF_* macros below expand to nothing and perf_probe_record() is an empty inline,
 * so instrumented call sites behave exactly like the un-instrumented code.
 *
 * Design:
 *  - One fixed ring of PERF_PROBE_RING uint32_t cycle samples per probe, allocated ONCE in
 *    PSRAM by perf_probe_init(). Count/sum/max live in internal SRAM.
 *  - Single writer per probe (decode task 0 and 1 have their own ids), no locks, no heap
 *    use after init. Reset is requested by the reader and honoured by the writer.
 *  - Cycle counts come from the per-core CCOUNT register, so a BEGIN/END pair must run on
 *    one core (all probed code is task-pinned: LVGL on core 1, tile decode on core 0).
 *    Convert to time with perf_probe_cpu_mhz() captured at probe start.
 */

#include <stdint.h>

enum PerfProbeId {
    PP_PAN_BY,           // map_pan_by(): whole call
    PP_TILE_POS,         // map_tiles_update_positions(): whole call
    PP_REFR,             // one LVGL refresh cycle that flushed at least one strip
    PP_RENDER,           // REFR minus SWAP, FLUSH_CB and FLUSH_WAIT (draw + layout)
    PP_SWAP,             // per frame: sum of FLUSH_START -> flush_cb entry (RGB565 byte swap)
    PP_FLUSH_CB,         // per frame: sum of flush_cb durations
    PP_FLUSH_WAIT,       // per frame: sum of FLUSH_WAIT_START -> FLUSH_WAIT_FINISH
    PP_DOTS,             // draw_earthquake_dots_canvas(): whole call
    PP_DOTS_PROJ,        // per dots call: sum of map_latlon_to_pixel() cycles
    PP_DECODE_T0,        // decode_qoi_to_psram() on TileDec0
    PP_DECODE_T1,        // decode_qoi_to_psram() on TileDec1
    PP_ACQUIRE_T0,       // tile_pool_acquire() inside decode on TileDec0
    PP_ACQUIRE_T1,       // tile_pool_acquire() inside decode on TileDec1
    PP_VECTOR,           // render_vector(): whole call
    PP_FRAME_INTERVAL,   // REFR_START(n) - REFR_START(n-1) between flushed frames
    PP_COUNT
};

#if defined(GEOQUAKE_PERF_PROBE) && GEOQUAKE_PERF_PROBE

#include <esp_cpu.h>
#include <lvgl.h>

#ifndef GEOQUAKE_PERF_VARIANT
#define GEOQUAKE_PERF_VARIANT "V?"
#endif

#define PERF_PROBE_RING 256  // samples kept per probe (power of two)

struct PerfProbeStats {
    uint32_t n;      // samples recorded since the last reset
    uint32_t win;    // samples behind p50/p95 (min(n, PERF_PROBE_RING), the newest ones)
    uint32_t mean;   // cycles, over all n samples
    uint32_t p50;    // cycles, nearest-rank over the window
    uint32_t p95;    // cycles, nearest-rank over the window
    uint32_t max;    // cycles, over all n samples
};

static inline uint32_t perf_cycles() { return esp_cpu_get_ccount(); }

void perf_probe_init();                                   // allocate rings (idempotent)
void perf_probe_record(PerfProbeId id, uint32_t cycles);  // single writer per id
void perf_probe_reset_all();                              // also restarts frame accounting
bool perf_probe_report(PerfProbeId id, PerfProbeStats* out);  // false when n == 0
const char* perf_probe_name(PerfProbeId id);
void perf_probe_set_cpu_mhz(uint32_t mhz);
uint32_t perf_probe_cpu_mhz();

// LVGL display accounting (LVGL task only): REFR/RENDER/SWAP/FLUSH_CB/FLUSH_WAIT/FRAME_INTERVAL.
void perf_probe_attach_display(lv_display_t* disp);
void perf_flush_cb_enter();   // first statement of the display flush callback
void perf_flush_cb_exit();    // last statement of the display flush callback
// Byte swap done inside flush_cb (instead of LVGL's call_flush_cb): count it as SWAP,
// not FLUSH_CB, so both stay comparable across variants.
void perf_flush_swap_inside_cb(uint32_t swap_cycles);
uint32_t perf_frame_count();  // monotonic count of refresh cycles that flushed pixels

struct PerfProbeScope {  // RAII form for functions with several return paths
    uint32_t t0;
    PerfProbeId id;
    explicit PerfProbeScope(PerfProbeId probe) : t0(perf_cycles()), id(probe) {}
    ~PerfProbeScope() { perf_probe_record(id, perf_cycles() - t0); }
};

#define PERF_PROBE_INIT()            perf_probe_init()
#define PERF_PROBE_BEGIN(tag)        const uint32_t tag##_perf_t0 = perf_cycles()
#define PERF_PROBE_END(tag, id)      perf_probe_record((id), perf_cycles() - tag##_perf_t0)
#define PERF_PROBE_SCOPE(tag, id)    PerfProbeScope tag##_perf_scope((id))
#define PERF_PROBE_RECORD(id, cyc)   perf_probe_record((id), (cyc))
// Accumulate the cycles of a sub-step inside a loop, record the sum once afterwards.
#define PERF_CYC_DECL(acc)           uint32_t acc = 0
#define PERF_CYC_BEGIN(tag)          const uint32_t tag##_perf_t0 = perf_cycles()
#define PERF_CYC_ACCUM(tag, acc)     (acc) += perf_cycles() - tag##_perf_t0
#define PERF_ATTACH_DISPLAY(disp)    perf_probe_attach_display(disp)
#define PERF_FLUSH_CB_ENTER()        perf_flush_cb_enter()
#define PERF_FLUSH_CB_EXIT()         perf_flush_cb_exit()
#define PERF_FLUSH_SWAP_INSIDE_CB(c) perf_flush_swap_inside_cb(c)

#else  // probes compiled out: every macro is empty, the record helper is an empty inline

static inline void perf_probe_record(PerfProbeId, uint32_t) {}

#define PERF_PROBE_INIT()
#define PERF_PROBE_BEGIN(tag)
#define PERF_PROBE_END(tag, id)
#define PERF_PROBE_SCOPE(tag, id)
#define PERF_PROBE_RECORD(id, cyc)
#define PERF_CYC_DECL(acc)
#define PERF_CYC_BEGIN(tag)
#define PERF_CYC_ACCUM(tag, acc)
#define PERF_ATTACH_DISPLAY(disp)
#define PERF_FLUSH_CB_ENTER()
#define PERF_FLUSH_CB_EXIT()
#define PERF_FLUSH_SWAP_INSIDE_CB(c)

#endif  // GEOQUAKE_PERF_PROBE
