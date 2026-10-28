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
    static uint8_t  audio_out;      // 0 auto (+ HDMI), 1 PWM, 2 I2S, 3 HDMI only (audio_out.c)
    static bool     dbg_uart;       // Debug > UART log (dbg_uart.c)
    static bool     render_c1;      // Debug > Render on core 1 (amiga_render_async)
    // Sound
    static uint8_t  volume;         // 0..16 (16 = full scale)
    static bool     hdmi_audio;     // retired: HDMI audio is audio_out 0 (with the DAC) or 3
    static bool     hdmi_capture;   // Video > Capture card compatibility (PIO HDMI, tmds_pio.h)

    // Video
    static uint8_t  hdmi_vmode;     // Video > Mode on HDMI / on VGA (video_modes.h VM_*, pico-speccy's
    static uint8_t  vga_vmode;      //   numbers); read at boot, a change reboots
    static uint8_t  hpos;           // picture position: 0 centred, 1 left overscan
    static bool     vcentre;        // centre short pictures vertically (amiga_set_vcentre)
    static bool     disk_led;       // Video > Disk activity icon (UiStats.cpp)

    // Machine (machine.cpp)
    static uint8_t  machine;        // 0 A500 (OCS, 512 KB chip + 512 KB slow), 1 A500+ (ECS, 1 MB chip)
    static std::string kick;        // built-in ROM id (roms.h), or "sd" = kick_file from the card
    static std::string kick_file;   // Machine > Custom ROM: the Kickstart image on the card
    static std::string df0;         // ADF in DF0 ("" = empty drive)
    static bool     df0_wp;         // DF0 write-protected
    static uint8_t  drives;         // floppy drives: 0 Auto (one per disk of a set), 1..4
    static uint8_t  df0_disk;       // df0 is an .m3u disk set: the disk in the drive (0-based)

    static void load();
    static bool save();
    // A new video mode is kept only once the user has seen it: before the reboot
    // into it the modes it replaces go to vmode_pending.nvs, and the boot that finds
    // the file asks (pico-speccy's savePendingVideoMode).
    static void savePendingVideoMode();
    static bool loadPendingVideoMode(uint8_t& hdmi_vm, uint8_t& vga_vm);
    static void clearPendingVideoMode();
    static bool loaded;             // storage.nvs was read (or written) this session
};

#ifndef CONFIG_BOARD_TAG
#define CONFIG_BOARD_TAG "board"
#endif
#define CONFIG_DIR       "/.config/pico-amiga"
#define CONFIG_DIR_BOARD CONFIG_DIR "/" CONFIG_BOARD_TAG
#define STORAGE_NVS      CONFIG_DIR_BOARD "/storage.nvs"
