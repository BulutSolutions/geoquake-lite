// QOI decoder equivalence: the run-fast-path decoder (src/qoi_decoder.h)
// against the pre-change decoder (verbatim below as legacy_qoi_decode).
// Real tiles must decode byte-identically; malformed input must give the same
// result code and never write past the output buffer.

#include <unity.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <chrono>
#include <set>
#include <utility>
#include "../src/qoi_decoder.h"  // the decoder the firmware's tile manager uses
#include "../src/tile_pak_format.h"

#ifndef GQ_PROJECT_DIR
#define GQ_PROJECT_DIR "."
#endif

namespace {

// --- verbatim pre-change decoder (qoi_decoder.h qoi_decode_from_memory) -------
int legacy_qoi_decode(const uint8_t* data, size_t data_len, uint8_t* output_buffer, int width, int height, bool swap_bytes) {
    if (!data || !output_buffer || data_len < 22) return 0;
    if (data[0] != 'q' || data[1] != 'o' || data[2] != 'i' || data[3] != 'f') return 0;
    uint32_t hdr_w = ((uint32_t)data[4] << 24) | ((uint32_t)data[5] << 16) | ((uint32_t)data[6] << 8) | data[7];
    uint32_t hdr_h = ((uint32_t)data[8] << 24) | ((uint32_t)data[9] << 16) | ((uint32_t)data[10] << 8) | data[11];
    uint8_t  hdr_channels = data[12];
    uint8_t  hdr_colorspace = data[13];
    if ((int)hdr_w != width || (int)hdr_h != height) return 0;
    if (hdr_channels < 3 || hdr_channels > 4) return 0;
    if (hdr_colorspace > 1) return 0;
    uint32_t total_pixels = width * height;
    uint32_t pixel_idx = 0;
    qoi_rgba_t index[64];
    memset(index, 0, sizeof(index));
    qoi_rgba_t px = {0, 0, 0, 255};
    int run = 0;
    size_t pos = 14;
    size_t end = (data_len > 8) ? data_len - 8 : data_len;
    uint16_t* dst = (uint16_t*)output_buffer;
    while (pixel_idx < total_pixels) {
        if (run > 0) {
            run--;
        } else {
            if (pos >= end) break;
            uint8_t b1 = data[pos++];
            if (b1 == QOI_OP_RGB) {
                if (pos + 3 > end) break;
                px.r = data[pos++]; px.g = data[pos++]; px.b = data[pos++];
            } else if (b1 == QOI_OP_RGBA) {
                if (pos + 4 > end) break;
                px.r = data[pos++]; px.g = data[pos++]; px.b = data[pos++]; px.a = data[pos++];
            } else if ((b1 & QOI_MASK_2) == QOI_OP_INDEX) {
                px = index[b1];
            } else if ((b1 & QOI_MASK_2) == QOI_OP_DIFF) {
                px.r += ((b1 >> 4) & 0x03) - 2;
                px.g += ((b1 >> 2) & 0x03) - 2;
                px.b += ( b1       & 0x03) - 2;
            } else if ((b1 & QOI_MASK_2) == QOI_OP_LUMA) {
                if (pos >= end) break;
                uint8_t b2_val = data[pos++];
                int vg = (b1 & 0x3f) - 32;
                px.g += vg;
                px.r += vg - 8 + ((b2_val >> 4) & 0x0f);
                px.b += vg - 8 +  (b2_val       & 0x0f);
            } else if ((b1 & QOI_MASK_2) == QOI_OP_RUN) {
                run = (b1 & 0x3f);
            }
            index[(px.r * 3 + px.g * 5 + px.b * 7 + px.a * 11) % 64] = px;
        }
        uint16_t c = ((px.r & 0xF8) << 8) | ((px.g & 0xFC) << 3) | (px.b >> 3);
        if (swap_bytes) c = (c >> 8) | (c << 8);
        *dst++ = c;
        pixel_idx++;
    }
    if (pixel_idx != total_pixels) return 0;
    if (data_len < 8) return 0;
    static const uint8_t qoi_end[8] = {0, 0, 0, 0, 0, 0, 0, 1};
    if (memcmp(data + (data_len - 8), qoi_end, 8) != 0) return 0;
    return 1;
}

constexpr int kW = 256;
constexpr int kH = 256;
constexpr size_t kOut = (size_t)kW * kH * 2;
constexpr size_t kGuard = 64;

struct Buf {
    uint8_t* p = nullptr;
    size_t n = 0;
};

Buf read_file(const char* path) {
    Buf b;
    FILE* f = fopen(path, "rb");
    if (!f) return b;
    fseek(f, 0, SEEK_END);
    b.n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    b.p = (uint8_t*)malloc(b.n ? b.n : 1);
    if (fread(b.p, 1, b.n, f) != b.n) b.n = 0;
    fclose(f);
    return b;
}

// Decode with both; return false (and report) on any divergence.
bool compare_one(const uint8_t* data, size_t len, const char* label, uint8_t* out_old, uint8_t* out_new) {
    memset(out_old, 0xA5, kOut + kGuard);
    memset(out_new, 0xA5, kOut + kGuard);
    const int r_old = legacy_qoi_decode(data, len, out_old, kW, kH, false);
    const int r_new = qoi_decode_from_memory(data, len, out_new, kW, kH, false);
    for (size_t i = kOut; i < kOut + kGuard; ++i) {
        if (out_new[i] != 0xA5) {
            printf("\n[QOI] %s: new decoder wrote past the buffer at +%zu\n", label, i - kOut);
            return false;
        }
    }
    if (r_old != r_new) {
        printf("\n[QOI] %s: result differs (legacy %d, new %d)\n", label, r_old, r_new);
        return false;
    }
    if (r_new && memcmp(out_old, out_new, kOut) != 0) {
        printf("\n[QOI] %s: pixels differ\n", label);
        return false;
    }
    return true;
}

}  // namespace

void test_qoi_equivalence_real_and_malformed() {
    const char* pack_path = GQ_PROJECT_DIR "/data/GeoQuakeLite/tiles/core.pak";
    Buf pack = read_file(pack_path);
    TEST_ASSERT_NOT_NULL_MESSAGE(pack.p, pack_path);

    static tile_pak::Index index;  // 43.7 KB: off the stack
    const tile_pak::Result parsed = tile_pak::parse(pack.p, pack.n, static_cast<uint32_t>(pack.n), &index);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("ok", tile_pak::result_name(parsed), pack_path);

    // Every distinct payload once: the pack stores identical tiles (plain sea, plain land) a single time.
    const int slots = static_cast<int>(tile_pak::tile_count(index.levels));
    std::set<std::pair<uint32_t, uint32_t>> seen;
    int count = 0;
    uint8_t* out_old = (uint8_t*)malloc(kOut + kGuard);
    uint8_t* out_new = (uint8_t*)malloc(kOut + kGuard);
    TEST_ASSERT_NOT_NULL(out_old);
    TEST_ASSERT_NOT_NULL(out_new);
    uint32_t lcg = 12345u;
    auto rnd = [&lcg]() { lcg = lcg * 1664525u + 1013904223u; return lcg >> 8; };

    int real_ok = 0, real_decoded = 0, malformed = 0, failures = 0;
    double t_old = 0.0, t_new = 0.0;
    for (int slot = 0; slot < slots; ++slot) {
        const tile_pak::Entry& e = index.entry[slot];
        if (!seen.insert({e.offset, e.length}).second) continue;
        const int i = count++;  // index among the distinct payloads
        const uint8_t* data = pack.p + e.offset;
        const size_t len = e.length;
        char label[48];
        snprintf(label, sizeof(label), "core.pak slot %d", slot);
        if (compare_one(data, len, label, out_old, out_new)) ++real_ok; else ++failures;
        if (legacy_qoi_decode(data, len, out_old, kW, kH, false)) {
            ++real_decoded;
            auto a = std::chrono::steady_clock::now();
            legacy_qoi_decode(data, len, out_old, kW, kH, false);
            auto m = std::chrono::steady_clock::now();
            qoi_decode_from_memory(data, len, out_new, kW, kH, false);
            auto e_time = std::chrono::steady_clock::now();
            t_old += std::chrono::duration<double, std::micro>(m - a).count();
            t_new += std::chrono::duration<double, std::micro>(e_time - m).count();
        }
        if (i % 10 == 0 && len > 40) {
            uint8_t* m = (uint8_t*)malloc(len + 4096);
            // (a) truncation
            size_t cut = 14 + rnd() % (len - 14);
            memcpy(m, data, cut);
            ++malformed; if (!compare_one(m, cut, "truncated", out_old, out_new)) ++failures;
            // (b) byte flips in the chunk stream
            memcpy(m, data, len);
            for (int k = 0; k < 5; ++k) m[14 + rnd() % (len - 22)] ^= (uint8_t)(1u << (rnd() % 8));
            ++malformed; if (!compare_one(m, len, "flipped", out_old, out_new)) ++failures;
            // (c) oversized runs: 4096 max runs (> 65536 pixels) then a valid end marker
            memcpy(m, data, 14);
            size_t p = 14;
            for (int k = 0; k < 4096 && p < len + 4096 - 8; ++k) m[p++] = 0xFD;  // run of 62
            static const uint8_t endm[8] = {0, 0, 0, 0, 0, 0, 0, 1};
            memcpy(m + p, endm, 8);
            ++malformed; if (!compare_one(m, p + 8, "oversized-run", out_old, out_new)) ++failures;
            free(m);
        }
    }
    free(out_old);
    free(out_new);
    free(pack.p);
    printf("[QOI] real tiles: %d/%d identical (%d decodable), malformed cases: %d, failures: %d\n",
           real_ok, count, real_decoded, malformed, failures);
    printf("[QOI] host decode time (all decodable tiles): legacy %.1f ms, new %.1f ms (%.2fx)\n",
           t_old / 1000.0, t_new / 1000.0, t_new > 0 ? t_old / t_new : 0.0);
    TEST_ASSERT_EQUAL(count, real_decoded);
    TEST_ASSERT_EQUAL(0, failures);
}

void run_qoi_equivalence_tests() {
    RUN_TEST(test_qoi_equivalence_real_and_malformed);
}
