#pragma once

#include <Arduino.h>
#include <FS.h>

// Minimal QOI Decoder for RGB565 (Zero-Allocation)
// Optimized for ESP32-S3 with PSRAM direct write

#define QOI_OP_INDEX  0x00 /* 00xxxxxx */
#define QOI_OP_DIFF   0x40 /* 01xxxxxx */
#define QOI_OP_LUMA   0x80 /* 10xxxxxx */
#define QOI_OP_RUN    0xc0 /* 11xxxxxx */
#define QOI_OP_RGB    0xfe /* 11111110 */
#define QOI_OP_RGBA   0xff /* 11111111 */

#define QOI_MASK_2    0xc0 /* 11000000 */
#define QOI_HEADER_SIZE 14
#define QOI_END_MARKER_SIZE 8
#define QOI_MIN_DATA_SIZE (QOI_HEADER_SIZE + QOI_END_MARKER_SIZE)

typedef struct {
    uint8_t r, g, b, a;
} qoi_rgba_t;

// RGB888 → RGB565 little-endian (native LVGL format)
#define RGB565_LE(r, g, b) \
    ((uint16_t)(((r) & 0xF8) << 8) | (uint16_t)(((g) & 0xFC) << 3) | (uint16_t)((b) >> 3))

// Decode QOI from pre-loaded memory buffer (no heap alloc, reentrant)
// Returns 1 on success, 0 on failure
// data/data_len: raw QOI file contents; output_buffer: must be width*height*2 bytes
static inline int __attribute__((hot)) qoi_decode_from_memory(
    const uint8_t* data, size_t data_len, uint8_t* output_buffer,
    int width, int height)
{
    if (!data || !output_buffer || data_len < QOI_MIN_DATA_SIZE) return 0;

    // Verify magic 'qoif'
    if (data[0] != 'q' || data[1] != 'o' || data[2] != 'i' || data[3] != 'f') return 0;

    // Validate header dimensions
    uint32_t hdr_w = ((uint32_t)data[4] << 24) | ((uint32_t)data[5] << 16) | ((uint32_t)data[6] << 8) | data[7];
    uint32_t hdr_h = ((uint32_t)data[8] << 24) | ((uint32_t)data[9] << 16) | ((uint32_t)data[10] << 8) | data[11];
    uint8_t  hdr_channels = data[12];
    uint8_t  hdr_colorspace = data[13];

    if ((int)hdr_w != width || (int)hdr_h != height) return 0;
    if (hdr_channels < 3 || hdr_channels > 4) return 0;
    if (hdr_colorspace > 1) return 0;

    const uint32_t total_pixels = width * height;
    uint32_t pixel_idx = 0;

    // Color index in DRAM for fast access (256 bytes fits in internal SRAM cache)
    qoi_rgba_t index[64];
    memset(index, 0, sizeof(index));

    qoi_rgba_t px = {0, 0, 0, 255};

    int run = 0;
    size_t pos = QOI_HEADER_SIZE;  // Skip header
    const size_t end = (data_len > QOI_END_MARKER_SIZE) ? data_len - QOI_END_MARKER_SIZE : data_len;

    uint16_t* dst = static_cast<uint16_t*>(static_cast<void*>(output_buffer));

    while (pixel_idx < total_pixels) {
        if (run > 0) {
            uint16_t c = RGB565_LE(px.r, px.g, px.b);
            uint32_t batch = run + 1;
            if (pixel_idx + batch > total_pixels) batch = total_pixels - pixel_idx;
            const uint32_t batch_total = batch;

            if ((((uintptr_t)dst) & 0x3U) && batch > 0) {
                *dst++ = c;
                batch--;
            }

            uint32_t c32 = ((uint32_t)c << 16) | c;
            uint32_t* dst32 = reinterpret_cast<uint32_t*>(dst);
            uint32_t pairs = batch / 2;
            for (uint32_t i = 0; i < pairs; i++) *dst32++ = c32;
            dst = reinterpret_cast<uint16_t*>(dst32);
            if (batch & 1) *dst++ = c;

            pixel_idx += batch_total;
            run = 0;
            continue;
        }

        if (pos >= end) break;
        uint8_t b1 = data[pos++];

        switch (b1) {
        case QOI_OP_RGB:
            if (pos + 3 > end) goto done;
            px.r = data[pos++];
            px.g = data[pos++];
            px.b = data[pos++];
            break;

        case QOI_OP_RGBA:
            if (pos + 4 > end) goto done;
            px.r = data[pos++];
            px.g = data[pos++];
            px.b = data[pos++];
            px.a = data[pos++];
            break;

        default: {
            uint8_t op = b1 & QOI_MASK_2;
            switch (op) {
            case QOI_OP_INDEX:
                px = index[b1];
                break;

            case QOI_OP_DIFF:
                px.r += ((b1 >> 4) & 0x03) - 2;
                px.g += ((b1 >> 2) & 0x03) - 2;
                px.b += ( b1       & 0x03) - 2;
                break;

            case QOI_OP_LUMA: {
                if (pos >= end) goto done;
                uint8_t b2_val = data[pos++];
                int vg = (b1 & 0x3f) - 32;
                px.g += vg;
                px.r += vg - 8 + ((b2_val >> 4) & 0x0f);
                px.b += vg - 8 +  (b2_val       & 0x0f);
                break;
            }

            case QOI_OP_RUN:
                run = (b1 & 0x3f);
                break;
            }
            break;
        }
        }

        index[(px.r * 3 + px.g * 5 + px.b * 7 + px.a * 11) % 64] = px;

        if (run == 0) {
            *dst++ = RGB565_LE(px.r, px.g, px.b);
            pixel_idx++;
        }
    }

done:
    if (pixel_idx != total_pixels) return 0;

    if (data_len < QOI_END_MARKER_SIZE) return 0;
    static const uint8_t qoi_end[8] = {0, 0, 0, 0, 0, 0, 0, 1};
    if (memcmp(data + (data_len - 8), qoi_end, 8) != 0) return 0;

    return 1;
}
