// Input: USB HID keyboards, mice, gamepads and XInput pads, PS/2 keyboards and
// NES/SNES pads, through pico-speccy's drivers (src/pico/drivers/usbhost, input,
// ps2kbd, nespad) and its funnel: every keyboard becomes HID reports, every
// report becomes fabgl virtual keys (process_kbd_report / map_key /
// kbdExtraMapping, taken from pico-speccy's main.cpp), and every pad pushes
// VK_DPAD_* / VK_MENU_* into the same queue.  input_frame() drains that queue
// into the Amiga once per frame:
//   - keys -> Amiga raw keycodes (amiga_key); PC keys without an Amiga twin:
//     Insert -> Help, Home/End -> keypad ( ), PgUp/PgDn -> right/left Amiga,
//     Win -> left/right Amiga, Menu -> right Amiga
//   - VK_DPAD_* (pads; cursor keys + Left Ctrl while CursorAsJoy) -> joystick in
//     game port 2; cursor keys are not passed on as keys while CursorAsJoy is on
//   - the HID mouse -> port 1 mouse
// Hotkeys (WinUAE's layout): F12 opens the menu (ui/, from pico-speccy), Ctrl+F12
// the DF0 disk browser, Shift+F12 resets the Amiga; Ctrl+J toggles CursorAsJoy (as
// on pico-speccy). F11 resets the Amiga too, Win+F11 reboots the board (asked;
// pico-speccy's F12). Win+F1 is the System info page (pico-speccy's Alt+F1: Alt is
// the Amiga's), Win+F5 the Open file browser (pico-speccy's F5; it opens snapshots
// too: pico-speccy's F2 has no key here), Win+F3 / Win+F4 the snapshot slots
// (pico-speccy's F3 / F4), Alt+Win+F3 / Alt+Win+F4 the current slot at once
// (its Alt+F3 / Alt+F4), Win+F8 the
// performance band (pico-speccy's F8; it runs over the picture, the Amiga runs on). F1..F10 are the Amiga's, Insert its Help key.
#include "input.h"
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "amiga.h"
#include "ui/OSDNewMenu.h"
#include "ui/UiPlatform.h"
#include "ui/ui.h"
#include "input_devices.h"
#include "joymap.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"      // pico-speccy's headers
#include "drivers/input/keyboard.h"
#pragma GCC diagnostic pop

namespace Mouse {
    bool    seen;
    bool    buttonL, buttonR, buttonM;
    int32_t x, y;
    uint8_t wheel;
}

input_bits_t gamepad1_bits = { false, false, false, false, false, false, false, false };
input_bits_t gamepad2_bits = { false, false, false, false, false, false, false, false };

static fabgl::Keyboard kbd;
static volatile bool reset_req;
volatile bool g_ui_text_entry;          // ui/UiDialog.cpp: a line editor owns the keyboard
static bool menu_open;

// Key repeat for the menu (pico-speccy's repeat_handler and the last_key_pressed
// bookkeeping in its kbdPushData): only while the menu is open — the Amiga repeats
// keys itself. F-keys never repeat (one lost key-up would re-open the menu forever).
static fabgl::VirtualKey last_key_pressed = fabgl::VirtualKey::VK_NONE;
static uint32_t tickKbdRep;

// The key last pressed (pico-speccy's helper): SortedFiles.h aborts a long sort on F1.
fabgl::VirtualKey get_last_key_pressed(void) { return last_key_pressed; }

void repeat_handler(void) {
    const fabgl::VirtualKey v = last_key_pressed;
    if (!menu_open || v == fabgl::VirtualKey::VK_NONE) return;
    if (v >= fabgl::VirtualKey::VK_F1 && v <= fabgl::VirtualKey::VK_F12) return;
    if (tickKbdRep == 0) {
        if (v == fabgl::VirtualKey::VK_UP) kbd.injectVirtualKey(fabgl::VirtualKey::VK_MENU_UP, true);
        else if (v == fabgl::VirtualKey::VK_DOWN) kbd.injectVirtualKey(fabgl::VirtualKey::VK_MENU_DOWN, true);
        kbd.injectVirtualKey(v, true);
    } else if (time_us_32() - tickKbdRep > 500000) {
        tickKbdRep = 0;
    }
}

void joyPushData(fabgl::VirtualKey virtualKey, bool down) {
    kbd.injectVirtualKey(virtualKey, down);
}

void kbdPushData(fabgl::VirtualKey virtualKey, bool down) {
    static bool ctrl;
    if (virtualKey == fabgl::VirtualKey::VK_LCTRL || virtualKey == fabgl::VirtualKey::VK_RCTRL) ctrl = down;
    if (down) {
        if (ctrl && virtualKey == fabgl::VirtualKey::VK_J) Config::CursorAsJoy = !Config::CursorAsJoy;
        if (last_key_pressed != virtualKey &&
            last_key_pressed != fabgl::VirtualKey::VK_MENU_UP &&
            last_key_pressed != fabgl::VirtualKey::VK_MENU_DOWN) {
            last_key_pressed = virtualKey;
            tickKbdRep = time_us_32();
        }
    } else {
        last_key_pressed = fabgl::VirtualKey::VK_NONE;
        tickKbdRep = 0;
    }
    kbd.injectVirtualKey(virtualKey, down);
}

// ---- HID keyboard reports -> virtual keys (pico-speccy main.cpp) --------------------
inline static bool isInReport(hid_keyboard_report_t const *report, const unsigned char keycode) {
    for (unsigned char i: report->keycode) {
        if (i == keycode) {
            return true;
        }
    }
    return false;
}

static uint8_t pressed_key[256] = { 0 };
static fabgl::VirtualKey map_key(uint8_t kc) {
    switch(kc) {
        case HID_KEY_SPACE: return fabgl::VirtualKey::VK_SPACE;

        case HID_KEY_A: return fabgl::VirtualKey::VK_A;
        case HID_KEY_B: return fabgl::VirtualKey::VK_B;
        case HID_KEY_C: return fabgl::VirtualKey::VK_C;
        case HID_KEY_D: return fabgl::VirtualKey::VK_D;
        case HID_KEY_E: return fabgl::VirtualKey::VK_E;
        case HID_KEY_F: return fabgl::VirtualKey::VK_F;
        case HID_KEY_G: return fabgl::VirtualKey::VK_G;
        case HID_KEY_H: return fabgl::VirtualKey::VK_H;
        case HID_KEY_I: return fabgl::VirtualKey::VK_I;
        case HID_KEY_J: return fabgl::VirtualKey::VK_J;
        case HID_KEY_K: return fabgl::VirtualKey::VK_K;
        case HID_KEY_L: return fabgl::VirtualKey::VK_L;
        case HID_KEY_M: return fabgl::VirtualKey::VK_M;
        case HID_KEY_N: return fabgl::VirtualKey::VK_N;
        case HID_KEY_O: return fabgl::VirtualKey::VK_O;
        case HID_KEY_P: return fabgl::VirtualKey::VK_P;
        case HID_KEY_Q: return fabgl::VirtualKey::VK_Q;
        case HID_KEY_R: return fabgl::VirtualKey::VK_R;
        case HID_KEY_S: return fabgl::VirtualKey::VK_S;
        case HID_KEY_T: return fabgl::VirtualKey::VK_T;
        case HID_KEY_U: return fabgl::VirtualKey::VK_U;
        case HID_KEY_V: return fabgl::VirtualKey::VK_V;
        case HID_KEY_W: return fabgl::VirtualKey::VK_W;
        case HID_KEY_X: return fabgl::VirtualKey::VK_X;
        case HID_KEY_Y: return fabgl::VirtualKey::VK_Y;
        case HID_KEY_Z: return fabgl::VirtualKey::VK_Z;

        case HID_KEY_0: return fabgl::VirtualKey::VK_0;
        case HID_KEY_1: return fabgl::VirtualKey::VK_1;
        case HID_KEY_2: return fabgl::VirtualKey::VK_2;
        case HID_KEY_3: return fabgl::VirtualKey::VK_3;
        case HID_KEY_4: return fabgl::VirtualKey::VK_4;
        case HID_KEY_5: return fabgl::VirtualKey::VK_5;
        case HID_KEY_6: return fabgl::VirtualKey::VK_6;
        case HID_KEY_7: return fabgl::VirtualKey::VK_7;
        case HID_KEY_8: return fabgl::VirtualKey::VK_8;
        case HID_KEY_9: return fabgl::VirtualKey::VK_9;

        case HID_KEY_KEYPAD_0: return fabgl::VirtualKey::VK_KP_0;
        case HID_KEY_KEYPAD_1: return fabgl::VirtualKey::VK_KP_1;
        case HID_KEY_KEYPAD_2: return fabgl::VirtualKey::VK_KP_2;
        case HID_KEY_KEYPAD_3: return fabgl::VirtualKey::VK_KP_3;
        case HID_KEY_KEYPAD_4: return fabgl::VirtualKey::VK_KP_4;
        case HID_KEY_KEYPAD_5: return fabgl::VirtualKey::VK_KP_5;
        case HID_KEY_KEYPAD_6: return fabgl::VirtualKey::VK_KP_6;
        case HID_KEY_KEYPAD_7: return fabgl::VirtualKey::VK_KP_7;
        case HID_KEY_KEYPAD_8: return fabgl::VirtualKey::VK_KP_8;
        case HID_KEY_KEYPAD_9: return fabgl::VirtualKey::VK_KP_9;
        case HID_KEY_NUM_LOCK: return fabgl::VirtualKey::VK_NUMLOCK;
        case HID_KEY_KEYPAD_DIVIDE: return fabgl::VirtualKey::VK_KP_DIVIDE;
        case HID_KEY_KEYPAD_MULTIPLY: return fabgl::VirtualKey::VK_KP_MULTIPLY;
        case HID_KEY_KEYPAD_SUBTRACT: return fabgl::VirtualKey::VK_KP_MINUS;
        case HID_KEY_KEYPAD_ADD: return fabgl::VirtualKey::VK_KP_PLUS;
        case HID_KEY_KEYPAD_ENTER: return fabgl::VirtualKey::VK_KP_ENTER;
        case HID_KEY_KEYPAD_DECIMAL: return fabgl::VirtualKey::VK_KP_PERIOD;

        case HID_KEY_PRINT_SCREEN: return fabgl::VirtualKey::VK_PRINTSCREEN;
        case HID_KEY_SCROLL_LOCK: return fabgl::VirtualKey::VK_SCROLLLOCK;
        case HID_KEY_PAUSE: return fabgl::VirtualKey::VK_PAUSE;

        case HID_KEY_INSERT: return fabgl::VirtualKey::VK_INSERT;
        case HID_KEY_HOME: return fabgl::VirtualKey::VK_HOME;
        case HID_KEY_PAGE_UP: return fabgl::VirtualKey::VK_PAGEUP;
        case HID_KEY_PAGE_DOWN: return fabgl::VirtualKey::VK_PAGEDOWN;
        case HID_KEY_DELETE: return fabgl::VirtualKey::VK_DELETE;
        case HID_KEY_END: return fabgl::VirtualKey::VK_END;

        case HID_KEY_F1: return fabgl::VirtualKey::VK_F1;
        case HID_KEY_F2: return fabgl::VirtualKey::VK_F2;
        case HID_KEY_F3: return fabgl::VirtualKey::VK_F3;
        case HID_KEY_F4: return fabgl::VirtualKey::VK_F4;
        case HID_KEY_F5: return fabgl::VirtualKey::VK_F5;
        case HID_KEY_F6: return fabgl::VirtualKey::VK_F6;
        case HID_KEY_F7: return fabgl::VirtualKey::VK_F7;
        case HID_KEY_F8: return fabgl::VirtualKey::VK_F8;
        case HID_KEY_F9: return fabgl::VirtualKey::VK_F9;
        case HID_KEY_F10: return fabgl::VirtualKey::VK_F10;
        case HID_KEY_F11: return fabgl::VirtualKey::VK_F11;
        case HID_KEY_F12: return fabgl::VirtualKey::VK_F12;

        case HID_KEY_ALT_LEFT: return fabgl::VirtualKey::VK_LALT;
        case HID_KEY_ALT_RIGHT: return fabgl::VirtualKey::VK_RALT;
        case HID_KEY_CONTROL_LEFT: return fabgl::VirtualKey::VK_LCTRL;
        case HID_KEY_CONTROL_RIGHT: return fabgl::VirtualKey::VK_RCTRL;
        case HID_KEY_SHIFT_LEFT: return fabgl::VirtualKey::VK_LSHIFT;
        case HID_KEY_SHIFT_RIGHT: return fabgl::VirtualKey::VK_RSHIFT;
        case HID_KEY_CAPS_LOCK: return fabgl::VirtualKey::VK_CAPSLOCK;

        case HID_KEY_TAB: return fabgl::VirtualKey::VK_TAB;
        case HID_KEY_ENTER: return fabgl::VirtualKey::VK_RETURN;
        case HID_KEY_ESCAPE: return fabgl::VirtualKey::VK_ESCAPE;

        case HID_KEY_GRAVE: return fabgl::VirtualKey::VK_TILDE;
        case HID_KEY_MINUS: return fabgl::VirtualKey::VK_MINUS;
        case HID_KEY_EQUAL: return fabgl::VirtualKey::VK_EQUALS;
        case HID_KEY_BACKSLASH: return fabgl::VirtualKey::VK_BACKSLASH;
        case HID_KEY_EUROPE_1: return fabgl::VirtualKey::VK_BACKSLASH; // ???
        case HID_KEY_EUROPE_2: return fabgl::VirtualKey::VK_LESS;        // PICO-AMIGA: <> left of Z
        case HID_KEY_APPLICATION: return fabgl::VirtualKey::VK_APPLICATION;   // PICO-AMIGA
        case HID_KEY_BRACKET_LEFT: return fabgl::VirtualKey::VK_LEFTBRACKET;
        case HID_KEY_BRACKET_RIGHT: return fabgl::VirtualKey::VK_RIGHTBRACKET;
        case HID_KEY_SEMICOLON: return fabgl::VirtualKey::VK_SEMICOLON;
        case HID_KEY_APOSTROPHE: return fabgl::VirtualKey::VK_QUOTE;
        case HID_KEY_COMMA: return fabgl::VirtualKey::VK_COMMA;
        case HID_KEY_PERIOD: return fabgl::VirtualKey::VK_PERIOD;
        case HID_KEY_SLASH: return fabgl::VirtualKey::VK_SLASH;
        case HID_KEY_BACKSPACE: return fabgl::VirtualKey::VK_BACKSPACE;

        case HID_KEY_ARROW_UP: return fabgl::VirtualKey::VK_UP;
        case HID_KEY_ARROW_DOWN: return fabgl::VirtualKey::VK_DOWN;
        case HID_KEY_ARROW_LEFT: return fabgl::VirtualKey::VK_LEFT;
        case HID_KEY_ARROW_RIGHT: return fabgl::VirtualKey::VK_RIGHT;

        case HID_KEY_VOLUME_UP: return fabgl::VirtualKey::VK_VOLUMEUP;
        case HID_KEY_VOLUME_DOWN: return fabgl::VirtualKey::VK_VOLUMEDOWN;
        case HID_KEY_MUTE: return fabgl::VirtualKey::VK_VOLUMEMUTE;
 // TODO:
//        case HID_KEY_GUI_LEFT: return fabgl::VirtualKey::VK_F1;
//        case HID_KEY_GUI_RIGHT: return fabgl::VirtualKey::VK_F1;
        default: break;
    }
    return fabgl::VirtualKey::VK_NONE;
}

static void kbdExtraMapping(fabgl::VirtualKey virtualKey, bool pressed) {
    switch(virtualKey) {
        case fabgl::VirtualKey::VK_HOME: joyPushData(fabgl::VirtualKey::VK_MENU_HOME, pressed); break;
        case fabgl::VirtualKey::VK_UP: {
            joyPushData(fabgl::VirtualKey::VK_MENU_UP, pressed);
            break;
        }
        case fabgl::VirtualKey::VK_DOWN: {
            joyPushData(fabgl::VirtualKey::VK_MENU_DOWN, pressed);
            break;
        }
        case fabgl::VirtualKey::VK_LEFT: {
            joyPushData(fabgl::VirtualKey::VK_MENU_LEFT, pressed);
            break;
        }
        case fabgl::VirtualKey::VK_RIGHT: {
            joyPushData(fabgl::VirtualKey::VK_MENU_RIGHT, pressed);
            break;
        }
        case fabgl::VirtualKey::VK_BACKSPACE: joyPushData(fabgl::VirtualKey::VK_MENU_BS, pressed); break;
        // Space doubles as Enter in menus, but inside a line editor it is a character.
        case fabgl::VirtualKey::VK_SPACE:     if (!g_ui_text_entry) joyPushData(fabgl::VirtualKey::VK_MENU_ENTER, pressed); break;
        case fabgl::VirtualKey::VK_RETURN:    joyPushData(fabgl::VirtualKey::VK_MENU_ENTER, pressed); break;
        default: break;
    }
    kbdPushData(virtualKey, pressed);
}

typedef struct mod2key_s {
    hid_keyboard_modifier_bm_t mod;
    enum fabgl::VirtualKey     key;
} mod2key_t;

static mod2key_t mod2key[] = {
    { KEYBOARD_MODIFIER_LEFTALT,    fabgl::VirtualKey::VK_LALT },
    { KEYBOARD_MODIFIER_RIGHTALT,   fabgl::VirtualKey::VK_RALT },
    { KEYBOARD_MODIFIER_LEFTCTRL,   fabgl::VirtualKey::VK_LCTRL},
    { KEYBOARD_MODIFIER_RIGHTCTRL,  fabgl::VirtualKey::VK_RCTRL},
    { KEYBOARD_MODIFIER_RIGHTSHIFT, fabgl::VirtualKey::VK_RSHIFT},
    { KEYBOARD_MODIFIER_LEFTSHIFT,  fabgl::VirtualKey::VK_LSHIFT},
    { KEYBOARD_MODIFIER_RIGHTGUI,   fabgl::VirtualKey::VK_RGUI},
    { KEYBOARD_MODIFIER_LEFTGUI,    fabgl::VirtualKey::VK_LGUI},
};

// The one funnel for USB (hid_app.cpp) and PS/2 (Ps2Kbd_Mrmltr) keyboards, and
// for the SDL keyboard on the PC (host/sdl).
void process_kbd_report(
    hid_keyboard_report_t const *report,
    hid_keyboard_report_t const *prev_report
) {
    for (size_t i = 0; i < sizeof(mod2key) / sizeof(mod2key[0]); ++i) {
        if (report->modifier & mod2key[i].mod) {
            if (!pressed_key[mod2key[i].key]) {
                pressed_key[mod2key[i].key] = mod2key[i].key;
                kbdExtraMapping(mod2key[i].key, true);
            }
        } else {
            if (pressed_key[mod2key[i].key]) {
                kbdExtraMapping(mod2key[i].key, false);
                pressed_key[mod2key[i].key] = 0;
            }
        }
    }
    for (uint8_t pkc: prev_report->keycode) {
        if (!pkc) continue;
        bool key_still_pressed = false;
        for (uint8_t kc: report->keycode) {
            if (kc == pkc) {
                key_still_pressed = true;
                break;
            }
        }
        if (!key_still_pressed) {
            kbdExtraMapping((fabgl::VirtualKey)pressed_key[pkc], false);
            pressed_key[pkc] = 0;
        }
    }
    for (uint8_t kc: report->keycode) {
        if (!kc) continue;
        uint8_t* pk = pressed_key + kc;
        fabgl::VirtualKey vk = (fabgl::VirtualKey)*pk;
        if (vk == fabgl::VirtualKey::VK_NONE) { // it was not yet pressed
            vk = map_key(kc);
            if (vk != fabgl::VirtualKey::VK_NONE) {
                *pk = (uint8_t)vk;
                kbdExtraMapping(vk, true);
            }
        }
    }
}

// ---- virtual keys -> Amiga ----------------------------------------------------------
#define AK_NONE 0xFF
static uint8_t vk_to_amiga(fabgl::VirtualKey vk) {
    using namespace fabgl;
    if (vk >= VK_A && vk <= VK_Z) {
        static const uint8_t letters[26] = {
            0x20, 0x35, 0x33, 0x22, 0x12, 0x23, 0x24, 0x25, 0x17, 0x26, 0x27, 0x28, 0x37,   // A-M
            0x36, 0x18, 0x19, 0x10, 0x13, 0x21, 0x14, 0x16, 0x34, 0x11, 0x32, 0x15, 0x31 }; // N-Z
        return letters[vk - VK_A];
    }
    if (vk >= VK_1 && vk <= VK_9) return (uint8_t)(0x01 + (vk - VK_1));
    if (vk >= VK_F1 && vk <= VK_F10) return (uint8_t)(0x50 + (vk - VK_F1));
    switch (vk) {
    case VK_0:            return 0x0A;
    case VK_SPACE:        return 0x40;
    case VK_TILDE:        return 0x00;   // ` key
    case VK_MINUS:        return 0x0B;
    case VK_EQUALS:       return 0x0C;
    case VK_BACKSLASH:    return 0x0D;
    case VK_LEFTBRACKET:  return 0x1A;
    case VK_RIGHTBRACKET: return 0x1B;
    case VK_SEMICOLON:    return 0x29;
    case VK_QUOTE:        return 0x2A;
    case VK_COMMA:        return 0x38;
    case VK_PERIOD:       return 0x39;
    case VK_SLASH:        return 0x3A;
    case VK_LESS:         return 0x30;   // intl <> left of Z
    case VK_RETURN:       return 0x44;
    case VK_ESCAPE:       return 0x45;
    case VK_BACKSPACE:    return 0x41;
    case VK_TAB:          return 0x42;
    case VK_DELETE:       return 0x46;
    case VK_CAPSLOCK:     return 0x62;
    case VK_UP:           return 0x4C;
    case VK_DOWN:         return 0x4D;
    case VK_RIGHT:        return 0x4E;
    case VK_LEFT:         return 0x4F;
    case VK_INSERT:       return 0x5F;   // Help
    case VK_HOME:         return 0x5A;   // keypad (
    case VK_END:          return 0x5B;   // keypad )
    case VK_PAGEUP:       return 0x67;   // right Amiga
    case VK_PAGEDOWN:     return 0x66;   // left Amiga
    case VK_APPLICATION:  return 0x67;   // right Amiga
    case VK_LSHIFT:       return 0x60;
    case VK_RSHIFT:       return 0x61;
    case VK_LCTRL:        return 0x63;
    case VK_RCTRL:        return 0x63;
    case VK_LALT:         return 0x64;
    case VK_RALT:         return 0x65;
    case VK_LGUI:         return 0x66;
    case VK_RGUI:         return 0x67;
    case VK_KP_0:         return 0x0F;
    case VK_KP_1:         return 0x1D;
    case VK_KP_2:         return 0x1E;
    case VK_KP_3:         return 0x1F;
    case VK_KP_4:         return 0x2D;
    case VK_KP_5:         return 0x2E;
    case VK_KP_6:         return 0x2F;
    case VK_KP_7:         return 0x3D;
    case VK_KP_8:         return 0x3E;
    case VK_KP_9:         return 0x3F;
    case VK_KP_PERIOD:    return 0x3C;
    case VK_KP_ENTER:     return 0x43;
    case VK_KP_MINUS:     return 0x4A;
    case VK_KP_PLUS:      return 0x5E;
    case VK_KP_DIVIDE:    return 0x5C;
    case VK_KP_MULTIPLY:  return 0x5D;
    case VK_NUMLOCK:      return 0x5A;   // keypad ( (PC num lock sits there)
    default:              return AK_NONE;
    }
}

// Amiga keys currently down, so the menu can release them on the way in.
static uint8_t amiga_down[(0x68 + 7) / 8];

static void send_amiga_key(uint8_t code, bool down) {
    const uint8_t bit = (uint8_t)(1u << (code & 7));
    if (down) amiga_down[code >> 3] |= bit; else amiga_down[code >> 3] &= (uint8_t)~bit;
    amiga_key(code, down);
}

static void release_amiga_keys(void) {
    for (uint8_t c = 0; c < 0x68; c++)
        if (amiga_down[c >> 3] & (1u << (c & 7))) send_amiga_key(c, false);
}

// ---- the pad's map (joymap.h) ---------------------------------------------------------
// The pad's controls arrive as VK_DPAD_* (directions, A, B, Start, Select: the
// drivers' joyPushData) and VK_JOY_* (the rest: their kbdPushData); the keyboard's
// keys as themselves.  Each frame the held ones are turned into actions through
// Config::joydef / Config::joykey (the keys only with Keyboard as joystick on).
const char *const joymap_action_name[JOY_ACTIONS] = {
    "None", "Up", "Down", "Left", "Right", "Red", "Blue", "Green", "Yellow", "Play", "Reverse", "Forward" };
const char *const joymap_slot_name[JOY_SLOTS] = {
    "Left", "Right", "Up", "Down", "Start", "Select", "A", "B", "C", "X", "Y", "Z", "L2", "R2" };
static const fabgl::VirtualKey kSlotVk[JOY_SLOTS] = {
    fabgl::VirtualKey::VK_DPAD_LEFT, fabgl::VirtualKey::VK_DPAD_RIGHT, fabgl::VirtualKey::VK_DPAD_UP,
    fabgl::VirtualKey::VK_DPAD_DOWN, fabgl::VirtualKey::VK_DPAD_START, fabgl::VirtualKey::VK_DPAD_SELECT,
    fabgl::VirtualKey::VK_DPAD_FIRE, fabgl::VirtualKey::VK_DPAD_ALTFIRE, fabgl::VirtualKey::VK_JOY_C,
    fabgl::VirtualKey::VK_JOY_X, fabgl::VirtualKey::VK_JOY_Y, fabgl::VirtualKey::VK_JOY_Z,
    fabgl::VirtualKey::VK_JOY_L2, fabgl::VirtualKey::VK_JOY_R2 };

// A is red (fire), B blue, X green, Y yellow - the CD32 pad's diamond on a SNES /
// XInput one; the shoulders and the triggers reverse and forward; Start play.
void joymap_pad_defaults(uint8_t out[JOY_SLOTS]) {
    static const uint8_t k[JOY_SLOTS] = { JOY_A_LEFT, JOY_A_RIGHT, JOY_A_UP, JOY_A_DOWN, JOY_A_PLAY, JOY_A_NONE,
        JOY_A_RED, JOY_A_BLUE, JOY_A_FFW, JOY_A_GREEN, JOY_A_YELLOW, JOY_A_RWD, JOY_A_RWD, JOY_A_FFW };
    memcpy(out, k, sizeof k);
}
void joymap_key_defaults(uint16_t out[JOY_ACTIONS]) {
    memset(out, 0, JOY_ACTIONS * sizeof out[0]);
    out[JOY_A_UP] = fabgl::VK_UP; out[JOY_A_DOWN] = fabgl::VK_DOWN;
    out[JOY_A_LEFT] = fabgl::VK_LEFT; out[JOY_A_RIGHT] = fabgl::VK_RIGHT;
    out[JOY_A_RED] = fabgl::VK_LCTRL; out[JOY_A_BLUE] = fabgl::VK_RCTRL;
}
bool joymap_slot_down(int slot) { return slot >= 0 && slot < JOY_SLOTS && kbd.isVKDown(kSlotVk[slot]); }
static bool is_lower(uint16_t vk) { return vk >= fabgl::VK_a && vk <= fabgl::VK_z; }
bool joymap_key_down(uint16_t vk) {
    if (!vk) return false;
    if (kbd.isVKDown((fabgl::VirtualKey)vk)) return true;
    return is_lower(vk) && kbd.isVKDown((fabgl::VirtualKey)(vk - fabgl::VK_a + fabgl::VK_A));
}
uint16_t joymap_key_norm(uint16_t vk) {
    if (vk == fabgl::VK_NONE || vk >= fabgl::VK_JOY_RIGHT) return 0;       // the pad's and the menu's own
    if (vk >= fabgl::VK_A && vk <= fabgl::VK_Z) return (uint16_t)(vk - fabgl::VK_A + fabgl::VK_a);
    return vk;
}
const char *joymap_key_name(uint16_t vk, char out[16]) {
    static const struct { uint16_t vk; const char *n; } k[] = {
        { fabgl::VK_SPACE, "Space" }, { fabgl::VK_UP, "Up" }, { fabgl::VK_DOWN, "Down" }, { fabgl::VK_LEFT, "Left" },
        { fabgl::VK_RIGHT, "Right" }, { fabgl::VK_LCTRL, "Left Ctrl" }, { fabgl::VK_RCTRL, "Right Ctrl" },
        { fabgl::VK_LSHIFT, "Left Shift" }, { fabgl::VK_RSHIFT, "Right Shift" }, { fabgl::VK_LALT, "Left Alt" },
        { fabgl::VK_RALT, "Right Alt" }, { fabgl::VK_RETURN, "Enter" }, { fabgl::VK_TAB, "Tab" },
        { fabgl::VK_BACKSPACE, "Backspace" }, { fabgl::VK_INSERT, "Insert" }, { fabgl::VK_DELETE, "Delete" },
        { fabgl::VK_HOME, "Home" }, { fabgl::VK_END, "End" }, { fabgl::VK_PAGEUP, "PgUp" }, { fabgl::VK_PAGEDOWN, "PgDn" },
        { fabgl::VK_CAPSLOCK, "Caps Lock" }, { fabgl::VK_KP_ENTER, "KP Enter" }, { fabgl::VK_KP_PLUS, "KP +" },
        { fabgl::VK_KP_MINUS, "KP -" }, { fabgl::VK_KP_MULTIPLY, "KP *" }, { fabgl::VK_KP_DIVIDE, "KP /" },
        { fabgl::VK_KP_PERIOD, "KP ." }, { fabgl::VK_COMMA, "," }, { fabgl::VK_PERIOD, "." }, { fabgl::VK_SLASH, "/" },
        { fabgl::VK_SEMICOLON, ";" }, { fabgl::VK_QUOTE, "'" }, { fabgl::VK_LEFTBRACKET, "[" },
        { fabgl::VK_RIGHTBRACKET, "]" }, { fabgl::VK_BACKSLASH, "\\" }, { fabgl::VK_MINUS, "-" }, { fabgl::VK_EQUALS, "=" },
        { fabgl::VK_GRAVEACCENT, "`" },
    };
    if (!vk) snprintf(out, 16, "None");
    else if (is_lower(vk)) snprintf(out, 16, "%c", 'A' + (vk - fabgl::VK_a));
    else if (vk >= fabgl::VK_A && vk <= fabgl::VK_Z) snprintf(out, 16, "%c", 'A' + (vk - fabgl::VK_A));
    else if (vk >= fabgl::VK_0 && vk <= fabgl::VK_9) snprintf(out, 16, "%c", '0' + (vk - fabgl::VK_0));
    else if (vk >= fabgl::VK_KP_0 && vk <= fabgl::VK_KP_9) snprintf(out, 16, "KP %c", '0' + (vk - fabgl::VK_KP_0));
    else if (vk >= fabgl::VK_F1 && vk <= fabgl::VK_F12) snprintf(out, 16, "F%d", 1 + (vk - fabgl::VK_F1));
    else {
        const char *n = NULL;
        for (const auto &e : k) if (e.vk == vk) n = e.n;
        if (n) snprintf(out, 16, "%s", n); else snprintf(out, 16, "Key %u", (unsigned)vk);
    }
    return out;
}

// A key the map uses while the keyboard is the joystick: the Amiga does not get it.
static bool joy_key(fabgl::VirtualKey vk) {
    const uint16_t n = joymap_key_norm((uint16_t)vk);
    if (!n) return false;
    for (int a = 1; a < JOY_ACTIONS; a++) if (Config::joykey[a] == n) return true;
    return false;
}

void input_frame(void) {
    fabgl::VirtualKeyItem item;
    while (kbd.virtualKeyAvailable() && kbd.getNextVirtualKey(&item, 0)) {
        // F12 is the emulator's key (as on WinUAE): the menu; with Ctrl the disk
        // browser for DF0, with Shift a reset. F1..F10 belong to the Amiga.
        if (item.vk == fabgl::VirtualKey::VK_F12) {
            if (!item.down) continue;
            const bool ctrl  = kbd.isVKDown(fabgl::VirtualKey::VK_LCTRL) || kbd.isVKDown(fabgl::VirtualKey::VK_RCTRL);
            const bool shift = kbd.isVKDown(fabgl::VirtualKey::VK_LSHIFT) || kbd.isVKDown(fabgl::VirtualKey::VK_RSHIFT);
            if (shift) { reset_req = true; continue; }
            // The Amiga must not see keys held across the menu (the Ctrl of
            // Ctrl+F12 included): release what it thinks is down, then hand
            // the screen over.
            release_amiga_keys();
            menu_open = true;
            if (ctrl) nm::runDiskInsert(0);
            else      nm::run();
            menu_open = false;
            kbd.emptyVirtualKeyQueue();
            continue;
        }
        // F11: reset the Amiga (as Shift+F12); Win+F11: reboot the board, asked.
        if (item.vk == fabgl::VirtualKey::VK_F11) {
            if (!item.down) continue;
            if (kbd.isVKDown(fabgl::VirtualKey::VK_LGUI) || kbd.isVKDown(fabgl::VirtualKey::VK_RGUI)) {
                release_amiga_keys();
                menu_open = true;
                nm::runRebootAsk();
                menu_open = false;
                kbd.emptyVirtualKeyQueue();
            } else {
                reset_req = true;
            }
            continue;
        }
        // Win+F8: the performance band, over the running Amiga.
        if (item.vk == fabgl::VirtualKey::VK_F8 && item.down &&
            (kbd.isVKDown(fabgl::VirtualKey::VK_LGUI) || kbd.isVKDown(fabgl::VirtualKey::VK_RGUI))) {
            ui_stats_toggle();
            continue;
        }
        // Win+F1: the System info page; Win+F5: Open file (the Amiga saw Win as
        // an Amiga key: released).
        if ((item.vk == fabgl::VirtualKey::VK_F1 || item.vk == fabgl::VirtualKey::VK_F5) && item.down &&
            (kbd.isVKDown(fabgl::VirtualKey::VK_LGUI) || kbd.isVKDown(fabgl::VirtualKey::VK_RGUI))) {
            release_amiga_keys();
            menu_open = true;
            if (item.vk == fabgl::VirtualKey::VK_F1) nm::runSysInfo();
            else                                     nm::runOpenFile();
            menu_open = false;
            kbd.emptyVirtualKeyQueue();
            continue;
        }
        // Win+F3 / Win+F4: Snapshots > Quick slots, Enter loading / saving; with
        // Alt, the current slot at once.
        if ((item.vk == fabgl::VirtualKey::VK_F3 || item.vk == fabgl::VirtualKey::VK_F4) && item.down &&
            (kbd.isVKDown(fabgl::VirtualKey::VK_LGUI) || kbd.isVKDown(fabgl::VirtualKey::VK_RGUI))) {
            const bool alt = kbd.isVKDown(fabgl::VirtualKey::VK_LALT) || kbd.isVKDown(fabgl::VirtualKey::VK_RALT);
            const bool save = item.vk == fabgl::VirtualKey::VK_F4;
            release_amiga_keys();
            menu_open = true;
            if (alt) nm::runQuickSnap(save);
            else     nm::runPersist(save);
            menu_open = false;
            kbd.emptyVirtualKeyQueue();
            continue;
        }
        if (Config::CursorAsJoy && joy_key(item.vk)) continue;
        uint8_t code = vk_to_amiga(item.vk);
        if (code != AK_NONE) send_amiga_key(code, item.down);
    }

    // Game port 2: the pad's controls through its map, and the keyboard's keys.
    uint32_t act = 0;
    for (int i = 0; i < JOY_SLOTS; i++)
        if (kbd.isVKDown(kSlotVk[i])) act |= 1u << Config::joydef[i];
    if (Config::CursorAsJoy)
        for (int a = 1; a < JOY_ACTIONS; a++)
            if (joymap_key_down(Config::joykey[a])) act |= 1u << a;
    const auto on = [act](int a) { return (act >> a) & 1u; };
    amiga_joystick(1, (uint8_t)(on(JOY_A_UP) | on(JOY_A_DOWN) << 1 | on(JOY_A_LEFT) << 2 | on(JOY_A_RIGHT) << 3),
                   on(JOY_A_RED));
    // The other buttons: the CD32 pad's; blue is the second fire button anywhere.
    amiga_cd32_pad((uint8_t)((on(JOY_A_RED) ? AMIGA_PAD_RED : 0) | (on(JOY_A_BLUE) ? AMIGA_PAD_BLUE : 0) |
                             (on(JOY_A_GREEN) ? AMIGA_PAD_GREEN : 0) | (on(JOY_A_YELLOW) ? AMIGA_PAD_YELLOW : 0) |
                             (on(JOY_A_PLAY) ? AMIGA_PAD_PLAY : 0) | (on(JOY_A_RWD) ? AMIGA_PAD_RWD : 0) |
                             (on(JOY_A_FFW) ? AMIGA_PAD_FFW : 0)));

    // Port 1 mouse: hid_app.cpp accumulates Mouse::x / y (y up-positive).
    static int32_t last_x, last_y;
    const int32_t x = Mouse::x, y = Mouse::y;
    amiga_mouse(x - last_x, last_y - y, Mouse::buttonL, Mouse::buttonR);
    last_x = x; last_y = y;
}

// ---- API ---------------------------------------------------------------------------

void input_init(void) {
    kbd.begin(true, true, 0, false);
    input_devices_init();
    if (!Config::loaded) {                       // no card: the maps' defaults
        joymap_pad_defaults(Config::joydef);
        joymap_key_defaults(Config::joykey);
    }
}

void input_poll(void) { repeat_me_for_input(); }

void input_request_reset(void) { reset_req = true; }

// ---- the menu's keyboard (ui/UiPlatform.h) ---------------------------------------
namespace nm {
bool uiKeyAvailable() { return kbd.virtualKeyAvailable() != 0; }
bool uiReadKey(fabgl::VirtualKeyItem* k) { return kbd.getNextVirtualKey(k, 0); }
fabgl::Keyboard* uiKeyboard() { return &kbd; }
}

bool input_reset_requested(void) {
    bool r = reset_req;
    reset_req = false;
    return r;
}

bool input_keyboard_mounted(void) { return input_devices_keyboard_mounted(); }

bool input_key_held(void) {
    for (int i = 0; i < 256; i++) if (pressed_key[i]) return true;
    return false;
}
