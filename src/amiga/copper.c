// Copper: MOVE / WAIT / SKIP against the beam, restarted from COP1LC at VBL.
//
// The copper keeps its own position within the line (cop_h, colour clocks)
// and is run in slices interleaved with the CPU (see amiga.c).  Every MOVE is
// handed to custom_write() with the copper's own hpos, so register changes the
// display depends on (colours, BPLCONx, pointers) land on the exact pixel even
// though the CPU and copper are only interleaved at slice granularity.
//
// Timing: 4 colour clocks per instruction (two word fetches on the copper's
// even slots), plus 2 to wake up from a WAIT.  The slots the bitplane DMA takes
// (5 or more planes in lowres, 3 or 4 in hires; denise_copper_slot) are not the
// copper's: an instruction's words wait for the next free ones.
#include <string.h>
#include "amiga_int.h"
#include "m68k.h"

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
// The CPU's own chip RAM writes (the opcode handlers', m68krun.h) and the
// blitter's clears come to the mirror only for the 4 KB pages marked here: a bit
// a page with bytes of a window (the read-ahead too) in it, at every address
// chip RAM appears at.  A window counts from the byte before it: a word written
// there has its low byte in it.  64 bytes (a count a page was 512: the menu's
// heap on PICO_DV is what SRAM leaves), rebuilt whenever a window moves.
AMIGA_HOT static void mir_pages(uint32_t lo, uint32_t size) {
    if (lo == COP_MIR_NONE) return;
    const uint32_t chip = A.chip_mask + 1;
    for (uint32_t p = (lo ? lo - 1 : 0) >> 12; p <= (lo + size - 1) >> 12; p++)
        for (uint32_t a = p << 12; a < M68K_FAST_CHIP_TOP; a += chip) m68k_fast_wmap[a >> 15] |= (uint8_t)(1u << ((a >> 12) & 7));
}
// Move a window (*var: its chip offset, or COP_MIR_NONE).  The read-ahead moves
// a few hundred times a frame: the map is cleared here, not by libc's memset in
// flash (the attribute keeps GCC from making the loop that call).
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("no-tree-loop-distribute-patterns")))
#endif
AMIGA_HOT static void mir_set(uint32_t *var, uint32_t lo, uint32_t size) {
    *var = lo;
    for (unsigned i = 0; i < M68K_FAST_WMAP; i++) m68k_fast_wmap[i] = 0;
    for (int w = 0; w < COP_MIR_N; w++) mir_pages(cop_mir_lo[w], COP_MIR_SIZE);
    mir_pages(cop_ra_lo, COP_RA_SIZE);
    (void)size;
}
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
    mir_set(&cop_mir_lo[victim], cand, COP_MIR_SIZE);
    mir_cand = COP_MIR_NONE; mir_cand_n = 0;
}

// The read-ahead: a list outside the windows (Roots' sprite part alternates two
// lists of 56 KB, 14,000 instructions a frame) was an XIP miss every second
// instruction, each a PSRAM transfer of its own with the CPU's between them:
// ~510 ns a line, 2.8 ms a frame (PC profile, 2026-11-10).  Lines read one after
// another continue one transfer - the board's boot line "PSRAM ns/line" has 141
// in order against 270 taking turns - so a fetch outside everything reads the
// COP_RA_SIZE bytes from its line on in one run and the copper goes on from SRAM.
uint8_t  cop_ra[COP_RA_SIZE] __attribute__((aligned(4)));
uint32_t cop_ra_lo = COP_MIR_NONE;
AMIGA_HOT static void ra_load(uint32_t a) {
    const uint32_t lo = a & ~7u;
    const uint8_t *src = A.cfg.chip_ram + lo;
    for (uint32_t i = 0; i < COP_RA_SIZE; i += 4) { uint32_t w; memcpy(&w, src + i, 4); memcpy(cop_ra + i, &w, 4); }
    mir_set(&cop_ra_lo, lo, COP_RA_SIZE);
}
void cop_ra_drop(void) { mir_set(&cop_ra_lo, COP_MIR_NONE, COP_RA_SIZE); }

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
    uint32_t i = a - cop_ra_lo;
    if (i > COP_RA_SIZE - 4) {
        if ((a & ~7u) + COP_RA_SIZE > A.chip_mask + 1) return ((uint32_t)chip_rw(a) << 16) | chip_rw(a + 2);
        ra_load(a);
        i = a - cop_ra_lo;
    }
    memcpy(&w, cop_ra + i, 4);
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
        // The usual mask ($FE: every bit compared) needs no walk along the line:
        // the first clock at or after the WAIT's position.
        if ((ir2 & 0xFE) == 0xFE) { const int hp = ir1 & 0xFE; if ((h & 0xFE) < hp) h = hp; }
        else for (; h < to && !cmp_h(h); h++) {}
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

// A frame that is not drawn (amiga_set_render_skip) does not run its list: the
// colours, bitplane and sprite registers it programs show in no picture, and the
// next drawn frame runs its list from the top again.  Roots' sprite part runs
// 14,000 instructions a frame, 8.5 ms of the board's core 0 - a skipped frame
// cost what a drawn one did (cpu 16 + cop 8.5 of 20 ms) and the part ran at 34
// fps whatever was skipped.  What a list does besides the display must still
// happen (Roots' sets the copper interrupt in INTREQ every frame): each frame
// that runs records its MOVEs to other registers - where in the list, which
// register, at which beam position - under the list's start (COP_REC_N lists: two that alternate),
// and a skipped frame whose list has a record replays those from the list as it
// is now, at their positions.  A list with more than COP_EV_MAX of them, or
// without a record yet, runs in full; one whose layout changed is recorded anew
// at the next drawn frame (5 frames at most).
#define COP_REC_N  2
#define COP_EV_MAX 8
typedef struct {
    uint32_t key, key2;                                 // the list: COP1LC and COP2LC at the frame's start
    uint8_t  n;
    bool     on, over;
    struct { uint32_t pc; uint16_t vpos; uint16_t ir1; uint8_t h; } ev[COP_EV_MAX];   // ir1: the MOVE's first word
} cop_rec_t;
static cop_rec_t  cop_rec[COP_REC_N], cop_cur;          // by list; the frame running
static const cop_rec_t *cop_play;                      // the skipped frame's record, or NULL: the copper runs
static int        cop_play_i, cop_rec_next;
static bool       cop_ran;
static inline bool display_only(uint32_t reg) {
    return (reg >= 0x0E0 && reg < 0x180) || (reg >= 0x08E && reg <= 0x094) ||
           reg == R_COPJMP1 || reg == R_COPJMP2 || reg == 0x1E4 || reg == 0x1FC || reg == 0x1FE;
}

// The skipped frame's side: the recorded MOVEs whose position is before `until`.
static void cop_replay(int until) {
    const cop_rec_t *r = cop_play;
    for (; cop_play_i < r->n; cop_play_i++) {
        const int v = r->ev[cop_play_i].vpos, h = r->ev[cop_play_i].h;
        if (v > A.vpos || (v == A.vpos && h >= until)) break;
        if (v < A.vpos) continue;                       // its line went by without the copper (DMA off)
        const uint16_t i1 = chip_rw(r->ev[cop_play_i].pc), i2 = chip_rw(r->ev[cop_play_i].pc + 2);
        const uint32_t reg = i1 & 0x1FE;
        if (i1 != r->ev[cop_play_i].ir1) continue;      // the list changed under it: not that MOVE any more
        if ((i1 & 1) || display_only(reg) || reg < 0x40 || (reg < 0x80 && !(A.copcon & 2))) continue;
        custom_write(reg, i2, h, true);
    }
}

static void cop_frame(void) {
    if (cop_ran) {                                      // the frame that ran: its record, over its list's old one
        int w = -1;
        for (int k = 0; k < COP_REC_N; k++)
            if (cop_rec[k].on && cop_rec[k].key == cop_cur.key && cop_rec[k].key2 == cop_cur.key2) w = k;
        if (w < 0) { w = cop_rec_next; cop_rec_next = (cop_rec_next + 1) % COP_REC_N; }
        cop_rec[w] = cop_cur;
        cop_rec[w].on = true;
    }
    cop_ran = false;
    // COP2LC is part of the list's name: Agony's frames all start at one COP1LC,
    // jump (COPJMP2) to one of two second lists in turn, and each of those sets
    // COP2LC to the other for the next frame.  Under COP1LC alone a skipped frame
    // replayed the MOVEs of the list drawn last - COP2LC to the skipped frame's
    // own list again, so the next frame showed the list the game was not ready
    // to show: its background flickered on the board whenever frames were
    // skipped (2026-11-10).  With both, each list has its record.
    cop_cur.key = lc[0]; cop_cur.key2 = lc[1]; cop_cur.n = 0; cop_cur.over = false;
    cop_play = NULL; cop_play_i = 0;
    if (!A.render_skip) return;
    for (int k = 0; k < COP_REC_N; k++)
        if (cop_rec[k].on && cop_rec[k].key == lc[0] && cop_rec[k].key2 == lc[1] && !cop_rec[k].over) cop_play = &cop_rec[k];
    // The record is the list's only while the list is what it was: each recorded
    // MOVE must still be there (its first word), or the list runs and is recorded
    // anew - another part may have built its own list at the same address.
    if (cop_play) {
        const cop_rec_t *r = cop_play;
        for (int i = 0; i < r->n; i++)
            if (chip_rw(r->ev[i].pc) != r->ev[i].ir1) { cop_play = NULL; break; }
    }
}

// A COPJMP strobe in a frame that replays its record (the CPU's: the copper's own
// are not replayed): the copper is somewhere else now and runs from there, the
// record's MOVEs are not what it would do.  Roots sets up its plasma that way -
// COP1LC, COPJMP1 - in the last frame of the part before, whose list sets BLTPRI
// at line 32 every frame; replayed after the strobe, that MOVE left BLTPRI set
// under the plasma's 128 blits a frame, which then overran each other: six
// stripes for a picture (2026-11-10).  What this frame then runs is not its
// list from the top: not a record to keep.
static void replay_off(void) {
    if (!cop_play) return;
    cop_play = NULL;
    cop_cur.over = true;
}

// The line has nothing for the copper (copper_line_start): stopped, its DMA off,
// waiting for a later line, or a skipped frame without a recorded MOVE on this
// line - the scheduler then leaves copper_run and its profile switches out of
// the line's slices (4,400 a frame: ~0.9 ms of a skipped frame on the board).
// A COPJMP strobe or a DMACON write wakes it; `from` is where the slice starts.
bool copper_asleep;

static bool cop_defer;                  // cop_h is an instruction's that waits for its slice: not moved up
AMIGA_HOT void copper_run(int from, int until) {
    if (cop_h < from && !cop_defer) cop_h = from;
    cop_defer = false;
    if (cop_play) {
        if (copper_on()) cop_replay(until);
        if (cop_h < until) cop_h = until;
        return;
    }
    cop_ran = true;
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
        // The instruction's two words, each in a cycle the bitplane DMA leaves the
        // copper (denise_copper_slot): the first from two clocks before cop_h on,
        // a MOVE lands with the second - at cop_h when every cycle is free, as
        // before.  One that lands in a later slice waits for that slice (this
        // one's CPU writes come first), its first word's cycle kept.
        if (denise_cop_steal) {
            const int s1 = denise_copper_slot(cop_h - 2);
            const int s2 = denise_copper_slot(s1 + 2);
            if (s2 >= until) { cop_h = s1 + 2; cop_defer = true; return; }
            cop_h = s2;
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
                if (!display_only(reg)) {
                    if (cop_cur.n < COP_EV_MAX) {
                        cop_cur.ev[cop_cur.n].pc = pc - 4;
                        cop_cur.ev[cop_cur.n].vpos = (uint16_t)A.vpos;
                        cop_cur.ev[cop_cur.n].ir1 = ir1;
                        cop_cur.ev[cop_cur.n].h = (uint8_t)cop_h;
                        cop_cur.n++;
                    } else cop_cur.over = true;
                }
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

// The first colour clock from h on at which the copper may have something to do
// on this line (AMIGA_CC_PER_LINE: nothing more): the scheduler's slices need
// not be cut finer than that (amiga.c run_line).
AMIGA_HOT int copper_next(int h) {
    if (copper_asleep) return AMIGA_CC_PER_LINE;
    if (cop_play) {
        if (cop_play_i < cop_play->n && cop_play->ev[cop_play_i].vpos == A.vpos)
            return cop_play->ev[cop_play_i].h > h ? cop_play->ev[cop_play_i].h : h;
        return AMIGA_CC_PER_LINE;
    }
    if (!copper_on() || state == COP_STOP) return AMIGA_CC_PER_LINE;
    if (state == COP_WAIT) {
        const int w = wait_until(cop_h > h ? cop_h : h, AMIGA_CC_PER_LINE);
        return w < 0 ? AMIGA_CC_PER_LINE : w;
    }
    return h;
}

AMIGA_HOT void copper_line_start(void) {
    cop_h = cop_h >= AMIGA_CC_PER_LINE ? cop_h - AMIGA_CC_PER_LINE : 0;
    cop_defer = false;
    if (cop_play) {
        while (cop_play_i < cop_play->n && cop_play->ev[cop_play_i].vpos < A.vpos) cop_play_i++;
        copper_asleep = !(cop_play_i < cop_play->n && cop_play->ev[cop_play_i].vpos == A.vpos);
    } else {
        copper_asleep = state == COP_STOP || !copper_on() || (state == COP_WAIT && cmp_v() < 0);
    }
}

void copper_vblank(void) {
    cop_frame();
    pc = lc[0];
    state = COP_RUN;
    cop_h = 0;
    mir_vblank();
}

void copper_dma_changed(void) { copper_asleep = false; }

AMIGA_HOT void copper_write(uint32_t reg, uint16_t v) {
    switch (reg) {
    case R_COP1LCH: lc[0] = (lc[0] & 0xFFFF) | ((uint32_t)(v & 0x1F) << 16); break;
    case R_COP1LCL: lc[0] = (lc[0] & 0xFFFF0000) | (v & 0xFFFE); break;
    case R_COP2LCH: lc[1] = (lc[1] & 0xFFFF) | ((uint32_t)(v & 0x1F) << 16); break;
    case R_COP2LCL: lc[1] = (lc[1] & 0xFFFF0000) | (v & 0xFFFE); break;
    case R_COPJMP1: pc = lc[0]; state = COP_RUN; copper_asleep = false; replay_off(); break;
    case R_COPJMP2: pc = lc[1]; state = COP_RUN; copper_asleep = false; replay_off(); break;
    default: break;
    }
    amiga_slice_cut();
}

uint32_t copper_pc(void) { return pc; }
uint32_t copper_list(void) { return lc[0]; }

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
    cop_ra_lo = COP_MIR_NONE;
    memset(m68k_fast_wmap, 0, M68K_FAST_WMAP);
    memset(cop_rec, 0, sizeof cop_rec);
    cop_play = NULL; cop_ran = false;
    mir_cand = COP_MIR_NONE; mir_cand_n = 0;
    memset(mir_cnt, 0, sizeof mir_cnt);
    memset(mir_acc, 0, sizeof mir_acc);
    wcv_line = -1;
}
