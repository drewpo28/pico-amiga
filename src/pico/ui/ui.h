// The menu and the settings, as the C side of the firmware (main.c) sees them.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Read the settings (Config::load) from the mounted card; without a card the
// compiled-in defaults stay.
void ui_load_settings(void);
// Give the menu the framebuffer it draws into.
void ui_init(uint16_t *framebuffer);

// The settings main.c applies while booting (Config, from C).
int  settings_cpu_mhz(void);
int  settings_audio_out(void);          // 0 auto, 1 PWM, 2 I2S
// Push the live sound settings (volume, HDMI audio) to the outputs.
void settings_apply_sound(void);

#ifdef __cplusplus
}
#endif
