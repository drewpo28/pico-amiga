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

// 32 (2026-11-09, was 16): a slice's fixed cost (the copper's and the CPU's
// entries, idle_skip, bus_wait, the profile switches, ~300 cycles) was ~11% of
// core 0 in Roots' heavy parts at 14 slices a line; 8 a line halve it.
#ifndef SLICE_CC
#define SLICE_CC 32
#endif
// The sprite DMA slots ($15-$34) are fetched after this slice: a DMACON write
// before them (the copper's at $10) has been seen.
#ifndef SPRITE_DMA_CC
#define SPRITE_DMA_CC 32
#endif

amiga_t A;
unsigned int amiga_bus_instr;                    // instructions run (m68k_execute)
#ifdef AMIGA_STATS
unsigned short amiga_icache_tag[64];             // the 68020's instruction cache, by line (m68kcpu.h): the bench's
unsigned int amiga_icache_miss;
#else
#define amiga_icache_miss 0u                     // the board keeps no tags (SRAM: the menu's heap)
#endif
unsigned int amiga_stat_data, amiga_stat_fetch, amiga_stat_audw;  // AMIGA_STATS counters
#ifdef AMIGA_STATS
unsigned int amiga_stat_hist[2048];                                // 8 KB: the bench only
unsigned int *amiga_pc_hist;                                       // the bench's --pchist (host/main.c)
#endif
uint8_t  prof_cur;
uint32_t prof_t, prof_acc[AMIGA_PROF_COUNT];
static uint32_t prof_frames;

// ---- time ----------------------------------------------------------------------
AMIGA_HOT uint64_t amiga_now(void) {
    if (!A.in_cpu) return A.cpu_cyc;
    const int run = m68k_cycles_run();
    // With the part of a cycle the last slice left over (cpu_rem): "now" does not
    // depend on where the slices were cut.
    return A.cpu_cyc + (uint64_t)(A.cpu_mul > 1 ? ((unsigned)run + A.cpu_rem) / A.cpu_mul : (unsigned)run);
}

AMIGA_HOT uint16_t amiga_hpos(void) {
    uint64_t now = amiga_now();
    if (now <= A.line_cyc) return 0;
    uint64_t h = (now - A.line_cyc) >> 1;
    return (uint16_t)(h >= AMIGA_CC_PER_LINE ? AMIGA_CC_PER_LINE - 1 : h);
}

// A slice longer than SLICE_CC (run_line merges them while the copper waits)
// ends where the CPU does something the short ones bounded the delay of: starts
// a blit (its end's interrupt), writes the copper's registers or DMACON.
static bool slice_long, slice_cut;
AMIGA_HOT void amiga_slice_cut(void) {
    if (slice_long && A.in_cpu) { slice_cut = true; m68k_end_timeslice(); }
}

AMIGA_HOT __attribute__((noinline)) static void cpu_until(uint64_t target) {    // noinline: run_line is in SRAM, and smaller this way
    const unsigned mul = A.cpu_mul > 1 ? A.cpu_mul : 1;
    if (idle_skip(target)) return;                                 // a busy-wait loop (idle.c)
    while (A.cpu_cyc < target) {
        A.in_cpu = true;
        int used = m68k_execute((int)(target - A.cpu_cyc) * (int)mul);
        A.in_cpu = false;
        if (used <= 0) { A.cpu_cyc = target; break; }
        if (mul == 1) A.cpu_cyc += (uint64_t)used;
        else {
            const unsigned u = (unsigned)used + A.cpu_rem;       // load test: mul cycles per one
            A.cpu_cyc += u / mul;
            A.cpu_rem = (uint8_t)(u % mul);
        }
        if (slice_cut) break;
    }
}

// The chip bus (amiga_set_chip_bus), after the CPU ran a slice [h0, h1) of
// colour clocks: a 68EC020 without fast RAM gets the chip bus for one access
// every two colour clocks, and only the clocks the DMA leaves: the bitplanes'
// (denise_bus_busy8), a running blit's (three in four, all of them with
// BLTPRI), and ~27 a line of sprites, disk, audio and refresh.  Its accesses
// are the chip data accesses mem.c counted and the fetches of the instructions
// it ran (bus_fetch8 below: three quarters of a 32-bit fetch each, no cache).
// What they need
// beyond the slice's free clocks is added to the CPU's time: the next slices
// start later, as on the machine.
#define BUS_FIXED8 15                        // ~27 of 227 clocks a line, per 16-clock slice, in eighths
// amiga_set_chip_bus_params, in eighths: fetch accesses an instruction and
// clocks an access.  6, 16 (no instruction cache credit, two clocks an access):
// ~1.3 MIPS, a stock A1200's; Roots' music routine, called from its frame loop,
// then runs every other frame in two parts (bench: `audw`, writes to Paula a
// frame).  2, 16 (a quarter fetch: the 68020's 256-byte cache serving the rest)
// keeps it at 50 Hz, as a free bus does - a sharp threshold, 3, 16 does not -
// at ~1.7 MIPS, 35,000 instructions a frame: 14-19 ms of the board's core 0 in
// those parts, over the frame with the blitter and copper (2026-11-09 log: fps
// 42-45, the audio ring starving everywhere heavy).  So the stock model is the
// default and the menu offers the other (Machine > A1200 > Chip bus).
static unsigned bus_fetch8 = 6, bus_cost8 = 16;
void amiga_set_chip_bus_params(int fetch8, int cost8) { bus_fetch8 = (unsigned)fetch8; bus_cost8 = (unsigned)cost8; }

// amiga_set_chip_bus_auto: the stock parameters, and the cache's (2) while the
// program's music routine falls behind.  A player called from the program's
// frame loop (Roots) runs every other frame once the loop takes two: Paula's
// registers are then written in every second frame at most.  The last 16 frames
// say so - no two in a row with a write to AUDx, five or more with one - and the
// CPU gets the cache's fetch credit: Roots' zigzag part 27k -> 42k instructions
// a frame and its music back at 50 Hz (bench audw 5 -> 10; the sprite part 3 ->
// 6, all its tune writes).  It stays for BUS_HOLD frames at least and until the
// copper's list is one the last frames did not run (a new part), then the stock
// model is tried again.  A tune that really ticks every other frame keeps the
// faster CPU: that costs emulation time, nothing else.
#define BUS_HOLD 250
bool amiga_aud_written;                          // custom.c: an AUDx register was written this frame
static bool     bus_auto, bus_boost;
static uint16_t aud_hist = 0xFFFF, bus_hold;
static uint32_t bus_key[2];
void amiga_set_chip_bus_auto(bool on) { bus_auto = on; bus_boost = false; aud_hist = 0xFFFF; }
static void bus_auto_frame(void) {
    aud_hist = (uint16_t)(aud_hist << 1 | (amiga_aud_written ? 1 : 0));
    amiga_aud_written = false;
    const uint32_t key = copper_list();
    const bool known = key == bus_key[0] || key == bus_key[1];
    if (!known) { bus_key[1] = bus_key[0]; bus_key[0] = key; }
    if (!bus_auto) return;
    if (!bus_boost) {
        if (!(aud_hist & (aud_hist >> 1)) && __builtin_popcount(aud_hist) >= 5) { bus_boost = true; bus_hold = BUS_HOLD; }
    } else if (bus_hold) {
        bus_hold--;
    } else if (!known) {
        bus_boost = false;
        aud_hist = 0xFFFF;
    }
}
AMIGA_HOT static void bus_wait(int h0, int h1) {
    static uint32_t instr_last, data_last, miss_last;
    const uint32_t data_now = A.bus_data + m68k_fast_data();        // mem.c's callbacks + the handlers' own
    const uint32_t instr = amiga_bus_instr - instr_last, data = data_now - data_last;
    const uint32_t miss = amiga_icache_miss - miss_last;
    instr_last = amiga_bus_instr; data_last = data_now; miss_last = amiga_icache_miss;
    if (!instr && !data) return;
    // bus_fetch8 255 (the bench's --bus-tune only: the tags are kept under
    // AMIGA_STATS, m68kcpu.h): the instruction cache's misses are the fetches
    // (one longword access each), instead of a flat share of an access an instruction.
    const uint32_t fetch8 = bus_fetch8 == 255 ? miss * 8 : instr * (bus_boost ? 2u : bus_fetch8);
    const uint32_t acc8 = data * 8 + (mem_fetch_chip ? fetch8 : 0);
    const int need8 = (int)(acc8 * bus_cost8 / 8);                   // bus_cost8 / 8 clocks an access
    const int blit8 = blitter_busy() ? ((A.dmacon & DMAF_BLTPRI) ? 8 : 6) : 0;
    uint32_t over8;
    if (h1 - h0 <= SLICE_CC) {
        int free8 = (h1 - h0) * (8 - blit8) - denise_bus_busy8(h0, h1) - BUS_FIXED8;
        if (free8 < 0) free8 = 0;
        if (need8 <= free8) return;
        // A slice's CPU time is 4 x 16 clocks at most: more than 8 slices of wait is
        // a count from before the model was switched on, not a slice's.
        over8 = (uint32_t)(need8 - free8);
        if (over8 > (uint32_t)(h1 - h0) * 64u) over8 = (uint32_t)(h1 - h0) * 64u;
    } else {
        // A merged slice (run_line): the accesses spread evenly along it, and each
        // stretch's free clocks are its own - before the bitplanes' fetch, in it,
        // after it - as the short slices had it: what the border leaves free is
        // of no use to the accesses inside the window.  No blit ran when it began
        // (run_line); one the CPU started cut it short and has the bus in the
        // last SLICE_CC clocks only.
        int ds, de;
        denise_bus_window(&ds, &de);
        const int hx = blit8 ? h1 - SLICE_CC : h1;
        int cut[5] = { h0, ds, de, hx, h1 };
        for (int i = 1; i < 4; i++) cut[i] = cut[i] < h0 ? h0 : cut[i] > h1 ? h1 : cut[i];
        if (cut[3] < cut[2]) { const int t = cut[3]; cut[3] = cut[2]; cut[2] = t; }
        if (cut[2] < cut[1]) { const int t = cut[2]; cut[2] = cut[1]; cut[1] = t; }
        over8 = 0;
        for (int i = 0; i < 4; i++) {
            const int a = cut[i], b = cut[i + 1];
            if (b <= a) continue;
            const int bl8 = a < hx ? 0 : blit8;
            int free8 = (b - a) * (8 - bl8) - denise_bus_busy8(a, b) - BUS_FIXED8 * (b - a) / SLICE_CC;
            if (free8 < 0) free8 = 0;
            const int n8 = (int)((int64_t)need8 * (b - a) / (h1 - h0));
            if (n8 <= free8) continue;
            uint32_t o = (uint32_t)(n8 - free8);
            if (o > (uint32_t)(b - a) * 64u) o = (uint32_t)(b - a) * 64u;
            over8 += o;
        }
        if (!over8) return;
    }
    const uint32_t stall8 = over8 + A.bus_rem8;
    const uint32_t cc = stall8 / 8;
    A.bus_rem8 = (uint8_t)(stall8 % 8);
    A.cpu_cyc += 2u * cc;
    A.bus_stall += cc;
}

// amiga_set_fast_throttle: per slice, the instructions run while the CPU fetched
// from fast RAM at its end (as bus_wait counts its fetches) are charged t8 / 8
// colour clocks each.  A parked CPU (idle.c) runs none and pays nothing.
static unsigned fast_thr8;
static uint32_t thr_instr_last;
void amiga_set_fast_throttle(unsigned t8) {
    if (t8 && !fast_thr8) thr_instr_last = amiga_bus_instr;
    fast_thr8 = t8;
}
AMIGA_HOT static void fast_throttle(void) {
    const uint32_t instr = amiga_bus_instr - thr_instr_last;
    thr_instr_last = amiga_bus_instr;
    if (!mem_fetch_fast || !instr) return;
    const uint32_t t8 = instr * fast_thr8 + A.thr_rem8;
    A.thr_rem8 = (uint8_t)(t8 % 8);
    A.cpu_cyc += 2u * (t8 / 8);
}

void amiga_set_chip_bus(bool on) { A.chip_bus = on; }
void amiga_set_render_skip(bool on) { A.render_skip = on; }

static void cpu_mul_update(void) {
    A.cpu_mul = (uint8_t)((A.cpu_clock ? A.cpu_clock : 1) * (A.load_mul ? A.load_mul : 1));
    A.cpu_rem = 0;
}

void amiga_set_load_test(int cpu_mul, bool extra_planes) {
    A.load_mul = (uint8_t)(cpu_mul < 1 ? 1 : cpu_mul > 3 ? 3 : cpu_mul);
    A.load_planes = extra_planes;
    cpu_mul_update();
}

// The 68000 RESET instruction asserts the reset line for the peripherals.
// The expansion boards drop their configuration: Kickstart's ColdReboot (RESET,
// then the ROM's entry) configures them again.
void amiga_cpu_reset_instr(void) {
    cia_reset();
    mem_autoconfig_reset();
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
int amiga_lace_field(void) { return denise_lace() ? (A.lof ? 0 : 1) : -1; }
void amiga_set_hdf(int unit, const amiga_disk_t *disk) { ide_insert(unit, disk); ide_reset(); }
void amiga_floppy_status(amiga_floppy_t *st) { floppy_status(st); }
// Read from the output IRQs (audio_out.c, hdmi_audio.c), hence AMIGA_HOT.
AMIGA_HOT uint32_t amiga_audio_write_pos(void) { return paula_audio_pos(); }

// ---- reset / init -------------------------------------------------------------
void amiga_set_roms(const uint8_t *rom, uint32_t rom_size, const uint8_t *ext, uint32_t ext_size) {
    A.cfg.rom = rom;     A.cfg.rom_size = rom_size;
    A.cfg.ext_rom = ext; A.cfg.ext_size = ext_size;
}

void amiga_set_cpu(int cpu) { A.cfg.cpu = (uint8_t)cpu; }
void amiga_set_akiko(bool on, uint8_t *nvram) { A.cfg.akiko = on; A.cfg.nvram = on ? nvram : NULL; }
void amiga_cd32_pad(uint8_t buttons) { A.pad_buttons = buttons; }

void amiga_set_machine(int chipset, uint32_t chip_size, uint32_t slow_size, uint8_t gayle) {
    A.cfg.chipset = (uint8_t)chipset;
    A.cfg.gayle = gayle;
    A.cfg.chip_size = chip_size;
    A.cfg.slow_size = slow_size;
}

void amiga_set_fast(uint8_t *ram, uint32_t size) {
    A.cfg.fast_ram = size ? ram : NULL;
    A.cfg.fast_size = ram ? size : 0;
}

void amiga_reset(bool hard) {
    A.ecs = A.cfg.chipset == AMIGA_ECS || A.cfg.chipset == AMIGA_AGA;
    A.aga = A.cfg.chipset == AMIGA_AGA;
    // The CPU: a 68000 at 7 MHz, or a 68EC020 at 14 MHz (the A1200's: twice the
    // cycles per colour clock).  Musashi's handler set follows (m68k_set_cpu_type).
    m68k_set_cpu_type(A.cfg.cpu == AMIGA_CPU_EC020 ? M68K_CPU_TYPE_68EC020 : M68K_CPU_TYPE_68000);
    A.cpu_clock = A.cfg.cpu == AMIGA_CPU_EC020 ? 2 : 1;
    cpu_mul_update();
    A.chip_mask = A.cfg.chip_size - 1;
    if (hard) {
        memset(A.cfg.chip_ram, 0, A.cfg.chip_size);
        if (A.cfg.slow_ram && A.cfg.slow_size) memset(A.cfg.slow_ram, 0, A.cfg.slow_size);
        if (A.cfg.fast_ram && A.cfg.fast_size) memset(A.cfg.fast_ram, 0, A.cfg.fast_size);
    }
    mem_init();
    custom_reset();
    cia_reset();
    rtc_reset();
    gayle_reset();
    akiko_reset();
    A.pad_shift = 8; A.pad_clk = 0;
    idle_reset();
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
    for (int h = SLICE_CC, h0 = 0; ; h0 = h, h += SLICE_CC) {
        // A slice bounds how stale the copper's and the CPU's views of each other
        // are, so it need not end before the copper has anything to do: it waits
        // for a later clock or for another line through most of most lines, and
        // the slices' fixed cost (the CPU's entry and exit, the bus model, the
        // idle test: ~330 ARM instructions each, 2,250 a frame) was more than the
        // CPU loop's own.  The merged slice ends on the same grid, at the start of
        // the slice the copper's next move or a running blit's end (its interrupt)
        // is in, and where the CPU cuts it (amiga_slice_cut): the machine's state
        // stays what the short slices made it, bit for bit (checked with idle.c
        // off: a wait loop is noticed a slice later, a few cycles' difference),
        // but for the bus model's waits (bus_wait).  The first slice stays as it
        // is (the sprite DMA follows it).
        if (h0 >= SPRITE_DMA_CC && h < AMIGA_CC_PER_LINE) {
            int n = copper_next(h);
            const uint64_t done = blitter_done_cyc();
            if (done) {
                // Under the bus model a blit keeps the short slices: the CPU is held
                // back slice by slice there, and a long one let it run ahead of the
                // bus it does not have (Roots' plasma: 4% fewer instructions a frame
                // for the busy-flag polls it then had time for).
                const int hb = A.chip_bus && A.cpu_clock > 1 ? h
                             : done > A.line_cyc ? (int)((done - A.line_cyc + 1) / 2) : 0;
                if (hb < n) n = hb;
            }
            n = n / SLICE_CC * SLICE_CC;
            if (n > h) h = n;
        }
        if (h > AMIGA_CC_PER_LINE) h = AMIGA_CC_PER_LINE;
        slice_long = h - h0 > SLICE_CC;
        slice_cut = false;
        if (!copper_asleep) {
            prof_switch(AMIGA_PROF_COPPER);
            copper_run(h0, h);
        }
        prof_switch(AMIGA_PROF_CPU);
        for (;;) {
            cpu_until(A.line_cyc + 2u * (uint32_t)h);
            if (!slice_cut) break;
            // Cut: back to the slices' grid from where the CPU is, and on to there.
            slice_cut = slice_long = false;
            const uint64_t now = A.cpu_cyc > A.line_cyc ? A.cpu_cyc - A.line_cyc : 0;
            const int hc = ((int)(now / 2) / SLICE_CC + 1) * SLICE_CC;
            if (hc < h) h = hc;
        }
        if (A.chip_bus && A.cpu_clock > 1) bus_wait(h0, h);
        if (fast_thr8) fast_throttle();
        prof_switch(AMIGA_PROF_OTHER);
        blitter_poll();
        if (h == SPRITE_DMA_CC) {
            prof_switch(AMIGA_PROF_LINE);
            denise_sprite_dma();
            prof_switch(AMIGA_PROF_OTHER);
        }
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
    if (A.cfg.gayle) gayle_line();
    if (A.cfg.akiko) { prof_switch(AMIGA_PROF_DISK); akiko_line(); prof_switch(AMIGA_PROF_OTHER); }   // its sector reads
    keyboard_line();
    A.line_cyc += 2u * AMIGA_CC_PER_LINE;
}

void amiga_run_frame(void) {
    // Only the time inside the frame is charged: start the clock here and bring
    // the buckets up to date at the end (the wait for the next frame is nobody's).
    if (A.cfg.prof_clock) { prof_t = A.cfg.prof_clock(); prof_cur = AMIGA_PROF_OTHER; }
    for (A.vpos = 0; A.vpos < A.lines; A.vpos++) run_line();
    denise_sync();                   // the frame is complete before anyone looks at it
    bus_auto_frame();
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

static uint32_t (*prof_clock_saved)(void);

void amiga_set_profiling(bool on) {
    if (!prof_clock_saved) prof_clock_saved = A.cfg.prof_clock;
    if (!prof_clock_saved || on == (A.cfg.prof_clock != NULL)) return;
    if (on) prof_t = prof_clock_saved();                 // no time charged for the pause
    A.cfg.prof_clock = on ? prof_clock_saved : NULL;
}

void amiga_get_profile(amiga_profile_t *p, bool reset) {
    for (int i = 0; i < AMIGA_PROF_COUNT; i++) p->ticks[i] = prof_acc[i];
    p->ticks[AMIGA_PROF_CORE1] = denise_core1_ticks();
    p->frames = prof_frames;
    p->idle_cycles = A.idle_cycles;
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
    denise_status(st);
    st->instr = amiga_bus_instr;
    st->bus_stall = (uint32_t)A.bus_stall;
    st->data_acc = amiga_stat_data;
    st->fetch_acc = amiga_stat_fetch;
}
