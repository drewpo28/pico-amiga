// Headless host runner for the pico-amiga core.
//
// Runs the same src/amiga code the firmware runs, against a ROM (and an
// optional ADF) from disk, and writes what came out: framebuffer snapshots
// as PNG, Paula output as WAV, SERDAT bytes to stdout.  It is the test bench:
// everything the core does can be checked here before it goes near a board.
//
//   pico-amiga-host --rom kick.rom [--ext ext.rom] [--adf disk.adf|.dms]... (DF0..DF3; 5th-8th: --swap only)
//                   [--drives N] (fewer drives than --adf: the rest wait for --swap)
//                   [--hdf disk.hdf] (the A600's IDE drive, read from the file a sector at a time;
//                    written to it only with --write-back, otherwise the drive's writes fail)
//                   [--hdf2 disk.hdf] (the IDE slave, the same way)
//                   [--cd32] (the CD32: AGA, a 68EC020, 2 MB, Akiko; --rom its Kickstart, --ext its extended ROM)
//                   [--cd disc.iso|disc.cue] (the disc in its drive) [--pad N:buttons]... (its pad from frame N on:
//                    1 play, 2 reverse, 4 forward, 8 green, 16 yellow, 32 red, 64 blue)
//                   [--write-back] (disks the Amiga wrote are saved to their files at the end)
//                   [--fast MB] (a Zorro II fast RAM board: 1, 2, 4 or 8 MB)
//                   [--lha game.lha|program [--whd DIR] [--lha-out img.hdf]] (the archive, or an Amiga executable, as a bootable
//                    hard disk, as the firmware builds it: whdboot.c; DIR's files go onto the
//                    volume too - C/WHDLoad, Devs/Kickstarts/kickN.M + .RTB, S/WHDLoad.prefs)
//                   [--frames N] [--shot N:file.png]... [--wav out.wav]
//                   [--slow] [--a500plus | --a600] [--vcentre] [--key N:code:down]... [--status K]
//                   [--skip-odd | --skip-field | --skip-rand P] (frames not drawn: every other one, an interlaced picture's short field, or P% of them without a pattern)
//                   [--joy N:dirs:fire]... [--mouse N:dx:dy:buttons]...
// --joy sets game port 2 from frame N on (dirs: bit0 up 1 down 2 left 3 right);
// --mouse moves the port 1 mouse once at frame N (buttons: bit0 left 1 right).
// --a500plus: the A500+ (ECS chipset, 1 MB chip RAM, no slow RAM, the battery
// clock showing the PC's local time).  --a600: the A600 (as the A500+ but with
// Gayle - an empty IDE port and PCMCIA slot - and no clock); --a601: an A600 with
// 2 MB of chip RAM (an A601 in its trapdoor).  --vcentre: amiga_set_vcentre(true).
// --cpu020: a 68EC020 at 14 MHz in place of the 68000 (any model).
// --a1200: the A1200 - a 68EC020, 2 MB of chip RAM, Gayle $D1, no clock; ECS chips
// until AGA is emulated.  --aga: the AGA chipset (Alice + Lisa, being built).
// --load-test 1: the A1200 headroom test's CPU x2 (amiga_set_load_test), 2: and 8 planes,
// 3: CPU x3, 4: and 8 planes.
// --state-save N:file writes the core's snapshot (amiga_state_save) after frame N;
// --state-load file starts from one instead of the reset (same ROM, model, disks).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include "amiga.h"
#include "amiga_int.h"
#include "dms.h"
#include "whdboot.h"
#include "cdimage.h"
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "m68k.h"

static amiga_row_t fb[AMIGA_FB_H];
#define AUDIO_FRAMES 65536
static int16_t audio[AUDIO_FRAMES * 2];

// Snapshot streams over stdio.
static bool st_fwrite(void *ctx, const void *p, uint32_t n) { return fwrite(p, 1, n, ctx) == n; }
static uint32_t st_fread(void *ctx, void *p, uint32_t n) { return (uint32_t)fread(p, 1, n, ctx); }

static uint8_t *load(const char *path, uint32_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc((size_t)n);
    if (fread(p, 1, (size_t)n, f) != (size_t)n) { perror(path); exit(1); }
    fclose(f);
    *size = (uint32_t)n;
    return p;
}

// A DMS (--adf x.dms) is unpacked to an ADF in memory, as the firmware does.
typedef struct { const uint8_t *p, *end; } dms_src_t;
static uint32_t dms_src_read(void *ctx, void *dst, uint32_t len) {
    dms_src_t *s = ctx;
    if (len > (uint32_t)(s->end - s->p)) len = (uint32_t)(s->end - s->p);
    memcpy(dst, s->p, len);
    s->p += len;
    return len;
}
static uint8_t *load_disk(const char *path, uint32_t *size) {
    uint8_t *p = load(path, size);
    if (*size < 4 || memcmp(p, "DMS!", 4)) return p;
    dms_src_t src = { p, p + *size };
    uint8_t *adf = malloc(DMS_ADF_SIZE);
    void *work = malloc(dms_work_size());
    const int err = dms_unpack(dms_src_read, &src, adf, work);
    if (err != DMS_OK) { fprintf(stderr, "%s: %s\n", path, dms_error(err)); exit(1); }
    free(work);
    free(p);
    *size = DMS_ADF_SIZE;
    return adf;
}

// ---- tiny PNG writer (stored deflate blocks) -------------------------------------
static uint32_t crc_table[256];
static uint32_t crc(uint32_t c, const uint8_t *p, size_t n) {
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
    c = ~c;
    while (n--) c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}
static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v; }
static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len) {
    uint8_t h[8]; be32(h, len); memcpy(h + 4, type, 4);
    fwrite(h, 1, 8, f);
    if (len) fwrite(data, 1, len, f);
    uint32_t c = crc(0, (const uint8_t *)type, 4);
    c = crc(c, data, len);
    uint8_t t[4]; be32(t, c); fwrite(t, 1, 4, f);
}

// The picture exactly as the HDMI scanout shows it: 720x576 (times --scale).
static void write_png(const char *path, int scale) {
    int w = AMIGA_OUT_W * scale, h = 2 * AMIGA_FB_H * scale;
    size_t row = (size_t)w * 3 + 1, raw_n = row * (size_t)h;
    uint8_t *raw = malloc(raw_n);
    uint16_t line[AMIGA_OUT_W];
    for (int y = 0; y < h; y++) {
        uint8_t *r = raw + (size_t)y * row;
        *r++ = 0;
        int oy = y / scale;
        amiga_fb_line(&fb[oy >> 1], oy & 1, line);
        for (int x = 0; x < w; x++) {
            uint16_t c = line[x / scale];
            *r++ = (uint8_t)(((c >> 8) & 15) * 17);
            *r++ = (uint8_t)(((c >> 4) & 15) * 17);
            *r++ = (uint8_t)((c & 15) * 17);
        }
    }
    size_t nblk = (raw_n + 65534) / 65535;
    size_t z_n = 2 + raw_n + nblk * 5 + 4;
    uint8_t *z = malloc(z_n), *q = z;
    *q++ = 0x78; *q++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    for (size_t off = 0; off < raw_n; off += 65535) {
        size_t n = raw_n - off > 65535 ? 65535 : raw_n - off;
        *q++ = off + n == raw_n;
        *q++ = (uint8_t)n; *q++ = (uint8_t)(n >> 8);
        *q++ = (uint8_t)~n; *q++ = (uint8_t)(~n >> 8);
        memcpy(q, raw + off, n); q += n;
    }
    be32(q, (b << 16) | a); q += 4;
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ih[13]; be32(ih, (uint32_t)w); be32(ih + 4, (uint32_t)h);
    ih[8] = 8; ih[9] = 2; ih[10] = 0; ih[11] = 0; ih[12] = 0;
    chunk(f, "IHDR", ih, 13);
    chunk(f, "IDAT", z, (uint32_t)(q - z));
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw); free(z);
}

// ---- WAV -------------------------------------------------------------------------
static FILE *wav;
static uint32_t wav_frames, audio_rd;
static void wav_open(const char *path) {
    wav = fopen(path, "wb");
    if (!wav) { perror(path); exit(1); }
    uint8_t h[44] = {0};
    fwrite(h, 1, 44, wav);
}
static void le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void wav_close(void) {
    uint8_t h[44];
    memcpy(h, "RIFF", 4); le32(h + 4, 36 + wav_frames * 4); memcpy(h + 8, "WAVEfmt ", 8);
    le32(h + 16, 16); h[20] = 1; h[21] = 0; h[22] = 2; h[23] = 0;
    le32(h + 24, AMIGA_AUDIO_RATE); le32(h + 28, AMIGA_AUDIO_RATE * 4);
    h[32] = 4; h[33] = 0; h[34] = 16; h[35] = 0;
    memcpy(h + 36, "data", 4); le32(h + 40, wav_frames * 4);
    fseek(wav, 0, SEEK_SET); fwrite(h, 1, 44, wav); fclose(wav);
}
static void drain_audio(void) {
    uint32_t wp = amiga_audio_write_pos();
    for (; audio_rd != wp; audio_rd++) {
        uint32_t i = (audio_rd % AUDIO_FRAMES) * 2;
        if (wav) { fwrite(&audio[i], 2, 2, wav); wav_frames++; }
    }
}

// ---- disk ----------------------------------------------------------------------
typedef struct { uint8_t *data; uint32_t size; bool dirty; } mem_disk_t;
static int disk_read(void *ctx, uint32_t off, void *dst, uint32_t len) {
    mem_disk_t *d = ctx;
    if (off + len > d->size) return -1;
    memcpy(dst, d->data + off, len);
    return 0;
}
static int disk_write(void *ctx, uint32_t off, const void *src, uint32_t len) {
    mem_disk_t *d = ctx;
    if (off + len > d->size) return -1;
    memcpy(d->data + off, src, len);
    d->dirty = true;
    return 0;
}

// The HDF: a file, a sector at a time (images can be large).
static int hdf_read(void *ctx, uint32_t off, void *dst, uint32_t len) {
    FILE *f = ctx;
    return fseek(f, (long)off, SEEK_SET) || fread(dst, 1, len, f) != len ? -1 : 0;
}
static int hdf_write(void *ctx, uint32_t off, const void *src, uint32_t len) {
    FILE *f = ctx;
    return fseek(f, (long)off, SEEK_SET) || fwrite(src, 1, len, f) != len ? -1 : 0;
}

// ---- --cd: a CD image in the CD32's drive (cdimage.c) --------------------------------
static cd_toc_t cd_toc;
static FILE *cd_files[CD_MAX_TRACKS];
static char cd_dir[512];
static uint32_t cd_file(void *ctx __attribute__((unused)), int index, const char *name) {
    char path[1024];
    const char *base = strrchr(name, '/');                 // sheets written elsewhere carry paths
    const char *b2 = strrchr(name, '\\');
    if (b2 && (!base || b2 > base)) base = b2;
    snprintf(path, sizeof path, "%s%s", cd_dir, base ? base + 1 : name);
    FILE *f = index < CD_MAX_TRACKS ? fopen(path, "rb") : NULL;
    if (!f) { fprintf(stderr, "cd: %s: cannot open\n", path); return 0; }
    cd_files[index] = f;
    fseek(f, 0, SEEK_END);
    return (uint32_t)ftell(f);
}
static bool cd_fread(void *ctx __attribute__((unused)), int index, uint32_t off, void *dst, uint32_t len) {
    FILE *f = cd_files[index];
    return f && !fseek(f, (long)off, SEEK_SET) && fread(dst, 1, len, f) == len;
}
static int cd_sector(void *ctx __attribute__((unused)), uint32_t lsn, uint8_t *dst) { return cd_read_sector(&cd_toc, lsn, dst, cd_fread, NULL); }
static void cd_track(void *ctx __attribute__((unused)), int n, uint32_t *lsn, uint8_t *ctrl) {
    *lsn = cd_toc.t[n - 1].lsn;
    *ctrl = cd_toc.t[n - 1].mode ? 4 : 0;
}
static const uint8_t *cd_audio(void *ctx __attribute__((unused)), uint32_t lsn) {
    static uint8_t buf[2352];
    int file, mode;
    uint32_t off, ss, run;
    if (!cd_locate(&cd_toc, lsn, &file, &off, &ss, &mode, &run) || mode || !cd_fread(NULL, file, off, buf, 2352)) return NULL;
    return buf;
}
static bool cd_open(const char *path) {
    const char *slash = strrchr(path, '/');
    snprintf(cd_dir, sizeof cd_dir, "%.*s", slash ? (int)(slash - path + 1) : 0, path);
    const char *ext = strrchr(path, '.');
    bool ok;
    if (ext && !strcasecmp(ext, ".cue")) {
        uint32_t n;
        uint8_t *text = load(path, &n);
        ok = cd_parse_cue((const char *)text, n, &cd_toc, cd_file, NULL);
        free(text);
    } else {
        const uint32_t size = cd_file(NULL, 0, slash ? slash + 1 : path);
        ok = size && cd_from_iso(size, &cd_toc);
    }
    if (!ok) { fprintf(stderr, "%s: not a CD image this reads\n", path); return false; }
    for (int i = 0; i < cd_toc.tracks; i++)
        fprintf(stderr, "cd: track %d %s/%u, sectors %u+%u, file %u at %u\n", i + 1,
                cd_toc.t[i].mode ? cd_toc.t[i].mode == 1 ? "MODE1" : "MODE2" : "AUDIO", cd_toc.t[i].secsize,
                cd_toc.t[i].lsn, cd_toc.t[i].sectors, cd_toc.t[i].file, cd_toc.t[i].foff);
    const amiga_cd_t cd = { cd_sector, cd_track, cd_audio, NULL, (uint8_t)cd_toc.tracks, cd_toc.leadout };
    amiga_set_cd(&cd);
    return true;
}

// ---- --lha: an LHA archive as a bootable hard disk (whdboot.c) ---------------------------
static uint32_t lha_fread(void *ctx, uint32_t off, void *dst, uint32_t len) {
    FILE *f = ctx;
    if (fseek(f, (long)off, SEEK_SET)) return 0;
    return (uint32_t)fread(dst, 1, len, f);
}
static int img_write(void *ctx, uint32_t off, const void *src, uint32_t len) {
    return hdf_write(ctx, off, src, len);
}
static whd_extra_t whd_extra[256];
static char whd_paths[256][400], whd_tags[256][24];
static int whd_nextra;
static void whd_walk(const char *root, const char *rel) {
    char dir[600];
    snprintf(dir, sizeof dir, "%s/%s", root, rel);
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) && whd_nextra < 256) {
        if (de->d_name[0] == '.') continue;
        char r[400], full[1000];
        snprintf(r, sizeof r, "%s%s%s", rel, rel[0] ? "/" : "", de->d_name);
        snprintf(full, sizeof full, "%s/%s", root, r);
        struct stat st;
        if (stat(full, &st)) continue;
        if (S_ISDIR(st.st_mode)) { whd_walk(root, r); continue; }
        uint32_t size;
        const int k = whd_nextra++;
        whd_extra[k].data = load(full, &size);
        whd_extra[k].size = size;
        snprintf(whd_paths[k], sizeof whd_paths[k], "%s", r);
        whd_extra[k].path = whd_paths[k];
        whd_extra[k].kick = whd_kick_tag(r, whd_tags[k]);
    }
    closedir(d);
}
// The image into `out` (a file opened for writing); its size, 0 = failed.
static uint32_t build_lha(const char *lha_path, const char *whd_dir, FILE *out) {
    FILE *f = fopen(lha_path, "rb");
    if (!f) { fprintf(stderr, "%s: cannot open\n", lha_path); return 0; }
    if (whd_dir) whd_walk(whd_dir, "");   // before the lone program: whd_extra fills in order
    whd_job_t job;
    memset(&job, 0, sizeof job);
    job.rd = lha_fread; job.rctx = f;
    // an Amiga executable instead of an archive: it alone, run at boot
    uint8_t magic[4] = { 0 };
    if (fread(magic, 1, 4, f) == 4 && magic[0] == 0 && magic[1] == 0 && magic[2] == 3 && magic[3] == 0xF3 && whd_nextra < 256) {
        const int k = whd_nextra++;
        uint32_t size;
        whd_extra[k].data = load(lha_path, &size);
        whd_extra[k].size = size;
        const char *b = strrchr(lha_path, '/');
        snprintf(whd_paths[k], sizeof whd_paths[k], "%.30s", b ? b + 1 : lha_path);
        whd_extra[k].path = whd_paths[k];
        whd_extra[k].kick = "";
        job.rd = NULL;
        job.run = whd_paths[k];
    }
    job.extra = whd_extra; job.extra_count = whd_nextra;
    char vol[64];
    const char *b = strrchr(lha_path, '/');
    snprintf(vol, sizeof vol, "%s", b ? b + 1 : lha_path);
    char *dot = strrchr(vol, '.');
    if (dot) *dot = 0;
    vol[30] = 0;
    job.volume = vol;
    job.unix_time = (uint32_t)time(NULL);
    const uint32_t arena_size = 4u << 20;
    void *arena = malloc(arena_size);
    int r = whd_plan(&job, arena, arena_size);
    if (r == WHD_OK) {
        if (ftruncate(fileno(out), (off_t)job.blocks * 512)) r = WHD_ERR_WRITE;
        else r = whd_write(&job, arena, img_write, out);
    }
    fflush(out);
    free(arena);
    fclose(f);
    fprintf(stderr, "lha: %s -> %u KB: %s; slave %s (chip %u KB, other %u KB), program %s, kick %s%s%s\n",
            lha_path, job.blocks / 2, whd_error(r), job.slave[0] ? job.slave : "-",
            job.base_mem / 1024, job.exp_mem / 1024, job.program[0] ? job.program : "-",
            job.kicks[0] ? job.kicks : "-", job.missing[0] ? ", missing " : "", job.missing);
    fprintf(stderr, "lha: boots with: %s\n", job.command);
    return r == WHD_OK ? job.blocks * 512 : 0;
}

// --render-thread: Denise on a second thread, as the firmware runs it on core 1.
static void *render_thread(void *arg) {
    (void)arg;
    for (;;) amiga_render_pump();
    return NULL;
}

static void serial_out(void *ctx, uint8_t c) {
    (void)ctx;
    if (c == '\r') return;
    putchar(c);
    if (c == '\n') fflush(stdout);
}

typedef struct { int frame; char path[256]; } shot_t;
typedef struct { int frame, code, down; } keyev_t;
typedef struct { int frame, a, b, c; } inev_t;     // --joy: dirs fire; --mouse: dx dy buttons

// --trace N: every custom write and sprite DMA fetch of frame N (amiga_trace_reg).
static int trace_frame = -1;
static void trace_reg(uint32_t reg, uint16_t v, int hpos, uint32_t x) {
    if (reg == 0x3000) { fprintf(stderr, "T v%3d h%3d cop fetch %06X\n", A.vpos, hpos, x); return; }
    if (reg >= 0x2000) { if (x) fprintf(stderr, "T v%3d bpl%d units %u pt=%06X\n", A.vpos, (int)(reg - 0x2000), v, x); return; }
    if (reg >= 0x1000) fprintf(stderr, "T v%3d spr%d st%u pt=%06X\n", A.vpos, (int)(reg - 0x1000), v, x);
    else fprintf(stderr, "T v%3d h%3d %c %03X=%04X\n", A.vpos, hpos, x ? 'C' : 'P', reg, v);
}

static uint32_t prof_clock(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000000000ll + ts.tv_nsec);
}

static int64_t rtc_time(void *ctx) {
    (void)ctx;
    const time_t t = time(NULL);
    struct tm lt;
    localtime_r(&t, &lt);
    return (int64_t)t + lt.tm_gmtoff;
}

int main(int argc, char **argv) {
    const char *rom_path = NULL, *ext_path = NULL, *wav_path = NULL;
    const char *adf_path[8] = {0}; int nadf = 0;
    const char *hdf_path = NULL, *hdf2_path = NULL, *cd_path = NULL;
    bool cd32 = false;
    inev_t pads[256]; int npads = 0;                   // --pad frame:buttons (AMIGA_PAD_*)
    const char *lha_path = NULL, *whd_dir = NULL, *lha_out = NULL;
    int fast_mb = 0;
    int load_test = 0;        // the first 4 go in DF0..DF3, the rest wait for --swap
    int frames = 250, status_every = 0, scale = 1, drives_opt = 0;
    bool slow = false, render_thread_on = false, plus = false, a600 = false, a601 = false, cpu020 = false, a1200 = false, aga = false, vcentre = false, write_back = false;
    int chip_bus = -1;                                 // --chip-bus / --fast-bus; default: on with --a1200
    bool skip_odd = false, skip_field = false, bus_auto = false;
    int fast_thr = 0;
    int skip_rand = 0;                                 // --skip-rand P: P% of the frames not drawn, in runs of up to 4 (as the board's lateness skips)
    int bus_fetch8 = 6, bus_cost8 = 16;
    const char *hist_path = NULL, *pchist_path = NULL;
    shot_t shots[64]; int nshots = 0;
    shot_t state_save = { -1, "" };
    const char *state_load = NULL;
    keyev_t keys[256]; int nkeys = 0;
    inev_t joys[256]; int njoys = 0;
    inev_t mice[256]; int nmice = 0;
    inev_t dumps[16]; int ndumps = 0;                  // --dump frame:addr:words (hex words to stderr)
    inev_t swaps[16]; int nswaps = 0;                  // --swap frame:drive:disk (an --adf index, -1 ejects)

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--rom") && v) { rom_path = v; i++; }
        else if (!strcmp(a, "--ext") && v) { ext_path = v; i++; }
        else if (!strcmp(a, "--adf") && v && nadf < 8) { adf_path[nadf++] = v; i++; }
        else if (!strcmp(a, "--hdf") && v) { hdf_path = v; i++; }
        else if (!strcmp(a, "--hdf2") && v) { hdf2_path = v; i++; }          // the IDE slave
        else if (!strcmp(a, "--lha") && v) { lha_path = v; i++; }
        else if (!strcmp(a, "--fast") && v) { fast_mb = atoi(v); i++; }   // Zorro II fast RAM, MB (1/2/4/8)
        else if (!strcmp(a, "--whd") && v) { whd_dir = v; i++; }
        else if (!strcmp(a, "--lha-out") && v) { lha_out = v; i++; }
        else if (!strcmp(a, "--load-test") && v) { load_test = atoi(v); i++; }   // DF0, DF1, ...
        else if (!strcmp(a, "--frames") && v) { frames = atoi(v); i++; }
        else if (!strcmp(a, "--wav") && v) { wav_path = v; i++; }
        else if (!strcmp(a, "--status") && v) { status_every = atoi(v); i++; }
        else if (!strcmp(a, "--scale") && v) { scale = atoi(v); i++; }
        else if (!strcmp(a, "--slow")) slow = true;
        else if (!strcmp(a, "--a500plus")) plus = true;
        else if (!strcmp(a, "--a600")) plus = a600 = true;
        else if (!strcmp(a, "--a601")) plus = a600 = a601 = true;
        else if (!strcmp(a, "--cpu020")) cpu020 = true;
        else if (!strcmp(a, "--aga")) aga = true;
        else if (!strcmp(a, "--a1200")) plus = a600 = a601 = cpu020 = a1200 = true;
        else if (!strcmp(a, "--cd32")) plus = a601 = cpu020 = aga = cd32 = true;   // AGA, a 68EC020, 2 MB, Akiko
        else if (!strcmp(a, "--cd") && v) { cd_path = v; i++; }
        else if (!strcmp(a, "--pad") && v && npads < 256) {
            if (sscanf(v, "%d:%i", &pads[npads].frame, &pads[npads].a) == 2) npads++;
            i++;
        }
        else if (!strcmp(a, "--chip-bus")) chip_bus = 1;
        else if (!strcmp(a, "--fast-bus")) chip_bus = 0;
        else if (!strcmp(a, "--skip-field")) skip_field = true;  // interlace: the short field's frames (the board's way when late)
        else if (!strcmp(a, "--skip-odd")) skip_odd = true;     // odd frames not drawn (amiga_set_render_skip)
        else if (!strcmp(a, "--skip-rand") && v) { skip_rand = atoi(v); i++; }
        else if (!strcmp(a, "--bus-auto")) bus_auto = true;     // amiga_set_chip_bus_auto
        else if (!strcmp(a, "--fast-thr") && v) { fast_thr = atoi(v); i++; }   // amiga_set_fast_throttle (eighths of a clock)
        else if (!strcmp(a, "--bus-tune") && v) { sscanf(v, "%d,%d", &bus_fetch8, &bus_cost8); i++; }   // amiga_set_chip_bus_params
        else if (!strcmp(a, "--hist") && v) { hist_path = v; i++; }
        else if (!strcmp(a, "--pchist") && v) { pchist_path = v; i++; }   // the 64 hottest PCs (instructions run) at the end
        else if (!strcmp(a, "--trace") && v) { trace_frame = atoi(v); i++; }   // handler runs by index at the end (tools/musashi_hist.py)
        else if (!strcmp(a, "--write-back")) write_back = true;
        else if (!strcmp(a, "--drives") && v) { drives_opt = atoi(v); i++; }
        else if (!strcmp(a, "--vcentre")) vcentre = true;
        else if (!strcmp(a, "--render-thread")) render_thread_on = true;
        else if (!strcmp(a, "--shot") && v && nshots < 64) {
            const char *c = strchr(v, ':');
            if (!c) { fprintf(stderr, "--shot N:file.png\n"); return 2; }
            shots[nshots].frame = atoi(v);
            snprintf(shots[nshots].path, sizeof shots[nshots].path, "%s", c + 1);
            nshots++; i++;
        } else if (!strcmp(a, "--state-save") && v) {
            const char *c = strchr(v, ':');
            if (!c) { fprintf(stderr, "--state-save N:file\n"); return 2; }
            state_save.frame = atoi(v);
            snprintf(state_save.path, sizeof state_save.path, "%s", c + 1);
            i++;
        } else if (!strcmp(a, "--state-load") && v) { state_load = v; i++;
        } else if (!strcmp(a, "--key") && v && nkeys < 256) {
            if (sscanf(v, "%d:%i:%d", &keys[nkeys].frame, &keys[nkeys].code, &keys[nkeys].down) == 3) nkeys++;
            i++;
        } else if (!strcmp(a, "--joy") && v && njoys < 256) {
            if (sscanf(v, "%d:%i:%d", &joys[njoys].frame, &joys[njoys].a, &joys[njoys].b) == 3) njoys++;
            i++;
        } else if (!strcmp(a, "--mouse") && v && nmice < 256) {
            if (sscanf(v, "%d:%d:%d:%i", &mice[nmice].frame, &mice[nmice].a, &mice[nmice].b, &mice[nmice].c) == 4) nmice++;
            i++;
        } else if (!strcmp(a, "--dump") && v && ndumps < 16) {
            if (sscanf(v, "%d:%i:%d", &dumps[ndumps].frame, &dumps[ndumps].a, &dumps[ndumps].b) == 3) ndumps++;
            i++;
        } else if (!strcmp(a, "--swap") && v && nswaps < 16) {
            if (sscanf(v, "%d:%d:%d", &swaps[nswaps].frame, &swaps[nswaps].a, &swaps[nswaps].b) == 3) nswaps++;
            i++;
        } else { fprintf(stderr, "unknown option %s\n", a); return 2; }
    }
    if (!rom_path) {
        fprintf(stderr, "usage: %s --rom kick.rom [--ext ext.rom] [--adf disk.adf]... [--frames N]\n"
                        "       [--shot N:file.png] [--wav out.wav] [--slow] [--a500plus | --a600 | --a601 | --a1200] [--status K] [--swap frame:drive:disk]\n", argv[0]);
        return 2;
    }

    amiga_config_t cfg = {0};
    cfg.chip_size = a601 ? 2048 * 1024 : plus ? 1024 * 1024 : 512 * 1024;
    cfg.chip_ram = calloc(1, cfg.chip_size);
    if (a600) cfg.gayle = a1200 ? AMIGA_GAYLE_A1200 : AMIGA_GAYLE_A600;
    if (cpu020) cfg.cpu = AMIGA_CPU_EC020;
    if (plus) { cfg.chipset = AMIGA_ECS; cfg.rtc_time = rtc_time; }
    static uint8_t nvram[1024];
    if (cd32) { cfg.akiko = true; cfg.nvram = nvram; cfg.rtc_time = NULL; }
    else if (slow) { cfg.slow_size = 512 * 1024; cfg.slow_ram = calloc(1, cfg.slow_size); }
    if (aga) cfg.chipset = AMIGA_AGA;          // after the model's ECS
    uint32_t rs; cfg.rom = load(rom_path, &rs); cfg.rom_size = rs;
    if (rs != 256 * 1024 && rs != 512 * 1024) { fprintf(stderr, "ROM must be 256 or 512 KB\n"); return 1; }
    if (ext_path) { uint32_t es; cfg.ext_rom = load(ext_path, &es); cfg.ext_size = es; }
    cfg.framebuffer = fb;
    cfg.audio_ring = audio; cfg.audio_frames = AUDIO_FRAMES;
    static uint16_t track[AMIGA_TRACK_WORDS];
    cfg.track = track;
    cfg.serial_out = serial_out;
    cfg.prof_clock = prof_clock;            // --status: the core's buckets, ns
    if (fast_mb) { cfg.fast_size = (uint32_t)fast_mb << 20; cfg.fast_ram = calloc(1, cfg.fast_size); }
    amiga_init(&cfg);
    amiga_set_vcentre(vcentre);
    amiga_set_chip_bus(chip_bus < 0 ? a1200 || cd32 : chip_bus != 0);   // the A1200's 68EC020 waits for the chip bus
    amiga_set_chip_bus_params(bus_fetch8, bus_cost8);
    amiga_set_chip_bus_auto(bus_auto);
    amiga_set_fast_throttle((unsigned)fast_thr);
    if (pchist_path) { extern unsigned int *amiga_pc_hist; amiga_pc_hist = calloc(1u << 20, sizeof *amiga_pc_hist); }
    if (render_thread_on) {
        pthread_t th;
        pthread_create(&th, NULL, render_thread, NULL);
        amiga_render_async(true);
    }

    static mem_disk_t md[8];
    int ndrv = nadf < 4 ? nadf : 4;
    if (cd32) ndrv = 0;                        // the CD32 has no floppy drive: --adf stays out
    if (drives_opt > 0 && drives_opt < ndrv) ndrv = drives_opt;
    for (int k = 0; k < nadf; k++) {
        md[k].data = load_disk(adf_path[k], &md[k].size);
        if (k >= ndrv) continue;
        amiga_disk_t d = { disk_read, disk_write, &md[k], md[k].size };
        amiga_insert_disk(k, &d);
    }
    if (ndrv > 1 || cd32) { amiga_set_drives(ndrv); amiga_reset(true); }   // Kickstart finds DF1+ at reset
    if (load_test) amiga_set_load_test(load_test >= 3 ? 3 : 2, load_test == 2 || load_test == 4);
    if (cd_path && !cd_open(cd_path)) return 1;
    FILE *hdf = NULL;
    if (hdf_path) {
        hdf = fopen(hdf_path, write_back ? "r+b" : "rb");
        if (!hdf) { fprintf(stderr, "%s: cannot open\n", hdf_path); return 1; }
        fseek(hdf, 0, SEEK_END);
        const amiga_disk_t d = { hdf_read, write_back ? hdf_write : NULL, hdf, (uint32_t)ftell(hdf) };
        amiga_set_hdf(0, &d);
        amiga_reset(true);
    }
    if (hdf2_path) {
        FILE *f2 = fopen(hdf2_path, write_back ? "r+b" : "rb");
        if (!f2) { fprintf(stderr, "%s: cannot open\n", hdf2_path); return 1; }
        fseek(f2, 0, SEEK_END);
        const amiga_disk_t d = { hdf_read, write_back ? hdf_write : NULL, f2, (uint32_t)ftell(f2) };
        amiga_set_hdf(1, &d);
        amiga_reset(true);
    }
    if (lha_path) {
        hdf = lha_out ? fopen(lha_out, "w+b") : tmpfile();
        const uint32_t size = hdf ? build_lha(lha_path, whd_dir, hdf) : 0;
        if (!size) return 1;
        const amiga_disk_t d = { hdf_read, hdf_write, hdf, size };
        amiga_set_hdf(0, &d);
        amiga_reset(true);
    }
    if (wav_path) wav_open(wav_path);
    if (state_load) {
        FILE *f = fopen(state_load, "rb");
        const amiga_stream_t io = { NULL, st_fread, f };
        if (!f || !amiga_state_load(&io)) { fprintf(stderr, "%s: cannot load the snapshot\n", state_load); return 1; }
        fclose(f);
    }

    for (int f = 0; f < frames; f++) {
        for (int k = 0; k < nkeys; k++) if (keys[k].frame == f) amiga_key((uint8_t)keys[k].code, keys[k].down);
        for (int k = 0; k < njoys; k++) if (joys[k].frame == f) amiga_joystick(1, (uint8_t)joys[k].a, joys[k].b);
        for (int k = 0; k < npads; k++) if (pads[k].frame == f) amiga_cd32_pad((uint8_t)pads[k].a);
        for (int k = 0; k < nmice; k++)
            if (mice[k].frame == f) amiga_mouse(mice[k].a, mice[k].b, mice[k].c & 1, mice[k].c & 2);
        for (int k = 0; k < nswaps; k++)
            if (swaps[k].frame == f) {
                if (swaps[k].b < 0 || swaps[k].b >= nadf) amiga_eject_disk(swaps[k].a);   // -1: eject
                else {
                    amiga_disk_t d = { disk_read, disk_write, &md[swaps[k].b], md[swaps[k].b].size };
                    amiga_insert_disk(swaps[k].a, &d);
                }
            }
        {
            // --skip-rand: without a pattern, as the board skips when it is late; a
            // program that alternates two copper lists a frame (Agony) meets every
            // order of drawn and skipped lists only so.
            static uint32_t lcg = 12345; static int run;
            lcg = lcg * 1664525u + 1013904223u;
            bool skip = skip_field ? amiga_lace_field() == 1 : skip_odd && (f & 1);
            if (skip_rand) skip = (int)((lcg >> 16) % 100) < skip_rand && run < 4;
            run = skip ? run + 1 : 0;
            amiga_set_render_skip(skip);
        }
        amiga_trace_reg = f + 1 == trace_frame ? trace_reg : NULL;
        amiga_run_frame();
        for (int k = 0; k < ndumps; k++) if (dumps[k].frame == f + 1) {
            fprintf(stderr, "dump %06X:", dumps[k].a);
            for (int w = 0; w < dumps[k].b; w++) fprintf(stderr, " %04X", m68k_read_memory_16((unsigned)(dumps[k].a + 2 * w)));
            fprintf(stderr, "\n");
        }
        drain_audio();
        if (state_save.frame == f + 1) {
            FILE *sf = fopen(state_save.path, "wb");
            const amiga_stream_t io = { st_fwrite, NULL, sf };
            if (!sf || !amiga_state_save(&io) || fclose(sf)) { fprintf(stderr, "%s: cannot save the snapshot\n", state_save.path); return 1; }
        }
        for (int s = 0; s < nshots; s++) if (shots[s].frame == f + 1) write_png(shots[s].path, scale);
        if (status_every && (f + 1) % status_every == 0) {
            amiga_status_t st; amiga_get_status(&st);
            int modes[3] = {0, 0, 0};
            for (int r = 0; r < AMIGA_FB_H; r++) if (fb[r].mode < 3) modes[fb[r].mode]++;
            struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
            static uint64_t idle_last;                              // the CPU's busy-wait time skipped (idle.c)
            amiga_profile_t pr; amiga_get_profile(&pr, true);        // since the last status
            const uint32_t nf = pr.frames ? pr.frames : 1;
            static amiga_status_t st_last;                         // the counters, per frame since the last status
            extern uint32_t denise_stat_compose, denise_stat_oto, denise_stat_ev, copper_stat_mir, copper_stat_fetch;
            extern unsigned int amiga_stat_audw;
            static uint32_t dc_last, do_last, de_last, da_last, cm_last, cf_last;
            fprintf(stderr, "render/frame: events %u o_to %u sprite composes %u audw %u cop %u (mirror %u) | ", (denise_stat_ev - de_last) / nf,
                    (denise_stat_oto - do_last) / nf, (denise_stat_compose - dc_last) / nf, (amiga_stat_audw - da_last) / nf,
                    (copper_stat_fetch - cf_last) / nf, (copper_stat_mir - cm_last) / nf);
            dc_last = denise_stat_compose; do_last = denise_stat_oto; de_last = denise_stat_ev; da_last = amiga_stat_audw;
            cm_last = copper_stat_mir; cf_last = copper_stat_fetch;
            fprintf(stderr, "frame %u t=%ld.%06ld pc=%06X dmacon=%04X intena=%04X intreq=%04X ovl=%d cop=%06X blits=%u "
                    "rows direct=%d idx8=%d lace4=%d | con0=%04X fmode=%X dmalines=%u | /frame instr %u data %u fetch %u busw %u%%"
                    " | us/frame cpu %u ren %u bpl %u rd %u cop %u blt %u | idle %u%%\n",
                    st.frame, (long)ts.tv_sec, ts.tv_nsec / 1000, st.pc, st.dmacon, st.intena, st.intreq, st.overlay, st.cop_pc, st.blits,
                    modes[0], modes[1], modes[2],
                    st.bplcon0, st.fmode, st.dma_lines,
                    (unsigned)((st.instr - st_last.instr) / nf), (unsigned)((st.data_acc - st_last.data_acc) / nf),
                    (unsigned)((st.fetch_acc - st_last.fetch_acc) / nf),
                    (unsigned)((st.bus_stall - st_last.bus_stall) * 100 / nf / (AMIGA_CC_PER_LINE * AMIGA_LINES_LONG)),
                    (unsigned)(pr.ticks[AMIGA_PROF_CPU] / nf / 1000), (unsigned)(pr.ticks[AMIGA_PROF_RENDER] / nf / 1000),
                    (unsigned)(pr.ticks[AMIGA_PROF_BPL] / nf / 1000), (unsigned)(pr.ticks[AMIGA_PROF_BPLRD] / nf / 1000),
                    (unsigned)(pr.ticks[AMIGA_PROF_COPPER] / nf / 1000), (unsigned)(pr.ticks[AMIGA_PROF_BLIT] / nf / 1000),
                    (unsigned)((pr.idle_cycles - idle_last) * 100 / nf / (2u * AMIGA_CC_PER_LINE * AMIGA_LINES_LONG)));
            idle_last = pr.idle_cycles;
            st_last = st;
        }
    }
    if (wav) wav_close();
    if (getenv("AMIGA_EVK")) {                       // display events by register, the hottest
        extern uint32_t denise_stat_evk[0x200];
        for (int k = 0; k < 12; k++) {
            unsigned best = 0, bi = 0;
            for (unsigned i = 0; i < 0x200; i++) if (denise_stat_evk[i] > best) { best = denise_stat_evk[i]; bi = i; }
            if (!best) break;
            fprintf(stderr, "evk %s%03X %u\n", bi >= 0x100 ? "EV_" : "", bi >= 0x100 ? bi - 0x100 : bi * 2, best);
            denise_stat_evk[bi] = 0;
        }
    }
    if (hist_path) {
        extern unsigned int amiga_stat_hist[2048];
        FILE *hf = fopen(hist_path, "w");
        if (!hf) { perror(hist_path); return 1; }
        for (int i = 0; i < 2048; i++) if (amiga_stat_hist[i]) fprintf(hf, "%d %u\n", i, amiga_stat_hist[i]);
        fclose(hf);
    }
    if (pchist_path) {
        // "PC count share%" for the 64 hottest instruction addresses, hottest first,
        // then the total: where a demo's CPU time goes (idle.c's loops show up here).
        extern unsigned int *amiga_pc_hist;
        FILE *hf = fopen(pchist_path, "w");
        if (!hf) { perror(pchist_path); return 1; }
        uint64_t total = 0;
        for (unsigned i = 0; i < (1u << 20); i++) total += amiga_pc_hist[i];
        for (int k = 0; k < 64; k++) {
            unsigned best = 0, bi = 0;
            for (unsigned i = 0; i < (1u << 20); i++) if (amiga_pc_hist[i] > best) { best = amiga_pc_hist[i]; bi = i; }
            if (!best) break;
            fprintf(hf, "%06X %u %.2f%%\n", bi * 2, best, 100.0 * best / (double)(total ? total : 1));
            amiga_pc_hist[bi] = 0;
        }
        fprintf(hf, "total %llu\n", (unsigned long long)total);
        fclose(hf);
    }
    // --write-back: what the Amiga wrote goes back into the image files (a DMS is
    // written as the ADF it was unpacked to, under its own name).
    for (int k = 0; write_back && k < nadf; k++) {
        if (!md[k].dirty) continue;
        FILE *f = fopen(adf_path[k], "wb");
        if (!f || fwrite(md[k].data, 1, md[k].size, f) != md[k].size) { perror(adf_path[k]); return 1; }
        fclose(f);
        fprintf(stderr, "wrote back %s\n", adf_path[k]);
    }
    return 0;
}
