#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Minimal QOI Decoder for RGB565
// Optimized for ESP32 PSRAM direct write

#define QOI_OP_INDEX  0x00 /* 00xxxxxx */
#define QOI_OP_DIFF   0x40 /* 01xxxxxx */
#define QOI_OP_LUMA   0x80 /* 10xxxxxx */
#define QOI_OP_RUN    0xc0 /* 11xxxxxx */
#define QOI_OP_RGB    0xfe /* 11111110 */
#define QOI_OP_RGBA   0xff /* 11111111 */

#define QOI_MASK_2    0xc0 /* 11000000 */

typedef struct {
    uint8_t r, g, b, a;
} qoi_rgba_t;

typedef uint32_t __attribute__((may_alias)) qoi_u32_alias_t;

// Write n copies of one RGB565 pixel. Runs are the common case on map tiles
// (ocean, land fill), so pair them into 32-bit stores once aligned.
static inline uint16_t* qoi_fill_rgb565(uint16_t* dst, uint16_t c, uint32_t n) {
    if (n >= 4) {
        if (reinterpret_cast<uintptr_t>(dst) & 2u) {
            *dst++ = c;
            --n;
        }
        const uint32_t c2 = ((uint32_t)c << 16) | c;
        qoi_u32_alias_t* d32 = reinterpret_cast<qoi_u32_alias_t*>(dst);
        for (uint32_t i = n >> 1; i != 0; --i) {
            *d32++ = c2;
        }
        dst = reinterpret_cast<uint16_t*>(d32);
        n &= 1u;
    }
    while (n--) {
        *dst++ = c;
    }
    return dst;
}

// Decode QOI from pre-loaded memory buffer (no file I/O, reentrant, parallel-safe)
// Returns 1 on success, 0 on failure. On success every one of width*height
// pixels has been written; on failure the output is unspecified.
// data/data_len: raw QOI file contents; output_buffer: must be width*height*2 bytes
static int qoi_decode_from_memory(const uint8_t* data, size_t data_len, uint8_t* output_buffer, int width, int height, bool swap_bytes) {
    if (!data || !output_buffer || data_len < 22) return 0;  // 14 header + 8 end marker minimum

    // Verify magic 'qoif'
    if (data[0] != 'q' || data[1] != 'o' || data[2] != 'i' || data[3] != 'f') return 0;

    // Validate header dimensions and channels match expectations
    uint32_t hdr_w = ((uint32_t)data[4] << 24) | ((uint32_t)data[5] << 16) | ((uint32_t)data[6] << 8) | data[7];
    uint32_t hdr_h = ((uint32_t)data[8] << 24) | ((uint32_t)data[9] << 16) | ((uint32_t)data[10] << 8) | data[11];
    uint8_t  hdr_channels = data[12];   // 3=RGB, 4=RGBA
    uint8_t  hdr_colorspace = data[13]; // 0=sRGB, 1=linear

    if ((int)hdr_w != width || (int)hdr_h != height) return 0;
    if (hdr_channels < 3 || hdr_channels > 4) return 0;
    if (hdr_colorspace > 1) return 0;

    const uint32_t total_pixels = (uint32_t)width * (uint32_t)height;
    uint32_t pixel_idx = 0;

    qoi_rgba_t index[64];
    memset(index, 0, sizeof(index));

    qoi_rgba_t px = {0, 0, 0, 255};

    size_t pos = 14;  // Skip header
    // All bounds checks use 'end' to avoid reading into the 8-byte QOI end marker
    size_t end = (data_len > 8) ? data_len - 8 : data_len;

    uint16_t* dst = (uint16_t*)output_buffer;

    while (pixel_idx < total_pixels) {
        if (pos >= end) break;
        uint8_t b1 = data[pos++];
        uint32_t count = 1;  // pixels produced by this chunk

        if (b1 == QOI_OP_RGB) {
            if (pos + 3 > end) break;
            px.r = data[pos++];
            px.g = data[pos++];
            px.b = data[pos++];
        }
        else if (b1 == QOI_OP_RGBA) {
            if (pos + 4 > end) break;
            px.r = data[pos++];
            px.g = data[pos++];
            px.b = data[pos++];
            px.a = data[pos++];
        }
        else if ((b1 & QOI_MASK_2) == QOI_OP_INDEX) {
            px = index[b1];
        }
        else if ((b1 & QOI_MASK_2) == QOI_OP_DIFF) {
            px.r += ((b1 >> 4) & 0x03) - 2;
            px.g += ((b1 >> 2) & 0x03) - 2;
            px.b += ( b1       & 0x03) - 2;
        }
        else if ((b1 & QOI_MASK_2) == QOI_OP_LUMA) {
            if (pos >= end) break;
            uint8_t b2_val = data[pos++];
            int vg = (b1 & 0x3f) - 32;
            px.g += vg;
            px.r += vg - 8 + ((b2_val >> 4) & 0x0f);
            px.b += vg - 8 +  (b2_val       & 0x0f);
        }
        else if ((b1 & QOI_MASK_2) == QOI_OP_RUN) {
            count = (uint32_t)(b1 & 0x3f) + 1u;  // this pixel plus the run
        }

        index[(px.r * 3 + px.g * 5 + px.b * 7 + px.a * 11) % 64] = px;

        // Convert to RGB565 and write
        uint16_t c = ((px.r & 0xF8) << 8) | ((px.g & 0xFC) << 3) | (px.b >> 3);
        if (swap_bytes) c = (c >> 8) | (c << 8);

        // A run may not write past the image (malformed or hostile input).
        const uint32_t remaining = total_pixels - pixel_idx;
        if (count > remaining) count = remaining;
        dst = qoi_fill_rgb565(dst, c, count);
        pixel_idx += count;
    }

    if (pixel_idx != total_pixels) return 0;

    // Validate 8-byte QOI end marker: {0,0,0,0,0,0,0,1}
    // Catches truncated/corrupt files that happen to produce the right pixel count
    if (data_len < 8) return 0;
    static const uint8_t qoi_end[8] = {0, 0, 0, 0, 0, 0, 0, 1};
    if (memcmp(data + (data_len - 8), qoi_end, 8) != 0) return 0;

    return 1;
}
