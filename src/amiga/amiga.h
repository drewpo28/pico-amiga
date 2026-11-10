// pico-amiga — Amiga 500 / 500+ / 600 (OCS / ECS, PAL) emulation core.
//
// This header is the whole contract between the emulated machine and whatever
// runs it: the RP2350 firmware (src/pico/) or the headless host runner (host/).
// The core never allocates, never touches hardware and never blocks; the
// platform hands it memory, calls amiga_run_frame() 50 times a second and
// collects pixels/samples through the buffers it supplied.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- Display ---------------------------------------------------------------
// The framebuffer is AMIGA_FB_H rows, one per PAL line from beam line
// AMIGA_FB_Y0; column 0 is lowres DIW coordinate x0 (amiga_set_fb_x0):
// AMIGA_FB_X0_CENTRE puts the standard DIWSTRT $2C81 window at (20, 18),
// 20 columns of border each side; AMIGA_FB_X0_WIDE at (32, 18), for overscan
// screens that start further left (the line ends 5 columns past the standard
// window, so the right side has nothing more to show).  Shown as 720x576: every row
// covers two output lines and 720 output pixels (one per HIRES pixel).
//
// Each row picks its own storage so that 288 rows fit in ~245 KB:
//   AMIGA_ROW_DIRECT  360 x 0x0RGB, one per lowres pixel (each shown twice).
//                     Lowres lines, HAM, and the fallback for anything else.
//   AMIGA_ROW_IDX8    720 x 8-bit index into pal[] (up to 64 colours): a
//                     hires line at full resolution.
//   AMIGA_ROW_LACE4   two lines of 720 x 4-bit index - the long field's line
//                     (output line 2r, pal[0..15]) and the short field's line
//                     (2r+1, pal[16..31]): interlace at full resolution.
// A line that needs more colours than its mode holds falls back to DIRECT
// (hires pixel pairs averaged, interlace showing the latest field).
// amiga_fb_line() expands any row into 720 colours, as the scanout does.
#define AMIGA_FB_W   360    // lowres columns
#define AMIGA_OUT_W  720    // output pixels per line
#define AMIGA_FB_H   288
#define AMIGA_FB_X0_CENTRE 0x6D   // lowres DIW x shown in column 0: centred
#define AMIGA_FB_X0_WIDE   0x61   // left overscan
#define AMIGA_FB_Y0  26     // beam line shown in row 0

enum { AMIGA_ROW_DIRECT = 0, AMIGA_ROW_IDX8 = 1, AMIGA_ROW_LACE4 = 2 };

typedef struct {
    uint8_t  mode;          // AMIGA_ROW_*
    uint8_t  ncol[2];       // palette entries in use (LACE4: per field line)
    uint8_t  pad;
    uint16_t pal[64];       // IDX8: 0..63; LACE4: field 0 = 0..15, field 1 = 16..31
    union {
        uint16_t rgb[AMIGA_FB_W];        // DIRECT
        uint8_t  idx[AMIGA_OUT_W];       // IDX8
        uint8_t  nib[2][AMIGA_FB_W];     // LACE4: two pixels per byte, high nibble first
    } px;
} amiga_row_t;

// Output line `sub` (0 = upper, 1 = lower) of a row as 720 0x0RGB colours.
void amiga_fb_line(const amiga_row_t *row, int sub, uint16_t out[AMIGA_OUT_W]);
// Paint the whole framebuffer one colour (DIRECT rows), e.g. for boot errors.
void amiga_fb_fill(amiga_row_t *fb, uint16_t colour);
// The lowres DIW x shown in framebuffer column 0, from the next line on.
void amiga_set_fb_x0(int x0);
// Vertical centring.  Off (the default): row 0 is beam line AMIGA_FB_Y0.  On:
// the lines with bitplane DMA are centred in the AMIGA_FB_H rows (row 0 down to
// beam line 0), so a short picture - an NTSC-sized 200-line game - sits in the
// middle instead of at the top; a new height is followed once it has held for
// half a second, so screens that flash or open their window line by line do
// not make the picture jump.
void amiga_set_vcentre(bool on);
// Frame skipping: on, the next frame (from its start) is emulated but not
// drawn - no bitplane reads, no fetch or output work, the framebuffer keeps the
// last picture; the display registers, sprites and copper are run as ever.
// The platform sets it for a frame when the one before overran (main.c).
void amiga_set_render_skip(bool on);
// Framebuffer rows [y0, y1) carry a platform overlay (row_done); y1 <= y0: none.
// Two ranges (`which` 0 or 1), so two overlays far apart (a band at the bottom,
// an icon at the top) do not send every row between them through row_done.
void amiga_set_overlay_rows(int which, int y0, int y1);
// Denise on another core: with async on, the emulation queues the display
// work and that core runs it by calling amiga_render_pump() (one small step
// per call; false when there is nothing queued) until amiga_run_frame() has
// waited for it.  Off (the default): the emulation renders as it goes.
void amiga_render_async(bool on);
bool amiga_render_pump(void);

// ---- Audio -----------------------------------------------------------------
// Paula is sampled twice per line: 313 * 2 * 50 = 31300 Hz, interleaved stereo
// int16 (L = channels 0+3, R = 1+2), written into a ring the platform drains.
#define AMIGA_AUDIO_RATE 31300

// ---- Timing (PAL) ----------------------------------------------------------
#define AMIGA_CC_PER_LINE    227        // colour clocks per line
#define AMIGA_LINES_LONG     313        // long frame (LOF=1); short frame 312
#define AMIGA_CPU_HZ         7093790    // 68000 clock, 2 CPU cycles per colour clock

// Chipsets (amiga_config_t.chipset).  ECS is the A500+'s: Agnus 8375 (21-bit
// DMA pointers, large blits, DIWHIGH, BEAMCON0 stored) and Denise 8373
// (DENISEID, super-hires, BPLCON3's border blank).
// AGA is the A1200's: Alice + Lisa (being built: their IDs, Lisa's 256-colour palette
// and BPLCON4 so far; the 64-bit fetch and 8 planes to come).  It has ECS's features.
enum { AMIGA_OCS = 0, AMIGA_ECS = 1, AMIGA_AGA = 2 };
#define AMIGA_GAYLE_A600  0xD0   // Gayle's ID
#define AMIGA_GAYLE_A1200 0xD1   // the A1200's (AGA) Gayle
// CPUs (amiga_config_t.cpu): the 68000 at 7.09 MHz, or the A1200's 68EC020 at 14.19 MHz.
enum { AMIGA_CPU_68000 = 0, AMIGA_CPU_EC020 = 1 };
#define AMIGA_TRACK_WORDS 6334   // a DD track at 300 rpm / 500 kbit/s (floppy.c)

typedef struct {
    // Memory supplied by the platform. Contents are big-endian Amiga order.
    uint8_t  *chip_ram;      uint32_t chip_size;   // 512 KB (A500), 1 MB (A500+); power of two
    uint8_t  *slow_ram;      uint32_t slow_size;   // 0 or 512 KB "trapdoor" at $C00000
    uint8_t   chipset;       // AMIGA_OCS / AMIGA_ECS
    // Gayle (A600, A1200): its ID (AMIGA_GAYLE_*), or 0 for none.  A Gayle machine
    // has an empty IDE port and PCMCIA slot at $DA0000, CIAs at $BF0000 only, no
    // custom-register mirror at $C00000-$D7FFFF and no battery clock (rtc_time
    // is ignored), as WinUAE maps them.
    uint8_t   gayle;
    uint8_t   cpu;           // AMIGA_CPU_68000 / AMIGA_CPU_EC020 (amiga_set_cpu)
    const uint8_t *rom;      uint32_t rom_size;    // 256 KB (mirrored) or 512 KB at $F80000
    const uint8_t *ext_rom;  uint32_t ext_size;    // 0 or 512 KB at $E00000 (AROS ext)
    // Fast RAM on a Zorro II autoconfig board (mem.c): 0 or 1 / 2 / 4 / 8 MB,
    // where Kickstart puts it ($200000 on).  WHDLoad's "other memory".
    uint8_t  *fast_ram;      uint32_t fast_size;

    amiga_row_t *framebuffer; // AMIGA_FB_H rows, or NULL (no video)

    int16_t  *audio_ring;    // interleaved stereo frames, or NULL
    uint32_t  audio_frames;  // ring capacity in stereo frames (power of two)

    // The floppy's MFM track: AMIGA_TRACK_WORDS words the CPU alone touches, at
    // a disk DMA (the firmware's sits in PSRAM, next to chip RAM).
    uint16_t *track;

    // Optional hooks (may be NULL).
    void (*serial_out)(void *ctx, uint8_t byte);   // SERDAT, for DiagROM / debug
    void (*frame_done)(void *ctx);                 // after each completed frame
    // Overlay rows (amiga_set_overlay_rows): Denise builds each of them in a
    // scratch row, hands it to row_done, which draws the overlay on it, and only
    // then copies it into the framebuffer, so the scanout never shows such a
    // row without its overlay.  Called for the overlay rows only.
    void (*row_done)(void *ctx, int row, amiga_row_t *r);
    // Profiling clock (any free-running counter, e.g. CPU cycles), or NULL: when
    // set, the core charges the time between its section switches to
    // amiga_profile_t's buckets.
    uint32_t (*prof_clock)(void);
    // Battery clock (MSM6242B at $DC0000, as on the A500+ board and the A501
    // card): the local time as seconds since 1970-01-01, read when Kickstart
    // reads the clock.  NULL: no clock (the registers read 0, as an empty slot).
    int64_t (*rtc_time)(void *ctx);
    void *ctx;
} amiga_config_t;

// Floppy image: 880 KB ADF, accessed through callbacks so the platform can
// stream tracks from SD instead of holding the image in RAM.
typedef struct {
    int  (*read)(void *ctx, uint32_t offset, void *dst, uint32_t len);       // 0 = ok
    int  (*write)(void *ctx, uint32_t offset, const void *src, uint32_t len); // NULL = write-protected
    void *ctx;
    uint32_t size;           // bytes (901120 for a DD ADF)
} amiga_disk_t;

void amiga_init(const amiga_config_t *cfg);
void amiga_reset(bool hard);             // hard: clears chip RAM as on power-on
// Swap the Kickstart (and extended) ROM; takes effect at the next amiga_reset(),
// which a ROM change wants hard.  Same rules as amiga_config_t's fields.
void amiga_set_roms(const uint8_t *rom, uint32_t rom_size, const uint8_t *ext, uint32_t ext_size);
// Switch the model (A500 / A500+ / A600): chipset, RAM sizes and Gayle (0 or
// its ID), the buffers staying those of amiga_init().  Takes effect at the next
// amiga_reset(), a hard one.
void amiga_set_machine(int chipset, uint32_t chip_size, uint32_t slow_size, uint8_t gayle);
// The Zorro II fast RAM board (amiga_config_t.fast_ram): size 0 removes it.
// Takes effect at the next amiga_reset(), a hard one.
void amiga_set_fast(uint8_t *ram, uint32_t size);
void amiga_run_frame(void);              // one PAL frame (312 or 313 lines)

void amiga_insert_disk(int drive, const amiga_disk_t *disk);  // drive 0..3
void amiga_eject_disk(int drive);
// How many drives are connected (1..4, DF0 first; default 1). Kickstart looks for
// them at reset, so a change wants one.
void amiga_set_drives(int n);

// The A600's IDE hard disk (Gayle machines only): an HDF image, a whole drive
// with its RDB or one bare partition (ide.c), read and written a 512-byte
// sector at a time through the callbacks (write NULL: writes fail); NULL = no
// drive.  unit 0 is the master, 1 the slave (a bare partition there is DH1).
// Kickstart looks for them at reset, so a change wants one (hard).
void amiga_set_hdf(int unit, const amiga_disk_t *disk);
// The CPU (AMIGA_CPU_*): takes effect at the next amiga_reset(), a hard one.
void amiga_set_cpu(int cpu);
// The chip bus (a 68EC020 machine; the 68000's is not modelled): on, the CPU
// waits for its chip RAM and custom register accesses as the A1200's does - one
// every two colour clocks at most, and none of the clocks the bitplane DMA
// takes (all of them with 8 lowres planes and FMODE 0, a quarter with FMODE 3)
// or a running blit does.  A stock A1200 (no fast RAM) runs about half the
// instructions of a 14 MHz 68020 with its own memory; off, the CPU runs them
// all, as it did before.  Takes effect at once.
void amiga_set_chip_bus(bool on);
// Its parameters, in eighths: instruction-fetch accesses per instruction (6: no
// instruction cache credit, the default; 2: a quarter, the cache serving the
// rest - a faster CPU, ~35% more emulation time) and colour clocks per access (16).
void amiga_set_chip_bus_params(int fetch8, int cost8);

// The geometry the drive reports in IDENTIFY (0s: no drive).
void amiga_hdf_geometry(int unit, uint16_t *cyls, uint8_t *heads, uint8_t *sectors);

// Floppy activity, for the platform's disk line (Win+F8) and drive icon: the
// drive selected (or the last one a DMA used), its head, and what the disk DMA
// does.  `idle` counts frames since the last disk DMA ended (0 while one runs).
typedef struct {
    int8_t   drive;          // -1: none yet
    uint8_t  cyl, side;
    uint8_t  sector;         // 0..10: where the last DMA started; 11 = the gap after them
    bool     motor;
    uint8_t  dma;            // 0 none, 1 read, 2 write (running now)
    bool     wrote;          // the last DMA was a write
    uint32_t idle;
} amiga_floppy_t;
void amiga_floppy_status(amiga_floppy_t *st);

// Hard disk (IDE) activity, for the same: the block the last transfer moved, and
// running totals the platform turns into a rate.  `idle` counts frames since
// the last sector moved.
typedef struct {
    bool     present;        // a drive is there (either one)
    uint8_t  op;             // the last transfer: 1 read, 2 write (0 none yet)
    bool     busy;           // a transfer is running now
    uint32_t lba;            // the last block it moved (the drive's, the virtual RDB included)
    uint32_t idle;
    uint32_t rd_blocks, wr_blocks;   // since the drive went in
} amiga_ide_t;
void amiga_ide_status(amiga_ide_t *st);

// Input. Keycodes are Amiga raw keycodes ($00-$67); see src/amiga/keymap.h.
void amiga_key(uint8_t amiga_code, bool down);
void amiga_mouse(int dx, int dy, bool left, bool right);     // port 1 (JOY0DAT)
void amiga_joystick(int port, uint8_t dirs, bool fire);      // dirs: bit0 up 1 down 2 left 3 right

// Audio ring write position (monotonic frame count; ring index = pos % frames).
uint32_t amiga_audio_write_pos(void);

// ---- Snapshots ---------------------------------------------------------------
// The machine's state between two amiga_run_frame() calls: the 68000, the
// custom chips, the CIAs, the drives' mechanics and DMA, chip and slow RAM, as
// blocks of id[4] size(u32 LE) data, unknown ids skipped on load.  Not in it:
// the ROMs (a CRC of each is, and must match), the disks' contents and the
// platform's buffers.  Before amiga_state_load() the platform sets the model
// and ROMs the snapshot was taken with (amiga_set_machine / amiga_set_roms)
// and puts its disks back in their drives (amiga_insert_disk); the load then
// resets the Amiga and replaces its state.  It fails before touching anything
// when the model or a ROM differs; a failure after that (a damaged file)
// leaves a half-loaded machine the platform must reset.
typedef struct {
    bool     (*write)(void *ctx, const void *p, uint32_t n);   // save: false = failed
    uint32_t (*read)(void *ctx, void *p, uint32_t n);          // load: bytes read, 0 at the end
    void *ctx;
} amiga_stream_t;
bool amiga_state_save(const amiga_stream_t *io);
bool amiga_state_load(const amiga_stream_t *io);
// The CRC-32 a snapshot keeps of each ROM (0 = no extended ROM), for the
// platform to check the right one is there before a load.  zlib's crc32():
// start from 0, feed the data in any number of pieces.
uint32_t amiga_crc32(uint32_t crc, const void *p, uint32_t n);

// A1200 headroom test (docs/PLAN.md, section 7, step 0), off by default and not
// in snapshots: cpu_mul 2 gives the 68000 twice the cycles per line (a 14 MHz
// CPU's load: twice the instructions in the same emulated time), 3 three times
// (a 68EC020 does more per cycle than a 68000: an A1200 benchmarks at 2.5-3x an
// A500), extra_planes
// reads two more words per plane fetch unit from chip RAM and spreads them into
// a scratch line (the cost of 8 planes where a screen has 6; the picture is
// unchanged).  For measuring ms per frame only: games run too fast with it.
void amiga_set_load_test(int cpu_mul, bool extra_planes);

// Diagnostics.
typedef struct {
    uint32_t frame;
    uint32_t pc;
    uint16_t dmacon, intena, intreq;
    uint8_t  overlay;
    uint32_t cop_pc;
    uint32_t blits;
    uint16_t bplcon0, fmode;     // the display's, at the frame's end
    uint16_t dma_lines;          // lines of the last frame with bitplane DMA on
    uint32_t instr;              // instructions run since reset
    uint32_t bus_stall;          // colour clocks the CPU waited for the chip bus (amiga_set_chip_bus)
    // Built with AMIGA_STATS (the host bench): the CPU's data accesses and
    // instruction-fetch words since reset.
    uint32_t data_acc, fetch_acc;
} amiga_status_t;
void amiga_get_status(amiga_status_t *st);

// Where the time went (cfg.prof_clock ticks) since the last reset: the 68000
// (blits excluded), the copper (renders excluded), the Denise render (bitplane
// fetch + pixel output, wherever it is triggered), the blitter, Paula, the line
// start (sprites), the floppy, and the rest (CIAs, keyboard, line bookkeeping).
// Interrupts on the emulator's core land in whatever bucket they interrupt.
enum { AMIGA_PROF_OTHER, AMIGA_PROF_CPU, AMIGA_PROF_COPPER, AMIGA_PROF_RENDER,
       AMIGA_PROF_BLIT,
       AMIGA_PROF_PAULA,    // the audio channels, twice a line
       AMIGA_PROF_LINE,     // Denise's line start: sprite DMA and composition
       AMIGA_PROF_DISK,     // floppy DMA, track encoding (the ADF reads included)
       AMIGA_PROF_BPL,      // of the render: the bitplane fetch (spreading the words)
       AMIGA_PROF_BPLRD,    // of the fetch: reading the plane words from chip RAM
       AMIGA_PROF_RWAIT,    // waiting for the render core (async rendering)
       AMIGA_PROF_CORE1,    // the render core's time (async rendering; its own clock)
       AMIGA_PROF_COUNT };
// idle_cycles: CPU cycles not run because the CPU sat in a busy-wait loop (idle.c),
// a running count since the reset.
typedef struct { uint32_t ticks[AMIGA_PROF_COUNT]; uint32_t frames; uint64_t idle_cycles; } amiga_profile_t;
void amiga_get_profile(amiga_profile_t *p, bool reset);
// Count it or not (cfg.prof_clock set): off, the section switches cost nothing
// (~2% of the emulation's core while counting on the board).  The counts stop
// while off; the next reader's first period is partial.
void amiga_set_profiling(bool on);

#ifdef __cplusplus
}
#endif
