// Video > Output > Mode: the output modes, after pico-speccy's Config::VM_* and
// video_mode_table.h.  Platform-free (the PC host builds it too).
//
// The picture is the AMIGA_FB_W x AMIGA_FB_H framebuffer at 720 x 576 (every
// row two output lines, every lowres column two pixels).  A mode that is larger
// shows it whole, centred in black; a smaller one shows a window of it, the
// standard 320 x 256 screen's middle (640 x 480: 320 x 240 framebuffer pixels).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// pico-speccy's numbers (storage.nvs hdmi_vmode / vga_vmode): never renumber.
// Ours start after its last (16).
enum {
    VM_640x480_60 = 0,
    VM_720x480_60 = 2,
    VM_720x576_50 = 3,
    VM_800x600_X2 = 11,
    VM_800x600_60 = 17,     // HDMI only: on VGA, VM_800x600_X2 is 60 Hz already
};

// The scanout that would run a mode.
enum { VIDEO_OUT_HSTX = 0, VIDEO_OUT_PIO = 1, VIDEO_OUT_VGA = 2 };

typedef struct {
    uint32_t pixel_hz;
    uint16_t h_active, h_fp, h_sync, h_bp;   // output pixels
    uint16_t v_active, v_fp, v_sync, v_bp;   // lines (the active ones last in our frame)
    uint8_t  hz;                             // nominal refresh
} video_timing_t;

// Where the framebuffer goes: output pixel x / line y of the picture's top left,
// the first framebuffer column / row shown and how many.
typedef struct {
    int x, y;
    int col, row;
    int cols, rows;
} video_window_t;

// The timing of mode `vm` on scanout `out` at a clk_sys of `mhz`; false when that
// scanout cannot run it there.
bool video_mode_timing(int vm, int out, int mhz, video_timing_t *t);
// The window of a mode (hpos: Video > Picture > Position, which moves the standard
// screen 12 columns right in the framebuffer).
void video_mode_window(const video_timing_t *t, int hpos, video_window_t *w);

#ifdef __cplusplus
}
#endif
