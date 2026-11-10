// Internal interfaces between the chip modules. Not for the platform side.
#pragma once
#include <string.h>
#include "amiga.h"
#include "m68k.h"

#ifndef AMIGA_HOT
// Placement hook for the hottest functions (the firmware maps it to SRAM).
#define AMIGA_HOT
#endif

#define U16BE(p)  ((uint16_t)(((p)[0] << 8) | (p)[1]))

// ---- custom register byte offsets ($DFF000 + n) ---------------------------
enum {
    R_BLTDDAT = 0x000, R_DMACONR = 0x002, R_VPOSR = 0x004, R_VHPOSR = 0x006,
    R_DSKDATR = 0x008, R_JOY0DAT = 0x00A, R_JOY1DAT = 0x00C, R_CLXDAT = 0x00E,
    R_ADKCONR = 0x010, R_POT0DAT = 0x012, R_POT1DAT = 0x014, R_POTGOR = 0x016,
    R_SERDATR = 0x018, R_DSKBYTR = 0x01A, R_INTENAR = 0x01C, R_INTREQR = 0x01E,
    R_DSKPTH = 0x020, R_DSKPTL = 0x022, R_DSKLEN = 0x024, R_DSKDAT = 0x026,
    R_REFPTR = 0x028, R_VPOSW = 0x02A, R_VHPOSW = 0x02C, R_COPCON = 0x02E,
    R_SERDAT = 0x030, R_SERPER = 0x032, R_POTGO = 0x034, R_JOYTEST = 0x036,
    R_STREQU = 0x038, R_STRVBL = 0x03A, R_STRHOR = 0x03C, R_STRLONG = 0x03E,
    R_BLTCON0 = 0x040, R_BLTCON1 = 0x042, R_BLTAFWM = 0x044, R_BLTALWM = 0x046,
    R_BLTCPTH = 0x048, R_BLTCPTL = 0x04A, R_BLTBPTH = 0x04C, R_BLTBPTL = 0x04E,
    R_BLTAPTH = 0x050, R_BLTAPTL = 0x052, R_BLTDPTH = 0x054, R_BLTDPTL = 0x056,
    R_BLTSIZE = 0x058, R_BLTCON0L = 0x05A, R_BLTSIZV = 0x05C, R_BLTSIZH = 0x05E,
    R_BLTCMOD = 0x060, R_BLTBMOD = 0x062, R_BLTAMOD = 0x064, R_BLTDMOD = 0x066,
    R_BLTCDAT = 0x070, R_BLTBDAT = 0x072, R_BLTADAT = 0x074,
    R_DENISEID = 0x07C, R_DSKSYNC = 0x07E,
    R_COP1LCH = 0x080, R_COP1LCL = 0x082, R_COP2LCH = 0x084, R_COP2LCL = 0x086,
    R_COPJMP1 = 0x088, R_COPJMP2 = 0x08A, R_COPINS = 0x08C,
    R_DIWSTRT = 0x08E, R_DIWSTOP = 0x090, R_DDFSTRT = 0x092, R_DDFSTOP = 0x094,
    R_DMACON = 0x096, R_CLXCON = 0x098, R_INTENA = 0x09A, R_INTREQ = 0x09C,
    R_ADKCON = 0x09E,
    R_AUD0LCH = 0x0A0,          // + 0x10 per channel: LCH LCL LEN PER VOL DAT
    R_BPL1PTH = 0x0E0,          // .. BPL6PTL = 0x0F6
    R_BPLCON0 = 0x100, R_BPLCON1 = 0x102, R_BPLCON2 = 0x104, R_BPLCON3 = 0x106,
    R_BPL1MOD = 0x108, R_BPL2MOD = 0x10A, R_BPLCON4 = 0x10C, R_FMODE = 0x1FC,
    R_BPL1DAT = 0x110,          // .. BPL6DAT = 0x11A
    R_SPR0PTH = 0x120,          // .. SPR7PTL = 0x13E
    R_SPR0POS = 0x140,          // + 8 per sprite: POS CTL DATA DATB
    R_COLOR00 = 0x180,          // .. COLOR31 = 0x1BE
    R_BEAMCON0 = 0x1DC, R_DIWHIGH = 0x1E4,          // ECS
};

// DMACON
enum {
    DMAF_SETCLR = 0x8000, DMAF_BBUSY = 0x4000, DMAF_BZERO = 0x2000,
    DMAF_BLTPRI = 0x0400, DMAF_MASTER = 0x0200, DMAF_RASTER = 0x0100,
    DMAF_COPPER = 0x0080, DMAF_BLITTER = 0x0040, DMAF_SPRITE = 0x0020,
    DMAF_DISK = 0x0010, DMAF_AUD0 = 0x0001,
};

// INTENA / INTREQ
enum {
    INTF_TBE = 0x0001, INTF_DSKBLK = 0x0002, INTF_SOFTINT = 0x0004,
    INTF_PORTS = 0x0008, INTF_COPER = 0x0010, INTF_VERTB = 0x0020,
    INTF_BLIT = 0x0040, INTF_AUD0 = 0x0080, INTF_RBF = 0x0800,
    INTF_DSKSYNC = 0x1000, INTF_EXTER = 0x2000, INTF_INTEN = 0x4000,
};

// ---- machine-wide state ----------------------------------------------------
typedef struct {
    amiga_config_t cfg;
    uint32_t chip_mask;        // chip_size - 1
    bool     ecs;              // cfg.chipset == AMIGA_ECS or AMIGA_AGA (AGA has ECS's features)
    bool     aga;              // cfg.chipset == AMIGA_AGA
    uint8_t  cpu_mul;          // CPU cycles per 7 MHz cycle: the clock (2 for the 68EC020's
                               // 14 MHz) times the A1200 load test's (amiga_set_load_test)
    uint8_t  cpu_clock;        // amiga_set_cpu: 1 (7 MHz) or 2 (14 MHz); 0 = 1
    uint8_t  load_mul;         // amiga_set_load_test: 1, 2 or 3; 0 = 1
    bool     load_planes;      // ...and two more bitplanes read and spread (8 for 6)
    uint8_t  cpu_rem;          // CPU cycles left over from the last slice (cpu_mul > 1)
    uint64_t idle_cycles;      // CPU cycles idle.c did not run (busy-wait loops)
    // The chip bus (amiga_set_chip_bus; amiga.c bus_wait): the 68EC020 waits for
    // its chip accesses, bus_data counts the CPU's data accesses to chip banks
    // (mem.c), bus_stall the colour clocks it waited, bus_rem8 the eighths over.
    bool     chip_bus;
    bool     render_skip;      // amiga_set_render_skip: the next frame's picture is not drawn
    uint32_t bus_data;
    uint64_t bus_stall;
    uint8_t  bus_rem8;

    // Beam / time. One colour clock = 2 CPU cycles.
    uint32_t frame;
    uint16_t vpos;             // 0 .. lines-1
    uint16_t lines;            // 313 long / 312 short
    bool     lof;              // long frame flag (VPOSR bit 15)
    uint64_t line_cyc;         // absolute CPU cycle at the start of this line
    uint64_t cpu_cyc;          // absolute CPU cycle reached by the CPU
    bool     in_cpu;           // inside m68k_execute()

    uint16_t dmacon, intena, intreq, adkcon;
    uint16_t copcon;
    uint16_t potgo, serper;
    uint16_t beamcon0;         // ECS: stored (PAL only, its sync options are not modelled)
    uint8_t  ipl_pending;      // IPL computed while the CPU was running

    // Input
    uint16_t joy0dat, joy1dat;
    bool     mouse_lmb, mouse_rmb, joy_fire[2];
    uint8_t  joy_dirs[2];
    // The CD32 pad in port 2 (cfg.akiko): its buttons (AMIGA_PAD_*), its shift
    // register's position (8 = the first button, blue) and the clock line as last seen.
    uint8_t  pad_buttons, pad_shift, pad_clk;
} amiga_t;

extern amiga_t A;

// Instructions run (m68k_execute's loop: the chip bus model's fetch estimate,
// the bench's count), and the window the CPU fetches from is a chip bank (mem.c).
extern unsigned int amiga_bus_instr;
extern uint8_t  mem_fetch_chip;
// AMIGA_STATS (the host bench): data accesses and instruction words fetched,
// counted where they happen (mem.c, m68kcpu.h).
extern unsigned int amiga_stat_data, amiga_stat_fetch, amiga_stat_audw;   // audw: writes to AUDx registers
extern unsigned int amiga_stat_hist[2048];                                // runs per handler index
#ifdef AMIGA_STATS
#define AMIGA_STAT(c) ((c)++)
// The bench's register trace: every custom write (custom.c) and sprite DMA
// fetch (denise.c, reg 0x1000 + sprite, v = state, x = pointer) when set.
extern void (*amiga_trace_reg)(uint32_t reg, uint16_t v, int hpos, uint32_t x);
#define AMIGA_TRACE_REG(r, v, h, x) do { if (amiga_trace_reg) amiga_trace_reg((r), (v), (h), (x)); } while (0)
#else
#define AMIGA_STAT(c) ((void)0)
#define AMIGA_TRACE_REG(r, v, h, x) ((void)0)
#endif

// ---- profiling (amiga_get_profile) ------------------------------------------
// Charge the time since the last switch to the current bucket and make `cat` the
// current one; returns the previous, for nested sections to switch back to.
// Free when cfg.prof_clock is NULL.  A platform whose prof_clock is a memory-
// mapped counter may name its address (AMIGA_PROF_CYCCNT; the firmware: the
// DWT cycle counter, which prof_clock enables at the frame's start): one load
// instead of a call.  A copper-heavy frame switches ~25,000 times (core 0 only).
extern uint8_t  prof_cur;
extern uint32_t prof_t, prof_acc[AMIGA_PROF_COUNT];
static inline int prof_switch(int cat) {
    if (!A.cfg.prof_clock) return cat;
#ifdef AMIGA_PROF_CYCCNT
    const uint32_t now = *(volatile uint32_t *)(AMIGA_PROF_CYCCNT);
#else
    const uint32_t now = A.cfg.prof_clock();
#endif
    prof_acc[prof_cur] += now - prof_t;
    prof_t = now;
    const int old = prof_cur;
    prof_cur = (uint8_t)cat;
    return old;
}

// ---- snapshots (state.c) -------------------------------------------------------
// Each module moves its state through one function, both ways: ST_SAVE writes
// the fields as they are, ST_LOAD reads them back into the same variables (and
// then fixes up what derives from them), ST_SIZE counts the bytes (a block's
// size goes out before its fields).  Little-endian on the card whatever the host.
enum { ST_SIZE, ST_SAVE, ST_LOAD };
typedef struct {
    int      mode;
    uint32_t n;                 // ST_SIZE: bytes so far; ST_LOAD: bytes left in the block
    bool     ok;                // ST_LOAD: past the block's end reads 0 and clears it
    const amiga_stream_t *io;
} st_t;
void st_raw(st_t *s, void *p, uint32_t n);
void st_u8(st_t *s, uint8_t *v);
void st_i8(st_t *s, int8_t *v);
void st_bool(st_t *s, bool *v);
void st_u16(st_t *s, uint16_t *v);
void st_i16(st_t *s, int16_t *v);
void st_u32(st_t *s, uint32_t *v);
void st_int(st_t *s, int *v);
void st_uint(st_t *s, unsigned *v);
void st_u64(st_t *s, uint64_t *v);
#define ST_ARRAY(s, fn, a) do { for (unsigned st_i_ = 0; st_i_ < sizeof(a) / sizeof((a)[0]); st_i_++) fn(s, &(a)[st_i_]); } while (0)

void     amiga_sys_state(st_t *s);      // amiga.c: beam, interrupts, keyboard
void     mem_state(st_t *s);
void     custom_state(st_t *s);
void     cia_state(st_t *s);
void     rtc_state(st_t *s);
void     copper_state(st_t *s);
void     blitter_state(st_t *s);
void     denise_state(st_t *s);
void     paula_state(st_t *s);
void     floppy_state(st_t *s);
void     gayle_state(st_t *s);
void     akiko_state(st_t *s);

// Current horizontal position in colour clocks (0..226), from the CPU's clock.
uint16_t amiga_hpos(void);
uint64_t amiga_now(void);              // absolute CPU cycles "now"
void     amiga_update_ipl(void);       // recompute the 68000 interrupt level
void     amiga_set_intreq(uint16_t bits);

// ---- memory (mem.c) --------------------------------------------------------
void     mem_init(void);
void     mem_set_overlay(bool on);
void     mem_autoconfig_reset(void);  // the Zorro II board unconfigured (reset, RESET instruction)
bool     mem_peek(uint32_t a, int size, uint32_t *v);   // a read with no side effect (idle.c)

// ---- busy-wait loops (idle.c) ------------------------------------------------
bool     idle_skip(uint64_t target);        // at a slice's start: true = the slice is done
void     idle_reset(void);
bool     mem_overlay(void);
// DMA-side chip RAM access (word address, already masked by the caller).
// One halfword access each (the address is even): two byte accesses cost
// twice on PSRAM.  Chip RAM is big-endian, the hosts are little-endian.
static inline uint16_t chip_rw(uint32_t addr) {
    uint16_t v;
    memcpy(&v, A.cfg.chip_ram + (addr & A.chip_mask & ~1u), 2);
    return __builtin_bswap16(v);
}
// The copper list mirror (copper.c): COP_MIR_N windows of COP_MIR_SIZE bytes
// of chip RAM (from cop_mir_lo[w]) kept in SRAM, read by the copper instead of
// PSRAM.  Every chip RAM writer keeps them current: the CPU (m68krun.h, mem.c),
// chip_ww (the blitter, the disk), the blitter's clear / copy path (blitter.c,
// which writes chip RAM itself); a bulk change (reset, a snapshot) drops them
// (copper_reset).  Two windows: a demo alternating two lists (Roots' zigzag
// part) has both mirrored.
#define COP_MIR_N    2
#define COP_MIR_SIZE 12288
#define COP_MIR_NONE 0x80000000u            // no window: off - lo is huge for any chip offset
extern uint8_t  cop_mir[COP_MIR_N][COP_MIR_SIZE];
extern uint32_t cop_mir_lo[COP_MIR_N];      // chip offset of each window, or COP_MIR_NONE
// The read-ahead (copper.c fetch): COP_RA_SIZE bytes from cop_ra_lo, read in one
// run when the copper leaves the windows; kept current by the same writers.
#define COP_RA_SIZE  256
extern uint8_t  cop_ra[COP_RA_SIZE];
extern uint32_t cop_ra_lo;
void cop_ra_drop(void);                     // forget it (a DMA write went through it)
static inline void cop_mir_w8(uint32_t off, uint8_t v) {
    for (int w = 0; w < COP_MIR_N; w++) {
        const uint32_t i = off - cop_mir_lo[w];
        if (i < COP_MIR_SIZE) cop_mir[w][i] = v;
    }
    const uint32_t i = off - cop_ra_lo;
    if (i < COP_RA_SIZE) cop_ra[i] = v;
}
static inline void cop_mir_w16(uint32_t off, uint16_t v) {   // v big-endian already; off may be odd
    for (int w = 0; w < COP_MIR_N; w++) {
        const uint32_t i = off - cop_mir_lo[w];
        if (i < COP_MIR_SIZE - 1) memcpy(cop_mir[w] + i, &v, 2);
        else if (i == COP_MIR_SIZE - 1) cop_mir[w][i] = (uint8_t)v;   // its low byte is outside
    }
    const uint32_t i = off - cop_ra_lo;
    if (i < COP_RA_SIZE - 1) memcpy(cop_ra + i, &v, 2);
    else if (i == COP_RA_SIZE - 1) cop_ra[i] = (uint8_t)v;
}
static inline void chip_ww(uint32_t addr, uint16_t v) {
    const uint32_t off = addr & A.chip_mask & ~1u;
    v = __builtin_bswap16(v);
    memcpy(A.cfg.chip_ram + off, &v, 2);
    if (M68K_FAST_WPAGE(m68k_fast_wmap, off)) cop_mir_w16(off, v);   // a page with a window (m68k.h)
}

// ---- custom chip register dispatch (custom.c) ------------------------------
uint16_t custom_read(uint32_t reg);                 // CPU read, reg = byte offset & 0x1FE
void     custom_write(uint32_t reg, uint16_t v, int hpos, bool from_copper);
void     custom_reset(void);

// ---- copper (copper.c) -----------------------------------------------------
void     copper_reset(void);
uint32_t copper_list(void);                         // COP1LC
extern bool amiga_aud_written;                      // amiga.c: an AUDx write this frame
void     copper_vblank(void);                       // restart from COP1LC at line 0
void     copper_line_start(void);
void     copper_run(int from_h, int until_h);       // execute the slice up to colour clock `until_h`
extern bool copper_asleep;                          // nothing for it on this line: copper_run is not called
void     copper_write(uint32_t reg, uint16_t v);    // COPxLC / COPJMPx / COPCON
uint32_t copper_pc(void);
void     copper_dma_changed(void);

// ---- blitter (blitter.c) ---------------------------------------------------
void     blitter_reset(void);
void     blitter_write(uint32_t reg, uint16_t v);
bool     blitter_busy(void);
bool     blitter_zero(void);
uint64_t blitter_done_cyc(void);                    // when the current blit ends
void     blitter_poll(void);                        // finish a blit whose time is up
void     blitter_dma_changed(void);
uint32_t blitter_count(void);

// ---- denise / bitplane + sprite DMA (denise.c) -----------------------------
void     denise_reset(void);
void     denise_write(uint32_t reg, uint16_t v, int hpos);
void     denise_bplpt(uint32_t reg, uint16_t v, int hpos);   // BPLxPTH/L: denise_write's short path (the copper)
extern uint16_t denise_q_color[256];                 // the colours as queued (denise_write)
void     denise_line_start(void);
void     denise_line_end(void);
void     denise_sprite_dma(void);                     // at SPRITE_DMA_CC of the line (run_line)
void     denise_frame_start(void);
bool     denise_lace(void);                         // BPLCON0 LACE
void     denise_status(amiga_status_t *st);          // bplcon0, fmode, dma_lines
int      denise_bus_busy8(int h0, int h1);           // bitplane DMA's chip cycles in [h0, h1), in eighths
int      denise_copper_slot(int h);                  // the copper's first free cycle from h on (bitplane DMA takes some)
// ...and whether the bitplane DMA can take any on this line now (5+ lowres or 3+
// hires planes, FMODE 0, the line fetching): the copper asks only then.  Roots'
// sprite part runs 14,000 copper instructions a frame, and asking each time was
// ~1M ARM instructions a frame on the emulation's core: below 50 fps again.
extern uint8_t denise_cop_steal;
void     denise_sync(void);                         // wait for the render core (async)
uint32_t denise_core1_ticks(void);

// ---- paula: audio (paula.c) ------------------------------------------------
void     paula_reset(void);
void     paula_write(uint32_t reg, uint16_t v);
void     paula_dma_changed(void);
void     paula_half_line(void);                     // advance + emit one stereo sample
uint32_t paula_audio_pos(void);

// ---- floppy + disk DMA (floppy.c) ------------------------------------------
void     floppy_reset(void);
void     floppy_write(uint32_t reg, uint16_t v);
uint16_t floppy_read(uint32_t reg);
void     floppy_cia_prb(uint8_t prb);               // CIA-B port B: step/dir/side/select/motor
uint8_t  floppy_cia_pra(void);                      // CIA-A port A bits 5..2 (active low)
void     floppy_line(void);                         // per-line: DMA completion, index pulse
void     floppy_insert(int drive, const amiga_disk_t *d);
void     floppy_eject(int drive);
void     floppy_set_drives(int n);
void     floppy_status(amiga_floppy_t *st);

// ---- battery clock (rtc.c) ---------------------------------------------------
uint8_t  rtc_read(int reg);                         // reg 0..15: the nibble
void     rtc_write(int reg, uint8_t v);
void     rtc_reset(void);

// ---- Gayle: IDE, PCMCIA, ID (gayle.c; cfg.gayle != 0) -----------------------
void     gayle_reset(void);
uint8_t  gayle_read(uint32_t a);                    // $D80000-$DDFFFF
void     gayle_write(uint32_t a, uint8_t v);
uint16_t gayle_read16(uint32_t a);                  // the IDE data register a word at a time
void     gayle_write16(uint32_t a, uint16_t v);
uint8_t  gayle_id_read(uint32_t a);                 // $DD0000-$DEFFFF
void     gayle_id_write(void);
void     gayle_line(void);                          // per line: the IDE interrupt

// ---- Akiko: the CD32's CD drive, NVRAM and C2P (akiko.c; cfg.akiko) ----------
void     akiko_reset(void);
uint8_t  akiko_read(uint32_t a);                    // $B80000-$B8FFFF
uint16_t akiko_read16(uint32_t a);
uint32_t akiko_read32(uint32_t a);                  // a long is one access (the C2P counts them)
void     akiko_write(uint32_t a, uint8_t v);
void     akiko_write16(uint32_t a, uint16_t v);
void     akiko_write32(uint32_t a, uint32_t v);
void     akiko_line(void);                          // per line: the drive, its interrupt
void     akiko_audio(int32_t *l, int32_t *r);       // per output sample (paula.c): CD audio added in

// ---- the IDE drive behind Gayle (ide.c) ------------------------------------
void     ide_insert(int unit, const amiga_disk_t *d);   // 0 master, 1 slave; NULL: no drive
void     ide_reset(void);
bool     ide_present(void);
uint8_t  ide_read(int reg);                         // 0-7 task file, 0x406/0x407 control block
void     ide_write(int reg, uint8_t v);
uint16_t ide_read_data(void);
void     ide_write_data(uint16_t v);
bool     ide_line(void);                            // per line; true while INTRQ is up
bool     ide_irq(void);
void     ide_state(st_t *s);

// ---- CIAs (cia.c) ----------------------------------------------------------
void     cia_reset(void);
uint8_t  cia_read(int which, int reg);              // which: 0 = CIA-A, 1 = CIA-B
void     cia_write(int which, int reg, uint8_t v);
void     cia_line(void);                            // HSYNC: CIA-B TOD + timers catch-up
void     cia_vsync(void);                           // VSYNC: CIA-A TOD
void     cia_flag(int which);                       // /FLAG input pulse (CIA-B: disk index)
void     cia_keyboard_byte(uint8_t sdr);            // CIA-A serial port byte from the keyboard
bool     cia_keyboard_handshake(void);              // true once the CPU acknowledged
