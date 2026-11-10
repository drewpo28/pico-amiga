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
    static uint16_t cpu_mhz;        // 252 / 378 / 504 / 540 (540: vreg_mv forced to 1800)
    static uint16_t vreg_mv;        // Options > Overclock > Core voltage, mV (storage.nvs
                                    //   vreq_voltage "1_60", pico-speccy's key and format)
    static uint16_t max_flash_freq; // flash SCK limit, MHz (default 66)
    static uint16_t max_psram_freq; // PSRAM SCK limit, MHz (default 166; 135 is the cap)
    static uint8_t  audio_out;      // 0 auto (+ HDMI), 1 PWM, 2 I2S, 3 HDMI only (audio_out.c)
    static bool     dbg_uart;       // Debug > UART log (dbg_uart.c)
    static bool     pcprof;         // Debug > PC profiler (board_pcprof; never saved)
    static uint8_t  load_test;      // Debug > A1200 load test: 0 off, 1 CPU x2, 2 + 8 planes, 3 CPU x3, 4 + 8 planes (never saved)
    static bool     render_c1;      // Debug > Render on core 1 (amiga_render_async)
    // Sound
    static uint8_t  volume;         // 0..16 (16 = full scale)
    static bool     hdmi_audio;     // retired: HDMI audio is audio_out 0 (with the DAC) or 3
    static bool     hdmi_capture;   // Video > Output > Capture card mode (PIO HDMI, tmds_pio.h)

    // Video
    static uint8_t  hdmi_vmode;     // Video > Output > Mode on HDMI / on VGA (video_modes.h VM_*, pico-speccy's
    static uint8_t  vga_vmode;      //   numbers); read at boot, a change reboots
    static uint8_t  hpos;           // picture position: 0 centred, 1 left overscan
    static bool     vcentre;        // centre short pictures vertically (amiga_set_vcentre)
    static bool     disk_led;       // Video > Picture > Drive icon (UiStats.cpp)
    static bool     frame_skip;     // Video > Picture > Skip frames when late (main.c, amiga_set_render_skip)

    // Machine (machine.cpp)
    static std::string hdf;         // the IDE master's hard disk image (card path), "" = none
    static std::string hdf_raw;     // or an SD window: "mode:part:lba:len" (mode 1 read-only,
                                    // 2 writable; part 1-8, 0 typed; card sectors), "" = none
    static std::string hdf2;        // the IDE slave's image and SD window, as hdf / hdf_raw
    static std::string hdf2_raw;
    static uint8_t  joydef[14];     // the pad's map: joymap.h JOY_A_* per JOY_S_* control
    static uint16_t joykey[12];     // the keyboard's: a fabgl virtual key per JOY_A_* action (0 none)
    static std::string cd;          // the CD32's disc: an .iso or .cue (card path), "" = none
    static uint8_t  a600_chip;      // the A600's chip RAM in MB: 1, or 2 (an A601; 8 MB PSRAM)
    static uint8_t  fast_ram;       // Machine > A600 / A1200 > Fast RAM, MB: 0, 1, 2, 4 (a Zorro II board; PSRAM permitting)
    static uint8_t  chip_bus;       // Machine > A1200 > Chip bus: 0 off, 1 stock A1200, 2 with cache credit (amiga_set_chip_bus)
    static uint8_t  machine;        // 0 A500 (OCS, 512 KB chip + 512 KB slow), 1 A500+ (ECS, 1 MB chip), 2 A600 (A500+ + Gayle), 3 A1200, 4 CD32
    static std::string kick;        // built-in ROM id (roms.h), or "sd" = kick_file from the card
    static std::string kick_file;   // Machine > Custom ROM: the Kickstart image on the card
    static std::string df0;         // ADF in DF0 ("" = empty drive)
    static bool     df0_wp;         // DF0 write-protected
    static uint8_t  drives;         // floppy drives: 0 Auto (one per disk of a set), 1..4
    static uint8_t  df0_disk;       // df0 is an .m3u disk set: the disk in the drive (0-based)
    // Snapshots (snapshot.cpp)
    static uint8_t  persist_slot;   // the quick slot Alt+Win+F3 / F4 load and save (1..40)

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
