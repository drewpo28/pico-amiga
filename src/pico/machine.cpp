// The machine around the core (machine.h): Kickstart selection and the drives.
#include "machine.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <string>
#include "ff.h"
#include "dms.h"
#include "roms.h"
#include "app/Config.h"

#define KB 1024u

static uint8_t *s_rom_buf, *s_ext_buf;
static uint8_t *s_dms_area;
static uint32_t s_dms_size;
static const char *s_rom_name = "";
static std::string s_file_name;          // s_rom_name of a Custom ROM

void machine_init(uint8_t *rom_buf, uint8_t *ext_buf, uint8_t *dms_area, uint32_t dms_size) {
    s_rom_buf = rom_buf;
    s_ext_buf = ext_buf;
    s_dms_area = dms_area;
    s_dms_size = dms_size;
}

static uint32_t load_file(const char *path, uint8_t *dst, uint32_t max) {
    FIL f;
    if (!dst || f_open(&f, path, FA_READ) != FR_OK) return 0;
    UINT n = 0;
    FSIZE_t size = f_size(&f);
    if (size > max) size = max;
    if (f_read(&f, dst, (UINT)size, &n) != FR_OK) n = 0;
    f_close(&f);
    return n;
}

static void select_model(amiga_config_t *cfg) {
    const bool plus = Config::machine == 1;
    cfg->chipset = plus ? AMIGA_ECS : AMIGA_OCS;
    cfg->chip_size = plus ? 1024 * KB : 512 * KB;
    cfg->slow_size = plus ? 0 : 512 * KB;
}

const char *machine_name(void) { return Config::machine == 1 ? "A500+" : "A500"; }

bool machine_select_rom(amiga_config_t *cfg) {
    select_model(cfg);
    cfg->ext_rom = NULL;
    cfg->ext_size = 0;
    if (Config::kick == "sd") {
        const std::string &path = Config::kick_file;
        const uint32_t rs = load_file(path.c_str(), s_rom_buf, 512 * KB);
        if (rs == 256 * KB || rs == 512 * KB) {
            cfg->rom = s_rom_buf;
            cfg->rom_size = rs;
            const std::string dir = path.substr(0, path.find_last_of('/') + 1);
            const uint32_t es = load_file((dir + "ext.rom").c_str(), s_ext_buf, 512 * KB);
            if (es == 512 * KB) { cfg->ext_rom = s_ext_buf; cfg->ext_size = es; }
            s_file_name = path.substr(path.find_last_of('/') + 1);
            s_rom_name = s_file_name.c_str();
            return true;
        }
    }
    const builtin_rom_t *r = builtin_rom_find(Config::kick.c_str());
    if (!r && builtin_rom_count) r = &builtin_roms[0];
    if (!r) return false;
    cfg->rom = r->data;
    cfg->rom_size = r->size;
    s_rom_name = r->label;
    return true;
}

bool machine_apply_kickstart(void) {
    amiga_config_t c;
    memset(&c, 0, sizeof c);
    if (!machine_select_rom(&c)) return false;
    amiga_set_roms(c.rom, c.rom_size, c.ext_rom, c.ext_size);
    amiga_set_machine(c.chipset, c.chip_size, c.slow_size);
    amiga_reset(true);
    return true;
}

const char *machine_rom_name(void) { return s_rom_name; }

// ---- drives ------------------------------------------------------------------------
// An ADF is streamed from the card: the core reads and writes whole tracks through
// these callbacks, the FIL stays open while the disk is in.  A DMS is unpacked
// into PSRAM when it goes in (dms.c) and read from there, write-protected.
struct Drive {
    FIL f;
    bool open;              // f is open (ADF)
    uint8_t *img;           // the unpacked DMS (PSRAM), or null
    std::string path;
};
static Drive s_drv[4];

static int adf_read(void *ctx, uint32_t off, void *dst, uint32_t len) {
    FIL *f = (FIL *)ctx; UINT n;
    if (f_lseek(f, off) != FR_OK || f_read(f, dst, len, &n) != FR_OK || n != len) return -1;
    return 0;
}
static int adf_write(void *ctx, uint32_t off, const void *src, uint32_t len) {
    FIL *f = (FIL *)ctx; UINT n;
    if (f_lseek(f, off) != FR_OK || f_write(f, src, len, &n) != FR_OK || n != len) return -1;
    f_sync(f);
    return 0;
}

static int img_read(void *ctx, uint32_t off, void *dst, uint32_t len) {
    if (off + len > DMS_ADF_SIZE) return -1;
    memcpy(dst, (const uint8_t *)ctx + off, len);
    return 0;
}

// The DMS area (machine_init): the decoder's work area, then one 880 KB image per
// drive, as many as the PSRAM holds.
static uint8_t *dms_slot(int drive) {
    const uint32_t off = ((dms_work_size() + 4095) & ~4095u) + (uint32_t)drive * DMS_ADF_SIZE;
    return s_dms_area && off + DMS_ADF_SIZE <= s_dms_size ? s_dms_area + off : nullptr;
}

static uint32_t dms_fread(void *ctx, void *dst, uint32_t len) {
    UINT n = 0;
    if (f_read((FIL *)ctx, dst, len, &n) != FR_OK) return 0;
    return n;
}

static bool has_ext(const char *path, const char *ext) {
    const char *dot = strrchr(path, '.');
    return dot && strcasecmp(dot, ext) == 0;
}

static void eject_adf(int drive) {
    if (drive < 0 || drive > 3) return;
    Drive &d = s_drv[drive];
    amiga_eject_disk(drive);
    if (d.open) f_close(&d.f);
    d.open = false;
    d.img = nullptr;
    d.path.clear();
}

// The disk set (machine_load_set); s_set_index is the disk in DF0.
static std::string s_set[16];
static int s_set_count, s_set_index;

static bool insert_adf(int drive, const char *path, bool write_protect);

void machine_eject(int drive) {
    if (drive == 0) s_set_count = 0;      // ejecting ends a disk set
    eject_adf(drive);
}

bool machine_insert(int drive, const char *path, bool write_protect) {
    if (drive == 0) s_set_count = 0;
    return insert_adf(drive, path, write_protect);
}


static bool insert_dms(int drive, const char *path) {
    Drive &d = s_drv[drive];
    uint8_t *img = dms_slot(drive);
    if (!img) return false;                                 // not enough PSRAM for this drive
    // Static: this runs from the menu, deep in core 0's 4 KB stack.
    static FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return false;
    const int err = dms_unpack(dms_fread, &f, img, s_dms_area);
    f_close(&f);
    if (err != DMS_OK) {
        printf("DF%d: %s: %s\n", drive, path, dms_error(err));
        return false;
    }
    d.img = img;
    d.path = path;
    amiga_disk_t disk = { img_read, NULL, img, DMS_ADF_SIZE };
    amiga_insert_disk(drive, &disk);
    return true;
}

static bool insert_adf(int drive, const char *path, bool write_protect) {
    if (drive < 0 || drive > 3 || !path || !*path) return false;
    eject_adf(drive);
    if (has_ext(path, ".dms")) return insert_dms(drive, path);
    Drive &d = s_drv[drive];
    bool rw = !write_protect && f_open(&d.f, path, FA_READ | FA_WRITE) == FR_OK;
    if (!rw && f_open(&d.f, path, FA_READ) != FR_OK) return false;
    if (f_size(&d.f) != 901120) {                          // 880 KB DD ADF only
        f_close(&d.f);
        return false;
    }
    d.open = true;
    d.path = path;
    amiga_disk_t disk = { adf_read, rw ? adf_write : NULL, &d.f, (uint32_t)f_size(&d.f) };
    amiga_insert_disk(drive, &disk);
    return true;
}

const char *machine_disk(int drive) {
    return (drive >= 0 && drive <= 3) ? s_drv[drive].path.c_str() : "";
}

static bool is_m3u(const std::string &p) {
    if (p.size() < 4) return false;
    std::string e = p.substr(p.size() - 4);
    for (char &c : e) c = (char)(c | 0x20);
    return e == ".m3u";
}

bool machine_load_set(const char *m3u_path, int index) {
    // Static: this runs from the menu, deep in core 0's 4 KB stack.
    static FIL f;
    if (f_open(&f, m3u_path, FA_READ) != FR_OK) return false;
    const std::string list = m3u_path;
    const std::string dir = list.substr(0, list.find_last_of('/') + 1);
    // A line at a time through a small buffer (SRAM is the menu's heap); a
    // line longer than it is dropped - a path that long would not open anyway.
    static char buf[256];
    static std::string set[16];
    int n = 0;
    UINT len = 0;                                           // bytes in buf
    bool eof = false, skip = false;
    while (n < 16) {
        char *nl = (char *)memchr(buf, '\n', len);
        if (!nl && !eof && len < sizeof buf) {
            UINT more = 0;
            if (f_read(&f, buf + len, sizeof buf - len, &more) != FR_OK || !more) eof = true;
            len += more;
            continue;
        }
        if (!len) break;
        const UINT ll = nl ? (UINT)(nl - buf) : len;
        const bool whole = nl || eof;
        std::string l = whole && !skip ? std::string(buf, ll) : std::string();
        skip = !whole;                                      // overlong: drop up to its newline
        const UINT drop = nl ? ll + 1 : len;
        memmove(buf, buf + drop, len - drop);
        len -= drop;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' ' || l.back() == '\t'))
            l.pop_back();
        if (l.compare(0, 3, "\xEF\xBB\xBF") == 0) l = l.substr(3);   // UTF-8 BOM (before a '#' too)
        size_t s = l.find_first_not_of(" \t");
        if (s == std::string::npos || l[s] == '#') continue;   // blank, or #SAVEDISK: etc.
        l = l.substr(s);
        // RetroArch's "file|label" form: the label is not a path.
        const size_t bar = l.find('|');
        if (bar != std::string::npos) l = l.substr(0, bar);
        set[n++] = l[0] == '/' ? l : dir + l;
    }
    f_close(&f);
    if (!n) return false;
    if (index < 0 || index >= n) index = 0;
    if (!insert_adf(0, set[index].c_str(), Config::df0_wp)) return false;
    for (int i = 0; i < n; i++) s_set[i] = set[i];
    s_set_count = n;
    s_set_index = index;
    return true;
}

int machine_set_count(void) { return s_set_count; }
int machine_set_index(void) { return s_set_index; }
const char *machine_set_disk(int i) { return (i >= 0 && i < s_set_count) ? s_set[i].c_str() : ""; }

bool machine_set_select(int i) {
    if (i < 0 || i >= s_set_count) return false;
    // A disk that sits in another drive changes places with DF0's.
    const std::string old = s_drv[0].path;
    int other = 0;
    for (int d = 1; d < 4; d++)
        if (!s_drv[d].path.empty() && s_drv[d].path == s_set[i]) other = d;
    if (other) eject_adf(other);
    if (!insert_adf(0, s_set[i].c_str(), Config::df0_wp)) return false;
    if (other && !old.empty()) insert_adf(other, old.c_str(), Config::df0_wp);
    s_set_index = i;
    return true;
}

// ---- drives ------------------------------------------------------------------------
static int s_drives = 1;

int machine_drives(void) { return s_drives; }

bool machine_apply_drives(void) {
    int n = Config::drives;
    if (!n) n = s_set_count > 1 ? (s_set_count < 4 ? s_set_count : 4) : 1;   // Auto
    const bool changed = n != s_drives;
    s_drives = n;
    amiga_set_drives(n);
    // DF1.. hold the set's disks 2.. (PUAE's multi-drive), DF0 the chosen one.
    for (int d = 1; d < 4; d++) {
        const bool want = d < n && d < s_set_count && s_set[d] != s_drv[0].path;
        if (want && s_drv[d].path == s_set[d]) continue;
        eject_adf(d);
        if (want) insert_adf(d, s_set[d].c_str(), Config::df0_wp);
    }
    return changed;
}

void machine_restore_disks(void) {
    if (!Config::df0.empty()) {
        const bool ok = is_m3u(Config::df0)
            ? machine_load_set(Config::df0.c_str(), Config::df0_disk)
            : machine_insert(0, Config::df0.c_str(), Config::df0_wp);
        if (!ok) Config::df0.clear();
        machine_apply_drives();
        return;
    }
    // The card layout before the menu existed: /amiga/df0.adf goes in by itself.
    machine_insert(0, "/amiga/df0.adf", false);
}
