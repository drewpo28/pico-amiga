// pico-amiga — Amiga 500 (OCS, PAL) emulation core.
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
// The framebuffer is one row per PAL line, one entry per LOWRES pixel, each
// entry the Amiga's own 12-bit colour 0x0RGB (hires pixel pairs are averaged).
// Column 0 is DIW coordinate AMIGA_FB_X0, row 0 is beam line AMIGA_FB_Y0: the
// standard 320x256 window (DIWSTRT $2C81) lands at (32, 18), with overscan
// room on every side.  360x288 is exactly half of a 720x576 HDMI mode.
#define AMIGA_FB_W   360
#define AMIGA_FB_H   288
#define AMIGA_FB_X0  0x61   // lowres DIW x shown in column 0
#define AMIGA_FB_Y0  26     // beam line shown in row 0

// ---- Audio -----------------------------------------------------------------
// Paula is sampled twice per line: 313 * 2 * 50 = 31300 Hz, interleaved stereo
// int16 (L = channels 0+3, R = 1+2), written into a ring the platform drains.
#define AMIGA_AUDIO_RATE 31300

// ---- Timing (PAL) ----------------------------------------------------------
#define AMIGA_CC_PER_LINE    227        // colour clocks per line
#define AMIGA_LINES_LONG     313        // long frame (LOF=1); short frame 312
#define AMIGA_CPU_HZ         7093790    // 68000 clock, 2 CPU cycles per colour clock

typedef struct {
    // Memory supplied by the platform. Contents are big-endian Amiga order.
    uint8_t  *chip_ram;      uint32_t chip_size;   // 512 KB (A500); power of two
    uint8_t  *slow_ram;      uint32_t slow_size;   // 0 or 512 KB "trapdoor" at $C00000
    const uint8_t *rom;      uint32_t rom_size;    // 256 KB (mirrored) or 512 KB at $F80000
    const uint8_t *ext_rom;  uint32_t ext_size;    // 0 or 512 KB at $E00000 (AROS ext)

    uint16_t *framebuffer;   // AMIGA_FB_W * AMIGA_FB_H entries, or NULL (no video)

    int16_t  *audio_ring;    // interleaved stereo frames, or NULL
    uint32_t  audio_frames;  // ring capacity in stereo frames (power of two)

    // Optional hooks (may be NULL).
    void (*serial_out)(void *ctx, uint8_t byte);   // SERDAT, for DiagROM / debug
    void (*frame_done)(void *ctx);                 // after each completed frame
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
void amiga_run_frame(void);              // one PAL frame (312 or 313 lines)

void amiga_insert_disk(int drive, const amiga_disk_t *disk);  // drive 0..3 (only DF0 wired so far)
void amiga_eject_disk(int drive);

// Input. Keycodes are Amiga raw keycodes ($00-$67); see src/amiga/keymap.h.
void amiga_key(uint8_t amiga_code, bool down);
void amiga_mouse(int dx, int dy, bool left, bool right);     // port 1 (JOY0DAT)
void amiga_joystick(int port, uint8_t dirs, bool fire);      // dirs: bit0 up 1 down 2 left 3 right

// Audio ring write position (monotonic frame count; ring index = pos % frames).
uint32_t amiga_audio_write_pos(void);

// Diagnostics.
typedef struct {
    uint32_t frame;
    uint32_t pc;
    uint16_t dmacon, intena, intreq;
    uint8_t  overlay;
    uint32_t cop_pc;
    uint32_t blits;
} amiga_status_t;
void amiga_get_status(amiga_status_t *st);

#ifdef __cplusplus
}
#endif
