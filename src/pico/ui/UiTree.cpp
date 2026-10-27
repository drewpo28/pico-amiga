// pico-amiga — the menu tree (pico-speccy's UiTree.cpp is the model for the shape;
// the content is the Amiga's).
//
// Static const Node arrays, see UiModel.h. Rows bind to settings through the
// SettingId X-table in UiStage.h; actions and info pages are the small functions
// below.

#include "OSDNewMenu.h"

#include <stdio.h>
#include <string.h>
#include "ff.h"

#include "UiModel.h"
#include "UiStage.h"
#include "UiDialog.h"
#include "UiPlatform.h"
#include "UiNav.h"
#include "UiBrowser.h"
#include "UiGfx.h"
#include "app/Config.h"
#include "roms.h"
#include "machine.h"
#include <string>

namespace nm {

// ── option tables ──────────────────────────────────────────────────────────────

static const Option opt_mouse_swap[] = {
    { "Off", 0 }, { "On", 1 }, { "Auto (first single press)", 2, "Auto" },
};
static const Option opt_mouse_sens[] = {
    { "1/4", 64 }, { "1/2", 128 }, { "1:1", 256 }, { "x2", 512 },
};
static const Option opt_audio_out[] = {
    { "Auto (PCM5122 / I2S DAC / PWM)", 0, "Auto" }, { "PWM", 1 }, { "I2S", 2 },
};
static const Option opt_cpu_mhz[] = {
    { "252 MHz", 252 }, { "378 MHz", 378 }, { "504 MHz", 504 },
};
// Same steps as pico-speccy's Overclock menu. The QMI divides clk_sys by a whole
// number, so the SCK is the fastest clk_sys / n at or below the limit (504 MHz:
// 66 -> 63, 84 -> 84, 100 -> 100.8, 133/166 -> 126); PSRAM never goes past 135.
static const Option opt_flash_freq[] = {
    { "33 MHz", 33 }, { "66 MHz", 66 }, { "84 MHz", 84 },
    { "100 MHz", 100 }, { "133 MHz", 133 }, { "166 MHz", 166 },
};
static const Option opt_psram_freq[] = {
    { "66 MHz", 66 }, { "84 MHz", 84 }, { "100 MHz", 100 },
    { "133 MHz", 133 }, { "166 MHz", 166 },
};
static const Option opt_drives[] = {
    { "Auto (one per disk of an .m3u set)", 0, "Auto" },
    { "1 (DF0)", 1, "1" }, { "2 (DF0, DF1)", 2, "2" }, { "3", 3 }, { "4", 4 },
};
static const Option opt_hpos[] = {
    { "Centred", 0 }, { "Left overscan (+12 columns)", 1, "Left overscan" },
};
static const Option opt_theme[] = {
    { "Slate", 0 }, { "ZX Spectrum", 1 },
};
static const Option opt_corners[] = {
    { "Square", 0 }, { "Rounded", 1 },
};
static const Option opt_vga_solid[] = {
    { "Dithered (full colour)", 0, "Dithered" }, { "Solid (VGA grid)", 1, "Solid" },
};

// Machine > Kickstart: the built-in ROMs, then the card's kick.rom. Built once per
// boot (the table is fixed in the image). Values are what UiStage's get_kick reads.
static const Option* kickOpts(uint8_t& cnt) {
    static Option opts[16];
    static uint8_t n;
    if (!n) {
        for (unsigned i = 0; i < builtin_rom_count && n < 15; i++)
            opts[n++] = { builtin_roms[i].label, (int32_t)i, nullptr };
        opts[n++] = { "SD card: amiga/kick.rom", 100, "SD card" };
    }
    cnt = n;
    return opts;
}

// ── actions and pages ──────────────────────────────────────────────────────────

static const char* baseName(const char* p) {
    const char* s = strrchr(p, '/');
    return s ? s + 1 : p;
}

// Disks > DF0: the file name of what is in the drive.
static const char* df0Label() {
    const char* p = machine_disk(0);
    return *p ? baseName(p) : "empty";
}

// Disks > DF1..DF3: what the other connected drives hold (a set's disks 2..).
static const char* dfLabel(int d) { const char* p = machine_disk(d); return *p ? baseName(p) : "empty"; }
static const char* df1Label() { return dfLabel(1); }
static const char* df2Label() { return dfLabel(2); }
static const char* df3Label() { return dfLabel(3); }
static bool p_df1() { return machine_drives() > 1; }
static bool p_df2() { return machine_drives() > 2; }
static bool p_df3() { return machine_drives() > 3; }
static void act_none() {}

// Disks > Swap disk: "2 of 3" while an .m3u disk set is loaded.
static bool p_diskSet() { return machine_set_count() > 1; }
static const char* setLabel() {
    static char b[16];
    snprintf(b, sizeof b, "%d of %d", machine_set_index() + 1, machine_set_count());
    return b;
}

// The set's disks, then "Other disk..." (the file browser) on the last row.
static void setRow(int i, char* out, size_t n) {
    if (i < machine_set_count())
        snprintf(out, n, "%c %d  %s", i == machine_set_index() ? '>' : ' ', i + 1,
                 baseName(machine_set_disk(i)));
    else
        snprintf(out, n, "  Other disk...");
}

static bool df0PickAndInsert();

// Pick a disk of the set for DF0 (RetroArch's Disk Control); true when the
// drive changed.
static bool df0SwapPick() {
    const int i = uiPickListCb("Swap DF0", machine_set_count() + 1, setRow,
                               machine_set_index(), 40);
    if (i < 0) return false;
    if (i == machine_set_count()) return df0PickAndInsert();
    if (i == machine_set_index()) return false;
    if (!machine_set_select(i)) {
        uiToast("That disk cannot be opened", true, 2000);
        return false;
    }
    Config::df0_disk = (uint8_t)i;
    Config::save();
    return true;
}

// Browse for an ADF and put it in DF0; true when a disk went in. Shared by the
// menu row and the Ctrl+F12 hotkey (runDiskInsert).
static bool df0PickAndInsert() {
    // Start where the last disk came from, else in /amiga/.
    static std::string dir;
    if (dir.empty()) {
        const std::string cur = machine_disk(0);
        dir = cur.empty() ? std::string("/amiga/") : cur.substr(0, cur.find_last_of('/') + 1);
        FILINFO fi;
        if (dir != "/" && f_stat(dir.substr(0, dir.size() - 1).c_str(), &fi) != FR_OK) dir = "/";
    }
    const std::string r = browseFile(dir, "Insert into DF0", BROWSE_ADF);
    if (r.size() < 2 || r[0] != 'R') return false;
    const std::string path = dir + r.substr(1);
    const size_t dot = path.find_last_of('.');
    std::string ext = dot == std::string::npos ? "" : path.substr(dot);
    for (char& c : ext) c = (char)(c | 0x20);
    // An .m3u is a disk set (RetroArch's playlist): disk 1 goes in, the rest
    // wait for Disks > Swap disk / Ctrl+F12.
    const bool set = ext == ".m3u";
    if (set ? !machine_load_set(path.c_str(), 0) : !machine_insert(0, path.c_str(), Config::df0_wp)) {
        gfxBegin();
        uiToast(set ? "The disk set's first disk cannot be opened"
                    : "Not an 880 KB ADF, or it cannot be opened", true, 2000);
        gfxEnd();
        return false;
    }
    Config::df0 = path;
    Config::df0_disk = 0;
    // A set in Auto spreads over DF0..DF3; more drives need a reset to be seen.
    if (machine_apply_drives()) uiAmigaReset();
    Config::save();
    return true;
}

static void act_df0_insert() {
    if (df0PickAndInsert()) requestClose();   // back to the Amiga with the disk in
}

static void act_df0_swap() {
    if (df0SwapPick()) requestClose();
}

// Ctrl+F12: with a disk set loaded, its disks (one key press to swap, as
// RetroArch's Disk Control); otherwise the file browser.
void runDiskInsert(int drive) {
    if (drive != 0) return;
    if (!p_diskSet()) { df0PickAndInsert(); return; }
    gfxBegin();
    df0SwapPick();
    gfxEnd();
}

static void act_df0_eject() {
    machine_eject(0);
    machine_apply_drives();
    Config::df0.clear();
    Config::save();
}


static void act_amiga_reset() {
    if (uiConfirm("Reset the Amiga?", "Reset", true)) {
        uiAmigaReset();
        requestClose();
    }
}

static void act_reboot() {
    if (uiConfirm("Reboot the board?", "Reboot", false)) OSD::esp_hard_reset();
}

static void page_hotkeys() {
    uiTextPage("Hotkeys",
        "F12         This menu (Esc goes back, F12 closes)\n"
        "Ctrl+F12    Insert a disk into DF0 (an .m3u set: swap disks)\n"
        "Shift+F12   Reset the Amiga\n"
        "Alt+F12     Performance band: summary, breakdown, off\n"
        "Ctrl+J      Cursor keys as joystick on/off\n"
        "F1..F10     Amiga function keys\n"
        "\n"
        "Joystick (game port 2) while cursor keys are a joystick:\n"
        "  Cursor keys   directions\n"
        "  Left Ctrl     fire\n"
        "\n"
        "Amiga keys on a PC keyboard:\n"
        "  Win / PgDn / PgUp   left / left / right Amiga\n"
        "  F11, Insert         Help\n"
        "  Home, End           keypad ( )\n"
        "\n"
        "Mouse: port 1. A USB/XInput/NES pad drives port 2.");
}

static void page_about() {
    uiTextPage("About",
        "Pico-Amiga " PORT_VERSION "\n"
        "Amiga 500 (OCS, PAL) for RP2350 + PSRAM\n"
        "\n"
        "CPU: Musashi 68000\n"
        "Drivers, menu and host build after pico-speccy");
}

// ── the tree ───────────────────────────────────────────────────────────────────

static const Node kMachine[] = {
    NM_RADIO_D("Kickstart", SET_KICK, kickOpts, nullptr),
    NM_RADIO  ("Floppy drives", SET_DRIVES, opt_drives, nullptr),
    NM_ACTION ("Reset the Amiga", act_amiga_reset, nullptr),
};

static const Node kDisks[] = {
    NM_ACTIONV("DF0", act_df0_insert, df0Label, nullptr),
    NM_ACTIONV("Swap disk", act_df0_swap, setLabel, p_diskSet),
    NM_ACTIONV("DF1", act_none, df1Label, p_df1),
    NM_ACTIONV("DF2", act_none, df2Label, p_df2),
    NM_ACTIONV("DF3", act_none, df3Label, p_df3),
    NM_ACTION ("Eject DF0", act_df0_eject, nullptr),
    NM_BOOL   ("DF0 write protect", SET_DF0_WP, nullptr),
};

static const Node kInterface[] = {
    NM_RADIO("Theme",        SET_UI_THEME,     opt_theme,     nullptr),
    NM_RADIO("Corners",      SET_UI_ROUNDED,   opt_corners,   nullptr),
    NM_RADIO("VGA colours",  SET_UI_VGA_SOLID, opt_vga_solid, nullptr),
};

static const Node kAudio[] = {
    NM_INT  ("Volume", SET_VOLUME, 0, 16, 1, nullptr),
    NM_RADIO("Output", SET_AUDIO_OUT, opt_audio_out, nullptr),
};

static const Node kVideo[] = {
    NM_RADIO("Picture position", SET_HPOS, opt_hpos, nullptr),
    NM_BOOL("HDMI audio", SET_HDMI_AUDIO, nullptr),
};

static const Node kOverclock[] = {
    NM_RADIO("CPU frequency",   SET_CPU_MHZ,    opt_cpu_mhz,    nullptr),
    NM_RADIO("Flash frequency", SET_FLASH_FREQ, opt_flash_freq, nullptr),
    NM_RADIO("PSRAM frequency", SET_PSRAM_FREQ, opt_psram_freq, nullptr),
};

static const Node kOptions[] = {
    NM_SUB("Overclock", kOverclock, nullptr),
    NM_SUB("Interface", kInterface, nullptr),
};

static const Node kInput[] = {
    NM_BOOL ("Cursor keys as joystick", SET_CURSOR_JOY, nullptr),
    NM_RADIO("Mouse speed",             SET_MOUSE_SENS, opt_mouse_sens, nullptr),
    NM_RADIO("Swap mouse buttons",      SET_MOUSE_SWAP, opt_mouse_swap, nullptr),
};


// The row exists only where the board has a log pin (cmake/boards.cmake).
static bool p_dbgUartPin() {
#ifdef DBG_UART_TX_PIN
    return true;
#else
    return false;
#endif
}

static const Node kDebug[] = {
    // TX-only 115200 log on the board's DBG_UART_TX_PIN: boot messages and a
    // PERF line a second. Reboot-class (the keyboard may move off the pin).
    NM_BOOL("UART log", SET_DBG_UART, p_dbgUartPin),
    // Denise's rendering shared with core 1 (off: all on core 0, as before).
    NM_BOOL("Render on core 1", SET_RENDER_C1, nullptr),
};

static const Node kHelp[] = {
    NM_PAGE("Hotkeys", page_hotkeys, nullptr),
    NM_PAGE("About",   page_about,   nullptr),
};

static const Node kReset[] = {
    NM_ACTION("Reset the Amiga",  act_amiga_reset, nullptr),
    NM_ACTION("Reboot the board", act_reboot,      nullptr),
};

static const Node kRoot[] = {
    NM_SUB("Disks",     kDisks,     nullptr),
    NM_SUB("Machine",   kMachine,   nullptr),
    NM_SUB("Input",     kInput,     nullptr),
    NM_SUB("Audio",     kAudio,     nullptr),
    NM_SUB("Video",     kVideo,     nullptr),
    NM_SUB("Options",   kOptions,   nullptr),
    NM_SUB("Debug",     kDebug,     nullptr),
    NM_SUB("Help",      kHelp,      nullptr),
    NM_SUB("Reset",     kReset,     nullptr),
};

const Node* rootNodes()     { return kRoot; }
uint8_t     rootNodeCount() { return NM_COUNT(kRoot); }

}  // namespace nm
