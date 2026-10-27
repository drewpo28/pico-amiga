// USB host input: boot-protocol keyboards and mice via TinyUSB.
//
// Keyboard reports are diffed against the previous one and turned into Amiga
// key down/up events; F12 is the emulator's reset key (Ctrl-Amiga-Amiga also
// works the Amiga way, through the keyboard handshake).  Mouse reports go to
// JOY0DAT counters and the two buttons.
//
// Keyboard joystick (on by default, Scroll Lock toggles it): the cursor keys
// and Right Ctrl / Right Alt (fire) drive the joystick in game port 2 and are
// not passed on as Amiga keys while the mode is on.
#include "usb_input.h"
#include <string.h>
#include "tusb.h"
#include "amiga.h"
#include "hid_keymap.h"

static uint8_t prev_keys[6], prev_mod;
static volatile bool reset_req;

#define JOY_FIRE_MODS (KEYBOARD_MODIFIER_RIGHTCTRL | KEYBOARD_MODIFIER_RIGHTALT)
static bool    kbd_joy = true;
static uint8_t joy_dirs;          // bit0 up 1 down 2 left 3 right (amiga_joystick)

static bool in_report(const uint8_t *keys, uint8_t k) {
    for (int i = 0; i < 6; i++) if (keys[i] == k) return true;
    return false;
}

static void joy_update(void) {
    amiga_joystick(1, joy_dirs, kbd_joy && (prev_mod & JOY_FIRE_MODS));
}

// Cursor keys (HID 0x4F right, 0x50 left, 0x51 down, 0x52 up) as joystick directions.
static bool joy_key(uint8_t usage, bool down) {
    static const uint8_t dir[4] = { 1u << 3, 1u << 2, 1u << 1, 1u << 0 };
    if (!kbd_joy || usage < 0x4F || usage > 0x52) return false;
    uint8_t bit = dir[usage - 0x4F];
    joy_dirs = down ? (joy_dirs | bit) : (joy_dirs & ~bit);
    joy_update();
    return true;
}

static void key_event(uint8_t usage, bool down) {
    if (usage == 0x45) { if (down) reset_req = true; return; }     // F12
    if (usage == 0x47) {                                            // Scroll Lock
        if (down) { kbd_joy = !kbd_joy; joy_dirs = 0; joy_update(); }
        return;
    }
    if (joy_key(usage, down)) return;
    uint16_t m = hid_to_amiga[usage];
    if (!(m & 0x100) || (m & 0xFF) == AK_NONE) return;
    amiga_key((uint8_t)m, down);
}

static void keyboard_report(const hid_keyboard_report_t *r) {
    uint8_t chg = r->modifier ^ prev_mod;
    if (kbd_joy) chg &= (uint8_t)~JOY_FIRE_MODS;
    for (int b = 0; b < 8; b++)
        if (chg & (1u << b)) amiga_key(hid_mod_to_amiga[b], (r->modifier >> b) & 1);
    bool fire_chg = (r->modifier ^ prev_mod) & JOY_FIRE_MODS;
    prev_mod = r->modifier;
    if (kbd_joy && fire_chg) joy_update();
    for (int i = 0; i < 6; i++) {
        uint8_t k = prev_keys[i];
        if (k > 3 && !in_report(r->keycode, k)) key_event(k, false);
    }
    for (int i = 0; i < 6; i++) {
        uint8_t k = r->keycode[i];
        if (k > 3 && !in_report(prev_keys, k)) key_event(k, true);
    }
    memcpy(prev_keys, r->keycode, 6);
}

static void mouse_report(const hid_mouse_report_t *r) {
    amiga_mouse(r->x, r->y, r->buttons & MOUSE_BUTTON_LEFT, r->buttons & MOUSE_BUTTON_RIGHT);
}

static uint8_t kbd_mounted;      // keyboard interfaces currently mounted

void tuh_hid_mount_cb(uint8_t dev, uint8_t inst, const uint8_t *desc, uint16_t len) {
    (void)desc; (void)len;
    if (tuh_hid_interface_protocol(dev, inst) == HID_ITF_PROTOCOL_KEYBOARD) kbd_mounted++;
    tuh_hid_receive_report(dev, inst);
}

void tuh_hid_umount_cb(uint8_t dev, uint8_t inst) {
    if (tuh_hid_interface_protocol(dev, inst) == HID_ITF_PROTOCOL_KEYBOARD && kbd_mounted) kbd_mounted--;
}

void tuh_hid_report_received_cb(uint8_t dev, uint8_t inst, const uint8_t *report, uint16_t len) {
    if (len == 0) { tuh_hid_receive_report(dev, inst); return; }   // ZLP idle traffic
    switch (tuh_hid_interface_protocol(dev, inst)) {
    case HID_ITF_PROTOCOL_KEYBOARD: {
        // Zero-padded copy: some keyboards send boot reports shorter than 8 bytes.
        hid_keyboard_report_t kb = { 0, 0, { 0 } };
        memcpy(&kb, report, len < sizeof kb ? len : sizeof kb);
        keyboard_report(&kb);
        break;
    }
    case HID_ITF_PROTOCOL_MOUSE:
        if (len >= 3) mouse_report((const hid_mouse_report_t *)report);
        break;
    default:
        break;   // report-protocol devices (gamepads) are a TODO
    }
    tuh_hid_receive_report(dev, inst);
}

void usb_input_init(void) {
    tuh_hid_set_default_protocol(HID_PROTOCOL_BOOT);
    tuh_init(BOARD_TUH_RHPORT);
}

void usb_input_poll(void) { tuh_task(); }

bool usb_input_keyboard_mounted(void) { return kbd_mounted != 0; }

bool usb_input_key_held(void) {
    if (prev_mod) return true;
    for (int i = 0; i < 6; i++) if (prev_keys[i]) return true;
    return false;
}

bool usb_input_reset_requested(void) {
    bool r = reset_req;
    reset_req = false;
    return r;
}
