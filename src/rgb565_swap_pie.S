// In-place byte swap of RGB565 pixels with ESP32-S3 PIE (128-bit SIMD).
//
//   void gq_swap16_pie_X(uint16_t* buf, uint32_t blocks)
//     a2 = buf     16-byte aligned (EE.VLD/VST.128 ignore the low 4 address bits)
//     a3 = blocks  number of 32-byte blocks (16 pixels each)
//
// Per block: load 2x128 bits, de-interleave bytes (even = low bytes, odd = high
// bytes), re-interleave with the halves exchanged, store 2x128 bits.
//
// The four variants cover the operand orders of EE.VUNZIP.8 / EE.VZIP.8 (which
// register receives the even bytes, which receives the low half). rgb565_swap.cpp
// selects, once, the variant whose output matches the scalar swap byte-for-byte
// and falls back to scalar if none does. Only the LVGL task may call these
// (single PIE user).

#include "sdkconfig.h"

#if defined(__XTENSA__) && defined(CONFIG_IDF_TARGET_ESP32S3)

    .text

    .align  4
    .global gq_swap16_pie_a
    .type   gq_swap16_pie_a, @function
gq_swap16_pie_a:
    entry   a1, 16
    mov     a4, a2
    loopnez a3, .La_end
    ee.vld.128.ip q0, a2, 16
    ee.vld.128.ip q1, a2, 16
    ee.vunzip.8   q0, q1
    ee.vzip.8     q1, q0
    ee.vst.128.ip q1, a4, 16
    ee.vst.128.ip q0, a4, 16
.La_end:
    retw
    .size   gq_swap16_pie_a, . - gq_swap16_pie_a

    .align  4
    .global gq_swap16_pie_b
    .type   gq_swap16_pie_b, @function
gq_swap16_pie_b:
    entry   a1, 16
    mov     a4, a2
    loopnez a3, .Lb_end
    ee.vld.128.ip q0, a2, 16
    ee.vld.128.ip q1, a2, 16
    ee.vunzip.8   q0, q1
    ee.vzip.8     q1, q0
    ee.vst.128.ip q0, a4, 16
    ee.vst.128.ip q1, a4, 16
.Lb_end:
    retw
    .size   gq_swap16_pie_b, . - gq_swap16_pie_b

    .align  4
    .global gq_swap16_pie_c
    .type   gq_swap16_pie_c, @function
gq_swap16_pie_c:
    entry   a1, 16
    mov     a4, a2
    loopnez a3, .Lc_end
    ee.vld.128.ip q0, a2, 16
    ee.vld.128.ip q1, a2, 16
    ee.vunzip.8   q0, q1
    ee.vzip.8     q0, q1
    ee.vst.128.ip q0, a4, 16
    ee.vst.128.ip q1, a4, 16
.Lc_end:
    retw
    .size   gq_swap16_pie_c, . - gq_swap16_pie_c

    .align  4
    .global gq_swap16_pie_d
    .type   gq_swap16_pie_d, @function
gq_swap16_pie_d:
    entry   a1, 16
    mov     a4, a2
    loopnez a3, .Ld_end
    ee.vld.128.ip q0, a2, 16
    ee.vld.128.ip q1, a2, 16
    ee.vunzip.8   q0, q1
    ee.vzip.8     q0, q1
    ee.vst.128.ip q1, a4, 16
    ee.vst.128.ip q0, a4, 16
.Ld_end:
    retw
    .size   gq_swap16_pie_d, . - gq_swap16_pie_d

#endif
