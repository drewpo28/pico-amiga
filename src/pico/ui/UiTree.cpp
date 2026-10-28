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
#include "UiRender.h"
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

// Machine: a row per machine, each a radio over the same SET_KICK listing the
// built-in ROMs filed under it (cmake/roms.cmake's machine field); a row whose
// ROM is not the one running shows no value. Values are what UiStage's get_kick
// reads: the built-in ROM's index, 100 the card's kick.rom. Built once per boot
// (the table is fixed in the image).
static uint8_t romOpts(const char* machine, Option* opts, uint8_t max, const char* slabel) {
    uint8_t n = 0;
    for (unsigned i = 0; i < builtin_rom_count && n < max; i++)
        if (!strcmp(builtin_roms[i].machine, machine)) opts[n++] = { builtin_roms[i].label, (int32_t)i, slabel };
    return n;
}
static const Option* diagOpts(uint8_t& cnt) {
    static Option opts[2];
    static uint8_t n = romOpts("DiagROM", opts, 2, "On");
    cnt = n;
    return opts;
}
static const Option* a500Opts(uint8_t& cnt) {
    static Option opts[8];
    static uint8_t n = romOpts("A500", opts, 8, nullptr);
    cnt = n;
    return opts;
}
static const Option* sdOpts(uint8_t& cnt) {
    static const Option opts[] = { { "amiga/kick.rom", 100, "On" } };
    cnt = NM_COUNT(opts);
    return opts;
}
// A600 and A1200: rows for the machines to come, greyed until their chipsets are.
static bool p_never() { return false; }
static void act_none() {}
static const char* laterLabel() { return "later"; }

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

// Disks > DF0 with an .m3u disk set loaded: the row shows the set, the right
// pane its disks (the one in DF0 marked; a disk in another drive says which),
// Enter swaps the chosen one into DF0.
static bool p_diskSet() { return machine_set_count() > 1; }
static bool p_noSet() { return !p_diskSet(); }
static const char* setDf0Label() { return baseName(Config::df0.c_str()); }

#define SET_ROWS 17                                   // machine.cpp's 16 disks + Other disk...
static const Option* setOpts(uint8_t& cnt) {
    static Option opts[SET_ROWS];
    static char labels[SET_ROWS][48];
    const int n = machine_set_count();
    for (int i = 0; i < n && i < SET_ROWS - 1; i++) {
        const char* disk = machine_set_disk(i);
        int in = -1;                                  // the drive holding it, if any
        for (int d = 0; d < machine_drives(); d++) if (!strcmp(machine_disk(d), disk)) in = d;
        if (in > 0) snprintf(labels[i], sizeof labels[i], "%s  (DF%d)", baseName(disk), in);
        else        snprintf(labels[i], sizeof labels[i], "%s", baseName(disk));
        opts[i] = { labels[i], i, nullptr };
    }
    cnt = (uint8_t)(n < SET_ROWS - 1 ? n : SET_ROWS - 1);
    opts[cnt] = { "Other disk...", cnt, nullptr };
    cnt++;
    return opts;
}

static bool df0PickAndInsert();
static void setRowKey(int32_t tag, uint8_t) {
    if (tag >= machine_set_count()) { if (df0PickAndInsert()) requestClose(); return; }
    if (tag == machine_set_index()) return;
    if (!machine_set_select((int)tag)) { uiToast("That disk cannot be opened", true, 2000); return; }
    Config::df0_disk = (uint8_t)tag;
    Config::save();
    requestClose();                                   // back to the Amiga with the disk in
}
static const Option opt_set_foot[] = {
    { SYM_ENTER " Insert into DF0   " SYM_LEFT " Back", 0 },
};

// Ctrl+F12's popup: the set's disks, then "Other disk..." (the file browser).
static void setRow(int i, char* out, size_t n) {
    if (i < machine_set_count())
        snprintf(out, n, "%c %d  %s", i == machine_set_index() ? '>' : ' ', i + 1,
                 baseName(machine_set_disk(i)));
    else
        snprintf(out, n, "  Other disk...");
}

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

// Browse for an ADF (or DMS) and put it in DF0; true when a disk went in. Shared by the
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
    // wait in Disks > DF0 (the right pane) / Ctrl+F12.
    const bool set = ext == ".m3u";
    if (set ? !machine_load_set(path.c_str(), 0) : !machine_insert(0, path.c_str(), Config::df0_wp)) {
        gfxBegin();
        uiToast(set ? "The disk set's first disk cannot be opened"
                    : "Not an 880 KB ADF or DMS, or it cannot be opened", true, 2000);
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
    NM_RADIO_D   ("DiagROM",     SET_KICK, diagOpts, nullptr),
    NM_RADIO_D   ("A500",        SET_KICK, a500Opts, nullptr),
    NM_ACTIONV_EN("A600",        act_none, laterLabel, nullptr, p_never),
    NM_ACTIONV_EN("A1200",       act_none, laterLabel, nullptr, p_never),
    NM_RADIO_D   ("SD card ROM", SET_KICK, sdOpts, nullptr),
};

static const Node kDisks[] = {
    NM_ACTIONV("DF0", act_df0_insert, df0Label, p_noSet),
    NM_PICK   ("DF0", SET_DF0_DISK, setOpts, setRowKey, setDf0Label, opt_set_foot, p_diskSet),
    NM_ACTION ("Eject DF0", act_df0_eject, nullptr),
    NM_BOOL   ("DF0 write protect", SET_DF0_WP, nullptr),
    NM_RADIO  ("Floppy drives", SET_DRIVES, opt_drives, nullptr),
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
