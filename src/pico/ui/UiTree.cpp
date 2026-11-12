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
#include "UiDesc.h"
#include "UiLogo.h"
#include "UiStrings.h"
#include "UiPlatform.h"
#include "UiNav.h"
#include "UiBrowser.h"
#include "UiGfx.h"
#include "UiRender.h"
#include "UiSnap.h"
#include "UiJoy.h"
#include "app/Config.h"
#include "roms.h"
#include "machine.h"
#include "snapshot.h"
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
    { "1/8", 32 }, { "1/4", 64 }, { "3/8", 96 }, { "1/2", 128 }, { "3/4", 192 },
    { "1:1", 256 }, { "x1.5", 384 }, { "x2", 512 },
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
// 540, as pico-speccy's: every video mode switches to a 27 / 36 MHz twin
// (video_modes.c, PIO divider 2.0 / 1.5, HSTX clk_hstx 135 = 540 / 4), the
// PSRAM runs at 135 MHz (540 / 4, the proven ceiling) and the core needs 1.80 V
// (Config::load raises it; at 1.60-1.70 V pico-speccy's DVp2 corrupted SRAM).
static const Option opt_cpu_mhz[] = {
    { "252 MHz", 252 }, { "378 MHz", 378 }, { "504 MHz", 504 }, { "540 MHz", 540 },
};
// pico-speccy's Core voltage steps (its 1.90 / 2.00 V are storage.nvs-only there
// and not offered here); the value is mV (Config::vreg_mv).
static const Option opt_vreg[] = {
    { "1.15 V", 1150 }, { "1.20 V", 1200 }, { "1.25 V", 1250 }, { "1.30 V", 1300 },
    { "1.35 V", 1350 }, { "1.40 V", 1400 }, { "1.50 V", 1500 }, { "1.60 V", 1600 },
    { "1.65 V", 1650 }, { "1.70 V", 1700 }, { "1.80 V", 1800 },
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
static const Option opt_a600_chip[] = {
    { "1 MB (as it came)", 1, "1 MB" }, { "2 MB (A601 trapdoor)", 2, "2 MB" },
};
// A Zorro II fast RAM board (WHDLoad's "other memory"); the PSRAM's DMS area
// gives it: with 4 MB, DMS images no longer fit (machine_fast_max).
static const Option opt_fast_ram[] = {
    { "None (as it came)", 0, "None" }, { "1 MB", 1 }, { "2 MB", 2 }, { "4 MB (no DMS disks)", 4, "4 MB" },
};
static bool p_fast_ok() { return machine_fast_max() > 0; }

static const Option opt_drives[] = {
    { "Auto (one per disk of an .m3u set)", 0, "Auto" },
    { "1 (DF0)", 1, "1" }, { "2 (DF0, DF1)", 2, "2" },
    { "3 (DF0, DF1, DF2)", 3, "3" }, { "4 (DF0..DF3)", 4, "4" },
};
static const Option opt_hpos[] = {
    { "Centred", 0 }, { "Wide left (+12 columns)", 1, "Wide left" },
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
// reads: the built-in ROM's index, + 200 x Config::machine. Custom ROM is a row of
// its own (a file from the card, then the machine).
// Built once per boot (the table is fixed in the image).
static uint8_t romOpts(const char* machine, Option* opts, uint8_t max, const char* slabel, int32_t add) {
    uint8_t n = 0;
    for (unsigned i = 0; i < builtin_rom_count && n < max; i++)
        if (builtin_rom_on(&builtin_roms[i], machine)) opts[n++] = { builtin_roms[i].label, (int32_t)i + add, slabel };
    return n;
}
static const Option* diagOpts(uint8_t& cnt) {
    // Its CPU is the choice: + 50 is UiStage's KICK_020 (Config::diag_cpu).
    static Option opts[2];
    static uint8_t n = romOpts("DiagROM", opts, 1, "68000", 0);
    if (n) {
        opts[1] = { "68EC020, 14 MHz", opts[0].value + 50, "68020" };
        opts[0].label = "68000, 7 MHz";
        n = 2;
    }
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
static const Option* a1200Opts(uint8_t& cnt) {
    static Option opts[4];
    static uint8_t n = romOpts("A1200", opts, 4, nullptr, 600);
    cnt = n;
    return opts;
}
static const Option* cd32Opts(uint8_t& cnt) {
    static Option opts[2];
    static uint8_t n = romOpts("CD32", opts, 2, nullptr, 800);
    cnt = n;
    return opts;
}
static const Option* a600Opts(uint8_t& cnt) {
    static Option opts[8];
    static uint8_t n = romOpts("A600", opts, 8, nullptr, 400);
    cnt = n;
    return opts;
}

// ── actions and pages ──────────────────────────────────────────────────────────

static const char* baseName(const char* p) {
    const char* s = strrchr(p, '/');
    return s ? s + 1 : p;
}

// Devices > Floppy drives > DF0 with an .m3u disk set loaded: the row shows the set, the right
// pane its disks (the one in DF0 marked; a disk in another drive says which),
// Enter swaps the chosen one into DF0.
static bool p_diskSet() { return machine_set_count() > 1; }

#define SET_ROWS 17                                   // machine.cpp's 16 disks + Other disk...
// PICO-AMIGA: the labels in PSRAM (ui_psram_scratch), out of the heap.
static char (*s_set_labels)[48];
void uiSetLabelsBuffer(void* p) { s_set_labels = (char (*)[48])p; }

static const Option* setOpts(uint8_t& cnt) {
    static Option opts[SET_ROWS];
    if (!s_set_labels) s_set_labels = (char (*)[48])malloc(SET_ROWS * sizeof *s_set_labels);
    if (!s_set_labels) { opts[0] = { "Other disk...", machine_set_count(), nullptr }; cnt = 1; return opts; }
    char (*const labels)[48] = s_set_labels;
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
// the rest wait in Devices > Floppy drives > DF0, the right pane, and Ctrl+F12), as
// Config keeps it. False, with a toast, when it cannot be opened.
static bool insertDf0(const std::string& path) {
    const bool set = extOf(path) == ".m3u";
    if (Config::machine == MACHINE_CD32) {      // no floppy port; a CD goes through insertCd
        gfxBegin();
        uiToast("The CD32 has no floppy drive: Machine", true, 2500);
        gfxEnd();
        return false;
    }
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
    // Start where the last disk came from, else in /pico-amiga/.
    static std::string dir;
    if (dir.empty()) {
        const std::string cur = machine_disk(0);
        dir = cur.empty() ? std::string("/pico-amiga/") : cur.substr(0, cur.find_last_of('/') + 1);
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
    static const char* const kMachines[] = { "A500", "A500+", "A600", "A1200", "CD32 (ext.rom beside it)" };
    const int m = uiPickList("Run it on", kMachines, MACHINE_COUNT, Config::machine);
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
// disk set, a Kickstart image runs as the Custom ROM, a .pas snapshot is loaded
// (pico-speccy's F2 has no key of its own here). F5 on a disk image: the
// drive picker - DF0..DF3 with what is in them; a drive past the connected ones
// connects that many (Kickstart counts them at reset).
static bool isDiskExt(const std::string& e) { return e == ".adf" || e == ".dms"; }
static bool isLhaExt(const std::string& e) { return e == ".lha" || e == ".lzh"; }

// An LHA (a WHDLoad install, or any program) as the hard disk: built into an
// HDF on the card the first time (machine_boot_lha), then the Amiga restarts
// from it.  True when it boots.
static bool bootLha(const std::string& path) {
    char msg[96];
    uiBusy("Preparing the hard disk...");
    const bool ok = machine_boot_lha(path.c_str(), msg, sizeof msg);
    Config::save();
    if (!ok) { uiToast(msg[0] ? msg : "The archive cannot be used", true, 2500); return false; }
    uiAmigaReset();
    if (msg[0]) uiToast(msg, true, 3000);
    return true;
}
static bool isCdExt(const std::string& e) { return e == ".iso" || e == ".cue"; }

// A CD image into the CD32's drive, the machine made a CD32 first when it is
// another (its built-in Kickstart; none linked: a toast).  The CD32 restarts
// and boots the disc.  True when it does.
static bool insertCd(const std::string& path) {
    if (Config::machine != MACHINE_CD32) {
        if (!builtin_rom_find("ks31cd32") || !machine_a600_2mb_ok()) {
            uiToast("A CD needs the CD32: Machine > CD32", true, 2500);
            return false;
        }
        Config::machine = MACHINE_CD32;
        Config::kick = "ks31cd32";
        if (!machine_apply_kickstart()) { uiToast("The CD32's ROM cannot be used", true, 2000); return false; }
    }
    if (!machine_set_cd(path.c_str())) {
        Config::cd.clear();
        Config::save();
        uiToast("Not a CD image this reads (ISO, CUE + BIN)", true, 2500);
        return false;
    }
    Config::cd = path;
    Config::save();
    uiAmigaReset();
    return true;
}

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
        dir = cur.empty() ? std::string("/pico-amiga/") : cur.substr(0, cur.find_last_of('/') + 1);
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
        else if (isLhaExt(ext)) done = bootLha(path);
        else if (isCdExt(ext)) done = insertCd(path);
        else if (isRomExt(ext)) done = runRomFile(path);
        else if (ext == ".pas") done = snapLoadPath(path);
        else if (machine_is_program(path.c_str())) done = bootLha(path);   // an Amiga executable
        else uiToast("Not an Amiga disk, disk set, CD, archive, program, Kickstart or snapshot", true, 2000);
        gfxEnd();
        if (done) return;                                       // back to the Amiga
    }
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

// Devices > Floppy drives > New blank disk: a formatted empty ADF on the card. With a disk set
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

// ── Devices > Floppy drives: a row per connected drive ─────────────────────────────────
// The row shows the disk's name; its right pane the drive's list: the disk itself
// (Enter: what is on it), Insert, Eject, Write protect - and in DF0 with an .m3u
// set loaded, the set's disks first (Enter swaps one in) in place of the name.
// Tags below DRV_T_INFO (UiDesc.h) are the set's disk numbers (SET_DF0_DISK marks DF0's).

static const char* driveShort(int d) {
    const char* p = machine_disk(d);
    return *p ? baseName(p) : "empty";
}

static const Option* driveOpts(int d, uint8_t& cnt) {
    // One buffer: the pane shows one drive's list at a time (SRAM is the heap).
    static Option o[SET_ROWS + 4];
    static char name[64], wp[32];
    uint8_t n = 0;
    const char* p = machine_disk(d);
    const bool set = d == 0 && p_diskSet();
    if (set) {
        uint8_t sc; const Option* so = setOpts(sc);
        for (uint8_t i = 0; i + 1 < sc; i++) o[n++] = so[i];        // without "Other disk..."
    } else if (*p) {
        snprintf(name, sizeof name, "%s", baseName(p));
        o[n++] = { name, DRV_T_INFO, nullptr };
    }
    o[n++] = { set ? "Other disk..." : "Insert disk...", DRV_T_INSERT, nullptr };
    if (*p) {
        if (set) o[n++] = { "What is on it", DRV_T_INFO, nullptr };
        o[n++] = { "Eject", DRV_T_EJECT, nullptr };
        snprintf(wp, sizeof wp, "Write protect: %s",
                 machine_disk_dms(d) ? "On (DMS)" : machine_disk_ro(d) ? "On" : "Off");
        o[n++] = { wp, DRV_T_WP, nullptr };
    }
    cnt = n;
    return o;
}

// A disk for DF1..DF3 from the browser (DF0 also takes .m3u sets: df0PickAndInsert).
static bool drivePickAndInsert(int d) {
    if (d == 0) return df0PickAndInsert();
    static std::string dir;
    if (dir.empty()) {
        const std::string cur = machine_disk(0);
        dir = cur.empty() ? std::string("/pico-amiga/") : cur.substr(0, cur.find_last_of('/') + 1);
        FILINFO fi;
        if (dir != "/" && f_stat(dir.substr(0, dir.size() - 1).c_str(), &fi) != FR_OK) dir = "/";
    }
    char title[24];
    snprintf(title, sizeof title, "Insert into DF%d", d);
    const std::string r = browseFile(dir, title, BROWSE_ADF);
    if (r.size() < 2 || r[0] != 'R') return false;
    if (extOf(r) == ".m3u") { uiToast("A disk set goes into DF0", true, 2000); return false; }
    if (!machine_insert(d, (dir + r.substr(1)).c_str(), Config::df0_wp)) {
        uiToast("Not an 880 KB ADF or DMS, or it cannot be opened", true, 2000);
        return false;
    }
    return true;
}

static void driveKey(int d, int32_t tag, uint8_t key) {
    if (key) return;
    const char* p = machine_disk(d);
    switch (tag) {
    case DRV_T_INFO: {
        if (!*p) return;
        char* info = (char*)malloc(2048);
        if (!info) return;
        machine_disk_info(p, info, 2048);
        uiTextPage(baseName(p), info);
        free(info);
        return;
    }
    case DRV_T_INSERT:
        if (drivePickAndInsert(d)) requestClose();   // back to the Amiga with the disk in
        return;
    case DRV_T_EJECT:
        if (d == 0) act_df0_eject();
        else machine_eject(d);
        return;
    case DRV_T_WP:
        if (machine_disk_dms(d)) { uiToast("A DMS is always write-protected", true, 2000); return; }
        if (!machine_write_protect(d, !machine_disk_ro(d))) { uiToast("The disk cannot be opened again", true, 2000); return; }
        if (d == 0) { Config::df0_wp = machine_disk_ro(0); Config::save(); }
        return;
    default:
        setRowKey(tag, key);                          // a disk of DF0's set
        return;
    }
}

static const Option opt_drive_foot[] = {
    { SYM_ENTER " Choose   " SYM_LEFT " Back", 0 },
};

#define DRIVE_ROW(d)                                                                     \
    static const Option* driveOpts##d(uint8_t& cnt) { return driveOpts(d, cnt); }      \
    static void driveKey##d(int32_t tag, uint8_t key) { driveKey(d, tag, key); }         \
    static const char* driveShort##d() { return driveShort(d); }
#define DRIVE_VIS(d) static bool p_drive##d() { return d < machine_drives(); }
DRIVE_VIS(1)
DRIVE_VIS(2)
DRIVE_VIS(3)
#undef DRIVE_VIS
DRIVE_ROW(0)
DRIVE_ROW(1)
DRIVE_ROW(2)
DRIVE_ROW(3)
#undef DRIVE_ROW


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
        "Win+F5      Open file: disks, disk sets, CDs, Kickstarts, snapshots;\n"
        "            F5 on a disk picks the drive (DF0..DF3)\n"
        "Win+F3      Snapshots: the quick slots, Enter loads\n"
        "Win+F4      Snapshots: the quick slots, Enter saves\n"
        "Alt+Win+F3  Load the current quick slot\n"
        "Alt+Win+F4  Save to the current quick slot\n"
        "Win+F8      Performance band (with the drive's track and\n"
        "            sector): summary, breakdown, off\n"
        "Ctrl+J      Keyboard as joystick on/off\n"
        "F1..F10     Amiga function keys\n"
        "\n"
        "Joystick (game port 2) while the keyboard is one:\n"
        "  Cursor keys   directions\n"
        "  Left Ctrl     fire (red), Right Ctrl blue\n"
        "\n"
        "Amiga keys on a PC keyboard:\n"
        "  Win / PgDn / PgUp   left / left / right Amiga\n"
        "  Insert              Help\n"
        "  Home, End           keypad ( )\n"
        "\n"
        "The pad's buttons and the keys that stand in for it\n"
        "  (the CD32 pad's seven; red is fire, blue the second\n"
        "  button): Devices > Input.\n"
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
    // Puya flash (ZERO2): the QE fix's outcome, board.c (4 and 6 are failures).
    if (const uint8_t qe = board_flash_qe())
        add(" Flash QE       : %s\n", qe == 1 ? "was set" : qe == 4 ? "FIX FAILED" : qe == 6 ? "no raw access" : "set at boot");
    add(" PSRAM          : %u MB, SCK <= %d MHz\n", (unsigned)(psram_size() >> 20), settings_psram_mhz());
    add(" Machine        : %s, %s\n", machine_name(), machine_rom_name());
    if (Config::machine == MACHINE_A500) add(" Amiga RAM      : 512 KB chip + 512 KB slow (OCS)\n");
    else add(" Amiga RAM      : %u MB chip (%s)\n", (unsigned)(machine_chip_size() >> 20),
             Config::machine >= MACHINE_A1200 ? "AGA" : "ECS");
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

// The logo above the credits (UiStrings.h), as pico-speccy's About.
static void page_about() {
    uiPicturePage("About  v" PORT_VERSION, ui_logo128, UI_LOGO_W, UI_LOGO_H, TXT_HELP_ABOUT_BODY);
}

// ── the tree ───────────────────────────────────────────────────────────────────

// Machine > A600: its Kickstarts, and its chip RAM (1 MB, or 2 MB with an A601).
static const Node kA600[] = {
    NM_RADIO_D   ("Kickstart",   SET_KICK, a600Opts, nullptr),
    NM_RADIO_EN  ("Chip RAM",    SET_A600_CHIP, opt_a600_chip, nullptr, machine_a600_2mb_ok),
    NM_RADIO_EN  ("Fast RAM",    SET_FAST_RAM, opt_fast_ram, nullptr, p_fast_ok),
};

// Machine > A1200: its Kickstarts (its 2 MB of chip RAM needs the 2 MB chip
// area: 8 MB of PSRAM), and how its 68EC020 waits for the chip bus
// (amiga_set_chip_bus): as a stock A1200's (~1.3 MIPS, the cheapest to emulate),
// with its instruction cache counted (~1.7 MIPS, a third more emulation work a
// frame), or not at all (as with fast RAM, twice the work).  "Stock, cache for
// music" is the stock one that takes the cache's speed while the program's
// music routine runs every other frame only (amiga_set_chip_bus_auto).
static const Option opt_chip_bus[] = {
    { "Stock A1200", 1, "Stock" }, { "Stock, cache for music", 3, "Auto" }, { "With cache", 2, "Cache" }, { "Off (fast RAM)", 0, "Off" },
};
static const Node kA1200[] = {
    NM_RADIO_DE  ("Kickstart",   SET_KICK, a1200Opts, nullptr, machine_a600_2mb_ok),
    NM_RADIO     ("Chip bus",    SET_CHIP_BUS, opt_chip_bus, nullptr),
    NM_RADIO_EN  ("Fast RAM",    SET_FAST_RAM, opt_fast_ram, nullptr, p_fast_ok),
};

static const Node kMachine[] = {
    NM_RADIO_D   ("DiagROM",     SET_KICK, diagOpts, nullptr),
    NM_RADIO_D   ("A500",        SET_KICK, a500Opts, nullptr),
    NM_RADIO_D   ("A500+",       SET_KICK, a500pOpts, nullptr),
    NM_SUB       ("A600",        kA600,    nullptr),
    NM_SUB       ("A1200",       kA1200,   nullptr),
    NM_RADIO_DE  ("CD32",        SET_KICK, cd32Opts, nullptr, machine_a600_2mb_ok),
    NM_ACTIONV   ("Custom ROM",  act_custom_rom, customRomLabel, nullptr),
};

// Devices > Hard disk (Gayle's IDE port: the A600, the A1200): Master and Slave,
// each row the drive's name; its pane the drive (Enter: name and size), Mount...
// and Remove.  Mount... is one list, pico-speccy's source pick: an image file
// (an HDF, or an LHA / a program built into one: the master only) or one of the
// card's partitions (its own sectors, pico-speccy's raw IDE window).  Kickstart
// looks for the drives at reset, so a change restarts the Amiga.
static bool p_a600() { return Config::machine == MACHINE_A600 || Config::machine == MACHINE_A1200; }   // Gayle
static std::string& hdCfgImg(int u) { return u ? Config::hdf2 : Config::hdf; }
static std::string& hdCfgRaw(int u) { return u ? Config::hdf2_raw : Config::hdf_raw; }
static const char* hdShort(int u) {
    const char* p = machine_hdf(u);
    return *p ? baseName(p) : "none";
}
static const Option* hdOpts(int u, uint8_t& cnt) {
    static Option o[3];
    static char name[64];
    uint8_t n = 0;
    const char* p = machine_hdf(u);
    if (*p) {
        snprintf(name, sizeof name, "%s", baseName(p));     // what it holds: the description (UiDesc.cpp)
        o[n++] = { name, DRV_T_INFO, nullptr };
    }
    o[n++] = { "Mount...", DRV_T_INSERT, nullptr };
    if (*p) o[n++] = { "Remove", DRV_T_EJECT, nullptr };
    cnt = n;
    return o;
}

// An image file from the browser into drive u.  True when the Amiga restarts with it.
static bool hdMountImage(int u) {
    static std::string dir;
    if (dir.empty()) {
        const std::string cur = machine_hdf_is_raw(u) ? "" : machine_hdf(u);
        dir = cur.empty() ? std::string("/pico-amiga/") : cur.substr(0, cur.find_last_of('/') + 1);
        FILINFO fi;
        if (dir != "/" && f_stat(dir.substr(0, dir.size() - 1).c_str(), &fi) != FR_OK) dir = "/";
    }
    const std::string r = browseFile(dir, u ? "Hard disk: slave" : "Hard disk: master", BROWSE_HDF);
    if (r.size() < 2 || r[0] != 'R') return false;
    const std::string path = dir + r.substr(1);
    if (isLhaExt(extOf(path))) {                       // an archive: its built image, the boot drive
        if (u) { uiToast("An archive boots from the master", true, 2500); return false; }
        return bootLha(path);
    }
    if (path == machine_hdf(u ^ 1)) {
        uiToast(u ? "That image is the master's" : "That image is the slave's", true, 2500);
        return false;
    }
    if (!machine_set_hdf(u, path.c_str())) {
        hdCfgImg(u).clear();
        hdCfgRaw(u).clear();
        Config::save();
        uiToast("That HDF cannot be opened", true, 2000);
        return false;
    }
    hdCfgImg(u) = path;
    hdCfgRaw(u).clear();
    Config::save();
    uiAmigaReset();
    return true;
}

// Mount...: the source list.  True when the Amiga restarts with a new drive.
static bool hdMount(int u) {
    MachinePart parts[8];
    const int found = machine_sd_parts(parts);
    if (found <= 0) return hdMountImage(u);            // no card sectors to offer: an image is the only source
    static char labels[8][56];
    const char* items[9];
    int map[9], k = 0, initial = 0;
    unsigned cm = 0, cp = 0, cl = 0, cn = 0;           // the window in use, for the cursor
    if (machine_hdf_is_raw(u)) sscanf(hdCfgRaw(u).c_str(), "%u:%u:%u:%u", &cm, &cp, &cl, &cn);
    items[k] = "Image file (HDF, LHA)...";
    map[k++] = -1;
    for (int i = 0; i < 8; i++) {
        if (!parts[i].sectors) continue;
        const uint32_t mb = parts[i].sectors / 2048;
        if (mb >= 1024)
            snprintf(labels[i], sizeof labels[i], "SD card: part %d  %u.%u GB, %s%s", parts[i].no, (unsigned)(mb / 1024),
                     (unsigned)(mb % 1024 * 10 / 1024), parts[i].kind, parts[i].files ? " (files)" : "");
        else
            snprintf(labels[i], sizeof labels[i], "SD card: part %d  %u MB, %s%s", parts[i].no, (unsigned)mb,
                     parts[i].kind, parts[i].files ? " (files)" : "");
        if (cm && parts[i].lba == cl && parts[i].sectors == cn) initial = k;
        items[k] = labels[i];
        map[k++] = i;
    }
    const int c = uiPickList(u ? "Hard disk: slave" : "Hard disk: master", items, k, initial);
    if (c < 0) return false;
    if (map[c] < 0) return hdMountImage(u);
    const MachinePart& q = parts[map[c]];
    // The card's own files are never written under FatFs; another partition asks.
    static const char* const kMode[] = { "Read-only", "Writable" };
    const int m = q.files ? 0 : uiPickList("The Amiga may", kMode, 2, 0);
    if (m < 0) return false;
    if (!machine_set_hdf_raw(u, q.no, q.lba, q.sectors, m == 1)) {
        hdCfgRaw(u).clear();
        hdCfgImg(u).clear();
        Config::save();
        uiToast("That partition cannot be used", true, 2000);
        return false;
    }
    char v[48];
    snprintf(v, sizeof v, "%d:%u:%u:%u", machine_hdf_ro(u) ? 1 : 2, (unsigned)q.no, (unsigned)q.lba, (unsigned)q.sectors);
    hdCfgRaw(u) = v;
    hdCfgImg(u).clear();
    Config::save();
    uiAmigaReset();
    if (m == 1 && machine_hdf_ro(u)) uiToast("Read-only: the other drive has it", true, 2500);
    return true;
}

static void hdKey(int u, int32_t tag, uint8_t key) {
    if (key) return;
    const char* p = machine_hdf(u);
    switch (tag) {
    case DRV_T_INFO: {
        FILINFO fi;
        char msg[96];
        if (machine_hdf_is_raw(u)) {
            snprintf(msg, sizeof msg, "%s: %s%s", p, machine_hdf_kind(u), machine_hdf_ro(u) ? ", read-only" : "");
            uiToast(msg, false, 2500);
            return;
        }
        if (!*p || f_stat(p, &fi) != FR_OK) return;
        snprintf(msg, sizeof msg, "%s: %u MB%s", baseName(p), (unsigned)(fi.fsize >> 20),
                 (fi.fattrib & AM_RDO) ? ", read-only" : "");
        uiToast(msg, false, 2500);
        return;
    }
    case DRV_T_INSERT:
        if (hdMount(u)) requestClose();               // back to the Amiga, booting with it
        return;
    case DRV_T_EJECT:
        machine_set_hdf(u, "");
        hdCfgImg(u).clear();
        hdCfgRaw(u).clear();
        Config::save();
        uiAmigaReset();
        requestClose();
        return;
    default:
        return;
    }
}

#define HD_ROW(u)                                                                       \
    static const Option* hdOpts##u(uint8_t& cnt) { return hdOpts(u, cnt); }           \
    static void hdKey##u(int32_t tag, uint8_t key) { hdKey(u, tag, key); }              \
    static const char* hdShort##u() { return hdShort(u); }
HD_ROW(0)
HD_ROW(1)
#undef HD_ROW

static const Node kHardDisk[] = {
    NM_PICK  ("Master", SET_NONE, hdOpts0, hdKey0, hdShort0, opt_drive_foot, nullptr),
    NM_PICK  ("Slave",  SET_NONE, hdOpts1, hdKey1, hdShort1, opt_drive_foot, nullptr),
};

// Devices > CD drive (the CD32): the disc's name; its pane the disc, Insert
// disc... (an .iso, or a .cue with its track files) and Eject.
static bool p_cd32() { return Config::machine == MACHINE_CD32; }
static bool p_floppy() { return Config::machine != MACHINE_CD32; }   // the CD32 has no floppy port
static const char* cdShort() {
    const char* p = machine_cd();
    return *p ? baseName(p) : "empty";
}
static const Option* cdOpts(uint8_t& cnt) {
    static Option o[3];
    static char name[64];
    uint8_t n = 0;
    const char* p = machine_cd();
    if (*p) {
        snprintf(name, sizeof name, "%s", baseName(p));
        o[n++] = { name, DRV_T_INFO, nullptr };
    }
    o[n++] = { "Insert disc...", DRV_T_INSERT, nullptr };
    if (*p) o[n++] = { "Eject", DRV_T_EJECT, nullptr };
    cnt = n;
    return o;
}
static void cdKey(int32_t tag, uint8_t key) {
    if (key) return;
    switch (tag) {
    case DRV_T_INFO: {
        FILINFO fi;
        char msg[96];
        const char* p = machine_cd();
        if (!*p || f_stat(p, &fi) != FR_OK) return;
        snprintf(msg, sizeof msg, "%s", baseName(p));
        uiToast(msg, false, 2500);
        return;
    }
    case DRV_T_INSERT: {
        static std::string dir;
        if (dir.empty()) {
            const std::string cur = machine_cd();
            dir = cur.empty() ? std::string("/pico-amiga/") : cur.substr(0, cur.find_last_of('/') + 1);
            FILINFO fi;
            if (dir != "/" && f_stat(dir.substr(0, dir.size() - 1).c_str(), &fi) != FR_OK) dir = "/";
        }
        const std::string r = browseFile(dir, "CD drive", BROWSE_CD);
        if (r.size() < 2 || r[0] != 'R') return;
        if (insertCd(dir + r.substr(1))) requestClose();
        return;
    }
    case DRV_T_EJECT:
        machine_set_cd("");
        Config::cd.clear();
        Config::save();
        return;
    default:
        return;
    }
}

static const Node kDisks[] = {
    NM_PICK  ("DF0", SET_DF0_DISK, driveOpts0, driveKey0, driveShort0, opt_drive_foot, nullptr),
    NM_PICK  ("DF1", SET_NONE,     driveOpts1, driveKey1, driveShort1, opt_drive_foot, p_drive1),
    NM_PICK  ("DF2", SET_NONE,     driveOpts2, driveKey2, driveShort2, opt_drive_foot, p_drive2),
    NM_PICK  ("DF3", SET_NONE,     driveOpts3, driveKey3, driveShort3, opt_drive_foot, p_drive3),
    NM_RADIO ("Drives", SET_DRIVES, opt_drives, nullptr),
    NM_ACTION("New blank disk", act_new_disk, nullptr),
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

// Video > Output > Mode: the modes this display's scanout runs at the staged CPU clock
// (video_modes.c), and the staged one whatever it is (the radio needs its row).
// The refresh is the scanout's (800x600 is 50 Hz on HDMI, VESA's 60 on VGA).
static const Option* videoModeOpts(uint8_t& cnt) {
    static const struct { uint8_t vm; uint16_t w, h; const char* how; } k[] = {
        { VM_640x480_60, 640, 480, ", cropped" },
        { VM_720x480_60, 720, 480, ", cropped" },
        { VM_800x600_60, 800, 600, ", framed" },
        { VM_720x576_50, 720, 576, "" },
        { VM_800x600_X2, 800, 600, ", framed" },
    };
    static Option opts[NM_COUNT(k)];
    static char lbl[NM_COUNT(k)][32], slbl[NM_COUNT(k)][16];
    const int32_t staged = Stage::get(SET_VIDEO_MODE);
    const int mhz = (int)Stage::get(SET_CPU_MHZ);
    uint8_t n = 0;
    for (uint8_t i = 0; i < NM_COUNT(k); i++) {
        video_timing_t t;
        const bool ok = video_mode_timing(k[i].vm, video_out(), mhz, &t);
        if (!ok && k[i].vm != staged) continue;
        // Staged but not runnable at the staged clock: it boots as 640x480.
        if (ok) snprintf(lbl[i], sizeof lbl[i], "%ux%u @%u Hz%s", k[i].w, k[i].h, t.hz, k[i].how);
        else    snprintf(lbl[i], sizeof lbl[i], "%ux%u (not at %d MHz)", k[i].w, k[i].h, mhz);
        snprintf(slbl[i], sizeof slbl[i], ok ? "%ux%u @%u" : "%ux%u", k[i].w, k[i].h, ok ? t.hz : 0);
        opts[n++] = { lbl[i], (int32_t)k[i].vm, slbl[i] };
    }
    cnt = n;
    return opts;
}

// Video: what goes out to the display, and how the picture sits in it - two
// groups on one level, their rows indented under the heading.
static const Node kVideo[] = {
    NM_HEAD   ("Output", nullptr),
    NM_RADIO_D(NM_IND "Mode", SET_VIDEO_MODE, videoModeOpts, nullptr),
    NM_BOOL   (NM_IND "Capture card mode", SET_HDMI_CAPTURE, p_pioHdmi),
    NM_HEAD   ("Picture", nullptr),
    NM_RADIO  (NM_IND "Position", SET_HPOS, opt_hpos, nullptr),
    NM_BOOL   (NM_IND "Vertical centring", SET_VCENTRE, nullptr),
    NM_BOOL   (NM_IND "Drive icon", SET_DISK_LED, nullptr),
    NM_BOOL   (NM_IND "Skip frames when late", SET_FRAME_SKIP, nullptr),
};

static const Node kOverclock[] = {
    NM_RADIO("CPU frequency",   SET_CPU_MHZ,    opt_cpu_mhz,    nullptr),
    NM_RADIO("Core voltage",    SET_VREG,       opt_vreg,       nullptr),
    NM_RADIO("Flash frequency", SET_FLASH_FREQ, opt_flash_freq, nullptr),
    NM_RADIO("PSRAM frequency", SET_PSRAM_FREQ, opt_psram_freq, nullptr),
};

static const Node kOptions[] = {
    NM_SUB("Overclock", kOverclock, nullptr),
    NM_SUB("Interface", kInterface, nullptr),
};

static const Node kInput[] = {
    NM_PAGE ("Pad buttons",             joyPadPage,  nullptr),
    NM_BOOL ("Keyboard as joystick",    SET_CURSOR_JOY, nullptr),
    NM_PAGE ("Keyboard keys",           joyKeysPage, nullptr),
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
    // Core 0's hot code, sampled 8000 times a second: PROF lines after each PERF
    // line (with UART log on); tools/pcprof.py names them from the ELF.
    NM_BOOL("PC profiler", SET_PCPROF, nullptr),
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

// Snapshots (UiSnap.cpp): a .pas from the card, and the 40 quick slots as a
// pick list in the right pane, the verbs on the function keys (pico-speccy's).
static const Node kSnapshots[] = {
    NM_ACTION(TXT_SNAP_FROMFILE, loadSnapshotFile, nullptr),
    NM_PICK  (TXT_SNAP_SLOTS, SET_PERSIST_SLOT, persist_rows, persist_key,
              persist_vlabel, persist_foot, nullptr),
};

// Win+F3 / Win+F4 open the menu on the slot list: found by what it is, so the
// row can move in the tree.
const Node* persistNodeFor() {
    for (uint8_t i = 0; i < NM_COUNT(kSnapshots); i++)
        if (kSnapshots[i].kind == K_PICK) return &kSnapshots[i];
    return nullptr;
}

// Devices: what is plugged into the Amiga (pico-speccy's Devices row).
static const Node kDevices[] = {
    NM_SUB("Floppy drives", kDisks,    p_floppy),
    NM_SUB("Hard disk",     kHardDisk, p_a600),
    NM_PICK("CD drive",     SET_NONE, cdOpts, cdKey, cdShort, opt_drive_foot, p_cd32),
    NM_SUB("Input",     kInput,     nullptr),
};

// pico-speccy's order: Help first, Devices after Machine.
static const Node kRoot[] = {
    NM_SUB("Help",      kHelp,      nullptr),
    NM_SUB("Machine",   kMachine,   nullptr),
    NM_SUB(TXT_SNAPSHOTS, kSnapshots, nullptr),
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
