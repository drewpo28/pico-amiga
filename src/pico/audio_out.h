// The core's audio ring to PWM, I2S or a PCM5122 board (audio_out.c).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ZERO2: is the PCM5122 board fitted?  Probed once over I2C and cached; call it
// before the PS/2 keyboard starts, which shares the pins (input_init).
bool audio_out_pcm5122_present(void);
// Pick the output and start playing `ring` (interleaved stereo int16, `frames` a
// power of two) at AMIGA_AUDIO_RATE.  `mode`: 0 auto (PCM5122, else an I2S DAC
// found by board_test_pins, else PWM), 1 PWM, 2 I2S, 3 none (HDMI only: the
// sample tick still feeds hdmi_audio.c, whose enable is the caller's).
void audio_out_start(const int16_t *ring, uint32_t frames, int mode);
// Output volume, Q8: 256 = full scale, 0 = silent.  Applies to every output,
// HDMI audio included (hdmi_audio.c reads it).
void audio_set_volume(int q8);
extern volatile int16_t audio_volume_q8;
const char *audio_out_name(void);
// clk_sys cycles spent in the sample tick's interrupt so far (the profiler's
// clock, board_cycles; wraps).
uint32_t audio_out_isr_cycles(void);
void     audio_out_counts(uint32_t *hold, uint32_t *skip);   // running totals   // "PWM", "I2S", "PCM5122", "HDMI"

#ifdef __cplusplus
}
#endif
