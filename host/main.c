// Headless host runner for the pico-amiga core.
//
// Runs the same src/amiga code the firmware runs, against a ROM (and an
// optional ADF) from disk, and writes what came out: framebuffer snapshots
// as PNG, Paula output as WAV, SERDAT bytes to stdout.  It is the test bench:
// everything the core does can be checked here before it goes near a board.
//
//   pico-amiga-host --rom kick.rom [--ext ext.rom] [--adf disk.adf|.dms]... (DF0, DF1, ...)
//                   [--frames N] [--shot N:file.png]... [--wav out.wav]
//                   [--slow] [--a500plus] [--vcentre] [--key N:code:down]... [--status K]
//                   [--joy N:dirs:fire]... [--mouse N:dx:dy:buttons]...
// --joy sets game port 2 from frame N on (dirs: bit0 up 1 down 2 left 3 right);
// --mouse moves the port 1 mouse once at frame N (buttons: bit0 left 1 right).
// --a500plus: the A500+ (ECS chipset, 1 MB chip RAM, no slow RAM, the battery
// clock showing the PC's local time).  --vcentre: amiga_set_vcentre(true).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include "amiga.h"
#include "dms.h"
#include "m68k.h"

static amiga_row_t fb[AMIGA_FB_H];
#define AUDIO_FRAMES 65536
static int16_t audio[AUDIO_FRAMES * 2];

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
typedef struct { uint8_t *data; uint32_t size; } mem_disk_t;
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
    return 0;
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
    const char *adf_path[4] = {0}; int nadf = 0;
    int frames = 250, status_every = 0, scale = 1;
    bool slow = false, render_thread_on = false, plus = false, vcentre = false;
    shot_t shots[64]; int nshots = 0;
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
        else if (!strcmp(a, "--adf") && v && nadf < 4) { adf_path[nadf++] = v; i++; }   // DF0, DF1, ...
        else if (!strcmp(a, "--frames") && v) { frames = atoi(v); i++; }
        else if (!strcmp(a, "--wav") && v) { wav_path = v; i++; }
        else if (!strcmp(a, "--status") && v) { status_every = atoi(v); i++; }
        else if (!strcmp(a, "--scale") && v) { scale = atoi(v); i++; }
        else if (!strcmp(a, "--slow")) slow = true;
        else if (!strcmp(a, "--a500plus")) plus = true;
        else if (!strcmp(a, "--vcentre")) vcentre = true;
        else if (!strcmp(a, "--render-thread")) render_thread_on = true;
        else if (!strcmp(a, "--shot") && v && nshots < 64) {
            const char *c = strchr(v, ':');
            if (!c) { fprintf(stderr, "--shot N:file.png\n"); return 2; }
            shots[nshots].frame = atoi(v);
            snprintf(shots[nshots].path, sizeof shots[nshots].path, "%s", c + 1);
            nshots++; i++;
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
                        "       [--shot N:file.png] [--wav out.wav] [--slow] [--a500plus] [--status K] [--swap frame:drive:disk]\n", argv[0]);
        return 2;
    }

    amiga_config_t cfg = {0};
    cfg.chip_size = plus ? 1024 * 1024 : 512 * 1024;
    cfg.chip_ram = calloc(1, cfg.chip_size);
    if (plus) { cfg.chipset = AMIGA_ECS; cfg.rtc_time = rtc_time; }
    else if (slow) { cfg.slow_size = 512 * 1024; cfg.slow_ram = calloc(1, cfg.slow_size); }
    uint32_t rs; cfg.rom = load(rom_path, &rs); cfg.rom_size = rs;
    if (rs != 256 * 1024 && rs != 512 * 1024) { fprintf(stderr, "ROM must be 256 or 512 KB\n"); return 1; }
    if (ext_path) { uint32_t es; cfg.ext_rom = load(ext_path, &es); cfg.ext_size = es; }
    cfg.framebuffer = fb;
    cfg.audio_ring = audio; cfg.audio_frames = AUDIO_FRAMES;
    cfg.serial_out = serial_out;
    cfg.prof_clock = prof_clock;            // --status: the core's buckets, ns
    amiga_init(&cfg);
    amiga_set_vcentre(vcentre);
    if (render_thread_on) {
        pthread_t th;
        pthread_create(&th, NULL, render_thread, NULL);
        amiga_render_async(true);
    }

    static mem_disk_t md[4];
    for (int k = 0; k < nadf; k++) {
        md[k].data = load_disk(adf_path[k], &md[k].size);
        amiga_disk_t d = { disk_read, disk_write, &md[k], md[k].size };
        amiga_insert_disk(k, &d);
    }
    if (nadf > 1) { amiga_set_drives(nadf); amiga_reset(true); }   // Kickstart finds DF1+ at reset
    if (wav_path) wav_open(wav_path);

    for (int f = 0; f < frames; f++) {
        for (int k = 0; k < nkeys; k++) if (keys[k].frame == f) amiga_key((uint8_t)keys[k].code, keys[k].down);
        for (int k = 0; k < njoys; k++) if (joys[k].frame == f) amiga_joystick(1, (uint8_t)joys[k].a, joys[k].b);
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
        amiga_run_frame();
        for (int k = 0; k < ndumps; k++) if (dumps[k].frame == f + 1) {
            fprintf(stderr, "dump %06X:", dumps[k].a);
            for (int w = 0; w < dumps[k].b; w++) fprintf(stderr, " %04X", m68k_read_memory_16((unsigned)(dumps[k].a + 2 * w)));
            fprintf(stderr, "\n");
        }
        drain_audio();
        for (int s = 0; s < nshots; s++) if (shots[s].frame == f + 1) write_png(shots[s].path, scale);
        if (status_every && (f + 1) % status_every == 0) {
            amiga_status_t st; amiga_get_status(&st);
            int modes[3] = {0, 0, 0};
            for (int r = 0; r < AMIGA_FB_H; r++) if (fb[r].mode < 3) modes[fb[r].mode]++;
            struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
            amiga_profile_t pr; amiga_get_profile(&pr, true);        // since the last status
            const uint32_t nf = pr.frames ? pr.frames : 1;
            fprintf(stderr, "frame %u t=%ld.%06ld pc=%06X dmacon=%04X intena=%04X intreq=%04X ovl=%d cop=%06X blits=%u "
                    "rows direct=%d idx8=%d lace4=%d | us/frame cpu %u ren %u bpl %u rd %u cop %u blt %u\n",
                    st.frame, (long)ts.tv_sec, ts.tv_nsec / 1000, st.pc, st.dmacon, st.intena, st.intreq, st.overlay, st.cop_pc, st.blits,
                    modes[0], modes[1], modes[2],
                    (unsigned)(pr.ticks[AMIGA_PROF_CPU] / nf / 1000), (unsigned)(pr.ticks[AMIGA_PROF_RENDER] / nf / 1000),
                    (unsigned)(pr.ticks[AMIGA_PROF_BPL] / nf / 1000), (unsigned)(pr.ticks[AMIGA_PROF_BPLRD] / nf / 1000),
                    (unsigned)(pr.ticks[AMIGA_PROF_COPPER] / nf / 1000), (unsigned)(pr.ticks[AMIGA_PROF_BLIT] / nf / 1000));
        }
    }
    if (wav) wav_close();
    return 0;
}
