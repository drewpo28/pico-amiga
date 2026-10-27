#pragma once
#include <stdbool.h>
#include <stdint.h>

// Video output of an AMIGA_FB_W x AMIGA_FB_H 0x0RGB framebuffer, doubled to 720x576:
// HDMI at 50.06 Hz (video_hstx.c, or video_pio.c on boards without HSTX pins), or
// VGA at 49.97 Hz (video_vga.c) when a VGA resistor ladder is on the same pins.

// Probe the display pins (VGA ladder or HDMI connector, as pico-speccy decides it).
// Call once at boot, before video_start(); takes ~130 ms.
void video_select(void);
bool video_is_vga(void);

// Start scanout. Call on the core that will own the DMA IRQ (core 1).  HDMI also
// carries the core's audio ring (interleaved stereo int16, `audio_frames` a power
// of two) in its data islands (hdmi_audio.c).
void video_start(const uint16_t *framebuffer, const int16_t *audio_ring, uint32_t audio_frames);

// Number of output frames completed.
uint32_t video_frames(void);

// The back-ends.
void hdmi_start(const uint16_t *framebuffer, const int16_t *audio_ring, uint32_t audio_frames);
uint32_t hdmi_frames(void);
void vga_start(const uint16_t *framebuffer);
uint32_t vga_frames(void);
