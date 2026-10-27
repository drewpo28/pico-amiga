// PIO HDMI by lookup (video_pio.c): a line is 416 16-bit entry numbers, one per
// slot of two output pixels; an entry is the slot's four TMDS pin words
// (tmds_pio.h format).  The DMA turns numbers into entries through a PIO
// address converter, so the CPU only encodes each DIFFERENT colour (or hires
// pair) of a line, once, and writes the numbers (pico-speccy's PIO HDMI works
// the same way, with one fixed palette; the Amiga needs one per line).
//
// The entries live in one 64 KB-aligned window (number n at window + 16 n):
//   [0, TL_FIXED)                 encoded once: control periods, the data-island
//                                 frame, the video lead, the packets that never
//                                 change (hdmi_audio.h HDMI_PK_*)
//   TL_FIXED + p * TL_PAGE ...    page p of TL_PAGES: a line's own entries (its
//                                 colours and its audio packet), p = line % 5.
//                                 Five, for four line buffers: the second line of
//                                 a row reuses the first one's numbers, so a page
//                                 must outlive the line after its own.
//
// Line layout in slots (720x576@50, 832 pixels a line; video_pio.c):
//   [0, 6)      front porch          [6, 28)   data island (inside hsync)
//   [28, 38)    rest of hsync        [38, 56)  back porch, the video lead at its end
//   [56, 416)   active (360 slots)
// Pure C (no SDK): host/tests/tmds_test.c checks it against the CPU encoder.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "amiga.h"
#include "tmds_pio.h"

#define TL_LINE      416
#define TL_ACTIVE    360
#define TL_BLANK     (TL_LINE - TL_ACTIVE)
#define TL_FP        6
#define TL_ISLAND    6                       // 22 slots: 5 frame, 16 packet, 1 frame
#define TL_SYNC_END  38
#define TL_LEAD      (TL_BLANK - 5)          // the 10-character video lead, 5 slots
#define TL_PAGE      384                     // >= 360 colours + 16 packet slots
#define TL_PAGES     5
#define TL_FIXED     192
#define TL_ENTRIES   (TL_FIXED + TL_PAGES * TL_PAGE)

// Fixed entry numbers.
#define TL_E_CTRL(vs, hs)    ((vs) * 2 + (hs))                 // a control period, both pixels
#define TL_E_PRE(vs, k)      (4 + (vs) * 5 + (k))              // island preamble/guard, 5 slots
#define TL_E_POST(vs)        (14 + (vs))                       // island trailing guard
#define TL_E_LEAD(k)         (16 + (k))                        // video preamble/guard, 5 slots
#define TL_E_PK(id, vs, k)   (32 + ((id) * 2 + (vs)) * 16 + (k))   // a fixed packet, 16 slots

typedef uint32_t tl_entry_t[4];

typedef struct {
    tl_entry_t *lut;                         // the window: TL_ENTRIES entries
    const uint32_t (*tab_r)[4];
    const uint32_t (*tab_gb)[4];
    const uint32_t (*tab_mix)[16][16][4];
    uint32_t terc[3][16][2];                 // TERC4 nibble -> pin words, per lane
    uint16_t map[4096];                      // colour / pair -> entry, tagged gen << 9
    uint16_t gen;                            // 1..127
} tl_t;

// A character (three 10-bit symbols, lane 0 lowest, as hdmi_audio.h packs them)
// as its two pin words.
static inline void tl_char_words(uint32_t c, uint32_t w[2]) {
    w[0] = w[1] = 0;
    for (int lane = 0; lane < 3; lane++) {
        uint32_t lw[2];
        tmds_lane_words(lane, (uint16_t)((c >> (10 * lane)) & 0x3FF), lw);
        w[0] |= lw[0];
        w[1] |= lw[1];
    }
}

static inline void tl_entry_chars(uint32_t *e, uint32_t a, uint32_t b) {
    tl_char_words(a, e);
    tl_char_words(b, e + 2);
}

// A packet character from its three TERC4 nibbles.
static inline void tl_terc_words(const tl_t *t, const uint8_t nib[3], uint32_t w[2]) {
    w[0] = t->terc[0][nib[0]][0] | t->terc[1][nib[1]][0] | t->terc[2][nib[2]][0];
    w[1] = t->terc[0][nib[0]][1] | t->terc[1][nib[1]][1] | t->terc[2][nib[2]][1];
}

// 16 entries of a packet's 32 characters.
static inline void tl_packet(const tl_t *t, uint32_t *e, const uint8_t nib[32][3]) {
    for (int k = 0; k < 16; k++, e += 4) {
        tl_terc_words(t, nib[2 * k], e);
        tl_terc_words(t, nib[2 * k + 1], e + 2);
    }
}

// The fixed entries.  terc4[n]: the 10-bit TERC4 symbol of nibble n; pre/post:
// the island's frame characters per vsync (hdmi_island_frame); lead: the video
// preamble/guard characters; fixed_nib(id, vs, nib): a fixed packet's nibbles.
static inline void tl_init(tl_t *t, tl_entry_t *lut, const uint32_t tab_r[16][4],
                           const uint32_t tab_gb[256][4], const uint32_t tab_mix[3][16][16][4],
                           const uint32_t terc4[16], const uint32_t pre[2][10], const uint32_t post[2][2],
                           const uint32_t lead[10], void (*fixed_nib)(int id, bool vs, uint8_t nib[32][3]),
                           int n_fixed) {
    t->lut = lut;
    t->tab_r = tab_r; t->tab_gb = tab_gb; t->tab_mix = tab_mix;
    for (int lane = 0; lane < 3; lane++)
        for (int n = 0; n < 16; n++) tmds_lane_words(lane, (uint16_t)terc4[n], t->terc[lane][n]);
    for (int vs = 0; vs < 2; vs++)
        for (int hs = 0; hs < 2; hs++) {
            uint32_t w[2];
            tmds_ctrl_words(vs ? 0 : 1, hs, w);   // negative polarity: the pulse is 0
            uint32_t *e = lut[TL_E_CTRL(vs, hs)];
            e[0] = e[2] = w[0]; e[1] = e[3] = w[1];
        }
    for (int vs = 0; vs < 2; vs++) {
        for (int k = 0; k < 5; k++) tl_entry_chars(lut[TL_E_PRE(vs, k)], pre[vs][2 * k], pre[vs][2 * k + 1]);
        tl_entry_chars(lut[TL_E_POST(vs)], post[vs][0], post[vs][1]);
    }
    for (int k = 0; k < 5; k++) tl_entry_chars(lut[TL_E_LEAD(k)], lead[2 * k], lead[2 * k + 1]);
    for (int id = 0; id < n_fixed; id++)
        for (int vs = 0; vs < 2; vs++) {
            uint8_t nib[32][3];
            fixed_nib(id, vs, nib);
            tl_packet(t, lut[TL_E_PK(id, vs, 0)], nib);
        }
    for (int i = 0; i < 4096; i++) t->map[i] = 0;
    t->gen = 1;
}

// The blanking part of a line: control periods, the island frame (its 16 packet
// slots are the caller's) and, before an active line, the video lead.
static inline void tl_blank(uint16_t *it, bool vsync, bool active) {
    const int vs = vsync ? 1 : 0;
    for (int s = 0; s < TL_FP; s++) it[s] = TL_E_CTRL(vs, 1);
    for (int k = 0; k < 5; k++) it[TL_ISLAND + k] = (uint16_t)TL_E_PRE(vs, k);
    it[TL_ISLAND + 21] = (uint16_t)TL_E_POST(vs);
    for (int s = TL_ISLAND + 22; s < TL_SYNC_END; s++) it[s] = TL_E_CTRL(vs, 0);
    for (int s = TL_SYNC_END; s < TL_BLANK; s++) it[s] = TL_E_CTRL(vs, 1);
    if (active)
        for (int k = 0; k < 5; k++) it[TL_LEAD + k] = (uint16_t)TL_E_LEAD(k);
}

// The 16 packet slots of the island: a fixed packet, or an audio packet's
// characters encoded into the line's page from entry *n on.
static inline void tl_island_fixed(uint16_t *it, int id, bool vsync) {
    for (int k = 0; k < 16; k++) it[TL_ISLAND + 5 + k] = (uint16_t)TL_E_PK(id, vsync ? 1 : 0, k);
}
static inline void tl_island_audio(tl_t *t, uint16_t *it, int page, int *n, const uint8_t nib[32][3]) {
    const int base = TL_FIXED + page * TL_PAGE + *n;
    tl_packet(t, t->lut[base], nib);
    for (int k = 0; k < 16; k++) it[TL_ISLAND + 5 + k] = (uint16_t)(base + k);
    *n += 16;
}

// A vertical-blank line's active part.
static inline void tl_active_blank(uint16_t *it, bool vsync) {
    const uint16_t e = (uint16_t)TL_E_CTRL(vsync ? 1 : 0, 1);
    for (int s = TL_BLANK; s < TL_LINE; s++) it[s] = e;
}

// Output line `sub` of a framebuffer row into the active slots, allocating the
// line's entries in `page` from *n on.  KEY(x) is a slot's colour or pair, NEW
// encodes entry e for key k; a run of one key costs one store a slot.
#define TL_ACTIVE_LOOP(KEY, NEW)                                                    \
    for (int x = 0; x < TL_ACTIVE; x++) {                                         \
        const uint32_t k = (KEY);                                                 \
        if (k != key) {                                                           \
            key = k;                                                              \
            const uint16_t m = t->map[k];                                         \
            if ((m & 0xFE00) == tag) {                                            \
                cur = (uint32_t)base + (m & 0x1FF);                               \
            } else {                                                              \
                const int i = (*n)++;                                             \
                t->map[k] = (uint16_t)(tag | (uint32_t)i);                        \
                cur = (uint32_t)base + (uint32_t)i;                               \
                uint32_t *e = t->lut[cur];                                        \
                NEW;                                                              \
            }                                                                     \
        }                                                                         \
        dst[x] = (uint16_t)cur;                                                   \
    }

static inline void tl_doubled(const tl_t *t, uint32_t *e, uint32_t c) {   // a lowres pixel
    const uint32_t *r = t->tab_r[(c >> 8) & 15];
    const uint32_t *gb = t->tab_gb[c & 0xFF];
    e[0] = r[0] | gb[0]; e[1] = r[1] | gb[1]; e[2] = r[2] | gb[2]; e[3] = r[3] | gb[3];
}

static inline void tl_active(tl_t *t, uint16_t *it, int page, int *n,
                             const amiga_row_t *row, int sub) {
    if (++t->gen == 128) {                   // tags wrapped: forget every entry
        for (int i = 0; i < 4096; i++) t->map[i] = 0;
        t->gen = 1;
    }
    const uint32_t tag = (uint32_t)t->gen << 9;
    const int base = TL_FIXED + page * TL_PAGE;
    uint16_t *dst = it + TL_BLANK;
    uint32_t key = 0xFFFFFFFFu, cur = 0;
    if (row->mode == AMIGA_ROW_IDX8) {
        const uint8_t *ix = row->px.idx;
        const uint16_t *pal = row->pal;
        TL_ACTIVE_LOOP((uint32_t)(ix[2 * x] & 63) << 6 | (ix[2 * x + 1] & 63),
                       tmds_pair(e, pal[k >> 6], pal[k & 63], t->tab_r, t->tab_gb, t->tab_mix))
    } else if (row->mode == AMIGA_ROW_LACE4) {
        const uint8_t *nb = row->px.nib[sub & 1];
        const uint16_t *pal = row->pal + 16 * (sub & 1);
        TL_ACTIVE_LOOP(nb[x], tmds_pair(e, pal[k >> 4], pal[k & 15], t->tab_r, t->tab_gb, t->tab_mix))
    } else {
        const uint16_t *rgb = row->px.rgb;
        TL_ACTIVE_LOOP(rgb[x] & 0xFFFu, tl_doubled(t, e, k))
    }
}
#undef TL_ACTIVE_LOOP
