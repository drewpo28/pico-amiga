// Picks the display output, as pico-speccy's graphics_init() does: the first two
// display pins are probed with board_test_pins(); a VGA resistor ladder ties them
// together through the monitor-side load the way an HDMI connector does not.
#include "video.h"
#include "board.h"

#ifndef VGA_BASE_PIN
#define VGA_BASE_PIN 6
#endif

static bool vga;

void video_select(void) {
    const int code = board_test_pins(VGA_BASE_PIN, VGA_BASE_PIN + 1);
#if defined(ZERO2) || defined(PICO_DV)
    vga = code == 0x1F;
#else
    vga = code == 0 || code == 0x1F;
#endif
}

bool video_is_vga(void) { return vga; }

void video_start(const amiga_row_t *framebuffer) {
    if (vga) vga_start(framebuffer);
    else     hdmi_start(framebuffer);
}

uint32_t video_frames(void) { return vga ? vga_frames() : hdmi_frames(); }
uint32_t video_late_lines(void) { return vga ? 0 : hdmi_late_lines(); }
void video_core1_cycles(uint32_t *irq, uint32_t *enc) {
    if (vga) *irq = *enc = 0;
    else     hdmi_core1_cycles(irq, enc);
}
