// The machine around the core (machine.h): Kickstart selection and the drives.
#include "machine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <string>
#include <algorithm>
#include <vector>
#include "ff.h"
#include "dms.h"
#include "roms.h"
#include "app/Config.h"
#include "pico/stdlib.h"

#define KB 1024u

static uint8_t *s_rom_buf, *s_ext_buf;
// One file for the menu's one-off jobs (a DMS, an .m3u, a new disk): they never
// overlap, and each FIL carries a 512-byte buffer of the menu's SRAM heap.
// Static: these run deep in core 0's 4 KB stack.
static FIL s_job;
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
    bool manual;            // DF1..DF3: put there by hand (Open file), not by a set
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
    d.manual = false;
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
    if (!insert_adf(drive, path, write_protect)) return false;
    s_drv[drive].manual = drive > 0;
    return true;
}


static bool insert_dms(int drive, const char *path) {
    Drive &d = s_drv[drive];
    uint8_t *img = dms_slot(drive);
    if (!img) return false;                                 // not enough PSRAM for this drive
    FIL &f = s_job;
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
    FIL &f = s_job;
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

// ---- blank disks ---------------------------------------------------------------------
static void put32(uint8_t *b, int off, uint32_t v) {
    b[off] = (uint8_t)(v >> 24); b[off + 1] = (uint8_t)(v >> 16);
    b[off + 2] = (uint8_t)(v >> 8); b[off + 3] = (uint8_t)v;
}

// AmigaDOS block checksum: the longs sum to 0.
static void block_sum(uint8_t *b, int off) {
    uint32_t s = 0;
    for (int i = 0; i < 512; i += 4)
        s += ((uint32_t)b[i] << 24) | ((uint32_t)b[i + 1] << 16) | ((uint32_t)b[i + 2] << 8) | b[i + 3];
    put32(b, off, (uint32_t)-s);
}

static bool file_exists(const std::string &p) {
    FILINFO fi;
    return f_stat(p.c_str(), &fi) == FR_OK;
}

bool machine_format_adf(const char *path, const char *volume) {
    FIL &f = s_job;
    if (f_open(&f, path, FA_WRITE | FA_CREATE_NEW) != FR_OK) return false;
    // The volume's dates tell it from other volumes of the same name (two
    // "Empty" disks in two drives): the creation time is the board's clock.
    const uint32_t t = time_us_32();
    const uint32_t days = 17000 + (t >> 26), mins = (t >> 20) % 1440, ticks = (t >> 10) % 3000;
    const size_t vlen = strlen(volume) < 30 ? strlen(volume) : 30;
    static uint8_t blk[512];
    bool ok = true;
    for (int n = 0; n < 1760 && ok; n++) {
        memset(blk, 0, sizeof blk);
        if (n == 0) {
            memcpy(blk, "DOS", 4);                          // DOS\0: OFS, not bootable
            put32(blk, 8, 880);
        } else if (n == 880) {                              // the root block
            put32(blk, 0x000, 2);                           // T_HEADER
            put32(blk, 0x00C, 72);                          // hash table size
            put32(blk, 0x138, 0xFFFFFFFFu);                 // bitmap valid
            put32(blk, 0x13C, 881);                         // bitmap block
            for (int o : { 0x1A4, 0x1D8, 0x1E4 }) {         // root, volume, creation dates
                put32(blk, o, days); put32(blk, o + 4, mins); put32(blk, o + 8, ticks);
            }
            blk[0x1B0] = (uint8_t)vlen;
            memcpy(blk + 0x1B1, volume, vlen);
            put32(blk, 0x1FC, 1);                           // ST_ROOT
            block_sum(blk, 0x014);
        } else if (n == 881) {                              // the bitmap: 1 = free
            for (int b = 2; b < 1760; b++)
                if (b != 880 && b != 881) blk[4 + (b - 2) / 32 * 4 + 3 - ((b - 2) % 32) / 8] |= (uint8_t)(1u << ((b - 2) % 8));
            block_sum(blk, 0);
        }
        UINT w = 0;
        ok = f_write(&f, blk, sizeof blk, &w) == FR_OK && w == sizeof blk;
    }
    f_close(&f);
    if (!ok) f_unlink(path);
    return ok;
}

bool machine_new_disk(const char *volume, char *path_out, unsigned path_size) {
    // Where: beside the set's .m3u, else beside DF0's disk, else /amiga/disks/.
    std::string dir = s_set_count ? Config::df0 : s_drv[0].path;
    dir = dir.substr(0, dir.find_last_of('/') + 1);
    if (dir.empty()) { dir = "/amiga/disks/"; if (!file_exists("/amiga/disks")) dir = "/amiga/"; }
    std::string base = volume;
    for (char &c : base)
        if (strchr(":/\\*?\"<>|", c) || (unsigned char)c < 32) c = '_';
    std::string path = dir + base + ".adf";
    for (int n = 2; file_exists(path) && n < 100; n++) path = dir + base + " " + std::to_string(n) + ".adf";
    if (!machine_format_adf(path.c_str(), volume)) return false;
    snprintf(path_out, path_size, "%s", path.c_str());
    return true;
}

// ---- disk info (the browser's F1) ------------------------------------------------------
typedef bool (*blk_fn)(void *ctx, uint32_t n, uint8_t *b);

static bool file_blk(void *ctx, uint32_t n, uint8_t *b) {
    UINT got = 0;
    return n < 1760 && f_lseek((FIL *)ctx, n * 512) == FR_OK &&
           f_read((FIL *)ctx, b, 512, &got) == FR_OK && got == 512;
}
static bool mem_blk(void *ctx, uint32_t n, uint8_t *b) {
    if (n >= 1760) return false;
    memcpy(b, (const uint8_t *)ctx + n * 512, 512);
    return true;
}
static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static bool sum_ok(const uint8_t *b) {             // header blocks: the longs sum to 0
    uint32_t s = 0;
    for (int i = 0; i < 512; i += 4) s += rd32(b + i);
    return s == 0;
}
static std::string bname(const uint8_t *b) {
    const int n = b[0x1B0] < 30 ? b[0x1B0] : 30;
    return std::string((const char *)b + 0x1B1, (size_t)n);
}
static bool name_eq(const std::string &a, const char *b) {
    return strcasecmp(a.c_str(), b) == 0;
}

// Days since 1978-01-01 -> "YYYY-MM-DD".
static void amiga_date(uint32_t days, char *out, size_t n) {
    int y = 1978;
    for (;;) {
        const int dy = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
        if (days < (uint32_t)dy || y > 2200) break;
        days -= (uint32_t)dy; y++;
    }
    static const uint8_t ml[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int m = 0;
    for (; m < 12; m++) {
        const int d = ml[m] + (m == 1 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)));
        if (days < (uint32_t)d) break;
        days -= (uint32_t)d;
    }
    snprintf(out, n, "%04d-%02d-%02d", y % 10000, (m + 1) % 100, (int)(days % 31) + 1);
}

// b0, b1: two 512-byte buffers.
static void adf_info(blk_fn rd, void *ctx, uint8_t *b0, uint8_t *b1, std::string &o) {
    char line[96];
    // The boot block is blocks 0-1: their longs summed with carry give ~0 when it boots.
    if (!rd(ctx, 0, b0) || !rd(ctx, 1, b1)) { o += "The image cannot be read.\n"; return; }
    uint32_t sum = 0;
    for (int i = 0; i < 1024; i += 4) {
        const uint32_t v = rd32(i < 512 ? b0 + i : b1 + i - 512), t = sum + v;
        sum = t < sum ? t + 1 : t;
    }
    const bool boots = sum == 0xFFFFFFFFu;
    const uint8_t dostype = b0[3];
    const bool dos = !memcmp(b0, "DOS", 3) && dostype <= 7;
    if (!dos) {
        snprintf(line, sizeof line, "Format   : not AmigaDOS (%s)\n",
                 boots ? "its own boot block: a game or demo loader" : "no boot block: a data disk");
        o += line;
        return;
    }
    static const char *const fs[] = { "OFS", "FFS", "OFS intl", "FFS intl", "OFS dircache", "FFS dircache", "OFS long names", "FFS long names" };
    const bool ffs = dostype & 1;
    snprintf(line, sizeof line, "Format   : AmigaDOS %s (DOS\\%u)%s\n", fs[dostype], dostype, boots ? ", bootable" : "");
    o += line;
    if (!rd(ctx, 880, b0) || rd32(b0) != 2 || rd32(b0 + 0x1FC) != 1 || !sum_ok(b0)) {
        o += boots ? "Files    : none - the boot block loads the rest itself\n"
                   : "Root     : not valid (an unformatted or damaged disk)\n";
        return;
    }
    char date[24];
    amiga_date(rd32(b0 + 0x1E4), date, sizeof date);
    snprintf(line, sizeof line, "Volume   : %s\nCreated  : %s\n", bname(b0).c_str(), date);
    o += line;
    // Free space: the bitmap's set bits for blocks 2..1759.
    const uint32_t bm = rd32(b0 + 0x13C);
    if (bm && rd(ctx, bm, b1)) {
        int nfree = 0;
        for (int b = 2; b < 1760; b++) nfree += (rd32(b1 + 4 + (b - 2) / 32 * 4) >> ((b - 2) % 32)) & 1;
        snprintf(line, sizeof line, "Used     : %d of 1758 blocks, %d KB free\n", 1758 - nfree, nfree * (ffs ? 512 : 488) / 1024);
        o += line;
    }
    // The root directory, sorted, directories first.
    std::vector<std::string> ents;              // "0name" dirs, "1name<tab>size" files
    uint32_t s_dir = 0;
    for (int i = 0; i < 72 && ents.size() < 40; i++) {
        for (uint32_t h = rd32(b0 + 24 + 4 * i); h && h < 1760 && ents.size() < 40; h = rd32(b1 + 0x1F0)) {
            if (!rd(ctx, h, b1)) break;
            const std::string n = bname(b1);
            if ((int32_t)rd32(b1 + 0x1FC) == 2) {
                ents.push_back("0" + n);
                if (name_eq(n, "s")) s_dir = h;
            } else {
                ents.push_back("1" + n + "\t" + std::to_string(rd32(b1 + 0x144)));
            }
        }
    }
    std::sort(ents.begin(), ents.end(), [](const std::string &x, const std::string &y) {
        return strcasecmp(x.c_str(), y.c_str()) < 0;
    });
    o += "\nRoot:\n";
    for (const std::string &e : ents) {
        const size_t t = e.find('\t');
        if (e[0] == '0') snprintf(line, sizeof line, "  %s/\n", e.c_str() + 1);
        else snprintf(line, sizeof line, "  %-30s %7s\n", e.substr(1, t - 1).c_str(), e.c_str() + t + 1);
        o += line;
    }
    if (ents.size() >= 40) o += "  ...\n";
    // The start of s/startup-sequence: what the disk runs.
    if (!s_dir || !rd(ctx, s_dir, b0)) return;
    uint32_t ss = 0;
    for (int i = 0; i < 72 && !ss; i++)
        for (uint32_t h = rd32(b0 + 24 + 4 * i); h && h < 1760; h = rd32(b1 + 0x1F0)) {
            if (!rd(ctx, h, b1)) break;
            if (name_eq(bname(b1), "startup-sequence")) { ss = h; break; }
        }
    if (!ss || !rd(ctx, ss, b0)) return;
    o += "\ns/startup-sequence:\n";
    const uint32_t size = rd32(b0 + 0x144), nblk = rd32(b0 + 8);
    std::string text;
    for (uint32_t k = 0; k < nblk && k < 72 && text.size() < 900; k++) {
        if (!rd(ctx, rd32(b0 + 24 + 4 * (71 - k)), b1)) break;
        if (ffs) text.append((const char *)b1, 512);
        else     text.append((const char *)b1 + 24, rd32(b1 + 12) <= 488 ? rd32(b1 + 12) : 488);
    }
    if (text.size() > size) text.resize(size);
    int lines = 0;
    for (size_t p = 0; p < text.size() && lines < 16; lines++) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        std::string l = text.substr(p, e - p);
        for (char &c : l) if ((unsigned char)c < 32 || (unsigned char)c > 126) c = ' ';
        if (l.size() > 44) l.resize(44);
        o += "  " + l + "\n";
        p = e + 1;
    }
}

void machine_disk_info(const char *path, char *out, unsigned size) {
    std::string o;
    uint8_t *buf = (uint8_t *)malloc(1024);
    if (!buf) { snprintf(out, size, "Out of memory.\n"); return; }
    FIL &f = s_job;
    if (has_ext(path, ".dms")) {
        // Unpacked into the slot after DF3's, where the PSRAM has one.
        uint8_t *img = dms_slot(4);
        if (!img) o = "A DMS archive: no spare PSRAM to unpack it for a look.\n";
        else if (f_open(&f, path, FA_READ) != FR_OK) o = "The file cannot be opened.\n";
        else {
            const int err = dms_unpack(dms_fread, &f, img, s_dms_area);
            f_close(&f);
            if (err != DMS_OK) o = std::string("DMS: ") + dms_error(err) + "\n";
            else { o = "A DMS archive, unpacked:\n"; adf_info(mem_blk, img, buf, buf + 512, o); }
        }
    } else if (f_open(&f, path, FA_READ) != FR_OK) {
        o = "The file cannot be opened.\n";
    } else {
        if (f_size(&f) != DMS_ADF_SIZE) o = "Not an 880 KB ADF.\n";
        else adf_info(file_blk, &f, buf, buf + 512, o);
        f_close(&f);
    }
    free(buf);
    snprintf(out, size, "%s", o.c_str());
}

bool machine_set_add(const char *path) {
    if (!s_set_count || s_set_count >= 16 || Config::df0.empty()) return false;
    const std::string dir = Config::df0.substr(0, Config::df0.find_last_of('/') + 1);
    std::string line = path;
    if (line.compare(0, dir.size(), dir) == 0) line = line.substr(dir.size());   // relative, as the rest
    FIL &f = s_job;
    if (f_open(&f, Config::df0.c_str(), FA_READ | FA_WRITE | FA_OPEN_EXISTING) != FR_OK) return false;
    // A last line without its newline gets one first.
    const FSIZE_t size = f_size(&f);
    char c = '\n'; UINT got = 0;
    if (size) { f_lseek(&f, size - 1); f_read(&f, &c, 1, &got); }
    const bool nl = c == '\n';
    f_lseek(&f, size);
    line = (nl ? "" : "\n") + line + "\n";
    UINT w = 0;
    const bool ok = f_write(&f, line.data(), (UINT)line.size(), &w) == FR_OK && w == line.size();
    f_close(&f);
    if (ok) s_set[s_set_count++] = path;
    return ok;
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
        if (!want && d < n && s_drv[d].manual) continue;     // a disk put there by hand stays
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
