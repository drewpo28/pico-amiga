// Word format of the PIO TMDS back-end (boards whose HDMI is not on the HSTX
// pins, e.g. PICO_DV on GPIO 6-13).  Pure C so host/tests/tmds_test.c checks
// the very tables the firmware uses.
//
// The PIO program (tmds.pio) shifts out 6 data pins per TMDS bit, LSB first,
// 5 bits per 32-bit FIFO word (autopull at 30), so one output pixel = 2 words:
//   word 0 bits 6t..6t+5 = pins at bit time t   (t = 0..4)
//   word 1 bits 6t..6t+5 = pins at bit time t+5
// The clock pair is side-set (high for the first 5 bits, low for the last 5).
//
// Pin order inside the 6 data bits is pico-speccy's for its non-ZERO2 boards
// (hdmi.h / hdmi_word.h: HDMI_PIN_RGB_notBGR = 1, invert_diffpairs = 1):
//   lane 0 (blue, carries sync)  bits 0/1     lane 1 (green) bits 2/3
//   lane 2 (red)                 bits 4/5
// and every pair is wired N/P: the first pin carries the inverted bit.
#pragma once
#include <stdint.h>
#include "tmds_pair.h"

#define TMDS_CTRL_00 0x354u
#define TMDS_CTRL_01 0x0abu
#define TMDS_CTRL_10 0x154u
#define TMDS_CTRL_11 0x2abu

static const int tmds_lane_shift[3] = { 0, 2, 4 };

// The pin bits one lane contributes to the two words of one output pixel.
static inline void tmds_lane_words(int lane, uint16_t sym, uint32_t w[2]) {
    w[0] = w[1] = 0;
    for (int t = 0; t < 10; t++) {
        uint32_t b = (sym >> t) & 1u;
        uint32_t pair = (b << 1) | (b ^ 1u);          // first pin inverted
        w[t / 5] |= pair << (6 * (t % 5) + tmds_lane_shift[lane]);
    }
}

// Pixel-doubled colour tables.  Each source pixel becomes two output pixels =
// 4 words: [px0 w0, px0 w1, px1 w0, px1 w1].  tab_r covers lane 2 for a red
// nibble; tab_gb covers lanes 1 and 0 for a (green << 4 | blue) byte.
// Nibbles are expanded to 8 bits (n * 17) and kept inside 0x08..0xF6, the
// level clamp pico-speccy found to steady marginal receivers.
static inline uint8_t tmds_level(int nibble) {
    int v = nibble * 17;
    return (uint8_t)(v < 0x08 ? 0x08 : v > 0xF6 ? 0xF6 : v);
}

static inline void tmds_pair_words(int lane, uint8_t v, uint32_t out[4]) {
    uint16_t a, b;
    tmds_balanced_pair(v, &a, &b);
    tmds_lane_words(lane, a, &out[0]);
    tmds_lane_words(lane, b, &out[2]);
}

static inline void tmds_build_tables(uint32_t tab_r[16][4], uint32_t tab_gb[256][4]) {
    uint32_t g[16][4], bl[16][4];
    for (int n = 0; n < 16; n++) {
        tmds_pair_words(2, tmds_level(n), tab_r[n]);
        tmds_pair_words(1, tmds_level(n), g[n]);
        tmds_pair_words(0, tmds_level(n), bl[n]);
    }
    for (int i = 0; i < 256; i++)
        for (int k = 0; k < 4; k++) tab_gb[i][k] = g[i >> 4][k] | bl[i & 15][k];
}

// Two words of a control period pixel: lane 0 carries {vsync, hsync}.
static inline void tmds_ctrl_words(int vsync, int hsync, uint32_t w[2]) {
    static const uint16_t ctrl[4] = { TMDS_CTRL_00, TMDS_CTRL_01, TMDS_CTRL_10, TMDS_CTRL_11 };
    uint32_t a[2], b[2], c[2];
    tmds_lane_words(0, ctrl[(vsync << 1) | hsync], a);
    tmds_lane_words(1, TMDS_CTRL_00, b);
    tmds_lane_words(2, TMDS_CTRL_00, c);
    w[0] = a[0] | b[0] | c[0];
    w[1] = a[1] | b[1] | c[1];
}

// Encode one framebuffer row (0x0RGB pixels) into 4 words per pixel.
static inline void tmds_encode_row(uint32_t *dst, const uint16_t *src, int n,
                                   const uint32_t tab_r[16][4], const uint32_t tab_gb[256][4]) {
    for (int x = 0; x < n; x++) {
        uint16_t c = src[x];
        const uint32_t *r = tab_r[(c >> 8) & 15], *gb = tab_gb[c & 0xFF];
        dst[0] = r[0] | gb[0];
        dst[1] = r[1] | gb[1];
        dst[2] = r[2] | gb[2];
        dst[3] = r[3] | gb[3];
        dst += 4;
    }
}
