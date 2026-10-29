// pico-amiga (from pico-speccy) — the new menu's own rasteriser.
//
// PICO-AMIGA: one backend: 0x0RGB pixels written straight into the Amiga
// framebuffer (gfxSetFramebuffer), which the video scanout is showing. The 16 UI
// colours are a theme table converted to RGB444 at gfxBegin(). Coordinates are
// framebuffer pixels (360x288). Everything clips. (pico-speccy's 8-bit indexed and
// Profi DS80 backends are gone; glyphScale stays 1.)

#pragma once


#include <stdint.h>
#include "amiga.h"

#include "UiFont.h"

namespace nm {

// The 16 UI colours. Values are logical slots, not palette indices.
// Budget note: exactly 16 — duplicates were merged (one white serves text/header/
// selection) to free four slots for the rainbow mark in the header.
enum UiColor : uint8_t {
    C_BG = 0,       // outside the rounded window
    C_PANEL,        // window and pane background
    C_PANEL_ALT,    // header / sub-header / pane-title bands
    C_SEP,          // rules and the window border
    C_TEXT,         // primary text
    C_TEXT_DIM,     // secondary text, footer hints
    C_WHITE,        // emphasis, text on the selection bar
    C_SEL_BG,       // selection bar, focused pane
    C_SEL_BAND,     // subtle band for the focused row of the unfocused pane
    C_ACCENT,       // chosen radio dot
    C_FOOT_BG,
    C_SHADOW,
    C_ICON_R,       // rainbow mark
    C_ICON_Y,
    C_ICON_C,
    C_DISABLED,     // "<< Back" and non-selectable rows
    C_COUNT
};

struct Surface {
    bool ds80;
    int  w, h;          // logical pixel extent of the drawable area
    int  fbx;           // framebuffer byte column of logical x=0 (standard mode)
    int  pad;           // DS80: framebuffer byte column of logical x=0 (=pad_l)
    int  oy;            // framebuffer row of logical y=0
    int  glyphScale;    // horizontal glyph scale: 1 standard, 2 DS80
};
extern Surface Sf;

// The framebuffer to draw into (the firmware's AMIGA_FB_H rows; the menu draws
// AMIGA_FB_W x AMIGA_FB_H lowres pixels, turning each row it touches DIRECT).
void gfxSetFramebuffer(amiga_row_t* fb);
// Framebuffer row `row` as 0x0RGB, turned into a DIRECT row first if need be.
uint16_t* gfxRowDirect(int row);
uint16_t* gfxRowToDirect(amiga_row_t* r);   // the same for any row
// Computes the surface from the live video mode. Cheap, no side effects — safe to call
// just to test whether the layout fits.
void gfxComputeSurface();
// Computes the surface AND installs the 16 UI colours. Call once per menu open.
void gfxBegin();
// Re-installs the 16 UI colours. Needed after anything that rewrites the hardware
// palette under us (VIDEO::applyPalette() rewrites indices 0..239, ours included).
void gfxInstallPalette();
// Restores whatever the palette was before gfxBegin().
void gfxEnd();

// The 16 UI colours (RGB888, enum UiColor order), and one as the 0x0RGB it is drawn with.
const uint32_t* uiPalette();
uint16_t        uiColor444(UiColor c);
// Hand the palette back to the emulator for the duration of a reused modal dialog
// (they draw with zxColor() indices, which in DS80 are our entries), then take it back.
void gfxSuspendPalette();
void gfxResumePalette();

// Metrics in logical pixels.
inline int glyphW() { return UI_FONT_W * Sf.glyphScale; }
inline int cols()   { return Sf.w / glyphW(); }
inline int rows()   { return Sf.h / UI_FONT_H; }

void px(int x, int y, UiColor c);
void hline(int x, int y, int w, UiColor c);
void vline(int x, int y, int h, UiColor c);
void fill(int x, int y, int w, int h, UiColor c);
void frame(int x, int y, int w, int h, UiColor c);
// Fill with a raw 0x0RGB colour instead of a UI colour.
void fillRgb(int x, int y, int w, int h, uint16_t rgb444);
// One row of raw 0x0RGB pixels (Help > About's logo).
void rowRgb(int x, int y, const uint16_t* src, int w);

// Transparent glyph run. Returns the advance in logical pixels.
int  text(int x, int y, const char* s, UiColor ink);
// As above but stops before exceeding `maxw` logical pixels, appending ".." when the
// string did not fit.
int  textClip(int x, int y, int maxw, const char* s, UiColor ink);
int  textWidth(const char* s);

// Widgets. These scale horizontally with glyphScale so they keep the same apparent
// size in DS80, where a logical pixel is half as wide.
void radio(int x, int y, bool on, UiColor ink, UiColor accent);
int  radioW();
void chevron(int x, int y, UiColor c);          // 4x7 "descend" arrow
int  chevronW();
void icon1bpp(int x, int y, const uint16_t* rows, int n, int wbits, UiColor c);
// Rounded rectangle: 1-px border with the corners cut back by `r` pixels.
void roundRect(int x, int y, int w, int h, int r, UiColor border, UiColor fill_c);
// Re-draws ONLY the border and the outside-corner pixels of a roundRect. The
// header/footer bands are plain rectangles, so every band repaint squares the
// window corners off — call this after them to restore the rounding.
void roundRectBorder(int x, int y, int w, int h, int r, UiColor border, UiColor outside);
// The ZX rainbow mark of the header.
void rainbow(int x, int y);
int  rainbowW();

} // namespace nm

