// OCS/ECS blitter: area mode (shifts, masks, minterms, fill) and line mode.
//
// A blit is executed to completion the moment it is started (BLTSIZE write
// with blitter DMA on), then BBUSY stays set for an estimate of the real
// duration so software waiting on it sees a plausible delay, after which BLIT
// is raised.  Results are bit-exact with the hardware algorithm; what is not
// modelled is the cycle interleaving with the CPU (a program racing the
// blitter through memory sees the final data early).
#include "amiga_int.h"
#include "m68k.h"

enum { BA, BB, BC, BD };

static uint16_t con0, con1, afwm = 0xFFFF, alwm = 0xFFFF;
static uint32_t pt[4];
static int16_t  mod[4];
static uint16_t adat, bdat, cdat;
// B shifter state.  BHOLD (the shifted word) outlives a blit: B DMA off uses
// the BLTBDAT the CPU wrote, shifted on the write against the previous B word -
// which a blit's start clears (WinUAE's fast path starts each blit with
// prevb = 0).  Sanity's Arte draws its logo morph as 1-word columns, ORed in
// with ASH = BSH and BLTBDAT = $FFFF written before each blit: the shifted
// $FFFF is the mask that drops the bits the A shifter carries over from the
// previous row; against the previous blit's $FFFF the mask was $FFFF and the
// logo came out in shards.
static uint16_t bold, bhold;
static int      bw = 64, bh = 1024; // blit width (words) and height (rows)
static bool     pending;          // BLTSIZE written while blitter DMA was off
static bool     busy, zero = true;
static uint64_t done_cyc;
static uint32_t nblits;

// The minterm as two selections (WinUAE's tables, without the 256 functions):
// bit i of LF is the result for (A, B, C) = i >> 2, i >> 1, i, so with each bit
// a 0 / $FFFF mask m[i], C picks within each pair, B within each pair of pairs,
// A between the halves - 14 operations a word for any LF.  Roots' plasma near
// its end runs 128 blits of A | B | C a frame: the eight terms tested one by
// one were a third of the blitter's instructions.  Checked against them for all
// 256 LFs.
typedef struct { uint16_t m0, m2, m4, m6, k10, k32, k54, k76; } mt_t;
static inline mt_t mt_make(uint8_t lf) {
    uint16_t m[8];
    for (int i = 0; i < 8; i++) m[i] = (lf >> i) & 1 ? 0xFFFF : 0;
    const mt_t t = { m[0], m[2], m[4], m[6], (uint16_t)(m[1] ^ m[0]), (uint16_t)(m[3] ^ m[2]),
                     (uint16_t)(m[5] ^ m[4]), (uint16_t)(m[7] ^ m[6]) };
    return t;
}
static inline uint16_t mt_eval(const mt_t *t, uint16_t a, uint16_t b, uint16_t c) {
    const uint16_t l0 = t->m0 ^ (c & t->k10), l1 = t->m2 ^ (c & t->k32);
    const uint16_t h0 = t->m4 ^ (c & t->k54), h1 = t->m6 ^ (c & t->k76);
    const uint16_t gl = l0 ^ (b & (l1 ^ l0)), gh = h0 ^ (b & (h1 ^ h0));
    return (uint16_t)(gl ^ (a & (gh ^ gl)));
}

static inline uint16_t minterm(uint16_t a, uint16_t b, uint16_t c, uint8_t lf) {
    switch (lf) {                       // the common ones (State of the Art: clear, copy)
    case 0x00: return 0;
    case 0xF0: return a;
    case 0xCC: return b;
    case 0xCA: return (uint16_t)((a & b) | (~a & c));           // cookie cut
    case 0xFF: return 0xFFFF;
    default: break;
    }
    uint16_t r = 0;
    if (lf & 0x80) r |=  a &  b &  c;
    if (lf & 0x40) r |=  a &  b & ~c;
    if (lf & 0x20) r |=  a & ~b &  c;
    if (lf & 0x10) r |=  a & ~b & ~c;
    if (lf & 0x08) r |= ~a &  b &  c;
    if (lf & 0x04) r |= ~a &  b & ~c;
    if (lf & 0x02) r |= ~a & ~b &  c;
    if (lf & 0x01) r |= ~a & ~b & ~c;
    return r;
}

// Fill runs from bit 0 upwards (right to left on screen); the carry flips on
// every set bit.  Inclusive keeps both edges, exclusive drops the left one.
// The whole word at once: bit i is inside when the carry in, XOR the bits below
// i, is set - a prefix XOR (checked against the bit loop on every input).
static inline uint16_t fill(uint16_t d, bool *carry, bool excl) {
    uint32_t x = (uint32_t)d << 1;
    x ^= x << 1; x ^= x << 2; x ^= x << 4; x ^= x << 8;
    const uint16_t inside = (uint16_t)(x ^ (*carry ? 0xFFFF : 0));
    *carry ^= ((x >> 15) ^ (d >> 15)) & 1;
    return excl ? (uint16_t)(inside ^ d) : (uint16_t)(inside | d);
}

static inline uint16_t shift_asc(uint16_t nw, uint16_t old, int sh) {
    return (uint16_t)((((uint32_t)old << 16) | nw) >> sh);
}
static inline uint16_t shift_desc(uint16_t nw, uint16_t old, int sh) {
    return (uint16_t)((((uint32_t)nw << 16) | old) >> (16 - sh));
}

// The copper list mirror's side of a word written straight to chip RAM (rare:
// out of the loops).
__attribute__((noinline)) static void mir_w16(uint32_t off, uint16_t v) { cop_mir_w16(off, v); }

// The words as they are - no shift, no mask, no fill, ascending: the minterm
// works on them as chip RAM stores them (big-endian, no swaps), the rows
// through pointers, a channel that is off or not looked at a constant with
// a stride of 0.  Roots' plasma is 128 blits of D = A | B | C a frame, 20
// words x 4 rows each: the loop below took 108 ARM instructions a word, 6 ms
// of the board's frame.  Same order of reads and writes, word by word; a row
// that would wrap at the end of chip RAM leaves the rest to area_blit's loop
// (the rows done are returned).  A function of its own: inlined, the minterm's
// eight masks and the four pointers cost area_blit 1.2 KB of SRAM in spills.
AMIGA_HOT __attribute__((noinline, optimize("Os"))) static int area_plain(const mt_t *mtp, bool needA, bool needC, bool *z) {
    const bool useA = con0 & 0x0800, useB = con0 & 0x0400, useC = con0 & 0x0200;
    const int w = bw, h = bh;
    const uint32_t m0 = mtp->m0, m2 = mtp->m2, m4 = mtp->m4, m6 = mtp->m6;
    const uint32_t k10 = mtp->k10, k32 = mtp->k32, k54 = mtp->k54, k76 = mtp->k76;
    uint8_t *cr = A.cfg.chip_ram;
    const uint32_t cm = A.chip_mask & ~1u, top = A.cfg.chip_size - 2u * (uint32_t)w;
    // k: the constants (BLTxDAT as stored); v: the words last read.
    const uint16_t k[3] = { __builtin_bswap16(adat), __builtin_bswap16(useB ? bdat : bhold), __builtin_bswap16(cdat) };
    uint16_t v[3] = { k[0], k[1], k[2] };
    const int sa = useA && needA ? 2 : 0, sb = useB ? 2 : 0, sc = useC && needC ? 2 : 0;
    uint16_t any = 0;
    int y0 = 0;
    for (; y0 < h; y0++) {
        const uint32_t oa = pt[BA] & cm, ob = pt[BB] & cm, oc = pt[BC] & cm, od = pt[BD] & cm;
        if ((useA && oa > top) || (useB && ob > top) || (useC && oc > top) || od > top) break;
        const uint8_t *pa = sa ? cr + oa : (const uint8_t *)&k[0];
        const uint8_t *pb = sb ? cr + ob : (const uint8_t *)&k[1];
        const uint8_t *pc = sc ? cr + oc : (const uint8_t *)&k[2];
        uint8_t *pd = cr + od;
        // The blit's last word in a pass of its own: BLTxDAT end up holding
        // the last words, also those of a channel the minterm does not look at.
        int n = y0 == h - 1 ? w - 1 : w;
        for (int pass = 0; pass < 2; pass++) {
            for (; n > 0; n--, pa += sa, pb += sb, pc += sc, pd += 2) {
                uint16_t o;
                memcpy(&v[0], pa, 2); memcpy(&v[1], pb, 2); memcpy(&v[2], pc, 2);
                // mt_eval, in the registers' width
                const uint32_t va = v[0], vb = v[1], vc = v[2];
                const uint32_t l0 = m0 ^ (vc & k10), l1 = m2 ^ (vc & k32);
                const uint32_t h0 = m4 ^ (vc & k54), h1 = m6 ^ (vc & k76);
                const uint32_t gl = l0 ^ (vb & (l1 ^ l0)), gh = h0 ^ (vb & (h1 ^ h0));
                const uint16_t d = (uint16_t)(gl ^ (va & (gh ^ gl)));
                any |= d;
                memcpy(&o, pd, 2);
                // A word that is already there is not written: its cache line stays
                // clean and is not written back to PSRAM (Hero Quest copies and clears
                // the same 40,000 words every frame, 89% of them unchanged).  The
                // copper list mirror follows a write, as it does chip_ww (Arte clears
                // and fills its copper lists with blits).
                if (o != d) { memcpy(pd, &d, 2); if (M68K_FAST_WPAGE(m68k_fast_wmap, (uint32_t)(pd - cr))) mir_w16((uint32_t)(pd - cr), d); }
            }
            if (pass || y0 != h - 1) break;
            const uint32_t l = 2u * (uint32_t)(w - 1);
            if (useA) pa = cr + oa + l;
            if (useC) pc = cr + oc + l;
            n = 1;
        }
        if (useA) pt[BA] += 2u * (uint32_t)w + mod[BA];
        if (useB) pt[BB] += 2u * (uint32_t)w + mod[BB];
        if (useC) pt[BC] += 2u * (uint32_t)w + mod[BC];
        pt[BD] += 2u * (uint32_t)w + mod[BD];
    }
    const uint16_t a = v[0], b = v[1], c = v[2];
    if (useA) adat = __builtin_bswap16(a);
    if (useB) { bdat = __builtin_bswap16(b); bold = bhold = bdat; }
    if (useC) cdat = __builtin_bswap16(c);
    if (any) *z = false;
    return y0;
}

// Any shift, masks and direction, no fill, D on: area_blit's loop through
// pointers.  Roots' particle part (with fast RAM) merges its chunky buffer
// into planes with 4 blits of 1 x 1536 words a frame, shifted, half of them
// descending: the general loop took ~180 ARM instructions a word there, most
// of it the row's bookkeeping.  So the rows that cannot wrap at the end of chip
// RAM are counted first and run with the pointers carried from row to row; the
// rest is left to area_blit's loop (the rows done are returned, A's shifter in
// *aoldp).  A channel the minterm does not look at is a constant with a stride
// of 0 (its last word is read afterwards, for BLTxDAT); B off is BLTBDAT's
// shifted word.  C is not read here (area_blit's loop does the blits that read
// it, cookie cuts): the minterm's C terms are constants, four registers
// instead of eight (spills cost more than the minterm).  A column (1 word wide)
// runs as one row of h words.  Two loops (ascending, descending), Os: 1.5 KB of
// SRAM; with the C-reading pair and O2 it was 4 KB, below the menu's heap floor.

// The rows from offset o (stepping s a row, a row's words at o .. o + span
// ascending, o - span .. o descending) that stay within 0 .. lim.
static inline int rows_in(uint32_t o, int32_t s, uint32_t span, uint32_t lim, bool desc, int h) {
    const uint32_t lo = desc ? o - span : o;
    if (lo > lim) return 0;
    if (s == 0) return h;
    const uint32_t n = s > 0 ? (lim - lo) / (uint32_t)s + 1 : lo / (uint32_t)-s + 1;
    return n < (uint32_t)h ? (int)n : h;
}

typedef struct {
    const uint8_t *pa, *pb, *pc; uint8_t *pd;
    int32_t ra, rb, rc, rd;                 // added after a row's words
    int32_t sa, sb, sc, sd;                 // a word's step: 0 for a constant
    uint32_t fw, mw, lw;                    // A's mask: a row's first word, the others; the last's AND
    int sha, shb;
    uint32_t aold, bo, bh, va, vc;
    bool mir;                               // D's words reach a page of the copper list mirror
    bool any;
} shift_run_t;

// One direction (and C read or not: area_shift uses only "not"), inlined into its own loop.
__attribute__((always_inline)) static inline void shift_rows(shift_run_t *r, const mt_t *mtp, int rows, int words, bool useB, const bool desc, const bool creg) {
    const uint8_t *pa = r->pa, *pb = r->pb, *pc = r->pc;
    uint8_t *pd = r->pd;
    const int32_t sa = r->sa, sb = r->sb, sd = r->sd;
    const int sha = r->sha, shb = r->shb;
    uint32_t aold = r->aold, bo = r->bo, bh = r->bh, va = r->va, vc = r->vc;
    uint32_t any = 0;
    // C's terms: with C not read, l0 / x1 (l1 ^ l0) / h0 / x2 (h1 ^ h0) once
    uint32_t c0 = mtp->m0, c1 = mtp->m2, c2 = mtp->m4, c3 = mtp->m6;
    if (!creg) {
        const uint32_t l0 = c0 ^ (vc & mtp->k10), l1 = c1 ^ (vc & mtp->k32);
        const uint32_t h0 = c2 ^ (vc & mtp->k54), h1 = c3 ^ (vc & mtp->k76);
        c0 = l0; c1 = l1 ^ l0; c2 = h0; c3 = h1 ^ h0;
    }
    for (int y = rows; y > 0; y--) {
        uint32_t mask = r->fw;
        for (int n = words; n > 0; n--) {
            uint16_t t;
            if (n == 1) mask &= r->lw;
            memcpy(&t, pa, 2); va = __builtin_bswap16(t); pa += sa;
            const uint32_t am = va & mask;
            const uint32_t ah = desc ? ((am << 16) | aold) >> sha : ((aold << 16) | am) >> sha;
            aold = am;
            if (useB) {
                memcpy(&t, pb, 2); pb += sb;
                const uint32_t vb = __builtin_bswap16(t);
                bh = desc ? ((vb << 16) | bo) >> shb : ((bo << 16) | vb) >> shb;
                bo = vb;
            }
            uint32_t gl, gh;
            if (creg) {
                memcpy(&t, pc, 2); vc = __builtin_bswap16(t); pc += r->sc;
                const uint32_t l0 = c0 ^ (vc & mtp->k10), l1 = c1 ^ (vc & mtp->k32);
                const uint32_t h0 = c2 ^ (vc & mtp->k54), h1 = c3 ^ (vc & mtp->k76);
                gl = l0 ^ (bh & (l1 ^ l0)); gh = h0 ^ (bh & (h1 ^ h0));
            } else {
                gl = c0 ^ (bh & c1); gh = c2 ^ (bh & c3);
            }
            const uint16_t s = __builtin_bswap16((uint16_t)(gl ^ (ah & (gh ^ gl))));
            any |= s;
            memcpy(&t, pd, 2);
            if (t != s) {                   // as area_plain: an unchanged word is not written
                memcpy(pd, &s, 2);
                if (r->mir) {
                    const uint32_t o = (uint32_t)(pd - A.cfg.chip_ram);
                    if (M68K_FAST_WPAGE(m68k_fast_wmap, o)) mir_w16(o, s);
                }
            }
            pd += sd;
            mask = r->mw;
        }
        pa += r->ra; pb += r->rb; pd += r->rd;
        if (creg) pc += r->rc;
    }
    r->aold = aold; r->bo = bo; r->bh = bh; r->va = va; r->vc = vc;
    r->any = any != 0;
}

AMIGA_HOT __attribute__((noinline, optimize("Os"))) static int area_shift(const mt_t *mtp, bool needA, bool needC, bool *z, uint16_t *aoldp) {
    const bool useA = con0 & 0x0800, useB = con0 & 0x0400, useC = con0 & 0x0200, desc = con1 & 0x0002;
    const int w = bw, h = bh;
    uint8_t *cr = A.cfg.chip_ram;
    const uint32_t cm = A.chip_mask & ~1u, span = 2u * (uint32_t)(w - 1);
    const uint32_t lim = A.cfg.chip_size - 2u - span;     // a row's lowest word at or below it
    const bool rA = useA && needA, rC = useC && needC;
    const int inc = desc ? -2 : 2;
    const int32_t row = inc * w;                      // a row's step: its words and the modulo
    const int32_t ma = row + (desc ? -mod[BA] : mod[BA]), mb = row + (desc ? -mod[BB] : mod[BB]);
    const int32_t mc = row + (desc ? -mod[BC] : mod[BC]), md = row + (desc ? -mod[BD] : mod[BD]);
    const uint32_t oa = pt[BA] & cm, ob = pt[BB] & cm, oc = pt[BC] & cm, od = pt[BD] & cm;
    int n = rows_in(od, md, span, lim, desc, h);
    if (rA) { const int k = rows_in(oa, ma, span, lim, desc, h); if (k < n) n = k; }
    if (useB) { const int k = rows_in(ob, mb, span, lim, desc, h); if (k < n) n = k; }
    if (rC) { const int k = rows_in(oc, mc, span, lim, desc, h); if (k < n) n = k; }
    if (n <= 0) return 0;
    static uint16_t ka, kc;                           // the constants as stored (static: not spilled for)
    ka = __builtin_bswap16(adat); kc = __builtin_bswap16(cdat);
    shift_run_t r = {
        .pa = rA ? cr + oa : (const uint8_t *)&ka, .pb = cr + ob, .pc = rC ? cr + oc : (const uint8_t *)&kc,
        .pd = cr + od,
        .ra = rA ? ma - row : 0, .rb = useB ? mb - row : 0, .rc = rC ? mc - row : 0, .rd = md - row,
        .sa = rA ? inc : 0, .sb = useB ? inc : 0, .sc = rC ? inc : 0, .sd = inc,
        .fw = afwm, .mw = 0xFFFF, .lw = alwm,
        .sha = desc ? 16 - (con0 >> 12) : con0 >> 12, .shb = desc ? 16 - (con1 >> 12) : con1 >> 12,
        .aold = *aoldp, .bo = bold, .bh = bhold, .va = adat, .vc = cdat,
    };
    // The pages D's words reach: none with a window, no look at the map a word.
    {
        const uint32_t d0 = desc ? od - span : od, d1 = d0 + (uint32_t)((n - 1) * md);
        const uint32_t lo = d0 < d1 ? d0 : d1, hi = (d0 < d1 ? d1 : d0) + span + 1;
        for (uint32_t pg = lo >> 12; pg <= hi >> 12 && !r.mir; pg++) r.mir = M68K_FAST_WPAGE(m68k_fast_wmap, pg << 12);
    }
    int rows = n, words = w;
    if (w == 1) {                           // a column: one row of n words, every word first and last
        rows = 1; words = n;
        r.sa += r.ra; r.sb += r.rb; r.sc += r.rc; r.sd += r.rd;
        r.ra = r.rb = r.rc = r.rd = 0;
        r.fw = r.mw = (uint32_t)(afwm & alwm); r.lw = 0xFFFF;
    }
    if (desc) shift_rows(&r, mtp, rows, words, useB, true, false);
    else      shift_rows(&r, mtp, rows, words, useB, false, false);
    if (n == h) {                           // the last words of A and C as the DMA leaves them
        const uint32_t l = (uint32_t)(inc * (w - 1));
        if (useA && !rA) r.va = chip_rw(pt[BA] + (uint32_t)((h - 1) * ma) + l);
        if (useC && !rC) r.vc = chip_rw(pt[BC] + (uint32_t)((h - 1) * mc) + l);
    }
    if (useA) { pt[BA] += (uint32_t)(n * ma); if (rA || n == h) adat = (uint16_t)r.va; }
    if (useB) { pt[BB] += (uint32_t)(n * mb); bdat = bold = (uint16_t)r.bo; bhold = (uint16_t)r.bh; }
    if (useC) { pt[BC] += (uint32_t)(n * mc); if (rC || n == h) cdat = (uint16_t)r.vc; }
    pt[BD] += (uint32_t)(n * md);
    *aoldp = (uint16_t)r.aold;
    if (r.any) *z = false;
    return n;
}

// In SRAM: run from flash, the blits competed with chip RAM (PSRAM) for the XIP
// cache (State of the Art's intro clears and fills ~12,000 words a frame).
AMIGA_HOT static uint32_t area_blit(void) {
    const bool useA = con0 & 0x0800, useB = con0 & 0x0400, useC = con0 & 0x0200, useD = con0 & 0x0100;
    const bool desc = con1 & 0x0002;
    const bool dofill = con1 & 0x0018, excl = con1 & 0x0010;
    const int ash = con0 >> 12, bsh = con1 >> 12;
    const uint8_t lf = (uint8_t)con0;
    const int w = bw, h = bh;
    const int inc = desc ? -2 : 2;
    uint16_t aold = 0;
    bool z = true;

    // A and C words the minterm does not look at are not read (Hero Quest
    // clears with A on and minterm 00), except the last: BLTxDAT ends up as
    // after the DMA.  B is always read, its shifter state outlives the blit.
    const bool needA = (lf >> 4) != (lf & 15);
    const bool needC = ((lf >> 1) & 0x55) != (lf & 0x55);
    const mt_t mt = mt_make(lf);
    int y0 = 0;
    // (A's shift and masks are nothing to a minterm that does not look at A:
    // Hero Quest's language screen clears 20x1000 words a frame with A on.)
    if (useD && !dofill && !desc && (!needA || (!ash && afwm == 0xFFFF && alwm == 0xFFFF)) && (!useB || !bsh))
        y0 = area_plain(&mt, needA, needC, &z);
    else if (useD && !dofill && !(useC && needC))
        y0 = area_shift(&mt, needA, needC, &z, &aold);
    for (int y = y0; y < h; y++) {
        bool carry = (con1 & 0x0004) != 0;
        uint16_t mask = afwm;
        const bool last_row = y == h - 1;
        for (int x = 0; x < w; x++) {
            if (x == w - 1) mask &= alwm;
            const bool last = last_row && x == w - 1;
            if (useA) { if (needA || last) adat = chip_rw(pt[BA]); pt[BA] += inc; }
            if (useB) { bdat = chip_rw(pt[BB]); pt[BB] += inc; }
            if (useC) { if (needC || last) cdat = chip_rw(pt[BC]); pt[BC] += inc; }
            uint16_t am = adat & mask;
            uint16_t ahold = desc ? shift_desc(am, aold, ash) : shift_asc(am, aold, ash);
            aold = am;
            if (useB) {
                bhold = desc ? shift_desc(bdat, bold, bsh) : shift_asc(bdat, bold, bsh);
                bold = bdat;
            }
            uint16_t d = mt_eval(&mt, ahold, bhold, cdat);
            if (dofill) d = fill(d, &carry, excl);
            if (d) z = false;
            if (useD) {                     // as above: an unchanged word is not written
                if (chip_rw(pt[BD]) != d) chip_ww(pt[BD], d);
                pt[BD] += inc;
            }
            mask = 0xFFFF;
        }
        if (useA) pt[BA] += desc ? -mod[BA] : mod[BA];
        if (useB) pt[BB] += desc ? -mod[BB] : mod[BB];
        if (useC) pt[BC] += desc ? -mod[BC] : mod[BC];
        if (useD) pt[BD] += desc ? -mod[BD] : mod[BD];
    }
    zero = z;
    int ch = (useA + useB + useC + useD); if (ch < 2) ch = 2;
    return (uint32_t)(w * h * ch);          // colour clocks, roughly
}

// Flash: lines are a few hundred pixels a frame, not worth the SRAM.
__attribute__((noinline)) static uint32_t line_blit(void) {
    const bool useA = con0 & 0x0800, useB = con0 & 0x0400, useC = con0 & 0x0200;
    const bool sing = con1 & 0x0002;
    const bool sud = con1 & 0x0010, sul = con1 & 0x0008, aul = con1 & 0x0004;
    bool sign = con1 & 0x0040;
    int ash = con0 >> 12, bsh = con1 >> 12;
    const uint8_t lf = (uint8_t)con0;
    const int h = bh;
    bool first = true, z = true;

#define INCX() do { if (++ash == 16) { ash = 0; pt[BC] += 2; } } while (0)
#define DECX() do { if (ash-- == 0) { ash = 15; pt[BC] -= 2; } } while (0)
#define INCY() do { pt[BC] += mod[BC]; first = true; } while (0)
#define DECY() do { pt[BC] -= mod[BC]; first = true; } while (0)

    for (int i = 0; i < h; i++) {
        if (useB) { bdat = chip_rw(pt[BB]); pt[BB] += mod[BB]; }
        if (useC) cdat = chip_rw(pt[BC]);
        uint16_t ahold = (uint16_t)((adat & afwm) >> ash);
        bool tex = (bdat >> bsh) & 1;
        if (bsh-- == 0) bsh = 15;
        uint16_t d = minterm(ahold, tex ? 0xFFFF : 0, cdat, lf);
        bool wr = (!sing || first) && useC;

        first = false;
        if (!sign) {
            if (sud) { if (sul) DECY(); else INCY(); }
            else     { if (sul) DECX(); else INCX(); }
        }
        if (sud) { if (aul) DECX(); else INCX(); }
        else     { if (aul) DECY(); else INCY(); }
        if (useA) pt[BA] = (uint32_t)((int32_t)pt[BA] + (sign ? mod[BB] : mod[BA]));
        sign = (int16_t)pt[BA] < 0;

        if (d) z = false;
        if (wr) chip_ww(pt[BD], d);
        pt[BD] = pt[BC];
    }
#undef INCX
#undef DECX
#undef INCY
#undef DECY
    con0 = (uint16_t)((con0 & 0x0FFF) | (ash << 12));
    con1 = (uint16_t)((con1 & 0x0FBF) | (bsh << 12) | (sign ? 0x40 : 0));
    zero = z;
    return (uint32_t)h * 4;
}

// noinline: it is called from blitter_dma_changed (flash), and GCC inlined it
// there once, the blits with it.
AMIGA_HOT __attribute__((noinline)) static void start(void) {       // the blits are inlined here
    pending = false;
    const int prof = prof_switch(AMIGA_PROF_BLIT);
    uint32_t cc = (con1 & 1) ? line_blit() : area_blit();
    prof_switch(prof);
    nblits++;
    busy = true;
    done_cyc = amiga_now() + 2ull * cc;
    amiga_slice_cut();
}

AMIGA_HOT void blitter_poll(void) {
    if (busy && amiga_now() >= done_cyc) {
        busy = false;
        amiga_set_intreq(INTF_BLIT);
    }
}

AMIGA_HOT bool     blitter_busy(void)     { blitter_poll(); return busy || pending; }
AMIGA_HOT bool     blitter_zero(void)     { return zero; }
uint64_t blitter_done_cyc(void) { return busy ? done_cyc : 0; }
uint32_t blitter_count(void)    { return nblits; }

void blitter_dma_changed(void) {
    if (pending && (A.dmacon & (DMAF_MASTER | DMAF_BLITTER)) == (DMAF_MASTER | DMAF_BLITTER))
        start();
}

static void set_ptr(int ch, bool hi, uint16_t v) {
    if (hi) pt[ch] = (pt[ch] & 0x0000FFFF) | ((uint32_t)(v & 0x1F) << 16);
    else    pt[ch] = (pt[ch] & 0xFFFF0000) | (v & 0xFFFE);
}

static void go(void) {
    blitter_poll();
    pending = true;
    blitter_dma_changed();
}

AMIGA_HOT void blitter_write(uint32_t reg, uint16_t v) {
    switch (reg) {
    case R_BLTCON0: con0 = v; break;
    case R_BLTCON1: con1 = v; break;
    case R_BLTAFWM: afwm = v; break;
    case R_BLTALWM: alwm = v; break;
    case R_BLTCPTH: set_ptr(BC, true, v); break;
    case R_BLTCPTL: set_ptr(BC, false, v); break;
    case R_BLTBPTH: set_ptr(BB, true, v); break;
    case R_BLTBPTL: set_ptr(BB, false, v); break;
    case R_BLTAPTH: set_ptr(BA, true, v); break;
    case R_BLTAPTL: pt[BA] = (pt[BA] & 0xFFFF0000) | v; break;   // line mode keeps bit 0 (error term)
    case R_BLTDPTH: set_ptr(BD, true, v); break;
    case R_BLTDPTL: set_ptr(BD, false, v); break;
    case R_BLTCMOD: mod[BC] = (int16_t)(v & 0xFFFE); break;
    case R_BLTBMOD: mod[BB] = (int16_t)(v & 0xFFFE); break;
    case R_BLTAMOD: mod[BA] = (int16_t)(v & 0xFFFE); break;
    case R_BLTDMOD: mod[BD] = (int16_t)(v & 0xFFFE); break;
    case R_BLTCDAT: cdat = v; break;
    case R_BLTBDAT:
        // The CPU write goes through the B barrel shifter like a DMA fetch.
        bdat = v;
        bhold = (con1 & 0x0002) ? shift_desc(v, bold, con1 >> 12) : shift_asc(v, bold, con1 >> 12);
        bold = v;
        break;
    case R_BLTADAT: adat = v; break;
    case R_BLTSIZE:
        bh = v >> 6;    if (!bh) bh = 1024;
        bw = v & 0x3F;  if (!bw) bw = 64;
        bold = 0;                       // see go()
        go();
        break;
    // ECS Agnus: BLTCON0's low byte alone, and blits up to 32768 x 2048 words
    // (BLTSIZV, then BLTSIZH, which starts it).
    case R_BLTCON0L: if (A.ecs) con0 = (uint16_t)((con0 & 0xFF00) | (v & 0xFF)); break;
    case R_BLTSIZV:  if (A.ecs) { bh = v & 0x7FFF; if (!bh) bh = 0x8000; } break;
    case R_BLTSIZH:
        if (!A.ecs) break;
        bw = v & 0x7FF; if (!bw) bw = 0x800;
        bold = 0;
        go();
        break;
    default: break;
    }
}

// Snapshots: a blit is done the moment it starts, so one "in flight" is only its
// BBUSY time (done_cyc, on the CPU's clock, which the snapshot keeps too).
void blitter_state(st_t *s) {
    st_u16(s, &con0); st_u16(s, &con1); st_u16(s, &afwm); st_u16(s, &alwm);
    ST_ARRAY(s, st_u32, pt);
    ST_ARRAY(s, st_i16, mod);
    st_u16(s, &adat); st_u16(s, &bdat); st_u16(s, &cdat);
    st_u16(s, &bold); st_u16(s, &bhold);
    st_int(s, &bw); st_int(s, &bh);
    st_bool(s, &pending); st_bool(s, &busy); st_bool(s, &zero);
    st_u64(s, &done_cyc);
    st_u32(s, &nblits);
}

void blitter_reset(void) {
    con0 = con1 = 0; afwm = alwm = 0xFFFF;
    for (int i = 0; i < 4; i++) { pt[i] = 0; mod[i] = 0; }
    adat = bdat = cdat = 0; bold = bhold = 0; bw = 64; bh = 1024;
    pending = busy = false; zero = true;
}
