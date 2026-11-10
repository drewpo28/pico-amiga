// tools/m68kbench (see m68kbench.sh): the emulation core (src/amiga + Musashi,
// compiled with the firmware's flags) on a Cortex-M33 under QEMU (mps2-an505),
// running an A1200 from a snapshot the host bench saved.  A TCG plugin counts
// the ARM instructions each function executes from bench_mark() on.
// argv: rom adf state frames [warm-up frames] [fetch8,cost8]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "amiga.h"

#define BIG __attribute__((section(".bigmem")))
BIG static uint8_t chip[2 << 20];
BIG static uint8_t rom[512 << 10];
BIG static uint8_t adf[901120];
BIG static amiga_row_t fb[AMIGA_FB_H];
BIG static int16_t audio[4096 * 2];
BIG static uint16_t track[AMIGA_TRACK_WORDS];

static uint32_t load(const char *path, uint8_t *dst, uint32_t max) {
    FILE *f = fopen(path, "rb");
    if (!f) { printf("%s: cannot open\n", path); exit(2); }
    const uint32_t n = (uint32_t)fread(dst, 1, max, f);
    fclose(f);
    return n;
}
static int disk_read(void *ctx, uint32_t off, void *dst, uint32_t len) {
    (void)ctx;
    if (off + len > sizeof adf) return -1;
    memcpy(dst, adf + off, len);
    return 0;
}
static uint32_t st_fread(void *ctx, void *p, uint32_t n) { return (uint32_t)fread(p, 1, n, (FILE *)ctx); }

// The plugin zeroes its counts when this runs (its address: `mark=` argument).
__attribute__((noinline, used)) void bench_mark(void) { __asm__ volatile("nop"); }

static uint32_t crc_of(const uint8_t *p, uint32_t n) { return amiga_crc32(0, p, n); }

int main(int argc, char **argv) {
    if (argc < 5) { printf("args: rom adf state frames [warm] [fetch8,cost8]\n"); return 2; }
    const int frames = atoi(argv[4]), warm = argc > 5 ? atoi(argv[5]) : 2;
    int fetch8 = 2, cost8 = 16;
    if (argc > 6) sscanf(argv[6], "%d,%d", &fetch8, &cost8);
    amiga_config_t cfg = {0};
    cfg.chip_ram = chip; cfg.chip_size = sizeof chip;
    cfg.gayle = AMIGA_GAYLE_A1200;
    cfg.cpu = AMIGA_CPU_EC020;
    cfg.chipset = AMIGA_AGA;
    cfg.rom = rom; cfg.rom_size = load(argv[1], rom, sizeof rom);
    cfg.framebuffer = fb;
    cfg.audio_ring = audio; cfg.audio_frames = 4096;
    cfg.track = track;
    amiga_init(&cfg);
    amiga_set_chip_bus(fetch8 != 0);
    if (fetch8) amiga_set_chip_bus_params(fetch8, cost8);
    if (strcmp(argv[2], "-")) {
        const amiga_disk_t d = { disk_read, NULL, NULL, load(argv[2], adf, sizeof adf) };
        amiga_insert_disk(0, &d);
    }
    if (strcmp(argv[3], "-")) {
        FILE *f = fopen(argv[3], "rb");
        const amiga_stream_t io = { NULL, st_fread, f };
        if (!f || !amiga_state_load(&io)) { printf("%s: cannot load the snapshot\n", argv[3]); return 2; }
        fclose(f);
    }
    amiga_status_t s0, s1;
    for (int f = 0; f < warm; f++) amiga_run_frame();
    amiga_get_status(&s0);
    bench_mark();
    for (int f = 0; f < frames; f++) amiga_run_frame();
    amiga_get_status(&s1);
    printf("FRAMES %d\nM68K %lu\n", frames, (unsigned long)(s1.instr - s0.instr));
    printf("STATE pc=%06lX frame=%lu chipcrc=%08lX fbcrc=%08lX\n", (unsigned long)s1.pc, (unsigned long)s1.frame,
           (unsigned long)crc_of(chip, sizeof chip), (unsigned long)crc_of((const uint8_t *)fb, sizeof fb));
    return 0;
}

// ---- bare-metal start (semihosting for files and the console) --------------------
int _getentropy(void *p, size_t n) { (void)p; (void)n; return -1; }
extern char __bss_start__[], __bss_end__[];
void initialise_monitor_handles(void);
void __libc_init_array(void);
static char *argv_[10]; static char cmdline[1024];
struct shcmd { char *p; int len; };
static int sh(int op, void *a) {
    register int r0 __asm__("r0") = op; register void *r1 __asm__("r1") = a;
    __asm__ volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}
__attribute__((used, noreturn)) void start_c(void) {
    memset(__bss_start__, 0, (size_t)(__bss_end__ - __bss_start__));
    initialise_monitor_handles();
    __libc_init_array();
    struct shcmd c = { cmdline, (int)sizeof cmdline - 1 };
    int argc = 0;
    if (sh(0x15, &c) == 0) {
        // Arguments are separated by '|' (paths have spaces).
        char *s = cmdline;
        while (*s && argc < 9) { argv_[argc++] = s; while (*s && *s != '|') s++; if (*s) *s++ = 0; }
    }
    exit(main(argc, argv_));
}
__attribute__((naked, noreturn)) void _start(void) {
    __asm__ volatile("ldr sp, =0x10400000\n ldr r0, =0xE000ED88\n ldr r1, [r0]\n orr r1, r1, #(0xF<<20)\n str r1, [r0]\n dsb\n isb\n b start_c\n");
}
void *__dso_handle = 0; void _init(void) {} void _fini(void) {}
__attribute__((used)) void fault_c(uint32_t *f) {
    printf("FAULT pc=%08lx lr=%08lx r0=%08lx\n", (unsigned long)f[6], (unsigned long)f[5], (unsigned long)f[0]);
    exit(3);
}
__attribute__((naked)) void fault_entry(void) {
    __asm__ volatile("tst lr, #4\n ite eq\n mrseq r0, msp\n mrsne r0, psp\n ldr sp, =0x103F0000\n b fault_c\n");
}
