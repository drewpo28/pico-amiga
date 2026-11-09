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

// The list mirror (amiga_int.h): the copper's fetches are counted by 4 KB
// block, decaying over ~8 frames (mir_acc), and the COP_MIR_N windows go to
// the COP_MIR_SIZE-long, 4 KB-aligned ranges with the most fetches that do not
// overlap (mir_vblank).  A window is replaced once a range has been in that
// top set for MIR_ADOPT frames in a row (a copy costs the PSRAM reads of its
// whole window).  Roots' zigzag part rewrites a 20 KB list every frame,
// alternating two of them, and the copper's reads of it missed the XIP cache:
// ~2.8 ms of the 9 ms its copper took.  COP1LC itself is no guide: Roots and
// Arte start with a stub that jumps to the real list.
uint8_t  cop_mir[COP_MIR_N][COP_MIR_SIZE];
uint32_t cop_mir_lo[COP_MIR_N] = { COP_MIR_NONE, COP_MIR_NONE };
#define MIR_ADOPT 3
#define MIR_BLK    4096
#define MIR_BLOCKS (0x200000 / MIR_BLK)              // chip RAM is 2 MB at most
#define MIR_W      (COP_MIR_SIZE / MIR_BLK)          // blocks a window
static uint16_t mir_cnt[MIR_BLOCKS];                 // fetches this frame, by block
static uint16_t mir_acc[MIR_BLOCKS];                 // ...decayed over the frames before
static uint32_t mir_cand = COP_MIR_NONE;
static int      mir_cand_n;
#ifdef AMIGA_STATS
uint32_t copper_stat_mir, copper_stat_fetch;        // the bench: fetches from the mirror / all
#endif

// The best COP_MIR_N non-overlapping window starts by mir_acc (sums over MIR_W
// blocks), best first; COP_MIR_NONE where nothing was fetched.
static void mir_best(uint32_t out[COP_MIR_N]) {
    for (int k = 0; k < COP_MIR_N; k++) {
        unsigned best = 0, bs = 0;
        for (unsigned s = 0; s + MIR_W <= MIR_BLOCKS; s++) {
            bool clash = false;
            for (int j = 0; j < k; j++) {
                const unsigned o = out[j] / MIR_BLK;
                if (s + MIR_W > o && s < o + MIR_W) { clash = true; break; }
            }
            if (clash) continue;
            unsigned sum = 0;
            for (unsigned b = 0; b < MIR_W; b++) sum += mir_acc[s + b];
            if (sum > best) { best = sum; bs = s; }
        }
        out[k] = best ? bs * MIR_BLK : COP_MIR_NONE;
    }
}

static void mir_vblank(void) {
    // Decay over ~8 frames: two lists alternating every frame rank alike, so
    // the top set holds a window in each instead of two in this frame's.
    for (unsigned i = 0; i < MIR_BLOCKS; i++) { mir_acc[i] = (uint16_t)(mir_acc[i] - mir_acc[i] / 8 + mir_cnt[i]); mir_cnt[i] = 0; }
    uint32_t top[COP_MIR_N];
    mir_best(top);
    // A top range not mirrored yet: the candidate; MIR_ADOPT frames of it and it
    // takes the window that is not in the top set (an empty one first).
    uint32_t cand = COP_MIR_NONE;
    for (int k = 0; k < COP_MIR_N && cand == COP_MIR_NONE; k++) {
        if (top[k] == COP_MIR_NONE) continue;
        bool have = false;
        for (int w = 0; w < COP_MIR_N; w++) if (cop_mir_lo[w] == top[k]) have = true;
        if (!have) cand = top[k];
    }
    if (cand == COP_MIR_NONE) { mir_cand = COP_MIR_NONE; mir_cand_n = 0; return; }
    if (cand != mir_cand) { mir_cand = cand; mir_cand_n = 1; return; }
    if (++mir_cand_n < MIR_ADOPT) return;
    if (cand + COP_MIR_SIZE > A.chip_mask + 1u) return;       // beyond chip RAM (a mirror address)
    int victim = -1;
    for (int w = 0; w < COP_MIR_N && victim < 0; w++) if (cop_mir_lo[w] == COP_MIR_NONE) victim = w;
    for (int w = 0; w < COP_MIR_N && victim < 0; w++) {
        bool in_top = false;
        for (int k = 0; k < COP_MIR_N; k++) if (cop_mir_lo[w] == top[k]) in_top = true;
        if (!in_top) victim = w;
    }
    if (victim < 0) return;
    memcpy(cop_mir[victim], A.cfg.chip_ram + cand, COP_MIR_SIZE);
    cop_mir_lo[victim] = cand;
    mir_cand = COP_MIR_NONE; mir_cand_n = 0;
}

// Both words of an instruction in one read: the list is in PSRAM, and a copper
// background runs ~13,000 instructions a frame (State of the Art).
AMIGA_HOT static inline uint32_t fetch(uint32_t addr) {
    const uint32_t a = addr & A.chip_mask & ~1u;
    uint32_t w;
    AMIGA_STAT(copper_stat_fetch);
    AMIGA_TRACE_REG(0x3000, 0, 0, a);                // the bench's --trace: the list's addresses
    mir_cnt[a / MIR_BLK]++;
    for (int m = 0; m < COP_MIR_N; m++) {
        const uint32_t i = a - cop_mir_lo[m];
        if (i <= COP_MIR_SIZE - 4) { AMIGA_STAT(copper_stat_mir); memcpy(&w, cop_mir[m] + i, 4); return __builtin_bswap32(w); }
    }
    if (a + 4 > A.chip_mask + 1) return ((uint32_t)chip_rw(a) << 16) | chip_rw(a + 2);
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
// The vertical compare is one answer per line (the beam's line is constant in
// it): kept, so the slices of a line the copper waits through cost a compare.
static int      wcv_line = -1, wcv;
static uint32_t wcv_ir;
AMIGA_HOT static int wait_until(int from, int to) {
    int cv;
    if (wcv_line == A.vpos && wcv_ir == ((uint32_t)ir1 << 16 | ir2)) cv = wcv;
    else { cv = wcv = cmp_v(); wcv_line = A.vpos; wcv_ir = (uint32_t)ir1 << 16 | ir2; }
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
                // AGA: the bank (BPLCON3) picks the entry; denise_write tests it.
                AMIGA_TRACE_REG(reg, ir2, cop_h, 1);
                if (A.aga || denise_q_color[(reg - R_COLOR00) >> 1] != (ir2 & 0xFFF)) denise_write(reg, ir2, cop_h);
            } else if (reg >= R_BPL1PTH && reg < R_BPL1PTH + (A.aga ? 32u : 24u)) {
                // Bitplane pointers straight to the DMA side (Roots: 16 a line).
                AMIGA_TRACE_REG(reg, ir2, cop_h, 1);
                denise_bplpt(reg, ir2, cop_h);
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
    mir_vblank();
}

void copper_dma_changed(void) {}

AMIGA_HOT void copper_write(uint32_t reg, uint16_t v) {
    switch (reg) {
    case R_COP1LCH: lc[0] = (lc[0] & 0xFFFF) | ((uint32_t)(v & 0x1F) << 16); break;
    case R_COP1LCL: lc[0] = (lc[0] & 0xFFFF0000) | (v & 0xFFFE); break;
    case R_COP2LCH: lc[1] = (lc[1] & 0xFFFF) | ((uint32_t)(v & 0x1F) << 16); break;
    case R_COP2LCL: lc[1] = (lc[1] & 0xFFFF0000) | (v & 0xFFFE); break;
    case R_COPJMP1: pc = lc[0]; state = COP_RUN; break;
    case R_COPJMP2: pc = lc[1]; state = COP_RUN; break;
    default: break;
    }
}

uint32_t copper_pc(void) { return pc; }

void copper_state(st_t *s) {
    st_u32(s, &lc[0]); st_u32(s, &lc[1]); st_u32(s, &pc);
    st_int(s, &state); st_int(s, &cop_h);
    st_u16(s, &ir1); st_u16(s, &ir2);
}

void copper_reset(void) {
    lc[0] = lc[1] = pc = 0;
    state = COP_STOP;
    cop_h = 0;
    for (int w = 0; w < COP_MIR_N; w++) cop_mir_lo[w] = COP_MIR_NONE;   // chip RAM changed under it (reset, a snapshot)
    mir_cand = COP_MIR_NONE; mir_cand_n = 0;
    memset(mir_cnt, 0, sizeof mir_cnt);
    memset(mir_acc, 0, sizeof mir_acc);
    wcv_line = -1;
}
