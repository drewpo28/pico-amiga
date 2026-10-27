// Internal interfaces between the chip modules. Not for the platform side.
#pragma once
#include "amiga.h"

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
    R_BLTSIZE = 0x058,
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
    R_BPL1MOD = 0x108, R_BPL2MOD = 0x10A,
    R_BPL1DAT = 0x110,          // .. BPL6DAT = 0x11A
    R_SPR0PTH = 0x120,          // .. SPR7PTL = 0x13E
    R_SPR0POS = 0x140,          // + 8 per sprite: POS CTL DATA DATB
    R_COLOR00 = 0x180,          // .. COLOR31 = 0x1BE
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
    uint8_t  ipl_pending;      // IPL computed while the CPU was running

    // Input
    uint16_t joy0dat, joy1dat;
    bool     mouse_lmb, mouse_rmb, joy_fire[2];
    uint8_t  joy_dirs[2];
} amiga_t;

extern amiga_t A;

// ---- profiling (amiga_get_profile) ------------------------------------------
// Charge the time since the last switch to the current bucket and make `cat` the
// current one; returns the previous, for nested sections to switch back to.
// Free when cfg.prof_clock is NULL.
extern uint8_t  prof_cur;
extern uint32_t prof_t, prof_acc[AMIGA_PROF_COUNT];
static inline int prof_switch(int cat) {
    if (!A.cfg.prof_clock) return cat;
    const uint32_t now = A.cfg.prof_clock();
    prof_acc[prof_cur] += now - prof_t;
    prof_t = now;
    const int old = prof_cur;
    prof_cur = (uint8_t)cat;
    return old;
}

// Current horizontal position in colour clocks (0..226), from the CPU's clock.
uint16_t amiga_hpos(void);
uint64_t amiga_now(void);              // absolute CPU cycles "now"
void     amiga_update_ipl(void);       // recompute the 68000 interrupt level
void     amiga_set_intreq(uint16_t bits);

// ---- memory (mem.c) --------------------------------------------------------
void     mem_init(void);
void     mem_set_overlay(bool on);
bool     mem_overlay(void);
// DMA-side chip RAM access (word address, already masked by the caller).
static inline uint16_t chip_rw(uint32_t addr) {
    const uint8_t *p = A.cfg.chip_ram + (addr & A.chip_mask & ~1u);
    return U16BE(p);
}
static inline void chip_ww(uint32_t addr, uint16_t v) {
    uint8_t *p = A.cfg.chip_ram + (addr & A.chip_mask & ~1u);
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}

// ---- custom chip register dispatch (custom.c) ------------------------------
uint16_t custom_read(uint32_t reg);                 // CPU read, reg = byte offset & 0x1FE
void     custom_write(uint32_t reg, uint16_t v, int hpos, bool from_copper);
void     custom_reset(void);

// ---- copper (copper.c) -----------------------------------------------------
void     copper_reset(void);
void     copper_vblank(void);                       // restart from COP1LC at line 0
void     copper_line_start(void);
void     copper_run(int until_h);                   // execute up to colour clock `until_h`
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
void     denise_line_start(void);
void     denise_line_end(void);
void     denise_frame_start(void);

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

// ---- CIAs (cia.c) ----------------------------------------------------------
void     cia_reset(void);
uint8_t  cia_read(int which, int reg);              // which: 0 = CIA-A, 1 = CIA-B
void     cia_write(int which, int reg, uint8_t v);
void     cia_line(void);                            // HSYNC: CIA-B TOD + timers catch-up
void     cia_vsync(void);                           // VSYNC: CIA-A TOD
void     cia_flag(int which);                       // /FLAG input pulse (CIA-B: disk index)
void     cia_keyboard_byte(uint8_t sdr);            // CIA-A serial port byte from the keyboard
bool     cia_keyboard_handshake(void);              // true once the CPU acknowledged
