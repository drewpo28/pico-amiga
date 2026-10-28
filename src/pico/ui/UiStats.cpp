// pico-amiga — the performance band (Alt+F12), after pico-speccy's F8 stats
// (OSD::drawStats): a few short lines in the UI face over the bottom of the
// picture, refreshed every 25 frames (~0.5 s).  The Amiga repaints the whole
// framebuffer each frame, so the band is kept as pixels and put back into each
// of its rows by the core's row_done hook (ui_stats_row) before the row reaches
// the framebuffer (amiga_set_overlay_rows): drawn after the frame it flickered,
// and put back after Denise's repaint a line could still slip through.  Alt+F12 steps
// off -> summary -> breakdown -> off.
//
// Summary:
//   EMU 14.2ms  141%    time amiga_run_frame() took, and the speed that makes
//                       (100% = exactly the 20 ms of a PAL frame)
//   FPS 50.0  378MHz    emulated frames per wall-clock second, the CPU clock
// Breakdown (amiga_get_profile, ms per Amiga frame):
//   CPU  9.1 REN 22.3   the 68000 (blits excluded), the Denise render (on core 1
//                       when it renders there)
//   COP  1.2 BLT  3.0   the copper (renders excluded), the blitter
//   OTH  4.5            Paula, floppy, CIAs, the scheduler
#include <stdio.h>
#include <string.h>
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
static bool     s_dirty;            // the text changed: draw the band again

// The band as drawn: framebuffer rectangle and its pixels.
enum { BAND_COLS = 19, BAND_W = (BAND_COLS + 2) * UI_FONT_W + 4, BAND_H = 3 * UI_FONT_H + 6 };
// In PSRAM (ui_stats_buffer): 9 KB of SRAM is most of the menu's heap. Read by
// ui_stats_row on the render core, a few KB a frame, only while the band is on.
static uint16_t (*s_px)[BAND_W];
static int      s_bx, s_by, s_bw, s_bh;

static void drawBand() {
    gfxComputeSurface();
    gfxInstallPalette();
    const int g = glyphW();
    // As wide as the longest line plus one blank column each side.
    int cols = 0;
    for (int i = 0; i < s_lines; i++) {
        const int n = (int)strlen(s_l[i]);
        if (n > cols) cols = n;
    }
    if (cols > BAND_COLS) cols = BAND_COLS;
    const int w = (cols + 2) * g + 4, h = s_lines * UI_FONT_H + 6;
    const int x = Sf.w - w - 4, y = Sf.h - h - 4;
    fill(x, y, w, h, C_PANEL);
    frame(x, y, w, h, C_SEP);
    for (int i = 0; i < s_lines; i++)      // one blank column of padding on the left
        text(x + 2 + g, y + 3 + i * UI_FONT_H, s_l[i], i ? C_TEXT : C_WHITE);
    s_bx = Sf.fbx + x; s_by = Sf.oy + y;
    s_bw = w < BAND_W ? w : BAND_W;
    s_bh = h < BAND_H ? h : BAND_H;
    for (int r = 0; r < s_bh; r++) {
        const uint16_t* src = gfxRowDirect(s_by + r);
        if (!src) { s_bh = 0; break; }
        for (int i = 0; i < s_bw; i++) s_px[r][i] = src[s_bx + i];
    }
    amiga_set_overlay_rows(s_by, s_by + s_bh);
}

}  // namespace nm

extern "C" size_t ui_stats_buffer_size(void) { return sizeof(uint16_t) * nm::BAND_H * nm::BAND_W; }

extern "C" void ui_stats_buffer(void* p) { nm::s_px = (uint16_t (*)[nm::BAND_W])p; }

extern "C" void ui_stats_toggle(void) {
    using namespace nm;
    if (!s_px) return;                  // no PSRAM to spare (a 2 MB chip): no band
    s_page = (uint8_t)((s_page + 1) % 3);
    s_frames = s_emu_sum = 0;
    s_t0 = time_us_32();
    amiga_get_profile(&s_prof, false);
    s_lines = s_page == 2 ? 3 : 2;
    for (int i = 0; i < 3; i++) snprintf(s_l[i], sizeof s_l[i], "  --");
    s_dirty = true;
    s_bh = 0;
    amiga_set_overlay_rows(0, 0);
}

extern "C" void ui_stats_row(void*, int row, amiga_row_t* r) {
    using namespace nm;
    if (!s_page || row < s_by || row >= s_by + s_bh) return;
    uint16_t* d = gfxRowToDirect(r);
    const uint16_t* s = s_px[row - s_by];
    for (int i = 0; i < s_bw; i++) d[s_bx + i] = s[i];
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
            ms(b, sizeof b, " REN", (uint64_t)p.ticks[AMIGA_PROF_RENDER] + p.ticks[AMIGA_PROF_BPL] + p.ticks[AMIGA_PROF_BPLRD] + p.ticks[AMIGA_PROF_CORE1], p.frames);
            snprintf(s_l[0], sizeof s_l[0], "%s%s", a, b);
            ms(a, sizeof a, "COP", p.ticks[AMIGA_PROF_COPPER], p.frames);
            ms(b, sizeof b, " BLT", p.ticks[AMIGA_PROF_BLIT], p.frames);
            snprintf(s_l[1], sizeof s_l[1], "%s%s", a, b);
            ms(a, sizeof a, "OTH", p.ticks[AMIGA_PROF_OTHER], p.frames);
            snprintf(s_l[2], sizeof s_l[2], "%s  %3uMHz", a, (unsigned)Config::cpu_mhz);
        }
        s_frames = s_emu_sum = 0;
        s_t0 = now;
        s_dirty = true;
    }
    if (s_dirty) { drawBand(); s_dirty = false; }
}
