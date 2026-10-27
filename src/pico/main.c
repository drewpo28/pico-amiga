// pico-amiga firmware: an Amiga 500 on an RP2350 with QSPI PSRAM.
//
//   core 0: clocks, PSRAM, SD, USB host, the emulator (src/amiga), PWM audio
//   core 1: HDMI scanout (HSTX + DMA, see video_hstx.c)
//
// PSRAM layout (2 MB minimum):
//   +0.0 MB  chip RAM 512 KB      +0.5 MB  slow RAM 512 KB ($C00000)
//   +1.0 MB  Kickstart ROM        +1.5 MB  extended ROM (AROS) if present
//
// SD card files:
//   /amiga/kick.rom   256 or 512 KB Kickstart (required; AROS works)
//   /amiga/ext.rom    512 KB extended ROM at $E00000 (optional, AROS needs it)
//   /amiga/df0.adf    disk in DF0 (optional, streamed from the card)
//
// Boot failures paint the screen one colour and blink the LED:
//   red = no or too little PSRAM, magenta = no SD card, blue = no kick.rom.
#include <string.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/structs/sysinfo.h"
#include "ff.h"
#include "amiga.h"
#include "audio_pwm.h"
#include "clock.h"
#include "psram.h"
#include "usb_input.h"
#include "video_hstx.h"

#ifndef CPU_MHZ
#define CPU_MHZ 378
#endif
#ifndef BUTTER_PSRAM_GPIO
#define BUTTER_PSRAM_GPIO 8
#endif
#ifndef PWM_PIN0
#define PWM_PIN0 -1
#endif
#ifndef PWM_PIN1
#define PWM_PIN1 -1
#endif

#define KB (1024u)
#define MB (1024u * 1024u)

static uint16_t fb[AMIGA_FB_W * AMIGA_FB_H];
#define AUDIO_FRAMES 4096
static int16_t audio_ring[AUDIO_FRAMES * 2];

static FATFS fs;
static FIL   adf;
static bool  adf_open;

static bool chip_is_rp2350a(void) { return sysinfo_hw->package_sel & 1u; }

static void core1_main(void) {
    video_start(fb);
    for (;;) __wfi();
}

static void led_init(void) {
#ifdef PICO_DEFAULT_LED_PIN
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
#endif
}
static void led(bool on) {
#ifdef PICO_DEFAULT_LED_PIN
    gpio_put(PICO_DEFAULT_LED_PIN, on);
#else
    (void)on;
#endif
}

static void __attribute__((noreturn)) fail(uint16_t colour) {
    for (int i = 0; i < AMIGA_FB_W * AMIGA_FB_H; i++) fb[i] = colour;
    for (;;) {
        led(true);  sleep_ms(150);
        led(false); sleep_ms(850);
        usb_input_poll();
    }
}

static uint32_t load_file(const char *path, uint8_t *dst, uint32_t max) {
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return 0;
    UINT n = 0;
    FSIZE_t size = f_size(&f);
    if (size > max) size = max;
    if (f_read(&f, dst, (UINT)size, &n) != FR_OK) n = 0;
    f_close(&f);
    return n;
}

static int adf_read(void *ctx, uint32_t off, void *dst, uint32_t len) {
    FIL *f = ctx; UINT n;
    if (f_lseek(f, off) != FR_OK || f_read(f, dst, len, &n) != FR_OK || n != len) return -1;
    return 0;
}
static int adf_write(void *ctx, uint32_t off, const void *src, uint32_t len) {
    FIL *f = ctx; UINT n;
    if (f_lseek(f, off) != FR_OK || f_write(f, src, len, &n) != FR_OK || n != len) return -1;
    f_sync(f);
    return 0;
}

int main(void) {
    clock_overclock(CPU_MHZ);
    led_init();
    // PSRAM first: its bring-up runs QMI direct mode, during which nothing on
    // either core may fetch from flash (core 1 is not started yet, USB is off).
    // RP2350A carriers wire CS1 to BUTTER_PSRAM_GPIO, RP2350B modules to 47.
    uint32_t psram = psram_init(chip_is_rp2350a() ? BUTTER_PSRAM_GPIO : 47);
    multicore_launch_core1(core1_main);
    usb_input_init();
    if (psram < 2 * MB) fail(0xF00);
    uint8_t *ps = (uint8_t *)PSRAM_BASE;

    amiga_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.chip_ram = ps;              cfg.chip_size = 512 * KB;
    cfg.slow_ram = ps + 512 * KB;   cfg.slow_size = 512 * KB;
    uint8_t *rom = ps + 1 * MB, *ext = ps + 1 * MB + 512 * KB;

    if (f_mount(&fs, "", 1) != FR_OK) fail(0xF0F);
    uint32_t rs = load_file("/amiga/kick.rom", rom, 512 * KB);
    if (rs != 256 * KB && rs != 512 * KB) fail(0x00F);
    cfg.rom = rom; cfg.rom_size = rs;
    uint32_t es = load_file("/amiga/ext.rom", ext, 512 * KB);
    if (es == 512 * KB) { cfg.ext_rom = ext; cfg.ext_size = es; }

    cfg.framebuffer = fb;
    cfg.audio_ring = audio_ring;
    cfg.audio_frames = AUDIO_FRAMES;
    amiga_init(&cfg);

    if (f_open(&adf, "/amiga/df0.adf", FA_READ | FA_WRITE) == FR_OK ||
        f_open(&adf, "/amiga/df0.adf", FA_READ) == FR_OK) {
        adf_open = true;
        amiga_disk_t d = { adf_read, NULL, &adf, (uint32_t)f_size(&adf) };
        if (adf.flag & FA_WRITE) d.write = adf_write;
        amiga_insert_disk(0, &d);
    }

    audio_pwm_start(PWM_PIN1, PWM_PIN0, audio_ring, AUDIO_FRAMES);

    uint32_t vf = video_frames();
    for (uint32_t frame = 0;; frame++) {
        usb_input_poll();
        if (usb_input_reset_requested()) amiga_reset(false);
        amiga_run_frame();
        led((frame & 32) != 0);
        // Pace to the HDMI frame (50.06 Hz); a slow frame simply runs late.
        while (video_frames() == vf) { usb_input_poll(); }
        vf = video_frames();
    }
}
