// pico-amiga (from pico-speccy) — staged settings overlay (see UiStage.h).

#include "OSDNewMenu.h"

#include <stdio.h>
#include <string.h>
#include <string>

#include "UiStage.h"
#include "UiModel.h"
#include "UiGfx.h"
#include "app/Config.h"
#include "roms.h"
#include "machine.h"
#include "ui.h"
#include "UiPlatform.h"
extern "C" {
#include "video.h"
}

namespace nm {

// ── accessors (the getter/setter/hook columns of NM_SETTINGS_TABLE) ─────────────

#define NM_BOOL_ACCESS(name, field) \
    static int32_t get_##name() { return Config::field ? 1 : 0; } \
    static void    put_##name(int32_t v) { Config::field = v != 0; }
#define NM_INT_ACCESS(name, field, type) \
    static int32_t get_##name() { return (int32_t)Config::field; } \
    static void    put_##name(int32_t v) { Config::field = (type)v; }

NM_BOOL_ACCESS(cursorjoy, CursorAsJoy)
NM_INT_ACCESS (mswap,     mouse_swap, uint8_t)
NM_INT_ACCESS (msens,     mouse_sens, uint16_t)
NM_INT_ACCESS (theme,     ui_theme,   uint8_t)
NM_BOOL_ACCESS(rounded,   ui_rounded)
NM_BOOL_ACCESS(vgasolid,  ui_vga_solid)

// Machine (DiagROM / A500 / A500+ rows): the built-in ROM's index, KICK_SD the
// card's (Custom ROM, set by its own row: no radio lists it); KICK_PLUS added on
// the A500+ (the same ROM is listed under both A500 and A500+, and only the
// running machine's row shows it).
#define KICK_SD   100
#define KICK_PLUS 200
static int32_t get_kick() {
    const int32_t plus = Config::machine ? KICK_PLUS : 0;
    if (Config::kick == "sd") return KICK_SD + plus;
    for (unsigned i = 0; i < builtin_rom_count; i++)
        if (Config::kick == builtin_roms[i].id) return (int32_t)i + plus;
    return -1;
}
static void put_kick(int32_t v) {
    Config::machine = v >= KICK_PLUS;
    if (v >= KICK_PLUS) v -= KICK_PLUS;
    if (v == KICK_SD) Config::kick = "sd";
    else if (v >= 0 && (unsigned)v < builtin_rom_count) Config::kick = builtin_roms[v].id;
}
// A new ROM or machine means a cold start of the Amiga (the disk stays in).
static bool hook_kick(int32_t, int32_t) { return machine_apply_kickstart(); }

NM_BOOL_ACCESS(df0wp, df0_wp)
// Re-insert the disk so the write protection takes hold.
static bool hook_df0wp(int32_t, int32_t) {
    const std::string path = machine_disk(0);
    return path.empty() || machine_insert(0, path.c_str(), Config::df0_wp);
}

// Devices > Disks > DF0 with an .m3u set: which of its disks is in the drive.  Read for
// the pick list's marker; the list itself swaps disks (machine_set_select).
static int32_t get_df0disk() { return machine_set_index(); }
static void    put_df0disk(int32_t v) { Config::df0_disk = (uint8_t)v; }

NM_INT_ACCESS (volume,    volume,     uint8_t)
NM_BOOL_ACCESS(hdmiaudio, hdmi_audio)
NM_BOOL_ACCESS(hdmicapture, hdmi_capture)
NM_INT_ACCESS (audioout,  audio_out,  uint8_t)
NM_INT_ACCESS (cpumhz,    cpu_mhz,    uint16_t)
NM_INT_ACCESS (vreg,      vreg_mv,    uint16_t)
NM_INT_ACCESS (flashfreq, max_flash_freq, uint16_t)
NM_INT_ACCESS (psramfreq, max_psram_freq, uint16_t)
NM_BOOL_ACCESS(dbguart,   dbg_uart)
NM_INT_ACCESS (drives,    drives,     uint8_t)
// Kickstart looks for drives at reset: a new count resets the Amiga.
static bool hook_drives(int32_t, int32_t) {
    if (machine_apply_drives()) uiAmigaReset();
    return true;
}
NM_INT_ACCESS (hpos,      hpos,       uint8_t)
NM_BOOL_ACCESS(renderc1,  render_c1)
NM_BOOL_ACCESS(vcentre,   vcentre)
NM_BOOL_ACCESS(diskled,   disk_led)
// Video > Output > Mode edits the mode of the display in use.
static int32_t get_vmode() { return video_is_vga() ? Config::vga_vmode : Config::hdmi_vmode; }
static void    put_vmode(int32_t v) { (video_is_vga() ? Config::vga_vmode : Config::hdmi_vmode) = (uint8_t)v; }
// Picture position, render core: from the next frame on.
static bool hook_hpos(int32_t, int32_t) { settings_apply_video(); return true; }
// Volume and HDMI audio reach the outputs at once (heard while editing).
static bool hook_sound(int32_t, int32_t) { settings_apply_sound(); return true; }

// The menu's look: Config is already written (F_PREVIEW puts first); F_PALETTE
// re-installs the colours and F_MODAL has the nav repaint everything.
static bool hook_ui(int32_t, int32_t) { return true; }

// ── descriptor table ───────────────────────────────────────────────────────────

typedef int32_t (*Getter)();
typedef void    (*Putter)(int32_t);
typedef bool    (*Hook)(int32_t nv, int32_t ov);

struct SettingDesc {
    ApplyClass cls;
    uint16_t   flags;
    Getter     get;
    Putter     put;
    Hook       hook;
};

#define NM_X_DESC(id, cls, flags, g, p, h) { cls, (uint16_t)(flags), g, p, h },
static const SettingDesc kDesc[SET_COUNT] = {
    { AC_PURE, 0, nullptr, nullptr, nullptr },  // SET_NONE
    NM_SETTINGS_TABLE(NM_X_DESC)
};
#undef NM_X_DESC

// ── the store: static BSS, zero heap ───────────────────────────────────────────

static int32_t g_val [SET_COUNT];
static int32_t g_base[SET_COUNT];                  // value at first touch (3-way merge)
static uint8_t g_dirty[(SET_COUNT + 7) / 8];

static inline bool bmGet(const uint8_t* bm, uint16_t i) { return bm[i >> 3] & (1u << (i & 7)); }
static inline void bmSet(uint8_t* bm, uint16_t i)       { bm[i >> 3] |= (uint8_t)(1u << (i & 7)); }
static inline void bmClr(uint8_t* bm, uint16_t i)       { bm[i >> 3] &= (uint8_t)~(1u << (i & 7)); }

static inline bool valid(uint16_t id) {
    return id != SET_NONE && id < SET_COUNT && kDesc[id].get && kDesc[id].put;
}

namespace Stage {

static const char* s_liveNote;

const char* takeNote() {
    const char* n = s_liveNote;
    s_liveNote = nullptr;
    return n;
}

void begin() {
    s_liveNote = nullptr;
    memset(g_dirty, 0, sizeof(g_dirty));
    memset(g_val, 0, sizeof(g_val));
    memset(g_base, 0, sizeof(g_base));
}

int32_t get(uint16_t id) {
    if (!valid(id)) return 0;
    return bmGet(g_dirty, id) ? g_val[id] : kDesc[id].get();
}

void set(uint16_t id, int32_t v) {
    if (!valid(id)) return;
    const SettingDesc& d = kDesc[id];
    const int32_t live = d.get();
    if (!bmGet(g_dirty, id)) g_base[id] = live;
    g_val[id] = v;
    // A -> B -> A leaves nothing staged, so leaving the menu stays a true no-op (no SD
    // write at all).
    if (v == g_base[id]) bmClr(g_dirty, id); else bmSet(g_dirty, id);

    // F_PREVIEW settings are applied immediately so the user sees the change. A hook
    // may refuse (false): the edit is then undone, live and staged both.
    if ((d.flags & F_PREVIEW) && d.hook) {
        d.put(v);
        if (!d.hook(v, live)) {
            d.put(g_base[id]);
            g_val[id] = g_base[id];
            bmClr(g_dirty, id);
        }
        if (d.flags & F_PALETTE) gfxInstallPalette();
    }
}

bool isDirty(uint16_t id) { return valid(id) && bmGet(g_dirty, id); }

bool rebootPending() {
    for (uint16_t id = 1; id < SET_COUNT; id++)
        if (bmGet(g_dirty, id) && kDesc[id].cls == AC_REBOOT) return true;
    return false;
}

bool anyDirty() {
    for (size_t i = 0; i < sizeof(g_dirty); i++) if (g_dirty[i]) return true;
    return false;
}

uint8_t dirtyCount() {
    uint8_t n = 0;
    for (uint16_t id = 1; id < SET_COUNT; id++) if (bmGet(g_dirty, id)) n++;
    return n;
}

void invalidate(uint16_t id) {
    if (valid(id)) bmClr(g_dirty, id);
}

bool editDrawsModal(uint16_t id) {
    if (!valid(id)) return false;
    const uint16_t f = kDesc[id].flags;
    return (f & F_PREVIEW) && (f & F_MODAL);
}

void discard() {
    // Undo the live previews before dropping the staged values.
    for (uint16_t id = 1; id < SET_COUNT; id++) {
        if (!bmGet(g_dirty, id)) continue;
        const SettingDesc& d = kDesc[id];
        if ((d.flags & F_PREVIEW) && d.hook) {
            d.put(g_base[id]);
            d.hook(g_base[id], g_val[id]);
            if (d.flags & F_PALETTE) gfxInstallPalette();
        }
    }
    begin();
}

void commit(CommitReport& rep) {
    memset(&rep, 0, sizeof(rep));
    if (!anyDirty()) return;                    // zero SD writes

    // 3-way merge: if the live value moved since we first touched it, an action did
    // it and wins. F_PREVIEW values are live by construction — not a conflict.
    for (uint16_t id = 1; id < SET_COUNT; id++) {
        if (!bmGet(g_dirty, id)) continue;
        if (kDesc[id].get() != g_base[id]) {
            if (!(kDesc[id].flags & F_PREVIEW) || kDesc[id].get() != g_val[id])
                bmClr(g_dirty, id);
        }
    }
    if (!anyDirty()) return;

    // A new video mode: the ones it replaces go to the rollback file first.
    for (uint16_t id = 1; id < SET_COUNT; id++)
        if (bmGet(g_dirty, id) && (kDesc[id].flags & F_VMODE)) { Config::savePendingVideoMode(); break; }

    // Write the staged values (emulation is suspended while the menu is open).
    for (uint16_t id = 1; id < SET_COUNT; id++) {
        if (!bmGet(g_dirty, id)) continue;
        kDesc[id].put(g_val[id]);
        rep.changed++;
        if (kDesc[id].cls == AC_REBOOT) rep.needsReboot = true;
    }

    // Persist once.
    rep.saved = Config::save();
    if (!rep.saved) rep.note = "Settings applied, but not saved to the card";

    // Live hooks, in table order (F_PREVIEW ones already ran).
    for (uint16_t id = 1; id < SET_COUNT; id++) {
        if (!bmGet(g_dirty, id)) continue;
        const SettingDesc& d = kDesc[id];
        if (d.cls != AC_LIVE || !d.hook || (d.flags & F_PREVIEW)) continue;
        if (!d.hook(g_val[id], g_base[id])) rep.failed++;
    }
    begin();
}

} // namespace Stage

// ── the model's view of a value ────────────────────────────────────────────────

int32_t nodeValue(const Node& n) {
    return Stage::get(n.setting);
}

void nodeSetValue(const Node& n, int32_t v) {
    Stage::set(n.setting, v);
}

const char* nodeValueLabel(const Node& n) {
    if (n.vlabel) return n.vlabel();        // live state of an action row
    if (n.kind == K_RADIO || n.kind == K_BOOL) {
        const int32_t v = Stage::get(n.setting);
        uint8_t cnt; const Option* o = nodeOptions(n, cnt);
        for (uint8_t i = 0; i < cnt; i++)
            if (o[i].value == v)
                return o[i].slabel ? o[i].slabel : o[i].label;
    }
    if (n.kind == K_INT) {
        // Normalised to the range so a -16..0 volume reads "13/16", not "-3".
        static char buf[16];
        snprintf(buf, sizeof(buf), "%ld/%ld",
                 (long)(Stage::get(n.setting) - n.lo), (long)(n.hi - n.lo));
        return buf;
    }
    return "";
}

} // namespace nm
