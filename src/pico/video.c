// Picks the display output, as pico-speccy's graphics_init() does: the first two
// display pins are probed with board_test_pins(); a VGA resistor ladder ties them
// together through the monitor-side load the way an HDMI connector does not.
#include "video.h"
#include "board.h"

#ifndef VGA_BASE_PIN
#define VGA_BASE_PIN 6
#endif

static bool vga;
static int  vmode = VM_720x576_50, vhpos;
video_timing_t video_cur_timing;
video_window_t video_cur_window;

void video_select(void) {
    const int code = board_test_pins(VGA_BASE_PIN, VGA_BASE_PIN + 1);
#if defined(ZERO2) || defined(PICO_DV)
    vga = code == 0x1F;
#else
    vga = code == 0 || code == 0x1F;
#endif
}

bool video_is_vga(void) { return vga; }

int video_out(void) {
#ifdef VIDEO_PIO
    return vga ? VIDEO_OUT_VGA : VIDEO_OUT_PIO;
#else
    return vga ? VIDEO_OUT_VGA : VIDEO_OUT_HSTX;
#endif
}

int video_set_mode(int vm, int mhz) {
    if (!video_mode_timing(vm, video_out(), mhz, &video_cur_timing)) {
        vm = VM_720x576_50;
        video_mode_timing(vm, video_out(), mhz, &video_cur_timing);
    }
    vmode = vm;
    video_mode_window(&video_cur_timing, vhpos, &video_cur_window);
    return vm;
}

int video_mode(void) { return vmode; }

void video_set_hpos(int hpos) {
    vhpos = hpos;
    video_window_t w;
    video_mode_window(&video_cur_timing, hpos, &w);
    video_cur_window.col = w.col;       // the only field a position moves
}

void video_start(const amiga_row_t *framebuffer) {
    if (vga) vga_start(framebuffer);
    else     hdmi_start(framebuffer);
}

uint32_t video_frames(void) { return vga ? vga_frames() : hdmi_frames(); }
uint32_t video_lines(void) { return vga ? vga_lines() : hdmi_lines(); }
uint32_t video_frame_lines(void) { return vga ? vga_frame_lines() : hdmi_frame_lines(); }
uint32_t video_late_lines(void) { return vga ? 0 : hdmi_late_lines(); }
uint32_t video_stalled_lines(void) { return vga ? 0 : hdmi_stalled_lines(); }
void video_core1_cycles(uint32_t *irq, uint32_t *enc) {
    if (vga) *irq = *enc = 0;
    else     hdmi_core1_cycles(irq, enc);
}
