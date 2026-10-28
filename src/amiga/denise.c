// Bitplane + sprite DMA (the Agnus side) and the Denise pixel pipeline.
//
// The current line is rendered INCREMENTALLY: every write to a display
// register first brings the line up to its hpos (the bitplane fetches, f_to,
// and the pixels the beam has passed, o_to) with the OLD register values, and
// only then applies the write.  That gives copper colour bars, mid-line
// BPLCON changes and split screens at pixel accuracy.
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
//
// Nothing the emulation does reads Denise back (pixels, plane pointers; there
// are no collisions), so it runs as the consumer of an event queue: the
// denise_* calls of the scheduler and the register writes are queued with the
// state they need (beam line, DMACON, LOF), and the c_* functions below
// replay them.  In sync mode (the default) each event is replayed at once; in
// async mode (amiga_render_async) another core replays them through
// amiga_render_pump(), in small steps, and the frame waits for it to finish
// (denise_sync).  Whichever core holds c_lock replays: the emulation, when it
// would wait (the frame end, a full queue), replays events itself instead, so
// the other core takes what it has time for (its scanout comes first) and
// the work is never slower than doing it all on one core.  The chip RAM reads
// (bitplane and sprite DMA) are the emulation's own, at the event's time, as
// the hardware's: the words go to the stages through a ring (ws[]) and as
// events.  Read by the stages instead, they saw what the CPU or blitter wrote
// after the beam had passed: IK+ erases and redraws its fighters ~50 lines
// behind the beam, and with the stages a frame behind they flickered.
#include <stdatomic.h>
#include <string.h>
#include "amiga_int.h"
#ifdef DENISE_DEBUG
#include <stdio.h>
#endif

#define PF_SIZE  1024
#define SPL_SIZE 512

// The output stage's registers.
static uint16_t bplcon0, bplcon2;
static uint16_t diwstrt = 0x2C81, diwstop = 0xF4C1;
// The bitplane DMA side of Agnus, twice: the emulation runs it as it goes
// (dma_st: the pointers and modulos, the chip RAM reads into the word ring)
// and the fetch stage replays it (fetch_st: spreads the words into pf_ring).
// Both apply the same register writes at the same points, so they walk the
// same fetch units in the same order.
typedef struct {
    uint16_t bplcon0, bplcon1;
    uint16_t diwstrt, diwstop, ddfstrt, ddfstop;
    bool     bpl_dma;                       // this line fetches
    int      fetch_k, nunits, ds, unit_cc;
    int      r_h;                           // done up to this colour clock
    int      vpos;
    uint16_t dmacon;
} agnus_t;
#define AGNUS_INIT { .diwstrt = 0x2C81, .diwstop = 0xF4C1, .ddfstrt = 0x38, .ddfstop = 0xD0 }
static agnus_t  dma_st = AGNUS_INIT, fetch_st = AGNUS_INIT;
static uint32_t bplpt[6];                   // the DMA side's
static int16_t  bplmod[2];
static uint16_t color[32];

// Bitplane words from the DMA side to the fetch stage, in fetch-unit order.
// The emulation waits at a line start while the ring has no room for a
// worst-case line (WS_LINE: 25 units x 4 planes x 2 words), helping.
#ifndef WS_SIZE
#define WS_SIZE 8192                        // words, a power of two: 16 KB, 40-80 lines
#endif
#define WS_LINE 200
static uint16_t ws[WS_SIZE];
static _Atomic uint32_t ws_head, ws_tail;

// Sprites.  The DMA side (the emulation): pointers, the per-line state
// machine, the positions it needs; what it reads goes to the output stage as
// events, like the CPU's writes.  The output stage: what sprite_compose draws.
static uint32_t sprpt[8];
static uint16_t dma_sprpos[8], dma_sprctl[8], spr_vstart[8], spr_vstop[8];
static uint8_t  spr_dma[8];
enum { SPR_IDLE, SPR_FETCH_CTL, SPR_WAIT, SPR_DATA };
static uint16_t sprpos[8], sprctl[8], sprdata[8], sprdatb[8];
static bool     spr_armed[8];
static bool     spl_dirty;                  // spl[] is stale: compose before the next pixels

// Per-line render state.  pf[] is the line's playfield, written by the fetch
// stage and read by the output stage a line or more behind: a ring.
#define PF_RING 4
static uint8_t  pf_ring[PF_RING][PF_SIZE];
static uint8_t *pf_f = pf_ring[0], *pf_o = pf_ring[0];
static uint8_t  spl[SPL_SIZE];
static int      spl_lo, spl_hi;           // dirty range of spl[]
static int      r_h;                      // output: rendered up to this colour clock
static bool     vwin;
static amiga_row_t *fbrow;              // row being written, or NULL
static amiga_row_t *fbdest;             // overlay row: where fbrow (the scratch) goes at line end
static amiga_row_t  scratch_row;
static int          ov_y0, ov_y1;       // amiga_set_overlay_rows
static int          fb_x0 = AMIGA_FB_X0_CENTRE;            // DIW x of column 0, this line
static volatile int fb_x0_next = AMIGA_FB_X0_CENTRE;       // amiga_set_fb_x0

void amiga_set_fb_x0(int x0) { fb_x0_next = x0; }

void amiga_set_overlay_rows(int y0, int y1) { ov_y0 = y0; ov_y1 = y1; }
static uint16_t ham_col;
static bool     ham_in;

// The line being replayed: the producer's state when it started (output
// stage; the fetch stage's are in fetch_st).
static int      c_vpos, c_lines;
static uint16_t c_dmacon;
static bool     c_lof;

// Profile buckets only in sync mode: in async mode the consumer is another
// core, and the profile is core 0's (its time shows as AMIGA_PROF_CORE1).
static _Atomic bool async_on;
static inline bool is_async(void) { return atomic_load_explicit(&async_on, memory_order_relaxed); }
static inline int dprof(int cat) { return is_async() ? cat : prof_switch(cat); }

// ---- helpers -----------------------------------------------------------------
AMIGA_HOT static int planes(uint16_t con0) {
    int np = (con0 >> 12) & 7;
    if (con0 & 0x8000) { if (np > 4) np = 4; }
    else if (np > 6) np = 6;
    return np;
}

// Bit-spread tables: one plane word -> per-pixel bytes holding 0/1.
static uint32_t spread4[16];     // nibble -> 4 bytes (hires: 1 byte per pixel)
static uint32_t spread8[256][2]; // byte -> 8 bytes, pixels 0-3 and 4-7

static void tables_init(void) {
    for (int n = 0; n < 16; n++) {
        uint32_t v = 0;
        for (int i = 0; i < 4; i++) {
            int bit = (n >> (3 - i)) & 1;     // MSB is the leftmost pixel
            v |= (uint32_t)bit << (8 * i);
        }
        spread4[n] = v;
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

// Fetch units are 8 colour clocks in both resolutions (hires fetches two words
// per plane per unit).  Units start at DDFSTRT + 8k; the unit that begins at
// or after DDFSTOP is the last one, as in the OCS sequencer.  So lowres
// $38/$D0 gives 20 units (20 words), hires $3C/$D4 and $3C/$D0 both 20 units
// (40 words) - the latter is what AROS programs for a 640-pixel screen.
AMIGA_HOT static void compute_ddf(agnus_t *f) {
    int a = f->ddfstrt & 0xFC, b = f->ddfstop & 0xFC;
    if (a < 0x18) a = 0x18;
    if (b > 0xD8) b = 0xD8;
    f->ds = a;
    f->unit_cc = 8;
    f->nunits = b < a ? 0 : ((b - a + 7) >> 3) + 1;
}

// DMA side: unit k's chip RAM reads (PSRAM latency: the BPLRD bucket), into
// the word ring.  A hires plane's two words are one 32-bit read when they
// share a word.
AMIGA_HOT static void dma_unit(int k) {
    const bool hires = dma_st.bplcon0 & 0x8000;
    const int np = planes(dma_st.bplcon0);
    const uint8_t *cr = A.cfg.chip_ram;
    const uint32_t cm = A.chip_mask & ~1u;
    uint32_t hd = atomic_load_explicit(&ws_head, memory_order_relaxed);
    for (int p = 0; p < np; p++) {
        const uint32_t a = bplpt[p] & cm;
        if (hires && !(a & 2)) {
            uint32_t v;
            memcpy(&v, cr + a, 4);
            v = __builtin_bswap32(v);
            ws[hd++ & (WS_SIZE - 1)] = (uint16_t)(v >> 16);
            ws[hd++ & (WS_SIZE - 1)] = (uint16_t)v;
        } else {
            ws[hd++ & (WS_SIZE - 1)] = U16BE(cr + a);
            if (hires) ws[hd++ & (WS_SIZE - 1)] = U16BE(cr + ((bplpt[p] + 2) & cm));
        }
        bplpt[p] += hires ? 4 : 2;
    }
    atomic_store_explicit(&ws_head, hd, memory_order_release);
    if (k == dma_st.nunits - 1)
        for (int p = 0; p < np; p++) bplpt[p] += (uint32_t)(int32_t)bplmod[p & 1];
}

// Planes first, first + step, ... of a unit's words, at hires-pixel position pos:
// eight pixels at a time, one byte of every plane through spread8.
AMIGA_HOT static void spread_planes(const uint16_t wd[6][2], int np, int first, int step, int pos, bool hires) {
    if (hires) {
        for (int j = 0; j < 2; j++, pos += 16) {
            if (pos < 0 || pos + 16 > PF_SIZE) continue;
            for (int s = 8, o = 0; s >= 0; s -= 8, o += 8) {
                uint32_t lo = 0, hi = 0;
                for (int p = first; p < np; p += step) {
                    const uint32_t *t = spread8[(wd[p][j] >> s) & 0xFF];
                    lo |= t[0] << p;
                    hi |= t[1] << p;
                }
                or32(pf_f + pos + o, lo);
                or32(pf_f + pos + o + 4, hi);
            }
        }
    } else {
        if (pos < 0 || pos + 32 > PF_SIZE) return;
        for (int s = 8, o = 0; s >= 0; s -= 8, o += 16) {
            uint32_t q[2] = { 0, 0 };
            for (int p = first; p < np; p += step) {
                const uint32_t *t = spread8[(wd[p][0] >> s) & 0xFF];
                q[0] |= t[0] << p;
                q[1] |= t[1] << p;
            }
            // Each lowres pixel twice: bytes abcd -> aabb ccdd.
            for (int h = 0; h < 2; h++) {
                const uint32_t v = q[h];
                or32(pf_f + pos + o + 8 * h,
                     (v & 0xFF) | (v & 0xFF) << 8 | (v & 0xFF00) << 8 | (v & 0xFF00) << 16);
                or32(pf_f + pos + o + 8 * h + 4,
                     (v >> 16 & 0xFF) | (v >> 16 & 0xFF) << 8 | (v >> 8 & 0xFF0000) | (v & 0xFF000000));
            }
        }
    }
}

// Fetch stage: unit k's words, from the ring, spread into the playfield.
AMIGA_HOT static void fetch_unit(int k) {
    const agnus_t *f = &fetch_st;
    int t = f->ds + k * f->unit_cc;
    bool hires = f->bplcon0 & 0x8000;
    int np = planes(f->bplcon0);
    int x = hires ? 2 * (2 * t + 9) : 2 * (2 * t + 17);       // hires-pixel coordinate
    int d1 = 2 * (f->bplcon1 & 15), d2 = 2 * ((f->bplcon1 >> 4) & 15);
    uint16_t wd[6][2];
    uint32_t tl = atomic_load_explicit(&ws_tail, memory_order_relaxed);
    for (int p = 0; p < np; p++) {
        wd[p][0] = ws[tl++ & (WS_SIZE - 1)];
        if (hires) wd[p][1] = ws[tl++ & (WS_SIZE - 1)];
    }
    atomic_store_explicit(&ws_tail, tl, memory_order_release);
    // The planes with one scroll land on the same pixels, so a group of pixels
    // is built in a register and ORed in once (OR, not store: a scroll or DDF
    // change mid-line may overlap units).  Odd and even planes (PF1H, PF2H)
    // usually share the scroll: one group of all planes; a dual-playfield
    // scroller shifts them apart: two groups.
    if (d1 == d2) spread_planes(wd, np, 0, 1, x + d1, hires);
    else {
        spread_planes(wd, np, 0, 2, x + d1, hires);
        spread_planes(wd, np, 1, 2, x + d2, hires);
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
    const int f0 = fb_x0;
    if (w_mode == AMIGA_ROW_DIRECT) {
        uint16_t *d = row->px.rgb - f0;
        if (!hires) {
            for (int x = x0; x < x1; x++) d[x] = lut[pf_o[2 * x]];
        } else {
            for (int x = x0; x < x1; x++) {
                const uint16_t c0 = lut[pf_o[2 * x]], c1 = lut[pf_o[2 * x + 1]];
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
        const uint8_t *s = pf_o + 2 * x0;
        int n = 2 * (x1 - x0);
        uint32_t any = 0;
        for (; n >= 4; n -= 4, d += 4, s += 4) { uint32_t t; memcpy(&t, s, 4); memcpy(d, &t, 4); any |= t; }
        for (; n > 0; n--) { any |= *s; *d++ = *s++; }
        if (!(any & 0xF0F0F0F0u)) return;
        ilut_ident = false;
    }
    for (int x = x0; x < x1; x++) {
        uint8_t v0 = pf_o[2 * x], v1 = hires ? pf_o[2 * x + 1] : v0;
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
        uint8_t v = pf_o[2 * x];
        int d = v & 15;
        switch (v >> 4) {
        case 0: ham_col = color[d]; break;
        case 1: ham_col = (uint16_t)((ham_col & 0xFF0) | d); break;
        case 2: ham_col = (uint16_t)((ham_col & 0x0FF) | (d << 8)); break;
        default: ham_col = (uint16_t)((ham_col & 0xF0F) | (d << 4)); break;
        }
        int fx = x - fb_x0;
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
    const bool ham = (bplcon0 & 0x0800) && !hires && planes(bplcon0) == 6;

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
    const int f0 = fb_x0, f1 = fb_x0 + AMIGA_FB_W;
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
                uint8_t v = pf_o[2 * x];
                uint16_t c = sprite_mix(x, lut[v], lut_pf[v]);
                w_put(x - f0, c, c);
            }
        }
    } else {
        {
            for (int x = wa; x < wb; x++) {
                uint8_t v0 = pf_o[2 * x], v1 = pf_o[2 * x + 1];
                w_put(x - f0, sprite_mix(x, lut[v0], lut_pf[v0]), sprite_mix(x, lut[v1], lut_pf[v1]));
            }
        }
    }
}

// The fetch units that start before colour clock h: the DMA side reads them,
// the fetch stage spreads them.
static inline bool units_due(const agnus_t *f, int h) {
    return f->bpl_dma && f->fetch_k < f->nunits && f->ds + f->fetch_k * f->unit_cc < h;
}

AMIGA_HOT static void units_to(agnus_t *f, int h, void (*unit)(int)) {
    while (units_due(f, h)) {
        unit(f->fetch_k);
        f->fetch_k++;
    }
    f->r_h = h;
}

AMIGA_HOT static void dma_to(int h) {
    if (h > AMIGA_CC_PER_LINE) h = AMIGA_CC_PER_LINE;
    if (h <= dma_st.r_h) return;
    if (!units_due(&dma_st, h)) { dma_st.r_h = h; return; }
    const int prof = prof_switch(AMIGA_PROF_BPLRD);
    units_to(&dma_st, h, dma_unit);
    prof_switch(prof);
}

AMIGA_HOT static void f_to(int h) {
    if (h > AMIGA_CC_PER_LINE) h = AMIGA_CC_PER_LINE;
    if (h <= fetch_st.r_h) return;
    if (!units_due(&fetch_st, h)) { fetch_st.r_h = h; return; }
    const int prof = dprof(AMIGA_PROF_BPL);
    units_to(&fetch_st, h, fetch_unit);
    dprof(prof);
}

// Output stage: the pixels before colour clock h.  Their playfield is complete
// once the fetch stage got to h: a unit starting at h or later puts its first
// pixel at DIW x 2h + 9 at the earliest.
static void sprite_compose(void);
AMIGA_HOT static void o_to(int h) {
    if (h > AMIGA_CC_PER_LINE) h = AMIGA_CC_PER_LINE;
    if (h <= r_h) return;
    const int prof = dprof(AMIGA_PROF_RENDER);
    if (spl_dirty) { sprite_compose(); spl_dirty = false; }
    output(2 * r_h, 2 * h);
    r_h = h;
    dprof(prof);
}

// ---- sprites -----------------------------------------------------------------
// The event queue's kinds (below): a sprite's DMA goes to the output stage as
// EV_SPR + s (POS, CTL) and EV_SPR + 8 + s (DATA, DATB).
enum { EV_LINE = 0x8000, EV_END = 0x8001, EV_FRAME = 0x8002, EV_SPR = 0x8010 };
static void push(uint16_t kind, uint16_t v, uint16_t h, uint16_t x);

AMIGA_HOT static void spr_decode(int s) {       // DMA side
    spr_vstart[s] = (uint16_t)((dma_sprpos[s] >> 8) | ((dma_sprctl[s] & 4) << 6));
    spr_vstop[s]  = (uint16_t)((dma_sprctl[s] >> 8) | ((dma_sprctl[s] & 2) << 7));
}

AMIGA_HOT static int spr_x(int s) {   // DIW x of the sprite's first pixel
    return (((sprpos[s] & 0xFF) << 1) | (sprctl[s] & 1)) + 1;
}

// DMA side, at a line start.
AMIGA_HOT static void sprite_dma(void) {
    const int vpos = A.vpos;
    bool on = (A.dmacon & (DMAF_MASTER | DMAF_SPRITE)) == (DMAF_MASTER | DMAF_SPRITE);
    if (!on || vpos < 25 || vpos >= A.lines - 1) return;
    for (int s = 0; s < 8; s++) {
        if (spr_dma[s] == SPR_IDLE) continue;
        if (spr_dma[s] == SPR_WAIT && vpos == spr_vstart[s]) spr_dma[s] = SPR_DATA;
        if (spr_dma[s] == SPR_DATA && vpos == spr_vstop[s]) spr_dma[s] = SPR_FETCH_CTL;
        if (spr_dma[s] == SPR_FETCH_CTL) {
            dma_sprpos[s] = chip_rw(sprpt[s]);
            dma_sprctl[s] = chip_rw(sprpt[s] + 2);
            sprpt[s] += 4;
            spr_decode(s);
            push((uint16_t)(EV_SPR + s), dma_sprpos[s], 0, dma_sprctl[s]);
            spr_dma[s] = (dma_sprpos[s] | dma_sprctl[s]) ? SPR_WAIT : SPR_IDLE;
            if (spr_dma[s] == SPR_WAIT && vpos == spr_vstart[s]) spr_dma[s] = SPR_DATA;
            if (spr_dma[s] != SPR_DATA) continue;
        }
        if (spr_dma[s] == SPR_DATA) {
            const uint16_t a = chip_rw(sprpt[s]), b = chip_rw(sprpt[s] + 2);
            sprpt[s] += 4;
            push((uint16_t)(EV_SPR + 8 + s), a, 0, b);
        }
    }
}

// Pixel x of a sprite starting at x0 (data a, b): its 2-bit colour, 0 outside.
static inline int spr_px(int x, int x0, uint16_t a, uint16_t b) {
    const unsigned i = (unsigned)(x - x0);
    if (i > 15) return 0;
    return ((a >> (15 - i)) & 1) | (((b >> (15 - i)) & 1) << 1);
}

// Output stage: spl[] from the sprites as they are now.
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
AMIGA_HOT static void c_frame_start(void) {       // output stage
    for (int s = 0; s < 8; s++) spr_armed[s] = false;
    spl_dirty = true;
}

static inline bool in_vwin(int vpos, uint16_t strt, uint16_t stop) {
    const int vstart = strt >> 8;
    const int vstop = (stop >> 8) | ((stop & 0x8000) ? 0 : 0x100);
    return vpos >= vstart && vpos < vstop;
}

// A line start on the DMA side / the fetch stage.
AMIGA_HOT static void a_line_start(agnus_t *f) {
    f->bpl_dma = in_vwin(f->vpos, f->diwstrt, f->diwstop) &&
                 (f->dmacon & (DMAF_MASTER | DMAF_RASTER)) == (DMAF_MASTER | DMAF_RASTER);
    compute_ddf(f);
    f->fetch_k = 0;
    f->r_h = 0;
}

// Fetch stage: line `seq`.
AMIGA_HOT static void f_line_start(uint32_t seq) {
    a_line_start(&fetch_st);
    pf_f = pf_ring[seq % PF_RING];
    clear_bytes(pf_f, PF_SIZE);
}

// Output stage: line `seq`.
AMIGA_HOT static void c_line_start(uint32_t seq) {
    vwin = in_vwin(c_vpos, diwstrt, diwstop);
    r_h = 0;
    pf_o = pf_ring[seq % PF_RING];
    ham_in = false;
    fb_x0 = fb_x0_next;
    int row = c_vpos - AMIGA_FB_Y0;
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
        if (bplcon0 & 0x0004)      w_begin(fbrow, AMIGA_ROW_LACE4, c_lof ? 0 : 1);
        else if (bplcon0 & 0x8000) w_begin(fbrow, AMIGA_ROW_IDX8, 0);
        else                       w_begin(fbrow, AMIGA_ROW_DIRECT, 0);
    }
#ifdef DENISE_DEBUG
    if (c_vpos == 120 && A.frame % 100 == 99) {
        printf("[denise] f%u con0=%04X con1=%04X con2=%04X diw=%04X/%04X ddf=%02X/%02X units=%d dma=%d\n",
               A.frame, bplcon0, fetch_st.bplcon1, bplcon2, diwstrt, diwstop, fetch_st.ddfstrt, fetch_st.ddfstop,
               fetch_st.nunits, fetch_st.bpl_dma);
    }
#endif
}

// The line's end, once it is rendered up to AMIGA_CC_PER_LINE.
AMIGA_HOT static void c_line_finish(void) {
    // The framebuffer reaches a few pixels past the end of the line: border.
    if (fbrow) {
        const int x = 2 * AMIGA_CC_PER_LINE - fb_x0;
        w_fill(x < 0 ? 0 : x, AMIGA_FB_W, color[0]);
        if (fbdest) {
            if (A.cfg.row_done) A.cfg.row_done(A.cfg.ctx, (int)(fbdest - A.cfg.framebuffer), fbrow);
            memcpy(fbdest, fbrow, sizeof *fbdest);
            fbrow = fbdest;
            fbdest = NULL;
        }
    }
}

// ---- registers ---------------------------------------------------------------
// A register write, once the side got to its hpos.  The DMA side and the
// fetch stage (the pointers and modulos are the DMA side's: denise_write):
AMIGA_HOT static void a_apply(agnus_t *f, uint32_t reg, uint16_t v) {
    switch (reg) {
    case R_BPLCON0: f->bplcon0 = v; break;
    case R_BPLCON1: f->bplcon1 = v; break;
    case R_DIWSTRT: f->diwstrt = v; break;
    case R_DIWSTOP: f->diwstop = v; break;
    case R_DDFSTRT: f->ddfstrt = v; if (f->fetch_k == 0) compute_ddf(f); break;
    case R_DDFSTOP: f->ddfstop = v; if (f->fetch_k == 0) compute_ddf(f); break;
    default: break;
    }
}

AMIGA_HOT static void f_apply(uint32_t reg, uint16_t v) { a_apply(&fetch_st, reg, v); }

// Output stage:
AMIGA_HOT static void c_apply(uint32_t reg, uint16_t v) {
    if (reg >= R_COLOR00 && reg < R_COLOR00 + 64) {
        // Copper colour bars and "chunky" backgrounds write a colour every few
        // pixels (Spaceballs' State of the Art: thousands a frame): a write
        // changes its one or two LUT entries, not the whole LUT (pico-speccy's
        // Next copper does the same per palette write).
        const int n = (reg - R_COLOR00) >> 1;
        const uint16_t c = v & 0xFFF;
        if (color[n] == c) return;
        color[n] = c;
        if (lut_dirty || (bplcon0 & 0x0400)) { lut_dirty = true; return; }   // dual playfield: rebuild
        // Single playfield: pf values n and n + 32 (EHB's half-bright) use it.
        lut[n] = c;
        lut[n + 32] = (uint16_t)((c >> 1) & 0x777);
        ilut[n] = ilut[n + 32] = 0xFF;
        if (n < 16) ilut_ident = false;
        return;
    }
    if (reg >= R_SPR0POS && reg < R_SPR0POS + 64) {
        int s = (reg - R_SPR0POS) >> 3;
        switch (reg & 6) {
        case 0: sprpos[s] = v; break;
        case 2: sprctl[s] = v; spr_armed[s] = false; break;
        case 4: sprdata[s] = v; spr_armed[s] = true; break;
        default: sprdatb[s] = v; break;
        }
        spl_dirty = true;
        return;
    }
    switch (reg) {
    case R_BPLCON0: bplcon0 = v; lut_dirty = true; break;
    case R_BPLCON2: bplcon2 = v; lut_dirty = true; break;
    case R_DIWSTRT: diwstrt = v; break;
    case R_DIWSTOP: diwstop = v; break;
    default: break;
    }
}

static void c_reset(void) {
    tables_init();
    bplcon0 = bplcon2 = 0;
    diwstrt = 0x2C81; diwstop = 0xF4C1;
    const agnus_t init = AGNUS_INIT;
    dma_st = fetch_st = init;
    memset(bplpt, 0, sizeof bplpt);
    bplmod[0] = bplmod[1] = 0;
    memset(color, 0, sizeof color);
    lut_dirty = true;
    memset(sprpt, 0, sizeof sprpt);
    memset(dma_sprpos, 0, sizeof dma_sprpos); memset(dma_sprctl, 0, sizeof dma_sprctl);
    memset(spr_vstart, 0, sizeof spr_vstart); memset(spr_vstop, 0, sizeof spr_vstop);
    for (int s = 0; s < 8; s++) spr_dma[s] = SPR_FETCH_CTL;
    memset(sprpos, 0, sizeof sprpos); memset(sprctl, 0, sizeof sprctl);
    memset(sprdata, 0, sizeof sprdata); memset(sprdatb, 0, sizeof sprdatb);
    memset(spr_armed, 0, sizeof spr_armed);
    memset(spl, 0, sizeof spl);
    memset(pf_ring, 0, sizeof pf_ring);
    pf_f = pf_o = pf_ring[0];
    spl_lo = SPL_SIZE; spl_hi = 0;
    c_frame_start();
}

// ---- the event queue -----------------------------------------------------------
// Single producer (the emulation), two consumer stages replaying the same events:
// the fetch stage (bitplane DMA into pf_ring), then the output stage (colours,
// sprites, the framebuffer).  The output stage replays an event only after the
// fetch stage has (its pixels before that hpos are then fetched), and the fetch
// stage starts line L only once the output stage has finished line L -
// PF_RING.  Each stage has its own lock, so the two can run on two cores at
// once; in sync mode one core replays both right away.  The chip RAM reads
// are the producer's (the DMA side, dma_*: at the event's time, as the
// hardware's DMA), so what the stages show does not depend on when they run.
typedef struct {
    uint16_t kind;          // EV_* (the enum above sprite_dma), or the register written (< $200)
    uint16_t v;             // the value; EV_LINE: DMACON; EV_SPR: POS / DATA
    uint16_t h;             // a write: hpos; EV_LINE: vpos
    uint16_t x;             // EV_LINE: LOF | lines << 1; EV_SPR: CTL / DATB
} dev_t;

#define EVQ 1024                        // power of two
#define STEP_CC 32                      // async: colour clocks per pump step
static dev_t            evq[EVQ];
static _Atomic uint32_t ev_head;        // producer
static _Atomic uint32_t f_tail, o_tail; // next event of each stage
static _Atomic uint32_t o_lines_done;   // lines the output stage has finished
static uint32_t         f_lines, o_lines;   // lines each stage has started
static dev_t            f_ev, o_ev;     // the event a stage is in (async: may take steps)
static bool             f_busy, o_busy;
static atomic_flag      f_lock = ATOMIC_FLAG_INIT, o_lock = ATOMIC_FLAG_INIT;

static inline int ev_hpos(const dev_t *e) {
    const int h = e->kind == EV_END ? AMIGA_CC_PER_LINE : e->h;
    return h > AMIGA_CC_PER_LINE ? AMIGA_CC_PER_LINE : h;
}

static inline void tail_next(_Atomic uint32_t *t) {
    atomic_store_explicit(t, atomic_load_explicit(t, memory_order_relaxed) + 1, memory_order_release);
}

// One step of the fetch stage: false when it cannot go on.
AMIGA_HOT static bool f_step(int step) {
    if (!f_busy) {
        const uint32_t t = atomic_load_explicit(&f_tail, memory_order_relaxed);
        if (t == atomic_load_explicit(&ev_head, memory_order_acquire)) return false;
        f_ev = evq[t & (EVQ - 1)];
        if (f_ev.kind == EV_LINE &&
            f_lines - atomic_load_explicit(&o_lines_done, memory_order_acquire) >= PF_RING)
            return false;                                     // its pf is still being output
        f_busy = true;
    }
    const dev_t *e = &f_ev;
    if (e->kind < EV_LINE || e->kind == EV_END) {
        const int h = ev_hpos(e);
        if (h > fetch_st.r_h + step) { f_to(fetch_st.r_h + step); return true; }
        f_to(h);
        if (e->kind < EV_LINE) f_apply(e->kind, e->v);
    } else if (e->kind == EV_LINE) {
        fetch_st.vpos = e->h; fetch_st.dmacon = e->v;
        f_line_start(f_lines++);
    }
    f_busy = false;
    tail_next(&f_tail);
    return true;
}

// One step of the output stage: false when it cannot go on.
AMIGA_HOT static bool o_step(int step) {
    if (!o_busy) {
        const uint32_t t = atomic_load_explicit(&o_tail, memory_order_relaxed);
        if (t == atomic_load_explicit(&f_tail, memory_order_acquire)) return false;
        o_ev = evq[t & (EVQ - 1)];
        o_busy = true;
    }
    const dev_t *e = &o_ev;
    if (e->kind < EV_LINE || e->kind == EV_END) {
        const int h = ev_hpos(e);
        if (h > r_h + step) { o_to(r_h + step); return true; }
        o_to(h);
        if (e->kind == EV_END) {
            c_line_finish();
            atomic_store_explicit(&o_lines_done, o_lines, memory_order_release);
        } else {
            c_apply(e->kind, e->v);
        }
    } else if (e->kind == EV_LINE) {
        c_vpos = e->h; c_dmacon = e->v; c_lof = e->x & 1; c_lines = e->x >> 1;
        c_line_start(o_lines++);
    } else if (e->kind == EV_FRAME) {
        c_frame_start();
    } else {                                                  // EV_SPR: a sprite's DMA
        const int s = (e->kind - EV_SPR) & 7;
        if (e->kind < EV_SPR + 8) { sprpos[s] = e->v; sprctl[s] = e->x; spr_armed[s] = false; }
        else                      { sprdata[s] = e->v; sprdatb[s] = e->x; spr_armed[s] = true; }
        spl_dirty = true;
    }
    o_busy = false;
    tail_next(&o_tail);
    return true;
}

AMIGA_HOT static bool try_stage(atomic_flag *lock, bool (*stage)(int), int step) {
    if (atomic_flag_test_and_set_explicit(lock, memory_order_acquire)) return false;
    const bool did = stage(step);
    atomic_flag_clear_explicit(lock, memory_order_release);
    return did;
}

// One step of whichever stage this core can take, `out_first` or the fetch
// stage first.  The render core prefers the output stage (the heavier one,
// and serial); the emulation's core, helping while it waits, the fetch stage,
// so the two do not queue for the same one.  False: nothing to do now.
AMIGA_HOT static bool c_step_any(int step, bool out_first) {
    if (out_first) return try_stage(&o_lock, o_step, step) || try_stage(&f_lock, f_step, step);
    return try_stage(&f_lock, f_step, step) || try_stage(&o_lock, o_step, step);
}

static uint32_t c1_ticks;               // render time on the other core (its clock)

AMIGA_HOT bool amiga_render_pump(void) {
    if (!is_async()) return false;
    const uint32_t t0 = A.cfg.prof_clock ? A.cfg.prof_clock() : 0;
    const bool did = c_step_any(STEP_CC, true);
    if (did && A.cfg.prof_clock) c1_ticks += A.cfg.prof_clock() - t0;
    return did;
}

// The emulation's core, waiting: replay what it can itself (time charged to
// RENDER), else spin.
AMIGA_HOT static void c_help(void) {
    const int prof = prof_switch(AMIGA_PROF_RENDER);
    c_step_any(AMIGA_CC_PER_LINE, false);
    prof_switch(prof);
}

uint32_t denise_core1_ticks(void) { return c1_ticks; }

// Wait until both stages have replayed everything queued.
AMIGA_HOT void denise_sync(void) {
    if (!is_async()) return;
    const int prof = prof_switch(AMIGA_PROF_RWAIT);
    while (atomic_load_explicit(&o_tail, memory_order_acquire) !=
           atomic_load_explicit(&ev_head, memory_order_relaxed)) c_help();
    prof_switch(prof);
}

AMIGA_HOT static void push(uint16_t kind, uint16_t v, uint16_t h, uint16_t x) {
    const uint32_t hd = atomic_load_explicit(&ev_head, memory_order_relaxed);
    if (hd - atomic_load_explicit(&o_tail, memory_order_acquire) >= EVQ) {
        const int prof = prof_switch(AMIGA_PROF_RWAIT);       // the consumers are a queue behind
        while (hd - atomic_load_explicit(&o_tail, memory_order_acquire) >= EVQ) c_help();
        prof_switch(prof);
    }
    dev_t *e = &evq[hd & (EVQ - 1)];
    e->kind = kind; e->v = v; e->h = h; e->x = x;
    atomic_store_explicit(&ev_head, hd + 1, memory_order_release);
    if (!is_async()) {
        bool more;
        do {
            more = f_step(AMIGA_CC_PER_LINE);
            more |= o_step(AMIGA_CC_PER_LINE);
        } while (more);
    }
}

void amiga_render_async(bool on) {
    denise_sync();
    atomic_store(&async_on, on);
}

// ---- the scheduler's side: the DMA side runs here -----------------------------------
static inline uint32_t ws_used(void) {
    return atomic_load_explicit(&ws_head, memory_order_relaxed) -
           atomic_load_explicit(&ws_tail, memory_order_acquire);
}

AMIGA_HOT void denise_frame_start(void) {
    for (int s = 0; s < 8; s++) spr_dma[s] = SPR_FETCH_CTL;
    push(EV_FRAME, 0, 0, 0);
}

AMIGA_HOT void denise_line_start(void) {
    if (ws_used() > WS_SIZE - WS_LINE) {                      // the fetch stage is a ring behind
        const int prof = prof_switch(AMIGA_PROF_RWAIT);
        while (ws_used() > WS_SIZE - WS_LINE) c_help();
        prof_switch(prof);
    }
    dma_st.vpos = A.vpos; dma_st.dmacon = A.dmacon;
    a_line_start(&dma_st);
    push(EV_LINE, A.dmacon, (uint16_t)A.vpos, (uint16_t)((A.lof ? 1 : 0) | (A.lines << 1)));
    sprite_dma();
}

AMIGA_HOT void denise_line_end(void) {
    dma_to(AMIGA_CC_PER_LINE);
    push(EV_END, 0, 0, 0);
}

// The colours as queued: a write that repeats one changes nothing on screen
// and is not queued.  State of the Art's copper backgrounds write COLOR00 and
// two zeros every 12 colour clocks, 12,800 MOVEs a frame, two thirds of them
// repeats - each one a trip through both stages.  The copper tests it itself.
uint16_t denise_q_color[32];

AMIGA_HOT void denise_write(uint32_t reg, uint16_t v, int hpos) {
    const uint16_t h = (uint16_t)(hpos < 0 ? 0 : hpos);
    if (reg >= R_COLOR00) {                                   // the last display registers
        const int n = (reg - R_COLOR00) >> 1;
        const uint16_t c = v & 0xFFF;
        if (denise_q_color[n] == c) return;
        denise_q_color[n] = c;
    }
    if (reg >= R_SPR0PTH && reg < R_SPR0PTH + 32) {
        const int s = (reg - R_SPR0PTH) >> 2;
        if (reg & 2) sprpt[s] = (sprpt[s] & 0xFFFF0000) | (v & 0xFFFE);
        else         sprpt[s] = (sprpt[s] & 0x0000FFFF) | ((uint32_t)(v & 7) << 16);
        return;                                               // the output stage has no use for it
    }
    if (reg >= R_SPR0POS && reg < R_SPR0POS + 64) {
        const int s = (reg - R_SPR0POS) >> 3;
        if ((reg & 6) == 0) { dma_sprpos[s] = v; spr_decode(s); }
        if ((reg & 6) == 2) { dma_sprctl[s] = v; spr_decode(s); }
    } else {
        dma_to(h);                                            // the units before the write read first
        if (reg >= R_BPL1PTH && reg < R_BPL1PTH + 24) {
            const int p = (reg - R_BPL1PTH) >> 2;
            if (reg & 2) bplpt[p] = (bplpt[p] & 0xFFFF0000) | (v & 0xFFFE);
            else         bplpt[p] = (bplpt[p] & 0x0000FFFF) | ((uint32_t)(v & 7) << 16);
            return;
        }
        if (reg == R_BPL1MOD) { bplmod[0] = (int16_t)(v & 0xFFFE); return; }
        if (reg == R_BPL2MOD) { bplmod[1] = (int16_t)(v & 0xFFFE); return; }
        a_apply(&dma_st, reg, v);
    }
    push((uint16_t)reg, v, h, 0);
}

AMIGA_HOT bool denise_lace(void) { return (dma_st.bplcon0 & 0x0004) != 0; }

void denise_reset(void) {
    denise_sync();
    f_busy = o_busy = false;
    const uint32_t hd = atomic_load(&ev_head);
    atomic_store(&f_tail, hd);
    atomic_store(&o_tail, hd);
    f_lines = o_lines = 0;
    atomic_store(&o_lines_done, 0);
    atomic_store(&ws_head, 0);
    atomic_store(&ws_tail, 0);
    memset(denise_q_color, 0, sizeof denise_q_color);         // c_reset clears color[]
    c_reset();
}
