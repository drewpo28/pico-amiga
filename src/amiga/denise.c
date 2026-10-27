// Bitplane + sprite DMA (the Agnus side) and the Denise pixel pipeline.
//
// The current line is rendered INCREMENTALLY: every write to a display
// register first calls render_to(hpos), which performs the bitplane fetches
// and emits the pixels the beam has passed with the OLD register values, and
// only then applies the write.  That gives copper colour bars, mid-line
// BPLCON changes and split screens at pixel accuracy without an event queue.
//
// Pipeline model (lowres pixel = 1/2 colour clock, "DIW coordinates"):
//   * fetch unit k starts at colour clock t = DDFSTRT + k*8 and reads one word
//     per enabled plane at once (hires: two consecutive words per plane);
//   * its 16 pixels appear from DIW x = 2t + 17 (hires: 2 x 16 from 2t + 9), delayed by
//     BPLCON1 PF1H (odd planes) / PF2H (even planes);
//   * pixels outside DIW, or on lines outside the vertical window, show COLOR00.
// So the standard DDFSTRT $38 / DIWSTRT $81 pair lines up, like the hardware.
//
// Playfield bits are accumulated per line in pf[] at hires resolution (two
// entries per lowres pixel), sprites in spl[] at lowres resolution; colours
// are resolved at output time, so palette writes affect only later pixels.
//
// The per-line path is AMIGA_HOT (SRAM on the board): run from flash it took
// 8-11 ms a frame on Cannon Fodder, its code competing with the PSRAM chip RAM
// for the same 16 KB XIP cache.
#include <string.h>
#include "amiga_int.h"
#ifdef DENISE_DEBUG
#include <stdio.h>
#endif

#define PF_SIZE  1024
#define SPL_SIZE 512

static uint16_t bplcon0, bplcon1, bplcon2;
static uint16_t diwstrt = 0x2C81, diwstop = 0xF4C1, ddfstrt = 0x38, ddfstop = 0xD0;
static uint32_t bplpt[6];
static int16_t  bplmod[2];
static uint16_t color[32];

static uint32_t sprpt[8];
static uint16_t sprpos[8], sprctl[8], sprdata[8], sprdatb[8];
static uint16_t spr_vstart[8], spr_vstop[8];
static bool     spr_armed[8];
static uint8_t  spr_dma[8];
enum { SPR_IDLE, SPR_FETCH_CTL, SPR_WAIT, SPR_DATA };

// Per-line render state.
static uint8_t  pf[PF_SIZE];
static uint8_t  spl[SPL_SIZE];
static int      spl_lo, spl_hi;           // dirty range of spl[]
static int      r_h;                      // rendered up to this colour clock
static bool     vwin, bpl_dma;
static int      fetch_k, nunits, ds, unit_cc;
static amiga_row_t *fbrow;              // row being written, or NULL
static amiga_row_t *fbdest;             // overlay row: where fbrow (the scratch) goes at line end
static amiga_row_t  scratch_row;
static int          ov_y0, ov_y1;       // amiga_set_overlay_rows

void amiga_set_overlay_rows(int y0, int y1) { ov_y0 = y0; ov_y1 = y1; }
static uint16_t ham_col;
static bool     ham_in;

// ---- helpers -----------------------------------------------------------------
AMIGA_HOT static int planes(void) {
    int np = (bplcon0 >> 12) & 7;
    if (bplcon0 & 0x8000) { if (np > 4) np = 4; }
    else if (np > 6) np = 6;
    return np;
}

// Bit-spread tables: one plane word -> per-pixel bytes holding 0/1.
static uint32_t spread4[16];     // nibble -> 4 bytes (hires: 1 byte per pixel)
static uint64_t spread4x2[16];   // nibble -> 8 bytes (lowres: each pixel twice)
static uint32_t spread8[256][2]; // byte -> 8 bytes, pixels 0-3 and 4-7

static void tables_init(void) {
    for (int n = 0; n < 16; n++) {
        uint32_t v = 0; uint64_t w = 0;
        for (int i = 0; i < 4; i++) {
            int bit = (n >> (3 - i)) & 1;     // MSB is the leftmost pixel
            v |= (uint32_t)bit << (8 * i);
            w |= (uint64_t)bit << (16 * i);
            w |= (uint64_t)bit << (16 * i + 8);
        }
        spread4[n] = v;
        spread4x2[n] = w;
    }
    for (int b = 0; b < 256; b++) {
        spread8[b][0] = spread4[b >> 4];
        spread8[b][1] = spread4[b & 15];
    }
}

// memset() is a libc call in flash: the hot path clears with this instead (the
// attribute keeps GCC from turning the loop back into that call).
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("no-tree-loop-distribute-patterns")))
#endif
static inline void fill_bytes(uint8_t *p, uint8_t v, int n) {
    const uint32_t w = v * 0x01010101u;
    while (n > 0 && ((uintptr_t)p & 3)) { *p++ = v; n--; }
    for (; n >= 4; n -= 4, p += 4) *(uint32_t *)(void *)p = w;
    while (n-- > 0) *p++ = v;
}
static inline void clear_bytes(uint8_t *p, int n) { fill_bytes(p, 0, n); }

static inline void or32(uint8_t *p, uint32_t v) { uint32_t t; memcpy(&t, p, 4); t |= v; memcpy(p, &t, 4); }
static inline void or64(uint8_t *p, uint64_t v) { uint64_t t; memcpy(&t, p, 8); t |= v; memcpy(p, &t, 8); }

// Fetch units are 8 colour clocks in both resolutions (hires fetches two words
// per plane per unit).  Units start at DDFSTRT + 8k; the unit that begins at
// or after DDFSTOP is the last one, as in the OCS sequencer.  So lowres
// $38/$D0 gives 20 units (20 words), hires $3C/$D4 and $3C/$D0 both 20 units
// (40 words) - the latter is what AROS programs for a 640-pixel screen.
AMIGA_HOT static void compute_ddf(void) {
    int a = ddfstrt & 0xFC, b = ddfstop & 0xFC;
    if (a < 0x18) a = 0x18;
    if (b > 0xD8) b = 0xD8;
    ds = a;
    unit_cc = 8;
    nunits = b < a ? 0 : ((b - a + 7) >> 3) + 1;
}

AMIGA_HOT static void fetch_unit(int k) {
    int t = ds + k * unit_cc;
    bool hires = bplcon0 & 0x8000;
    int np = planes();
    int x = hires ? 2 * (2 * t + 9) : 2 * (2 * t + 17);       // hires-pixel coordinate
    int d1 = 2 * (bplcon1 & 15), d2 = 2 * ((bplcon1 >> 4) & 15);
    // The chip RAM reads first, as a bucket of their own (PSRAM latency).  A
    // hires plane's two words are one 32-bit read when they share a word.
    uint16_t wd[6][2];
    const int prof = prof_switch(AMIGA_PROF_BPLRD);
    const uint8_t *cr = A.cfg.chip_ram;
    const uint32_t cm = A.chip_mask & ~1u;
    for (int p = 0; p < np; p++) {
        const uint32_t a = bplpt[p] & cm;
        if (hires && !(a & 2)) {
            uint32_t v;
            memcpy(&v, cr + a, 4);
            v = __builtin_bswap32(v);
            wd[p][0] = (uint16_t)(v >> 16);
            wd[p][1] = (uint16_t)v;
        } else {
            wd[p][0] = U16BE(cr + a);
            if (hires) wd[p][1] = U16BE(cr + ((bplpt[p] + 2) & cm));
        }
        bplpt[p] += hires ? 4 : 2;
    }
    prof_switch(prof);
    // Same scroll for odd and even planes (the usual case): every plane lands on
    // the same pixels, so a group of pixels is built in a register and ORed in
    // once (OR, not store: a scroll or DDF change mid-line may overlap units).
    if (d1 == d2 && np > 0) {
        // Eight pixels at a time: one byte of every plane through spread8.
        if (hires) {
            for (int j = 0, pos = x + d1; j < 2; j++, pos += 16) {
                if (pos < 0 || pos + 16 > PF_SIZE) continue;
                for (int s = 8, o = 0; s >= 0; s -= 8, o += 8) {
                    uint32_t lo = 0, hi = 0;
                    for (int p = 0; p < np; p++) {
                        const uint32_t *t = spread8[(wd[p][j] >> s) & 0xFF];
                        lo |= t[0] << p;
                        hi |= t[1] << p;
                    }
                    or32(pf + pos + o, lo);
                    or32(pf + pos + o + 4, hi);
                }
            }
        } else {
            const int pos = x + d1;
            if (pos >= 0 && pos + 32 <= PF_SIZE) {
                for (int s = 8, o = 0; s >= 0; s -= 8, o += 16) {
                    uint32_t q[2] = { 0, 0 };
                    for (int p = 0; p < np; p++) {
                        const uint32_t *t = spread8[(wd[p][0] >> s) & 0xFF];
                        q[0] |= t[0] << p;
                        q[1] |= t[1] << p;
                    }
                    // Each lowres pixel twice: bytes abcd -> aabb ccdd.
                    for (int h = 0; h < 2; h++) {
                        const uint32_t v = q[h];
                        or32(pf + pos + o + 8 * h,
                             (v & 0xFF) | (v & 0xFF) << 8 | (v & 0xFF00) << 8 | (v & 0xFF00) << 16);
                        or32(pf + pos + o + 8 * h + 4,
                             (v >> 16 & 0xFF) | (v >> 16 & 0xFF) << 8 | (v >> 8 & 0xFF0000) | (v & 0xFF000000));
                    }
                }
            }
        }
        if (k == nunits - 1)
            for (int p = 0; p < np; p++) bplpt[p] += (uint32_t)(int32_t)bplmod[p & 1];
        return;
    }
    for (int p = 0; p < np; p++) {
        int pos = x + ((p & 1) ? d2 : d1);
        if (hires) {
            for (int j = 0; j < 2; j++, pos += 16) {
                uint16_t w = wd[p][j];
                if (pos < 0 || pos + 16 > PF_SIZE) continue;
                for (int n = 0; n < 4; n++)
                    or32(pf + pos + 4 * n, spread4[(w >> (12 - 4 * n)) & 15] << p);
            }
        } else {
            uint16_t w = wd[p][0];
            if (pos < 0 || pos + 32 > PF_SIZE) continue;
            for (int n = 0; n < 4; n++)
                or64(pf + pos + 8 * n, spread4x2[(w >> (12 - 4 * n)) & 15] << p);
        }
    }
    if (k == nunits - 1) {
        for (int p = 0; p < np; p++) bplpt[p] += (uint32_t)(int32_t)bplmod[p & 1];
    }
}

// Playfield value -> colour, plus whether it is opaque and which playfield
// (1 or 2) owns it, for the sprite priority test.
static inline uint16_t pf_color(uint8_t v, int *which) {
    if (bplcon0 & 0x0400) {                                   // dual playfield
        int p1 = (v & 1) | ((v >> 1) & 2) | ((v >> 2) & 4);
        int p2 = ((v >> 1) & 1) | ((v >> 2) & 2) | ((v >> 3) & 4);
        bool pri2 = bplcon2 & 0x40;
        if (pri2) {
            if (p2) { *which = 2; return color[8 + p2]; }
            if (p1) { *which = 1; return color[p1]; }
        } else {
            if (p1) { *which = 1; return color[p1]; }
            if (p2) { *which = 2; return color[8 + p2]; }
        }
        *which = 0;
        return color[0];
    }
    *which = v ? 2 : 0;                                        // single PF uses PF2P
    if (v & 32) {                                              // EHB (6 planes, no HAM)
        return (uint16_t)((color[v & 31] >> 1) & 0x777);
    }
    return color[v & 31];
}

static inline uint16_t avg(uint16_t a, uint16_t b) {
    return (uint16_t)(((a & 0xEEE) + (b & 0xEEE)) >> 1);
}

// Apply a sprite on top of a playfield pixel.
static inline uint16_t sprite_mix(int x, uint16_t c, int which) {
    uint8_t s = spl[x];
    if (!(s & 0x80)) return c;
    int pair = (s >> 4) & 3;
    if (which) {
        int pri = which == 1 ? (bplcon2 & 7) : ((bplcon2 >> 3) & 7);
        if (pair >= pri) return c;                             // playfield in front
    }
    return color[16 + (s & 15)];
}

// Colour lookup for the current mode: pf value -> colour, and which playfield
// owns it (for sprite priority).  Rebuilt lazily after palette/BPLCON writes.
static uint16_t lut[64];
static uint8_t  lut_pf[64];
static bool     lut_dirty = true;
// pf value -> palette index of the row being written (0xFF = not assigned yet);
// valid for one line and one LUT generation.
static uint8_t  ilut[64];
// Hires IDX8 row whose palette starts with lut[0..15] as entries 0..15 (w_begin)
// and whose LUT has not changed since: a pf value is its own index.
static bool     ilut_ident;
AMIGA_HOT static void ilut_reset(void) { fill_bytes(ilut, 0xFF, (int)sizeof ilut); ilut_ident = false; }

AMIGA_HOT static void build_lut(void) {
    for (int v = 0; v < 64; v++) {
        int w;
        lut[v] = pf_color((uint8_t)v, &w);
        lut_pf[v] = (uint8_t)w;
    }
    lut_dirty = false;
    ilut_reset();
}

// ---- framebuffer row writer ---------------------------------------------------
// Pixels arrive as (lowres column, left hires colour, right hires colour).  The
// row's storage (amiga.h) is chosen at line start; colours are turned into
// palette indices through a 4096-entry map tagged per line, and a line that
// runs out of palette entries is converted to DIRECT on the spot.
static int      w_mode, w_field, w_base, w_max, w_n;
static uint16_t map_tag[4096];
static uint8_t  map_idx[4096];
static uint16_t map_gen;
static uint16_t last_c = 0xFFFF;
static uint8_t  last_i;

AMIGA_HOT static void w_begin(amiga_row_t *row, int mode, int field) {
    ilut_reset();
    if (++map_gen == 0) { memset(map_tag, 0, sizeof map_tag); map_gen = 1; }
    last_c = 0xFFFF;
    w_mode = mode; w_field = field; w_n = 0;
    w_base = mode == AMIGA_ROW_LACE4 ? 16 * field : 0;
    w_max = mode == AMIGA_ROW_LACE4 ? 16 : 64;
    if (mode == AMIGA_ROW_LACE4 && row->mode != AMIGA_ROW_LACE4) {
        // Entering interlace: the other field's line is unknown until it is drawn.
        int o = field ^ 1;
        row->pal[16 * o] = color[0];
        row->ncol[o] = 1;
        memset(row->px.nib[o], 0, sizeof row->px.nib[o]);
    }
    row->mode = (uint8_t)mode;
    if (mode == AMIGA_ROW_IDX8) {
        // Hires has at most 4 planes: the 16 pf values take entries 0..15 as
        // they are, leaving 48 for sprites and colours changed mid-line.
        for (int v = 0; v < 16; v++) {
            const uint16_t c = lut[v];
            row->pal[v] = c;
            if (map_tag[c] != map_gen) { map_tag[c] = map_gen; map_idx[c] = (uint8_t)v; }
            ilut[v] = (uint8_t)v;
        }
        w_n = 16;
        row->ncol[0] = 16;
        ilut_ident = true;
    }
}

AMIGA_HOT static void w_to_direct(amiga_row_t *row) {
    static uint8_t tmp[AMIGA_FB_W];
    if (w_mode == AMIGA_ROW_IDX8) {
        for (int x = 0; x < AMIGA_FB_W; x++)
            row->px.rgb[x] = avg(row->pal[row->px.idx[2 * x]], row->pal[row->px.idx[2 * x + 1]]);
    } else if (w_mode == AMIGA_ROW_LACE4) {
        memcpy(tmp, row->px.nib[w_field], sizeof tmp);
        for (int x = 0; x < AMIGA_FB_W; x++)
            row->px.rgb[x] = avg(row->pal[w_base + (tmp[x] >> 4)], row->pal[w_base + (tmp[x] & 15)]);
    }
    row->mode = AMIGA_ROW_DIRECT;
    w_mode = AMIGA_ROW_DIRECT;
}

AMIGA_HOT static inline int w_index(amiga_row_t *row, uint16_t c) {
    if (c == last_c) return last_i;
    int i;
    if (map_tag[c] == map_gen) i = map_idx[c];
    else {
        if (w_n == w_max) return -1;
        i = w_n++;
        row->pal[w_base + i] = c;
        row->ncol[w_field] = (uint8_t)w_n;
        map_tag[c] = map_gen;
        map_idx[c] = (uint8_t)i;
    }
    last_c = c; last_i = (uint8_t)i;
    return i;
}

AMIGA_HOT static inline void w_put(int fx, uint16_t c0, uint16_t c1) {
    amiga_row_t *row = fbrow;
    if (w_mode != AMIGA_ROW_DIRECT) {
        int i0 = w_index(row, c0), i1 = c1 == c0 ? i0 : w_index(row, c1);
        if (i0 >= 0 && i1 >= 0) {
            if (w_mode == AMIGA_ROW_IDX8) { row->px.idx[2 * fx] = (uint8_t)i0; row->px.idx[2 * fx + 1] = (uint8_t)i1; }
            else row->px.nib[w_field][fx] = (uint8_t)((i0 << 4) | i1);
            return;
        }
        w_to_direct(row);
    }
    row->px.rgb[fx] = c0 == c1 ? c0 : avg(c0, c1);
}

// Framebuffer columns [a, b) in one colour (the border).
AMIGA_HOT static void w_fill(int a, int b, uint16_t c) {
    amiga_row_t *row = fbrow;
    if (a >= b) return;
    if (w_mode != AMIGA_ROW_DIRECT) {
        const int i = w_index(row, c);
        if (i >= 0) {
            if (w_mode == AMIGA_ROW_IDX8) fill_bytes(row->px.idx + 2 * a, (uint8_t)i, 2 * (b - a));
            else fill_bytes(row->px.nib[w_field] + a, (uint8_t)((i << 4) | i), b - a);
            return;
        }
        w_to_direct(row);
    }
    for (int x = a; x < b; x++) row->px.rgb[x] = c;
}

// Window pixels [x0, x1) without sprites (DIW coordinates, already clipped to
// the framebuffer): the common case, kept free of per-pixel colour lookups.
AMIGA_HOT static void w_plain(int x0, int x1, bool hires) {
    amiga_row_t *row = fbrow;
    const int f0 = AMIGA_FB_X0;
    if (w_mode == AMIGA_ROW_DIRECT) {
        uint16_t *d = row->px.rgb - f0;
        if (!hires) {
            for (int x = x0; x < x1; x++) d[x] = lut[pf[2 * x]];
        } else {
            for (int x = x0; x < x1; x++) {
                const uint16_t c0 = lut[pf[2 * x]], c1 = lut[pf[2 * x + 1]];
                d[x] = c0 == c1 ? c0 : avg(c0, c1);
            }
        }
        return;
    }
    if (ilut_ident && hires) {
        // pf values are the indices: copy the line as it is (4 pixels a word).
        // A value of 16+ (planes fetched in lowres before a mode switch) has no
        // such entry: then the general path below redoes the range.
        uint8_t *d = row->px.idx + 2 * (x0 - f0);
        const uint8_t *s = pf + 2 * x0;
        int n = 2 * (x1 - x0);
        uint32_t any = 0;
        for (; n >= 4; n -= 4, d += 4, s += 4) { uint32_t t; memcpy(&t, s, 4); memcpy(d, &t, 4); any |= t; }
        for (; n > 0; n--) { any |= *s; *d++ = *s++; }
        if (!(any & 0xF0F0F0F0u)) return;
        ilut_ident = false;
    }
    for (int x = x0; x < x1; x++) {
        uint8_t v0 = pf[2 * x], v1 = hires ? pf[2 * x + 1] : v0;
        uint8_t i0 = ilut[v0], i1 = ilut[v1];
        if ((i0 | i1) & 0x80) {                 // first use on this line: allocate
            int a = w_index(row, lut[v0]);
            int b = a >= 0 ? w_index(row, lut[v1]) : -1;
            if (b < 0) {                        // overflow -> DIRECT, for the rest too
                w_put(x - f0, lut[v0], lut[v1]);
                w_plain(x + 1, x1, hires);
                return;
            }
            ilut[v0] = (uint8_t)a; ilut[v1] = (uint8_t)b;
            i0 = (uint8_t)a; i1 = (uint8_t)b;
        }
        if (w_mode == AMIGA_ROW_IDX8) { row->px.idx[2 * (x - f0)] = i0; row->px.idx[2 * (x - f0) + 1] = i1; }
        else row->px.nib[w_field][x - f0] = (uint8_t)((i0 << 4) | i1);
    }
}

AMIGA_HOT static void output_ham(int x0, int x1) {
    for (int x = x0; x < x1; x++) {
        if (!ham_in) { ham_col = color[0]; ham_in = true; }
        uint8_t v = pf[2 * x];
        int d = v & 15;
        switch (v >> 4) {
        case 0: ham_col = color[d]; break;
        case 1: ham_col = (uint16_t)((ham_col & 0xFF0) | d); break;
        case 2: ham_col = (uint16_t)((ham_col & 0x0FF) | (d << 8)); break;
        default: ham_col = (uint16_t)((ham_col & 0xF0F) | (d << 4)); break;
        }
        int fx = x - AMIGA_FB_X0;
        if (fbrow && fx >= 0 && fx < AMIGA_FB_W) {
            uint16_t c = sprite_mix(x, ham_col, v ? 2 : 0);
            w_put(fx, c, c);
        }
    }
}

// Pixels [x0, x1) of the current line, in DIW coordinates.
AMIGA_HOT static void output(int x0, int x1) {
    if (lut_dirty) build_lut();
    const int hstart = diwstrt & 0xFF, hstop = (diwstop & 0xFF) | 0x100;
    const bool hires = bplcon0 & 0x8000;
    const bool ham = (bplcon0 & 0x0800) && !hires && planes() == 6;

    // Window part of the range; everything else is border (COLOR00).
    int w0 = x0, w1 = x0;
    if (vwin) {
        w0 = x0 > hstart ? x0 : hstart;
        w1 = x1 < hstop ? x1 : hstop;
        if (w1 < w0) w1 = w0;
    }
    if (w0 > x0 || w1 < x1) ham_in = false;
    if (ham && w1 > w0) output_ham(w0, w1);
    if (!fbrow) return;

    // Clip to the framebuffer and write border + window.
    const int f0 = AMIGA_FB_X0, f1 = AMIGA_FB_X0 + AMIGA_FB_W;
    int a = x0 < f0 ? f0 : x0, b = x1 > f1 ? f1 : x1;
    if (a >= b) return;
    const uint16_t bg = color[0];
    int wa = w0 < a ? a : w0, wb = w1 > b ? b : w1;
    if (wb < wa) wb = wa;
    w_fill(a - f0, wa - f0, bg);
    w_fill(wb - f0, b - f0, bg);
    if (ham || wa >= wb) return;

    const bool sprites = spl_hi > wa && spl_lo < wb;
    if (!sprites) { w_plain(wa, wb, hires); return; }
    // Only the columns sprites cover take the per-pixel path.
    if (spl_lo > wa) { w_plain(wa, spl_lo, hires); wa = spl_lo; }
    const int tail = spl_hi < wb ? spl_hi : wb;
    if (tail < wb) { w_plain(tail, wb, hires); wb = tail; }
    if (!hires) {
        {
            for (int x = wa; x < wb; x++) {
                uint8_t v = pf[2 * x];
                uint16_t c = sprite_mix(x, lut[v], lut_pf[v]);
                w_put(x - f0, c, c);
            }
        }
    } else {
        {
            for (int x = wa; x < wb; x++) {
                uint8_t v0 = pf[2 * x], v1 = pf[2 * x + 1];
                w_put(x - f0, sprite_mix(x, lut[v0], lut_pf[v0]), sprite_mix(x, lut[v1], lut_pf[v1]));
            }
        }
    }
}

AMIGA_HOT static void render_to(int h) {
    if (h > AMIGA_CC_PER_LINE) h = AMIGA_CC_PER_LINE;
    if (h <= r_h) return;
    const int prof = prof_switch(AMIGA_PROF_RENDER);
    if (bpl_dma && fetch_k < nunits && ds + fetch_k * unit_cc < h) {
        prof_switch(AMIGA_PROF_BPL);
        while (fetch_k < nunits && ds + fetch_k * unit_cc < h) {
            fetch_unit(fetch_k);
            fetch_k++;
        }
        prof_switch(AMIGA_PROF_RENDER);
    }
    output(2 * r_h, 2 * h);
    r_h = h;
    prof_switch(prof);
}

// ---- sprites -----------------------------------------------------------------
AMIGA_HOT static void spr_decode(int s) {
    spr_vstart[s] = (uint16_t)((sprpos[s] >> 8) | ((sprctl[s] & 4) << 6));
    spr_vstop[s]  = (uint16_t)((sprctl[s] >> 8) | ((sprctl[s] & 2) << 7));
}

AMIGA_HOT static int spr_x(int s) {   // DIW x of the sprite's first pixel
    return (((sprpos[s] & 0xFF) << 1) | (sprctl[s] & 1)) + 1;
}

AMIGA_HOT static void sprite_dma(void) {
    bool on = (A.dmacon & (DMAF_MASTER | DMAF_SPRITE)) == (DMAF_MASTER | DMAF_SPRITE);
    if (!on || A.vpos < 25 || A.vpos >= A.lines - 1) return;
    for (int s = 0; s < 8; s++) {
        if (spr_dma[s] == SPR_IDLE) continue;
        if (spr_dma[s] == SPR_WAIT && A.vpos == spr_vstart[s]) spr_dma[s] = SPR_DATA;
        if (spr_dma[s] == SPR_DATA && A.vpos == spr_vstop[s]) spr_dma[s] = SPR_FETCH_CTL;
        if (spr_dma[s] == SPR_FETCH_CTL) {
            sprpos[s] = chip_rw(sprpt[s]);
            sprctl[s] = chip_rw(sprpt[s] + 2);
            sprpt[s] += 4;
            spr_decode(s);
            spr_armed[s] = false;
            spr_dma[s] = (sprpos[s] | sprctl[s]) ? SPR_WAIT : SPR_IDLE;
            if (spr_dma[s] == SPR_WAIT && A.vpos == spr_vstart[s]) spr_dma[s] = SPR_DATA;
            if (spr_dma[s] != SPR_DATA) continue;
        }
        if (spr_dma[s] == SPR_DATA) {
            sprdata[s] = chip_rw(sprpt[s]);
            sprdatb[s] = chip_rw(sprpt[s] + 2);
            sprpt[s] += 4;
            spr_armed[s] = true;
        }
    }
}

// Pixel x of a sprite starting at x0 (data a, b): its 2-bit colour, 0 outside.
static inline int spr_px(int x, int x0, uint16_t a, uint16_t b) {
    const unsigned i = (unsigned)(x - x0);
    if (i > 15) return 0;
    return ((a >> (15 - i)) & 1) | (((b >> (15 - i)) & 1) << 1);
}

AMIGA_HOT static void sprite_compose(void) {
    if (spl_hi > spl_lo) clear_bytes(spl + spl_lo, spl_hi - spl_lo);
    spl_lo = SPL_SIZE; spl_hi = 0;
    for (int pair = 3; pair >= 0; pair--) {
        int e = 2 * pair, o = e + 1;
        if (!spr_armed[e] && !spr_armed[o]) continue;
        bool att = (sprctl[o] & 0x80) != 0;
        int lo = SPL_SIZE, hi = 0;
        for (int s = e; s <= o; s++) if (spr_armed[s]) {
            int a = spr_x(s);
            if (a < lo) lo = a;
            if (a + 16 > hi) hi = a + 16;
        }
        if (lo < 0) lo = 0;
        if (hi > SPL_SIZE) hi = SPL_SIZE;
        // An unarmed sprite of the pair draws nothing: data 0.
        const int xe = spr_x(e), xo = spr_x(o);
        const uint16_t ea = spr_armed[e] ? sprdata[e] : 0, eb = spr_armed[e] ? sprdatb[e] : 0;
        const uint16_t oa = spr_armed[o] ? sprdata[o] : 0, ob = spr_armed[o] ? sprdatb[o] : 0;
        for (int x = lo; x < hi; x++) {
            int ve = spr_px(x, xe, ea, eb), vo = spr_px(x, xo, oa, ob), ci;
            if (att) ci = (vo << 2) | ve;
            else if (ve) ci = 4 * pair + ve;
            else if (vo) ci = 4 * pair + vo;
            else ci = 0;
            if (!ci) continue;
            spl[x] = (uint8_t)(0x80 | (pair << 4) | (ci & 15));
            if (x < spl_lo) spl_lo = x;
            if (x + 1 > spl_hi) spl_hi = x + 1;
        }
    }
}

// ---- line / frame hooks ------------------------------------------------------
AMIGA_HOT void denise_frame_start(void) {
    for (int s = 0; s < 8; s++) { spr_dma[s] = SPR_FETCH_CTL; spr_armed[s] = false; }
}

AMIGA_HOT void denise_line_start(void) {
    int vstart = diwstrt >> 8;
    int vstop = (diwstop >> 8) | ((diwstop & 0x8000) ? 0 : 0x100);
    vwin = A.vpos >= vstart && A.vpos < vstop;
    bpl_dma = vwin && (A.dmacon & (DMAF_MASTER | DMAF_RASTER)) == (DMAF_MASTER | DMAF_RASTER);
    compute_ddf();
    fetch_k = 0;
    r_h = 0;
    clear_bytes(pf, (int)sizeof pf);
    ham_in = false;
    int row = (int)A.vpos - AMIGA_FB_Y0;
    fbrow = (A.cfg.framebuffer && row >= 0 && row < AMIGA_FB_H) ? A.cfg.framebuffer + row : NULL;
    fbdest = NULL;
    if (fbrow && row >= ov_y0 && row < ov_y1) {
        // Built off-screen (the interlace writer keeps the other field: start
        // from the row as it is).
        fbdest = fbrow;
        memcpy(&scratch_row, fbdest, sizeof scratch_row);
        fbrow = &scratch_row;
    }
    if (fbrow) {
        if (lut_dirty) build_lut();
        if (bplcon0 & 0x0004)      w_begin(fbrow, AMIGA_ROW_LACE4, A.lof ? 0 : 1);
        else if (bplcon0 & 0x8000) w_begin(fbrow, AMIGA_ROW_IDX8, 0);
        else                       w_begin(fbrow, AMIGA_ROW_DIRECT, 0);
    }
    sprite_dma();
    sprite_compose();
#ifdef DENISE_DEBUG
    if (A.vpos == 120 && A.frame % 100 == 99) {
        printf("[denise] f%u con0=%04X con1=%04X con2=%04X diw=%04X/%04X ddf=%02X/%02X mod=%d/%d units=%d pt0=%06X dma=%d\n",
               A.frame, bplcon0, bplcon1, bplcon2, diwstrt, diwstop, ddfstrt, ddfstop, bplmod[0], bplmod[1], nunits, bplpt[0], bpl_dma);
    }
#endif
}

AMIGA_HOT void denise_line_end(void) {
    render_to(AMIGA_CC_PER_LINE);
    // The framebuffer reaches a few pixels past the end of the line: border.
    if (fbrow) {
        const int x = 2 * AMIGA_CC_PER_LINE - AMIGA_FB_X0;
        w_fill(x < 0 ? 0 : x, AMIGA_FB_W, color[0]);
        if (fbdest) {
            if (A.cfg.row_done) A.cfg.row_done(A.cfg.ctx, (int)(fbdest - A.cfg.framebuffer), fbrow);
            memcpy(fbdest, fbrow, sizeof *fbdest);
            fbrow = fbdest;
            fbdest = NULL;
        }
    }
}

AMIGA_HOT bool denise_lace(void) { return (bplcon0 & 0x0004) != 0; }

// ---- registers ---------------------------------------------------------------
AMIGA_HOT void denise_write(uint32_t reg, uint16_t v, int hpos) {
    render_to(hpos);
    if (reg >= R_COLOR00 && reg < R_COLOR00 + 64) {
        color[(reg - R_COLOR00) >> 1] = v & 0xFFF;
        lut_dirty = true;
        return;
    }
    if (reg >= R_BPL1PTH && reg < R_BPL1PTH + 24) {
        int p = (reg - R_BPL1PTH) >> 2;
        if (reg & 2) bplpt[p] = (bplpt[p] & 0xFFFF0000) | (v & 0xFFFE);
        else         bplpt[p] = (bplpt[p] & 0x0000FFFF) | ((uint32_t)(v & 7) << 16);
        return;
    }
    if (reg >= R_SPR0PTH && reg < R_SPR0PTH + 32) {
        int s = (reg - R_SPR0PTH) >> 2;
        if (reg & 2) sprpt[s] = (sprpt[s] & 0xFFFF0000) | (v & 0xFFFE);
        else         sprpt[s] = (sprpt[s] & 0x0000FFFF) | ((uint32_t)(v & 7) << 16);
        return;
    }
    if (reg >= R_SPR0POS && reg < R_SPR0POS + 64) {
        int s = (reg - R_SPR0POS) >> 3;
        switch (reg & 6) {
        case 0: sprpos[s] = v; spr_decode(s); break;
        case 2: sprctl[s] = v; spr_decode(s); spr_armed[s] = false; break;
        case 4: sprdata[s] = v; spr_armed[s] = true; break;
        default: sprdatb[s] = v; break;
        }
        sprite_compose();
        return;
    }
    switch (reg) {
    case R_BPLCON0: bplcon0 = v; lut_dirty = true; break;
    case R_BPLCON1: bplcon1 = v; break;
    case R_BPLCON2: bplcon2 = v; lut_dirty = true; break;
    case R_BPL1MOD: bplmod[0] = (int16_t)(v & 0xFFFE); break;
    case R_BPL2MOD: bplmod[1] = (int16_t)(v & 0xFFFE); break;
    case R_DIWSTRT: diwstrt = v; break;
    case R_DIWSTOP: diwstop = v; break;
    case R_DDFSTRT: ddfstrt = v; if (fetch_k == 0) compute_ddf(); break;
    case R_DDFSTOP: ddfstop = v; if (fetch_k == 0) compute_ddf(); break;
    default: break;
    }
}

void denise_reset(void) {
    tables_init();
    bplcon0 = bplcon1 = bplcon2 = 0;
    diwstrt = 0x2C81; diwstop = 0xF4C1; ddfstrt = 0x38; ddfstop = 0xD0;
    memset(bplpt, 0, sizeof bplpt);
    bplmod[0] = bplmod[1] = 0;
    memset(color, 0, sizeof color);
    lut_dirty = true;
    memset(sprpt, 0, sizeof sprpt);
    memset(sprpos, 0, sizeof sprpos); memset(sprctl, 0, sizeof sprctl);
    memset(spr_armed, 0, sizeof spr_armed);
    memset(spl, 0, sizeof spl);
    spl_lo = SPL_SIZE; spl_hi = 0;
    denise_frame_start();
}
