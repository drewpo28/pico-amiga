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
//     per enabled plane at once (hires: two consecutive words per plane, ECS
//     super-hires: four);
//   * its 16 pixels appear from DIW x = 2t + 17 (hires: 2 x 16 from 2t + 9,
//     super-hires 4 x 16 from 2t + 5: two colour clocks after the unit's first
//     plane-1 word, as lowres and hires), delayed by
//     BPLCON1 PF1H (odd planes) / PF2H (even planes);
//   * pixels outside DIW, or on lines outside the vertical window, show COLOR00.
// So the standard DDFSTRT $38 / DIWSTRT $81 pair lines up, like the hardware.
//
// Playfield bits are accumulated per line in pf[] at hires resolution (two
// entries per lowres pixel), sprites in spl[] at lowres resolution; colours
// are resolved at output time, so palette writes affect only later pixels.
// Super-hires (ECS, at most 2 planes) has twice the pixels of the output: a pf
// entry holds a pair, left pixel in bits 0-1, right in bits 2-3, and its LUT
// colour is the pair's average (pf_color) - hires rows, 16 values, as 4-plane
// hires.
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
static uint16_t bplcon0, bplcon2, bplcon3, bplcon4 = 0x0011;
// The producer's BPLCON2 / BPLCON3 (AGA: RDRAM, BANK, LOCT decide a colour write's entry).
static uint16_t q_bplcon2, q_bplcon3;
static uint16_t diwstrt = 0x2C81, diwstop = 0xF4C1, diwhigh;
// The bitplane DMA side of Agnus, twice: the emulation runs it as it goes
// (dma_st: the pointers and modulos, the chip RAM reads into the word ring)
// and the fetch stage replays it (fetch_st: spreads the words into pf_ring).
// Both apply the same register writes at the same points, so they walk the
// same fetch units in the same order.
typedef struct {
    uint16_t bplcon0, bplcon1;
    uint16_t diwstrt, diwstop, diwhigh, ddfstrt, ddfstop;
    uint16_t fmode;                         // AGA: FMODE (bits 0-1 the bitplane bus width)
    bool     bpl_dma;                       // this line fetches
    int      fetch_k, nunits, ds, unit_cc;
    int      r_h;                           // done up to this colour clock
    int      vpos;
    uint16_t dmacon;
} agnus_t;
#define AGNUS_INIT { .diwstrt = 0x2C81, .diwstop = 0xF4C1, .ddfstrt = 0x38, .ddfstop = 0xD0 }
static agnus_t  dma_st = AGNUS_INIT, fetch_st = AGNUS_INIT;
static uint32_t bplpt[8];                   // the DMA side's (AGA: 8 planes)
static int16_t  bplmod[2];
// The palette: 32 colours, or AGA's 256 (Lisa: BPLCON3's BANK picks the 32 a
// COLORxx write sets).  12 bits each: the output's depth, so an AGA write with
// LOCT (the low nibbles of 8-bit guns) changes nothing on screen and is dropped
// before the queue (denise_write); without LOCT it sets both halves, as Lisa.
static uint16_t color[256];

// Bitplane words from the DMA side to the fetch stage, in fetch-unit order.
// The emulation waits at a line start while the ring has no room for a
// worst-case line (WS_LINE: 25 units x 4 planes x 2 words), helping.
#ifndef WS_SIZE
#define WS_SIZE 8192                        // words, a power of two: 16 KB, 40-80 lines
#endif
#define WS_LINE 800                         // AGA: 8 planes x 25 units x 4 words
static uint16_t ws[WS_SIZE];
static _Atomic uint32_t ws_head, ws_tail;

// Sprites.  The DMA side (the emulation): pointers, the per-line state
// machine, the positions it needs; what it reads goes to the output stage as
// events, like the CPU's writes.  The output stage: what sprite_compose draws.
static uint32_t sprpt[8];
static uint16_t dma_sprpos[8], dma_sprctl[8], spr_vstart[8], spr_vstop[8];
static uint8_t  spr_dma[8];
enum { SPR_FETCH_CTL, SPR_WAIT, SPR_DATA };
static uint16_t sprpos[8], sprctl[8], sprdata[8], sprdatb[8];
// AGA: FMODE's SPR32 / SPR64 make a sprite 32 or 64 pixels wide: words 1-3 of its
// DATA / DATB (word 0 is sprdata / sprdatb), and how many it has (0 = 1).
static uint16_t sprdx[8][3], sprdbx[8][3];
static uint8_t  spr_nw[8];
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
static int          ov_y0[2], ov_y1[2]; // amiga_set_overlay_rows
static int          fb_x0 = AMIGA_FB_X0_CENTRE;            // DIW x of column 0, this line
static volatile int fb_x0_next = AMIGA_FB_X0_CENTRE;       // amiga_set_fb_x0

static int          fb_y0 = AMIGA_FB_Y0;                   // beam line of row 0, this frame

void amiga_set_fb_x0(int x0) { fb_x0_next = x0; }

void amiga_set_overlay_rows(int which, int y0, int y1) {
    if (which < 0 || which > 1) return;
    ov_y0[which] = y0; ov_y1[which] = y1;
}
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
enum { LORES, HIRES, SHRES };
AMIGA_HOT static inline int res(uint16_t con0) {
    if ((con0 & 0x0040) && A.ecs) return SHRES;
    return (con0 & 0x8000) ? HIRES : LORES;
}

AMIGA_HOT static inline int planes(uint16_t con0) {
    if (A.aga) {                                              // BPU3 (bit 4): 8 planes, any resolution
        const int n = ((con0 >> 12) & 7) | (con0 & 0x0010 ? 8 : 0);
        return n > 8 ? 8 : n;
    }
    const int np = (con0 >> 12) & 7, r = res(con0);
    const int max = r == LORES ? 6 : r == HIRES ? 4 : 2;
    return np > max ? max : np;
}

// The display window (DIW coordinates; vertical: beam lines).  OCS: start
// H8 = 0, V8 = 0; stop H8 = 1, V8 = NOT V7.  ECS: DIWHIGH gives those bits
// (V8-V10 as well) once written, until the next DIWSTRT/DIWSTOP write
// (diwhigh = 0: not written; else bit 15 set).
#define DIWHIGH_ON 0x8000
static inline int diw_vstart(uint16_t strt, uint16_t high) {
    return (strt >> 8) | (high ? (high & 7) << 8 : 0);
}
static inline int diw_vstop(uint16_t stop, uint16_t high) {
    return (stop >> 8) | (high ? ((high >> 8) & 7) << 8 : ((stop & 0x8000) ? 0 : 0x100));
}
static inline int diw_hstart(uint16_t strt, uint16_t high) {
    return (strt & 0xFF) | (high ? ((high >> 5) & 1) << 8 : 0);
}
static inline int diw_hstop(uint16_t stop, uint16_t high) {
    return (stop & 0xFF) | (high ? ((high >> 13) & 1) << 8 : 0x100);
}

// Bit-spread tables: one plane word -> per-pixel bytes holding 0/1.
static uint32_t spread4[16];     // nibble -> 4 bytes (hires: 1 byte per pixel)
static uint32_t spread8[256][2]; // byte -> 8 bytes, pixels 0-3 and 4-7
static uint16_t spread_sh[16];   // super-hires: nibble -> 2 pf entries, pixel pairs in bits 0 and 2

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
    for (int n = 0; n < 16; n++) {
        unsigned v = 0;
        for (int j = 0; j < 4; j++)
            v |= ((n >> (3 - j)) & 1u) << (8 * (j >> 1) + 2 * (j & 1));
        spread_sh[n] = (uint16_t)v;
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
// AGA's FMODE widens the bus to 32 or 64 bits (WinUAE's fetchunits[]): a unit is
// then 16 or 32 colour clocks in lowres, 8 or 16 in hires, still 8 in super-
// hires, and brings 1, 2 or 4 words of each plane (the unit's length times the
// resolution's pixels per colour clock over 16).  Workbench 3.1 on an A1200 runs
// its hires screen with FMODE 3 and DDF $38/$D8: 11 units, 88 bytes a plane.
static const uint8_t fetch_units[3][3] = { { 8, 8, 8 }, { 16, 8, 8 }, { 32, 16, 8 } };
static const uint8_t fm_maxplane[3][3] = { { 8, 4, 2 }, { 8, 8, 4 }, { 8, 8, 8 } };
static inline int fmode_i(const agnus_t *f) {
    const int fm = A.aga ? f->fmode & 3 : 0;
    return fm == 0 ? 0 : fm == 3 ? 2 : 1;
}
static inline int unit_words(const agnus_t *f) {
    const int r = res(f->bplcon0);
    return fetch_units[fmode_i(f)][r] * (2 << r) / 16;
}
AMIGA_HOT static void compute_ddf(agnus_t *f) {
    int a = f->ddfstrt & 0xFC, b = f->ddfstop & 0xFC;
    if (a < 0x18) a = 0x18;
    if (b > 0xD8) b = 0xD8;
    const int u = fetch_units[fmode_i(f)][res(f->bplcon0)];
    f->ds = a;
    f->unit_cc = u;
    f->nunits = b < a ? 0 : (b - a + u - 1) / u + 1;
}

// DMA side: unit k's chip RAM reads (PSRAM latency: the BPLRD bucket), into
// the word ring.  A hires plane's two words (super-hires: each pair of its four)
// are one 32-bit read when they share a word.
AMIGA_HOT static void dma_unit(int k) {
    const int nw = unit_words(&dma_st);                 // words per plane
    const int np = planes(dma_st.bplcon0);
    // AGA's wide bus reads aligned 32 / 64 bits: the pointers' low bits are not used.
    const uint32_t align = A.aga && (dma_st.fmode & 3) ? ((dma_st.fmode & 3) == 3 ? 7u : 3u) : 0u;
    const uint8_t *cr = A.cfg.chip_ram;
    const uint32_t cm = A.chip_mask & ~1u;
    uint32_t hd = atomic_load_explicit(&ws_head, memory_order_relaxed);
    for (int p = 0; p < np; p++) {
        const uint32_t pt = bplpt[p] & ~align;
        if (nw == 1) {
            ws[hd++ & (WS_SIZE - 1)] = U16BE(cr + (pt & cm));
        } else {
            for (int j = 0; j < nw; j += 2) {
                const uint32_t a = (pt + 2u * (uint32_t)j) & cm;
                if (!(a & 2)) {
                    uint32_t v;
                    memcpy(&v, cr + a, 4);
                    v = __builtin_bswap32(v);
                    ws[hd++ & (WS_SIZE - 1)] = (uint16_t)(v >> 16);
                    ws[hd++ & (WS_SIZE - 1)] = (uint16_t)v;
                } else {
                    ws[hd++ & (WS_SIZE - 1)] = U16BE(cr + a);
                    ws[hd++ & (WS_SIZE - 1)] = U16BE(cr + ((a + 2) & cm));
                }
            }
        }
        bplpt[p] = pt + 2u * (uint32_t)nw;
    }
    if (A.load_planes && np && nw <= 2)                 // load test: planes 7 and 8's reads
        for (int p = 0; p < 2; p++)
            for (int j = 0; j < nw; j++)
                ws[hd++ & (WS_SIZE - 1)] = U16BE(cr + ((bplpt[p] + 0x2000u + 2u * (uint32_t)j) & cm));
    atomic_store_explicit(&ws_head, hd, memory_order_release);
    if (k == dma_st.nunits - 1)
        for (int p = 0; p < np; p++) bplpt[p] += (uint32_t)(int32_t)bplmod[p & 1];
}

// Planes first, first + step, ... of a unit's words, at hires-pixel position pos:
// eight pixels at a time, one byte of every plane through spread8.
AMIGA_HOT static void spread_planes(const uint16_t wd[8][4], int np, int first, int step, int pos, bool hires, int nw) {
    if (hires) {
        for (int j = 0; j < nw; j++, pos += 16) {
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
      for (int j = 0; j < nw; j++, pos += 32) {               // AGA: up to 4 words a plane
        if (pos < 0 || pos + 32 > PF_SIZE) continue;
        for (int s = 8, o = 0; s >= 0; s -= 8, o += 16) {
            uint32_t q[2] = { 0, 0 };
            for (int p = first; p < np; p += step) {
                const uint32_t *t = spread8[(wd[p][j] >> s) & 0xFF];
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
}

// Super-hires: a plane's four words (64 pixels) at pf position pos, 32 entries.
AMIGA_HOT static void spread_shres(const uint16_t wd[8][4], int np, int first, int step, int pos) {
    if (pos < 0 || pos + 32 > PF_SIZE) return;
    for (int k = 0; k < 8; k++) {
        uint32_t v = 0;
        for (int p = first; p < np; p += step) {
            const unsigned b = (wd[p][k >> 1] >> ((k & 1) ? 0 : 8)) & 0xFF;
            v |= ((uint32_t)spread_sh[b >> 4] | (uint32_t)spread_sh[b & 15] << 16) << p;
        }
        or32(pf_f + pos + 4 * k, v);
    }
}

// Fetch stage: unit k's words, from the ring, spread into the playfield.
AMIGA_HOT static void fetch_unit(int k) {
    const agnus_t *f = &fetch_st;
    int t = f->ds + k * f->unit_cc;
    const int r = res(f->bplcon0), nw = unit_words(f);
    int np = planes(f->bplcon0);
    // Pixels start 1.5 colour clocks after the unit's first plane-1 fetch, which
    // the cycle diagram puts at its slot (maxplane - 1): DIW x 2t + 2 x maxplane + 1,
    // i.e. 17 / 9 / 5 without FMODE.  A wider bus uses a wider diagram (WinUAE's
    // fm_maxplanes): hires with FMODE 3 takes lowres's 17 - Workbench 3.1 on AGA
    // lined up with its ECS screen to the pixel only so.
    int x = 2 * (2 * t + 2 * fm_maxplane[fmode_i(f)][r] + 1);   // hires-pixel coordinate
    int d1 = 2 * (f->bplcon1 & 15), d2 = 2 * ((f->bplcon1 >> 4) & 15);
    uint16_t wd[8][4];
    uint32_t tl = atomic_load_explicit(&ws_tail, memory_order_relaxed);
    for (int p = 0; p < np; p++)
        for (int j = 0; j < nw; j++) wd[p][j] = ws[tl++ & (WS_SIZE - 1)];
    uint16_t wx[8][4];
    const bool extra = A.load_planes && np && nw <= 2;   // load test: dma_unit's two more planes
    if (extra)
        for (int p = 0; p < 2; p++)
            for (int j = 0; j < nw; j++) wx[p][j] = ws[tl++ & (WS_SIZE - 1)];
    atomic_store_explicit(&ws_tail, tl, memory_order_release);
    if (extra) {                                         // spread as planes are, into a scratch line
        static uint8_t scratch[PF_SIZE];
        uint8_t *const keep = pf_f;
        pf_f = scratch;
        spread_planes(wx, 2, 0, 1, x + d1, r == HIRES, nw);
        pf_f = keep;
    }
    if (r == SHRES) {
        if (d1 == d2) spread_shres(wd, np, 0, 1, x + d1);
        else {
            spread_shres(wd, np, 0, 2, x + d1);
            spread_shres(wd, np, 1, 2, x + d2);
        }
        return;
    }
    const bool hires = r == HIRES;
    // The planes with one scroll land on the same pixels, so a group of pixels
    // is built in a register and ORed in once (OR, not store: a scroll or DDF
    // change mid-line may overlap units).  Odd and even planes (PF1H, PF2H)
    // usually share the scroll: one group of all planes; a dual-playfield
    // scroller shifts them apart: two groups.
    if (d1 == d2) spread_planes(wd, np, 0, 1, x + d1, hires, nw);
    else {
        spread_planes(wd, np, 0, 2, x + d1, hires, nw);
        spread_planes(wd, np, 1, 2, x + d2, hires, nw);
    }
}

// Playfield value -> colour, plus whether it is opaque and which playfield
// (1 or 2) owns it, for the sprite priority test.
static inline uint16_t avg(uint16_t a, uint16_t b) {
    return (uint16_t)(((a & 0xEEE) + (b & 0xEEE)) >> 1);
}
// Super-hires (ECS): a pixel pair p1, p2 selects COLOR[p2 * 4 + p1]; the left
// pixel shows its top two bits of each gun, the right one the low two (each
// widened to four bits) - 64 colours, the palette as graphics.library lays it
// out for these modes (WinUAE's linetoscr_*_sh does the same).
static inline uint16_t shres_hi(uint16_t c) { c &= 0xCCC; return (uint16_t)(c | c >> 2); }
static inline uint16_t shres_lo(uint16_t c) { c &= 0x333; return (uint16_t)(c << 2 | c); }

// AGA (Lisa): dual playfield's PF2 colours start at PF2OF (BPLCON3 bits 10-12),
// BPLCON4's BPLAM XORs a single playfield's value, EHB only with 6 planes and
// KILLEHB (BPLCON2 bit 9) clear.
static inline uint16_t pf_color_aga(uint8_t v, int *which) {
    if (bplcon0 & 0x0400) {                                   // dual playfield: 4 planes each
        const int p1 = (v & 1) | ((v >> 1) & 2) | ((v >> 2) & 4) | ((v >> 3) & 8);
        const int p2 = ((v >> 1) & 1) | ((v >> 2) & 2) | ((v >> 3) & 4) | ((v >> 4) & 8);
        static const uint8_t pf2of[8] = { 0, 2, 4, 8, 16, 32, 64, 128 };
        const int o2 = pf2of[(bplcon3 >> 10) & 7];
        const bool pri2 = bplcon2 & 0x40;
        if (pri2 ? p2 : !p1 && p2) { *which = 2; return color[(p2 + o2) & 255]; }
        if (p1) { *which = 1; return color[p1]; }
        *which = 0;
        return color[0];
    }
    *which = v ? 2 : 0;
    if ((v & 32) && planes(bplcon0) == 6 && !(bplcon2 & 0x0200) && !(bplcon0 & 0x0800))
        return (uint16_t)((color[(v & 31) ^ (bplcon4 >> 8 & 31)] >> 1) & 0x777);   // EHB
    return color[v ^ (bplcon4 >> 8)];
}

static inline uint16_t pf_color(uint8_t v, int *which) {
    if (A.aga && res(bplcon0) != SHRES) return pf_color_aga(v, which);
    if (res(bplcon0) == SHRES) {                              // a pixel pair (bits 0-1, 2-3)
        *which = v ? 2 : 0;
        const uint16_t c = color[v & 15];
        return avg(shres_hi(c), shres_lo(c));
    }
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

// Apply a sprite on top of a playfield pixel.
static inline uint16_t sprite_mix(int x, uint16_t c, int which) {
    uint8_t s = spl[x];
    if (!(s & 0x80)) return c;
    int pair = (s >> 4) & 3;
    if (which) {
        int pri = which == 1 ? (bplcon2 & 7) : ((bplcon2 >> 3) & 7);
        if (pair >= pri) return c;                             // playfield in front
    }
    // AGA: the sprites' 16 colours start at ESPRM x 16 (BPLCON4 bits 4-7; the odd
    // sprites' OSPRM is taken as the same: the pair is all spl[] keeps).
    return color[(A.aga ? (bplcon4 & 0xF0) : 16) + (s & 15)];
}

// Colour lookup for the current mode: pf value -> colour, and which playfield
// owns it (for sprite priority).  Rebuilt lazily after palette/BPLCON writes.
static uint16_t lut[256];                   // 64 used without AGA (6 planes)
static uint8_t  lut_pf[256];
static bool     lut_dirty = true;
// pf value -> palette index of the row being written (0xFF = not assigned yet);
// valid for one line and one LUT generation.
static uint8_t  ilut[256];
// Hires IDX8 row whose palette starts with lut[0..15] as entries 0..15 (w_begin)
// and whose LUT has not changed since: a pf value is its own index.
static bool     ilut_ident;
AMIGA_HOT static void ilut_reset(void) { fill_bytes(ilut, 0xFF, (int)sizeof ilut); ilut_ident = false; }

AMIGA_HOT static void build_lut(void) {
    const int n = A.aga ? 256 : 64;
    for (int v = 0; v < n; v++) {
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
// map[c] = generation << 6 | index: one 8 KB table (a tag table and an index
// table were 12 KB, and the menu's heap is what is left of SRAM).
static uint16_t map[4096];
static uint16_t map_gen;                // 1..1023, one per line
static uint16_t last_c = 0xFFFF;
static uint8_t  last_i;

AMIGA_HOT static void w_begin(amiga_row_t *row, int mode, int field) {
    ilut_reset();
    if (++map_gen == 1024) { clear_bytes((uint8_t *)map, (int)sizeof map); map_gen = 1; }   // every ~3 frames
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
            if (map[c] >> 6 != map_gen) map[c] = (uint16_t)(map_gen << 6 | v);
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
    if (map[c] >> 6 == map_gen) i = map[c] & 63;
    else {
        if (w_n == w_max) return -1;
        i = w_n++;
        row->pal[w_base + i] = c;
        row->ncol[w_field] = (uint8_t)w_n;
        map[c] = (uint16_t)(map_gen << 6 | i);
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

// The border's colour: COLOR00, or black with ECS BPLCON3 BRDRBLNK (ECSENA on).
static inline uint16_t border(void) {
    return (A.ecs && (bplcon0 & 1) && (bplcon3 & 0x20)) ? 0 : color[0];
}

// Pixels [x0, x1) of the current line, in DIW coordinates.
AMIGA_HOT static void output(int x0, int x1) {
    if (lut_dirty) build_lut();
    const int hstart = diw_hstart(diwstrt, diwhigh), hstop = diw_hstop(diwstop, diwhigh);
    const bool hires = res(bplcon0) != LORES;
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
    const uint16_t bg = border();
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

// DMA side, at a line start.  Agnus compares the beam with each sprite's VSTART
// and VSTOP every line, wherever they came from: the control words it fetched,
// or SPRxPOS / SPRxCTL written by the copper or the CPU (denise_write).  So a
// sprite whose control words were 0 (the end of its list) only waits: a later
// POS/CTL write with a VSTART ahead of the beam starts its data DMA there.
// The frame's first control words are fetched on line 25 alone, and only with
// sprite DMA on then: RoboCop 2 writes POS/CTL and pointers straight at the
// data in the vertical blank and turns sprite DMA on later - fetched as control
// words on the first DMA line, RoboCop's pixels became garbage and he vanished.
#define SPR_FIRST_LINE 25
AMIGA_HOT static void sprite_dma(void) {
    const int vpos = A.vpos;
    bool on = (A.dmacon & (DMAF_MASTER | DMAF_SPRITE)) == (DMAF_MASTER | DMAF_SPRITE);
    if (vpos == SPR_FIRST_LINE && !on)
        for (int s = 0; s < 8; s++) if (spr_dma[s] == SPR_FETCH_CTL) spr_dma[s] = SPR_WAIT;
    if (!on || vpos < SPR_FIRST_LINE || vpos >= A.lines - 1) return;
    for (int s = 0; s < 8; s++) {
        if (spr_dma[s] == SPR_WAIT && vpos == spr_vstart[s]) spr_dma[s] = SPR_DATA;
        if (spr_dma[s] == SPR_DATA && vpos == spr_vstop[s]) spr_dma[s] = SPR_FETCH_CTL;
        // AGA's SPR32 / SPR64 (FMODE bits 2-3): 2 or 4 words at a time, aligned; the
        // control words are the first of two such fetches, the data a DATA run then
        // a DATB run each line (WinUAE).
        const int fm = A.aga ? (dma_st.fmode >> 2) & 3 : 0;
        const unsigned nw = fm == 0 ? 1u : fm == 3 ? 4u : 2u;
        if (nw > 1) sprpt[s] &= ~(2u * nw - 1);
        if (spr_dma[s] == SPR_FETCH_CTL) {
            dma_sprpos[s] = chip_rw(sprpt[s]);
            dma_sprctl[s] = chip_rw(sprpt[s] + 2 * nw);
            sprpt[s] += 4 * nw;
            spr_decode(s);
            push((uint16_t)(EV_SPR + s), dma_sprpos[s], 0, dma_sprctl[s]);
            spr_dma[s] = vpos == spr_vstart[s] ? SPR_DATA : SPR_WAIT;
            if (spr_dma[s] != SPR_DATA) continue;
        }
        if (spr_dma[s] == SPR_DATA) {
            for (unsigned j = 0; j < nw; j++) {                  // h: the word, and how many
                const uint16_t a = chip_rw(sprpt[s] + 2 * j), b = chip_rw(sprpt[s] + 2 * (nw + j));
                push((uint16_t)(EV_SPR + 8 + s), a, (uint16_t)(j | nw << 4), b);
            }
            sprpt[s] += 4 * nw;
        }
    }
}

// Pixel x of sprite s starting at x0: its 2-bit colour, 0 outside (or unarmed).
// A sprite is 16 x spr_nw pixels of SPRES's size (BPLCON3 bits 6-7, AGA): lowres
// ones one spl[] entry each, hires / super-hires ones 2 / 4 to an entry (the
// entry shows the first; spl[] is lowres).
static inline int spr_px(int s, int x, int x0, int step) {
    if (!spr_armed[s]) return 0;
    const unsigned i = (unsigned)(x - x0) * (unsigned)step, w = 16u * (spr_nw[s] ? spr_nw[s] : 1u);
    if (i >= w) return 0;
    const unsigned k = i >> 4, bit = 15 - (i & 15);
    const uint16_t a = k ? sprdx[s][k - 1] : sprdata[s], b = k ? sprdbx[s][k - 1] : sprdatb[s];
    return ((a >> bit) & 1) | (((b >> bit) & 1) << 1);
}

// Output stage: spl[] from the sprites as they are now.
AMIGA_HOT static void sprite_compose(void) {
    if (spl_hi > spl_lo) clear_bytes(spl + spl_lo, spl_hi - spl_lo);
    spl_lo = SPL_SIZE; spl_hi = 0;
    for (int pair = 3; pair >= 0; pair--) {
        int e = 2 * pair, o = e + 1;
        if (!spr_armed[e] && !spr_armed[o]) continue;
        bool att = (sprctl[o] & 0x80) != 0;
        const int sres = A.aga ? (bplcon3 >> 6) & 3 : 0;
        const int step = sres == 2 ? 2 : sres == 3 ? 4 : 1;
        int lo = SPL_SIZE, hi = 0;
        for (int s = e; s <= o; s++) if (spr_armed[s]) {
            int a = spr_x(s);
            const int w = 16 * (spr_nw[s] ? spr_nw[s] : 1) / step;
            if (a < lo) lo = a;
            if (a + w > hi) hi = a + w;
        }
        if (lo < 0) lo = 0;
        if (hi > SPL_SIZE) hi = SPL_SIZE;
        // An unarmed sprite of the pair draws nothing.
        const int xe = spr_x(e), xo = spr_x(o);
        for (int x = lo; x < hi; x++) {
            int ve = spr_px(e, x, xe, step), vo = spr_px(o, x, xo, step), ci;
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
AMIGA_HOT static void c_frame_start(int y0) {     // output stage
    fb_y0 = y0;
    for (int s = 0; s < 8; s++) spr_armed[s] = false;
    spl_dirty = true;
}

static inline bool in_vwin(int vpos, uint16_t strt, uint16_t stop, uint16_t high) {
    return vpos >= diw_vstart(strt, high) && vpos < diw_vstop(stop, high);
}

// A line start on the DMA side / the fetch stage.
AMIGA_HOT static void a_line_start(agnus_t *f) {
    f->bpl_dma = in_vwin(f->vpos, f->diwstrt, f->diwstop, f->diwhigh) &&
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
    vwin = in_vwin(c_vpos, diwstrt, diwstop, diwhigh);
    r_h = 0;
    pf_o = pf_ring[seq % PF_RING];
    ham_in = false;
    fb_x0 = fb_x0_next;
    int row = c_vpos - fb_y0;
    fbrow = (A.cfg.framebuffer && row >= 0 && row < AMIGA_FB_H) ? A.cfg.framebuffer + row : NULL;
    fbdest = NULL;
    if (fbrow && ((row >= ov_y0[0] && row < ov_y1[0]) || (row >= ov_y0[1] && row < ov_y1[1]))) {
        // Built off-screen (the interlace writer keeps the other field: start
        // from the row as it is).
        fbdest = fbrow;
        memcpy(&scratch_row, fbdest, sizeof scratch_row);
        fbrow = &scratch_row;
    }
    if (fbrow) {
        if (lut_dirty) build_lut();
        if (bplcon0 & 0x0004)      w_begin(fbrow, AMIGA_ROW_LACE4, c_lof ? 0 : 1);
        else if (res(bplcon0) != LORES) w_begin(fbrow, AMIGA_ROW_IDX8, 0);
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
        w_fill(x < 0 ? 0 : x, AMIGA_FB_W, border());
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
    case R_BPLCON0: f->bplcon0 = v; if (A.aga && f->fetch_k == 0) compute_ddf(f); break;   // AGA: the unit's length
    case R_BPLCON1: f->bplcon1 = v; break;
    case R_FMODE:   f->fmode = v; if (f->fetch_k == 0) compute_ddf(f); break;
    case R_DIWSTRT: f->diwstrt = v; f->diwhigh = 0; break;
    case R_DIWSTOP: f->diwstop = v; f->diwhigh = 0; break;
    case R_DIWHIGH: f->diwhigh = (uint16_t)(v | DIWHIGH_ON); break;
    case R_DDFSTRT: f->ddfstrt = v; if (f->fetch_k == 0) compute_ddf(f); break;
    case R_DDFSTOP: f->ddfstop = v; if (f->fetch_k == 0) compute_ddf(f); break;
    case R_DMACON: {
        // Bitplane DMA turned off mid-line fetches no further unit (nor adds the
        // modulos); turned on, it fetches the units from here on.  A-Type's panel
        // turns it off at the top of its first line, rewrites the six pointers
        // after DDFSTRT and turns it on again on the next: fetched anyway, planes
        // 4 and 5 lagged 1-3 by a word (a grey copy of the text beside it).
        const bool was = f->bpl_dma;
        f->dmacon = v;
        f->bpl_dma = in_vwin(f->vpos, f->diwstrt, f->diwstop, f->diwhigh) &&
                     (v & (DMAF_MASTER | DMAF_RASTER)) == (DMAF_MASTER | DMAF_RASTER);
        if (f->bpl_dma && !was)
            while (f->fetch_k < f->nunits && f->ds + f->fetch_k * f->unit_cc < f->r_h) f->fetch_k++;
        break;
    }
    default: break;
    }
}

AMIGA_HOT static void f_apply(uint32_t reg, uint16_t v) { a_apply(&fetch_st, reg, v); }

// Output stage:
AMIGA_HOT static void c_apply(uint32_t reg, uint16_t v, uint16_t x) {
    if (reg >= R_COLOR00 && reg < R_COLOR00 + 64 && A.aga) {   // x: the palette entry (denise_write)
        const uint16_t c = v & 0xFFF;
        if (color[x & 255] == c) return;
        color[x & 255] = c;
        // A plain single playfield (no BPLAM, no EHB, not SHRES): pf value x is it.
        if (lut_dirty || (bplcon0 & 0x0400) || (bplcon4 & 0xFF00) || res(bplcon0) == SHRES ||
            planes(bplcon0) == 6) { lut_dirty = true; return; }
        lut[x] = c;
        ilut[x] = 0xFF;
        if (x < 16) ilut_ident = false;
        return;
    }
    if (reg >= R_COLOR00 && reg < R_COLOR00 + 64) {
        // Copper colour bars and "chunky" backgrounds write a colour every few
        // pixels (Spaceballs' State of the Art: thousands a frame): a write
        // changes its one or two LUT entries, not the whole LUT (pico-speccy's
        // Next copper does the same per palette write).
        const int n = (reg - R_COLOR00) >> 1;
        const uint16_t c = v & 0xFFF;
        if (color[n] == c) return;
        color[n] = c;
        if (lut_dirty || (bplcon0 & 0x0400) || res(bplcon0) == SHRES) { lut_dirty = true; return; }   // DPF, SHRES: rebuild
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
        case 4: sprdata[s] = v; spr_nw[s] = 1; spr_armed[s] = true; break;
        default: sprdatb[s] = v; break;
        }
        spl_dirty = true;
        return;
    }
    switch (reg) {
    case R_BPLCON0: bplcon0 = v; lut_dirty = true; break;
    case R_BPLCON2: bplcon2 = v; lut_dirty = true; break;
    case R_BPLCON3: if (A.ecs) bplcon3 = v; if (A.aga) lut_dirty = true; break;   // AGA: PF2OF
    case R_BPLCON4: bplcon4 = v; lut_dirty = true; break;
    case R_DIWSTRT: diwstrt = v; diwhigh = 0; break;
    case R_DIWSTOP: diwstop = v; diwhigh = 0; break;
    case R_DIWHIGH: diwhigh = (uint16_t)(v | DIWHIGH_ON); break;
    default: break;
    }
}

static void c_reset(void) {
    tables_init();
    bplcon0 = bplcon2 = bplcon3 = 0;
    bplcon4 = 0x0011;
    q_bplcon2 = q_bplcon3 = 0;
    diwstrt = 0x2C81; diwstop = 0xF4C1; diwhigh = 0;
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
    memset(sprdx, 0, sizeof sprdx); memset(sprdbx, 0, sizeof sprdbx); memset(spr_nw, 0, sizeof spr_nw);
    memset(spl, 0, sizeof spl);
    memset(pf_ring, 0, sizeof pf_ring);
    pf_f = pf_o = pf_ring[0];
    spl_lo = SPL_SIZE; spl_hi = 0;
    c_frame_start(AMIGA_FB_Y0);
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
            c_apply(e->kind, e->v, e->x);
        }
    } else if (e->kind == EV_LINE) {
        c_vpos = e->h; c_dmacon = e->v; c_lof = e->x & 1; c_lines = e->x >> 1;
        c_line_start(o_lines++);
    } else if (e->kind == EV_FRAME) {
        c_frame_start(e->v);
    } else {                                                  // EV_SPR: a sprite's DMA
        const int s = (e->kind - EV_SPR) & 7;
        if (e->kind < EV_SPR + 8) { sprpos[s] = e->v; sprctl[s] = e->x; spr_armed[s] = false; }
        else {
            const unsigned j = e->h & 15;                     // AGA: word j of e->h >> 4
            if (j == 0) { sprdata[s] = e->v; sprdatb[s] = e->x; spr_nw[s] = (uint8_t)(e->h >> 4); spr_armed[s] = true; }
            else if (j < 4) { sprdx[s][j - 1] = e->v; sprdbx[s][j - 1] = e->x; }
        }
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

// Vertical centring (amiga_set_vcentre), on the DMA side: the first and last
// line that fetched bitplanes this frame, and the beam line of row 0 the output
// stage gets with each frame (EV_FRAME), moved once a new one held VC_HOLD frames.
#define VC_HOLD 25
static volatile bool vc_on;
static int vc_first = -1, vc_last, vc_y0 = AMIGA_FB_Y0, vc_want, vc_count;

void amiga_set_vcentre(bool on) { vc_on = on; }

static int vc_target(void) {
    if (!vc_on) return AMIGA_FB_Y0;
    if (vc_first < 0) return vc_y0;                           // nothing shown: stay
    const int h = vc_last - vc_first + 1;
    int y = vc_first - (h < AMIGA_FB_H ? (AMIGA_FB_H - h) / 2 : 0);
    if (y < 0) y = 0;
    if (y > AMIGA_FB_Y0) y = AMIGA_FB_Y0;
    return y;
}

AMIGA_HOT void denise_frame_start(void) {
    for (int s = 0; s < 8; s++) spr_dma[s] = SPR_FETCH_CTL;
    const int want = vc_target();
    if (!vc_on || want == vc_y0) vc_y0 = want, vc_count = 0;
    else if (want == vc_want) { if (++vc_count >= VC_HOLD) vc_y0 = want, vc_count = 0; }
    else vc_want = want, vc_count = 1;
    vc_first = -1;
    push(EV_FRAME, (uint16_t)vc_y0, 0, 0);
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
    if (dma_st.fetch_k > 0 && planes(dma_st.bplcon0) > 0) {
        if (vc_first < 0) vc_first = A.vpos;
        vc_last = A.vpos;
    }
    push(EV_END, 0, 0, 0);
}

// The colours as queued: a write that repeats one changes nothing on screen
// and is not queued.  State of the Art's copper backgrounds write COLOR00 and
// two zeros every 12 colour clocks, 12,800 MOVEs a frame, two thirds of them
// repeats - each one a trip through both stages.  The copper tests it itself.
uint16_t denise_q_color[256];

AMIGA_HOT void denise_write(uint32_t reg, uint16_t v, int hpos) {
    const uint16_t h = (uint16_t)(hpos < 0 ? 0 : hpos);
    uint16_t x = 0;
    if (reg >= R_COLOR00) {                                   // the last display registers
        int n = (reg - R_COLOR00) >> 1;
        if (A.aga) {
            if (q_bplcon2 & 0x0100) return;                  // RDRAM: a read mode, writes ignored
            if (q_bplcon3 & 0x0200) return;                  // LOCT: low nibbles, not shown
            n += (q_bplcon3 >> 13) * 32;                     // BANK
            x = (uint16_t)n;
        }
        const uint16_t c = v & 0xFFF;
        if (denise_q_color[n] == c) return;
        denise_q_color[n] = c;
    } else if (reg == R_BPLCON2) {
        q_bplcon2 = v;
    } else if (reg == R_BPLCON3) {
        q_bplcon3 = v;
    }
    if (reg >= R_SPR0PTH && reg < R_SPR0PTH + 32) {
        const int s = (reg - R_SPR0PTH) >> 2;
        if (reg & 2) sprpt[s] = (sprpt[s] & 0xFFFF0000) | (v & 0xFFFE);
        else         sprpt[s] = (sprpt[s] & 0x0000FFFF) | ((uint32_t)(v & 0x1F) << 16);
        return;                                               // the output stage has no use for it
    }
    // Every queued write first has the DMA side read the units before it: the
    // event lets the fetch stage go up to h, and the words must be in the ring by
    // then.  The sprite registers skipped this, and RoboCop 2's SPR0CTL write at
    // h 226 had the fetch stage (at once in sync mode, when caught up in async)
    // spread stale ring words over its playfield's last line.
    dma_to(h);
    if (reg >= R_SPR0POS && reg < R_SPR0POS + 64) {
        const int s = (reg - R_SPR0POS) >> 3;
        if ((reg & 6) == 0) { dma_sprpos[s] = v; spr_decode(s); }
        if ((reg & 6) == 2) { dma_sprctl[s] = v; spr_decode(s); }
    } else {
        if (reg >= R_BPL1PTH && reg < R_BPL1PTH + 32) {      // AGA: BPL7PT, BPL8PT
            const int p = (reg - R_BPL1PTH) >> 2;
            if (reg & 2) bplpt[p] = (bplpt[p] & 0xFFFF0000) | (v & 0xFFFE);
            else         bplpt[p] = (bplpt[p] & 0x0000FFFF) | ((uint32_t)(v & 0x1F) << 16);
            return;
        }
        if (reg == R_BPL1MOD) { bplmod[0] = (int16_t)(v & 0xFFFE); return; }
        if (reg == R_BPL2MOD) { bplmod[1] = (int16_t)(v & 0xFFFE); return; }
        a_apply(&dma_st, reg, v);
    }
    push((uint16_t)reg, v, h, x);
}

AMIGA_HOT bool denise_lace(void) { return (dma_st.bplcon0 & 0x0004) != 0; }

// Snapshots, between frames: both stages have replayed everything (denise_sync),
// so what is left is register state.  The fetch stage's registers equal the DMA
// side's then; the per-line state is rebuilt by the next line start, the sprites'
// DMA by the next frame start.
void denise_state(st_t *s) {
    st_u16(s, &bplcon0); st_u16(s, &bplcon2); st_u16(s, &bplcon3);
    st_u16(s, &diwstrt); st_u16(s, &diwstop); st_u16(s, &diwhigh);
    agnus_t *f = &dma_st;
    st_u16(s, &f->bplcon0); st_u16(s, &f->bplcon1);
    st_u16(s, &f->diwstrt); st_u16(s, &f->diwstop); st_u16(s, &f->diwhigh);
    st_u16(s, &f->ddfstrt); st_u16(s, &f->ddfstop);
    for (int i = 0; i < 6; i++) st_u32(s, &bplpt[i]);
    ST_ARRAY(s, st_i16, bplmod);
    for (int i = 0; i < 32; i++) st_u16(s, &color[i]);
    ST_ARRAY(s, st_u32, sprpt);
    ST_ARRAY(s, st_u16, dma_sprpos);
    ST_ARRAY(s, st_u16, dma_sprctl);
    ST_ARRAY(s, st_u16, sprpos);
    ST_ARRAY(s, st_u16, sprctl);
    ST_ARRAY(s, st_u16, sprdata);
    ST_ARRAY(s, st_u16, sprdatb);
    st_int(s, &vc_first); st_int(s, &vc_last);
    st_int(s, &vc_y0); st_int(s, &vc_want); st_int(s, &vc_count);
    for (int i = 32; i < 256; i++) st_u16(s, &color[i]);        // AGA (at the end: older read 0)
    st_u16(s, &bplcon4);
    st_u16(s, &q_bplcon2); st_u16(s, &q_bplcon3);
    st_u32(s, &bplpt[6]); st_u32(s, &bplpt[7]); st_u16(s, &f->fmode);
    if (s->mode != ST_LOAD) return;
    if (!bplcon4) bplcon4 = 0x0011;                              // an older snapshot
    fetch_st.bplcon0 = f->bplcon0; fetch_st.bplcon1 = f->bplcon1;
    fetch_st.diwstrt = f->diwstrt; fetch_st.diwstop = f->diwstop; fetch_st.diwhigh = f->diwhigh;
    fetch_st.ddfstrt = f->ddfstrt; fetch_st.ddfstop = f->ddfstop;
    fetch_st.fmode = f->fmode;
    for (int i = 0; i < 256; i++) { color[i] &= 0xFFF; denise_q_color[i] = color[i]; }
    for (int i = 0; i < 8; i++) spr_decode(i);
    lut_dirty = true;
    spl_dirty = true;
}

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
    vc_first = -1; vc_y0 = AMIGA_FB_Y0; vc_count = 0;
    c_reset();
}
