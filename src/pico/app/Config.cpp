// pico-amiga settings (Config.h).  The storage code is pico-speccy's
// (src/app/Config.cpp: NvsWriter, NvsLines, nvs_get_* / nvs_set_*), cut down to
// what pico-amiga needs: no profiles yet, no RAM fallback without a card.
#include "app/Config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "ff.h"

using std::string;
using std::vector;

uint8_t  Config::secondJoy    = 0;
uint16_t Config::mouse_sens   = 256;
uint8_t  Config::mouse_swap   = 0;
bool     Config::CursorAsJoy  = true;
uint8_t  Config::ui_theme     = 0;
bool     Config::ui_rounded   = true;
bool     Config::ui_vga_solid = true;
#ifndef CPU_MHZ
#define CPU_MHZ 378
#endif
uint16_t Config::cpu_mhz      = CPU_MHZ;
uint16_t Config::max_flash_freq = 66;
#ifndef DBG_UART_DEFAULT
#define DBG_UART_DEFAULT 0
#endif
bool     Config::dbg_uart     = DBG_UART_DEFAULT;
bool     Config::render_c1    = true;
uint16_t Config::max_psram_freq = 166;
uint8_t  Config::audio_out    = 0;
uint8_t  Config::volume       = 16;
bool     Config::hdmi_audio   = true;
uint8_t  Config::hpos         = 0;
std::string Config::kick      = "ks13";
std::string Config::df0       = "";
bool     Config::df0_wp       = false;
uint8_t  Config::df0_disk     = 0;
uint8_t  Config::drives       = 0;
bool     Config::loaded       = false;

// ---- reading -------------------------------------------------------------------------
// The parsed file: one buffer ('\n' -> '\0') plus a table of line starts.
struct NvsLines {
    string buf;
    vector<uint32_t> off;
    void split() {
        size_t start = 0;
        off.clear();
        for (size_t i = 0; i < buf.size(); ++i) {
            if (buf[i] == '\n') {
                buf[i] = '\0';
                off.push_back((uint32_t)start);
                start = i + 1;
            }
        }
        if (start < buf.size()) off.push_back((uint32_t)start);   // no trailing '\n'
    }
    bool read(FIL* f) {
        const FSIZE_t sz = f_size(f);
        buf.assign((size_t)sz, '\0');
        UINT br = 0;
        if (sz && (f_read(f, &buf[0], (UINT)sz, &br) != FR_OK || br != sz)) return false;
        split();
        return true;
    }
};

static bool nvs_get_str(const char* key, string& v, const NvsLines& sts) {
    const size_t kl = strlen(key);
    const char* base = sts.buf.c_str();
    for (uint32_t o : sts.off) {
        const char* s = base + o;
        if (strncmp(key, s, kl) == 0 && s[kl] == '=') {
            if (s[kl + 1] == '\0') return false;
            v = s + kl + 1;
            return true;
        }
    }
    return false;
}
static void nvs_get_b(const char* key, bool& v, const NvsLines& sts) {
    string t;
    if (nvs_get_str(key, t, sts)) v = (t == "true");
}
static void nvs_get_u8(const char* key, uint8_t& v, const NvsLines& sts) {
    string t;
    if (nvs_get_str(key, t, sts)) v = (uint8_t)atoi(t.c_str());
}
static void nvs_get_u16(const char* key, uint16_t& v, const NvsLines& sts) {
    string t;
    if (nvs_get_str(key, t, sts)) v = (uint16_t)atoi(t.c_str());
}

void Config::load() {
    FIL f;
    if (f_open(&f, STORAGE_NVS, FA_READ) != FR_OK) return;   // no file yet: defaults
    NvsLines sts;
    const bool ok = sts.read(&f);
    f_close(&f);
    if (!ok) return;
    loaded = true;
    nvs_get_u8 ("joy2",          secondJoy,    sts);
    nvs_get_u16("mouse_sens",    mouse_sens,   sts);
    nvs_get_u8 ("mouse_swap",    mouse_swap,   sts);
    nvs_get_b  ("cursor_as_joy", CursorAsJoy,  sts);
    nvs_get_u8 ("ui_theme",      ui_theme,     sts);
    nvs_get_b  ("ui_rounded",    ui_rounded,   sts);
    nvs_get_b  ("ui_vga_solid",  ui_vga_solid, sts);
    nvs_get_u16("cpu_mhz",       cpu_mhz,      sts);
    nvs_get_u16("max_flash_freq", max_flash_freq, sts);
    nvs_get_u16("max_psram_freq", max_psram_freq, sts);
    nvs_get_u8 ("audio_out",     audio_out,    sts);
    nvs_get_b  ("dbg_uart",      dbg_uart,     sts);
    nvs_get_b  ("render_c1",     render_c1,    sts);
    nvs_get_u8 ("volume",        volume,       sts);
    nvs_get_b  ("hdmi_audio",    hdmi_audio,   sts);
    if (cpu_mhz != 252 && cpu_mhz != 378 && cpu_mhz != 504) cpu_mhz = CPU_MHZ;
    if (volume > 16) volume = 16;
    if (max_flash_freq < 33 || max_flash_freq > 166) max_flash_freq = 66;
    if (max_psram_freq < 66 || max_psram_freq > 166) max_psram_freq = 166;
    nvs_get_str("kick",          kick,         sts);
    if (!nvs_get_str("df0",      df0,          sts)) df0.clear();
    nvs_get_b  ("df0_wp",        df0_wp,       sts);
    nvs_get_u8 ("df0_disk",      df0_disk,     sts);
    nvs_get_u8 ("drives",        drives,       sts);
    if (drives > 4) drives = 0;
    nvs_get_u8 ("hpos",          hpos,         sts);
    if (hpos > 1) hpos = 0;
}

// ---- writing -------------------------------------------------------------------------
// Streams key=value lines straight to the file; the config is never built in one
// heap string.
struct NvsWriter {
    FIL* f = nullptr;
    bool ok = true;
    void write(const char* s, size_t n) {
        UINT bw;
        if (f_write(f, s, (UINT)n, &bw) != FR_OK || bw != n) ok = false;
    }
};
static void nvs_set_str(NvsWriter& buf, const char* name, const char* val) {
    buf.write(name, strlen(name));
    buf.write("=", 1);
    buf.write(val, strlen(val));
    buf.write("\n", 1);
}
static void nvs_set_i(NvsWriter& buf, const char* name, int val) {
    char t[16]; snprintf(t, sizeof(t), "%d", val);
    nvs_set_str(buf, name, t);
}
static void nvs_set_b(NvsWriter& buf, const char* name, bool val) {
    nvs_set_str(buf, name, val ? "true" : "false");
}

static bool mkdir_parents(const char* path) {
    char p[128];
    snprintf(p, sizeof p, "%s", path);
    for (char* s = p + 1; ; s++) {
        const bool end = !*s;
        if (*s == '/' || end) {
            const char c = *s;
            *s = '\0';
            FRESULT r = f_mkdir(p);
            if (r != FR_OK && r != FR_EXIST) return false;
            *s = c;
        }
        if (end) return true;
    }
}

bool Config::save() {
    if (!loaded) {
        // Never overwrite a file this session could not read with defaults.
        FILINFO fi;
        if (f_stat(STORAGE_NVS, &fi) == FR_OK) return false;
    }
    if (!mkdir_parents(CONFIG_DIR_BOARD)) return false;
    static const char* tmp = STORAGE_NVS ".tmp";
    FIL f;
    if (f_open(&f, tmp, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;
    NvsWriter buf;
    buf.f = &f;
    nvs_set_i(buf, "joy2",          secondJoy);
    nvs_set_i(buf, "mouse_sens",    mouse_sens);
    nvs_set_i(buf, "mouse_swap",    mouse_swap);
    nvs_set_b(buf, "cursor_as_joy", CursorAsJoy);
    nvs_set_i(buf, "ui_theme",      ui_theme);
    nvs_set_b(buf, "ui_rounded",    ui_rounded);
    nvs_set_b(buf, "ui_vga_solid",  ui_vga_solid);
    nvs_set_i(buf, "cpu_mhz",       cpu_mhz);
    nvs_set_i(buf, "max_flash_freq", max_flash_freq);
    nvs_set_i(buf, "max_psram_freq", max_psram_freq);
    nvs_set_i(buf, "audio_out",     audio_out);
    nvs_set_b(buf, "dbg_uart",      dbg_uart);
    nvs_set_b(buf, "render_c1",     render_c1);
    nvs_set_i(buf, "volume",        volume);
    nvs_set_b(buf, "hdmi_audio",    hdmi_audio);
    nvs_set_str(buf, "kick",        kick.c_str());
    nvs_set_str(buf, "df0",         df0.c_str());
    nvs_set_b(buf, "df0_wp",        df0_wp);
    nvs_set_i(buf, "df0_disk",      df0_disk);
    nvs_set_i(buf, "drives",        drives);
    nvs_set_i(buf, "hpos",          hpos);
    // f_sync before the rename, so a stalled card cannot leave a half-written file
    // under the real name.
    const FRESULT sy = buf.ok ? f_sync(&f) : FR_DISK_ERR;
    f_close(&f);
    if (!buf.ok || sy != FR_OK) { f_unlink(tmp); return false; }
    FRESULT rn = f_rename(tmp, STORAGE_NVS);
    if (rn == FR_EXIST) {
        f_unlink(STORAGE_NVS);
        rn = f_rename(tmp, STORAGE_NVS);
    }
    if (rn != FR_OK) return false;
    loaded = true;
    return true;
}
