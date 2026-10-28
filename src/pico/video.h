#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "amiga.h"
#include "video_modes.h"

// Video output of the AMIGA_FB_H framebuffer rows (amiga.h), at 720x576 or in
// another Video > Mode (video_modes.h): HDMI (video_hstx.c, or video_pio.c on
// boards without HSTX pins), or VGA (video_vga.c) when a VGA resistor ladder is
// on the same pins.  720x576 is 50.06 Hz on HDMI, 49.97 Hz on VGA.

// Probe the display pins (VGA ladder or HDMI connector, as pico-speccy decides it).
// Call once at boot, before video_start(); takes ~130 ms.
void video_select(void);
bool video_is_vga(void);
int  video_out(void);               // VIDEO_OUT_* of this board and display

// The mode to start in (after video_select()): `vm` if this scanout runs it at
// clk_sys `mhz`, else 720x576.  Returns the mode taken.
int  video_set_mode(int vm, int mhz);
int  video_mode(void);
// Video > Picture position (0 / 1): where a window smaller than the picture
// sits in it (640x480).  Takes hold from the next output line.
void video_set_hpos(int hpos);
// The running mode's timing and window.  Read by the scanout IRQs: plain data.
extern video_timing_t video_cur_timing;
extern video_window_t video_cur_window;
static inline const video_timing_t *video_timing(void) { return &video_cur_timing; }
static inline const video_window_t *video_window(void) { return &video_cur_window; }

// Start scanout. Call on the core that will own the DMA IRQ (core 1).  HDMI also
// carries the sound in its data islands, fed by audio_out.c's tick (hdmi_audio.c).
void video_start(const amiga_row_t *framebuffer);

// Number of output frames completed.
uint32_t video_frames(void);
// Output lines begun, counted so that a multiple of video_frame_lines() is the
// first line of a picture (row 0 of the framebuffer); main.c paces the frames
// to a line of it.
uint32_t video_lines(void);
uint32_t video_frame_lines(void);
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
uint32_t hdmi_lines(void);
uint32_t hdmi_frame_lines(void);
uint32_t hdmi_late_lines(void);
uint32_t hdmi_stalled_lines(void);
void hdmi_core1_cycles(uint32_t *irq, uint32_t *enc);
void vga_start(const amiga_row_t *framebuffer);
uint32_t vga_frames(void);
uint32_t vga_lines(void);
uint32_t vga_frame_lines(void);
