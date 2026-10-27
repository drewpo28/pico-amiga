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
static uint16_t ham_col;
static bool     ham_in;

// ---- helpers -----------------------------------------------------------------
static int planes(void) {
    int np = (bplcon0 >> 12) & 7;
    if (bplcon0 & 0x8000) { if (np > 4) np = 4; }
    else if (np > 6) np = 6;
    return np;
}

// Bit-spread tables: one plane word -> per-pixel bytes holding 0/1.
static uint32_t spread4[16];     // nibble -> 4 bytes (hires: 1 byte per pixel)
static uint64_t spread4x2[16];   // nibble -> 8 bytes (lowres: each pixel twice)

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
}

static inline void or32(uint8_t *p, uint32_t v) { uint32_t t; memcpy(&t, p, 4); t |= v; memcpy(p, &t, 4); }
static inline void or64(uint8_t *p, uint64_t v) { uint64_t t; memcpy(&t, p, 8); t |= v; memcpy(p, &t, 8); }

// Fetch units are 8 colour clocks in both resolutions (hires fetches two words
// per plane per unit).  Units start at DDFSTRT + 8k; the unit that begins at
// or after DDFSTOP is the last one, as in the OCS sequencer.  So lowres
// $38/$D0 gives 20 units (20 words), hires $3C/$D4 and $3C/$D0 both 20 units
// (40 words) - the latter is what AROS programs for a 640-pixel screen.
static void compute_ddf(void) {
    int a = ddfstrt & 0xFC, b = ddfstop & 0xFC;
    if (a < 0x18) a = 0x18;
    if (b > 0xD8) b = 0xD8;
    ds = a;
    unit_cc = 8;
    nunits = b < a ? 0 : ((b - a + 7) >> 3) + 1;
}

static void fetch_unit(int k) {
    int t = ds + k * unit_cc;
    bool hires = bplcon0 & 0x8000;
    int np = planes();
    int x = hires ? 2 * (2 * t + 9) : 2 * (2 * t + 17);       // hires-pixel coordinate
    int d1 = 2 * (bplcon1 & 15), d2 = 2 * ((bplcon1 >> 4) & 15);
    for (int p = 0; p < np; p++) {
        int pos = x + ((p & 1) ? d2 : d1);
        if (hires) {
            for (int j = 0; j < 2; j++, pos += 16) {
                uint16_t w = chip_rw(bplpt[p]);
                bplpt[p] += 2;
                if (pos < 0 || pos + 16 > PF_SIZE) continue;
                for (int n = 0; n < 4; n++)
                    or32(pf + pos + 4 * n, spread4[(w >> (12 - 4 * n)) & 15] << p);
            }
        } else {
            uint16_t w = chip_rw(bplpt[p]);
            bplpt[p] += 2;
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
static void ilut_reset(void) { memset(ilut, 0xFF, sizeof ilut); }

static void build_lut(void) {
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

static void w_begin(amiga_row_t *row, int mode, int field) {
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
}

static void w_to_direct(amiga_row_t *row) {
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

static inline int w_index(amiga_row_t *row, uint16_t c) {
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

static inline void w_put(int fx, uint16_t c0, uint16_t c1) {
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

// Window pixels [x0, x1) without sprites (DIW coordinates, already clipped to
// the framebuffer): the common case, kept free of per-pixel colour lookups.
static void w_plain(int x0, int x1, bool hires) {
    amiga_row_t *row = fbrow;
    const int f0 = AMIGA_FB_X0;
    for (int x = x0; x < x1; x++) {
        uint8_t v0 = pf[2 * x], v1 = hires ? pf[2 * x + 1] : v0;
        if (w_mode == AMIGA_ROW_DIRECT) {
            uint16_t c0 = lut[v0], c1 = lut[v1];
            row->px.rgb[x - f0] = c0 == c1 ? c0 : avg(c0, c1);
            continue;
        }
        uint8_t i0 = ilut[v0], i1 = ilut[v1];
        if ((i0 | i1) & 0x80) {                 // first use on this line: allocate
            int a = w_index(row, lut[v0]);
            int b = a >= 0 ? w_index(row, lut[v1]) : -1;
            if (b < 0) { w_put(x - f0, lut[v0], lut[v1]); continue; }   // overflow -> DIRECT
            ilut[v0] = (uint8_t)a; ilut[v1] = (uint8_t)b;
            i0 = (uint8_t)a; i1 = (uint8_t)b;
        }
        if (w_mode == AMIGA_ROW_IDX8) { row->px.idx[2 * (x - f0)] = i0; row->px.idx[2 * (x - f0) + 1] = i1; }
        else row->px.nib[w_field][x - f0] = (uint8_t)((i0 << 4) | i1);
    }
}

static void output_ham(int x0, int x1) {
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
static void output(int x0, int x1) {
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
    for (int x = a; x < wa; x++) w_put(x - f0, bg, bg);
    for (int x = wb; x < b; x++) w_put(x - f0, bg, bg);
    if (ham || wa >= wb) return;

    const bool sprites = spl_hi > wa && spl_lo < wb;
    if (!sprites) { w_plain(wa, wb, hires); return; }
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

static void render_to(int h) {
    if (h > AMIGA_CC_PER_LINE) h = AMIGA_CC_PER_LINE;
    if (h <= r_h) return;
    if (bpl_dma) {
        while (fetch_k < nunits && ds + fetch_k * unit_cc < h) {
            fetch_unit(fetch_k);
            fetch_k++;
        }
    }
    output(2 * r_h, 2 * h);
    r_h = h;
}

// ---- sprites -----------------------------------------------------------------
static void spr_decode(int s) {
    spr_vstart[s] = (uint16_t)((sprpos[s] >> 8) | ((sprctl[s] & 4) << 6));
    spr_vstop[s]  = (uint16_t)((sprctl[s] >> 8) | ((sprctl[s] & 2) << 7));
}

static int spr_x(int s) {   // DIW x of the sprite's first pixel
    return (((sprpos[s] & 0xFF) << 1) | (sprctl[s] & 1)) + 1;
}

static void sprite_dma(void) {
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

static inline int spr_bits(int s, int x) {
    if (!spr_armed[s]) return 0;
    int i = x - spr_x(s);
    if (i < 0 || i > 15) return 0;
    return ((sprdata[s] >> (15 - i)) & 1) | (((sprdatb[s] >> (15 - i)) & 1) << 1);
}

static void sprite_compose(void) {
    if (spl_hi > spl_lo) memset(spl + spl_lo, 0, (size_t)(spl_hi - spl_lo));
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
        for (int x = lo; x < hi; x++) {
            int ve = spr_bits(e, x), vo = spr_bits(o, x), ci;
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
void denise_frame_start(void) {
    for (int s = 0; s < 8; s++) { spr_dma[s] = SPR_FETCH_CTL; spr_armed[s] = false; }
}

void denise_line_start(void) {
    int vstart = diwstrt >> 8;
    int vstop = (diwstop >> 8) | ((diwstop & 0x8000) ? 0 : 0x100);
    vwin = A.vpos >= vstart && A.vpos < vstop;
    bpl_dma = vwin && (A.dmacon & (DMAF_MASTER | DMAF_RASTER)) == (DMAF_MASTER | DMAF_RASTER);
    compute_ddf();
    fetch_k = 0;
    r_h = 0;
    memset(pf, 0, sizeof pf);
    ham_in = false;
    int row = (int)A.vpos - AMIGA_FB_Y0;
    fbrow = (A.cfg.framebuffer && row >= 0 && row < AMIGA_FB_H) ? A.cfg.framebuffer + row : NULL;
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

void denise_line_end(void) {
    render_to(AMIGA_CC_PER_LINE);
    // The framebuffer reaches a few pixels past the end of the line: border.
    if (fbrow) {
        int x = 2 * AMIGA_CC_PER_LINE - AMIGA_FB_X0;
        for (; x < AMIGA_FB_W; x++) if (x >= 0) w_put(x, color[0], color[0]);
    }
}

bool denise_lace(void) { return (bplcon0 & 0x0004) != 0; }

// ---- registers ---------------------------------------------------------------
void denise_write(uint32_t reg, uint16_t v, int hpos) {
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
