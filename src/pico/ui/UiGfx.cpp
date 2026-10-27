// pico-amiga (from pico-speccy) — the new menu's own rasteriser (see UiGfx.h).
// PICO-AMIGA: draws 0x0RGB pixels straight into the Amiga framebuffer
// (AMIGA_FB_W x AMIGA_FB_H uint16) instead of palette indices into VIDEO::vga; the
// themes below are converted to RGB444 when installed. No DS80 backend.

#include "OSDNewMenu.h"

#include <string.h>

#include "UiGfx.h"
#include "UiFont.h"
#include "UiRender.h"
#include "app/Config.h"     // ui_vga_solid / ui_rounded / ui_theme — the menu-look preferences
extern "C" {
#include "amiga.h"
#include "video.h"
}

namespace nm {

Surface Sf;

// ── the UI palette ─────────────────────────────────────────────────────────────
// A cool neutral scheme: this is the whole point of owning the palette instead of
// borrowing the 16 ZX colours. Index order matches enum UiColor.
// Every channel below sits on a level whose HDMI pair is a SINGLE TMDS symbol
// (2026-08-30, hw-confirmed: colours come through a capture card correctly). Under HDMI_TMDS_BALANCED_PAIR the two output pixels of a doubled
// framebuffer pixel normally carry v and v±1, which is invisible on a monitor — but
// an HDMI capture card here has a pixel-phase-dependent stage that turns any such
// pair into two WILDLY different colours: C_ACCENT 0x4ADE80 came back as
// (209,252,255) on even pixels and (0,8,30) on odd ones, measured 1:1 against a
// framebuffer dump. Colours whose pair is one repeated symbol have no phase for that
// stage to act on and come through exactly — which is why the ZX Spectrum theme, all
// of whose values happen to be single-symbol, always rendered correctly on the same
// card while this table did not.
//
// Computed against HDMI_TMDS_LEVEL_HI = 0xF6: the ceiling decides where a channel
// above it lands, so these two numbers are COUPLED — re-run the snap if it moves.
// 118 of the 256 levels are single-symbol, so snapping costs almost nothing: the
// worst channel here moved 4 code units (1.6% of full scale) and most moved 1-2.
// The old values are in the comments. Re-check with tools/ if a colour is ever
// retuned — the property is invisible on a monitor and only a capture reveals it.
// (kUiPaletteVga and kUiPaletteZx need no such treatment: every level on the
// {00,55,AA,FF} DAC grid is single-symbol already — all 64 grid colours verified.)
static const uint32_t kUiPalette[C_COUNT] = {
    0x101116,   // C_BG         was 0x0F1218
    0x1C232F,   // C_PANEL      was 0x1E2431
    0x262D3A,   // C_PANEL_ALT  was 0x262D3C
    0x343A4B,   // C_SEP        was 0x333B4D
    0xE8EDEF,   // C_TEXT       was 0xE6EBF2
    0x8B95A7,   // C_TEXT_DIM   already single-symbol
    0xFFFFFF,   // C_WHITE      already single-symbol
    0x3A6DF6,   // C_SEL_BG     was 0x3B6EF5
    0x2A3445,   // C_SEL_BAND   was 0x2B3346
    0x4ADD84,   // C_ACCENT     was 0x4ADE80
    0x16212A,   // C_FOOT_BG    was 0x191F2A
    0x081013,   // C_SHADOW     was 0x0B0E14
    0xE23A3A,   // C_ICON_R     was 0xE23B3B
    0xEFC43A,   // C_ICON_Y     was 0xF2C43B
    0x3AC7E2,   // C_ICON_C     was 0x3BC7E2
    0x5A6477,   // C_DISABLED   was 0x5A6478
};

// VGA twin of kUiPalette: every channel sits on the 2-bit DAC grid {00,55,AA,FF}.
// An on-grid RGB888 makes vga_bayer4() come out with sub=0 for all channels, so the
// ordinary dithered path renders it SOLID — the menu loses the Bayer checkerboard on
// VGA without touching the driver. (The CRT grille / scanline taps attenuate colours
// off the grid and re-dither them, same as they do to the 16 solid ZX colours.)
// Hand-picked, not nearest-rounded: 4 levels per channel cannot keep the slate
// theme's subtle darks apart, so the dark ladder is re-spread over black → navy →
// blue while text, selection and icon hues stay nearest-grid.
static const uint32_t kUiPaletteVga[C_COUNT] = {
    0x000000,   // C_BG
    0x000055,   // C_PANEL
    0x0000AA,   // C_PANEL_ALT
    0x555555,   // C_SEP
    0xFFFFFF,   // C_TEXT      (merges with C_WHITE — they were 25 units apart anyway)
    0xAAAAAA,   // C_TEXT_DIM
    0xFFFFFF,   // C_WHITE
    0x5555FF,   // C_SEL_BG
    0x0000AA,   // C_SEL_BAND  (shares C_PANEL_ALT's slot value; the roles never abut)
    0x55FFAA,   // C_ACCENT
    0x000000,   // C_FOOT_BG   (darker than the panel, same as the original relation)
    0x000000,   // C_SHADOW
    0xFF5555,   // C_ICON_R
    0xFFAA55,   // C_ICON_Y
    0x55AAFF,   // C_ICON_C
    0x555555,   // C_DISABLED
};

// "ZX Spectrum" theme (Options > Theme): the classic pico-spec cascade menu's colours,
// read off its OSDMenu.cpp — black ink on bright-white paper (zxColor 0,1 / 7,1),
// bright-cyan selection with black text (0,1 / 5,1), normal cyan for the focused row
// of the unfocused pane (the classic dimmed selection paper 5,0), blue secondary text,
// a cyan footer (BASIC edit-line look) and the bright rainbow. The emphasis role
// C_WHITE becomes black here: in a light theme the ink that reads on the selection
// bar and the header IS the ink. Every channel sits on {00,AA,FF} — the VGA DAC grid
// (normal=0xAA, bright=0xFF) — so this theme renders solid on VGA by construction and
// needs no dithered/solid twin.
static const uint32_t kUiPaletteZx[C_COUNT] = {
    0x000000,   // C_BG        black backdrop (the classic border)
    0xFFFFFF,   // C_PANEL     bright white paper
    0xAAAAAA,   // C_PANEL_ALT normal white bands
    0x000000,   // C_SEP       black rules, the classic window border
    0x000000,   // C_TEXT      black ink
    0x0000AA,   // C_TEXT_DIM  normal blue secondary text
    0x000000,   // C_WHITE     emphasis ink (black on cyan/white, as the classic menu)
    0x00FFFF,   // C_SEL_BG    bright cyan selection bar
    0x00AAAA,   // C_SEL_BAND  normal cyan — the classic dimmed selection
    0x00FF00,   // C_ACCENT    bright green (radio, rainbow stripe)
    0x00AAAA,   // C_FOOT_BG   cyan footer, blue hints on it
    0x000000,   // C_SHADOW
    0xFF0000,   // C_ICON_R    bright red
    0xFFFF00,   // C_ICON_Y    bright yellow
    0x00FFFF,   // C_ICON_C    bright cyan
    0xAAAAAA,   // C_DISABLED  normal white — the classic dimmed ink
};

// The palette the active output actually shows. The ZX theme is one table for every
// output (its values are on the VGA grid already); the Slate theme keeps the
// VGA solid/dithered choice — the on-grid twin unless the user prefers the
// full-depth scheme through the dither (Interface > Theme > VGA menu colors).
//
// ...and that choice only exists while there IS a dither. With per-pixel PWM the
// full-depth scheme renders exactly, so the on-grid twin would be a pointless
// quantisation to 64 colours — the twin exists to stop the Bayer block shimmering,
// and PWM has no block. The row is hidden in the same case (p_vgaDither).
static const uint32_t* uiPaletteActive() {
    if (Config::ui_theme == 1) return kUiPaletteZx;
    // VGA: the on-grid twin renders solid through video_vga.c's Bayer dither.
    if (video_is_vga() && Config::ui_vga_solid) return kUiPaletteVga;
    return kUiPalette;
}

static uint16_t* s_fb;              // the framebuffer the menu draws into
static uint16_t  s_pal[C_COUNT];    // the active theme as 0x0RGB

void gfxSetFramebuffer(uint16_t* fb) { s_fb = fb; }

static inline uint16_t rgb444(uint32_t c) {
    // Nearest 4-bit level per channel: (v * 15 + 127) / 255.
    const uint32_t r = (((c >> 16) & 0xFF) * 15 + 127) / 255;
    const uint32_t g = (((c >>  8) & 0xFF) * 15 + 127) / 255;
    const uint32_t b = (( c        & 0xFF) * 15 + 127) / 255;
    return (uint16_t)(r << 8 | g << 4 | b);
}

// ── surface / palette lifecycle ────────────────────────────────────────────────

void gfxComputeSurface() {
    memset(&Sf, 0, sizeof(Sf));
    Sf.w = AMIGA_FB_W;
    Sf.h = AMIGA_FB_H;
    Sf.glyphScale = 1;
}

// The framebuffer holds colours, not indices: a theme change takes a full redraw.
void gfxInstallPalette() {
    const uint32_t* pal = uiPaletteActive();
    for (int i = 0; i < C_COUNT; i++) s_pal[i] = rgb444(pal[i]);
}

void gfxBegin() {
    gfxComputeSurface();
    gfxInstallPalette();
}

void gfxSuspendPalette() {}
void gfxResumePalette() {}
const uint32_t* uiPalette() { return uiPaletteActive(); }
uint16_t        uiColor444(UiColor c) { return s_pal[c]; }
void gfxEnd() {}

// ── pixels ─────────────────────────────────────────────────────────────────────

static inline uint16_t* fbRow(int y) {
    return s_fb ? s_fb + (Sf.oy + y) * AMIGA_FB_W + Sf.fbx : nullptr;
}

void px(int x, int y, UiColor c) {
    if (x < 0 || y < 0 || x >= Sf.w || y >= Sf.h) return;
    uint16_t* row = fbRow(y);
    if (!row) return;
    row[x] = s_pal[c];
}

void hline(int x, int y, int w, UiColor c) {
    if (y < 0 || y >= Sf.h || w <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (x + w > Sf.w) w = Sf.w - x;
    if (w <= 0) return;
    uint16_t* row = fbRow(y);
    if (!row) return;
    const uint16_t v = s_pal[c];
    for (int i = 0; i < w; i++) row[x + i] = v;
}

void vline(int x, int y, int h, UiColor c) {
    if (y < 0) { h += y; y = 0; }
    if (y + h > Sf.h) h = Sf.h - y;
    for (int i = 0; i < h; i++) px(x, y + i, c);
}

void fill(int x, int y, int w, int h, UiColor c) {
    if (y < 0) { h += y; y = 0; }
    if (y + h > Sf.h) h = Sf.h - y;
    for (int i = 0; i < h; i++) hline(x, y + i, w, c);
}

void fillRgb(int x, int y, int w, int h, uint16_t rgb444) {
    if (x < 0) { w += x; x = 0; }
    if (x + w > Sf.w) w = Sf.w - x;
    if (y < 0) { h += y; y = 0; }
    if (y + h > Sf.h) h = Sf.h - y;
    for (int j = 0; j < h; j++) {
        uint16_t* row = fbRow(y + j);
        if (!row) continue;
        for (int i = 0; i < w; i++) row[x + i] = rgb444;
    }
}

void frame(int x, int y, int w, int h, UiColor c) {
    if (w <= 0 || h <= 0) return;
    hline(x, y, w, c);
    hline(x, y + h - 1, w, c);
    vline(x, y, h, c);
    vline(x + w - 1, y, h, c);
}

// ── text ───────────────────────────────────────────────────────────────────────

static void glyph(int x, int y, char ch, UiColor ink) {
    const uint8_t* g = uiGlyph(ch);
    const int sc = Sf.glyphScale;
    for (int r = 0; r < UI_FONT_H; r++) {
        uint8_t bits = g[r];
        if (!bits) continue;
        for (int b = 0; b < UI_FONT_W; b++) {
            if (!(bits & (0x20 >> b))) continue;
            if (sc == 1) px(x + b, y + r, ink);
            else for (int s = 0; s < sc; s++) px(x + b * sc + s, y + r, ink);
        }
    }
}

int textWidth(const char* s) {
    if (!s) return 0;
    return (int)strlen(s) * glyphW();
}

int text(int x, int y, const char* s, UiColor ink) {
    if (!s) return 0;
    const int adv = glyphW();
    int cx = x;
    for (; *s; ++s) {
        if (cx >= Sf.w) break;
        if (*s != ' ') glyph(cx, y, *s, ink);   // space costs nothing to draw
        cx += adv;
    }
    return cx - x;
}

int textClip(int x, int y, int maxw, const char* s, UiColor ink) {
    if (!s || maxw <= 0) return 0;
    const int adv = glyphW();
    const int fits = maxw / adv;
    const int len = (int)strlen(s);
    if (len <= fits) return text(x, y, s, ink);
    // Truncate and mark it: two dots cost one cell each, so keep fits-2 characters.
    int keep = fits - 2;
    if (keep < 0) keep = 0;
    int cx = x;
    for (int i = 0; i < keep; i++, cx += adv)
        if (s[i] != ' ') glyph(cx, y, s[i], ink);
    for (int i = 0; i < 2 && cx < x + maxw; i++, cx += adv) glyph(cx, y, '.', ink);
    return cx - x;
}

// ── widgets ────────────────────────────────────────────────────────────────────

// A DS80 logical pixel is half as WIDE as a standard one (512 vs 320 across the same
// screen) and exactly as tall, so any widget that should look square must be scaled
// horizontally by glyphScale. Hairline rules are deliberately left unscaled: there they
// are genuinely finer, which is the point of rendering natively in DS80.
void radio(int x, int y, bool on, UiColor ink, UiColor accent) {
    const int sc = Sf.glyphScale;
    const int w = 5 * sc, h = 5;
    hline(x + sc, y,         w - 2 * sc, ink);
    hline(x + sc, y + h - 1, w - 2 * sc, ink);
    fill (x,          y + 1, sc, h - 2, ink);
    fill (x + w - sc, y + 1, sc, h - 2, ink);
    if (on) fill(x + sc, y + 1, w - 2 * sc, h - 2, accent);
}

int radioW() { return 5 * Sf.glyphScale; }

void chevron(int x, int y, UiColor c) {
    const int sc = Sf.glyphScale;
    for (int i = 0; i < 4; i++) {
        fill(x + i * sc, y + i,     sc, 1, c);
        fill(x + i * sc, y + 6 - i, sc, 1, c);
    }
}

int chevronW() { return 4 * Sf.glyphScale; }

// Corner cut-back table for r<=4: how many pixels to inset on each of the first r rows.
// Quarter-circle-ish: the outermost row cuts deepest so the curve actually reads.
static const uint8_t kCorner[5] = { 0, 1, 1, 2, 4 };

static inline int cornerCut(int i, int h, int r) {
    if (i < r)           return kCorner[r - i];
    if (i >= h - r)      return kCorner[r - (h - 1 - i)];
    return 0;
}

void roundRect(int x, int y, int w, int h, int r, UiColor border, UiColor fill_c) {
    if (w <= 0 || h <= 0) return;
    if (!Config::ui_rounded) r = 0;    // Options > Menu corners: Square
    if (r > 4) r = 4;
    const int sc = Sf.glyphScale;           // corners stay square on screen in DS80
    for (int i = 0; i < h; i++) {
        const int cut = cornerCut(i, h, r) * sc;
        hline(x + cut, y + i, w - 2 * cut, fill_c);
    }
    // Border. In the corner region each row's segment stretches to the deeper cut
    // of its neighbouring row, so the steps CONNECT into a curve instead of
    // leaving single pixels with gaps between them.
    for (int i = 0; i < h; i++) {
        const int cut = cornerCut(i, h, r) * sc;
        if (i == 0 || i == h - 1) { hline(x + cut, y + i, w - 2 * cut, border); continue; }
        const int above = cornerCut(i - 1, h, r) * sc;
        const int below = cornerCut(i + 1, h, r) * sc;
        const int adj   = above > below ? above : below;
        const int wseg  = (adj > cut) ? (adj - cut + sc) : sc;
        fill(x + cut, y + i, wseg, 1, border);
        fill(x + w - cut - wseg, y + i, wseg, 1, border);
    }
}

void roundRectBorder(int x, int y, int w, int h, int r, UiColor border, UiColor outside) {
    if (w <= 0 || h <= 0) return;
    if (!Config::ui_rounded) r = 0;    // Options > Menu corners: Square
    if (r > 4) r = 4;
    const int sc = Sf.glyphScale;
    for (int i = 0; i < h; i++) {
        const int cut = cornerCut(i, h, r) * sc;
        if (cut) {                              // clear the outside of the curve
            fill(x, y + i, cut, 1, outside);
            fill(x + w - cut, y + i, cut, 1, outside);
        }
        if (i == 0 || i == h - 1) { hline(x + cut, y + i, w - 2 * cut, border); continue; }
        const int above = cornerCut(i - 1, h, r) * sc;
        const int below = cornerCut(i + 1, h, r) * sc;
        const int adj   = above > below ? above : below;
        const int wseg  = (adj > cut) ? (adj - cut + sc) : sc;
        fill(x + cut, y + i, wseg, 1, border);
        fill(x + w - cut - wseg, y + i, wseg, 1, border);
    }
}

// ZX rainbow: four slanted bands. 11x9 logical px (x2 wide in DS80).
void rainbow(int x, int y) {
    const int sc = Sf.glyphScale;
    for (int r = 0; r < 9; r++) {
        for (int c = 0; c < 11; c++) {
            const int k = c + (8 - r) / 3;
            const UiColor col = (k < 3) ? C_ACCENT : (k < 6) ? C_ICON_Y
                              : (k < 9) ? C_ICON_R : C_ICON_C;
            fill(x + c * sc, y + r, sc, 1, col);
        }
    }
}

int rainbowW() { return 11 * Sf.glyphScale; }

void icon1bpp(int x, int y, const uint16_t* rows, int n, int wbits, UiColor c) {
    const int sc = Sf.glyphScale;
    for (int r = 0; r < n; r++)
        for (int b = 0; b < wbits; b++)
            if (rows[r] & (1 << (wbits - 1 - b)))
                fill(x + b * sc, y + r, sc, 1, c);
}

} // namespace nm

