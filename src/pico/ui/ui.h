// The menu and the settings, as the C side of the firmware (main.c) sees them.
#pragma once
#include <stdint.h>
#include "amiga.h"

#ifdef __cplusplus
extern "C" {
#endif

// Read the settings (Config::load) from the mounted card; without a card the
// compiled-in defaults stay.
void ui_load_settings(void);
// Give the menu the framebuffer it draws into.
void ui_init(amiga_row_t *framebuffer);

// The settings main.c applies while booting (Config, from C).
int  settings_cpu_mhz(void);
int  settings_flash_mhz(void);          // flash / PSRAM SCK limits (Options)
int  settings_psram_mhz(void);
int  settings_audio_out(void);
bool settings_dbg_uart(void);          // 0 auto, 1 PWM, 2 I2S
// Push the live sound settings (volume, HDMI audio) to the outputs.
void settings_apply_sound(void);

// The performance band (UiStats.cpp, Alt+F12): toggle it; after every emulated
// frame, the time amiga_run_frame() took (draws the band when it is on).
void ui_stats_toggle(void);
void ui_stats_frame(uint32_t emu_us);
void ui_stats_row(void *ctx, int row);   // amiga_config_t.row_done

#ifdef __cplusplus
}
#endif
