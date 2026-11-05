#pragma once

#include <stdint.h>

// In-place RGB565 byte swap to the panel's big-endian order, done at the top
// of flush_cb (replaces LVGL's LV_COLOR_16_SWAP backward-compat pass in
// lv_refr.c call_flush_cb; identical bytes on the wire).
//
// ESP32-S3 PIE path for 16-byte-aligned 32-byte blocks, scalar head/tail.
// LVGL task only: it is the single user of the PIE registers.
void rgb565_swap_inplace(uint16_t* buf, uint32_t px);

// Which implementation is active ("pie-a".."pie-d" or "scalar"); valid after
// the first rgb565_swap_inplace() call, which runs the one-time self-test.
const char* rgb565_swap_impl();
