// The "PERF" line of the debug log (dbg_uart.h), shared by the board and the PC
// host (where printf is the terminal).
#include <stdio.h>
#include "dbg_uart.h"
#include "amiga.h"
#include "board.h"
#include "pico/time.h"
#include "ui/ui.h"
#include "audio_out.h"
#include "video.h"
#include "hdmi_audio.h"

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
    static uint32_t isr_last, late_last, c1irq_last, c1enc_last, hold_last, skip_last, full_last, starve_last, miss_last;
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
    const uint32_t late = video_late_lines() - late_last;   // video lines, this second
    late_last += late;
    const uint32_t miss = video_irq_missed() - miss_last;
    miss_last += miss;
    uint32_t c1irq, c1enc;                                  // core 1's video work, per frame
    video_core1_cycles(&c1irq, &c1enc);
    const uint32_t irq1 = (c1irq - c1irq_last) / nf / cpu_us / 100, enc1 = (c1enc - c1enc_last) / nf / cpu_us / 100;
    c1irq_last = c1irq; c1enc_last = c1enc;
    uint32_t hold, skip, full, starve, qmin;                // audio, this second
    audio_out_counts(&hold, &skip);
    hdmi_audio_counts(&full, &starve, &qmin);
    const uint32_t dh = hold - hold_last, ds = skip - skip_last, df = full - full_last, dst = starve - starve_last;
    hold_last = hold; skip_last = skip; full_last = full; starve_last = starve;
    const uint32_t emu_avg = emu_sum / frames, emu10 = emu_avg / 100;
    const uint32_t fps10 = (uint32_t)((uint64_t)frames * 10000000u / (now - t0));
#define MS(i) (unsigned)(ms10[i] / 10), (unsigned)(ms10[i] % 10)
    printf("PERF fps %u.%u emu %u.%u ms (%u%%, %u slow) | cpu %u.%u ren %u.%u+bpl %u.%u+rd %u.%u cop %u.%u blt %u.%u"
           " aud %u.%u line %u.%u dsk %u.%u oth %u.%u wait %u.%u (irq %u.%u) | core1 render %u.%u irq %u.%u enc %u.%u"
           " | cpu %u flash %u psram %u MHz | late %u irqmiss %u | au hold %u drop %u hdmi starve %u full %u min %u\n",
           (unsigned)(fps10 / 10), (unsigned)(fps10 % 10),
           (unsigned)(emu10 / 10), (unsigned)(emu10 % 10),
           (unsigned)(emu_avg ? 2000000u / emu_avg : 0), (unsigned)slow,     // 20 ms = 100%
           MS(AMIGA_PROF_CPU), MS(AMIGA_PROF_RENDER), MS(AMIGA_PROF_BPL), MS(AMIGA_PROF_BPLRD), MS(AMIGA_PROF_COPPER), MS(AMIGA_PROF_BLIT),
           MS(AMIGA_PROF_PAULA), MS(AMIGA_PROF_LINE), MS(AMIGA_PROF_DISK), MS(AMIGA_PROF_OTHER),
           MS(AMIGA_PROF_RWAIT), (unsigned)(irq10 / 10), (unsigned)(irq10 % 10), MS(AMIGA_PROF_CORE1),
           (unsigned)(irq1 / 10), (unsigned)(irq1 % 10), (unsigned)(enc1 / 10), (unsigned)(enc1 % 10),
           (unsigned)settings_cpu_mhz(), sck((unsigned)settings_cpu_mhz(), (unsigned)settings_flash_mhz()),
           sck((unsigned)settings_cpu_mhz(), settings_psram_mhz() < 135 ? (unsigned)settings_psram_mhz() : 135u),
           (unsigned)late, (unsigned)miss, (unsigned)dh, (unsigned)ds, (unsigned)dst, (unsigned)df, (unsigned)qmin);
    last = p;
    frames = emu_sum = slow = 0;
    t0 = now;
}
