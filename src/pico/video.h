#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "amiga.h"

// Video output of the AMIGA_FB_H framebuffer rows (amiga.h) at 720x576: HDMI at
// 50.06 Hz (video_hstx.c, or video_pio.c on boards without HSTX pins), or VGA at
// 49.97 Hz (video_vga.c) when a VGA resistor ladder is on the same pins.

// Probe the display pins (VGA ladder or HDMI connector, as pico-speccy decides it).
// Call once at boot, before video_start(); takes ~130 ms.
void video_select(void);
bool video_is_vga(void);

// Start scanout. Call on the core that will own the DMA IRQ (core 1).  HDMI also
// carries the sound in its data islands, fed by audio_out.c's tick (hdmi_audio.c).
void video_start(const amiga_row_t *framebuffer);

// Number of output frames completed.
uint32_t video_frames(void);
// Output lines sent before core 1 had encoded them (PIO HDMI; 0 elsewhere).
uint32_t video_late_lines(void);
// Output lines in which the serializer ran dry (PIO HDMI; 0 elsewhere).
uint32_t video_stalled_lines(void);
// Core 1's cycles so far in the scanout's line IRQ and line building (PIO HDMI;
// 0 elsewhere): its share of core 1 next to the render (amiga_render_pump).
void video_core1_cycles(uint32_t *irq, uint32_t *enc);

// The back-ends.
void hdmi_start(const amiga_row_t *framebuffer);
uint32_t hdmi_frames(void);
uint32_t hdmi_late_lines(void);
uint32_t hdmi_stalled_lines(void);
void hdmi_core1_cycles(uint32_t *irq, uint32_t *enc);
void vga_start(const amiga_row_t *framebuffer);
uint32_t vga_frames(void);
