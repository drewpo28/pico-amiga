// pico-amiga - Devices > Input > Pad buttons / Keyboard keys.
//
// pico-speccy's joystick mapping page (its UiJoy.cpp) for the Amiga: the same
// spatial plan - a cell where each control physically is, the cursor walking the
// pad and not a list, Test lighting the real control as it is pressed - without
// its joystick types and profiles (one pad here: the CD32's, whose red button is
// the joystick's fire on every machine).  Two pages over one engine:
//   Pad buttons    the 14 controls of a NES / SNES / USB / XInput pad (joymap.h
//                  JOY_S_*), each given an action (direction, CD32 button, None)
//   Keyboard keys  the pad's 11 actions as the CD32 pad lays them out, each given
//                  a key: Enter, then the key.  Used while Keyboard as joystick is on.
// A change is live at once and saved when the page closes.
#include "OSDNewMenu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "UiJoy.h"
#include "UiGfx.h"
#include "UiFont.h"
#include "UiRender.h"       // SYM_* glyphs, uiIdle
#include "UiDialog.h"
#include "UiPlatform.h"
#include "app/Config.h"
#include "joymap.h"

namespace nm {

// A cell: its place in "design pixels" of pico-speccy's 320-wide plan (x scaled
// by the glyph scale, y as it is), its caption, its pill's width; id < 0: a button.
struct Cell {
    int16_t x, y;
    int8_t  id;               // the slot / action it edits; -1 Test, -2 Defaults
    const char* cap;
    int8_t  w;
};
enum { B_TEST = -1, B_DEF = -2 };

static const Cell kPad[] = {
    {   4,  82, JOY_S_LEFT,  "Left",   66 }, {  76,  82, JOY_S_RIGHT, "Right",  66 },
    {  40,  52, JOY_S_UP,    "Up",     66 }, {  40, 112, JOY_S_DOWN,  "Down",   66 },
    {   4, 142, JOY_S_START, "Start",  66 }, {  76, 142, JOY_S_MODE,  "Select", 66 },
    { 168,  52, JOY_S_A,     "A",      54 }, { 250,  52, JOY_S_B,     "B",      54 },
    { 168,  82, JOY_S_X,     "X",      54 }, { 250,  82, JOY_S_Y,     "Y",      54 },
    { 168, 112, JOY_S_C,     "C (R)",  54 }, { 250, 112, JOY_S_Z,     "Z (L)",  54 },
    { 168, 142, JOY_S_L2,    "L2",     54 }, { 250, 142, JOY_S_R2,    "R2",     54 },
    {  16, 172, B_DEF,  nullptr, 0 }, { 214, 172, B_TEST, nullptr, 0 },
};
// The CD32 pad: the shoulders above, the diamond (green, yellow / red, blue), play below.
static const Cell kKeys[] = {
    {   4,  82, JOY_A_LEFT,   "Left",    66 }, {  76,  82, JOY_A_RIGHT,  "Right",   66 },
    {  40,  52, JOY_A_UP,     "Up",      66 }, {  40, 112, JOY_A_DOWN,   "Down",    66 },
    { 168,  52, JOY_A_RWD,    "Reverse", 66 }, { 244,  52, JOY_A_FFW,    "Forward", 66 },
    { 168,  82, JOY_A_GREEN,  "Green",   66 }, { 244,  82, JOY_A_YELLOW, "Yellow",  66 },
    { 168, 112, JOY_A_RED,    "Red",     66 }, { 244, 112, JOY_A_BLUE,   "Blue",    66 },
    { 168, 142, JOY_A_PLAY,   "Play",    66 },
    {  16, 172, B_DEF,  nullptr, 0 }, { 214, 172, B_TEST, nullptr, 0 },
};

struct Page {
    const Cell* c; int n;
    bool keys;                  // the keyboard's page
    const char* title;
};
static const Page* P;
static int  s_sel;
static bool s_test;
static bool s_lit[32];

// ── layout (pico-speccy's computeJoyLayout; a short or narrow surface squeezes
// the plan: rows 22 apart instead of 30, the columns pulled in) ─────────────────
static struct { int ix, iy, iw, ih, margin, hdr_h, foot_h, pad, ox, oy; bool compact; } JL;

static void joyLayout() {
    const int sc = Sf.glyphScale;
    JL.pad = 2 * sc;
    JL.margin = 2 * sc;
    JL.ix = JL.margin + sc;
    JL.iy = 3;
    JL.iw = Sf.w - 2 * JL.ix;
    JL.ih = Sf.h - JL.iy - 3;
    JL.hdr_h = UI_FONT_H + 6;
    JL.foot_h = UI_FONT_H + 4;
    const int body_y = JL.iy + JL.hdr_h;
    const int body_h = JL.iy + JL.ih - JL.foot_h - body_y;
    JL.compact = Sf.w < 316 * sc || body_h < 184;
    const int dw = JL.compact ? 250 : 312, dh = JL.compact ? 132 : 190;
    JL.ox = (Sf.w - dw * sc) / 2;
    if (JL.ox < JL.ix) JL.ox = JL.ix;
    JL.oy = body_y + (body_h - dh) / 2 - (JL.compact ? 0 : 4);
    if (JL.oy < body_y) JL.oy = body_y;
}
static int cellW(int i) {
    int w = P->c[i].w ? P->c[i].w : 56;
    if (JL.compact && w > 58) w = 58;
    return w * Sf.glyphScale;
}
static int cx(int i) {
    int x = P->c[i].x;
    if (JL.compact) x = x >= 160 ? 128 + (x - 168) * 60 / 80 : x * 62 / 76;
    return JL.ox + x * Sf.glyphScale;
}
static int cy(int i) {
    const int y = P->c[i].y;
    return JL.oy + (JL.compact ? 32 + (y - 52) * 22 / 30 : y) - (JL.compact && y >= 172 ? 0 : 0);
}

// ── drawing ────────────────────────────────────────────────────────────────────
static const char* btnText(int id) { return id == B_TEST ? "Test" : "Load defaults"; }

static void cellValue(int i, char* out, size_t n) {
    const int id = P->c[i].id;
    if (P->keys) { char b[16]; snprintf(out, n, "%s", joymap_key_name(Config::joykey[id], b)); }
    else snprintf(out, n, "%s", joymap_action_name[Config::joydef[id]]);
}

static void drawCell(int i) {
    const int x = cx(i), y = cy(i);
    const bool sel = i == s_sel;
    const int id = P->c[i].id;
    if (id < 0) {
        const char* t = btnText(id);
        const int w = ((int)strlen(t) + 2) * glyphW();
        fill(x, y - 1, w, 11, sel && !s_test ? C_SEL_BG : C_PANEL_ALT);
        text(x + glyphW(), y, t, sel && !s_test ? C_WHITE : C_TEXT);
        return;
    }
    const bool lit = s_test && s_lit[i];
    fill(x, y - UI_FONT_H - 2, cellW(i), UI_FONT_H + 1, C_PANEL);
    textClip(x, y - UI_FONT_H - 1, cellW(i), P->c[i].cap,
             s_test ? (lit ? C_ACCENT : C_TEXT_DIM) : (sel ? C_WHITE : C_TEXT_DIM));
    char v[24];
    cellValue(i, v, sizeof v);
    fill(x, y - 1, cellW(i), 11, sel && !s_test ? C_SEL_BG : C_PANEL_ALT);
    frame(x, y - 1, cellW(i), 11, lit ? C_ACCENT : C_SEP);
    textClip(x + 2 * Sf.glyphScale, y, cellW(i) - 4 * Sf.glyphScale, v, sel && !s_test ? C_WHITE : C_TEXT);
}
static void drawCells() { for (int i = 0; i < P->n; i++) drawCell(i); }

static void drawFooter() {
    const int fy = JL.iy + JL.ih - JL.foot_h;
    fill(JL.ix, fy, JL.iw, JL.foot_h, C_FOOT_BG);
    hline(JL.ix, fy, JL.iw, C_SEP);
    text(JL.ix + JL.pad, fy + 3,
         s_test ? (P->keys ? "Press the keys   Esc Leave test" : "Press pad buttons   Esc Leave test")
                : SYM_ENTER " Assign  Del Clear  Esc Close",
         C_TEXT_DIM);
    roundRectBorder(JL.margin, JL.iy - 1, Sf.w - 2 * JL.margin, JL.ih + 2, 4, C_SEP, C_BG);
}

static void drawChrome() {
    fill(0, 0, Sf.w, Sf.h, C_BG);
    roundRect(JL.margin, JL.iy - 1, Sf.w - 2 * JL.margin, JL.ih + 2, 4, C_SEP, C_PANEL);
    fill(JL.ix, JL.iy, JL.iw, JL.hdr_h, C_PANEL);
    rainbow(JL.ix + JL.pad, JL.iy + 3);
    const int tx = JL.ix + JL.pad + rainbowW() + 2 * JL.pad;
    text(tx, JL.iy + 4, P->title, C_WHITE);
    const char* r = s_test ? "Test" : P->keys ? (Config::CursorAsJoy ? "Keyboard as joystick: on" : "Keyboard as joystick: OFF")
                                              : "Port 2";
    const int rw = textWidth(r);
    if (tx + textWidth(P->title) + 2 * glyphW() + rw < JL.ix + JL.iw - JL.pad)
        text(JL.ix + JL.iw - rw - JL.pad, JL.iy + 4, r, s_test ? C_ACCENT : C_TEXT_DIM);
    hline(JL.ix, JL.iy + JL.hdr_h - 1, JL.iw, C_SEP);
    drawCells();
    drawFooter();
}

// ── moving: the nearest cell that way (the plan is the pad's, not a list) ──────
static int neighbour(int from, int dx, int dy) {
    int best = -1, bestd = 1 << 30;
    const int fx = P->c[from].x + (P->c[from].id < 0 ? 20 : 0), fy = P->c[from].y;
    for (int i = 0; i < P->n; i++) {
        if (i == from) continue;
        const int ex = P->c[i].x + (P->c[i].id < 0 ? 20 : 0) - fx, ey = P->c[i].y - fy;
        const int along = dx ? ex * dx : ey * dy, across = dx ? abs(ey) : abs(ex);
        if (along <= 0) continue;
        // A step along a row / column wins over a diagonal one: the pad's plan.
        const int d = along + 3 * across;
        if (d < bestd) { bestd = d; best = i; }
    }
    return best;
}

static bool nextKeyDown(fabgl::VirtualKeyItem& k) {
    while (uiKeyAvailable()) {
        if (!uiReadKey(&k)) continue;
        if (k.down) return true;
    }
    return false;
}
static void drainKeys() {
    fabgl::VirtualKeyItem k;
    uiIdle(30);
    while (uiKeyAvailable()) uiReadKey(&k);
}

// The next real key pressed (the menu's and the pad's twins skipped); 0: Esc.
static uint16_t captureKey() {
    fabgl::VirtualKeyItem k;
    drainKeys();
    for (;;) {
        if (!nextKeyDown(k)) { uiIdle(5); continue; }
        if (k.vk == fabgl::VK_ESCAPE) { drainKeys(); return 0; }
        const uint16_t n = joymap_key_norm((uint16_t)k.vk);
        if (!n) continue;
        drainKeys();
        return n;
    }
}

static void assign(int i) {
    const int id = P->c[i].id;
    if (!P->keys) {
        char t[32];
        snprintf(t, sizeof t, "%s does", joymap_slot_name[id]);
        const int a = uiPickList(t, joymap_action_name, JOY_ACTIONS, Config::joydef[id]);
        if (a >= 0) Config::joydef[id] = (uint8_t)a;
        return;
    }
    // The key: pressed, not picked.  One key, one action: its old one lets go.
    const int fy = JL.iy + JL.ih - JL.foot_h;
    fill(JL.ix, fy + 1, JL.iw, JL.foot_h - 1, C_FOOT_BG);
    char t[48];
    snprintf(t, sizeof t, "%s: press a key   Esc Cancel", joymap_action_name[id]);
    text(JL.ix + JL.pad, fy + 3, t, C_ACCENT);
    const uint16_t vk = captureKey();
    if (!vk) return;
    for (int a = 1; a < JOY_ACTIONS; a++) if (Config::joykey[a] == vk) Config::joykey[a] = 0;
    Config::joykey[id] = vk;
}

static void runPage(const Page& page) {
    P = &page;
    gfxBegin();
    joyLayout();
    memset(s_lit, 0, sizeof s_lit);
    s_sel = 0;
    s_test = false;
    drawChrome();
    drainKeys();
    fabgl::VirtualKeyItem k;
    for (;;) {
        if (s_test) {                          // light what is held
            bool dirty = false;
            for (int i = 0; i < P->n; i++) {
                const int id = P->c[i].id;
                if (id < 0) continue;
                const bool down = P->keys ? joymap_key_down(Config::joykey[id]) : joymap_slot_down(id);
                if (down != s_lit[i]) { s_lit[i] = down; dirty = true; }
            }
            if (dirty) drawCells();
        }
        if (!nextKeyDown(k)) { uiIdle(s_test ? 20 : 5); continue; }
        if (k.vk == fabgl::VK_ESCAPE || k.vk == fabgl::VK_F1) {
            if (!s_test) break;
            s_test = false;
            memset(s_lit, 0, sizeof s_lit);
            drawChrome();
            continue;
        }
        if (s_test) continue;
        int ns = -1;
        switch (k.vk) {
        case fabgl::VK_MENU_LEFT:  ns = neighbour(s_sel, -1, 0); break;
        case fabgl::VK_MENU_RIGHT: ns = neighbour(s_sel, 1, 0); break;
        case fabgl::VK_MENU_UP:    ns = neighbour(s_sel, 0, -1); break;
        case fabgl::VK_MENU_DOWN:  ns = neighbour(s_sel, 0, 1); break;
        case fabgl::VK_DELETE: {
            const int id = P->c[s_sel].id;
            if (id < 0) break;
            if (P->keys) Config::joykey[id] = 0; else Config::joydef[id] = JOY_A_NONE;
            drawCell(s_sel);
            break;
        }
        case fabgl::VK_MENU_ENTER: {
            const int id = P->c[s_sel].id;
            if (id == B_TEST) { s_test = true; drawChrome(); break; }
            if (id == B_DEF) {
                if (P->keys) joymap_key_defaults(Config::joykey); else joymap_pad_defaults(Config::joydef);
                drawCells();
                break;
            }
            assign(s_sel);
            drawChrome();                      // the picker drew over the page
            break;
        }
        default: break;
        }
        if (ns >= 0 && ns != s_sel) {
            const int old = s_sel;
            s_sel = ns;
            drawCell(old);
            drawCell(s_sel);
        }
    }
    Config::save();
    gfxEnd();
}

void joyPadPage() {
    static const Page p = { kPad, (int)(sizeof kPad / sizeof kPad[0]), false, "Pad buttons" };
    runPage(p);
}
void joyKeysPage() {
    static const Page p = { kKeys, (int)(sizeof kKeys / sizeof kKeys[0]), true, "Keyboard keys" };
    runPage(p);
}

}  // namespace nm
