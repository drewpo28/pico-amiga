// Copper: MOVE / WAIT / SKIP against the beam, restarted from COP1LC at VBL.
//
// The copper keeps its own position within the line (cop_h, colour clocks)
// and is run in slices interleaved with the CPU (see amiga.c).  Every MOVE is
// handed to custom_write() with the copper's own hpos, so register changes the
// display depends on (colours, BPLCONx, pointers) land on the exact pixel even
// though the CPU and copper are only interleaved at slice granularity.
//
// Timing: 4 colour clocks per instruction (two word fetches on the copper's
// even slots), plus 2 to wake up from a WAIT.  Bitplane DMA stealing copper
// slots is not modelled yet.
#include <string.h>
#include "amiga_int.h"

enum { COP_RUN, COP_WAIT, COP_STOP };

static uint32_t lc[2];
static uint32_t pc;

// Both words of an instruction in one read: the list is in PSRAM, and a copper
// background runs ~13,000 instructions a frame (State of the Art).
AMIGA_HOT static inline uint32_t fetch(uint32_t addr) {
    const uint32_t a = addr & A.chip_mask & ~1u;
    if (a + 4 > A.chip_mask + 1) return ((uint32_t)chip_rw(a) << 16) | chip_rw(a + 2);
    uint32_t w;
    memcpy(&w, A.cfg.chip_ram + a, 4);
    return __builtin_bswap32(w);
}
static int      state = COP_STOP;
static int      cop_h;
static uint16_t ir1, ir2;

static inline bool copper_on(void) {
    return (A.dmacon & (DMAF_MASTER | DMAF_COPPER)) == (DMAF_MASTER | DMAF_COPPER);
}

// WAIT/SKIP comparator: beam >= (VP,HP) under the enable masks. Only V7..V0
// are compared (V8 is invisible to the copper), and V7 cannot be masked.
static inline int cmp_v(void) {
    int ve = ((ir2 >> 8) & 0x7F) | 0x80;
    int v = A.vpos & 0xFF & ve, vp = (ir1 >> 8) & ve;
    return (v > vp) - (v < vp);
}
static inline bool cmp_h(int h) {
    int he = ir2 & 0xFE;
    return (h & he) >= (ir1 & 0xFE & he);
}

// First colour clock in [from, to) at which the current WAIT is satisfied, or -1.
AMIGA_HOT static int wait_until(int from, int to) {
    int cv = cmp_v();
    if (cv < 0) return -1;
    int h = from;
    if (cv == 0) {
        for (; h < to && !cmp_h(h); h++) {}
        if (h >= to) return -1;
    }
    if (!(ir2 & 0x8000) && blitter_busy()) {          // BFD = 0: also wait for the blitter
        uint64_t done = blitter_done_cyc();
        int hb = done > A.line_cyc ? (int)((done - A.line_cyc) / 2) : 0;
        if (hb >= to) return -1;
        if (hb > h) h = hb;
    }
    return h;
}

AMIGA_HOT void copper_run(int until) {
    if (!copper_on() || state == COP_STOP) {
        if (cop_h < until) cop_h = until;
        return;
    }
    while (cop_h < until) {
        if (state == COP_STOP) { cop_h = until; return; }
        if (state == COP_WAIT) {
            int h = wait_until(cop_h, until);
            if (h < 0) { cop_h = until; return; }
            cop_h = h + 2;
            state = COP_RUN;
            continue;
        }
        const uint32_t w = fetch(pc);
        ir1 = (uint16_t)(w >> 16);
        ir2 = (uint16_t)w;
        pc += 4;
        if (!(ir1 & 1)) {                                  // MOVE
            uint32_t reg = ir1 & 0x1FE;
            if (reg < 0x40 || (reg < 0x80 && !(A.copcon & 2))) { state = COP_STOP; return; }
            // Colours straight to Denise, and not even that when the colour is
            // the one already queued (denise_write would drop it).
            if (reg >= R_COLOR00 && reg < R_COLOR00 + 64) {
                if (denise_q_color[(reg - R_COLOR00) >> 1] != (ir2 & 0xFFF)) denise_write(reg, ir2, cop_h);
            } else {
                custom_write(reg, ir2, cop_h, true);
            }
            cop_h += 4;
        } else if (!(ir2 & 1)) {                           // WAIT
            cop_h += 4;
            state = COP_WAIT;
        } else {                                           // SKIP
            cop_h += 4;
            int cv = cmp_v();
            if (cv > 0 || (cv == 0 && cmp_h(cop_h))) pc += 4;
        }
        if (!copper_on()) { if (cop_h < until) cop_h = until; return; }
    }
}

AMIGA_HOT void copper_line_start(void) {
    cop_h = cop_h >= AMIGA_CC_PER_LINE ? cop_h - AMIGA_CC_PER_LINE : 0;
}

void copper_vblank(void) {
    pc = lc[0];
    state = COP_RUN;
    cop_h = 0;
}

void copper_dma_changed(void) {}

void copper_write(uint32_t reg, uint16_t v) {
    switch (reg) {
    case R_COP1LCH: lc[0] = (lc[0] & 0xFFFF) | ((uint32_t)(v & 7) << 16); break;
    case R_COP1LCL: lc[0] = (lc[0] & 0xFFFF0000) | (v & 0xFFFE); break;
    case R_COP2LCH: lc[1] = (lc[1] & 0xFFFF) | ((uint32_t)(v & 7) << 16); break;
    case R_COP2LCL: lc[1] = (lc[1] & 0xFFFF0000) | (v & 0xFFFE); break;
    case R_COPJMP1: pc = lc[0]; state = COP_RUN; break;
    case R_COPJMP2: pc = lc[1]; state = COP_RUN; break;
    default: break;
    }
}

uint32_t copper_pc(void) { return pc; }

void copper_reset(void) {
    lc[0] = lc[1] = pc = 0;
    state = COP_STOP;
    cop_h = 0;
}
