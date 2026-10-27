// The "PERF" line of the debug log (dbg_uart.h), shared by the board and the PC
// host (where printf is the terminal).
#include <stdio.h>
#include "dbg_uart.h"
#include "amiga.h"
#include "board.h"
#include "pico/time.h"
#include "ui/ui.h"
#include "audio_out.h"

// The QMI SCK a limit gives: clk_sys over the smallest whole divider that keeps
// under it, never below 2 (clock.c / psram.c).
static unsigned sck(unsigned mhz, unsigned limit) {
    unsigned d = (mhz + limit - 1) / limit;
    if (d < 2) d = 2;
    return mhz / d;
}

void perf_log_frame(uint32_t emu_us) {
    static uint32_t frames, emu_sum, t0, slow;
    static amiga_profile_t last;
    static uint32_t isr_last;
    if (!dbg_uart_active()) return;
    if (!t0) { t0 = time_us_32(); amiga_get_profile(&last, false); isr_last = audio_out_isr_cycles(); return; }
    frames++;
    emu_sum += emu_us;
    if (emu_us > 20000) slow++;
    const uint32_t now = time_us_32();
    if (now - t0 < 1000000u) return;

    amiga_profile_t p;
    amiga_get_profile(&p, false);
    const uint32_t nf = p.frames - last.frames ? p.frames - last.frames : 1;
    const uint32_t cpu_us = board_cycles_per_us();
    uint32_t ms10[AMIGA_PROF_COUNT];                 // tenths of a ms per frame
    for (int i = 0; i < AMIGA_PROF_COUNT; i++)
        ms10[i] = (uint32_t)((uint64_t)(p.ticks[i] - last.ticks[i]) / nf / cpu_us / 100);
    const uint32_t isr = audio_out_isr_cycles();
    const uint32_t irq10 = (isr - isr_last) / nf / cpu_us / 100;   // the audio tick, per frame
    isr_last = isr;
    const uint32_t emu_avg = emu_sum / frames, emu10 = emu_avg / 100;
    const uint32_t fps10 = (uint32_t)((uint64_t)frames * 10000000u / (now - t0));
#define MS(i) (unsigned)(ms10[i] / 10), (unsigned)(ms10[i] % 10)
    printf("PERF fps %u.%u emu %u.%u ms (%u%%, %u slow) | cpu %u.%u ren %u.%u+bpl %u.%u cop %u.%u blt %u.%u"
           " aud %u.%u line %u.%u dsk %u.%u oth %u.%u (irq %u.%u) | cpu %u flash %u psram %u MHz\n",
           (unsigned)(fps10 / 10), (unsigned)(fps10 % 10),
           (unsigned)(emu10 / 10), (unsigned)(emu10 % 10),
           (unsigned)(emu_avg ? 2000000u / emu_avg : 0), (unsigned)slow,     // 20 ms = 100%
           MS(AMIGA_PROF_CPU), MS(AMIGA_PROF_RENDER), MS(AMIGA_PROF_BPL), MS(AMIGA_PROF_COPPER), MS(AMIGA_PROF_BLIT),
           MS(AMIGA_PROF_PAULA), MS(AMIGA_PROF_LINE), MS(AMIGA_PROF_DISK), MS(AMIGA_PROF_OTHER),
           (unsigned)(irq10 / 10), (unsigned)(irq10 % 10),
           (unsigned)settings_cpu_mhz(), sck((unsigned)settings_cpu_mhz(), (unsigned)settings_flash_mhz()),
           sck((unsigned)settings_cpu_mhz(), settings_psram_mhz() < 135 ? (unsigned)settings_psram_mhz() : 135u));
    last = p;
    frames = emu_sum = slow = 0;
    t0 = now;
}
