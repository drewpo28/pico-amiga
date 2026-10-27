// HDMI audio for both HDMI back-ends: the data islands (hdmi_audio.c).
//
// Every output line carries one data island in its horizontal sync pulse
// (HDMI_ISLAND_PX characters: data-island preamble, guard band, a 32-character
// packet, guard band), and every active line a video preamble + guard band
// (HDMI_VLEAD_PX) right before its pixels; together these make the link HDMI
// rather than DVI.  Characters are three 10-bit TMDS symbols packed as
// lane0 | lane1 << 10 | lane2 << 20, which is the HSTX raw word; the PIO
// back-end spreads them over its pin words.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HDMI_ISLAND_PX 44
#define HDMI_VLEAD_PX  10

// `pixel_hz`, `h_total`, `v_total`: the mode, for the ACR packet and the sample
// credit per line.
void hdmi_audio_init(uint32_t pixel_hz, uint32_t h_total, uint32_t v_total);

// One output sample at AMIGA_AUDIO_RATE, volume applied: audio_out.c's tick
// (core 0) calls it for every sample it plays. A no-op until hdmi_audio_init.
void hdmi_audio_push(int16_t l, int16_t r);

// The island of the next output line.  `blank` is the line's 1-based index in
// the vertical blanking (0 on an active line): the ACR and the InfoFrames go out
// on fixed blanking lines, audio packets wherever the sample credit allows.
// `vsync`: the line is inside the vertical sync pulse (VSYNC level 0); the island
// sits inside the horizontal sync pulse (HSYNC level 0).  Call once per line, in
// order: it consumes audio.
void hdmi_audio_island(uint32_t *chars, uint32_t blank, bool vsync);

// Audio sample packets on/off (the link stays HDMI either way; off sends Null
// packets where the audio went).
void hdmi_audio_set_enabled(bool on);

// The video preamble + guard band (VSYNC inactive, HSYNC inactive).
extern const uint32_t hdmi_video_lead[HDMI_VLEAD_PX];

// The same island in parts, for a back-end with its own symbol format: the 32
// packet characters as TERC4 nibbles per lane ([t][lane], lane 0 carrying the
// syncs; same order and side effects as hdmi_audio_island), the 10 characters
// before them (preamble, guard band) and the 2 after (guard band), and TERC4.
void hdmi_audio_packet(uint8_t nib[32][3], uint32_t blank, bool vsync);
void hdmi_island_frame(uint32_t pre[10], uint32_t post[2], bool vsync);
uint32_t hdmi_terc4(int nibble);

#ifdef __cplusplus
}
#endif
