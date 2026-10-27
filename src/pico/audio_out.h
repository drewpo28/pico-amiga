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
// found by board_test_pins, else PWM), 1 PWM, 2 I2S.
void audio_out_start(const int16_t *ring, uint32_t frames, int mode);
// Output volume, Q8: 256 = full scale, 0 = silent.  Applies to every output,
// HDMI audio included (hdmi_audio.c reads it).
void audio_set_volume(int q8);
extern volatile int16_t audio_volume_q8;
const char *audio_out_name(void);   // "PWM", "I2S", "PCM5122"

#ifdef __cplusplus
}
#endif
