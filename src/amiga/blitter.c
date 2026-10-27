// OCS blitter: area mode (shifts, masks, minterms, fill) and line mode.
//
// A blit is executed to completion the moment it is started (BLTSIZE write
// with blitter DMA on), then BBUSY stays set for an estimate of the real
// duration so software waiting on it sees a plausible delay, after which BLIT
// is raised.  Results are bit-exact with the hardware algorithm; what is not
// modelled is the cycle interleaving with the CPU (a program racing the
// blitter through memory sees the final data early).
#include "amiga_int.h"

enum { BA, BB, BC, BD };

static uint16_t con0, con1, afwm = 0xFFFF, alwm = 0xFFFF;
static uint32_t pt[4];
static int16_t  mod[4];
static uint16_t adat, bdat, cdat;
static uint16_t bold, bhold;      // B shifter state; kept between blits (B DMA off uses BLTBDAT)
static uint16_t size;
static bool     pending;          // BLTSIZE written while blitter DMA was off
static bool     busy, zero = true;
static uint64_t done_cyc;
static uint32_t nblits;

static inline uint16_t minterm(uint16_t a, uint16_t b, uint16_t c, uint8_t lf) {
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
static inline uint16_t fill(uint16_t d, bool *carry, bool excl) {
    uint16_t out = 0;
    bool c = *carry;
    for (int i = 0; i < 16; i++) {
        bool in = (d >> i) & 1;
        bool o = excl ? (c ^ in) : (c | in);
        c ^= in;
        out |= (uint16_t)o << i;
    }
    *carry = c;
    return out;
}

static inline uint16_t shift_asc(uint16_t nw, uint16_t old, int sh) {
    return (uint16_t)((((uint32_t)old << 16) | nw) >> sh);
}
static inline uint16_t shift_desc(uint16_t nw, uint16_t old, int sh) {
    return (uint16_t)((((uint32_t)nw << 16) | old) >> (16 - sh));
}

static uint32_t area_blit(void) {
    const bool useA = con0 & 0x0800, useB = con0 & 0x0400, useC = con0 & 0x0200, useD = con0 & 0x0100;
    const bool desc = con1 & 0x0002;
    const bool dofill = con1 & 0x0018, excl = con1 & 0x0010;
    const int ash = con0 >> 12, bsh = con1 >> 12;
    const uint8_t lf = (uint8_t)con0;
    int w = size & 0x3F;  if (!w) w = 64;
    int h = size >> 6;    if (!h) h = 1024;
    const int inc = desc ? -2 : 2;
    uint16_t aold = 0;
    bool z = true;

    for (int y = 0; y < h; y++) {
        bool carry = (con1 & 0x0004) != 0;
        uint16_t mask = afwm;
        for (int x = 0; x < w; x++) {
            if (x == w - 1) mask &= alwm;
            if (useA) { adat = chip_rw(pt[BA]); pt[BA] += inc; }
            if (useB) { bdat = chip_rw(pt[BB]); pt[BB] += inc; }
            if (useC) { cdat = chip_rw(pt[BC]); pt[BC] += inc; }
            uint16_t am = adat & mask;
            uint16_t ahold = desc ? shift_desc(am, aold, ash) : shift_asc(am, aold, ash);
            aold = am;
            if (useB) {
                bhold = desc ? shift_desc(bdat, bold, bsh) : shift_asc(bdat, bold, bsh);
                bold = bdat;
            }
            uint16_t d = minterm(ahold, bhold, cdat, lf);
            if (dofill) d = fill(d, &carry, excl);
            if (d) z = false;
            if (useD) { chip_ww(pt[BD], d); pt[BD] += inc; }
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

static uint32_t line_blit(void) {
    const bool useA = con0 & 0x0800, useB = con0 & 0x0400, useC = con0 & 0x0200;
    const bool sing = con1 & 0x0002;
    const bool sud = con1 & 0x0010, sul = con1 & 0x0008, aul = con1 & 0x0004;
    bool sign = con1 & 0x0040;
    int ash = con0 >> 12, bsh = con1 >> 12;
    const uint8_t lf = (uint8_t)con0;
    int h = size >> 6; if (!h) h = 1024;
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

static void start(void) {
    pending = false;
    const int prof = prof_switch(AMIGA_PROF_BLIT);
    uint32_t cc = (con1 & 1) ? line_blit() : area_blit();
    prof_switch(prof);
    nblits++;
    busy = true;
    done_cyc = amiga_now() + 2ull * cc;
}

void blitter_poll(void) {
    if (busy && amiga_now() >= done_cyc) {
        busy = false;
        amiga_set_intreq(INTF_BLIT);
    }
}

bool     blitter_busy(void)     { blitter_poll(); return busy || pending; }
bool     blitter_zero(void)     { return zero; }
uint64_t blitter_done_cyc(void) { return busy ? done_cyc : 0; }
uint32_t blitter_count(void)    { return nblits; }

void blitter_dma_changed(void) {
    if (pending && (A.dmacon & (DMAF_MASTER | DMAF_BLITTER)) == (DMAF_MASTER | DMAF_BLITTER))
        start();
}

static void set_ptr(int ch, bool hi, uint16_t v) {
    if (hi) pt[ch] = (pt[ch] & 0x0000FFFF) | ((uint32_t)(v & 0x7) << 16);
    else    pt[ch] = (pt[ch] & 0xFFFF0000) | (v & 0xFFFE);
}

void blitter_write(uint32_t reg, uint16_t v) {
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
        size = v;
        blitter_poll();
        pending = true;
        blitter_dma_changed();
        break;
    default: break;
    }
}

void blitter_reset(void) {
    con0 = con1 = 0; afwm = alwm = 0xFFFF;
    for (int i = 0; i < 4; i++) { pt[i] = 0; mod[i] = 0; }
    adat = bdat = cdat = 0; bold = bhold = 0; size = 0;
    pending = busy = false; zero = true;
}
