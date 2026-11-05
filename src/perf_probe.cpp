#include "perf_probe.h"

#if defined(GEOQUAKE_PERF_PROBE) && GEOQUAKE_PERF_PROBE

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <string.h>

namespace {

struct Probe {
    uint32_t* ring;               // PSRAM samples, nullptr -> probe disabled
    uint32_t count;               // writer-owned: samples since last reset
    uint64_t sum;                 // writer-owned
    uint32_t max;                 // writer-owned
    uint32_t epoch;               // writer-owned: last reset request honoured
    volatile uint32_t reset_req;  // reader-owned: bumped by perf_probe_reset_all()
};

Probe s_probe[PP_COUNT];
uint32_t s_scratch[PERF_PROBE_RING];  // sorted copy, report time only
uint32_t s_cpu_mhz = 240;
bool s_init_done = false;

const char* const kProbeNames[PP_COUNT] = {
    "PAN_BY",     "TILE_POS",   "REFR",       "RENDER",     "SWAP",
    "FLUSH_CB",   "FLUSH_WAIT", "DOTS",       "DOTS_PROJ",  "DECODE_T0",
    "DECODE_T1",  "ACQUIRE_T0", "ACQUIRE_T1", "VECTOR",     "FRAME_INTERVAL",
};

// Per-frame accumulators fed by the LVGL display events and the flush callback.
// Everything here is touched by the LVGL task only.
struct FrameAcc {
    uint32_t refr_start;
    uint32_t flush_start;
    uint32_t cb_enter;
    uint32_t wait_start;
    uint32_t swap_acc;
    uint32_t cb_acc;
    uint32_t wait_acc;
    uint32_t strips;
    uint32_t prev_refr_start;
    bool have_prev;
    uint32_t frames;
};
FrameAcc s_fr;

// LVGL 9.5 sends REFR_START/REFR_READY on every refresh timer run, even when nothing is
// redrawn, so a cycle only counts as a frame when at least one strip reached flush_cb.
void perf_disp_event_cb(lv_event_t* e) {
    const uint32_t now = perf_cycles();
    switch (lv_event_get_code(e)) {
        case LV_EVENT_REFR_START:
            s_fr.refr_start = now;
            s_fr.swap_acc = 0;
            s_fr.cb_acc = 0;
            s_fr.wait_acc = 0;
            s_fr.strips = 0;
            break;
        case LV_EVENT_FLUSH_START:
            s_fr.flush_start = now;
            s_fr.strips++;
            break;
        case LV_EVENT_FLUSH_WAIT_START:
            s_fr.wait_start = now;
            break;
        case LV_EVENT_FLUSH_WAIT_FINISH:
            s_fr.wait_acc += now - s_fr.wait_start;
            break;
        case LV_EVENT_REFR_READY:
            if (s_fr.strips != 0) {
                const uint32_t refr = now - s_fr.refr_start;
                const uint32_t other = s_fr.swap_acc + s_fr.cb_acc + s_fr.wait_acc;
                perf_probe_record(PP_REFR, refr);
                perf_probe_record(PP_RENDER, refr > other ? refr - other : 0);
                perf_probe_record(PP_SWAP, s_fr.swap_acc);
                perf_probe_record(PP_FLUSH_CB, s_fr.cb_acc);
                perf_probe_record(PP_FLUSH_WAIT, s_fr.wait_acc);
                if (s_fr.have_prev) {
                    perf_probe_record(PP_FRAME_INTERVAL, s_fr.refr_start - s_fr.prev_refr_start);
                }
                s_fr.prev_refr_start = s_fr.refr_start;
                s_fr.have_prev = true;
                s_fr.frames++;
            }
            break;
        default:
            break;
    }
}

}  // namespace

void perf_probe_init() {
    if (s_init_done) return;
    s_init_done = true;

    const size_t bytes = sizeof(uint32_t) * PERF_PROBE_RING * PP_COUNT;
    uint32_t* block = static_cast<uint32_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
    if (!block) {
        Serial.println("[PERF_PROBE] PSRAM ring allocation failed, probes disabled");
        return;
    }
    memset(block, 0, bytes);
    for (int i = 0; i < PP_COUNT; ++i) {
        s_probe[i].ring = block + static_cast<size_t>(i) * PERF_PROBE_RING;
    }
    Serial.printf("[PERF_PROBE] %d probes x %d samples in PSRAM (%u B)\n",
                  static_cast<int>(PP_COUNT), PERF_PROBE_RING, static_cast<unsigned>(bytes));
}

void perf_probe_record(PerfProbeId id, uint32_t cycles) {
    Probe& p = s_probe[id];
    uint32_t* ring = p.ring;
    if (!ring) return;
    if (p.epoch != p.reset_req) {  // reset requested by the reader; honour it here
        p.epoch = p.reset_req;
        p.count = 0;
        p.sum = 0;
        p.max = 0;
    }
    const uint32_t n = p.count;
    ring[n & (PERF_PROBE_RING - 1)] = cycles;
    p.count = n + 1;
    p.sum += cycles;
    if (cycles > p.max) p.max = cycles;
}

void perf_probe_reset_all() {
    for (int i = 0; i < PP_COUNT; ++i) {
        s_probe[i].reset_req = s_probe[i].reset_req + 1u;
    }
    s_fr.have_prev = false;  // no interval across the reset gap
}

bool perf_probe_report(PerfProbeId id, PerfProbeStats* out) {
    memset(out, 0, sizeof(*out));
    const Probe& p = s_probe[id];
    if (!p.ring || p.epoch != p.reset_req || p.count == 0) return false;

    const uint32_t n = p.count;
    const uint32_t win = n < PERF_PROBE_RING ? n : PERF_PROBE_RING;
    for (uint32_t i = 0; i < win; ++i) {
        s_scratch[i] = p.ring[(n - win + i) & (PERF_PROBE_RING - 1)];
    }
    for (uint32_t i = 1; i < win; ++i) {  // insertion sort, report time only
        const uint32_t v = s_scratch[i];
        uint32_t j = i;
        while (j > 0 && s_scratch[j - 1] > v) {
            s_scratch[j] = s_scratch[j - 1];
            --j;
        }
        s_scratch[j] = v;
    }

    out->n = n;
    out->win = win;
    out->mean = static_cast<uint32_t>((p.sum + n / 2) / n);
    out->p50 = s_scratch[(win * 50 + 99) / 100 - 1];  // nearest rank
    out->p95 = s_scratch[(win * 95 + 99) / 100 - 1];
    out->max = p.max;
    return true;
}

const char* perf_probe_name(PerfProbeId id) {
    return static_cast<unsigned>(id) < static_cast<unsigned>(PP_COUNT) ? kProbeNames[id] : "?";
}

void perf_probe_set_cpu_mhz(uint32_t mhz) {
    s_cpu_mhz = mhz ? mhz : 240;
}

uint32_t perf_probe_cpu_mhz() {
    return s_cpu_mhz;
}

void perf_probe_attach_display(lv_display_t* disp) {
    lv_display_add_event_cb(disp, perf_disp_event_cb, LV_EVENT_REFR_START, nullptr);
    lv_display_add_event_cb(disp, perf_disp_event_cb, LV_EVENT_REFR_READY, nullptr);
    lv_display_add_event_cb(disp, perf_disp_event_cb, LV_EVENT_FLUSH_START, nullptr);
    lv_display_add_event_cb(disp, perf_disp_event_cb, LV_EVENT_FLUSH_WAIT_START, nullptr);
    lv_display_add_event_cb(disp, perf_disp_event_cb, LV_EVENT_FLUSH_WAIT_FINISH, nullptr);
}

// LVGL 9.5 call_flush_cb(): FLUSH_START -> RGB565 byte swap (LV_COLOR_16_SWAP) -> flush_cb,
// so the gap between the FLUSH_START event and flush_cb entry is the swap.
void perf_flush_cb_enter() {
    const uint32_t now = perf_cycles();
    s_fr.swap_acc += now - s_fr.flush_start;
    s_fr.cb_enter = now;
}

void perf_flush_cb_exit() {
    s_fr.cb_acc += perf_cycles() - s_fr.cb_enter;
}

void perf_flush_swap_inside_cb(uint32_t swap_cycles) {
    s_fr.swap_acc += swap_cycles;
    s_fr.cb_enter += swap_cycles;  // exclude it from this strip's FLUSH_CB time
}

uint32_t perf_frame_count() {
    return s_fr.frames;
}

#endif  // GEOQUAKE_PERF_PROBE
