// Machine glue: reset, the line/frame scheduler, the 68000, input.
//
// Scheduling.  A PAL line is 227 colour clocks (454 CPU cycles).  It is run
// in SLICE_CC-wide slices; in each slice the copper executes up to the slice
// end, then the CPU does, then a finished blit raises its interrupt.  Within
// a slice the copper and CPU are not ordered against each other, but every
// display-register write carries its own hpos, so what is drawn is exact; the
// slice only bounds how stale one side's view of the other can be.
// At the end of the line: Denise finishes the line, Paula emits two samples,
// the floppy and CIAs advance, the keyboard gets a chance to send.
#include <string.h>
#include "amiga_int.h"
#include "m68k.h"

#ifndef SLICE_CC
#define SLICE_CC 16
#endif

amiga_t A;
uint8_t  prof_cur;
uint32_t prof_t, prof_acc[AMIGA_PROF_COUNT];
static uint32_t prof_frames;

// ---- time ----------------------------------------------------------------------
AMIGA_HOT uint64_t amiga_now(void) {
    return A.in_cpu ? A.cpu_cyc + (uint64_t)m68k_cycles_run() : A.cpu_cyc;
}

AMIGA_HOT uint16_t amiga_hpos(void) {
    uint64_t now = amiga_now();
    if (now <= A.line_cyc) return 0;
    uint64_t h = (now - A.line_cyc) >> 1;
    return (uint16_t)(h >= AMIGA_CC_PER_LINE ? AMIGA_CC_PER_LINE - 1 : h);
}

AMIGA_HOT static void cpu_until(uint64_t target) {
    while (A.cpu_cyc < target) {
        A.in_cpu = true;
        int used = m68k_execute((int)(target - A.cpu_cyc));
        A.in_cpu = false;
        if (used <= 0) { A.cpu_cyc = target; break; }
        A.cpu_cyc += (uint64_t)used;
    }
}

// The 68000 RESET instruction asserts the reset line for the peripherals.
void amiga_cpu_reset_instr(void) {
    cia_reset();
}

// ---- keyboard controller ---------------------------------------------------------
// Bytes go out through CIA-A's serial port, one at a time, each waiting for the
// CPU's handshake (SPMODE pulse) or a 143 ms timeout, like the 6570 does.
#define KQ 32
static uint8_t  kq[KQ];
static unsigned kq_head, kq_tail;
static bool     kb_waiting;
static uint32_t kb_timeout;

void amiga_key(uint8_t code, bool down) {
    unsigned next = (kq_head + 1) % KQ;
    if (next == kq_tail) return;              // full: drop, as the real buffer would
    kq[kq_head] = (uint8_t)((code & 0x7F) | (down ? 0 : 0x80));
    kq_head = next;
}

static void keyboard_line(void) {
    if (kb_waiting) {
        if (cia_keyboard_handshake() || --kb_timeout == 0) kb_waiting = false;
        else return;
    }
    if (kq_tail == kq_head) return;
    uint8_t k = kq[kq_tail];
    kq_tail = (kq_tail + 1) % KQ;
    cia_keyboard_byte((uint8_t)~((k << 1) | (k >> 7)));
    kb_waiting = true;
    kb_timeout = 2235;                         // ~143 ms of lines
}

// ---- input --------------------------------------------------------------------
void amiga_mouse(int dx, int dy, bool left, bool right) {
    uint8_t x = (uint8_t)(A.joy0dat + dx);
    uint8_t y = (uint8_t)((A.joy0dat >> 8) + dy);
    A.joy0dat = (uint16_t)((y << 8) | x);
    A.mouse_lmb = left;
    A.mouse_rmb = right;
}

void amiga_joystick(int port, uint8_t dirs, bool fire) {
    if (port < 0 || port > 1) return;
    A.joy_dirs[port] = dirs;
    A.joy_fire[port] = fire;
}

void amiga_insert_disk(int drive, const amiga_disk_t *disk) { floppy_insert(drive, disk); }
void amiga_eject_disk(int drive) { floppy_eject(drive); }
void amiga_set_drives(int n) { floppy_set_drives(n); }
void amiga_floppy_status(amiga_floppy_t *st) { floppy_status(st); }
// Read from the output IRQs (audio_out.c, hdmi_audio.c), hence AMIGA_HOT.
AMIGA_HOT uint32_t amiga_audio_write_pos(void) { return paula_audio_pos(); }

// ---- reset / init -------------------------------------------------------------
void amiga_set_roms(const uint8_t *rom, uint32_t rom_size, const uint8_t *ext, uint32_t ext_size) {
    A.cfg.rom = rom;     A.cfg.rom_size = rom_size;
    A.cfg.ext_rom = ext; A.cfg.ext_size = ext_size;
}

void amiga_set_machine(int chipset, uint32_t chip_size, uint32_t slow_size) {
    A.cfg.chipset = (uint8_t)chipset;
    A.cfg.chip_size = chip_size;
    A.cfg.slow_size = slow_size;
}

void amiga_reset(bool hard) {
    A.ecs = A.cfg.chipset == AMIGA_ECS;
    A.chip_mask = A.cfg.chip_size - 1;
    if (hard) {
        memset(A.cfg.chip_ram, 0, A.cfg.chip_size);
        if (A.cfg.slow_ram && A.cfg.slow_size) memset(A.cfg.slow_ram, 0, A.cfg.slow_size);
    }
    mem_init();
    custom_reset();
    cia_reset();
    rtc_reset();
    copper_reset();
    blitter_reset();
    denise_reset();
    paula_reset();
    floppy_reset();
    kq_head = kq_tail = 0; kb_waiting = false;
    A.vpos = 0;
    A.lof = true;
    A.lines = AMIGA_LINES_LONG;
    A.line_cyc = A.cpu_cyc;
    m68k_pulse_reset();
}

void amiga_init(const amiga_config_t *cfg) {
    memset(&A, 0, sizeof A);
    A.cfg = *cfg;
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    amiga_reset(true);
}

// ---- scheduler ------------------------------------------------------------------
// noinline: inlined, it went where amiga_run_frame is (flash on the board), the
// slice loop and its 14 calls a line into SRAM through veneers, the code competing
// with chip RAM (PSRAM) for the XIP cache.
AMIGA_HOT __attribute__((noinline)) static void run_line(void) {
    if (A.vpos == 0) {
        amiga_set_intreq(INTF_VERTB);
        cia_vsync();
        copper_vblank();
        denise_frame_start();
    }
    prof_switch(AMIGA_PROF_LINE);
    denise_line_start();
    prof_switch(AMIGA_PROF_OTHER);
    copper_line_start();
    for (int h = SLICE_CC; ; h += SLICE_CC) {
        if (h > AMIGA_CC_PER_LINE) h = AMIGA_CC_PER_LINE;
        prof_switch(AMIGA_PROF_COPPER);
        copper_run(h);
        prof_switch(AMIGA_PROF_CPU);
        cpu_until(A.line_cyc + 2u * (uint32_t)h);
        prof_switch(AMIGA_PROF_OTHER);
        blitter_poll();
        if (h == AMIGA_CC_PER_LINE) break;
    }
    denise_line_end();
    prof_switch(AMIGA_PROF_PAULA);
    paula_half_line();
    paula_half_line();
    prof_switch(AMIGA_PROF_DISK);
    floppy_line();
    prof_switch(AMIGA_PROF_OTHER);
    cia_line();
    keyboard_line();
    A.line_cyc += 2u * AMIGA_CC_PER_LINE;
}

void amiga_run_frame(void) {
    // Only the time inside the frame is charged: start the clock here and bring
    // the buckets up to date at the end (the wait for the next frame is nobody's).
    if (A.cfg.prof_clock) { prof_t = A.cfg.prof_clock(); prof_cur = AMIGA_PROF_OTHER; }
    for (A.vpos = 0; A.vpos < A.lines; A.vpos++) run_line();
    denise_sync();                   // the frame is complete before anyone looks at it
    prof_switch(AMIGA_PROF_OTHER);
    A.vpos = 0;
    A.frame++;
    prof_frames++;
    // Agnus toggles the long-frame flag every frame while LACE is set: long
    // (313-line) and short (312-line) fields alternate, which is what makes the
    // copper lists of interlaced screens show both fields.
    A.lof = denise_lace() ? !A.lof : true;
    A.lines = A.lof ? AMIGA_LINES_LONG : AMIGA_LINES_LONG - 1;
    if (A.cfg.frame_done) A.cfg.frame_done(A.cfg.ctx);
}

// Snapshots (state.c): the beam and clock, the registers nobody else owns, the
// keyboard controller.  Taken between frames: vpos is 0.
void amiga_sys_state(st_t *s) {
    st_u32(s, &A.frame);
    st_u16(s, &A.lines);
    st_bool(s, &A.lof);
    st_u64(s, &A.line_cyc);
    st_u64(s, &A.cpu_cyc);
    st_u16(s, &A.dmacon); st_u16(s, &A.intena); st_u16(s, &A.intreq); st_u16(s, &A.adkcon);
    st_u16(s, &A.copcon); st_u16(s, &A.potgo); st_u16(s, &A.serper); st_u16(s, &A.beamcon0);
    st_u8(s, &A.ipl_pending);
    st_u16(s, &A.joy0dat); st_u16(s, &A.joy1dat);
    ST_ARRAY(s, st_u8, kq);
    st_uint(s, &kq_head); st_uint(s, &kq_tail);
    st_bool(s, &kb_waiting);
    st_u32(s, &kb_timeout);
    if (s->mode == ST_LOAD) {
        A.vpos = 0;
        A.in_cpu = false;
        kq_head %= KQ; kq_tail %= KQ;
    }
}

void amiga_get_profile(amiga_profile_t *p, bool reset) {
    for (int i = 0; i < AMIGA_PROF_COUNT; i++) p->ticks[i] = prof_acc[i];
    p->ticks[AMIGA_PROF_CORE1] = denise_core1_ticks();
    p->frames = prof_frames;
    if (reset) {
        for (int i = 0; i < AMIGA_PROF_COUNT; i++) prof_acc[i] = 0;
        prof_frames = 0;
    }
}

void amiga_get_status(amiga_status_t *st) {
    st->frame = A.frame;
    st->pc = m68k_get_reg(NULL, M68K_REG_PC);
    st->dmacon = A.dmacon;
    st->intena = A.intena;
    st->intreq = A.intreq;
    st->overlay = mem_overlay();
    st->cop_pc = copper_pc();
    st->blits = blitter_count();
}
