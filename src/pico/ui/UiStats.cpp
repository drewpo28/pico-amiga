// pico-amiga — the performance band (Alt+F12), after pico-speccy's F8 stats
// (OSD::drawStats): two short lines in the UI face over the bottom of the picture,
// refreshed every 25 frames (~0.5 s) and redrawn after every emulated frame, since
// the Amiga repaints the whole framebuffer each frame.
//
//   EMU 14.2ms  141%    time amiga_run_frame() took, and the speed that makes
//                       (100% = exactly the 20 ms of a PAL frame)
//   FPS 50.0  378MHz    emulated frames per wall-clock second, the CPU clock
#include <stdio.h>
#include "ui.h"
#include "UiGfx.h"
#include "app/Config.h"
#include "pico/time.h"

namespace nm {

static bool     s_on;
static char     s_l1[24], s_l2[24];
static uint32_t s_frames, s_emu_sum, s_t0;

static void drawBand() {
    gfxComputeSurface();
    gfxInstallPalette();
    const int w = 18 * glyphW() + 4, h = 2 * UI_FONT_H + 4;
    const int x = Sf.w - w - 4, y = Sf.h - h - 4;
    fill(x, y, w, h, C_PANEL);
    frame(x, y, w, h, C_SEP);
    text(x + 2, y + 2, s_l1, C_WHITE);
    text(x + 2, y + 2 + UI_FONT_H, s_l2, C_TEXT);
}

}  // namespace nm

extern "C" void ui_stats_toggle(void) {
    nm::s_on = !nm::s_on;
    nm::s_frames = nm::s_emu_sum = 0;
    nm::s_t0 = time_us_32();
    snprintf(nm::s_l1, sizeof nm::s_l1, "EMU   --");
    snprintf(nm::s_l2, sizeof nm::s_l2, "FPS   --");
}

extern "C" void ui_stats_frame(uint32_t emu_us) {
    using namespace nm;
    if (!s_on) return;
    s_frames++;
    s_emu_sum += emu_us;
    if (s_frames >= 25) {
        const uint32_t now = time_us_32();
        const uint32_t avg = s_emu_sum / s_frames;
        const uint32_t pct = avg ? 2000000u / avg : 0;            // 20000 us = 100%
        const uint32_t fps10 = (uint32_t)((uint64_t)s_frames * 10000000u / (now - s_t0 ? now - s_t0 : 1));
        snprintf(s_l1, sizeof s_l1, "EMU %2u.%01ums %4u%%", (unsigned)(avg / 1000),
                 (unsigned)(avg % 1000 / 100), (unsigned)pct);
        snprintf(s_l2, sizeof s_l2, "FPS %2u.%01u  %3uMHz", (unsigned)(fps10 / 10),
                 (unsigned)(fps10 % 10), (unsigned)Config::cpu_mhz);
        s_frames = s_emu_sum = 0;
        s_t0 = now;
    }
    drawBand();
}
