// pico-amiga settings: static fields, persisted as pico-speccy's storage.nvs
// (plain key=value lines, written to a .tmp and renamed over the file).
//
// The file is /.config/pico-amiga/<board tag>/storage.nvs on the card.  load()
// runs once the card is mounted; a key that is missing or unreadable keeps its
// compiled-in default.  save() refuses to overwrite a file it could not read.
#pragma once
#include <stdint.h>
#include <string>

class Config {
public:
    // Input (input.cpp, pico-speccy's drivers)
    static uint8_t  secondJoy;      // 1: pad 1 does not drive joystick 1
    static uint16_t mouse_sens;     // Q8 multiplier on HID counts (256 = 1:1)
    static uint8_t  mouse_swap;     // 0 off, 1 on, 2 auto (first single press decides)
    static bool     CursorAsJoy;    // cursor keys + Ctrl drive joystick 1

    // Interface (the menu's look)
    static uint8_t  ui_theme;       // 0 Slate, 1 ZX Spectrum
    static bool     ui_rounded;     // rounded window corners
    static bool     ui_vga_solid;   // VGA: the on-grid theme twin (no dither shimmer)

    // Board (read at boot)
    static uint16_t cpu_mhz;        // 252 / 378 / 504
    static uint16_t max_flash_freq; // flash SCK limit, MHz (default 66)
    static uint16_t max_psram_freq; // PSRAM SCK limit, MHz (default 166; 135 is the cap)
    static uint8_t  audio_out;      // 0 auto, 1 PWM, 2 I2S (audio_out.c)
    // Sound
    static uint8_t  volume;         // 0..16 (16 = full scale)
    static bool     hdmi_audio;     // send audio in the HDMI data islands

    // Machine (machine.cpp)
    static std::string kick;        // built-in ROM id (roms.h), or "sd" = /amiga/kick.rom
    static std::string df0;         // ADF in DF0 ("" = empty drive)
    static bool     df0_wp;         // DF0 write-protected

    static void load();
    static bool save();
    static bool loaded;             // storage.nvs was read (or written) this session
};

#ifndef CONFIG_BOARD_TAG
#define CONFIG_BOARD_TAG "board"
#endif
#define CONFIG_DIR       "/.config/pico-amiga"
#define CONFIG_DIR_BOARD CONFIG_DIR "/" CONFIG_BOARD_TAG
#define STORAGE_NVS      CONFIG_DIR_BOARD "/storage.nvs"
