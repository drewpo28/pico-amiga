// pico-amiga (from pico-speccy) — staged settings overlay for the fullscreen menu.
//
// The menu never writes Config while it is open. Every value edit lands in this
// overlay; on exit one ordered commit writes the values, persists once
// (Config::save) and asks at most one "Apply and reboot?" question.  Reads are
// read-through (staged -> live), so the menu always shows the truth.  The store is
// static BSS.
//
// PICO-AMIGA: pico-speccy's mechanism (store, 3-way merge, F_PREVIEW hooks,
// commit) with pico-amiga's settings; its Spectrum constraint rules, subsystem
// budget gates and machine switching are gone.

#pragma once

#include <stdint.h>

namespace nm {

// How a setting reaches the hardware.
enum ApplyClass : uint8_t {
    AC_PURE = 0,    // persist only (whoever uses it reads Config live)
    AC_LIVE,        // persist + call the hook
    AC_REBOOT,      // read once at boot; one prompt at the end of the commit
};

enum SettingFlags : uint16_t {
    F_PREVIEW = 1u << 0,   // hook is idempotent + allocation-free: apply it live so the
                           //   user sees the change at once
    F_PALETTE = 1u << 1,   // changes the menu's own colours: re-install them
    F_MODAL   = 1u << 7,   // the edit changes the whole screen (or draws boxes): the nav
                           //   redraws everything afterwards
};

// One stable id per staged setting. APPEND ONLY — the id is the only link between
// a menu row and the value it edits.
//
//     X(id, class, flags, getter, setter, hook)
#define NM_SETTINGS_TABLE(X)                                                              \
    X(SET_CURSOR_JOY,  AC_PURE, 0,                             get_cursorjoy, put_cursorjoy, nullptr) \
    X(SET_MOUSE_SWAP,  AC_PURE, 0,                             get_mswap,     put_mswap,     nullptr) \
    X(SET_MOUSE_SENS,  AC_PURE, 0,                             get_msens,     put_msens,     nullptr) \
    X(SET_UI_THEME,    AC_PURE, F_PREVIEW | F_PALETTE | F_MODAL, get_theme,   put_theme,     hook_ui) \
    X(SET_UI_ROUNDED,  AC_PURE, F_PREVIEW | F_MODAL,           get_rounded,   put_rounded,   hook_ui) \
    X(SET_UI_VGA_SOLID,AC_PURE, F_PREVIEW | F_PALETTE | F_MODAL, get_vgasolid, put_vgasolid, hook_ui) \
    X(SET_KICK,        AC_LIVE, 0,                             get_kick,      put_kick,      hook_kick) \
    X(SET_DF0_WP,      AC_LIVE, 0,                             get_df0wp,     put_df0wp,     hook_df0wp) \
    X(SET_VOLUME,      AC_LIVE, F_PREVIEW,                     get_volume,    put_volume,    hook_sound) \
    /* retired, no row: HDMI audio is Audio > Output (Auto or HDMI) */ \
    X(SET_HDMI_AUDIO,  AC_LIVE, F_PREVIEW,                     get_hdmiaudio, put_hdmiaudio, hook_sound) \
    X(SET_AUDIO_OUT,   AC_REBOOT, 0,                           get_audioout,  put_audioout,  nullptr) \
    X(SET_CPU_MHZ,     AC_REBOOT, 0,                           get_cpumhz,    put_cpumhz,    nullptr) \
    X(SET_FLASH_FREQ,  AC_REBOOT, 0,                           get_flashfreq, put_flashfreq, nullptr) \
    X(SET_PSRAM_FREQ,  AC_REBOOT, 0,                           get_psramfreq, put_psramfreq, nullptr) \
    X(SET_DBG_UART,    AC_REBOOT, 0,                           get_dbguart,   put_dbguart,   nullptr) \
    X(SET_DRIVES,      AC_LIVE, 0,                             get_drives,    put_drives,    hook_drives) \
    X(SET_HPOS,        AC_LIVE, 0,                             get_hpos,      put_hpos,      hook_hpos) \
    X(SET_RENDER_C1,   AC_LIVE, 0,                             get_renderc1,  put_renderc1,  hook_hpos) \
    X(SET_DF0_DISK,    AC_LIVE, 0,                             get_df0disk,   put_df0disk,   nullptr) \
    /* Video > Capture card compatibility: the PIO HDMI tables are built at boot */ \
    X(SET_HDMI_CAPTURE, AC_REBOOT, 0,                          get_hdmicapture, put_hdmicapture, nullptr) \
    X(SET_VCENTRE,     AC_LIVE, 0,                             get_vcentre,   put_vcentre,   hook_hpos) \
    X(SET_DISK_LED,    AC_LIVE, 0,                             get_diskled,   put_diskled,   nullptr)

#define NM_X_ENUM(id, cls, flags, g, p, h) id,
enum SettingId : uint16_t {
    SET_NONE = 0,
    NM_SETTINGS_TABLE(NM_X_ENUM)
    SET_COUNT
};
#undef NM_X_ENUM

namespace Stage {

void    begin();                    // called when the menu opens
int32_t get(uint16_t id);           // staged value if touched, else the live value
void    set(uint16_t id, int32_t v);
bool    isDirty(uint16_t id);
bool    anyDirty();
bool    rebootPending();            // a dirty reboot-class setting is staged
uint8_t dirtyCount();
void    invalidate(uint16_t id);    // an action moved this behind our back
void    discard();                  // drop every staged edit, undoing live previews

struct CommitReport {
    uint8_t changed;        // settings actually written
    bool    saved;          // Config::save() succeeded
    bool    needsReboot;    // a reboot-class value was persisted
    uint8_t failed;         // hooks that reported failure
    bool    machineSwitched;// (kept for the nav; never set on pico-amiga)
    const char* note;       // one message for the user, or nullptr
};

// The single exit point of the menu: persist once, apply, report.
void commit(CommitReport& out);

// The message of a set() side effect, or nullptr; cleared by the call.
const char* takeNote();

// True when editing `id` redraws the whole screen (F_PREVIEW | F_MODAL).
bool editDrawsModal(uint16_t id);

} // namespace Stage
} // namespace nm
