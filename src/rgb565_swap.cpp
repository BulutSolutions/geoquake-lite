#include "rgb565_swap.h"

#include <lvgl.h>
#include <string.h>
#include "sdkconfig.h"

#if defined(CONFIG_IDF_TARGET_ESP32S3)
extern "C" {
void gq_swap16_pie_a(uint16_t* buf, uint32_t blocks);
void gq_swap16_pie_b(uint16_t* buf, uint32_t blocks);
void gq_swap16_pie_c(uint16_t* buf, uint32_t blocks);
void gq_swap16_pie_d(uint16_t* buf, uint32_t blocks);
}
#endif

namespace {

using SwapBlocksFn = void (*)(uint16_t*, uint32_t);

SwapBlocksFn s_pie = nullptr;
const char* s_impl = "scalar";
bool s_probed = false;

// Pick the PIE variant whose output matches the scalar swap exactly on a
// pattern with 64 distinct byte values (two blocks). None matching -> scalar.
void probe() {
    s_probed = true;
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    struct Candidate { SwapBlocksFn fn; const char* name; };
    static const Candidate kCandidates[] = {
        {gq_swap16_pie_a, "pie-a"},
        {gq_swap16_pie_b, "pie-b"},
        {gq_swap16_pie_c, "pie-c"},
        {gq_swap16_pie_d, "pie-d"},
    };
    alignas(16) uint8_t pattern[64];
    alignas(16) uint8_t expect[64];
    alignas(16) uint8_t test[64];
    for (int i = 0; i < 64; ++i) {
        pattern[i] = (uint8_t)(i * 37 + 11);
    }
    for (int i = 0; i < 64; i += 2) {
        expect[i] = pattern[i + 1];
        expect[i + 1] = pattern[i];
    }
    for (const Candidate& c : kCandidates) {
        memcpy(test, pattern, sizeof(test));
        c.fn(reinterpret_cast<uint16_t*>(test), 2);
        if (memcmp(test, expect, sizeof(test)) == 0) {
            s_pie = c.fn;
            s_impl = c.name;
            return;
        }
    }
#endif
}

}  // namespace

void rgb565_swap_inplace(uint16_t* buf, uint32_t px) {
    if (!s_probed) probe();

    if (s_pie && px >= 32) {
        // Scalar head up to 16-byte alignment (LVGL draw buffers are already aligned).
        while (px && (reinterpret_cast<uintptr_t>(buf) & 15u)) {
            *buf = (uint16_t)((*buf >> 8) | (*buf << 8));
            ++buf;
            --px;
        }
        const uint32_t blocks = px >> 4;  // 16 pixels per 32-byte block
        if (blocks) {
            s_pie(buf, blocks);
            buf += blocks << 4;
            px -= blocks << 4;
        }
    }
    if (px) {
        lv_draw_sw_rgb565_swap(buf, px);  // LVGL's scalar swap for the tail
    }
}

const char* rgb565_swap_impl() {
    return s_impl;
}
