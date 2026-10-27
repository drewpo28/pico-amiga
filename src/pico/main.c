// pico-amiga firmware: an Amiga 500 on an RP2350 with QSPI PSRAM.
//
//   core 0: clocks, PSRAM, SD, input (USB host, PS/2, NES pads), the emulator
//           (src/amiga), audio (PWM, I2S or PCM5122)
//   core 1: HDMI scanout (video_hstx.c, or video_pio.c on boards without HSTX
//           pins), or VGA (video_vga.c) when a VGA ladder is on the display pins
//
// PSRAM layout (2 MB minimum):
//   +0.0 MB  chip RAM 512 KB      +0.5 MB  slow RAM 512 KB ($C00000)
//   +1.0 MB  Kickstart ROM        +1.5 MB  extended ROM (AROS) if present
//
// ROMs: the built-in ones (roms.h, picked in Machine > Kickstart), or from the card:
//   /amiga/kick.rom   256 or 512 KB Kickstart (Kickstart = "SD card"; AROS works)
//   /amiga/ext.rom    512 KB extended ROM at $E00000 (optional, AROS needs it)
// Disks: Disks > DF0 picks an ADF on the card (streamed from it); without a pick,
//   /amiga/df0.adf goes into DF0 by itself.
// Settings: /.config/pico-amiga/<board>/storage.nvs (app/Config.h).
//
// Boot failures paint the screen one colour and blink the LED:
//   red = no or too little PSRAM, magenta = no ROM and no card, blue = no ROM.
#include <string.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/dma.h"
#include "hardware/structs/sysinfo.h"
#include "ff.h"
#include "amiga.h"
#include "audio_out.h"
#include "clock.h"
#include "psram.h"
#include "input.h"
#include "video.h"
#include "ui/ui.h"
#include "machine.h"
#include "board.h"
#include "dbg_uart.h"

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

static amiga_row_t fb[AMIGA_FB_H];
#define AUDIO_FRAMES 4096
static int16_t audio_ring[AUDIO_FRAMES * 2];

static FATFS fs;

static bool chip_is_rp2350a(void) { return sysinfo_hw->package_sel & 1u; }

static void core1_main(void) {
    video_start(fb);
    for (;;) __wfi();
}

// PICO_DEFAULT_LED_PIN 255 = no LED (ZERO2, the stock W module's is on the radio).
#if defined(PICO_DEFAULT_LED_PIN) && PICO_DEFAULT_LED_PIN < NUM_BANK0_GPIOS
#define HAVE_LED 1
#endif

static void led_init(void) {
#ifdef HAVE_LED
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
#endif
}
static void led(bool on) {
#ifdef HAVE_LED
    gpio_put(PICO_DEFAULT_LED_PIN, on);
#else
    (void)on;
#endif
}

static void __attribute__((noreturn)) fail(uint16_t colour) {
    amiga_fb_fill(fb, colour);
    for (;;) {
        led(true);  sleep_ms(150);
        led(false); sleep_ms(850);
        input_poll();
    }
}

// drivers/sdcard: the SPI divider follows clk_sys, re-derived after a clock change.
void sdcard_reclock(void);

int main(void) {
    led_init();
    // The card first, at the boot clock: storage.nvs says which clock to run at.
    const bool card = f_mount(&fs, "", 1) == FR_OK;
    if (card) ui_load_settings();
    const int mhz = clock_overclock(settings_cpu_mhz(), settings_flash_mhz());
    if (card) sdcard_reclock();
    if (dbg_uart_start(settings_dbg_uart()))
        printf("\npico-amiga " PORT_VERSION ": %d MHz, flash <= %d MHz, card %s\n",
               mhz, settings_flash_mhz(), card ? "ok" : "none");
    // PSRAM first: its bring-up runs QMI direct mode, during which nothing on
    // either core may fetch from flash (core 1 is not started yet, USB is off).
    // RP2350A carriers wire CS1 to BUTTER_PSRAM_GPIO, RP2350B modules to 47.
    uint32_t psram = psram_init(chip_is_rp2350a() ? BUTTER_PSRAM_GPIO : 47,
                                settings_psram_mhz());
    dma_channel_claim(0);            // video_hstx.c / video_pio.c scanout ping-pong
    dma_channel_claim(1);
    video_select();                  // VGA or HDMI on the display pins
    multicore_launch_core1(core1_main);
    audio_out_pcm5122_present();     // probe before the PS/2 keyboard takes GP2/3 (ZERO2)
    input_init();
    printf("psram %u KB (SCK <= %d MHz), video %s\n", (unsigned)(psram / KB), settings_psram_mhz(),
           video_is_vga() ? "VGA" : "HDMI");
    dbg_uart_pump();
    if (psram < 2 * MB) fail(0xF00);
    uint8_t *ps = (uint8_t *)PSRAM_BASE;

    amiga_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.chip_ram = ps;              cfg.chip_size = 512 * KB;
    cfg.slow_ram = ps + 512 * KB;   cfg.slow_size = 512 * KB;
    machine_init(ps + 1 * MB, ps + 1 * MB + 512 * KB);

    // The card holds the settings, the disks and an optional kick.rom; with a
    // built-in ROM the machine also boots without one.
    ui_init(fb);                     // the menu draws into fb
    if (!machine_select_rom(&cfg)) fail(card ? 0x00F : 0xF0F);

    cfg.framebuffer = fb;
    cfg.audio_ring = audio_ring;
    cfg.audio_frames = AUDIO_FRAMES;
    cfg.prof_clock = board_cycles;   // where the frame time goes (Alt+F12, second page)
    amiga_init(&cfg);
    if (card) machine_restore_disks();

    audio_out_start(audio_ring, AUDIO_FRAMES, settings_audio_out());
    settings_apply_sound();
    printf("kickstart %s, audio %s\n", machine_rom_name(), audio_out_name());

    uint32_t vf = video_frames();
    for (uint32_t frame = 0;; frame++) {
        input_poll();
        input_frame();
        if (input_reset_requested()) amiga_reset(false);
        const uint32_t t_emu = time_us_32();
        amiga_run_frame();
        const uint32_t emu_us = time_us_32() - t_emu;
        ui_stats_frame(emu_us);
        perf_log_frame(emu_us);
        // LED: slow blink = no keyboard, fast blink = keyboard mounted, steady = key held.
        led(input_key_held() || (frame & (input_keyboard_mounted() ? 8 : 32)) != 0);
        // Pace to the HDMI frame (50.06 Hz); a slow frame simply runs late.
        while (video_frames() == vf) { input_poll(); dbg_uart_pump(); }
        dbg_uart_pump();
        vf = video_frames();
    }
}
