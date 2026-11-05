#include "gq_lv_profiler.h"

#if GEOQUAKE_LV_PROFILE

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdlib.h>
#include "perf_probe.h"

namespace {

constexpr int kSlots = 256;  // open-addressed by tag pointer, power of two
constexpr int kDepth = 64;

struct Entry {
    const char* tag;
    uint32_t calls;
    uint64_t incl;  // cycles including callees
    uint64_t self;  // cycles minus profiled callees
};
struct Frame {
    const char* tag;
    uint32_t t0;
    uint32_t child;
};

Entry s_tab[kSlots];
Frame s_stack[kDepth];
int s_sp = 0;
uint32_t s_skipped = 0;  // begins dropped at full depth; their ends are dropped too
uint32_t s_mismatch = 0;
uint32_t s_full = 0;
uint32_t s_t_start = 0;
uint32_t s_t_total = 0;
TaskHandle_t s_task = nullptr;
volatile bool s_on = false;

Entry* slot(const char* tag) {
    uint32_t h = (static_cast<uint32_t>(reinterpret_cast<uintptr_t>(tag)) >> 2) * 2654435761u;
    for (int i = 0; i < kSlots; ++i) {
        Entry& e = s_tab[(h + i) & (kSlots - 1)];
        if (e.tag == tag) return &e;
        if (e.tag == nullptr) {
            e.tag = tag;
            return &e;
        }
    }
    ++s_full;
    return nullptr;
}

}  // namespace

extern "C" void gq_prof_begin(const char* tag) {
    if (!s_on || xTaskGetCurrentTaskHandle() != s_task) return;
    if (s_sp >= kDepth) {
        ++s_skipped;
        return;
    }
    s_stack[s_sp++] = Frame{tag, perf_cycles(), 0};
}

extern "C" void gq_prof_end(const char* tag) {
    const uint32_t now = perf_cycles();
    if (!s_on || xTaskGetCurrentTaskHandle() != s_task) return;
    if (s_skipped > 0) {
        --s_skipped;
        return;
    }
    if (s_sp == 0) {
        ++s_mismatch;
        return;
    }
    const Frame f = s_stack[--s_sp];
    if (f.tag != tag) ++s_mismatch;  // still charged to the begin's tag
    const uint32_t elapsed = now - f.t0;
    if (Entry* e = slot(f.tag)) {
        e->calls++;
        e->incl += elapsed;
        e->self += elapsed - f.child;
    }
    if (s_sp > 0) s_stack[s_sp - 1].child += elapsed;
}

extern "C" void gq_prof_start(void) {
    s_on = false;
    memset(s_tab, 0, sizeof(s_tab));
    s_sp = 0;
    s_skipped = s_mismatch = s_full = 0;
    s_task = xTaskGetCurrentTaskHandle();
    s_t_start = perf_cycles();
    s_on = true;
}

extern "C" void gq_prof_stop(void) {
    s_on = false;
    s_t_total = perf_cycles() - s_t_start;
}

extern "C" void gq_prof_report(void (*out)(const char* fmt, ...), const char* variant, int frames) {
    static int order[kSlots];
    int n = 0;
    for (int i = 0; i < kSlots; ++i) {
        if (s_tab[i].tag) order[n++] = i;
    }
    // Insertion sort by self cycles, descending (n <= 256, once per run)
    for (int i = 1; i < n; ++i) {
        const int v = order[i];
        int j = i - 1;
        while (j >= 0 && s_tab[order[j]].self < s_tab[v].self) {
            order[j + 1] = order[j];
            --j;
        }
        order[j + 1] = v;
    }
    const float mhz = static_cast<float>(perf_probe_cpu_mhz());
    const float per_frame = frames > 0 ? 1.0f / static_cast<float>(frames) : 1.0f;
    out("PROBE_INFO,variant=%s,frames=%d,window_ms=%.2f,tags=%d,mismatch=%u,depth_skipped=%u,table_full=%u\n",
        variant, frames, s_t_total / mhz / 1000.0f, n, static_cast<unsigned>(s_mismatch),
        static_cast<unsigned>(s_skipped), static_cast<unsigned>(s_full));
    for (int k = 0; k < n && k < 60; ++k) {
        const Entry& e = s_tab[order[k]];
        out("PROBE_PROF,variant=%s,fn=%s,calls_per_frame=%.1f,self_ms_per_frame=%.3f,"
            "incl_ms_per_frame=%.3f,self_us_per_call=%.2f\n",
            variant, e.tag, e.calls * per_frame, e.self / mhz / 1000.0f * per_frame,
            e.incl / mhz / 1000.0f * per_frame, e.calls ? e.self / mhz / e.calls : 0.0f);
    }
}

#endif  // GEOQUAKE_LV_PROFILE
