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
