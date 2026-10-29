// Video > Output > Mode timings (video_modes.h).  The numbers are pico-speccy's
// (src/drivers/graphics/video_mode_table.h, its "bytes" doubled into pixels),
// with the line counts refitted to 50 Hz where pico-speccy fits each Spectrum.
#include "video_modes.h"
#include "amiga.h"

typedef struct {
    uint8_t  vm;
    uint8_t  outs;          // 1 << VIDEO_OUT_*
    uint16_t mhz;           // the clk_sys it is for; 0 = any
    video_timing_t t;
} mode_row_t;

#define O_HSTX (1u << VIDEO_OUT_HSTX)
#define O_PIO  (1u << VIDEO_OUT_PIO)
#define O_VGA  (1u << VIDEO_OUT_VGA)

// HSTX: clk_hstx = clk_sys / 1..4 and 5 of its cycles per pixel, so a pixel
// clock is clk_sys / 5, / 10, / 15 or / 20.  PIO HDMI: 10 PIO cycles a pixel
// at a divider of a whole or half number from 1 (a clean TMDS clock), so
// clk_sys / 10, / 15, / 20...  First match wins.
static const mode_row_t k_modes[] = {
    // 832 x 605, 50.06 Hz: pico-speccy's [5] (hw-proven); 25.2 MHz is 252/10,
    // 378/15, 504/20.  The PIO scanout keeps its own copy of it (tmds_lut.h).
    { VM_720x576_50, O_HSTX | O_PIO, 0, { 25200000, 720, 12, 64, 36,  576, 5, 5, 19,  50 } },
    // VGA: its twin, 880 x 614 at 27 MHz, 49.97 Hz (video_vga.c's own copy).
    { VM_720x576_50, O_VGA, 0,   { 27000000, 720, 16, 48, 96,  576, 4, 7, 27,  50 } },
    // VGA's 800 x 525 at 25.2 MHz, 60.0 Hz ([0]).
    { VM_640x480_60, O_HSTX | O_PIO | O_VGA, 0, { 25200000, 640, 16, 96, 48,  480, 10, 2, 33, 60 } },
    // 832 x 504, 60.10 Hz: 720x576's line, 480 of its lines ([7], 25.175 MHz there).
    { VM_720x480_60, O_HSTX | O_PIO, 0, { 25200000, 720, 12, 64, 36,  480, 10, 2, 12, 60 } },
    // VGA: 824 x 510, 59.97 Hz ([7]'s VGA half).
    { VM_720x480_60, O_VGA, 0,   { 25200000, 720, 16, 48, 40,  480, 20, 2, 8,   60 } },
    // 1008 x 750, 50.0 Hz ([31], 37.8 MHz = 378 / 10).
    { VM_800x600_X2, O_HSTX | O_PIO, 378, { 37800000, 800, 44, 72, 92,  600, 1, 4, 145, 50 } },
    // 1600 x 630, 50.0 Hz ([39], 50.4 MHz = 252 / 5, 504 / 10).
    { VM_800x600_X2, O_HSTX, 0,  { 50400000, 800, 32, 128, 640, 600, 1, 4, 25, 50 } },
    // PIO at 504: 1056 x 636, 50.03 Hz ([37], 33.6 MHz = 504 / 15).
    { VM_800x600_X2, O_PIO, 504, { 33600000, 800, 40, 128, 88,  600, 1, 4, 31, 50 } },
    // VGA: VESA 800x600 @60's line count and rates at 42 MHz ([37]'s VGA half:
    // 1108 x 628, 60.36 Hz), 252 / 6, 378 / 9, 504 / 12 - a monitor knows it,
    // where 800x600 @50 is nobody's mode.
    { VM_800x600_X2, O_VGA, 0,   { 42000000, 800, 48, 154, 106, 600, 1, 4, 23, 60 } },
    // 800x600 @60, framed: 625 lines (VESA's 628 less 3 of back porch) at 37.5 kHz,
    // 60.0 Hz on every row.  378: 1008 px, 800x600 @50's line (37.8 MHz).
    { VM_800x600_60, O_HSTX | O_PIO, 378, { 37800000, 800, 44, 72, 92,  600, 1, 4, 20, 60 } },
    // HSTX at 252 / 504: 1344 px (50.4 MHz).
    { VM_800x600_60, O_HSTX, 0,  { 50400000, 800, 32, 128, 384, 600, 1, 4, 20, 60 } },
    // PIO at 504: 896 px (33.6 MHz), a reduced blanking of 96, the island's 44
    // pixels just inside its 48 of hsync.
    { VM_800x600_60, O_PIO, 504, { 33600000, 800, 8, 48, 40,   600, 1, 4, 20, 60 } },
};

bool video_mode_timing(int vm, int out, int mhz, video_timing_t *t) {
    for (unsigned i = 0; i < sizeof k_modes / sizeof k_modes[0]; i++) {
        const mode_row_t *m = &k_modes[i];
        if (m->vm != vm || !(m->outs & (1u << out)) || (m->mhz && m->mhz != mhz)) continue;
        if (out == VIDEO_OUT_HSTX) {
            const uint32_t k = (uint32_t)mhz * 1000000u / m->t.pixel_hz;   // clk_sys per pixel
            if (k % 5 || k / 5 < 1 || k / 5 > 4 || (uint32_t)mhz * 1000000u % m->t.pixel_hz) continue;
        } else if (out == VIDEO_OUT_PIO) {
            const uint32_t k2 = (uint32_t)mhz * 2000000u / m->t.pixel_hz;   // 2 x clk_sys per pixel
            if ((uint32_t)mhz * 2000000u % m->t.pixel_hz || k2 % 10 || k2 < 20) continue;
        }
        *t = m->t;
        return true;
    }
    return false;
}

// The standard screen (DIWSTRT $2C81, 320 x 256) is at column 20 (32 with the
// left overscan position), row 18 (amiga.h).
void video_mode_window(const video_timing_t *t, int hpos, video_window_t *w) {
    const int fw = AMIGA_OUT_W, fh = AMIGA_FB_H * 2;
    if (t->h_active >= fw) {
        w->x = (t->h_active - fw) / 2; w->col = 0; w->cols = AMIGA_FB_W;
    } else {
        w->x = 0; w->cols = t->h_active / 2;
        w->col = (hpos ? 32 : 20) + 160 - w->cols / 2;
        if (w->col + w->cols > AMIGA_FB_W) w->col = AMIGA_FB_W - w->cols;
        if (w->col < 0) w->col = 0;
    }
    if (t->v_active >= fh) {
        w->y = (t->v_active - fh) / 2; w->row = 0; w->rows = AMIGA_FB_H;
    } else {
        w->y = 0; w->rows = t->v_active / 2;
        w->row = 18 + 128 - w->rows / 2;
    }
}
