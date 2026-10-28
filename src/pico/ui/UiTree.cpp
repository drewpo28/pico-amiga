// pico-amiga — the menu tree (pico-speccy's UiTree.cpp is the model for the shape;
// the content is the Amiga's).
//
// Static const Node arrays, see UiModel.h. Rows bind to settings through the
// SettingId X-table in UiStage.h; actions and info pages are the small functions
// below.

#include "OSDNewMenu.h"

#include <stdio.h>
#include <stdlib.h>
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
extern "C" {
#include "video.h"
#include "audio_out.h"
#include "board.h"
#include "psram.h"
#include "ui.h"
#include "pico/time.h"
}
#include <string>

namespace nm {

// ── option tables ──────────────────────────────────────────────────────────────

static const Option opt_mouse_swap[] = {
    { "Off", 0 }, { "On", 1 }, { "Auto (first single press)", 2, "Auto" },
};
static const Option opt_mouse_sens[] = {
    { "1/4", 64 }, { "1/2", 128 }, { "1:1", 256 }, { "x2", 512 },
};
// Audio > Output: Auto is the DAC it finds (or PWM) and HDMI as well; HDMI alone
// is offered on an HDMI display.
static const Option* audioOutOpts(uint8_t& cnt) {
    static const Option opts[] = {
        { "Auto (DAC or PWM, and HDMI)", 0, "Auto" }, { "PWM", 1 }, { "I2S", 2 },
        { "HDMI", 3 },
    };
    cnt = video_is_vga() ? 3 : 4;
    return opts;
}
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
// built-in ROMs filed under it (cmake/roms.cmake's machines field); a row whose
// ROM is not the one running shows no value. Values are what UiStage's get_kick
// reads: the built-in ROM's index, + 200 on the A500+. Custom ROM is a row of
// its own (a file from the card, then the machine).
// Built once per boot (the table is fixed in the image).
static uint8_t romOpts(const char* machine, Option* opts, uint8_t max, const char* slabel, int32_t add) {
    uint8_t n = 0;
    for (unsigned i = 0; i < builtin_rom_count && n < max; i++)
        if (builtin_rom_on(&builtin_roms[i], machine)) opts[n++] = { builtin_roms[i].label, (int32_t)i + add, slabel };
    return n;
}
static const Option* diagOpts(uint8_t& cnt) {
    static Option opts[2];
    static uint8_t n = romOpts("DiagROM", opts, 2, "On", 0);
    cnt = n;
    return opts;
}
static const Option* a500Opts(uint8_t& cnt) {
    static Option opts[8];
    static uint8_t n = romOpts("A500", opts, 8, nullptr, 0);
    cnt = n;
    return opts;
}
static const Option* a500pOpts(uint8_t& cnt) {
    static Option opts[8];
    static uint8_t n = romOpts("A500+", opts, 8, nullptr, 200);
    cnt = n;
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

// Devices > Disks > DF0: the file name of what is in the drive.
static const char* df0Label() {
    const char* p = machine_disk(0);
    return *p ? baseName(p) : "empty";
}

// Devices > Disks > DF0 with an .m3u disk set loaded: the row shows the set, the right
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
// A file's extension, lower case with its dot ("" when none).
static std::string extOf(const std::string& path) {
    const size_t dot = path.find_last_of('.'), slash = path.find_last_of('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
    std::string ext = path.substr(dot);
    for (char& c : ext) c = (char)tolower((unsigned char)c);
    return ext;
}

// An ADF or DMS into DF0, or an .m3u set (RetroArch's playlist: disk 1 goes in,
// the rest wait in Devices > Disks > DF0, the right pane, and Ctrl+F12), as
// Config keeps it. False, with a toast, when it cannot be opened.
static bool insertDf0(const std::string& path) {
    const bool set = extOf(path) == ".m3u";
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
    return insertDf0(dir + r.substr(1));
}

// Machine > Custom ROM: a Kickstart image from the card, then the machine to run
// it on; a cold start, as any ROM change. The row shows the file while it runs.
static const char* customRomLabel() {
    return Config::kick == "sd" ? machine_rom_name() : "";
}

// A Kickstart file from the card, then the machine to run it on: a cold start.
// False (a toast for a wrong file) when nothing changed.
static bool runRomFile(const std::string& path) {
    FILINFO fi;
    gfxBegin();
    if (f_stat(path.c_str(), &fi) != FR_OK || (fi.fsize != 256 * 1024 && fi.fsize != 512 * 1024)) {
        uiToast("Not a 256 or 512 KB Kickstart image", true, 2000);
        gfxEnd();
        return false;
    }
    static const char* const kMachines[] = { "A500", "A500+" };
    const int m = uiPickList("Run it on", kMachines, 2, Config::machine);
    gfxEnd();
    if (m < 0) return false;
    Config::kick = "sd";
    Config::kick_file = path;
    Config::machine = (uint8_t)m;
    if (!machine_apply_kickstart()) { uiToast("That ROM cannot be read", true, 2000); return false; }
    Config::save();
    return true;
}

static void act_custom_rom() {
    static std::string dir;
    if (dir.empty()) {
        dir = Config::kick_file.substr(0, Config::kick_file.find_last_of('/') + 1);
        FILINFO fi;
        if (dir.size() < 2 || f_stat(dir.substr(0, dir.size() - 1).c_str(), &fi) != FR_OK) dir = "/";
    }
    const std::string r = browseFile(dir, "Custom ROM", BROWSE_ROM);
    if (r.size() < 2 || r[0] != 'R') return;
    if (runRomFile(dir + r.substr(1))) requestClose();   // back to the Amiga, booting it
}

// ── Win+F5: Open file (pico-speccy's F5 browser, for the Amiga) ────────────────
// Every file on the card. Enter: an ADF / DMS goes into DF0, an .m3u loads as a
// disk set, a Kickstart image runs as the Custom ROM. F5 on a disk image: the
// drive picker - DF0..DF3 with what is in them; a drive past the connected ones
// connects that many (Kickstart counts them at reset).
static bool isDiskExt(const std::string& e) { return e == ".adf" || e == ".dms"; }
static bool isRomExt(const std::string& e) {
    return e == ".rom" || e == ".bin" || e == ".a500" || e == ".a600" || e == ".a1000" || e == ".cdtv";
}

static void driveRow(int i, char* out, size_t n) {
    const char* p = machine_disk(i);
    snprintf(out, n, "DF%d  %s%s", i, *p ? baseName(p) : "empty",
             i >= machine_drives() ? "  (not connected)" : "");
}

// True when the disk went in.
static bool toDrive(const std::string& path) {
    char title[64];
    snprintf(title, sizeof title, "Put %.40s into", baseName(path.c_str()));
    const int d = uiPickListCb(title, 4, driveRow, 0, 44);
    if (d < 0) return false;
    if (d == 0) return insertDf0(path);
    const bool more = d >= machine_drives();
    if (more) {
        char q[64];
        snprintf(q, sizeof q, "Connect %d floppy drives?\nThe Amiga is reset.", d + 1);
        if (!uiConfirm(q)) return false;
        Config::drives = (uint8_t)(d + 1);
        machine_apply_drives();
    }
    if (!machine_insert(d, path.c_str(), Config::df0_wp)) {
        uiToast("Not an 880 KB ADF or DMS, or it cannot be opened", true, 2000);
        return false;
    }
    if (more) { Config::save(); uiAmigaReset(); }
    return true;
}

void runOpenFile() {
    static std::string dir;
    if (dir.empty()) {
        const std::string cur = machine_disk(0);
        dir = cur.empty() ? std::string("/amiga/") : cur.substr(0, cur.find_last_of('/') + 1);
        FILINFO fi;
        if (dir != "/" && f_stat(dir.substr(0, dir.size() - 1).c_str(), &fi) != FR_OK) dir = "/";
    }
    for (;;) {
        const std::string r = browseFile(dir, "Open file", BROWSE_ALL);
        if (r.size() < 2 || (r[0] != 'R' && r[0] != 'P')) return;
        const std::string path = dir + r.substr(1);
        const std::string ext = extOf(path);
        gfxBegin();
        bool done = false;
        if (r[0] == 'P') done = toDrive(path);                  // F5: a disk image (the browser checked)
        else if (isDiskExt(ext) || ext == ".m3u") done = insertDf0(path);
        else if (isRomExt(ext)) done = runRomFile(path);
        else uiToast("Not an Amiga disk, disk set or Kickstart", true, 2000);
        gfxEnd();
        if (done) return;                                       // back to the Amiga
    }
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

// Devices > Disks > New blank disk: a formatted empty ADF on the card. With a disk set
// loaded it joins the set (Ctrl+F12 swaps it in, e.g. when a game's own
// save-disk formatter asks for a disk); otherwise it goes into DF0.
static void act_new_disk() {
    std::string name = "Empty";
    if (!uiPrompt("New blank disk: volume name", name, 30)) return;
    char path[256];
    if (!machine_new_disk(name.c_str(), path, sizeof path)) {
        uiToast("The disk cannot be created on the card", true, 2000);
        return;
    }
    char msg[96];
    if (machine_set_count() > 0) {
        if (!machine_set_add(path)) { uiToast("Created, but the disk set is full", true, 2000); return; }
        snprintf(msg, sizeof msg, "%s: in the disk set (Ctrl+F12)", baseName(path));
    } else {
        if (!machine_insert(0, path, Config::df0_wp)) { uiToast("Created, but it cannot be opened", true, 2000); return; }
        Config::df0 = path;
        Config::df0_disk = 0;
        Config::save();
        snprintf(msg, sizeof msg, "%s: in DF0", baseName(path));
    }
    uiToast(msg, false, 2000);
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

void runRebootAsk() {
    gfxBegin();
    const bool yes = uiConfirm("Reboot the board?", "Reboot", true);
    gfxEnd();
    if (yes) OSD::esp_hard_reset();
}

static void page_hotkeys() {
    uiTextPage("Hotkeys",
        "F12         This menu (Esc goes back, F12 closes)\n"
        "Ctrl+F12    Insert a disk into DF0 (an .m3u set: swap disks)\n"
        "F11         Reset the Amiga (as Shift+F12)\n"
        "Shift+F12   Reset the Amiga\n"
        "Win+F11     Reboot the board (asked, Yes by default)\n"
        "Win+F1      System info: board, memory, chip temperature\n"
        "Win+F5      Open file: disks, disk sets, Kickstarts; F5 on a\n"
        "            disk picks the drive (DF0..DF3)\n"
        "Win+F8      Performance band (with the drive's track and\n"
        "            sector): summary, breakdown, off\n"
        "Ctrl+J      Cursor keys as joystick on/off\n"
        "F1..F10     Amiga function keys\n"
        "\n"
        "Joystick (game port 2) while cursor keys are a joystick:\n"
        "  Cursor keys   directions\n"
        "  Left Ctrl     fire\n"
        "\n"
        "Amiga keys on a PC keyboard:\n"
        "  Win / PgDn / PgUp   left / left / right Amiga\n"
        "  Insert              Help\n"
        "  Home, End           keypad ( )\n"
        "\n"
        "Mouse: port 1. A USB/XInput/NES pad drives port 2.");
}

// System info (Help, Win+F1): the board, its memories and the machine running.
// Rebuilt every second (the temperature, the free heap and the uptime move), into
// a buffer taken from the heap while the page is open (SRAM is the heap).
#define SYSINFO_BUF 768
static char* s_sysinfo;

static const char* sysInfoText() {
    char* const buf = s_sysinfo;
    int pos = 0;
    const auto add = [&](const char* fmt, auto... a) {
        if (pos < SYSINFO_BUF) pos += snprintf(buf + pos, SYSINFO_BUF - pos, fmt, a...);
    };
#ifdef VIDEO_PIO
    const char* hdmi = "HDMI (PIO)";
#else
    const char* hdmi = "HDMI (HSTX)";
#endif
    add(" Firmware       : %s / %s [v%s]\n", CONFIG_BOARD_TAG, video_is_vga() ? "VGA" : hdmi, PORT_VERSION);
    const video_timing_t* vt = video_timing();
    const uint32_t vtot = (uint32_t)(vt->h_fp + vt->h_sync + vt->h_bp + vt->h_active) *
                          (vt->v_fp + vt->v_sync + vt->v_bp + vt->v_active);
    const uint32_t hz100 = (uint32_t)((uint64_t)vt->pixel_hz * 100 / vtot);
    add(" Video mode     : %ux%u @%u.%02u Hz, %u.%u MHz\n", vt->h_active, vt->v_active, (unsigned)(hz100 / 100),
        (unsigned)(hz100 % 100), (unsigned)(vt->pixel_hz / 1000000), (unsigned)(vt->pixel_hz / 100000 % 10));
    const int mv = board_vreg_mv(), t = board_temp_x10();
    add(" Chip           : RP2350%c, %u MHz\n", board_chip_package(), (unsigned)board_cpu_mhz());
    add(" Chip VREG/TEMP : %d.%02d V / %d.%d C\n", mv / 1000, mv % 1000 / 10, t / 10, (t < 0 ? -t : t) % 10);
    add(" Chip RAM       : 520 KB, %u KB free (heap)\n", (unsigned)(board_free_heap() / 1024));
    const uint8_t* j = board_flash_jedec();
    add(" Flash          : %u MB, JEDEC %02X-%02X-%02X\n", (unsigned)(board_flash_size() >> 20), j[0], j[1], j[2]);
    add(" PSRAM          : %u MB, SCK <= %d MHz\n", (unsigned)(psram_size() >> 20), settings_psram_mhz());
    add(" Machine        : %s, %s\n", machine_name(), machine_rom_name());
    add(" Amiga RAM      : %s\n", Config::machine == 1 ? "1 MB chip (ECS)" : "512 KB chip + 512 KB slow (OCS)");
    add(" Floppy drives  : %d\n", machine_drives());
    add(" Audio          : %s\n", audio_out_name());
    const unsigned s = (unsigned)(time_us_64() / 1000000);
    add(" Uptime         : %u:%02u:%02u\n", s / 3600, s / 60 % 60, s % 60);
    return buf;
}

static void page_sysinfo() {
    s_sysinfo = (char*)malloc(SYSINFO_BUF);
    if (!s_sysinfo) { uiToast("Out of memory", true, 1500); return; }
    uiTextPageLive("System info", sysInfoText, 1000);
    free(s_sysinfo);
    s_sysinfo = nullptr;
}

void runSysInfo() {
    gfxBegin();
    page_sysinfo();
    gfxEnd();
}

static void page_about() {
    uiTextPage("About",
        "Pico-Amiga " PORT_VERSION "\n"
        "Amiga 500 / 500+ (OCS / ECS, PAL) for RP2350 + PSRAM\n"
        "\n"
        "CPU: Musashi 68000\n"
        "Drivers, menu and host build after pico-speccy");
}

// ── the tree ───────────────────────────────────────────────────────────────────

static const Node kMachine[] = {
    NM_RADIO_D   ("DiagROM",     SET_KICK, diagOpts, nullptr),
    NM_RADIO_D   ("A500",        SET_KICK, a500Opts, nullptr),
    NM_RADIO_D   ("A500+",       SET_KICK, a500pOpts, nullptr),
    NM_ACTIONV_EN("A600",        act_none, laterLabel, nullptr, p_never),
    NM_ACTIONV_EN("A1200",       act_none, laterLabel, nullptr, p_never),
    NM_ACTIONV   ("Custom ROM",  act_custom_rom, customRomLabel, nullptr),
};

static const Node kDisks[] = {
    NM_ACTIONV("DF0", act_df0_insert, df0Label, p_noSet),
    NM_PICK   ("DF0", SET_DF0_DISK, setOpts, setRowKey, setDf0Label, opt_set_foot, p_diskSet),
    NM_ACTION ("Eject DF0", act_df0_eject, nullptr),
    NM_ACTION ("New blank disk", act_new_disk, nullptr),
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
    NM_RADIO_D("Output", SET_AUDIO_OUT, audioOutOpts, nullptr),
};

// Capture card compatibility: only the PIO HDMI encodes pixel pairs itself (the
// HSTX encoder and VGA have nothing to change).
static bool p_pioHdmi() {
#ifdef VIDEO_PIO
    return !video_is_vga();
#else
    return false;
#endif
}

// Video > Mode: the modes this display's scanout runs at the staged CPU clock
// (video_modes.c), and the staged one whatever it is (the radio needs its row).
static const Option* videoModeOpts(uint8_t& cnt) {
    static const Option k[] = {
        { "720x576 @50 Hz",          VM_720x576_50, "720x576 @50" },
        { "800x600 @50 Hz, framed",  VM_800x600_X2, "800x600 @50" },
        { "640x480 @60 Hz, cropped", VM_640x480_60, "640x480 @60" },
    };
    static Option opts[NM_COUNT(k)];
    const int32_t staged = Stage::get(SET_VIDEO_MODE);
    const int mhz = (int)Stage::get(SET_CPU_MHZ);
    video_timing_t t;
    uint8_t n = 0;
    for (const Option& o : k)
        if (o.value == staged || video_mode_timing((int)o.value, video_out(), mhz, &t)) opts[n++] = o;
    cnt = n;
    return opts;
}

static const Node kVideo[] = {
    NM_RADIO_D("Mode", SET_VIDEO_MODE, videoModeOpts, nullptr),
    NM_RADIO("Picture position", SET_HPOS, opt_hpos, nullptr),
    NM_BOOL("Centre short pictures vertically", SET_VCENTRE, nullptr),
    NM_BOOL("Disk activity icon", SET_DISK_LED, nullptr),
    NM_BOOL("Capture card compatibility", SET_HDMI_CAPTURE, p_pioHdmi),
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
    NM_PAGE("Hotkeys",     page_hotkeys, nullptr),
    NM_PAGE("About",       page_about,   nullptr),
    NM_PAGE("System info", page_sysinfo, nullptr),
};

static const Node kReset[] = {
    NM_ACTION("Reset the Amiga",  act_amiga_reset, nullptr),
    NM_ACTION("Reboot the board", act_reboot,      nullptr),
};

// Devices: what is plugged into the Amiga (pico-speccy's Devices row).
static const Node kDevices[] = {
    NM_SUB("Disks",     kDisks,     nullptr),
    NM_SUB("Input",     kInput,     nullptr),
};

// pico-speccy's order: Help first, Devices after Machine.
static const Node kRoot[] = {
    NM_SUB("Help",      kHelp,      nullptr),
    NM_SUB("Machine",   kMachine,   nullptr),
    NM_SUB("Devices",   kDevices,   nullptr),
    NM_SUB("Video",     kVideo,     nullptr),
    NM_SUB("Audio",     kAudio,     nullptr),
    NM_SUB("Options",   kOptions,   nullptr),
    NM_SUB("Reset",     kReset,     nullptr),
    NM_SUB("Debug",     kDebug,     nullptr),
};

const Node* rootNodes()     { return kRoot; }
uint8_t     rootNodeCount() { return NM_COUNT(kRoot); }

}  // namespace nm
