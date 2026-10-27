#pragma once
#include <stdint.h>

// Start PWM playback of the emulator's audio ring (interleaved stereo int16,
// `frames` stereo frames, a power of two).  Pins < 0 are not driven.
void audio_pwm_start(int left_pin, int right_pin, const int16_t *ring, uint32_t frames);
