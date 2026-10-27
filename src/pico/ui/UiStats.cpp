// pico-amiga — the performance band (Alt+F12), after pico-speccy's F8 stats
// (OSD::drawStats): a few short lines in the UI face over the bottom of the
// picture, refreshed every 25 frames (~0.5 s) and redrawn after every emulated
// frame, since the Amiga repaints the whole framebuffer each frame. Alt+F12 steps
// off -> summary -> breakdown -> off.
//
// Summary:
//   EMU 14.2ms  141%    time amiga_run_frame() took, and the speed that makes
//                       (100% = exactly the 20 ms of a PAL frame)
//   FPS 50.0  378MHz    emulated frames per wall-clock second, the CPU clock
// Breakdown (amiga_get_profile, ms per Amiga frame):
//   CPU  9.1 REN 22.3   the 68000 (blits excluded), the Denise render
//   COP  1.2 BLT  3.0   the copper (renders excluded), the blitter
//   OTH  4.5            Paula, floppy, CIAs, the scheduler
#include <stdio.h>
#include "ui.h"
#include "UiGfx.h"
#include "app/Config.h"
#include "pico/time.h"
extern "C" {
#include "amiga.h"
#include "board.h"
}

namespace nm {

static uint8_t  s_page;             // 0 off, 1 summary, 2 breakdown
static char     s_l[3][24];
static int      s_lines;
static uint32_t s_frames, s_emu_sum, s_t0;
static amiga_profile_t s_prof;       // the profile at the last refresh (perf_log.c keeps its own)

static void drawBand() {
    gfxComputeSurface();
    gfxInstallPalette();
    const int w = 18 * glyphW() + 4, h = s_lines * UI_FONT_H + 4;
    const int x = Sf.w - w - 4, y = Sf.h - h - 4;
    fill(x, y, w, h, C_PANEL);
    frame(x, y, w, h, C_SEP);
    for (int i = 0; i < s_lines; i++)
        text(x + 2, y + 2 + i * UI_FONT_H, s_l[i], i ? C_TEXT : C_WHITE);
}

}  // namespace nm

extern "C" void ui_stats_toggle(void) {
    using namespace nm;
    s_page = (uint8_t)((s_page + 1) % 3);
    s_frames = s_emu_sum = 0;
    s_t0 = time_us_32();
    amiga_get_profile(&s_prof, false);
    s_lines = s_page == 2 ? 3 : 2;
    for (int i = 0; i < 3; i++) snprintf(s_l[i], sizeof s_l[i], "  --");
}

// ticks per frame as "mm.m" milliseconds
static void ms(char* out, size_t n, const char* tag, uint64_t ticks, uint32_t frames) {
    const uint32_t us = (uint32_t)(ticks / (frames ? frames : 1) / board_cycles_per_us());
    snprintf(out, n, "%s%3u.%01u", tag, (unsigned)(us / 1000), (unsigned)(us % 1000 / 100));
}

extern "C" void ui_stats_frame(uint32_t emu_us) {
    using namespace nm;
    if (!s_page) return;
    s_frames++;
    s_emu_sum += emu_us;
    if (s_frames >= 25) {
        const uint32_t now = time_us_32();
        const uint32_t avg = s_emu_sum / s_frames;
        const uint32_t pct = avg ? 2000000u / avg : 0;            // 20000 us = 100%
        const uint32_t fps10 = (uint32_t)((uint64_t)s_frames * 10000000u / (now - s_t0 ? now - s_t0 : 1));
        if (s_page == 1) {
            snprintf(s_l[0], sizeof s_l[0], "EMU %2u.%01ums %4u%%", (unsigned)(avg / 1000),
                     (unsigned)(avg % 1000 / 100), (unsigned)pct);
            snprintf(s_l[1], sizeof s_l[1], "FPS %2u.%01u  %3uMHz", (unsigned)(fps10 / 10),
                     (unsigned)(fps10 % 10), (unsigned)Config::cpu_mhz);
        } else {
            amiga_profile_t p, now_p;
            amiga_get_profile(&now_p, false);
            for (int i = 0; i < AMIGA_PROF_COUNT; i++) p.ticks[i] = now_p.ticks[i] - s_prof.ticks[i];
            p.frames = now_p.frames - s_prof.frames;
            s_prof = now_p;
            char a[12], b[12];
            ms(a, sizeof a, "CPU", p.ticks[AMIGA_PROF_CPU], p.frames);
            ms(b, sizeof b, " REN", p.ticks[AMIGA_PROF_RENDER], p.frames);
            snprintf(s_l[0], sizeof s_l[0], "%s%s", a, b);
            ms(a, sizeof a, "COP", p.ticks[AMIGA_PROF_COPPER], p.frames);
            ms(b, sizeof b, " BLT", p.ticks[AMIGA_PROF_BLIT], p.frames);
            snprintf(s_l[1], sizeof s_l[1], "%s%s", a, b);
            ms(a, sizeof a, "OTH", p.ticks[AMIGA_PROF_OTHER], p.frames);
            snprintf(s_l[2], sizeof s_l[2], "%s  %3uMHz", a, (unsigned)Config::cpu_mhz);
        }
        s_frames = s_emu_sum = 0;
        s_t0 = now;
    }
    drawBand();
}
