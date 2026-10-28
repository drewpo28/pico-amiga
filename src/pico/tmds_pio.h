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
//
// `capture` (Video > Capture card compatibility, pico-speccy's option of that
// name): a pixel pair normally carries v and v+-1 (a doubled pixel) or a second
// level shifted up to 4 (a hires pair) to stay DC-balanced.  A monitor cannot
// tell, but an MS2130 capture card has a pixel-phase stage that turns such
// pairs into two different colours (pico-speccy, 2026-08-30); State of the
// Art's dithered hires "Spaceballs production" screen showed on a monitor and
// not through the card.  With it, every level snaps to one whose character is
// balanced by itself (five ones of ten: nibbles 3, 6, 9, 12 move by 1-2 of 255)
// and a pair is those characters as they are.
static inline bool tmds_single(uint8_t v, uint16_t *c) {   // v's five-ones character
    const uint16_t q = tmds_qm(v);
    for (int i = 0; i < 2; i++)
        if (tmds_ones(tmds_rep(q, i)) == 5) { *c = tmds_rep(q, i); return true; }
    return false;
}

static inline uint8_t tmds_level(int nibble, bool capture) {
    int v = nibble * 17;
    v = v < 0x08 ? 0x08 : v > 0xF6 ? 0xF6 : v;
    uint16_t c;
    if (capture)
        for (int d = 0; d < 256; d++) {                     // the nearest, the lower on a tie
            if (v - d >= 0 && tmds_single((uint8_t)(v - d), &c)) return (uint8_t)(v - d);
            if (v + d <= 255 && tmds_single((uint8_t)(v + d), &c)) return (uint8_t)(v + d);
        }
    return (uint8_t)v;
}

static inline void tmds_pair_words(int lane, uint8_t v, uint32_t out[4], bool capture) {
    uint16_t a, b;
    if (!capture || !tmds_single(v, &a) || !tmds_single(v, &b)) tmds_balanced_pair(v, &a, &b);
    tmds_lane_words(lane, a, &out[0]);
    tmds_lane_words(lane, b, &out[2]);
}

static inline void tmds_build_tables(uint32_t tab_r[16][4], uint32_t tab_gb[256][4], bool capture) {
    uint32_t g[16][4], bl[16][4];
    for (int n = 0; n < 16; n++) {
        tmds_pair_words(2, tmds_level(n, capture), tab_r[n], capture);
        tmds_pair_words(1, tmds_level(n, capture), g[n], capture);
        tmds_pair_words(0, tmds_level(n, capture), bl[n], capture);
    }
    for (int i = 0; i < 256; i++)
        for (int k = 0; k < 4; k++) tab_gb[i][k] = g[i >> 4][k] | bl[i & 15][k];
}

// Two DIFFERENT colours side by side (hires pixels): the first character is
// exactly va, the second vb or a neighbour within +-4 chosen so that the
// one-counts still sum to 10 - the same rule tmds_balanced_pair() applies to
// a doubled pixel, so the line stays DC-balanced pair by pair.
static inline void tmds_mixed_pair(uint8_t va, uint8_t vb, uint16_t *first, uint16_t *second) {
    if (va == vb) { tmds_balanced_pair(va, first, second); return; }
    const uint16_t qa = tmds_qm(va);
    int best = 1 << 30;
    *first = qa; *second = tmds_qm(vb);
    for (int d = 0; d <= 4; d++) {
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            int c = vb + sgn * d;
            if (c < 0 || c > 255 || (d == 0 && sgn > 0)) continue;
            const uint16_t qb = tmds_qm((uint8_t)c);
            for (int i = 0; i < 2; i++)
                for (int j = 0; j < 2; j++) {
                    uint16_t a = tmds_rep(qa, i), b = tmds_rep(qb, j);
                    if (tmds_ones(a) + tmds_ones(b) != 10) continue;
                    int score = d * 1000 + tmds_pair_swing(a, b) * 32 + tmds_pair_run(a, b);
                    if (score < best) { best = score; *first = a; *second = b; }
                }
        }
        if (best < (1 << 30)) return;
    }
}

// Per-lane tables for a pair of different nibbles: [left][right] -> 4 words.
static inline void tmds_build_mixed(uint32_t tab[3][16][16][4], bool capture) {
    for (int lane = 0; lane < 3; lane++)
        for (int a = 0; a < 16; a++)
            for (int b = 0; b < 16; b++) {
                uint16_t ca, cb;
                const uint8_t va = tmds_level(a, capture), vb = tmds_level(b, capture);
                if (!capture || !tmds_single(va, &ca) || !tmds_single(vb, &cb))
                    tmds_mixed_pair(va, vb, &ca, &cb);
                tmds_lane_words(lane, ca, &tab[lane][a][b][0]);
                tmds_lane_words(lane, cb, &tab[lane][a][b][2]);
            }
}

// Encode one output line of two-colours-per-column pixels: left[x], right[x]
// are the 0x0RGB colours of output pixels 2x and 2x+1.  Equal pairs take the
// doubled-pixel tables, the rest the mixed ones.
// The four words of one pair of output pixels (colours a, b).
static inline void tmds_pair(uint32_t w[4], uint16_t a, uint16_t b,
                             const uint32_t tab_r[16][4], const uint32_t tab_gb[256][4],
                             const uint32_t mix[3][16][16][4]) {
    if (a == b) {
        const uint32_t *p0 = tab_r[(a >> 8) & 15], *p1 = tab_gb[a & 0xFF];
        for (int k = 0; k < 4; k++) w[k] = p0[k] | p1[k];
    } else {
        const uint32_t *p0 = mix[2][(a >> 8) & 15][(b >> 8) & 15];
        const uint32_t *p1 = mix[1][(a >> 4) & 15][(b >> 4) & 15];
        const uint32_t *p2 = mix[0][a & 15][b & 15];
        for (int k = 0; k < 4; k++) w[k] = p0[k] | p1[k] | p2[k];
    }
}

static inline void tmds_encode_pairs(uint32_t *dst, const uint16_t *left, const uint16_t *right, int n,
                                     const uint32_t tab_r[16][4], const uint32_t tab_gb[256][4],
                                     const uint32_t mix[3][16][16][4]) {
    for (int x = 0; x < n; x++, dst += 4) tmds_pair(dst, left[x], right[x], tab_r, tab_gb, mix);
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
