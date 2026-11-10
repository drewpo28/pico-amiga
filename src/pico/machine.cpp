// The machine around the core (machine.h): Kickstart selection and the drives.
#include "machine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <string>
#include "ff.h"
#include "diskio.h"
#include "ide_mbr.h"
#include "dms.h"
#include "whdboot.h"
#include "cdimage.h"
#include <vector>
#include <new>
#include <ctype.h>
#include "roms.h"
#include "app/Config.h"
#include "board.h"              // board_load_cpu_overlay
#include "ui/ui.h"
#include "pico/stdlib.h"

#define KB 1024u

static uint8_t *s_rom_buf, *s_ext_buf;
// One file for the menu's one-off jobs (a DMS, an .m3u, a new disk): they never
// overlap, and each FIL carries a 512-byte buffer of the menu's SRAM heap.
// Static: these run deep in core 0's 4 KB stack.
static FIL s_job;
static uint8_t *s_dms_area;
static uint32_t s_dms_size;              // what the DMS slots may use: the area less the fast RAM
static uint32_t s_dms_total;             // the whole area machine_init gave
static uint32_t s_fast_size;             // the fast RAM board running, at the area's end (0: none)
static const char *s_rom_name = "";
static std::string s_file_name;          // s_rom_name of a Custom ROM

static uint32_t s_chip_area = 1024 * KB;

void machine_init(uint8_t *rom_buf, uint8_t *ext_buf, uint8_t *dms_area, uint32_t dms_size, uint32_t chip_area) {
    s_chip_area = chip_area;
    s_rom_buf = rom_buf;
    s_ext_buf = ext_buf;
    s_dms_area = dms_area;
    s_dms_size = s_dms_total = dms_size;
}

// The fast RAM board: the end of the DMS area, as long as the rest still holds
// the WHDLoad builder's arena (machine_boot_lha) - 4 MB with 8 MB of PSRAM.
uint32_t machine_fast_max(void) {
    for (uint32_t mb = 8; mb; mb >>= 1)
        if (s_dms_area && (mb << 20) + WHD_ARENA_MIN + 64 * KB <= s_dms_total) return mb;
    return 0;
}
uint32_t machine_fast_size(void) { return s_fast_size; }

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

// The A500+ and the A600 are both ECS with 1 MB of chip RAM; the A600 adds Gayle.
// The A1200 is AGA with 2 MB, a 68EC020 and the AGA Gayle.
static void select_model(amiga_config_t *cfg) {
    const int m = Config::machine < MACHINE_COUNT ? (int)Config::machine : (int)MACHINE_A500;
    const bool ecs = m != MACHINE_A500;
    const bool aga = m == MACHINE_A1200 || m == MACHINE_CD32;
    cfg->chipset = aga ? AMIGA_AGA : ecs ? AMIGA_ECS : AMIGA_OCS;
    // An A600 with an A601 in its trapdoor: 2 MB of chip RAM (the 8375 Agnus
    // addresses it), where the PSRAM layout leaves room (machine_init).
    const bool chip2 = s_chip_area >= 2048 * KB &&
                       (aga || (m == MACHINE_A600 && Config::a600_chip == 2));
    cfg->chip_size = chip2 ? 2048 * KB : ecs ? 1024 * KB : 512 * KB;
    cfg->slow_size = ecs ? 0 : 512 * KB;
    cfg->gayle = m == MACHINE_A600 ? AMIGA_GAYLE_A600 : m == MACHINE_A1200 ? AMIGA_GAYLE_A1200 : 0;
    cfg->cpu = aga ? AMIGA_CPU_EC020 : AMIGA_CPU_68000;
    cfg->akiko = m == MACHINE_CD32;
    // Fast RAM on the Gayle machines (a Zorro II board: PCMCIA / trapdoor RAM
    // as WHDLoad sees it), the size the PSRAM permits.
    uint32_t mb = cfg->gayle ? Config::fast_ram : 0;
    while (mb > machine_fast_max()) mb >>= 1;
    cfg->fast_size = mb << 20;
    cfg->fast_ram = mb ? s_dms_area + s_dms_total - cfg->fast_size : NULL;
}

static void drop_lost_dms(void);
// The CD32's read-ahead buffer: the end of the DMS area (a CD32 has no fast RAM
// board there), taken from the DMS slots while the machine is one.
#define CD_AHEAD (32u * KB)
static uint8_t *nvram_get(bool cd32);

static const char *const kModelName[MACHINE_COUNT] = { "A500", "A500+", "A600", "A1200", "CD32" };
static const char *const kModelDesc[MACHINE_COUNT] = {
    "A500 (OCS, 512 KB chip + 512 KB slow)", "A500+ (ECS, 1 MB chip RAM)", "A600 (ECS, 1 MB chip RAM, Gayle)",
    "A1200 (AGA, 68EC020, 2 MB chip RAM, Gayle)", "CD32 (AGA, 68EC020, 2 MB chip RAM, Akiko, CD)" };
const char *machine_model_name(int m) { return m >= 0 && m < MACHINE_COUNT ? kModelName[m] : "?"; }
const char *machine_model_desc(int m) { return m >= 0 && m < MACHINE_COUNT ? kModelDesc[m] : "?"; }
uint32_t machine_chip_size(void) {
    amiga_config_t c;
    memset(&c, 0, sizeof c);
    select_model(&c);
    return c.chip_size;
}
bool machine_a600_2mb_ok(void) { return s_chip_area >= 2048 * KB; }

const char *machine_name(void) { return machine_model_name(Config::machine); }

bool machine_select_rom(amiga_config_t *cfg) {
    select_model(cfg);
    s_fast_size = cfg->fast_size;
    s_dms_size = s_dms_total - s_fast_size - (cfg->akiko && s_dms_total >= 2 * CD_AHEAD ? CD_AHEAD : 0);
    drop_lost_dms();
    board_load_cpu_overlay(cfg->cpu);
    cfg->nvram = nvram_get(cfg->akiko);
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
    if (cfg->akiko) {                           // the CD32's extended ROM goes with its Kickstart
        const builtin_rom_t *e = builtin_rom_find("cd32ext");
        if (e) { cfg->ext_rom = e->data; cfg->ext_size = e->size; }
    }
    return true;
}

void machine_home_migrate(void) {
    FILINFO fi;
    if (f_stat("/pico-amiga", &fi) == FR_OK || f_stat("/amiga", &fi) != FR_OK || !(fi.fattrib & AM_DIR)) return;
    if (f_rename("/amiga", "/pico-amiga") != FR_OK) return;
    printf("card: /amiga is /pico-amiga now\n");
    std::string *const paths[] = { &Config::kick_file, &Config::df0, &Config::hdf, &Config::hdf2, &Config::cd };
    for (std::string *p : paths)
        if (p->compare(0, 7, "/amiga/") == 0) p->replace(0, 6, "/pico-amiga");
    Config::save();
}

// ---- the CD32: its NVRAM and its disc -----------------------------------------------
// The NVRAM (1 KB: the games' saves) is on the heap while the machine is a CD32
// and in /pico-amiga/cd32.nvram on the card; written back a second after the Amiga
// last touched it (machine_frame).
#define CD32_NVRAM "/pico-amiga/cd32.nvram"
static uint8_t *s_nvram;
static uint8_t s_nvram_wait;

static uint8_t *nvram_get(bool cd32) {
    if (!cd32) { free(s_nvram); s_nvram = NULL; return NULL; }
    if (s_nvram) return s_nvram;
    s_nvram = (uint8_t *)calloc(1, 1024);
    if (s_nvram) load_file(CD32_NVRAM, s_nvram, 1024);
    return s_nvram;
}

void machine_frame(void) {
    if (!s_nvram) return;
    if (amiga_cd_nvram_dirty()) s_nvram_wait = 50;
    if (!s_nvram_wait || --s_nvram_wait) return;
    FIL f;
    UINT n;
    f_mkdir("/pico-amiga");
    if (f_open(&f, CD32_NVRAM, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return;
    f_write(&f, s_nvram, 1024, &n);
    f_close(&f);
}

// The disc: the TOC and one open track file on the heap while it is in (a sheet
// with a file a track has one open at a time).  Akiko asks for a sector 75 or
// 150 times a second (cd_sector, from the emulation).
// Reading ahead: a sector asked for is read with the ones after it in one go
// (CD_RUN: 16 KB of a 2048-byte track - one multi-block command, ~5 ms at the
// card's 30 MHz) into PSRAM and the next ones come from there.  One command a
// sector cost ~1.5-2.5 ms each, three a frame at double speed: Cannon Fodder's
// loading fell to 40 fps (the PERF line's `oth` 8 ms, 2026-11-10).  The track
// file has FatFs's cluster map (fast seek), as the HDF: a seek back does not
// walk the FAT chain from the file's start.
#define CD_RUN 8
struct CdDisc {
    cd_toc_t toc;
    FIL f;
    DWORD map[64];
    int open;                           // the file f is, -1 = none
    uint32_t c_lsn, c_n, c_ss;          // the sectors in the read-ahead buffer, their stored size
    int c_mode;
    std::string dir;
    std::vector<std::string> names;     // the sheet's FILEs
};
static CdDisc *s_cd;
static std::string s_cd_path;

static uint32_t cd_file_cb(void *ctx, int index, const char *name) {
    CdDisc *d = (CdDisc *)ctx;
    std::string n = name;
    const size_t cut = n.find_last_of("/\\");             // a sheet written elsewhere carries its paths
    if (cut != std::string::npos) n = n.substr(cut + 1);
    FILINFO fi;
    if (f_stat((d->dir + n).c_str(), &fi) != FR_OK) return 0;
    if ((int)d->names.size() <= index) d->names.resize(index + 1);
    d->names[index] = n;
    return (uint32_t)fi.fsize;
}
static bool cd_read_cb(void *ctx, int index, uint32_t off, void *dst, uint32_t len) {
    CdDisc *d = (CdDisc *)ctx;
    if (d->open != index) {
        if (d->open >= 0) f_close(&d->f);
        d->open = -1;
        if (index >= (int)d->names.size() || f_open(&d->f, (d->dir + d->names[index]).c_str(), FA_READ) != FR_OK) return false;
        d->open = index;
        d->f.cltbl = d->map;                         // fast seek, or the plain one
        d->map[0] = sizeof d->map / sizeof d->map[0];
        if (f_lseek(&d->f, CREATE_LINKMAP) != FR_OK) d->f.cltbl = NULL;
    }
    UINT n = 0;
    return f_lseek(&d->f, off) == FR_OK && f_read(&d->f, dst, len, &n) == FR_OK && n == len;
}
// The sector as its file holds it, from the read-ahead buffer (filled from lsn on
// when it is not there); NULL: in no track, unreadable, or no buffer.
static const uint8_t *cd_cached(CdDisc *d, uint32_t lsn) {
    uint8_t *buf = s_dms_area && s_dms_size + CD_AHEAD <= s_dms_total - s_fast_size ? s_dms_area + s_dms_total - CD_AHEAD : NULL;
    if (!buf) return NULL;
    if (lsn < d->c_lsn || lsn >= d->c_lsn + d->c_n) {
        int file, mode;
        uint32_t off, ss, run;
        d->c_n = 0;
        if (!cd_locate(&d->toc, lsn, &file, &off, &ss, &mode, &run)) return NULL;
        uint32_t n = run < CD_RUN ? run : CD_RUN;
        if (n * ss > CD_AHEAD) n = CD_AHEAD / ss;
        if (!cd_read_cb(d, file, off, buf, n * ss)) return NULL;
        d->c_lsn = lsn; d->c_n = n; d->c_ss = ss; d->c_mode = mode;
    }
    return buf + (lsn - d->c_lsn) * d->c_ss;
}
static int cd_sector(void *, uint32_t lsn, uint8_t *dst) {
    CdDisc *d = s_cd;
    if (!d) return -1;
    const uint8_t *p = cd_cached(d, lsn);
    if (!p) return d->c_n ? -1 : cd_read_sector(&d->toc, lsn, dst, cd_read_cb, d);   // no buffer: a sector a read
    if (!d->c_mode) return -1;
    cd_unpack_sector(p, d->c_ss, dst);
    return d->c_mode;
}
// CD audio (akiko_audio): the sector's samples where they are in the buffer.
static const uint8_t *cd_audio(void *, uint32_t lsn) {
    CdDisc *d = s_cd;
    if (!d) return NULL;
    const uint8_t *p = cd_cached(d, lsn);
    return p && !d->c_mode && d->c_ss == 2352 ? p : NULL;
}
static void cd_track(void *, int n, uint32_t *lsn, uint8_t *ctrl) {
    if (!s_cd || n < 1 || n > s_cd->toc.tracks) return;
    *lsn = s_cd->toc.t[n - 1].lsn;
    *ctrl = s_cd->toc.t[n - 1].mode ? 4 : 0;
}

bool machine_set_cd(const char *path) {
    amiga_set_cd(NULL);
    if (s_cd) {
        if (s_cd->open >= 0) f_close(&s_cd->f);
        delete s_cd;
        s_cd = NULL;
    }
    s_cd_path.clear();
    if (!path || !*path) return true;
    CdDisc *d = new (std::nothrow) CdDisc;
    if (!d) return false;
    d->open = -1;
    d->c_n = 0;
    const std::string p = path;
    d->dir = p.substr(0, p.find_last_of('/') + 1);
    const size_t dot = p.find_last_of('.');
    std::string ext = dot == std::string::npos ? "" : p.substr(dot);
    for (char &c : ext) c = (char)tolower((unsigned char)c);
    bool ok = false;
    FILINFO fi;
    if (ext == ".cue") {
        if (f_stat(path, &fi) == FR_OK && fi.fsize && fi.fsize <= 16384) {
            char *text = (char *)malloc((size_t)fi.fsize);
            if (text) {
                if (load_file(path, (uint8_t *)text, (uint32_t)fi.fsize) == fi.fsize)
                    ok = cd_parse_cue(text, (uint32_t)fi.fsize, &d->toc, cd_file_cb, d);
                free(text);
            }
        }
    } else if (f_stat(path, &fi) == FR_OK) {
        d->names.push_back(p.substr(p.find_last_of('/') + 1));
        ok = cd_from_iso((uint32_t)fi.fsize, &d->toc);
    }
    if (!ok) { delete d; return false; }
    s_cd = d;
    s_cd_path = path;
    printf("cd: %s, %d track%s, %u MB\n", path, d->toc.tracks, d->toc.tracks == 1 ? "" : "s",
           (unsigned)(d->toc.leadout / 512));
    const amiga_cd_t cd = { cd_sector, cd_track, cd_audio, NULL, (uint8_t)d->toc.tracks, d->toc.leadout };
    amiga_set_cd(&cd);
    return true;
}
const char *machine_cd(void) { return s_cd_path.c_str(); }

// ---- the A600's hard disk --------------------------------------------------------
// The HDF stays open while it is in; reads and writes go a sector at a time
// (ide.c), through FatFs's fast seek (a cluster map: a large image's FAT chain
// is not walked per sector) when the map fits.  Each write is synced, as the
// floppies' are.
// The FIL and its cluster map come from the heap while an HDF is open only
// (pico-speccy's rule for IDE): ~870 bytes of SRAM nobody else can use otherwise.
struct HdfFile { FIL f; DWORD map[64]; };
// A window of the card's own sectors (raw_read's ctx).
struct RawWin { uint32_t start, count; };
// The two drives of the port: [0] the master, [1] the slave.
struct Hd {
    HdfFile *file;
    bool open, ro;
    std::string path;                   // the file, or the SD window's name
    bool raw;                           // the SD window (machine_set_hdf_raw)
    RawWin win;
    const char *kind;
};
static Hd s_hd[2] = { { NULL, false, false, "", false, { 0, 0 }, "" }, { NULL, false, false, "", false, { 0, 0 }, "" } };

static int raw_read(void *ctx, uint32_t off, void *dst, uint32_t len) {
    const RawWin *w = (const RawWin *)ctx;
    const uint32_t s = off / 512, n = len / 512;
    if ((off | len) & 511 || s + n > w->count) return -1;
    return disk_read(0, (BYTE *)dst, (LBA_t)(w->start + s), n) == RES_OK ? 0 : -1;
}
static int raw_write(void *ctx, uint32_t off, const void *src, uint32_t len) {
    const RawWin *w = (const RawWin *)ctx;
    const uint32_t s = off / 512, n = len / 512;
    if ((off | len) & 511 || s + n > w->count) return -1;
    if (disk_write(0, (const BYTE *)src, (LBA_t)(w->start + s), n) != RES_OK) return -1;
    for (uint32_t i = 0; i < n; i++)                  // FatFs's cached copy, if it has one
        ff_raw_write_notify(0, (LBA_t)(w->start + s + i), (const BYTE *)src + i * 512);
    return 0;
}

static int hdf_read(void *ctx, uint32_t off, void *dst, uint32_t len) {
    FIL *f = (FIL *)ctx; UINT n;
    if (f_lseek(f, off) != FR_OK || f_read(f, dst, len, &n) != FR_OK || n != len) return -1;
    return 0;
}
static int hdf_write(void *ctx, uint32_t off, const void *src, uint32_t len) {
    FIL *f = (FIL *)ctx; UINT n;
    if (f_lseek(f, off) != FR_OK || f_write(f, src, len, &n) != FR_OK || n != len) return -1;
    f_sync(f);
    return 0;
}

// The drive the core gets: the HDF on the A600, nothing elsewhere.
// What a hard disk holds, as Kickstart will see it: an RDB in blocks 0-15 (its
// partitions mount), one bare Amiga partition (ide.c puts a virtual RDB in
// front), or nothing it can mount - a FAT volume, an empty or unknown one: the
// drive is there, but Workbench shows no icon until it is partitioned (HDToolBox)
// or an HDF is written onto it.
static const char *disk_kind(int (*rd)(void *, uint32_t, void *, uint32_t), void *ctx, uint32_t blocks) {
    uint8_t *b = (uint8_t *)malloc(512);
    if (!b) return "?";
    const char *k = "empty";
    for (uint32_t n = 0; n < 16 && n < blocks; n++) {
        if (rd(ctx, n * 512u, b, 512) != 0) { k = "unreadable"; break; }
        if (!memcmp(b, "RDSK", 4)) { k = "RDB"; break; }
        if (n == 0) {
            if (!memcmp(b, "DOS", 3)) { k = "Amiga partition"; break; }
            if (b[510] == 0x55 && b[511] == 0xAA &&
                (!memcmp(b + 54, "FAT", 3) || !memcmp(b + 82, "FAT", 3) || !memcmp(b + 3, "EXFAT", 5))) { k = "FAT, not Amiga"; break; }
            if (b[510] == 0x55 && b[511] == 0xAA) { k = "a PC MBR, not Amiga"; break; }
            if (!memcmp(b, "PFS", 3) || !memcmp(b, "SFS", 3)) { k = "PFS/SFS without an RDB"; break; }
        }
        for (int i = 0; i < 512; i++) if (b[i]) { k = "no RDB, unknown"; break; }
    }
    free(b);
    return k;
}

// The RDB's partitions, for the log: name, cylinders, size, and whether one ends
// past the image (HDToolBox partitions to the drive's last cylinder: a drive
// that once reported more than it holds left a partition hanging off the end).
static uint32_t be32p(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void hdf_log_parts(int (*rd)(void *, uint32_t, void *, uint32_t), void *ctx, uint32_t blocks) {
    uint8_t *b = (uint8_t *)malloc(512);
    if (!b) return;
    uint32_t part = 0xFFFFFFFF;
    for (uint32_t n = 0; n < 16 && n < blocks; n++)
        if (rd(ctx, n * 512u, b, 512) == 0 && !memcmp(b, "RDSK", 4)) {
            printf("hard disk: RDB at block %u, its geometry %u x %u x %u\n", (unsigned)n,
                   (unsigned)be32p(b + 64), (unsigned)be32p(b + 72), (unsigned)be32p(b + 68));
            part = be32p(b + 28);
            break;
        }
    for (int i = 0; i < 16 && part < blocks; i++) {
        if (rd(ctx, part * 512u, b, 512) != 0 || memcmp(b, "PART", 4)) break;
        const uint8_t *e = b + 128;
        const uint32_t per = be32p(e + 12) * be32p(e + 20), lo = be32p(e + 36), hi = be32p(e + 40);
        const uint32_t end = (hi + 1) * per;            // first block after it
        char name[32];
        const unsigned nl = b[36] < 31 ? b[36] : 31;
        memcpy(name, b + 37, nl); name[nl] = 0;
        printf("hard disk:   %s: cylinders %u-%u (%u x %u), %u MB, DOS type %08X%s", name, (unsigned)lo,
               (unsigned)hi, (unsigned)be32p(e + 12), (unsigned)be32p(e + 20),
               (unsigned)((hi - lo + 1) * (per / 2) / 1024), (unsigned)be32p(e + 64),
               (be32p(b + 20) & 1) ? ", bootable" : "");
        if (end > blocks) printf(" - ENDS %u BLOCKS PAST THE DISK", (unsigned)(end - blocks));
        printf("\n");
        part = be32p(b + 16);
    }
    free(b);
}

static void hdf_attach(int u) {
    Hd &h = s_hd[u];
    if ((Config::machine != MACHINE_A600 && Config::machine != MACHINE_A1200) || (!h.open && !h.raw)) {
        amiga_set_hdf(u, NULL);
        return;
    }
    const amiga_disk_t d = h.raw
        ? amiga_disk_t{ raw_read, h.ro ? NULL : raw_write, &h.win, h.win.count * 512u }
        : amiga_disk_t{ hdf_read, h.ro ? NULL : hdf_write, &h.file->f, (uint32_t)f_size(&h.file->f) };
    h.kind = disk_kind(d.read, d.ctx, d.size / 512);
    printf("hard disk %d: %s, %u MB%s, %s", u, h.path.c_str(), (unsigned)(d.size >> 20),
           h.ro ? ", read-only" : "", h.kind);
    if (h.raw) printf(" (card sectors %u+%u)", (unsigned)h.win.start, (unsigned)h.win.count);
    printf("\n");
    amiga_set_hdf(u, &d);
    uint16_t c; uint8_t hh, sp;
    amiga_hdf_geometry(u, &c, &hh, &sp);
    printf("hard disk: IDENTIFY %u x %u x %u = %u of %u blocks\n", c, hh, sp,
           (unsigned)((uint32_t)c * hh * sp), (unsigned)(d.size / 512));
    hdf_log_parts(d.read, d.ctx, d.size / 512);
}
const char *machine_hdf_kind(int u) { return s_hd[u & 1].kind; }

static void hdf_close(int u) {
    Hd &h = s_hd[u];
    if (h.open) { f_close(&h.file->f); h.open = false; }
    free(h.file);
    h.file = NULL;
    h.raw = false;
    h.ro = false;
    h.path.clear();
    h.kind = "";
}

bool machine_set_hdf(int u, const char *path) {
    u &= 1;
    hdf_close(u);
    Hd &h = s_hd[u];
    bool ok = true;
    if (path && *path) {
        if (s_hd[u ^ 1].open && s_hd[u ^ 1].path == path) { hdf_attach(u); return false; }   // one file, one drive
        h.file = (HdfFile *)malloc(sizeof *h.file);
        if (!h.file) { hdf_attach(u); return false; }
        FIL *f = &h.file->f;
        FRESULT r = f_open(f, path, FA_READ | FA_WRITE);
        if (r == FR_DENIED || r == FR_WRITE_PROTECTED) { h.ro = true; r = f_open(f, path, FA_READ); }
        ok = r == FR_OK && f_size(f) >= 1024 && f_size(f) <= 0xFFFFFE00u;
        if (r == FR_OK && !ok) f_close(f);
        if (ok) {
            h.open = true;
            h.path = path;
            f->cltbl = h.file->map;                  // fast seek, or the plain one
            h.file->map[0] = sizeof h.file->map / sizeof h.file->map[0];
            if (f_lseek(f, CREATE_LINKMAP) != FR_OK) f->cltbl = NULL;
        } else {
            free(h.file);
            h.file = NULL;
            h.ro = false;
        }
    }
    hdf_attach(u);
    return ok;
}

const char *machine_hdf(int u) { return s_hd[u & 1].path.c_str(); }
bool machine_hdf_is_raw(int u) { return s_hd[u & 1].raw; }
bool machine_hdf_ro(int u) { return s_hd[u & 1].ro; }

// The sector FatFs mounted the card's files from (0 when it cannot tell).
static uint32_t files_base(void) {
    DWORD n;
    FATFS *fs = NULL;
    if (f_getfree("SD:", &n, &fs) != FR_OK || !fs) return 0;
    return (uint32_t)fs->volbase;
}

int machine_sd_parts(MachinePart out[8]) {
    for (int i = 0; i < 8; i++) out[i] = MachinePart{ (uint8_t)(i + 1), 0, 0, 0, false, "" };
    LBA_t card = 0;
    disk_ioctl(0, GET_SECTOR_COUNT, &card);
    uint8_t *sec = (uint8_t *)malloc(512);               // the heap, not .bss (pico-speccy's rule)
    if (!sec) return -1;
    if (disk_read(0, sec, 0, 1) != RES_OK) { free(sec); return -1; }
    ide_mbr::Part p[ide_mbr::MAX_ALL];
    for (int i = 0; i < ide_mbr::MAX_ALL; i++) p[i] = ide_mbr::Part{ 0, 0, 0 };
    int n = ide_mbr::parse(sec, (uint32_t)card, p);
    uint32_t ext_lba = 0, ext_n = 0;
    if (ide_mbr::extended(sec, (uint32_t)card, &ext_lba, &ext_n)) {
        uint32_t ebr = ext_lba;
        for (int i = 0; i < ide_mbr::MAX_LOGICAL && ebr; i++) {
            if (disk_read(0, sec, (LBA_t)ebr, 1) != RES_OK) break;
            ebr = ide_mbr::parseEbr(sec, ebr, ext_lba, ext_n, &p[ide_mbr::MAX_PARTS + i]);
            if (p[ide_mbr::MAX_PARTS + i].sectors) n++;
        }
    }
    free(sec);
    const uint32_t fb = files_base();
    for (int i = 0; i < 8; i++) {
        out[i].type = p[i].type;
        out[i].lba = p[i].lba;
        out[i].sectors = p[i].sectors;
        out[i].files = fb && p[i].sectors && p[i].lba == fb;
        out[i].kind = "";
        if (p[i].sectors) {
            RawWin w = { p[i].lba, p[i].sectors };                  // raw_read's window, for the look
            out[i].kind = disk_kind(raw_read, &w, p[i].sectors);
        }
    }
    return n;
}

bool machine_set_hdf_raw(int u, int part, uint32_t lba, uint32_t len, bool writable) {
    u &= 1;
    hdf_close(u);
    Hd &h = s_hd[u];
    LBA_t card = 0;
    bool ok = disk_ioctl(0, GET_SECTOR_COUNT, &card) == RES_OK && card > lba;
    if (ok) {
        const uint32_t avail = (uint32_t)(card - lba);
        if (!len || len > avail) len = avail;
        if (len > 0xFFFFFE00u / 512) len = 0xFFFFFE00u / 512;    // amiga_disk_t: byte offsets
        ok = len >= 2;
    }
    if (ok) {
        // Never a writable window over the card's own files, or over the other drive's.
        const uint32_t fb = files_base();
        MachinePart parts[8];
        bool over = fb >= lba && fb < lba + len;
        if (!over && machine_sd_parts(parts) > 0)
            for (const MachinePart &q : parts)
                if (q.files && q.lba < lba + len && lba < q.lba + q.sectors) over = true;
        const Hd &o = s_hd[u ^ 1];
        if (o.raw && o.win.start < lba + len && lba < o.win.start + o.win.count) over = true;
        h.raw = true;
        h.win = RawWin{ lba, len };
        h.ro = !writable || over;
        char name[32];
        if (part > 0) snprintf(name, sizeof name, "SD part %d", part);
        else snprintf(name, sizeof name, "SD sectors %u+", (unsigned)lba);
        h.path = name;
    }
    hdf_attach(u);
    return ok;
}

bool machine_apply_kickstart(void) {
    amiga_config_t c;
    memset(&c, 0, sizeof c);
    if (!machine_select_rom(&c)) return false;
    amiga_set_roms(c.rom, c.rom_size, c.ext_rom, c.ext_size);
    amiga_set_machine(c.chipset, c.chip_size, c.slow_size, c.gayle);
    amiga_set_fast(c.fast_ram, c.fast_size);
    amiga_set_cpu(c.cpu);
    amiga_set_akiko(c.akiko, c.nvram);
    board_load_cpu_overlay(c.cpu);
    settings_apply_chip_bus();
    hdf_attach(0);
    hdf_attach(1);
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
    bool ro;                // the Amiga cannot write it: protected, a DMS, a read-only file
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
    d.ro = false;
}

// The disk set (machine_load_set); s_set_index is the disk in DF0.
static std::string s_set[16];
static int s_set_count, s_set_index;
static int s_set_drives;                  // the set's "#DRIVES:n" line, 0 = none

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

// The fast RAM grew over a DMS slot: that drive is emptied (its image is gone).
static void drop_lost_dms(void) {
    for (int d = 0; d < 4; d++)
        if (s_drv[d].img && s_drv[d].img != dms_slot(d)) {
            printf("DF%d: its DMS slot went to the fast RAM, ejected\n", d);
            machine_eject(d);
        }
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
    d.ro = true;
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
    d.ro = !rw;
    amiga_disk_t disk = { adf_read, rw ? adf_write : NULL, &d.f, (uint32_t)f_size(&d.f) };
    amiga_insert_disk(drive, &disk);
    return true;
}

const char *machine_disk(int drive) {
    return (drive >= 0 && drive <= 3) ? s_drv[drive].path.c_str() : "";
}

bool machine_disk_ro(int drive) {
    return drive >= 0 && drive <= 3 && !s_drv[drive].path.empty() && s_drv[drive].ro;
}
bool machine_disk_dms(int drive) {
    return drive >= 0 && drive <= 3 && s_drv[drive].img;
}

bool machine_write_protect(int drive, bool on) {
    if (drive < 0 || drive > 3 || s_drv[drive].path.empty()) return false;
    const std::string path = s_drv[drive].path;
    const bool manual = s_drv[drive].manual;
    if (!insert_adf(drive, path.c_str(), on)) return false;
    s_drv[drive].manual = manual;
    return true;
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
    int drives = 0;
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
        if (s == std::string::npos) continue;
        l = l.substr(s);
        if (l[0] == '#') {                                  // #SAVEDISK: etc. skipped
            // Ours: "#DRIVES:n", the drives Floppy drives Auto connects (Gobliiins
            // reads DF0 only and hangs short of memory with a volume in each of 3).
            if (l.size() > 8 && !strncasecmp(l.c_str(), "#DRIVES:", 8) && l[8] >= '1' && l[8] <= '4')
                drives = l[8] - '0';
            continue;
        }
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
    s_set_drives = drives;
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
    // Where: beside the set's .m3u, else beside DF0's disk, else /pico-amiga/disks/.
    std::string dir = s_set_count ? Config::df0 : s_drv[0].path;
    dir = dir.substr(0, dir.find_last_of('/') + 1);
    if (dir.empty()) { dir = "/pico-amiga/disks/"; if (!file_exists("/pico-amiga/disks")) dir = "/pico-amiga/"; }
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
// A header block's name (BCPL string, at most 30 characters).
static void bname(const uint8_t *b, char out[31]) {
    const int n = b[0x1B0] < 30 ? b[0x1B0] : 30;
    memcpy(out, b + 0x1B1, (size_t)n);
    out[n] = 0;
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

// The text goes straight into the caller's buffer, cut where it is full.
struct InfoOut {
    char *p; unsigned size, len;
    void add(const char *t) {
        while (*t && len + 1 < size) p[len++] = *t++;
        p[len] = 0;
    }
};

// Its work memory (two blocks, the root's entries, the startup-sequence) is the
// caller's, not the heap: the menu has a few KB of it and a DF0 info died there.
struct InfoEnt { char name[31]; bool dir; uint32_t size; };
struct InfoWork {
    uint8_t b0[512], b1[512];
    InfoEnt ents[40];
    char text[1024];
};

static int ent_cmp(const void *a, const void *b) {
    const InfoEnt *x = (const InfoEnt *)a, *y = (const InfoEnt *)b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;           // directories first
    return strcasecmp(x->name, y->name);
}

static void adf_info(blk_fn rd, void *ctx, InfoWork *w, InfoOut &o) {
    uint8_t *b0 = w->b0, *b1 = w->b1;
    char line[96], name[31];
    // The boot block is blocks 0-1: their longs summed with carry give ~0 when it boots.
    if (!rd(ctx, 0, b0) || !rd(ctx, 1, b1)) { o.add("The image cannot be read.\n"); return; }
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
        o.add(line);
        return;
    }
    static const char *const fs[] = { "OFS", "FFS", "OFS intl", "FFS intl", "OFS dircache", "FFS dircache", "OFS long names", "FFS long names" };
    const bool ffs = dostype & 1;
    snprintf(line, sizeof line, "Format   : AmigaDOS %s (DOS\\%u)%s\n", fs[dostype], dostype, boots ? ", bootable" : "");
    o.add(line);
    if (!rd(ctx, 880, b0) || rd32(b0) != 2 || rd32(b0 + 0x1FC) != 1 || !sum_ok(b0)) {
        o.add(boots ? "Files    : none - the boot block loads the rest itself\n"
                    : "Root     : not valid (an unformatted or damaged disk)\n");
        return;
    }
    char date[24];
    amiga_date(rd32(b0 + 0x1E4), date, sizeof date);
    bname(b0, name);
    snprintf(line, sizeof line, "Volume   : %s\nCreated  : %s\n", name, date);
    o.add(line);
    // Free space: the bitmap's set bits for blocks 2..1759.
    const uint32_t bm = rd32(b0 + 0x13C);
    if (bm && rd(ctx, bm, b1)) {
        int nfree = 0;
        for (int b = 2; b < 1760; b++) nfree += (rd32(b1 + 4 + (b - 2) / 32 * 4) >> ((b - 2) % 32)) & 1;
        snprintf(line, sizeof line, "Used     : %d of 1758 blocks, %d KB free\n", 1758 - nfree, nfree * (ffs ? 512 : 488) / 1024);
        o.add(line);
    }
    // The root directory, sorted, directories first.
    const int max_ents = (int)(sizeof w->ents / sizeof w->ents[0]);
    int nent = 0;
    uint32_t s_dir = 0;
    for (int i = 0; i < 72 && nent < max_ents; i++) {
        for (uint32_t h = rd32(b0 + 24 + 4 * i); h && h < 1760 && nent < max_ents; h = rd32(b1 + 0x1F0)) {
            if (!rd(ctx, h, b1)) break;
            InfoEnt &e = w->ents[nent++];
            bname(b1, e.name);
            e.dir = (int32_t)rd32(b1 + 0x1FC) == 2;
            e.size = rd32(b1 + 0x144);
            if (e.dir && !strcasecmp(e.name, "s")) s_dir = h;
        }
    }
    qsort(w->ents, (size_t)nent, sizeof w->ents[0], ent_cmp);
    o.add("\nRoot:\n");
    for (int k = 0; k < nent; k++) {
        const InfoEnt &e = w->ents[k];
        if (e.dir) snprintf(line, sizeof line, "  %s/\n", e.name);
        else       snprintf(line, sizeof line, "  %-30s %7lu\n", e.name, (unsigned long)e.size);
        o.add(line);
    }
    if (nent >= max_ents) o.add("  ...\n");
    // The start of s/startup-sequence: what the disk runs.
    if (!s_dir || !rd(ctx, s_dir, b0)) return;
    uint32_t ss = 0;
    for (int i = 0; i < 72 && !ss; i++)
        for (uint32_t h = rd32(b0 + 24 + 4 * i); h && h < 1760; h = rd32(b1 + 0x1F0)) {
            if (!rd(ctx, h, b1)) break;
            bname(b1, name);
            if (!strcasecmp(name, "startup-sequence")) { ss = h; break; }
        }
    if (!ss || !rd(ctx, ss, b0)) return;
    o.add("\ns/startup-sequence:\n");
    const uint32_t size = rd32(b0 + 0x144), nblk = rd32(b0 + 8);
    unsigned tlen = 0;
    const unsigned tmax = sizeof w->text;
    for (uint32_t k = 0; k < nblk && k < 72 && tlen < 900; k++) {
        if (!rd(ctx, rd32(b0 + 24 + 4 * (71 - k)), b1)) break;
        const uint8_t *src = ffs ? b1 : b1 + 24;
        unsigned n = ffs ? 512 : (rd32(b1 + 12) <= 488 ? rd32(b1 + 12) : 488);
        if (n > tmax - tlen) n = tmax - tlen;
        memcpy(w->text + tlen, src, n);
        tlen += n;
    }
    if (tlen > size) tlen = size;
    int lines = 0;
    for (unsigned p = 0; p < tlen && lines < 16; lines++) {
        unsigned e = p;
        while (e < tlen && w->text[e] != '\n') e++;
        unsigned n = e - p > 44 ? 44 : e - p;
        char l[48];
        for (unsigned k = 0; k < n; k++) {
            const unsigned char c = (unsigned char)w->text[p + k];
            l[k] = c < 32 || c > 126 ? ' ' : (char)c;
        }
        l[n] = 0;
        snprintf(line, sizeof line, "  %s\n", l);
        o.add(line);
        p = e + 1;
    }
}

void machine_disk_info(const char *path, char *out, unsigned size) {
    InfoOut o = { out, size, 0 };
    if (size) out[0] = 0;
    FIL &f = s_job;
    if (has_ext(path, ".dms")) {
        // Unpacked into the slot after DF3's, where the PSRAM has one; the work
        // memory is then the unpacker's area, free again.
        uint8_t *img = dms_slot(4);
        if (!img) { o.add("A DMS archive: no spare PSRAM to unpack it for a look.\n"); return; }
        if (f_open(&f, path, FA_READ) != FR_OK) { o.add("The file cannot be opened.\n"); return; }
        const int err = dms_unpack(dms_fread, &f, img, s_dms_area);
        f_close(&f);
        if (err != DMS_OK) { o.add("DMS: "); o.add(dms_error(err)); o.add("\n"); return; }
        o.add("A DMS archive, unpacked:\n");
        adf_info(mem_blk, img, (InfoWork *)s_dms_area, o);
        return;
    }
    // An ADF: the work memory in PSRAM too (the unpacker's area, idle now), the
    // heap only on a board without one.
    InfoWork *w = s_dms_area && s_dms_size >= sizeof(InfoWork) ? (InfoWork *)s_dms_area
                                                               : (InfoWork *)malloc(sizeof(InfoWork));
    if (!w) { o.add("Out of memory.\n"); return; }
    if (f_open(&f, path, FA_READ) != FR_OK) o.add("The file cannot be opened.\n");
    else {
        if (f_size(&f) != DMS_ADF_SIZE) o.add("Not an 880 KB ADF.\n");
        else adf_info(file_blk, &f, w, o);
        f_close(&f);
    }
    if ((uint8_t *)w != s_dms_area) free(w);
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

// The drives Config::drives connects: Auto is one per disk of the set (or its
// #DRIVES line), up to 4.
static int drives_wanted(void) {
    int n = Config::drives;
    if (!n && s_set_count && s_set_drives) n = s_set_drives;                 // Auto, the set's #DRIVES
    if (!n) n = s_set_count > 1 ? (s_set_count < 4 ? s_set_count : 4) : 1;   // Auto
    return n;
}

bool machine_apply_drives(void) {
    const int n = drives_wanted();
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

// ---- snapshots (snapshot.cpp) ----------------------------------------------------------
void machine_get_disks(MachineDisks &d) {
    for (int i = 0; i < 4; i++) {
        d.path[i] = s_drv[i].path;
        d.wp[i] = s_drv[i].ro;
        d.manual[i] = s_drv[i].manual;
    }
    d.set = s_set_count ? Config::df0 : std::string();
    d.set_index = s_set_index;
}

void machine_put_disks(const MachineDisks &d, std::string &missing) {
    for (int i = 0; i < 4; i++) eject_adf(i);
    s_set_count = 0;
    if (!d.set.empty() && !machine_load_set(d.set.c_str(), d.set_index)) {
        missing += missing.empty() ? "" : ", ";
        missing += d.set.substr(d.set.find_last_of('/') + 1);
    }
    for (int i = 0; i < 4; i++) {
        if (d.path[i].empty()) continue;
        if (s_drv[i].path != d.path[i] && !insert_adf(i, d.path[i].c_str(), d.wp[i])) {
            missing += missing.empty() ? "" : ", ";
            missing += d.path[i].substr(d.path[i].find_last_of('/') + 1);
            continue;
        }
        s_drv[i].manual = d.manual[i];
    }
    s_drives = drives_wanted();
    amiga_set_drives(s_drives);
}

// A file's CRC-32 when it is `size` bytes (or either of two sizes), read through
// a small buffer.  False: no such file, another size.
static bool file_crc(const char *path, uint32_t size_a, uint32_t size_b, uint32_t *crc) {
    FIL &f = s_job;
    if (f_open(&f, path, FA_READ) != FR_OK) return false;
    const FSIZE_t size = f_size(&f);
    uint8_t *buf = (size == size_a || size == size_b) ? (uint8_t *)malloc(512) : nullptr;
    bool ok = buf != nullptr;
    uint32_t c = 0;
    for (FSIZE_t at = 0; ok && at < size; at += 512) {
        UINT n = 0;
        ok = f_read(&f, buf, 512, &n) == FR_OK && n == 512;
        c = amiga_crc32(c, buf, n);
    }
    free(buf);
    f_close(&f);
    *crc = c;
    return ok;
}

// The choice machine_select_rom makes, for a kick / kick_file pair.
bool machine_rom_crcs(const std::string &kick, const std::string &file, uint32_t *rom, uint32_t *ext) {
    *rom = *ext = 0;
    if (kick == "sd" && file_crc(file.c_str(), 256 * KB, 512 * KB, rom)) {
        const std::string dir = file.substr(0, file.find_last_of('/') + 1);
        if (!file_crc((dir + "ext.rom").c_str(), 512 * KB, 512 * KB, ext)) *ext = 0;
        return true;
    }
    const builtin_rom_t *r = builtin_rom_find(kick.c_str());
    if (!r && builtin_rom_count) r = &builtin_roms[0];
    if (!r) return false;
    *rom = amiga_crc32(0, r->data, r->size);
    return true;
}

std::string machine_rom_label(const std::string &kick, const std::string &file) {
    if (kick == "sd") return file.substr(file.find_last_of('/') + 1);
    const builtin_rom_t *r = builtin_rom_find(kick.c_str());
    return r ? r->label : kick;
}

void machine_restore_disks(void) {
    for (int u = 0; u < 2; u++) {
        std::string &img = u ? Config::hdf2 : Config::hdf, &raw = u ? Config::hdf2_raw : Config::hdf_raw;
        if (!raw.empty()) {
            unsigned mode = 0, part = 0, lba = 0, len = 0;
            if (sscanf(raw.c_str(), "%u:%u:%u:%u", &mode, &part, &lba, &len) != 4 || !mode ||
                !machine_set_hdf_raw(u, (int)part, lba, len, mode == 2))
                raw.clear();
        } else if (!img.empty() && !machine_set_hdf(u, img.c_str())) {
            img.clear();
        }
    }
    if (!Config::cd.empty() && !machine_set_cd(Config::cd.c_str())) Config::cd.clear();
    if (!Config::df0.empty()) {
        const bool ok = is_m3u(Config::df0)
            ? machine_load_set(Config::df0.c_str(), Config::df0_disk)
            : machine_insert(0, Config::df0.c_str(), Config::df0_wp);
        if (!ok) Config::df0.clear();
        machine_apply_drives();
        return;
    }
    // The card layout before the menu existed: /pico-amiga/df0.adf goes in by itself.
    machine_insert(0, "/pico-amiga/df0.adf", false);
}

// ---- LHA archives: WHDLoad installs as the hard disk (whdboot.c) -------------------------
// The volume is built into /pico-amiga/whdload/images/<name>.hdf with the DMS area as
// the builder's arena (the floppies are ejected first: DF0 would boot before the
// hard disk, and their DMS slots are the arena).  Block 0 after "DOS\1" carries
// a stamp: the archive's size and date, so an unchanged archive boots its image
// again (the game's saves are in it), and what the build found out about it.
#define WHD_DIR     "/pico-amiga/whdload"
#define WHD_IMAGES  WHD_DIR "/images"
#define WHD_CARD_MAX 64

struct WhdStamp {               // big-endian on the card, at byte 16 of block 0
    char     magic[4];          // "PAWH"
    uint8_t  version;           // the builder's (a new one rebuilds the images)
    uint8_t  pad[3];
    uint32_t lha_size, lha_date;// fdate << 16 | ftime
    uint32_t base_mem, exp_mem;
    char     missing[24];       // the Kickstarts the slave named that were not there
};
#define WHD_STAMP_VERSION 1

struct WhdCard { char path[96]; char full[128]; char tag[24]; };   // a file of the card's /pico-amiga/whdload/
#define WHD_EXTRA_MAX (WHD_CARD_MAX + 48 + 16)
#define WHD_WALK_DEPTH 4

// Everything the build keeps, in PSRAM (the DMS area's start): the SRAM heap is
// short while the menu is open, and the SDK's malloc panics rather than fail.
// The SD driver moves data with the CPU (blocking SPI), so FatFs may use PSRAM.
struct WhdBuild {
    const char *lha;
    const char *img;
    char       *msg; unsigned msg_size;
    FIL         in, out, card_f;
    DIR         dirs[WHD_WALK_DEPTH];
    FILINFO     fi;
    WhdCard     card[WHD_CARD_MAX]; int ncard;
    whd_extra_t extra[WHD_EXTRA_MAX]; int nextra;
    whd_job_t   job;
    uint8_t    *arena; uint32_t arena_size;
    bool        ok;
    bool        exe;                 // not an archive: an Amiga executable, run alone
    char        exe_name[32];        // its name on the volume
    char        whd_tags[48][24], rom_tags[16][24], rom_paths[16][48];   // the built-in extras' names
};

static void put_be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t get_be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static void stamp_to(uint8_t *b, const WhdStamp &s) {
    memcpy(b, s.magic, 4); b[4] = s.version;
    put_be32(b + 8, s.lha_size); put_be32(b + 12, s.lha_date);
    put_be32(b + 16, s.base_mem); put_be32(b + 20, s.exp_mem);
    memcpy(b + 24, s.missing, sizeof s.missing);
}
static bool stamp_from(const uint8_t *b, WhdStamp &s) {
    memcpy(s.magic, b, 4); s.version = b[4];
    s.lha_size = get_be32(b + 8); s.lha_date = get_be32(b + 12);
    s.base_mem = get_be32(b + 16); s.exp_mem = get_be32(b + 20);
    memcpy(s.missing, b + 24, sizeof s.missing);
    s.missing[sizeof s.missing - 1] = 0;
    return !memcmp(s.magic, "PAWH", 4) && s.version == WHD_STAMP_VERSION;
}

static uint32_t whd_lha_read(void *ctx, uint32_t off, void *dst, uint32_t len) {
    FIL *f = (FIL *)ctx; UINT n = 0;
    if (f_lseek(f, off) != FR_OK || f_read(f, dst, len, &n) != FR_OK) return 0;
    return n;
}
static int whd_img_write(void *ctx, uint32_t off, const void *src, uint32_t len) {
    FIL *f = (FIL *)ctx; UINT n = 0;
    if (f_lseek(f, off) != FR_OK || f_write(f, src, len, &n) != FR_OK || n != len) return -1;
    return 0;
}
static WhdBuild *s_whd;                  // the build running (its card_f reads the card's files)
static uint32_t whd_card_read(void *ctx, uint32_t off, void *dst, uint32_t len) {
    FIL *f = &s_whd->card_f;
    UINT n = 0;
    if (f_open(f, ((WhdCard *)ctx)->full, FA_READ) == FR_OK) {
        if (f_lseek(f, off) == FR_OK) f_read(f, dst, len, &n);
        f_close(f);
    }
    return n;
}

// A FAT date and time as seconds since 1970.
static uint32_t fat_to_unix(uint16_t date, uint16_t time) {
    static const uint16_t mdays[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    const unsigned y = (date >> 9) + 1980u, m = (date >> 5) & 15u, d = date & 31u;
    if (m < 1 || m > 12 || d < 1) return 0;
    uint32_t days = (y - 1970) * 365 + (y - 1969) / 4 + mdays[m - 1] + d - 1;
    if (m > 2 && y % 4 == 0) days++;
    return days * 86400u + (time >> 11) * 3600u + ((time >> 5) & 63u) * 60u + (time & 31u) * 2u;
}

// The card's /pico-amiga/whdload/ tree (but images/), depth-first.
static void whd_walk(WhdBuild &b, const char *rel, int depth) {
    if (depth >= WHD_WALK_DEPTH) return;
    char dir[160];
    snprintf(dir, sizeof dir, WHD_DIR "%s%s", rel[0] ? "/" : "", rel);
    DIR *d = &b.dirs[depth];
    FILINFO *fi = &b.fi;
    if (f_opendir(d, dir) == FR_OK) {
        while (b.ncard < WHD_CARD_MAX && f_readdir(d, fi) == FR_OK && fi->fname[0]) {
            if (fi->fname[0] == '.' || (fi->fattrib & (AM_HID | AM_SYS))) continue;
            char r[96];
            if (snprintf(r, sizeof r, "%s%s%s", rel, rel[0] ? "/" : "", fi->fname) >= (int)sizeof r) continue;
            if (fi->fattrib & AM_DIR) {
                if (!rel[0] && !strcasecmp(fi->fname, "images")) continue;
                whd_walk(b, r, depth + 1);
                continue;
            }
            WhdCard &c = b.card[b.ncard];
            strcpy(c.path, r);
            snprintf(c.full, sizeof c.full, WHD_DIR "/%s", r);
            whd_kick_tag(c.path, c.tag);
            whd_extra_t &x = b.extra[b.nextra++];
            memset(&x, 0, sizeof x);
            x.path = c.path; x.size = (uint32_t)fi->fsize; x.rd = whd_card_read; x.ctx = &c; x.kick = c.tag;
            b.ncard++;
        }
        f_closedir(d);
    }
}

// The build itself (on its own stack: whd_call_on_stack).
static void whd_build(void *arg) {
    WhdBuild &b = *(WhdBuild *)arg;
    // the extra files: the card's first (they win), the built-in WHDLoad files,
    // the built-in Kickstarts under WHDLoad's names
    whd_walk(b, "", 0);
    char (*whd_tags)[24] = b.whd_tags, (*rom_tags)[24] = b.rom_tags, (*rom_paths)[48] = b.rom_paths;
    for (unsigned i = 0; i < builtin_whd_count && i < 48 && b.nextra < WHD_EXTRA_MAX; i++) {
        whd_extra_t &x = b.extra[b.nextra++];
        memset(&x, 0, sizeof x);
        x.path = builtin_whd[i].path; x.data = builtin_whd[i].data; x.size = builtin_whd[i].size;
        x.kick = whd_kick_tag(x.path, whd_tags[i]);
    }
    for (unsigned i = 0; i < builtin_rom_count && i < 16; i++) {
        const builtin_rom_t &r = builtin_roms[i];
        if (!r.file || !*whd_kick_tag(r.file, rom_tags[i]) || b.nextra >= WHD_EXTRA_MAX) continue;
        snprintf(rom_paths[i], sizeof rom_paths[i], "Devs/Kickstarts/%s", r.file);
        whd_extra_t &x = b.extra[b.nextra++];
        memset(&x, 0, sizeof x);
        x.path = rom_paths[i]; x.data = r.data; x.size = r.size; x.kick = rom_tags[i];
    }
    // the volume's name: the archive's, without its extension
    char vol[32];
    const char *base = strrchr(b.lha, '/');
    snprintf(vol, sizeof vol, "%s", base ? base + 1 : b.lha);
    char *dot = strrchr(vol, '.');
    if (dot) *dot = 0;
    for (char *c = vol; *c; c++) if (*c == ':' || *c == '/') *c = '_';

    // a lone program: one more extra file, read from the card like the folder's
    if (b.exe && b.ncard < WHD_CARD_MAX && b.nextra < WHD_EXTRA_MAX) {
        WhdCard &c = b.card[b.ncard++];
        snprintf(c.path, sizeof c.path, "%s", b.exe_name);
        snprintf(c.full, sizeof c.full, "%s", b.lha);
        c.tag[0] = 0;
        whd_extra_t &x = b.extra[b.nextra++];
        memset(&x, 0, sizeof x);
        x.path = c.path; x.size = (uint32_t)f_size(&b.in); x.rd = whd_card_read; x.ctx = &c; x.kick = c.tag;
    }
    whd_job_t &j = b.job;
    memset(&j, 0, sizeof j);
    j.rd = b.exe ? nullptr : whd_lha_read; j.rctx = &b.in;
    j.run = b.exe ? b.exe_name : nullptr;
    j.extra = b.extra; j.extra_count = b.nextra;
    j.volume = vol;
    FILINFO fi;
    if (f_stat(b.lha, &fi) != FR_OK) memset(&fi, 0, sizeof fi);
    j.unix_time = fat_to_unix(fi.fdate, fi.ftime);       // the volume's date: the archive's
    int r = whd_plan(&j, b.arena, b.arena_size);
    if (r == WHD_OK) {
        const FSIZE_t size = (FSIZE_t)j.blocks * 512u;
        if (f_open(&b.out, b.img, FA_READ | FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) r = WHD_ERR_WRITE;
        else {
            // the clusters first, one contiguous run if the card has one (fast seek)
            if (f_expand(&b.out, size, 1) != FR_OK && (f_lseek(&b.out, size) != FR_OK || f_tell(&b.out) != size))
                r = WHD_ERR_WRITE;
            if (r == WHD_OK) r = whd_write(&j, b.arena, whd_img_write, &b.out);
            if (r == WHD_OK) {
                uint8_t blk[512];
                memset(blk, 0, sizeof blk);
                memcpy(blk, "DOS\1", 4);
                WhdStamp s;
                memset(&s, 0, sizeof s);
                memcpy(s.magic, "PAWH", 4);
                s.version = WHD_STAMP_VERSION;
                s.lha_size = (uint32_t)fi.fsize;
                s.lha_date = (uint32_t)fi.fdate << 16 | fi.ftime;
                s.base_mem = j.base_mem; s.exp_mem = j.exp_mem;
                snprintf(s.missing, sizeof s.missing, "%.23s", j.missing);
                stamp_to(blk + 16, s);
                if (whd_img_write(&b.out, 0, blk, 512)) r = WHD_ERR_WRITE;
            }
            f_close(&b.out);
            if (r != WHD_OK) f_unlink(b.img);
        }
    }
    printf("whdload: %s -> %s: %s; slave %s (chip %u KB, other %u KB), program %s, kick %s%s%s; %s\n",
           b.lha, b.img, whd_error(r), j.slave[0] ? j.slave : "-", (unsigned)(j.base_mem / 1024),
           (unsigned)(j.exp_mem / 1024), j.program[0] ? j.program : "-", j.kicks[0] ? j.kicks : "-",
           j.missing[0] ? ", missing " : "", j.missing, j.command);
    if (r != WHD_OK) snprintf(b.msg, b.msg_size, "%s", whd_error(r));
    b.ok = r == WHD_OK;
}

// pico-speccy's zipCallOnStack (fs/ZipExtract.cpp): fn(arg) on a heap stack,
// MSPLIM off while SP crosses between the stacks.  The builder (FatFs, the LHA
// decoder, the icon parser) is too deep for core 0's 4 KB stack under the menu.
#if HOST_BUILD
static void whd_call_on_stack(void *new_top, void (*fn)(void *), void *arg, void *new_bottom) {
    (void)new_top; (void)new_bottom; fn(arg);
}
#else
__attribute__((naked, noinline))
static void whd_call_on_stack(void *new_top, void (*fn)(void *), void *arg, void *new_bottom) {
    __asm volatile(
        "mrs  r12, msplim       \n"
        "push {r4}              \n"
        "movs r4, #0            \n"
        "msr  msplim, r4        \n"
        "mov  r4, sp            \n"
        "mov  sp, r0            \n"
        "msr  msplim, r3        \n"
        "push {r2, r4, r12, lr} \n"
        "mov  r0, r2            \n"
        "blx  r1                \n"
        "pop  {r2, r4, r12, lr} \n"
        "movs r1, #0            \n"
        "msr  msplim, r1        \n"
        "mov  sp, r4            \n"
        "msr  msplim, r12       \n"
        "pop  {r4}              \n"
        "bx   lr                \n"
    );
}
#endif

static void whd_mkdirs(const char *path) {
    char p[160];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = strchr(p + 1, '/'); s; s = strchr(s + 1, '/')) { *s = 0; f_mkdir(p); *s = '/'; }
    f_mkdir(p);
}

// The build's stack: SRAM (a stack in PSRAM would share the bus the video's
// reads use), from the heap without the SDK's panic on failure (__wrap_malloc
// panics; __real_malloc returns NULL).  None free: the build runs in place, as
// pico-speccy's ZipExtract does then.
#if HOST_BUILD
static void *whd_try_malloc(size_t n) { return malloc(n); }
#else
extern "C" void *__real_malloc(size_t);
static void *whd_try_malloc(size_t n) { return __real_malloc(n); }
#endif

bool machine_is_program(const char *path) {
    FIL *f = (FIL *)whd_try_malloc(sizeof(FIL));
    uint8_t m[4] = { 0 };
    UINT n = 0;
    if (f && f_open(f, path, FA_READ) == FR_OK) { f_read(f, m, 4, &n); f_close(f); }
    free(f);
    return n == 4 && m[0] == 0 && m[1] == 0 && m[2] == 3 && m[3] == 0xF3;
}

bool machine_boot_lha(const char *path, char *msg, unsigned msg_size) {
    msg[0] = 0;
    if (Config::machine != MACHINE_A600 && Config::machine != MACHINE_A1200) {
        snprintf(msg, msg_size, "An LHA boots on the A600 or A1200 (Machine)");
        return false;
    }
    FILINFO fi;
    if (f_stat(path, &fi) != FR_OK) { snprintf(msg, msg_size, "The archive cannot be opened"); return false; }
    // The floppies out (DF0 boots first; the DMS slots become the builder's PSRAM).
    for (int d = 0; d < 4; d++) machine_eject(d);
    Config::df0.clear();
    machine_apply_drives();
    const uint32_t head = (uint32_t)((sizeof(WhdBuild) + 4095) & ~4095u);
    if (!s_dms_area || s_dms_size < head + WHD_ARENA_MIN) {
        snprintf(msg, msg_size, "Not enough PSRAM to unpack it");
        return false;
    }
    WhdBuild *b = (WhdBuild *)s_dms_area;
    memset(b, 0, sizeof *b);
    s_whd = b;

    // the image's path
    std::string name = path;
    name = name.substr(name.find_last_of('/') + 1);
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot) name.resize(dot);
    const std::string img = std::string(WHD_IMAGES "/") + name + ".hdf";

    // Its image, if the archive has not changed since it was built.
    WhdStamp s;
    memset(&s, 0, sizeof s);
    bool reuse = false;
    if (f_open(&b->in, img.c_str(), FA_READ) == FR_OK) {
        uint8_t blk[64];
        UINT n = 0;
        reuse = f_read(&b->in, blk, sizeof blk, &n) == FR_OK && n == sizeof blk && !memcmp(blk, "DOS\1", 4) &&
                stamp_from(blk + 16, s) && s.lha_size == (uint32_t)fi.fsize &&
                s.lha_date == ((uint32_t)fi.fdate << 16 | fi.ftime);
        f_close(&b->in);
    }

    if (!reuse) {
        machine_set_hdf(0, "");                          // it may be the one in use (and its FIL goes back to the heap)
        Config::hdf.clear();
        if (img == machine_hdf(1)) { machine_set_hdf(1, ""); Config::hdf2.clear(); }   // ...or the slave's
        whd_mkdirs(WHD_IMAGES);
        if (f_open(&b->in, path, FA_READ) != FR_OK) {
            snprintf(msg, msg_size, "The archive cannot be opened");
            return false;
        }
        b->lha = path; b->img = img.c_str(); b->msg = msg; b->msg_size = msg_size;
        {   // an Amiga executable (a hunk file) rather than an archive
            uint8_t m[4];
            UINT n = 0;
            b->exe = f_read(&b->in, m, 4, &n) == FR_OK && n == 4 && m[0] == 0 && m[1] == 0 && m[2] == 3 && m[3] == 0xF3;
            const char *base = strrchr(path, '/');
            snprintf(b->exe_name, sizeof b->exe_name, "%.30s", base ? base + 1 : path);
            for (char *c = b->exe_name; *c; c++) if (*c == ':' || *c == '"') *c = '_';
        }
        b->arena = s_dms_area + head;
        b->arena_size = s_dms_size - head;
        uint32_t ssize = 8192;
        uint8_t *stack = (uint8_t *)whd_try_malloc(ssize);
        if (!stack) { ssize = 6144; stack = (uint8_t *)whd_try_malloc(ssize); }
        printf("whdload: building %s (stack %s %u)\n", img.c_str(), stack ? "heap" : "in place", stack ? (unsigned)ssize : 0u);
        if (stack) {
            whd_call_on_stack((void *)(((uintptr_t)stack + ssize) & ~(uintptr_t)7), whd_build, b, stack);
            free(stack);
        } else {
            whd_build(b);
        }
        f_close(&b->in);
        if (!b->ok) return false;
        s.base_mem = b->job.base_mem; s.exp_mem = b->job.exp_mem;
        snprintf(s.missing, sizeof s.missing, "%.23s", b->job.missing);
    }
    if (img == machine_hdf(1)) { machine_set_hdf(1, ""); Config::hdf2.clear(); }   // the master boots it
    if (!machine_set_hdf(0, img.c_str())) { snprintf(msg, msg_size, "The image cannot be opened"); return false; }
    Config::hdf = img;
    Config::hdf_raw.clear();
    // What it will want (WHDLoad would say so in a requester): its ExpMem in fast
    // RAM, or in the chip RAM its base memory leaves (~200 KB for the system).
    amiga_config_t next;
    memset(&next, 0, sizeof next);
    select_model(&next);
    if (s.exp_mem > next.fast_size && s.base_mem + s.exp_mem + 200 * KB > next.chip_size)
        snprintf(msg, msg_size, "It needs %u KB of fast RAM: Machine > %s > Fast RAM",
                 (unsigned)(s.exp_mem / 1024), machine_name());
    else if (s.missing[0])
        snprintf(msg, msg_size, "It needs Kickstart %s (none built in)", s.missing);
    return true;
}
